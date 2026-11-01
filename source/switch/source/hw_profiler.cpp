// Sampling profiler on the console (LSWTCS_HWPROF=period in ms, default 0 = off), for where the guest CPU goes. Run 7's
// heaviest level was limited by the guest threads, and the main thread held the scheduler's turn for 31-44 % of the time
// on 17-18 % CPU: something it waits on while holding it. CPU counters cannot name that; samples of a paused thread can,
// because a blocked thread's saved pc and lr are the wait it is in.
//
// One thread pauses a target (svcSetThreadActivity), reads its context, looks through its stack and resumes it. The target
// may hold any lock while paused, so nothing between pause and resume allocates, prints or takes a lock. Samples go into
// a fixed table, written to hwprof.txt every 60 s; tools/hwprof_report.py names them from the ELF.
//
// The call chain is a stack scan, not an unwind: the generated code has no frame pointers, so each word on the stack that
// points just after a call in the code segment counts as a return address. Stale words can appear in it, so it says what
// was probably calling, while pc and lr are exact.
#include <switch.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void lsw_log_path(char* out, size_t capacity, const char* name);
extern "C" __attribute__((weak)) int lsw_sched_turn() { return -1; }

namespace {
constexpr int kMaxThreads = 48, kFrames = 8, kStackWords = 512;
constexpr uint32_t kTableSize = 1u << 16;
constexpr uint32_t kBands = 3;   // recorded draws of the current frame: under 300, under 1300, the rest

struct Target {
    char name[24];
    Handle handle;
    int sched_id;   // -1 for a host thread
    uint64_t cpu_ns_last;
    uint64_t samples;
};
Target g_targets[kMaxThreads];
std::atomic<int> g_target_count{0};
std::atomic<uint32_t> g_draws{0};

struct Entry {
    uint32_t count;
    uint8_t target, band, holds_turn, used;
    uint32_t frames[kFrames];   // offsets from the code base: pc, lr, then return addresses from the stack
};
Entry* g_table;
uint32_t g_table_used;
uint64_t g_table_dropped;
uintptr_t g_code_base, g_code_end;

bool in_code(uint64_t v) { return v >= g_code_base && v < g_code_end; }
// Whether the instruction before `v` is a call: BL, or BLR.
bool follows_call(uint64_t v) {
    if ((v & 3) || v < g_code_base + 4 || v >= g_code_end) return false;
    const uint32_t insn = *reinterpret_cast<const uint32_t*>(v - 4);
    return (insn & 0xFC000000u) == 0x94000000u || (insn & 0xFFFFFC1Fu) == 0xD63F0000u;
}

void add_sample(int target, uint32_t band, bool holds_turn, const uint32_t* frames) {
    uint32_t h = 2166136261u ^ uint32_t(target) ^ (band << 8) ^ (uint32_t(holds_turn) << 12);
    for (int i = 0; i < kFrames; ++i) h = (h ^ frames[i]) * 16777619u;
    for (uint32_t probe = 0; probe < 64; ++probe) {
        Entry& e = g_table[(h + probe) & (kTableSize - 1)];
        if (!e.used) {
            e.used = 1; e.target = uint8_t(target); e.band = uint8_t(band); e.holds_turn = holds_turn;
            memcpy(e.frames, frames, sizeof e.frames);
            e.count = 1;
            ++g_table_used;
            return;
        }
        if (e.target == target && e.band == band && e.holds_turn == holds_turn && !memcmp(e.frames, frames, sizeof e.frames)) {
            ++e.count;
            return;
        }
    }
    ++g_table_dropped;
}

void sample_target(int index, int turn, uint32_t band) {
    Target& t = g_targets[index];
    if (R_FAILED(svcSetThreadActivity(t.handle, ThreadActivity_Paused))) return;
    ThreadContext ctx;
    uint32_t frames[kFrames] = {};
    Result rc = svcGetThreadContext3(&ctx, t.handle);
    for (int retry = 0; R_FAILED(rc) && retry < 3; ++retry) {   // still leaving its core
        svcSleepThread(0);
        rc = svcGetThreadContext3(&ctx, t.handle);
    }
    if (R_SUCCEEDED(rc)) {
        int n = 0;
        if (in_code(ctx.pc.x)) frames[n] = uint32_t(ctx.pc.x - g_code_base);
        ++n;
        if (in_code(ctx.lr)) frames[n] = uint32_t(ctx.lr - g_code_base);
        ++n;
        MemoryInfo mi;
        u32 page;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &page, ctx.sp)) && (mi.perm & Perm_R)) {
            const uint64_t* word = reinterpret_cast<const uint64_t*>(ctx.sp & ~uint64_t(7));
            const uint64_t limit = mi.addr + mi.size;
            for (int w = 0; w < kStackWords && n < kFrames && reinterpret_cast<uint64_t>(word + 1) <= limit; ++w, ++word)
                if (follows_call(*word)) {
                    const uint32_t off = uint32_t(*word - g_code_base);
                    if (off != frames[n - 1]) frames[n++] = off;
                }
        }
    }
    svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
    if (R_SUCCEEDED(rc)) {
        add_sample(index, band, t.sched_id >= 0 && t.sched_id == turn, frames);
        ++t.samples;
    }
}

