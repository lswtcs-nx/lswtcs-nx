// CPU-side translation of the existing recorder, shared by live and replay tests.
// Unsupported render-target semantics fail explicitly before GPU submission.
#pragma once
#include "lsw_gpu_frame.h"
#include "lsw_deko_frame_data.h"
#include "lsw_deko_resolve_plan.h"
#include "lsw_deko_edram.h"
#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace lsw_deko {
inline bool draw_plan(const LswGpuDrawRec& source,FrameRecord& out,std::string& error,ResolvePlan* transfer=nullptr){
  out={};out.vs=out.ps=out.gs=~0u;out.resolveSurface=~0u;error.clear();
  auto fail=[&](const char* message){error=message;return false;};
  if(source.kind){
    ResolvePlan operation;
    if(!normalize_resolve(source,operation,error))return false;
    if(transfer){*transfer=operation;out.kind=4;return true;}
    // Compatibility adapter for the existing frozen 220-byte capture ABI.
    // It cannot encode partial/sample/clear operations. Their interpretation is
    // still normalize_resolve above; do not implement another semantic path.
    if(!operation.copy||operation.clearColor||operation.clearDepth||operation.sample||operation.average||
       operation.scaleX!=1||operation.scaleY!=1||(!operation.depth&&operation.colorSlot!=0)||
       operation.source.x||operation.source.y||operation.source.width!=1280||operation.source.height!=720||
       operation.destinationRect.x||operation.destinationRect.y||operation.storagePitch!=1280||operation.storageHeight!=720)
      return fail("normalized resolve not representable in legacy frozen capture ABI");
    out.kind=operation.depth?2:1;out.scaleX=out.scaleY=1;
    out.resolveRect[2]=operation.source.width;out.resolveRect[3]=operation.source.height;out.destinationSwap=operation.swap;
    return true;
  }
  if(!source.surf_info&&!source.color_info&&!source.depth_info&&!source.color_mask&&!source.ps_rt_mask&&
     !source.scissor[2]&&!source.scissor[3]&&!source.color_info_mrt[0]&&!source.color_info_mrt[1]&&
     !source.color_info_mrt[2]&&!source.color_info_mrt[3]){out.kind=3;return true;}
  EdramDraw attachments;if(!edram_draw(source,attachments,error))return false;
  unsigned msaa=(source.surf_info>>16)&3;
  if(source.topology!=1&&source.topology!=2&&source.topology!=3&&source.topology!=4&&source.topology!=5&&source.topology!=10)
    return fail("primitive topology unsupported");
  out.topology=source.topology;out.indexCount=source.ib_count;out.index32=source.ib_32bit;out.indexOffset=source.ib_off;
  out.vertexCount=source.vertex_count;out.rangeFirst=source.first_range;out.rangeCount=source.range_count;
  out.scaleX=msaa==2?2:1;out.scaleY=msaa?2:1;out.colorMask=source.color_mask;out.depthControl=source.depthcontrol;
  out.stencilRef=source.stencil_ref;out.cull=source.sc_mode_cntl;out.rtMask=source.ps_rt_mask;out.edramMode=source.edram_mode;
  out.blend[0]=source.blendcontrol;
  for(unsigned i=1;i<4;++i)out.blend[i]=source.blend_mrt[i];
  std::copy_n(source.viewport,6,out.viewport);std::copy_n(source.scissor,4,out.scissor);
  out.scissor[0]=std::min(out.scissor[0],source.surf_info&0x3fff);
  out.scissor[2]=std::min(out.scissor[2],(source.surf_info&0x3fff)-out.scissor[0]);
  // Shader IDs are resolved by the offline-cache owner; preserve PS presence.
  out.vs=0;out.ps=source.ps_size?0:~0u;out.gs=source.gs_size?0:~0u;
  return true;
}

class FrameConstants {
  const LswGpuFrame& source;
  std::map<std::tuple<uint32_t,uint32_t,uint32_t>,uint32_t> blocks;
 public:
  std::vector<uint8_t> data;
  explicit FrameConstants(const LswGpuFrame& frame):source(frame){}
  bool append(uint32_t offset,uint32_t bytes,uint32_t padded,uint32_t& destination,std::string& error){
    if(offset>source.arena.size()||bytes>source.arena.size()-offset||bytes>padded||padded>8192){error="constant block outside recorded arena";return false;}
    auto key=std::make_tuple(offset,bytes,padded);
    auto found=blocks.find(key);if(found!=blocks.end()){destination=found->second;return true;}
    destination=uint32_t((data.size()+255)&~size_t(255));
    if(destination>32*1024*1024-padded){error="constant upload exceeds bounded frame budget";return false;}
    data.resize(destination+padded,0);memcpy(data.data()+destination,source.arena.data()+offset,bytes);
    blocks.emplace(key,destination);return true;
  }
  bool draw(const LswGpuDrawRec& r,FrameRecord& out,std::string& error){
    if(r.cb_sys_size!=608){error="unexpected bridge system-constant layout";return false;}
    const uint32_t offsets[]={r.cb_sys_off,r.cb_vsf_off,r.cb_psf_off,r.cb_bool_off,r.cb_fetch_off,r.cb_sys_off+512};
    const uint32_t sizes[]={512,r.cb_vsf_size,r.cb_psf_size,160,768,96};
    for(unsigned i=0;i<6;++i){unsigned padded=std::max(16u,sizes[i]);if((i==1||i==2)&&padded>=4096)padded=8192;
      // kernel_stubs put_float stores 512 entries for an extended block, even
      // though cb_*f_size describes its original constants. Preserve its tail
      // (CBPAD modes 1/3 may contain real PS or wrapped VS values).
      unsigned stored=(i==1||i==2)&&sizes[i]>=4096?padded:sizes[i];
      if(!append(offsets[i],stored,padded,out.cbOffset[i],error))return false;
      out.cbSize[i]=padded;}
    return true;
  }
};
} // namespace lsw_deko
