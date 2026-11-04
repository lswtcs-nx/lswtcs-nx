// Included by the existing texture decoder TU, after component_mapping().
// Reuse that TU's host formats and Xenia's guest-layout/address functions.
#include "lsw_gpu_texture_layout.h"
#include <cstdlib>

extern "C" int lsw_gpu_plan_texture(const uint32_t* fetch6, LswGpuTextureLayout* out) {
  *out = {};
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch6, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) return 0;
  HostFormat hf;
  if (!host_format(fetch.format, hf)) return 0;
  auto dim = fetch.dimension;
  if (dim == xenos::DataDimension::k3D || dim == xenos::DataDimension::k1D) return 0;
  uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page,
                                                &mip_min, &mip_max);
  uint32_t width=w1+1, height=h1+1, layers=d1+1;
  if (!base_page || width>8192 || height>8192 || layers>64) return 0;
  const auto* fi=FormatInfo::Get(uint32_t(fetch.format));
  uint32_t bw=fi->block_width, bh=fi->block_height, bpb=fi->bytes_per_block();
  if (bpb!=hf.bpb || bw!=hf.block || bh!=hf.block) return 0;
  static const bool nomips=[] {
    const char* e=getenv("LSWTCS_GPUDRAW_NOMIPS"); return e && e[0]=='1';
  }();
  uint32_t max_level=(mip_page && !nomips)?std::min<uint32_t>(mip_max,15):0;
  auto guest=texture_util::GetGuestTextureLayout(dim,fetch.pitch,width,height,layers,
      fetch.tiled,fetch.format,fetch.packed_mips,true,max_level);
  auto& image=out->image;
  image.dxgi=hf.dxgi; image.width=width; image.height=height; image.layers=layers;
  image.dim=(dim==xenos::DataDimension::kCube)?2:0;
  image.block=hf.block; image.bpb=bpb; image.mips=max_level+1;
  image.component_mapping=component_mapping(fetch,hf)|(1u<<12);
  uint64_t layer_bytes=0;
  out->tiled=fetch.tiled; out->block_bytes_log2=xe::log2_floor(bpb);
  constexpr uint32_t endian_xor[]={0,1,3,2};
  out->endian_xor=endian_xor[uint32_t(fetch.endianness)];
  for (uint32_t m=0;m<image.mips;++m) {
    uint32_t wm=std::max(width>>m,1u), hm=std::max(height>>m,1u);
    image.mip_row_bytes[m]=((wm+bw-1)/bw)*bpb;
    image.mip_rows[m]=(hm+bh-1)/bh;
    image.mip_offset[m]=uint32_t(layer_bytes);
    layer_bytes+=uint64_t(image.mip_row_bytes[m])*image.mip_rows[m];
    if (layer_bytes>UINT32_MAX || layer_bytes*layers>UINT32_MAX) return 0;
    const auto& lv=m==0?guest.base:guest.mips[m];
    auto& level=out->levels[m];
    uint64_t base=m==0?uint64_t(base_page)<<12:
        (uint64_t(mip_page)<<12)+guest.mip_offsets_bytes[std::min(m,guest.packed_level)];
    // The fetch contains a physical page number. Fold the initial page once;
    // mip offsets must not wrap into an unrelated physical allocation.
    uint64_t initial=(uint64_t(m==0?base_page:mip_page)<<12)&0x1fffffffull;
    uint64_t relative=base-(uint64_t(m==0?base_page:mip_page)<<12);
    if (initial+relative>=0x20000000ull) return 0;
    level.base=uint32_t(initial+relative);
    if (fetch.packed_mips && m>=guest.packed_level) {
      uint32_t unused_z=0;
      texture_util::GetPackedMipOffset(width,height,1,fetch.format,m,
                                      level.offset_x,level.offset_y,unused_z);
    }
    const auto& storage=(fetch.packed_mips && m>guest.packed_level && guest.packed_level!=0)?
        guest.mips[guest.packed_level]:lv;
    bool packed_base=m>=guest.packed_level && guest.packed_level==0;
    level.row_pitch=packed_base?guest.base.row_pitch_bytes:storage.row_pitch_bytes;
    level.pitch_blocks=level.row_pitch/bpb;
    if (fetch.tiled) level.pitch_blocks=xe::align(level.pitch_blocks,xenos::kTextureTileWidthHeight);
    level.slice_stride=packed_base?guest.base.array_slice_stride_bytes:storage.array_slice_stride_bytes;
    level.width_blocks=(wm+bw-1)/bw; level.height_blocks=(hm+bh-1)/bh;
  }
  image.layer_bytes=uint32_t(layer_bytes); image.data_size=uint32_t(layer_bytes*layers);
  image.row_bytes=image.mip_row_bytes[0]; image.rows=image.mip_rows[0]; image.ok=1;
  return 1;
}

