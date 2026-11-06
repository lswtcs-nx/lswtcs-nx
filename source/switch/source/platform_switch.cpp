// Switch platform layer: process entry, data directory, env file, logging, guest memory window,
// thread stacks. GPU / audio backends live in their own files (null_backends.cpp for M2).
#include <switch.h>
#include "lsw_guest_layout.h"
#include "lsw_save_platform.h"
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <typeinfo>
#include <malloc.h>
#include <new>
#ifndef LSW_DEKO_BACKEND
extern "C" void lsw_bo_check_init(uint64_t);
extern "C" void lsw_bo_check_dump(FILE*, const uintptr_t*, size_t);
extern "C" void lsw_bo_check_finish();
#endif
extern "C" bool lsw_log_async_init();
extern "C" void lsw_log_async_path(char* out, size_t capacity, const char* name);
extern "C" void lsw_log_async_drain();
extern "C" void lsw_hw_monitor_start();
extern "C" void lsw_hw_profiler_start();
extern "C" void lsw_prof_register(const char* name, int sched_id);

#ifndef LSW_DATA_DIR
#define LSW_DATA_DIR "sdmc:/lswtcs"
#endif

// The runtime's main() is compiled as lsw_runtime_main (-Dmain=lsw_runtime_main on main.cpp).
extern int lsw_runtime_main();

namespace {
FILE* g_stdout_log;
uint64_t g_run_id;
char g_log_dir[48] = ".";   // this session's folder, relative to LSW_DATA_DIR

uint64_t info_u64(uint32_t id) { uint64_t v = 0; svcGetInfo(&v, id, CUR_PROCESS_HANDLE, 0); return v; }

// env.txt: one NAME=VALUE per line, '#' comments. Replaces the PC launch scripts' environment.
void load_env_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { printf("[NX] no %s (using defaults)\n", path); return; }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        char* e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        setenv(s, eq + 1, 1);
        printf("[NX] env %s=%s\n", s, eq + 1);
        ++n;
    }
    fclose(f);
    printf("[NX] %d env settings from %s\n", n, path);
    // Which cores this launch may use: 0x7 for a normal homebrew launch, 0xF only from a title whose NPDM allows core 3 (the
    // fourth-core route of bo2-360-nx, an owner decision). Logged so the launch route of each session is on record.
    { u64 mask = 0; const Result rc = svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
      printf("[NX] process core mask 0x%llX (rc 0x%X)\n", (unsigned long long)mask, rc); }
}
}  // namespace

// ---- thread stacks --------------------------------------------------------------------------
// Each guest call is a host call, so deep guest recursion needs a deep host stack. std::thread
// passes no attributes; give every thread at least kMinStack (linked with --wrap=pthread_create).
static constexpr size_t kMinStack = 8u << 20;
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
// Core hint for threads that third-party code creates (Mesa's glthread worker): libnx starts every thread on the
// process's default core (0 = the guest threads). While lsw_set_new_thread_core(c) is set, new threads first pin
// themselves to core c.
// The hint is per creating thread (thread_local): other threads creating threads meanwhile (shader-compile workers,
// Mesa queues) are not affected. Pinned threads get priority 0x2C: below the audio output thread (0x2B, also core 2;
// with LSWTCS_GLPIPE the worker spins in nouveau's GPU fence wait, which made audio extremely choppy on hardware,
// 2026-10-09) and above the core-2 shader-compile worker (0x2D): Horizon does not time-slice equal priorities.
static thread_local int g_new_thread_core = -1;
extern "C" void lsw_set_new_thread_core(int core) { g_new_thread_core = core; }
namespace {
struct PinnedStart { void* (*fn)(void*); void* arg; int core; };
void* pinned_start(void* p) {
    PinnedStart s = *static_cast<PinnedStart*>(p);
    delete static_cast<PinnedStart*>(p);
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, s.core, 1u << s.core);
    const Result rp = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    printf("[NX] new thread pinned to core %d, priority 0x2C (rc 0x%X / 0x%X)\n", s.core, rc, rp);
    lsw_prof_register("mesa_worker", -1);
    fflush(stdout);
    return s.fn(s.arg);
}
}  // namespace
extern "C" int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    pthread_attr_t a;
    if (attr) a = *attr; else pthread_attr_init(&a);
    size_t sz = 0;
    pthread_attr_getstacksize(&a, &sz);
    if (sz < kMinStack) pthread_attr_setstacksize(&a, kMinStack);
    int rc;
    if (const int core = g_new_thread_core; core >= 0) {
        auto* s = new PinnedStart{fn, arg, core};
        rc = __real_pthread_create(t, &a, pinned_start, s);
        if (rc) delete s;
    } else {
        rc = __real_pthread_create(t, &a, fn, arg);
    }
    if (!attr) pthread_attr_destroy(&a);
    return rc;
}

