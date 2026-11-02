// Native replacement for the game's gain-ramp mixer, guest sub_827E51D0, as A/B state N (kernel_stubs.cpp has no part in it).
// Runs 11 and 12: with the XMA lock gone, the audio thread's own work is most of what it costs in the heavy band, and this function
// alone is about 19 % of that thread's samples, 2.5 % of the guest turn.
//
// What it does (from the recompiled code, ppc_recomp.495.cpp): r3 = destination, r4 = source, r5 = start gain, r6 = gain step
// (both vectors of 4 floats), r7 = float count. For each group of 4 vectors (64 bytes), dst[k] = src[k] * gain_k + dst[k], with
// gain_k = start + k * step for the four vectors and each one advanced by 4 * step per group, all as the recompiled code orders
// its additions. A count that is not a multiple of 16 floats still processes whole groups, as the guest does.
//
// The arithmetic follows the recompiled code in the same order. Its build fuses each vmaddfp's multiply and add into one FMLA (one
// rounding; checked in the disassembly of __imp__sub_827E51D0), so `madd` does the same explicitly; the gain additions stay separate,
// as there. What is saved is the context traffic (every vector register a store and a load through PPCContext) and the guest memory
// wrappers.
// LSWTCS_NATIVEMIX=1 switches it on; state N of LSWTCS_ABSTATES does the same during play. Every 64th call runs the guest code on the
// real buffer and the native code on a copy, and compares the bytes: the first difference prints [NATIVEMIX] MISMATCH and sends
// every later call to the guest code for the rest of the session.
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_827E51D0);

namespace {
std::atomic<bool> g_on{[] { const char* e = getenv("LSWTCS_NATIVEMIX"); return e && e[0] == '1'; }()};
bool g_broken = false;
uint64_t g_calls = 0, g_checks = 0;

inline simde__m128 load_vec(uint8_t* base, uint32_t a) {
    return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(PPC_HOST(a & ~0xFu))),
                                                       simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}
inline void store_vec(uint8_t* base, uint32_t a, simde__m128 v) {
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(PPC_HOST(a & ~0xFu)),
                         simde_mm_shuffle_epi8(simde_mm_castps_si128(v), simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

// a * b + c with one rounding, as the recompiled build computes vmaddfp.
inline simde__m128 madd(simde__m128 a, simde__m128 b, simde__m128 c) {
    return simde__m128_from_neon_f32(vfmaq_f32(simde__m128_to_neon_f32(c), simde__m128_to_neon_f32(a), simde__m128_to_neon_f32(b)));
}
// `dst_host` redirects the destination to a host copy (the check); null = guest memory.
void mix(PPCContext& ctx, uint8_t* base, uint32_t dst, uint32_t src, uint32_t gain, uint32_t step, uint32_t count, uint8_t* dst_host) {
    const simde__m128 v0_step = load_vec(base, step);
    const simde__m128 g0_start = load_vec(base, gain);
    ctx.fpscr.enableFlushMode();
    const simde__m128 step2 = simde_mm_add_ps(v0_step, v0_step);
    simde__m128 g1 = simde_mm_add_ps(g0_start, v0_step);
    const simde__m128 step3 = simde_mm_add_ps(step2, v0_step);
    const simde__m128 step4 = simde_mm_add_ps(step2, step2);
    simde__m128 g2 = simde_mm_add_ps(g0_start, step2);
    simde__m128 g3 = simde_mm_add_ps(g0_start, step3);
    simde__m128 g0 = g0_start;
    const uint32_t vectors = count >> 2;
    if (!vectors) return;
    const uint32_t groups = ((vectors - 1) >> 2) + 1;
    auto dload = [&](uint32_t a) {
        if (!dst_host) return load_vec(base, a);
        uint8_t* p = dst_host + ((a & ~0xFu) - (dst & ~0xFu));
        return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p)),
                                                           simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
    };
    auto dstore = [&](uint32_t a, simde__m128 v) {
        if (!dst_host) { store_vec(base, a, v); return; }
        uint8_t* p = dst_host + ((a & ~0xFu) - (dst & ~0xFu));
        simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(p),
                              simde_mm_shuffle_epi8(simde_mm_castps_si128(v), simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
    };
    for (uint32_t i = 0; i < groups; ++i) {
        const uint32_t d = dst + i * 64, s = src + i * 64;
        const simde__m128 r3 = madd(load_vec(base, s + 48), g3, dload(d + 48));
        const simde__m128 r0 = madd(load_vec(base, s), g0, dload(d));
        g0 = simde_mm_add_ps(g0, step4);
        const simde__m128 r1 = madd(load_vec(base, s + 16), g1, dload(d + 16));
        const simde__m128 r2 = madd(load_vec(base, s + 32), g2, dload(d + 32));
        g1 = simde_mm_add_ps(g1, step4);
        g2 = simde_mm_add_ps(g2, step4);
        g3 = simde_mm_add_ps(g3, step4);
        dstore(d + 48, r3);
        dstore(d, r0);
        dstore(d + 16, r1);
        dstore(d + 32, r2);
    }
}
}  // namespace

extern "C" void lsw_native_mix_set(int on) { g_on.store(on != 0 && !g_broken, std::memory_order_relaxed); }

// Strong definition: replaces the recompiled sub_827E51D0 (a weak alias).
PPC_FUNC(sub_827E51D0) {
    if (!g_on.load(std::memory_order_relaxed)) { __imp__sub_827E51D0(ctx, base); return; }
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, gain = ctx.r5.u32, step = ctx.r6.u32, count = ctx.r7.u32;
    const uint32_t vectors = count >> 2, bytes = vectors ? (((vectors - 1) >> 2) + 1) * 64 : 0;
    const bool overlap = bytes && (src & ~0xFu) < (dst & ~0xFu) + bytes && (dst & ~0xFu) < (src & ~0xFu) + bytes;
    if ((++g_calls % 64) == 0 && bytes && bytes <= (1u << 20) && !overlap) {
        static std::vector<uint8_t> copy;
        copy.assign(PPC_HOST(dst & ~0xFu), PPC_HOST(dst & ~0xFu) + bytes);
        __imp__sub_827E51D0(ctx, base);   // the real result, from the guest code
        mix(ctx, base, dst, src, gain, step, count, copy.data());
        ++g_checks;
        if (std::memcmp(copy.data(), PPC_HOST(dst & ~0xFu), bytes)) {
            size_t i = 0;
            while (copy[i] == PPC_HOST(dst & ~0xFu)[i]) ++i;
            g_broken = true;
            g_on.store(false, std::memory_order_relaxed);
            printf("[NATIVEMIX] MISMATCH at byte %zu of %u after %llu checks (count %u): native mixing off for this session\n", i, bytes,
                   (unsigned long long)g_checks, count);
            fflush(stdout);
        } else if (g_checks == 1 || (g_checks % 20000) == 0) {
            printf("[NATIVEMIX] %llu calls, %llu checks, all equal\n", (unsigned long long)g_calls, (unsigned long long)g_checks);
            fflush(stdout);
        }
        return;
    }
    mix(ctx, base, dst, src, gain, step, count, nullptr);
    // The recompiled code leaves its last register values behind and saves r31 below the stack pointer; callers treat both as
    // scratch (r3 to r12 and v0 to v13 are volatile in the PowerPC ABI), so neither is reproduced.
}
