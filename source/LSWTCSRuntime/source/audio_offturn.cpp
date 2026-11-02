// A/B state U: the XAudio thread's voice processing, guest sub_827DDE40, runs off the guest turn, beside whichever guest thread holds it.
// The first real step of running guest threads in parallel (docs/reviews/CPU_ROADMAP.md, route 1).
//
// Why this function: in run 31's heavy frames it was 57 % of the XAudio thread's turn (private/session-tools/thread_inclusive.py), and
// run 27 showed that single small calls lose: 0.010 ms of mixing against 0.41 ms of turn handoffs, so log25's sub_827F5990 is gone.
// sub_827F1258, one level down, is reached per voice through a dispatch table. This one is above that table.
// What it reaches, from the recompiled code (private/session-tools/offturn_audit.py): only the XAudio thread on the run 31 stacks; spinlocks,
// which state L makes real; two pure kernel stubs; and a critical section in sub_827D9DF8. Its indirect calls cannot be followed statically,
// so any kernel stub not marked as free of the turn takes the turn back and the rest runs on it (kernel_stubs.cpp, offturn_rejoin).
// On the 360 this ran on its own hardware thread beside the game; whatever it shares, the game guards with spinlocks there.
// The risk left: the generated code drops sync/lwsync, so lock-free sharing with no lock around it is not ordered on ARM. Expect rare
// corruption rather than crashes if that exists.
//
// Needs L: a section left off the turn while holding a lock taken only through the turn would not exclude anyone.
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_827DDE40);
extern "C" bool lswtcs_gil_offturn(void (*fn)(void*), void* arg);   // kernel_stubs.cpp: false if it came back to the turn on the way
extern "C" __attribute__((weak)) void lsw_offcore_begin() {}   // Switch: platform_switch.cpp (core 2, priority 0x2B)
extern "C" __attribute__((weak)) void lsw_offcore_end() {}
extern thread_local bool g_guest_off_turn;  // kernel_stubs.cpp
extern "C" bool lsw_spin_all_real();        // kernel_stubs.cpp: L is on and every spinlock held is really taken
extern "C" uint64_t lsw_offturn_rejoins();

namespace {
std::atomic<bool> g_on{false};
uint64_t g_calls = 0, g_off = 0, g_stayed = 0, g_on_ns = 0, g_off_work_ns = 0, g_off_total_ns = 0, g_on_calls = 0, g_last_ns = 0, g_last_calls = 0;
inline uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Job { PPCContext* ctx; uint8_t* base; uint64_t work_ns; };
void report(uint64_t now) {
    if (now - g_last_ns < 5000000000ull) return;
    const double secs = g_last_ns ? (now - g_last_ns) / 1e9 : 1.0;
    printf("[AUDIOOFF] %llu calls, %.0f a second, %llu off the turn, %llu stayed off to the end, %llu returns for a kernel stub | on the turn %.3f ms per call; "
           "off it %.3f ms of work, %.3f ms with the handoffs\n", (unsigned long long)g_calls, (g_calls - g_last_calls) / secs, (unsigned long long)g_off,
           (unsigned long long)g_stayed, (unsigned long long)lsw_offturn_rejoins(), g_on_calls ? g_on_ns / 1e6 / g_on_calls : 0.0,
           g_off ? g_off_work_ns / 1e6 / g_off : 0.0, g_off ? g_off_total_ns / 1e6 / g_off : 0.0);
    fflush(stdout);
    g_last_ns = now; g_last_calls = g_calls;
}
}  // namespace

extern "C" void lsw_audio_offturn_set(int on) { g_on.store(on != 0, std::memory_order_relaxed); }
extern "C" void lsw_audio_offturn_counts(uint64_t* out) {   // [ABSTAT]; racy reads of running totals
    out[0] = g_calls; out[1] = g_off; out[2] = g_stayed; out[3] = g_off_work_ns; out[4] = g_off_total_ns; out[5] = g_on_ns;
}

// Strong definition: replaces the recompiled sub_827DDE40 (a weak alias) for every caller and the call table.
PPC_FUNC(sub_827DDE40) {
    if (g_guest_off_turn) { __imp__sub_827DDE40(ctx, base); return; }
    ++g_calls;
    const uint64_t t0 = now_ns();
    if (!g_on.load(std::memory_order_relaxed) || !lsw_spin_all_real()) {
        __imp__sub_827DDE40(ctx, base);
        const uint64_t t1 = now_ns();
        g_on_ns += t1 - t0; ++g_on_calls;
        report(t1);
        return;
    }
    Job job{&ctx, base, 0};
    const bool stayed = lswtcs_gil_offturn([](void* p) {
        Job& j = *static_cast<Job*>(p);
        const uint64_t a = now_ns();
        __imp__sub_827DDE40(*j.ctx, j.base);
        j.work_ns = now_ns() - a;
    }, &job);
    const uint64_t t1 = now_ns();
    ++g_off;
    if (stayed) ++g_stayed;
    g_off_work_ns += job.work_ns;
    g_off_total_ns += t1 - t0;
    report(t1);
}