// Host-only bursts of work by a guest thread that has left the scheduler rotation (native_reverb.cpp,
// LSWTCS_REVERBOFF): every guest thread is on core 0, so the work moves to core 2 at the audio output thread's
// priority (0x2B: ahead of the glthread worker 0x2C and the compile worker 0x2D, which would otherwise keep the core;
// Horizon does not time-slice equal priorities) and comes back afterwards.
static thread_local s32 g_offcore_prev_core = -1;
static thread_local u64 g_offcore_prev_mask = 0;
static thread_local s32 g_offcore_prev_prio = -1;
extern "C" void lsw_offcore_begin_prio(int prio);
extern "C" void lsw_offcore_begin() { lsw_offcore_begin_prio(0x2B); }
// Same with a chosen priority (the PM4 walk, kernel_stubs.cpp LSWTCS_WALKOFF: its multi-ms bursts at 0x2B starved the
// glthread worker on core 2 and with it the render thread).
// [OFFCORE]: the first few moves and returns, with what was saved and what the thread is left with.
static std::atomic<int> g_offcore_logs{0};
// Threads moved off their core right now (lsw_offcore_active), so the placement guard (hw_profiler.cpp) leaves them alone.
static std::atomic<Handle> g_offcore_now[32];
static void offcore_mark(Handle h, bool on) {
    for (auto& s : g_offcore_now) {
        Handle cur = s.load();
        if (on ? cur == 0 : cur == h) { if (s.compare_exchange_strong(cur, on ? h : 0)) return; }
    }
}
extern "C" bool lsw_offcore_active(Handle h) {
    for (auto& s : g_offcore_now) if (s.load() == h) return true;
    return false;
}
extern "C" void lsw_offcore_begin_mask(int ideal_core, unsigned target_mask, int new_prio);
extern "C" void lsw_offcore_begin_on(int target_core, int new_prio) { lsw_offcore_begin_mask(target_core, 1u << target_core, new_prio); }
// Any core of target_mask (the walk with A/B state f, kernel_stubs.cpp: cores 1 and 2), the kernel choosing among them.
extern "C" void lsw_offcore_begin_mask(int ideal_core, unsigned target_mask, int new_prio) {
    s32 core = 0; u64 mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&core, &mask, CUR_THREAD_HANDLE))) { g_offcore_prev_core = -1; return; }
    if (core < 0) core = 0;   // "saved" is now only a flag that a return is due; the mask names the cores
    s32 prio = 0;
    g_offcore_prev_prio = R_SUCCEEDED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE)) ? prio : -1;
    g_offcore_prev_core = core; g_offcore_prev_mask = mask;
    offcore_mark(CUR_THREAD_HANDLE, true);
    svcSetThreadPriority(CUR_THREAD_HANDLE, new_prio);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal_core, target_mask);
}
extern "C" void lsw_offcore_begin_prio(int new_prio) { lsw_offcore_begin_on(2, new_prio); }
extern "C" void lsw_offcore_end() {
    if (g_offcore_prev_core < 0) return;
    // Run 22: guest threads start with ideal core -1 and mask 7 (cores 0-2), and putting back "-1" sometimes left the thread where
    // the move had put it ([OFFCORE] saved core 1 prio 0x30: the file loader stuck beside the render thread at a higher priority).
    // The lowest core of the saved mask is named as the ideal core instead.
    const s32 home = g_offcore_prev_core >= 0 ? g_offcore_prev_core : -1;
    const s32 ideal = g_offcore_prev_mask ? s32(__builtin_ctzll(g_offcore_prev_mask)) : 0;
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, g_offcore_prev_mask);
    (void)home;
    if (R_FAILED(rc) && g_offcore_logs.fetch_add(1) < 12) { printf("[OFFCORE] return failed: rc 0x%X\n", rc); fflush(stdout); }
    if (g_offcore_prev_prio >= 0) svcSetThreadPriority(CUR_THREAD_HANDLE, g_offcore_prev_prio);
    if (g_offcore_logs.fetch_add(1) < 12) {
        s32 core = -9, prio = -1; u64 mask = 0;
        svcGetThreadCoreMask(&core, &mask, CUR_THREAD_HANDLE);
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        printf("[OFFCORE] back: saved core %d mask %llx prio %x, now core %d mask %llx prio %x\n", g_offcore_prev_core,
               (unsigned long long)g_offcore_prev_mask, g_offcore_prev_prio, core, (unsigned long long)mask, prio);
        fflush(stdout);
    }
    g_offcore_prev_core = -1;
    offcore_mark(CUR_THREAD_HANDLE, false);
}

