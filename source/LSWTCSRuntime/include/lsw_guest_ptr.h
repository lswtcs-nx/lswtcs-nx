// log135: guest pointer range checks.
//
// Runtime and hand-written patches in the generated game code accept guest pointers in 0x80000000 (or 0x82000000) to
// 0x94000000, where the image, the virtual heap and the physical blocks lie. log135 adds a range for virtual
// reservations the 512 MB cannot hold (LSW_VEXT_LO-LSW_VEXT_HI): address space whose memory the runtime maps only when
// the game commits pages, as on the console. workspace-tools/widen_guest_ptr_checks.py rewrites the checks to these
// helpers.
#pragma once
#include <stdint.h>

#define LSW_VEXT_LO 0x60000000u
#define LSW_VEXT_HI 0x70000000u

static inline bool lsw_vext_ptr(uint32_t a) { return a - LSW_VEXT_LO < LSW_VEXT_HI - LSW_VEXT_LO; }
static inline bool lsw_gptr80(uint32_t a) { return a - 0x80000000u < 0x14000000u || lsw_vext_ptr(a); }
static inline bool lsw_gptr82(uint32_t a) { return a - 0x82000000u < 0x12000000u || lsw_vext_ptr(a); }
