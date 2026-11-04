// Guest function linkage for GameCode units built with LSW_GAME_INLINE (tools/make_game_units.py). Included by each unit after
// ppc_recomp_shared.h and before the generated files, so the generated output stays as checked (tools/generated_output.py).
//
// The generated code declares each function as `__attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);`: a weak,
// noinline alias, so the runtime can replace it and no guest call is ever inlined. Here only the functions in
// lsw_guest_overrides.h stay weak and noinline. The others become strong aliases, which GCC can inline into a caller in the same
// unit (with -fno-semantic-interposition, as GameCode builds with -fPIC). Inlining keeps the generated semantics; the guest
// prologue (call counter, GIL quantum) runs inside the caller.
#pragma once
#include "lsw_guest_overrides.h"

#define LSW_OVR_SECOND_(a, b, ...) b
#define LSW_OVR_SECOND(...) LSW_OVR_SECOND_(__VA_ARGS__, 0, ~)
#define LSW_OVR_IS(x) LSW_OVR_SECOND(LSW_OVR_##x)   // 1 when lsw_guest_overrides.h defines LSW_OVR_x as "~, 1", else 0
#define LSW_LINK_1 __attribute__((weak, noinline))
#define LSW_LINK_0
#define LSW_LINK_(v) LSW_LINK_##v
#define LSW_LINK(v) LSW_LINK_(v)
#undef PPC_WEAK_FUNC
#define PPC_WEAK_FUNC(x) LSW_LINK(LSW_OVR_IS(x)) PPC_FUNC(x)