// ── Fourth core (LSWTCS_CORE3=1), after bo2-360-nx's route, techniques only ──────────────────────────────────────────
// Core 3 is usable only when the process core mask has it: a title whose NPDM allows cores 0-3. hbloader's NPDM stops at
// core 2, so every homebrew launch logs mask 0x7 and nothing here happens. When it is there, threads that only do host work
// move to core 3 at the lowest application priority (0x3F) on their own call (lsw_core3_begin): the XMA worker, the
// background file writer, the log writer. The guest turn stays on core 0, so this frees cores 1 and 2 (render, Mesa), not
// the turn. Core 3 also runs system services: bo2-360-nx reports a hard freeze of a Switch Lite from starving them, so a
// meter checks once a second and sends every moved thread home for the rest of the session if they used more than 85 % of
// the core for 2 seconds in a row ([CORE3]).
namespace {
struct Core3Thread { Handle handle; s32 home_core; u64 home_mask; s32 home_prio; u64 ticks_last; char name[16]; };
Core3Thread g_core3[8];
std::atomic<int> g_core3_count{0};
std::atomic<bool> g_core3_on{false};
Mutex g_core3_lock;
bool core3_available() {
    static const bool ok = [] {
        const char* e = getenv("LSWTCS_CORE3");
        u64 mask = 0;
        return e && e[0] == '1' && R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && (mask & 8);
    }();
    return ok;
}
void core3_send_home() {
    mutexLock(&g_core3_lock);
    g_core3_on.store(false);
    for (int i = 0; i < g_core3_count.load(); ++i) {
        svcSetThreadCoreMask(g_core3[i].handle, g_core3[i].home_core, g_core3[i].home_mask);
        if (g_core3[i].home_prio >= 0) svcSetThreadPriority(g_core3[i].handle, g_core3[i].home_prio);
    }
    mutexUnlock(&g_core3_lock);
}
void core3_meter(void*) {
    int over = 0;
    for (;;) {
        svcSleepThread(1000000000);
        if (!g_core3_on.load()) return;
        u64 used = 0;
        for (int i = 0; i < g_core3_count.load(); ++i) {
            u64 t = 0;
            if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, g_core3[i].handle, UINT64_MAX))) continue;
            used += t - g_core3[i].ticks_last;
            g_core3[i].ticks_last = t;
        }
        const double share = double(armTicksToNs(used)) / 1e9;
        over = share > 0.85 ? over + 1 : 0;
        if (over >= 2) {
            core3_send_home();
            printf("[CORE3] moved threads used %.0f %% of core 3 for 2 s: back on their own cores for this session\n", share * 100);
            fflush(stdout);
            return;
        }
    }
}
}  // namespace
// The calling thread moves to core 3 if the fourth core is on; returns whether it moved.
extern "C" int lsw_core3_begin(const char* name) {
    if (!core3_available()) return 0;
    static std::once_flag started;
    std::call_once(started, [] {
        mutexInit(&g_core3_lock);
        g_core3_on.store(true);
        static Thread meter;
        if (R_SUCCEEDED(threadCreate(&meter, core3_meter, nullptr, nullptr, 0x4000, 0x2A, 2))) threadStart(&meter);
        printf("[CORE3] fourth core in use for host threads (meter on core 2)\n");
        fflush(stdout);
    });
    if (!g_core3_on.load()) return 0;
    mutexLock(&g_core3_lock);
    const int i = g_core3_count.load();
    if (i >= 8) { mutexUnlock(&g_core3_lock); return 0; }
    Core3Thread& t = g_core3[i];
    t.handle = threadGetCurHandle();
    if (R_FAILED(svcGetThreadCoreMask(&t.home_core, &t.home_mask, t.handle))) { mutexUnlock(&g_core3_lock); return 0; }
    if (R_FAILED(svcGetThreadPriority(&t.home_prio, t.handle))) t.home_prio = -1;
    svcGetInfo(&t.ticks_last, InfoType_ThreadTickCount, t.handle, UINT64_MAX);
    snprintf(t.name, sizeof t.name, "%s", name);
    svcSetThreadPriority(t.handle, 0x3F);
    const Result rc = svcSetThreadCoreMask(t.handle, 3, 1u << 3);
    g_core3_count.store(i + 1);
    mutexUnlock(&g_core3_lock);
    printf("[CORE3] %s on core 3 (rc 0x%X)\n", name, rc);
    fflush(stdout);
    return R_SUCCEEDED(rc);
}