extern "C" int lsw_gpu_visit_texture_blocks(const LswGpuTextureLayout* layout,
    bool (*visit)(void*,const LswGpuTextureBlock*),void* context) {
  const auto& image=layout->image;
  if (!visit || !image.ok || !image.layers || !image.mips || image.mips>16 ||
      !image.bpb || image.bpb>16 || (image.bpb&(image.bpb-1)) ||
      layout->block_bytes_log2!=xe::log2_floor(image.bpb) || layout->endian_xor>3) return 0;
  for (uint32_t m=0;m<image.mips;++m) {
    const auto& level=layout->levels[m];
    for (uint32_t z=0;z<image.layers;++z) for (uint32_t y=0;y<level.height_blocks;++y)
      for (uint32_t x=0;x<level.width_blocks;++x) {
        int64_t relative=layout->tiled?
            texture_address::Tiled2D(int32_t(x+level.offset_x),int32_t(y+level.offset_y),
                                    level.pitch_blocks,layout->block_bytes_log2):
            int64_t(y+level.offset_y)*level.row_pitch+int64_t(x+level.offset_x)*image.bpb;
        if (relative<0) return 0;
        uint64_t physical=uint64_t(level.base)+uint64_t(z)*level.slice_stride+uint64_t(relative);
        if (physical>=0x20000000ull || physical+image.bpb>0x20000000ull) return 0;
        // For R8/RG8, endian swaps can move a whole texel to a neighbouring
        // position in its 32-bit word. Swapping only inside that texel loses it.
        physical^=layout->endian_xor&~(image.bpb-1);
        uint64_t offset=uint64_t(z)*image.layer_bytes+image.mip_offset[m]+
            uint64_t(y)*image.mip_row_bytes[m]+uint64_t(x)*image.bpb;
        if (physical+image.bpb>0x20000000ull || offset+image.bpb>image.data_size) return 0;
        LswGpuTextureBlock block{uint32_t(physical),uint32_t(offset),image.bpb,
                                layout->endian_xor&(image.bpb-1)};
        if (!visit(context,&block)) return 0;
      }
  }
  return 1;
}

extern "C" int64_t lsw_gpu_tiled_offset(uint32_t x,uint32_t y,uint32_t z,
    uint32_t pitch,uint32_t height,uint32_t log2_bytes,bool array) {
  if (x>=0x4000 || y>=0x4000 || !pitch || pitch>0x4000 || (pitch&31) || log2_bytes>4 ||
      (array && (!height || height>0x4000 || (height&31) || z>7))) return -1;
  return array?xe::gpu::texture_address::Tiled3D(x,y,z,pitch,height,log2_bytes):
               xe::gpu::texture_address::Tiled2D(x,y,pitch,log2_bytes);
}

// log78: decode into storage the caller provides (reserve returns at least `bytes`, or null), so several threads can decode at
// once: lsw_gpu_decode_texture's scratch() is one buffer for the process.
extern "C" int lsw_gpu_decode_texture_into(const uint32_t* fetch6,const uint8_t* physical,LswGpuTexDesc* out,
                                           uint8_t* (*reserve)(void* context,uint32_t bytes),void* context) {
  *out={};
  LswGpuTextureLayout layout{};
  if (!reserve || !lsw_gpu_plan_texture(fetch6,&layout)) return 0;
  uint8_t* destination=reserve(context,layout.image.data_size);
  if (!destination) return 0;
  struct Decode {const uint8_t* source; uint8_t* destination;} decode{physical,destination};
  if (!physical || !lsw_gpu_visit_texture_blocks(&layout,[](void* c,const LswGpuTextureBlock* block) {
    auto& decode=*static_cast<Decode*>(c);
    auto* o=decode.destination+block->decoded_offset;
    auto* in=decode.source+block->physical;
    if (!block->byte_xor) std::memcpy(o,in,block->bytes);
    else for (uint32_t i=0;i<block->bytes;++i) o[i]=in[i^block->byte_xor];
    return true;
  },&decode)) return 0;
  *out=layout.image;out->data=destination;
  return 1;
}

extern "C" int lsw_gpu_decode_texture(const uint32_t* fetch6,const uint8_t* physical,
                                      LswGpuTexDesc* out) {
  *out={};
  LswGpuTextureLayout layout{};
  if (!lsw_gpu_plan_texture(fetch6,&layout)) {
    static unsigned reports=0;
    if (reports++<8) dbg_ram("[TEXLAYOUT] unsupported or invalid fetch %08X/%08X/%08X/%08X/%08X/%08X\n",
        fetch6[0],fetch6[1],fetch6[2],fetch6[3],fetch6[4],fetch6[5]);
    return 0;
  }
  auto& destination=scratch();
  destination.resize(layout.image.data_size);
  struct Decode {const uint8_t* source; uint8_t* destination;} decode{physical,destination.data()};
  if (!physical || !lsw_gpu_visit_texture_blocks(&layout,[](void* context,const LswGpuTextureBlock* block) {
    auto& decode=*static_cast<Decode*>(context);
    auto* out=decode.destination+block->decoded_offset;
    auto* in=decode.source+block->physical;
    if (!block->byte_xor) std::memcpy(out,in,block->bytes);
    else for (uint32_t i=0;i<block->bytes;++i) out[i]=in[i^block->byte_xor];
    return true;
  },&decode)) {
    static unsigned reports=0;
    if (reports++<8) dbg_ram("[TEXLAYOUT] texture block outside guest physical backing\n");
    return 0;
  }
  *out=layout.image;out->data=destination.data();
  return 1;
}
