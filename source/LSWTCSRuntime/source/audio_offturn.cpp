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
extern "C" bool lsw_spin_all_real();        // kernel_stubs.cpp: every spinlock held is really taken (state L)
extern "C" void lsw_spin_report(uint64_t* taken, uint64_t* waits);
extern "C" void lsw_spin_report2(uint64_t* forced, uint64_t* yield_to);

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
    if (!g_on.load(std::memory_order_relaxed) || g_guest_off_turn) {
        __imp__sub_827F5990(ctx, base);
        if ((g_calls % 2000) == 0) {
            uint64_t taken = 0, waits = 0, forced = 0, yto = 0;
            lsw_spin_report(&taken, &waits);
            lsw_spin_report2(&forced, &yto);
            printf("[AUDIOOFF] %llu calls (U off now), %llu off the turn so far | real spinlocks taken %llu, waited %llu, turn passed to the "
                   "holder %llu, taken after 200 ms %llu\n", (unsigned long long)g_calls, (unsigned long long)g_off, (unsigned long long)taken,
                   (unsigned long long)waits, (unsigned long long)yto, (unsigned long long)forced);
            fflush(stdout);
        }
        return;
    }
    // Under a spinlock only when it is really taken (state L); a lock held only through the turn keeps the call on the turn.
    if (g_spinlock_depth > 0 && !lsw_spin_all_real()) {
        ++g_locked;
        __imp__sub_827F5990(ctx, base);
        if ((g_calls % 2000) == 0) {
            printf("[AUDIOOFF] %llu calls, %llu off the turn, %llu kept on: a spinlock held through the turn\n", (unsigned long long)g_calls,
                   (unsigned long long)g_off, (unsigned long long)g_locked);
            fflush(stdout);
        }
        return;
    }
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
    // Run 26 printed nothing (every 20,000th call, and none went off): now every 2,000 calls, off or not.
    if ((g_calls % 2000) == 0) {
        uint64_t taken = 0, waits = 0;
        lsw_spin_report(&taken, &waits);
        printf("[AUDIOOFF] %llu calls, %llu off the turn (%llu kept on: a spinlock held through the turn), per call %.3f ms mixing, "
               "%.3f ms with the handoffs | real spinlocks taken %llu, waited %llu\n",
               (unsigned long long)g_calls, (unsigned long long)g_off, (unsigned long long)g_locked, g_off ? g_dsp_ns / 1e6 / g_off : 0.0,
               g_off ? g_off_ns / 1e6 / g_off : 0.0, (unsigned long long)taken, (unsigned long long)waits);
        fflush(stdout);
    }
}