// DIAG: per-core load ([CPULOAD], printed with [GLPROF]). Horizon reports a core's idle ticks only to a thread running
// on that core (InfoType_IdleTickCount, sub-id -1 = the caller's core), so one sampler thread per core, at the lowest
// application priority, reads its core's counter every 100 ms. A sampler only misses its wake-up when its core has no
// idle time at all, so a missing sample means the core was fully busy. libnx threads with 16 KB stacks: std::thread
// would get the 8 MB minimum above. Opt-in (LSWTCS_CPULOAD=1): on hardware (2026-10-11 s8) it read core 0 as 66-75 %
// busy while the guest threads alone used >90 % of it ([SCHEDPROF] cpu=), so its numbers are not trusted yet.
namespace {
struct CoreSample { std::atomic<uint32_t> seq{0}; uint64_t idle = 0, tick = 0; };
CoreSample g_core_sample[4];
Thread g_core_thread[4];
bool g_core_thread_ok[4];
void core_sampler(void* arg) {
    CoreSample& s = g_core_sample[uintptr_t(arg)];
    for (;;) {
        uint64_t idle = 0;
        if (R_SUCCEEDED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, UINT64_MAX))) {
            const uint64_t tick = armGetSystemTick();
            s.seq.fetch_add(1, std::memory_order_acq_rel);   // odd: being written
            s.idle = idle; s.tick = tick;
            s.seq.fetch_add(1, std::memory_order_acq_rel);
        }
        svcSleepThread(100000000);
    }
}
}  // namespace
extern "C" void lsw_core_load_start() {
    const char* e = getenv("LSWTCS_CPULOAD");
    if (!(e && e[0] == '1')) return;
    for (int c = 0; c < 4; ++c) {
        const Result rc = threadCreate(&g_core_thread[c], core_sampler, reinterpret_cast<void*>(uintptr_t(c)), nullptr,
                                       0x4000, 0x3B, c);
        const Result rs = R_SUCCEEDED(rc) ? threadStart(&g_core_thread[c]) : rc;
        g_core_thread_ok[c] = R_SUCCEEDED(rs);
        printf("[CPULOAD] core %d sampler: rc 0x%X / 0x%X\n", c, rc, rs);
    }
    fflush(stdout);
}
// [SCHEDPROF] cpu= (kernel_stubs.cpp): the calling thread's handle, and a thread's CPU time across all cores.
extern "C" uint64_t lsw_thread_token() { return threadGetCurHandle(); }
extern "C" uint64_t lsw_thread_cpu_ns(uint64_t token) {
    uint64_t ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, Handle(token), UINT64_MAX))) return 0;
    return armTicksToNs(ticks);
}
// Busy % per core since the previous call (one caller: the render thread). -1 = no sampler on that core.
extern "C" void lsw_core_load(float out[4]) {
    static uint64_t prev_idle[4], prev_tick[4];
    for (int c = 0; c < 4; ++c) {
        out[c] = -1.0f;
        if (!g_core_thread_ok[c]) continue;
        CoreSample& s = g_core_sample[c];
        uint64_t idle, tick;
        uint32_t q0, q1;
        do {
            q0 = s.seq.load(std::memory_order_acquire);
            idle = s.idle; tick = s.tick;
            q1 = s.seq.load(std::memory_order_acquire);
        } while ((q0 & 1) || q0 != q1);
        if (!tick) continue;
        if (prev_tick[c] && tick > prev_tick[c]) {
            const double busy = 1.0 - double(idle - prev_idle[c]) / double(tick - prev_tick[c]);
            out[c] = float(std::clamp(busy, 0.0, 1.0) * 100.0);
        } else if (prev_tick[c]) {
            out[c] = 100.0f;   // no new sample: the sampler never got the core
        }
        if (tick > prev_tick[c]) { prev_idle[c] = idle; prev_tick[c] = tick; }
    }
}

// C11 thrd_create for Mesa: newlib defines thrd_success = 4, but switch-mesa 20.1's u_thread_create() does
// `if (ret) return 0;` (assumes thrd_success == 0), so every Mesa worker thread was created and then reported as
// failed: util_queue_init() returned false -> glthread never enabled, Mesa's other queues (e.g. the shader disk-cache
// writer) never ran. Only Mesa uses C11 threads here (our code and libstdc++ use pthreads).
extern "C" int __real_thrd_create(void* thr, int (*func)(void*), void* arg);
extern "C" int __wrap_thrd_create(void* thr, int (*func)(void*), void* arg) {
    const int r = __real_thrd_create(thr, func, arg);
    return r == 4 /* newlib thrd_success */ ? 0 : (r ? r : 2 /* thrd_error */);
}

// ---- crash reports --------------------------------------------------------------------------
// libnx calls this for CPU exceptions (alignment, undefined instruction, brk, host data aborts).
// NOTE: Eden does not raise data aborts for unmapped addresses (EDEN_MEMORY.md probe #2).
// Offsets are relative to _start (ELF address 0 = NRO load base; libnx's __start__ is an absolute,
// unrelocated symbol). Symbolize with: aarch64-none-elf-addr2line -fCe switch/build/lswtcs.elf <offset>
extern "C" void _start();
#define __start__ (*(char*)_start)
extern "C" void lsw_log_path(char* out, size_t capacity, const char* name) {
    snprintf(out, capacity, "%s/%s", g_log_dir, name);
}

