// A/B state U: the game's second audio mixer, guest sub_827F5990, runs off the guest turn, beside whichever guest thread holds it.
// The first step of running guest threads in parallel (docs/reviews/CPU_ROADMAP.md, route 1).
//
// Why this function: in run 25's heavy frames it was 21 % of the XAudio thread's turn (private/session-tools/thread_turn_leaves.py),
// and run 24 showed the XAudio thread waiting for the turn 28-40 % of the time, with the audio buffers emptier as the frame rate rose.
// What it does, from the recompiled code (ppc_recomp.498.cpp): arithmetic on its argument buffers and the stack, one call to
// sub_82511680 (an integer-to-double conversion, no memory but the stack), no kernel import, no spinlock, no atomic. On the 360 it
// ran on its own hardware thread beside the game; whatever it shares with other threads, the game guards with spinlocks there.
//
// How: the XAudio thread leaves the turn (lswtcs_gil_blocking_urgent), moves to core 2 at the audio priority as the native reverb
// does, runs the recompiled function unchanged, and takes the turn back. While it is off the turn, guest function entries must not
// run the scheduler's quantum (g_guest_off_turn, kernel_stubs.cpp). A caller holding a guest spinlock (g_spinlock_depth) keeps it
// on the turn, as before. Mutex handoffs at both ends order its memory accesses against the other threads'.
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_827F5990);
extern "C" void lswtcs_gil_blocking_urgent(void (*fn)(void*), void* arg);
extern "C" __attribute__((weak)) void lsw_offcore_begin() {}   // Switch: platform_switch.cpp (core 2, priority 0x2B)
extern "C" __attribute__((weak)) void lsw_offcore_end() {}
extern thread_local int g_spinlock_depth;   // kernel_stubs.cpp
extern thread_local bool g_guest_off_turn;  // kernel_stubs.cpp

namespace {
std::atomic<bool> g_on{[] { const char* e = getenv("LSWTCS_AUDIOOFF"); return e && e[0] == '1'; }()};
uint64_t g_calls = 0, g_off = 0, g_locked = 0, g_off_ns = 0, g_dsp_ns = 0;
inline uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Job { PPCContext* ctx; uint8_t* base; uint64_t dsp_ns; };
}  // namespace

extern "C" void lsw_audio_offturn_set(int on) { g_on.store(on != 0, std::memory_order_relaxed); }

// Strong definition: replaces the recompiled sub_827F5990 (a weak alias).
PPC_FUNC(sub_827F5990) {
    ++g_calls;
    if (!g_on.load(std::memory_order_relaxed) || g_guest_off_turn) { __imp__sub_827F5990(ctx, base); return; }
    if (g_spinlock_depth > 0) { ++g_locked; __imp__sub_827F5990(ctx, base); return; }
    Job job{&ctx, base, 0};
    const uint64_t t0 = now_ns();
    lswtcs_gil_blocking_urgent([](void* p) {
        Job& j = *static_cast<Job*>(p);
        lsw_offcore_begin();
        g_guest_off_turn = true;
        const uint64_t a = now_ns();
        __imp__sub_827F5990(*j.ctx, j.base);
        j.dsp_ns = now_ns() - a;
        g_guest_off_turn = false;
        lsw_offcore_end();
    }, &job);
    ++g_off;
    g_dsp_ns += job.dsp_ns;
    g_off_ns += now_ns() - t0;
    if ((g_off % 20000) == 0) {
        printf("[AUDIOOFF] %llu calls, %llu off the turn (%llu kept on: a spinlock held), per call %.3f ms mixing, %.3f ms with the handoffs\n",
               (unsigned long long)g_calls, (unsigned long long)g_off, (unsigned long long)g_locked, g_dsp_ns / 1e6 / g_off,
               g_off_ns / 1e6 / g_off);
        fflush(stdout);
    }
}
