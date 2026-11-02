#include "lsw_guest_host_lock.h"
#include "lsw_guest_memory_access.h"
// ── XMA decoder hardware (replaces Xenia's XmaDecoder; the per-context decode is Xenia's
// XmaContextNew, compiled unchanged against xma/shim) ─────────────────────────────────────────
// The 360 decodes XMA in hardware: 320 64-byte contexts in physical memory, whose physical base
// the game reads from MMIO 0x7FEA1800 (ContextArrayAddress). The game's XMA library writes
// bitmasks to Kick (0x7FEA1940+), Lock (0x7FEA1A40+) and Clear (0x7FEA1A80+) registers; those
// stwbrx sites in ppc_recomp.493.cpp call lswtcs_xma_mmio_store (our guest memory has no MMIO
// traps). Kicks decode synchronously (Xenia use_dedicated_xma_thread=false behaviour) and a host
// worker thread keeps decoding enabled contexts afterwards, like Xenia's WorkerThreadMain.
#include <windows.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "xenia/apu/xma_context_new.h"
#include "xenia/memory.h"

extern "C" void dbg_ram(const char* fmt, ...);
extern "C" void lswtcs_sched_snapshot();   // DIAG (kernel_stubs.cpp)
extern "C" __attribute__((weak)) void lsw_walkoff_report() {}   // kernel_stubs.cpp: [WALKOFF]
extern "C" __attribute__((weak)) void lsw_offcore_begin_prio(int) {}   // Switch: platform_switch.cpp
extern "C" __attribute__((weak)) void lsw_prof_register(const char*, int) {}   // Switch: hw_profiler.cpp
extern "C" __attribute__((weak)) int lsw_core3_begin(const char*) { return 0; }   // Switch: platform_switch.cpp (fourth core)

namespace {
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kMmioBase = 0x7FEA0000u;
constexpr uint32_t kRegContextArrayAddress = 0x600, kRegKick = 0x650, kRegLock = 0x690,
                   kRegClear = 0x6A0;

uint8_t* g_xbase = nullptr;
xe::Memory* g_mem = nullptr;
xe::apu::XmaContext* g_ctx[kContextCount] = {};
uint32_t g_first = 0;                 // guest VA of context 0
bool g_used[kContextCount] = {};
std::mutex g_alloc_mx;
// The worker's wakeup is its own, and a kick takes no lock. Runs 10 and 11: a kick that locked anything the worker also holds spent 20 % of the heavy band's
// turn there, first on the mutex of all wait objects, then on an empty lock of this one (disassembly of run 11's samples). A woken thread probably owns the
// mutex before it gets a core, so the guest turn waited for busy core 2. Notifying without the mutex can lose a wakeup in the moment before the worker
// waits; that costs at most the 4 ms timeout.
std::mutex g_wake_mx;
std::condition_variable g_wake_cv;
std::atomic<bool> g_wake_pending{false}, g_worker_waiting{false};
std::atomic<uint64_t> g_fn[8];   // DIAG call counters (lswtcs_fncount)
std::atomic<uint64_t> g_kicks{0}, g_works{0}, g_locks{0}, g_clears{0}, g_other{0};
// A/B state X: a kick only wakes the worker, as the 360's decoder runs beside the CPU, instead of decoding on the thread that
// holds the guest scheduler's turn (run 8: about 18 % of the audio thread's turn in heavy scenes).
std::atomic<bool> g_kick_async{false};

void wake_worker() {
    g_wake_pending.store(true);
    if (g_worker_waiting.load()) g_wake_cv.notify_one();
}
// Still wakes every 4 ms without a kick: a context waiting for output space has nothing to wake it.
void worker_wait() {
    g_worker_waiting.store(true);
    if (!g_wake_pending.load()) {
        std::unique_lock<std::mutex> lk(g_wake_mx);
        g_wake_cv.wait_for(lk, std::chrono::milliseconds(4), [] { return g_wake_pending.load(); });
    }
    g_worker_waiting.store(false);
    g_wake_pending.store(false);
}

void worker_main() {
    // On core 2 beside the audio output thread: a frame decodes in well under a millisecond, and on core 0 this thread
    // would compete with whichever guest thread holds the turn.
    lsw_offcore_begin_prio(0x2B);
    lsw_core3_begin("xma");   // only with the fourth core
    lsw_prof_register("xma", -1);
    for (;;) {
        bool did = false;
        for (uint32_t i = 0; i < kContextCount; ++i)
            if (g_ctx[i] && g_ctx[i]->is_enabled() && g_ctx[i]->Work()) { did = true; ++g_works; }
        if (!did) worker_wait();
        static DWORD last = GetTickCount();
        if (GetTickCount() - last >= 5000) {   // periodic activity report
            last = GetTickCount();
            uint32_t en = 0, al = 0;
            for (uint32_t i = 0; i < kContextCount; ++i) if (g_ctx[i]) { en += g_ctx[i]->is_enabled(); al += g_ctx[i]->is_allocated(); }
            dbg_ram("[XMA] stat: kicks=%llu locks=%llu clears=%llu other=%llu works=%llu allocated=%u enabled=%u\n",
                    (unsigned long long)g_kicks.load(), (unsigned long long)g_locks.load(), (unsigned long long)g_clears.load(),
                    (unsigned long long)g_other.load(), (unsigned long long)g_works.load(), al, en);
            lswtcs_sched_snapshot();
            lsw_walkoff_report();
            dbg_ram("[XMA] chain: DAA20=%llu DAB90=%llu F1258=%llu EF820=%llu EF920=%llu F0D30=%llu\n",
                    (unsigned long long)g_fn[0].load(), (unsigned long long)g_fn[1].load(), (unsigned long long)g_fn[2].load(),
                    (unsigned long long)g_fn[3].load(), (unsigned long long)g_fn[4].load(), (unsigned long long)g_fn[5].load());
        }
    }
}
}  // namespace