extern "C" char* fake_heap_end;   // libnx: the end of newlib's heap range (see lsw_heap_stats)
extern "C" size_t lsw_big_alloc_min __attribute__((weak));   // third_party/rpmalloc/rpmalloc_switch.c (LSW_RPMALLOC)
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const u64 b = (u64)&__start__;
    char crash_path[96];
    lsw_log_path(crash_path, sizeof crash_path, "crash.log");
    FILE* f = fopen(crash_path, "w");
    FILE* outs[2] = {f, stdout};
    for (FILE* o : outs) {
        if (!o) continue;
        fprintf(o, "[CRASH] run=%016llX\n", (unsigned long long)g_run_id);
        fprintf(o, "[CRASH] desc=0x%X pc=0x%llX (+0x%llX) lr=0x%llX (+0x%llX) far=0x%llX sp=0x%llX\n",
                ctx->error_desc, (unsigned long long)ctx->pc.x, (unsigned long long)(ctx->pc.x - b),
                (unsigned long long)ctx->lr.x, (unsigned long long)(ctx->lr.x - b),
                (unsigned long long)ctx->far.x, (unsigned long long)ctx->sp.x);
        {   // log74: run 74's crash was in Mesa's compiler, likely a failed C malloc (null + 0x20). No malloc lock here (the
            // crashing thread may hold it): only the untouched top of the heap, which malloc grows into.
            char* const brk = static_cast<char*>(sbrk(0));
            fprintf(o, "[CRASH] heap untouched top %lld MB\n",
                    brk == reinterpret_cast<char*>(-1) ? -1LL : (long long)((fake_heap_end - brk) >> 20));
        }
        for (int i = 0; i < 29; ++i) fprintf(o, "[CRASH] x%-2d=0x%016llX\n", i, (unsigned long long)ctx->cpu_gprs[i].x);
#ifndef LSW_DEKO_BACKEND
        if (o == f) {
            uintptr_t registers[29];
            for (int i = 0; i < 29; ++i) registers[i] = ctx->cpu_gprs[i].x;
            lsw_bo_check_dump(o, registers, 29);
        }
#endif
        // Frame-pointer walk (x29 chain), offsets only.
        u64 fp = ctx->fp.x;
        for (int d = 0; d < 32 && fp && !(fp & 7); ++d) {
            MemoryInfo mi; u32 pi;
            if (R_FAILED(svcQueryMemory(&mi, &pi, fp)) || !(mi.perm & Perm_R)) break;
            u64 ret = ((u64*)fp)[1];
            fprintf(o, "[CRASH]   frame %2d ret=+0x%llX\n", d, (unsigned long long)(ret - b));
            fp = ((u64*)fp)[0];
        }
        fflush(o);
    }
    if (f) fclose(f);
    lsw_log_async_drain();
    svcExitProcess();
}

// exit/abort wrappers (-Wl,--wrap=...): log who ended the process. Symbolize like crash offsets.
extern "C" [[noreturn]] void __real_exit(int);
extern "C" [[noreturn]] void __real_abort(void);
extern "C" [[noreturn]] void __wrap_exit(int code) {
    printf("[NX] exit(%d) called from +0x%llX\n", code,
           (unsigned long long)((u64)__builtin_return_address(0) - (u64)&__start__));
    fflush(stdout);
    lsw_log_async_drain();
    __real_exit(code);
}
extern "C" [[noreturn]] void __wrap_abort(void) {
    printf("[NX] abort() called from +0x%llX\n",
           (unsigned long long)((u64)__builtin_return_address(0) - (u64)&__start__));
    fflush(stdout);
    lsw_log_async_drain();
    __real_abort();
}

// C++ throws (-Wl,--wrap=__cxa_throw): the runtime is built without exception handling, so any
// libstdc++ throw ends in std::terminate. Log the type and the thrower's frames first.
static void log_backtrace(const char* tag) {
    const u64 b = (u64)&__start__;
    u64 fp = (u64)__builtin_frame_address(0);
    for (int d = 0; d < 24 && fp && !(fp & 7); ++d) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, fp)) || !(mi.perm & Perm_R)) break;
        printf("[NX] %s frame %2d ret=+0x%llX\n", tag, d, (unsigned long long)(((u64*)fp)[1] - b));
        fp = ((u64*)fp)[0];
    }
    fflush(stdout);
}
extern "C" [[noreturn]] void __real___cxa_throw(void*, std::type_info*, void (*)(void*));
extern "C" [[noreturn]] void __wrap___cxa_throw(void* obj, std::type_info* type, void (*dtor)(void*)) {
    printf("[NX] C++ throw of %s\n", type ? type->name() : "?");
    log_backtrace("throw");
    __real___cxa_throw(obj, type, dtor);
}

