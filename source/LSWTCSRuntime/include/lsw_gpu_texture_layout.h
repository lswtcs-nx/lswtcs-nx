// Shared guest-byte interpretation used by decoding and resource dependencies.
// Planning reads fetch words only. Visiting reads no RAM and reports EXACT
// source blocks, including endian swaps which cross 8/16-bit texel boundaries.
#pragma once
#include "lsw_gpu_bridge.h"
#include <cstdint>

struct LswGpuTextureLevel {
  uint32_t base, row_pitch, slice_stride, pitch_blocks;
  uint32_t offset_x, offset_y, width_blocks, height_blocks;
};
struct LswGpuTextureLayout {
  LswGpuTexDesc image; // owned metadata only; data remains null
  LswGpuTextureLevel levels[16];
  uint32_t tiled, endian_xor, block_bytes_log2;
};
struct LswGpuTextureBlock {
  uint32_t physical, decoded_offset, bytes, byte_xor;
};
extern "C" int lsw_gpu_plan_texture(const uint32_t* fetch6, LswGpuTextureLayout* out);
// Shared Xenos tiled addressing, also used for resolve destination words.
// Returns -1 for invalid geometry; does not mask/wrap a guest address.
extern "C" int64_t lsw_gpu_tiled_offset(uint32_t x,uint32_t y,uint32_t z,
    uint32_t pitch,uint32_t height,uint32_t log2_bytes,bool array);
// 1 = complete, 0 = invalid layout/address or the visitor requested failure.
// Never silently replaces an out-of-domain block with zeros.
extern "C" int lsw_gpu_visit_texture_blocks(const LswGpuTextureLayout* layout,
    bool (*visit)(void*, const LswGpuTextureBlock*), void* context);