void write_window(FILE* f, uint64_t start_ns, uint64_t now_ns) {
    const uint64_t window_ns = now_ns - start_ns;
    fprintf(f, "window %llu %llu dropped %llu\n", (unsigned long long)(start_ns / 1000000), (unsigned long long)(now_ns / 1000000),
            (unsigned long long)g_table_dropped);
    const int count = g_target_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        Target& t = g_targets[i];
        uint64_t ticks = 0;
        const bool have = R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, t.handle, UINT64_MAX));
        const uint64_t cpu = have ? armTicksToNs(ticks) : 0;
        const double share = have && t.cpu_ns_last ? 100.0 * double(cpu - t.cpu_ns_last) / double(window_ns) : -1.0;
        t.cpu_ns_last = cpu;
        fprintf(f, "thread %d %s sched %d cpu %.1f samples %llu\n", i, t.name, t.sched_id, share, (unsigned long long)t.samples);
        t.samples = 0;
    }
    for (uint32_t i = 0; i < kTableSize; ++i) {
        Entry& e = g_table[i];
        if (!e.used) continue;
        fprintf(f, "s %u %u %u %u", e.target, e.band, e.holds_turn, e.count);
        for (uint32_t frame : e.frames) { if (frame) fprintf(f, " %x", frame); else fprintf(f, " 0"); }
        fputc('\n', f);
        e.used = 0;
    }
    g_table_used = 0;
    g_table_dropped = 0;
    fflush(f);
}

void profiler_main(void* arg) {
    const int64_t period_ns = int64_t(uintptr_t(arg)) * 1000000;
    char path[96];
    lsw_log_path(path, sizeof path, "hwprof.txt");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "hwprof 1 code %llx %llx\n", (unsigned long long)g_code_base, (unsigned long long)g_code_end);
    uint64_t window_start = armTicksToNs(armGetSystemTick()), tick = 0;
    for (;;) {
        svcSleepThread(period_ns);
        const uint32_t draws = g_draws.load(std::memory_order_relaxed);
        const uint32_t band = draws < 300 ? 0 : draws < 1300 ? 1 : 2;
        const int turn = lsw_sched_turn();
        const int count = g_target_count.load(std::memory_order_acquire);
        // Whoever holds the turn each tick, everyone else every 4th: blocked threads are the question, not the volume.
        const bool all = (++tick & 3) == 0;
        for (int i = 0; i < count; ++i)
            if (all || (g_targets[i].sched_id >= 0 && g_targets[i].sched_id == turn)) sample_target(i, turn, band);
        const uint64_t now = armTicksToNs(armGetSystemTick());
        if (now - window_start >= 60000000000ull) { write_window(f, window_start, now); window_start = now; }
    }
}
}  // namespace

// The calling thread becomes a sampling target. No-op cost when the profiler is off, beyond the list entry.
extern "C" void lsw_prof_register(const char* name, int sched_id) {
    const int i = g_target_count.load(std::memory_order_relaxed);
    if (i >= kMaxThreads) return;
    // Threads register rarely and the slot is published last; a lost race only costs one target.
    int expected = i;
    Target& t = g_targets[i];
    snprintf(t.name, sizeof t.name, "%s", name);
    t.handle = threadGetCurHandle();
    t.sched_id = sched_id;
    t.cpu_ns_last = 0;
    t.samples = 0;
    g_target_count.compare_exchange_strong(expected, i + 1, std::memory_order_acq_rel);
}
// Same for a guest thread, named after its entry point.
extern "C" void lsw_prof_register_guest(int sched_id, uint32_t entry) {
    char name[24];
    snprintf(name, sizeof name, "guest_%08X", entry);
    lsw_prof_register(name, sched_id);
}
extern "C" void lsw_prof_note_draws(uint32_t draws) { g_draws.store(draws, std::memory_order_relaxed); }

extern "C" void lsw_hw_profiler_start() {
    const char* e = getenv("LSWTCS_HWPROF");
    const int period_ms = e ? atoi(e) : 0;
    if (period_ms <= 0) return;
    MemoryInfo mi;
    u32 page;
    if (R_FAILED(svcQueryMemory(&mi, &page, reinterpret_cast<u64>(&lsw_hw_profiler_start)))) return;
    g_code_base = mi.addr;
    g_code_end = mi.addr + mi.size;
    g_table = static_cast<Entry*>(calloc(kTableSize, sizeof(Entry)));
    if (!g_table) return;
    static Thread t;
    // Above the audio output thread (0x2B) on its core, or a busy core would delay the samples: a sample is a few microseconds.
    const Result rc = threadCreate(&t, profiler_main, reinterpret_cast<void*>(uintptr_t(period_ms)), nullptr, 0x10000, 0x2A, 2);
    const Result rs = R_SUCCEEDED(rc) ? threadStart(&t) : rc;
    printf("[HWPROF] sampling every %d ms, code %lx..%lx (rc 0x%X / 0x%X)\n", period_ms, (unsigned long)g_code_base, (unsigned long)g_code_end, rc, rs);
    fflush(stdout);
}
