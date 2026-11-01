// Private diagnostic capture, not a shipping asset format. Both tools check the
// 64-bit little-endian ABI sizes. No game pointers may survive serialization.
#pragma once
#include "lsw_gpu_frame.h"
#include <type_traits>
namespace tcs_capture {
constexpr uint32_t tag(char a, char b, char c, char d) {
  return uint32_t(a) | (uint32_t(b)<<8) | (uint32_t(c)<<16) | (uint32_t(d)<<24);
}
struct Header {
  char magic[8] = {'T','C','S','F','R','M','0','1'};
  uint32_t version = 1, regs = 0x6000;
  uint32_t draw_size = sizeof(LswGpuDrawRec), range_size = sizeof(LswGpuRange);
  uint32_t texture_size = sizeof(LswGpuTexDesc), header_size = sizeof(Header);
};
struct Chunk { uint32_t kind, bytes; };
struct Input { uint32_t record, endian, vs_words, ps_words; };
struct TextureRef { uint32_t record, fetch, texture; };
// Version 2 stores complete per-draw vertex/index snapshots instead of a 512 MB
// initial physical dump. Version 3 additionally requires the GPU sidecar below.
// Version 1 remains readable for archived captures.
struct VertexInput { uint32_t record, ranges; };
struct VertexRange { uint32_t address, bytes; };
// A v3 compact frame requires this separately fenced GPU-source sidecar.
struct GpuHeader {
  char magic[8]={'T','C','S','S','R','C','0','1'};
  uint32_t version=1,header_size=sizeof(GpuHeader);
  uint64_t serial=0;
};
// GIMG: base=guest address, producer=resolve record or UINT32_MAX (prior history).
// GTGT checkpoint: base=color attachment, producer=record just executed.
struct GpuImage { uint32_t id,base,producer,format,width,height,bytes,reserved; };
// image=UINT32_MAX records the actual desktop null SRV; mapping is then zero.
struct GpuBinding { uint32_t record,stage,fetch,image,mapping; };
// Optional PRES in the GPU sidecar. Source is what the desktop actually showed,
// not an inferred match to swap_fb/fetch. Image indexes GIMG, never GTGT.
// 0 = no image, 1 = resolved image, 2 = attachment fallback. Raw fetch preserves
// the guest request independently; crop is the desktop source/destination rect.
struct Presentation {
  // v1: host presentation call; v2: fetch at the frame's consumed PM4 swap.
  uint32_t version=1,source=0,image=UINT32_MAX,mapping=0x688;
  uint32_t fetch[6]{},rect[4]{};
};
static_assert(sizeof(Presentation)==56);
static_assert(sizeof(GpuHeader)==24 && sizeof(GpuImage)==32 && sizeof(GpuBinding)==20);
struct Frame {
  uint64_t serial;
  uint32_t swap_fb, arena_bytes, ranges, records, prelude, reserved;
};
// Optional RREG chunk after FRAM: an array of LswGpuResolveRegisters. It keeps
// destination endian/array/slice/number/exp-bias bits without changing the draw
// ABI. Its absence in archived captures means these fields are unknown.
static_assert(sizeof(Header)==32 && sizeof(Chunk)==8 && sizeof(Input)==16 && sizeof(Frame)==32);
static_assert(sizeof(void*)==8 && std::is_trivially_copyable_v<LswGpuDrawRec>);
}
