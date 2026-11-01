// Included at the END of the real folded ppc_context.h. Observation is opt-in;
// the other configuration keeps raw generated operations. Keep the integration
// separate from the observer API so include order
// cannot accidentally disable scalar hooks. All SIMD headers must precede it.
#pragma once
#include "lsw_guest_memory_access.h"

#ifdef LSW_GUEST_MEMORY_TRACKING

// PPC_HOST itself may evaluate its argument twice. Evaluate addresses before
// expansion for loads as well as stores, preserving volatile access semantics.
#undef PPC_LOAD_U8
#undef PPC_LOAD_U16
#undef PPC_LOAD_U32
#undef PPC_LOAD_U64
#define PPC_LOAD_U8(x) ([&]() { const uint32_t a_=uint32_t(x); return lsw_guest::load<uint8_t>(PPC_HOST(a_)); }())
#define PPC_LOAD_U16(x) ([&]() { const uint32_t a_=uint32_t(x); return __builtin_bswap16(lsw_guest::load<uint16_t>(PPC_HOST(a_))); }())
#define PPC_LOAD_U32(x) ([&]() { const uint32_t a_=uint32_t(x); return __builtin_bswap32(lsw_guest::load<uint32_t>(PPC_HOST(a_))); }())
#define PPC_LOAD_U64(x) ([&]() { const uint32_t a_=uint32_t(x); return __builtin_bswap64(lsw_guest::load<uint64_t>(PPC_HOST(a_))); }())

// Redefine here even if the API was already included by another runtime header.
#undef PPC_STORE_U8
#undef PPC_STORE_U16
#undef PPC_STORE_U32
#undef PPC_STORE_U64
#define PPC_STORE_U8(x,y) do { const uint32_t a_=uint32_t(x); lsw_guest::store<uint8_t>(PPC_HOST(a_), uint8_t(y)); } while (0)
#define PPC_STORE_U16(x,y) do { const uint32_t a_=uint32_t(x); lsw_guest::store<uint16_t>(PPC_HOST(a_), __builtin_bswap16(uint16_t(y))); } while (0)
#define PPC_STORE_U32(x,y) do { const uint32_t a_=uint32_t(x); lsw_guest::store<uint32_t>(PPC_HOST(a_), __builtin_bswap32(uint32_t(y))); } while (0)
#define PPC_STORE_U64(x,y) do { const uint32_t a_=uint32_t(x); lsw_guest::store<uint64_t>(PPC_HOST(a_), __builtin_bswap64(uint64_t(y))); } while (0)

namespace lsw_guest {
inline simde__m128i simd_load(const simde__m128i* source) noexcept {
  ReadScope read(source, 16);
  return simde_mm_load_si128(source);
}
inline void simd_store(simde__m128i* destination, simde__m128i value) noexcept {
  WriteScope write(destination, 16);
  simde_mm_store_si128(destination, value);
}
} // namespace lsw_guest
// Only generated operands containing PPC_HOST receive these tags. Host vector
// registers/temporaries keep the original SIMD intrinsics and optimizer freedom.
#define LSW_GUEST_SIMD_LOAD lsw_guest::simd_load
#define LSW_GUEST_SIMD_STORE lsw_guest::simd_store
#define LSW_GUEST_CAS lsw_guest::compare_exchange
#define LSW_GUEST_FILL lsw_guest::fill
#define LSW_GUEST_RESERVATION_LOAD(type,host) lsw_guest::load<type>(host)
#else
// Preserve the original generated access operations in the Mesa configuration.
#define LSW_GUEST_SIMD_LOAD simde_mm_load_si128
#define LSW_GUEST_SIMD_STORE simde_mm_store_si128
#define LSW_GUEST_CAS __sync_bool_compare_and_swap
#define LSW_GUEST_FILL std::memset
#define LSW_GUEST_RESERVATION_LOAD(type,host) (*(type*)(host))
#endif