// ---- C++ runtime self-test ------------------------------------------------------------------
// The runtime relies on std::call_once / std::thread / std::condition_variable (dbg_ram, the GIL
// scheduler). Exercise each once at startup and log, so a libstdc++/libnx gap shows up by name.
static void runtime_selftest() {
    printf("[NX] selftest: call_once..."); fflush(stdout);
    static std::once_flag once; int hits = 0;
    std::call_once(once, [&] { ++hits; });
    std::call_once(once, [&] { ++hits; });
    printf(" ok (%d)\n[NX] selftest: thread...", hits); fflush(stdout);
    int v = 0;
    std::thread t([&] { v = 42; });
    t.join();
    printf(" ok (%d)\n[NX] selftest: condvar...", v); fflush(stdout);
    std::mutex m; std::condition_variable cv; bool ready = false;
    std::thread t2([&] { std::lock_guard<std::mutex> lk(m); ready = true; cv.notify_all(); });
    { std::unique_lock<std::mutex> lk(m); cv.wait_for(lk, std::chrono::seconds(2), [&] { return ready; }); }
    t2.join();
    printf(" ok (%d)\n", (int)ready); fflush(stdout);
}

// libnx's pthread_detach fails, so std::thread::detach() throws (-> terminate). The runtime detaches
// long-lived workers (trace flusher, XMA, ...): report success; the thread runs on and its control
// block is simply never reclaimed (-Wl,--wrap=pthread_detach).
extern "C" int __wrap_pthread_detach(pthread_t) { return 0; }

// DIAG: time threads spend acquiring newlib's single malloc lock (-Wl,--wrap=__malloc_lock). Parallel
// shader builds ran 10-20x slower than one build alone (2026-10-08); this measures whether malloc
// contention is why. Acquisitions over ~1 us (20 ticks of the 19.2 MHz counter) count as contended.
struct _reent;
extern "C" void __real___malloc_lock(struct _reent*);
static uint64_t s_malloc_wait_ticks = 0, s_malloc_contended = 0, s_malloc_calls = 0;
extern "C" void __wrap___malloc_lock(struct _reent* r) {
    const uint64_t t0 = armGetSystemTick();
    __real___malloc_lock(r);
    const uint64_t d = armGetSystemTick() - t0;
    ++s_malloc_calls;   // under the lock: exact
    if (d > 20) { s_malloc_wait_ticks += d; ++s_malloc_contended; }
}
// Totals so far: wait in ns, contended acquisitions, all acquisitions.
extern "C" void lsw_malloc_lock_stats(uint64_t* wait_ns, uint64_t* contended, uint64_t* calls) {
    *wait_ns = armTicksToNs(s_malloc_wait_ticks);
    *contended = s_malloc_contended;
    *calls = s_malloc_calls;
}

// Host heap (log73). Run 73 aborted with std::bad_alloc on the walker thread after 592 s, and nothing in the log said how full
// the heap was. libnx gives newlib's malloc one range (fake_heap_start..fake_heap_end, all the memory left at start); malloc grows
// into it with sbrk and never hands memory back, so what is free = the untouched top + freed chunks inside (mallinfo fordblks).
// The guest window and Mesa's buffer objects come out of the same heap.
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;
extern "C" void lsw_heap_stats(uint64_t* size, uint64_t* in_use, uint64_t* free_inside, uint64_t* untouched) {
    const struct mallinfo mi = mallinfo();
    char* const brk = static_cast<char*>(sbrk(0));
    *size = uint64_t(fake_heap_end - fake_heap_start);
    *in_use = mi.uordblks;
    *free_inside = mi.fordblks;
    *untouched = (brk != reinterpret_cast<char*>(-1) && brk >= fake_heap_start && brk <= fake_heap_end) ? uint64_t(fake_heap_end - brk) : 0;
}
// Free memory left, in MB (untouched top + free chunks).
extern "C" uint32_t lsw_heap_free_mb() {
    uint64_t size, used, inside, top;
    lsw_heap_stats(&size, &used, &inside, &top);
    return uint32_t((inside + top) >> 20);
}
// A C++ allocation failed: log the heap once, then let the retry throw std::bad_alloc as before (no allocation in here).
static void lsw_new_failed() {
    std::set_new_handler(nullptr);
    uint64_t size, used, inside, top;
    lsw_heap_stats(&size, &used, &inside, &top);
    char line[192];
    snprintf(line, sizeof line, "[HEAP] C++ allocation failed: heap %llu MB, in use %llu MB, free inside %llu MB, untouched top %llu MB\n",
             (unsigned long long)(size >> 20), (unsigned long long)(used >> 20), (unsigned long long)(inside >> 20), (unsigned long long)(top >> 20));
    fputs(line, stdout);
    fflush(stdout);
}

