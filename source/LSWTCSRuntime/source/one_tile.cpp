// LSWTCS_ONETILE=1: the game draws its world once per frame instead of once per EDRAM tile.
// docs/reviews/CPU_ROADMAP.md, "Big picture after run 32".
//
// The 360's EDRAM holds 2048 tiles of 5120 bytes. The engine's own guest sub_82316AF8 adds up what one set of render targets needs; above 2048 it
// splits the screen into predicated tiles (three in Episode I chapter 2: two colour targets and depth at 1280x704, 2x MSAA), and every draw is then
// walked, recorded and merged three times. At or below 2048 it takes a plain path, which each user of the tile count also has
// (sub_82316530, sub_823168D0, sub_82316978). This port has no EDRAM limit: a host target is keyed by base, pitch and format, 2048 rows tall (gl_rt.inc).
//
// So while the engine lays out a set, the helper it sizes surfaces with (sub_822A3CE0) answers a quarter of the real size. The layout then fits,
// and the bases it hands out stay apart: 0, 352 and 1696 for that set. Only the layout's own calls see it.
// MSAA and the resolution do not change, and the port already draws 2x MSAA at one sample (LSWTCS_MSAA1X), so the picture should not.
//
// Decided at boot: a set's surfaces are created from this layout, so it does not switch during play.
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_82316AF8);
extern "C" PPC_FUNC(__imp__sub_82316FB8);
extern "C" PPC_FUNC(__imp__sub_822A3CE0);

namespace {
thread_local int g_laying_out = 0;
bool on() { static const bool v = [] { const char* e = getenv("LSWTCS_ONETILE"); return e && e[0] == '1'; }(); return v; }
}  // namespace

// Strong definitions: replace the recompiled weak aliases for every caller and the call table.
PPC_FUNC(sub_822A3CE0) {
    // Only the layout's own calls: creating the surface on the way (sub_823243D0 > sub_822A63B8 > sub_822A5958) also asks, and gets the real size.
    const uint32_t lr = uint32_t(ctx.lr);
    const bool layout = (lr >= 0x82316AF8u && lr < 0x82316E20u) || (lr >= 0x823243D0u && lr < 0x82324578u);
    __imp__sub_822A3CE0(ctx, base);
    if (g_laying_out > 0 && layout) ctx.r3.u32 = (ctx.r3.u32 + 3) / 4;
}

PPC_FUNC(sub_82316AF8) {
    const uint32_t set = ctx.r3.u32;
    if (on()) ++g_laying_out;
    __imp__sub_82316AF8(ctx, base);
    if (on()) --g_laying_out;
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) < 60) {
        printf("[ONETILE] %s: set %08X %dx%d tile count %u, colour %u + depth %u EDRAM tiles\n", on() ? "on" : "off", set, int(PPC_LOAD_U32(set + 240)),
               int(PPC_LOAD_U32(set + 244)), PPC_LOAD_U32(set + 264), PPC_LOAD_U32(set + 256), PPC_LOAD_U32(set + 260));
        fflush(stdout);
    }
}

PPC_FUNC(sub_82316FB8) {
    if (on()) ++g_laying_out;
    __imp__sub_82316FB8(ctx, base);
    if (on()) --g_laying_out;
}
