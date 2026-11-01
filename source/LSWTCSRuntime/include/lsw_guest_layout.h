#pragma once
#include <cstdint>

// One mapping contract for Switch and the Windows sparse-memory diagnostic.
struct LswGuestRange { uint64_t off, size; const char* what; bool bypass_only; };
inline constexpr LswGuestRange lsw_guest_ranges[] = {
    {0x00000000ull, 0x00100000ull, "stray page 0x00000000 (1 MB)", false},
    {0x01000000ull, 0x00100000ull, "stray page 0x01000000 (1 MB)", false},
    {0x79600000ull, 0x00100000ull, "stray page 0x79600000 (1 MB)", false},
    {0x7F000000ull, 0x01000000ull, "GPU writeback + MMIO 0x7F000000", false},
    {0x80000000ull, 0x20000000ull, "physical 512 MB (image, vheap, phys)", false},
    {0xE0000000ull, 0x10000000ull, "small-alloc pool (unless HEAPBYPASS=0)", true},
    {0x100000000ull, 0x04000000ull, "dispatch table", false},
};