// ---- init -----------------------------------------------------------------------------------
extern "C" void lsw_boot_mark(const char* what);   // main.cpp: [BOOT] milliseconds since the process started
extern "C" bool lsw_switch_init() {
    mkdir(LSW_DATA_DIR, 0777);
    chdir(LSW_DATA_DIR);
    g_run_id = armGetSystemTick();
    // Every session logs into its own logs/<date_time> folder, so a relaunch overwrites nothing. Capture triggers
    // left by a session that ended mid-capture are dropped, or the first frame of this one is captured (a black
    // shot_1.png).
    mkdir("logs", 0777);
    const time_t session_time = time(nullptr);
    const tm* local = localtime(&session_time);
    if (local && local->tm_year >= 120) strftime(g_log_dir, sizeof g_log_dir, "logs/%Y-%m-%d_%H-%M-%S", local);
    else snprintf(g_log_dir, sizeof g_log_dir, "logs/run_%016llX", (unsigned long long)g_run_id);
    mkdir(g_log_dir, 0777);
    remove("shot_now");
    remove("resdump_now");
    // stdout/stderr -> log file (printf is the runtime's main log). A 1 KB default buffer meant one SD write per
    // 1 KB of log on the thread printing it: a capture's 700 KB draw log took 12-15 s.
    // Both go through the ring in log_async.cpp: no thread that prints waits for the SD card.
    const bool log_async = lsw_log_async_init();
    char log_name[96], log_path[104];
    lsw_log_path(log_name, sizeof log_name, "stdout.log");
    lsw_log_async_path(log_path, sizeof log_path, log_name);
    g_stdout_log = freopen(log_path, "w", stdout);
    if (!g_stdout_log) g_stdout_log = freopen(log_name, "w", stdout);
    if (g_stdout_log) setvbuf(g_stdout_log, nullptr, _IOFBF, 64 * 1024);
    lsw_log_path(log_name, sizeof log_name, "stderr.log");
    lsw_log_async_path(log_path, sizeof log_path, log_name);
    if (!freopen(log_path, "w", stderr)) freopen(log_name, "w", stderr);
    if (!log_async) printf("[NX] log ring unavailable: logging directly to the SD card\n");
    printf("[NX] LSWTCS Switch runtime\n");
    printf("[NX] run=%016llX\n", (unsigned long long)g_run_id);
    printf("[NX] TotalMemory %llu MB, UsedMemory %llu MB at start\n",
           (unsigned long long)(info_u64(InfoType_TotalMemorySize) >> 20),
           (unsigned long long)(info_u64(InfoType_UsedMemorySize) >> 20));
#ifdef LSW_BUILD_GUEST_NV
    printf("[NX] guest code: loads and stores without volatile, barriers at loop headers and fences (log77 B)\n");
#else
    printf("[NX] guest code: volatile loads and stores (normal build)\n");
#endif
    {
        std::set_new_handler(lsw_new_failed);
        uint64_t size, used, inside, top;
        lsw_heap_stats(&size, &used, &inside, &top);
        printf("[HEAP] at start: heap %llu MB, in use %llu MB, untouched top %llu MB\n", (unsigned long long)(size >> 20),
               (unsigned long long)(used >> 20), (unsigned long long)(top >> 20));
    }
    load_env_file(LSW_DATA_DIR "/env.txt");
    if (&lsw_big_alloc_min) {   // log75: blocks of this many KB or more from newlib, not rpmalloc's caches (0 = all rpmalloc)
        if (const char* e = getenv("LSWTCS_BIGALLOC")) lsw_big_alloc_min = size_t(atoi(e)) * 1024;
        printf("[HEAP] blocks of %zu KB or more from newlib's allocator (LSWTCS_BIGALLOC, 0 = all in rpmalloc)\n", lsw_big_alloc_min / 1024);
    }
    lsw_boot_mark("logs open, settings read");   // (log68) the steps of the 8.4 s before the save storage was up in run 67
    lsw_hw_monitor_start();
    lsw_hw_profiler_start();
    lsw_boot_mark("hardware monitor and profiler started");
#ifndef LSW_DEKO_BACKEND
    lsw_bo_check_init(g_run_id);
    lsw_boot_mark("buffer check ready");
#endif
    if (!lsw_save_init()) {
        // Never select a different user or silently save to SD after a native-storage error.
        char detail[256];
        snprintf(detail, sizeof detail, "%s\nError: 0x%X\nYour existing saves have not been replaced.",
                 lsw_save_error_stage(), lsw_save_last_error());
        ErrorApplicationConfig error{};
        if (R_SUCCEEDED(errorApplicationCreate(&error, "Unable to open TCS save data.", detail)))
            errorApplicationShow(&error);
        return false;
    }
    runtime_selftest();
    return true;
}

// ---- guest memory window ----------------------------------------------------------------------
// Probe #2 (Eden/EDEN_MEMORY.md): no overcommit, no svcMapPhysicalMemory, so each used range is
// heap memory turned into code memory and mapped RW (MapOwner) at its exact window offset.
// Unbacked parts of the window stay unmapped. NOTE: Eden does not fault on unmapped accesses
// (reads 0, drops writes); real hardware does.
// Measured on PC (kernel_stubs "memscan_now", hub, single-view): touched guest memory is
// 0x7FC/0x7FE MMIO pages, 0x80000000-0x9F400000 and the table; plus ONE page each at 0x00000000,
// 0x01000000 and 0x79600000 (near-null reads? not investigated yet; backed so hardware does not fault
// where Windows read zeros). The 0xE0000000 pool is skipped when LSWTCS_HEAPBYPASS=0 (the run config).
namespace {
}