extern "C" void lsw_xma_async_set(int on) { g_kick_async.store(on != 0, std::memory_order_relaxed); }
extern "C" void lswtcs_fncount(int id) { if (id >= 0 && id < 8) ++g_fn[id]; }

// base = g_base; ctx_va = guest VA of a 320*64-byte, 256-aligned block in physical memory.
extern "C" int lswtcs_xma_init(uint8_t* base, uint32_t ctx_va) {
    if (g_mem) return 1;
    g_xbase = base;
    g_mem = new xe::Memory(base);
    g_first = ctx_va;
    lsw_guest::fill(ppc_view_base[ctx_va >> 29] + ctx_va, 0, kContextCount * 64);  // folded (single-view)
    for (uint32_t i = 0; i < kContextCount; ++i) {
        auto* c = new xe::apu::XmaContextNew();
        if (c->Setup(i, g_mem, ctx_va + i * 64)) { dbg_ram("[XMA] context %u setup FAILED\n", i); delete c; continue; }
        g_ctx[i] = c;
    }
    // Registers the game READS: ContextArrayAddress (lwbrx -> stored little-endian).
    uint32_t phys = ctx_va & 0x1FFFFFFFu;
    lsw_guest::store<uint32_t>(base + kMmioBase + kRegContextArrayAddress * 4, phys);
    std::thread(worker_main).detach();
    dbg_ram("[XMA] decoder up: %u contexts at VA 0x%08X (phys 0x%08X)\n", kContextCount, ctx_va, phys);
    printf("[XMA] decoder up: %u contexts at VA 0x%08X\n", kContextCount, ctx_va); fflush(stdout);
    return 1;
}

extern "C" uint32_t lswtcs_xma_alloc() {
    if (!g_mem) return 0;
    std::unique_lock<std::mutex> lk(g_alloc_mx, std::defer_lock);
    lsw_guest::lock_host(lk);
    for (uint32_t i = 0; i < kContextCount; ++i)
        if (!g_used[i] && g_ctx[i]) { g_used[i] = true; g_ctx[i]->set_is_allocated(true); return g_first + i * 64; }
    return 0;
}

extern "C" void lswtcs_xma_release(uint32_t ptr) {
    if (!g_mem || ptr < g_first || ptr >= g_first + kContextCount * 64) return;
    uint32_t i = (ptr - g_first) >> 6;
    std::unique_lock<std::mutex> lk(g_alloc_mx, std::defer_lock);
    lsw_guest::lock_host(lk);
    if (g_ctx[i]) g_ctx[i]->Release();
    g_used[i] = false;
}

// A guest stwbrx to addr; value = the source register (the logical register value). The raw
// store keeps stwbrx semantics (host-native bytes, read back by the game's lwbrx).
extern "C" void lswtcs_xma_mmio_store(uint8_t* base, uint32_t addr, uint32_t value) {
    lsw_guest::store<uint32_t>(base + addr, value);
    if ((addr & 0xFFFF0000u) != kMmioBase || !g_mem) return;
    uint32_t r = (addr & 0xFFFF) / 4;
    if (r >= kRegKick && r < kRegKick + 10) {
        uint32_t id0 = (r - kRegKick) * 32, v = value;
        while (v) {
            uint32_t id = id0 + std::countr_zero(v); v &= v - 1;
            if (id < kContextCount && g_ctx[id]) {
                g_ctx[id]->Enable();
                if (!g_kick_async.load(std::memory_order_relaxed) && g_ctx[id]->Work()) ++g_works;
                ++g_kicks;
            }
        }
        wake_worker();
        static uint64_t last = 0;
        if (g_kicks - last >= 2000 || last == 0) { last = g_kicks; dbg_ram("[XMA] kicks=%llu works=%llu\n", (unsigned long long)g_kicks.load(), (unsigned long long)g_works.load()); }
    } else if (r >= kRegLock && r < kRegLock + 10) {
        ++g_locks;
        uint32_t id0 = (r - kRegLock) * 32, v = value;
        while (v) {
            uint32_t id = id0 + std::countr_zero(v); v &= v - 1;
            if (id < kContextCount && g_ctx[id]) { g_ctx[id]->Disable(); g_ctx[id]->Block(false); }
        }
    } else if (r >= kRegClear && r < kRegClear + 10) {
        ++g_clears;
        uint32_t id0 = (r - kRegClear) * 32, v = value;
        while (v) {
            uint32_t id = id0 + std::countr_zero(v); v &= v - 1;
            if (id < kContextCount && g_ctx[id]) g_ctx[id]->Clear();
        }
    } else {
        ++g_other;
    }
}
