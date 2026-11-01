#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Guest layouts derived from the pinned Xenia kernel, not host ABI structs.
// The caller owns/reserves this arena through its guest physical allocator.
namespace lsw_kernel {
inline constexpr uint32_t arena_bytes = 0x10000, header_offset = 0x200;
struct Binding { uint32_t slot, offset, bytes; const char* name; };
inline constexpr Binding bindings[] = {
    {0x83290A3C, 0x000, 24, "KeTimeStampBundle"},
    {0x83290A6C, 0x020, 28, "ExEventObjectType"},
    {0x83290A78, 0x040, 28, "ExSemaphoreObjectType"},
    {0x83290A88, 0x060, 28, "ExTimerObjectType"},
    {0x83290AF0, 0x080, 28, "VdHSIOCalibrationLock"},
    {0x83290B04, 0x0A0, 4, "VdGpuClockInMHz"},
    {0x83290B4C, 0x0A8, 8, "XboxKrnlVersion"},
    {0x83290B58, 0x0B0, 4, "KeCertMonitorData"},
    // Preserve the game's existing device-cell alias, filled by game D3D init.
    {0x83290B90, 0x830CB7E4, 4, "VdGlobalDevice"},
    {0x83290BC8, 0x0B8, 4, "XexExecutableModuleHandle"},
    {0x83290BD8, 0x0C0, 14, "ExLoadedCommandLine"},
    {0x83290BDC, 0x0E0, 4, "KeDebugMonitorData"},
    {0x83290BF4, 0x100, 28, "ExThreadObjectType"},
};
inline uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void write32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline void write64(uint8_t* p, uint64_t v) {
    write32(p, uint32_t(v >> 32)); write32(p + 4, uint32_t(v));
}
inline uint32_t target(const Binding& b, uint32_t arena) {
    return b.slot == 0x83290B90 ? b.offset : arena + b.offset;
}
inline bool valid_header(const uint8_t* h, size_t n) {
    return h && n >= 24 && n <= arena_bytes - header_offset
        && !std::memcmp(h, "XEX2", 4) && read32(h + 8) == n
        && read32(h + 20) <= (n - 24) / 8;
}
inline bool initialize(uint8_t* arena, uint32_t address,
                       const uint8_t* header, size_t header_bytes) {
    if (!arena || (address & 0xFFF) || address < 0x88000000u
        || uint64_t(address) + arena_bytes > 0xA0000000ull
        || !valid_header(header, header_bytes)) return false;
    std::memset(arena, 0, arena_bytes);
    // Distinct object type identities; event/semaphore/timer allocation uses
    // actual imported pool thunks. Thread callbacks remain unsupported: native
    // ExCreateThread owns thread allocation and uses this object only as identity.
    const uint32_t offsets[] = {0x20, 0x40, 0x60, 0x100};
    const uint32_t tags[] = {0x76657645, 0x616D6553, 0x656D6954, 0x65726854};
    for (unsigned i = 0; i < 4; ++i) {
        if (i != 3) {
            write32(arena + offsets[i], 0x8213E3B0); // ExAllocatePoolTypeWithTag
            write32(arena + offsets[i] + 4, 0x8213DC50); // ExFreePool
        }
        write32(arena + offsets[i] + 24, tags[i]);
    }
    // X_RTL_CRITICAL_SECTION: 16-byte dispatch header + lock/recursion/owner.
    // lock_count is native-endian signed -1 in pinned Xenia (all-ones either way).
    arena[0x80] = 1; arena[0x81] = uint8_t((10000 + 255) >> 8);
    write32(arena + 0x80 + 16, 0xFFFFFFFFu);
    write32(arena + 0xA0, 500); // emulated Xenos MHz, never host GPU clocks
    // Pinned Xenia default compatibility version: 2.0.1888.0, four BE u16.
    arena[0xA9] = 2; arena[0xAC] = 0x07; arena[0xAD] = 0x60;
    std::memcpy(arena + 0xC0, "\"default.xex\"", 14);
    // Export -> cell -> LDR_DATA_TABLE_ENTRY -> copied XEX header.
    write32(arena + 0xB8, address + 0x140);
    write32(arena + 0x140 + 0x1C, 0x82000000);
    write32(arena + 0x140 + 0x20, 0x01460000);
    write32(arena + 0x140 + 0x38, 0x01460000);
    write32(arena + 0x140 + 0x3C, 0x8229E618);
    write32(arena + 0x140 + 0x58, address + header_offset);
    std::memcpy(arena + header_offset, header, header_bytes);
    return true;
}
inline void update_time(uint8_t* arena, uint64_t uptime_ms, uint64_t system_100ns) {
    write64(arena, uptime_ms * 10000ull);
    write64(arena + 8, system_100ns);
    write32(arena + 16, uint32_t(uptime_ms));
    write32(arena + 20, 0);
}
inline uint32_t header_field(const uint8_t* header, size_t n, uint32_t address, uint32_t key) {
    if (!valid_header(header, n)) return 0;
    for (uint32_t i = 0, count = read32(header + 20); i < count; ++i) {
        const uint32_t off = 24 + i * 8;
        if (read32(header + off) != key) continue;
        const uint32_t value = read32(header + off + 4);
        if ((key & 0xFF) == 0) return value;
        if ((key & 0xFF) == 1) return address + off + 4;
        return value < n ? address + value : 0;
    }
    return 0;
}
} // namespace lsw_kernel
