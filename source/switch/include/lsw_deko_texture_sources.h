// Recording-time source classification through the SAME byte traversal as the
// decoder. A uniform GPU result still needs view/layout compatibility; mixed
// ranges require composition/materialization, never a base-address shortcut.
#pragma once
#include "lsw_deko_resource_owner.h"
#include "lsw_gpu_texture_layout.h"
#include "lsw_deko_resolve_memory.h"
#include "lsw_deko_image_view.h"

inline unsigned lsw_deko_image_mapping(unsigned mapping,bool swap,bool depth=false){
  unsigned result=0;
  for(unsigned c=0;c<4;++c){unsigned component=(mapping>>(3*c))&7;
    if(swap&&!depth&&(component==0||component==2))component^=2;
    if(component<4&&depth)component=0;else if(component>5)component=4;
    result|=component<<(3*c);
  }
  return result;
}
inline bool lsw_deko_blit_mapping(unsigned mapping){
  return lsw_deko_blit_format_index(mapping)!=0;
}

// Zero-copy compatibility is deliberately explicit. A different pitch, endian,
// format, mip/layer or logical extent must go through exact guest composition
// and the shared decoder, never reinterpret an image by its base address.
inline bool lsw_deko_resolve_view_matches(const LswGpuTextureLayout& layout,const lsw_deko::ResolveMemory& memory,uint32_t width,uint32_t height){
  const auto& image=layout.image;const auto& level=layout.levels[0];
  return image.ok&&image.dxgi==28&&image.dim==0&&image.layers==1&&image.mips==1&&image.block==1&&image.bpb==4&&
    layout.tiled&&layout.block_bytes_log2==2&&layout.endian_xor==memory.endianXor&&
    !memory.array&&!memory.slice&&!memory.resolve.depth&&memory.guestFormat==6&&
    level.base==memory.resolve.destination&&level.pitch_blocks==memory.pitch&&!level.offset_x&&!level.offset_y&&
    image.width==width&&image.height==height;
}

namespace lsw_deko {
struct TextureAuthority {
  enum Kind {Cpu,Gpu,Mixed} kind=Cpu;
  uint64_t key=0, version=0;
};
inline bool texture_authority(const ResourceOwner& owner,const LswGpuTextureLayout& layout,
                              TextureAuthority& out) {
  struct Query {
    const ResourceOwner& owner;
    TextureAuthority result;
    bool seen=false;
    std::vector<GuestSource> spans;
  } query{owner,{},false,{}};
  if (!lsw_gpu_visit_texture_blocks(&layout,[](void* context,const LswGpuTextureBlock* block) {
    auto& query=*static_cast<Query*>(context);
    if (!query.owner.capture({block->physical,block->bytes},query.spans)) return false;
    for (const auto& source:query.spans) {
      TextureAuthority current{source.kind==GuestProducer::Cpu?TextureAuthority::Cpu:TextureAuthority::Gpu,
                               source.key,source.version};
      if (!query.seen) {query.result=current;query.seen=true;}
      else if (query.result.kind!=current.kind || query.result.key!=current.key ||
               query.result.version!=current.version) query.result={TextureAuthority::Mixed,0,0};
    }
    return true;
  },&query) || !query.seen) return false;
  out=query.result;
  return true;
}
} // namespace lsw_deko
