// One interpretation of recorded Xenos resolves for live and replay adapters.
// This is the observed 32-bit destination domain, not an all-Xenos implementation.
// Storage pitch/height remain distinct from the written rectangle and later
// texture-view dimensions. Tiled byte ranges belong to resource ownership.
#pragma once
#include "lsw_gpu_frame.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace lsw_deko {
struct ResolveRect { uint32_t x=0,y=0,width=0,height=0; };
struct ResolvePlan {
  bool copy=false, clearColor=false, clearDepth=false, depth=false, swap=false;
  uint32_t colorSlot=0,colorBase=0,depthBase=0,colorFormat=0,depthFormat=0;
  uint32_t sourcePitch=0,scaleX=1,scaleY=1,sample=0,sampleCount=1;
  uint32_t destination=0,storagePitch=0,storageHeight=0,destinationFormat=0;
  // Minimum backing extents, NOT the logical size of a sampled texture view.
  uint32_t requiredWidth=0,requiredHeight=0;
  ResolveRect source, destinationRect;
  bool average=false;
  uint32_t colorClear=0,stencilClear=0;
  float depthClear=0;
};

inline bool normalize_resolve(const LswGpuDrawRec& r,ResolvePlan& out,std::string& error){
  out={};error.clear();
  auto fail=[&](const char* text){error=text;return false;};
  if(r.kind!=1)return fail("resolve record kind invalid");
  int64_t width=int64_t(r.rs_rect[2])-r.rs_rect[0],height=int64_t(r.rs_rect[3])-r.rs_rect[1];
  // An empty clipped copy has no destination or clear work.
  if(width<=0||height<=0)return true;
  if(r.rs_rect[0]<0||r.rs_rect[1]<0)return fail("negative recorded resolve rectangle");
  if(r.rs_copy_cmd>3||r.rs_copy_cmd==2)return fail("resolve copy command unsupported");
  if(r.rs_src>4)return fail("resolve source attachment invalid");
  unsigned msaa=(r.surf_info>>16)&3;
  if(msaa>2)return fail("resolve sample organization unsupported");
  out.sourcePitch=r.surf_info&0x3fff;
  if(!out.sourcePitch||uint64_t(r.rs_rect[2])>out.sourcePitch)return fail("resolve exceeds source surface pitch");
  out.scaleX=msaa==2?2:1;out.scaleY=msaa?2:1;out.sampleCount=1u<<msaa;
  out.source={uint32_t(r.rs_rect[0]),uint32_t(r.rs_rect[1]),uint32_t(width),uint32_t(height)};
  out.depth=r.rs_src==4;out.colorSlot=out.depth?0:r.rs_src;
  out.colorBase=r.color_info&2047u;out.colorFormat=(r.color_info>>16)&15;
  out.depthBase=r.depth_info&2047u;out.depthFormat=(r.depth_info>>16)&1;
  out.copy=r.rs_copy_cmd<=1;
  out.clearColor=r.rs_clear_color!=0;out.clearDepth=r.rs_clear_depth!=0;
  if(out.copy){
    if(!r.rs_dest||r.rs_dest>=0x20000000u)return fail("resolve destination outside recorded physical domain");
    if(r.rs_dest_fmt!=6)return fail("resolve destination format unsupported");
    if(!r.rs_dest_pitch||r.rs_dest_x<0||r.rs_dest_y<0)return fail("resolve destination geometry invalid");
    uint64_t right=uint64_t(r.rs_dest_x)+uint64_t(width),bottom=uint64_t(r.rs_dest_y)+uint64_t(height);
    if(right>UINT32_MAX||bottom>UINT32_MAX)return fail("resolve destination extent overflow");
    if(right>r.rs_dest_pitch)return fail("resolve rectangle exceeds destination row pitch");
    out.destination=r.rs_dest;out.destinationFormat=r.rs_dest_fmt;
    out.storagePitch=r.rs_dest_pitch;out.storageHeight=r.rs_dest_height;
    out.destinationRect={uint32_t(r.rs_dest_x),uint32_t(r.rs_dest_y),uint32_t(width),uint32_t(height)};
    out.requiredWidth=uint32_t(right);out.requiredHeight=uint32_t(bottom);
    out.sample=r.rs_sample;
    out.average=!out.depth&&((msaa==1&&r.rs_sample==4)||(msaa==2&&r.rs_sample==6));
    // Other valid Xenos sample groups need an executor implementation before
    // expanding this observed domain. Do not silently select a different sample.
    if(!out.average&&r.rs_sample>=out.sampleCount)return fail("resolve sample selection unsupported");
    out.swap=!out.depth&&r.rs_dest_swap!=0;
  }
  if(out.clearColor&&out.colorFormat!=0)return fail("resolve color clear format unsupported");
  out.colorClear=r.rs_color_clear;
  unsigned bits=(r.rs_depth_clear>>8)&0xffffff;
  out.depthClear=float(bits)/16777215.0f;
  if(out.depthFormat){unsigned exponent=bits>>20,mantissa=bits&0xfffff;
    out.depthClear=exponent?std::ldexp(1.0f+float(mantissa)/1048576.0f,int(exponent)-15)*0.5f:
      float(mantissa)/1048576.0f/16384.0f*0.5f;}
  out.stencilClear=r.rs_depth_clear&255;
  return true;
}
} // namespace lsw_deko