// log89: 2 MB pages for guest memory. The kernel maps a range with 2 MB block descriptors only where its virtual address and
// the backing's physical address are 2 MB aligned alike; anything else gets 4 KB pages. virtmemFindAslr's second argument is a
// guard size, not an alignment, and the backings came from aligned_alloc(0x1000): run 90's window started at 0xeb5e75000, so the
// 512 MB guest physical range (nearly every guest load and store) and the 64 MB dispatch table were all 4 KB pages, and the
// A57's TLB (about 1024 entries, 4 MB at 4 KB a page) misses constantly over them. Technique from torchlight-nx's
// nfsmw-sdk.patch (GPL-3, technique only, no code taken), where a TLB miss there measured 100-170 ns on the console.
// Now the window is rounded up to 2 MB, and every range whose offset and size are multiples of 2 MB gets a 2 MB-aligned backing
// (2 MB more is asked for and the pointer rounded up; the backings are never freed). LSWTCS_GUEST2MB=0 = the old layout.
extern "C" uint8_t* lsw_switch_guest_window(size_t size) {
    constexpr size_t kBlock = 0x200000;
    const char* g2 = getenv("LSWTCS_GUEST2MB");
    const bool big = !(g2 && g2[0] == '0');
    virtmemLock();
    uint8_t* hole = (uint8_t*)virtmemFindAslr(size + (big ? kBlock : 0), 0x10000);
    uint8_t* w = hole && big ? (uint8_t*)(((uintptr_t)hole + kBlock - 1) & ~(uintptr_t)(kBlock - 1)) : hole;
    VirtmemReservation* res = w ? virtmemAddReservation(w, size) : nullptr;
    virtmemUnlock();
    if (!res) { printf("[NX] cannot reserve 0x%zX-byte guest window\n", size); return nullptr; }
    printf("[NX] guest window %p (0x%zX bytes, %s)\n", (void*)w, size,
           ((uintptr_t)w & (kBlock - 1)) == 0 ? "2 MB aligned" : "not 2 MB aligned: 4 KB pages");
    const char* hb = getenv("LSWTCS_HEAPBYPASS");
    const bool bypass = !(hb && hb[0] == '0');   // same default as main.cpp lswtcs_bypass_on(): ON unless "0"
    for (const LswGuestRange& r : lsw_guest_ranges) {
        if (r.off + r.size > size || (r.bypass_only && !bypass)) continue;
        const bool block = big && (r.off % kBlock) == 0 && (r.size % kBlock) == 0;
        void* src = nullptr;
        if (block) {
            void* raw = malloc(r.size + kBlock);
            if (raw) src = (void*)(((uintptr_t)raw + kBlock - 1) & ~(uintptr_t)(kBlock - 1));
        } else {
            src = aligned_alloc(0x1000, r.size);
        }
        if (!src) { printf("[NX]   %-40s alloc 0x%llX FAILED\n", r.what, (unsigned long long)r.size); return nullptr; }
        Handle h;
        Result rc = svcCreateCodeMemory(&h, src, r.size);
        if (R_SUCCEEDED(rc)) rc = svcControlCodeMemory(h, CodeMapOperation_MapOwner, w + r.off, r.size, Perm_Rw);
        if (R_FAILED(rc)) { printf("[NX]   %-40s map FAILED rc=0x%X\n", r.what, rc); return nullptr; }
        memset(w + r.off, 0, r.size);
        printf("[NX]   +0x%09llX %-40s %4llu MB, backing %p%s\n", (unsigned long long)r.off, r.what, (unsigned long long)(r.size >> 20),
               src, block ? " (2 MB aligned)" : "");
    }
    printf("[NX] UsedMemory after window: %llu MB\n", (unsigned long long)(info_u64(InfoType_UsedMemorySize) >> 20));
    return w;
}

// ---- entry ----------------------------------------------------------------------------------
static void* runtime_thread(void*) { int rc = lsw_runtime_main(); printf("[NX] runtime main returned %d\n", rc); return reinterpret_cast<void*>(intptr_t(rc)); }

int main(int, char**) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&t, &a, runtime_thread, nullptr) != 0) return 1;
    pthread_attr_destroy(&a);
    void* result = nullptr;
    if (pthread_join(t, &result) != 0) return 1;
    lsw_save_shutdown();
    const int rc = int(intptr_t(result));
    printf("[NX] main returning %d run=%016llX\n", rc, (unsigned long long)g_run_id);
#ifndef LSW_DEKO_BACKEND
    lsw_bo_check_finish();
#endif
    if (g_stdout_log) fflush(g_stdout_log);
    lsw_log_async_drain();
    return rc;
}
