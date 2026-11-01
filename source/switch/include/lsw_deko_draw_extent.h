// Bounds for loose-scissor screen-space rectangles, derived from their actual
// vertex shader and inputs. The interpreter lives in an isolated Xenia target.
#pragma once
#include "lsw_deko_edram.h"
#include <cmath>

using LswDekoReadWord=bool(*)(void*,uint32_t,uint32_t*);
struct LswDekoExtentPosition {float value[4]{};unsigned mask=0;bool killed=false;};
bool lsw_deko_extent_positions(const uint32_t* regs,const uint32_t* codeBE,unsigned words,
                              int first,unsigned count,LswDekoReadWord read,void* context,
                              LswDekoExtentPosition* positions);

inline bool lsw_deko_frame_attachments(const LswGpuFrame& frame,unsigned index,
                                        lsw_deko::EdramDraw& draw,std::string& error){
  if(index>=frame.draws.size()){error="draw extent record outside frame";return false;}
  const uint32_t* extent=nullptr;const auto& r=frame.draws[index];
  for(const auto& item:frame.draw_extents)if(item.record==index){
    if(extent){error="duplicate derived draw extent";return false;}
    extent=item.rect;
    for(unsigned c=0;c<2;++c)if(!extent[c+2]||extent[c]<r.scissor[c]||
        uint64_t(extent[c])+extent[c+2]>uint64_t(r.scissor[c])+r.scissor[c+2]){
      error="derived draw extent outside recorded scissor";return false;
    }
  }
  return lsw_deko::edram_draw(r,draw,error,extent);
}

inline bool lsw_deko_draw_needs_extent(const LswGpuDrawRec& r){
  lsw_deko::EdramView view;std::string error;
  if(r.kind||!lsw_deko::edram_view(r.surf_info,r.depth_info,true,view,error))return false;
  return uint64_t(r.scissor[1])+r.scissor[3]>view.height()/(view.samples?2:1);
}

inline bool lsw_deko_capture_draw_extent(LswGpuFrame& frame,unsigned index,const uint32_t* regs,
                                        const uint32_t* codeBE,unsigned words,LswDekoReadWord read,void* context){
  if(index>=frame.draws.size())return false;const auto& r=frame.draws[index];
  if(!lsw_deko_draw_needs_extent(r))return true;
  // Internal D3D rectangle lists are three non-indexed corners. Do not infer
  // positions for arbitrary shaders, indexed geometry, points or clip planes.
  if(r.topology!=4||r.gs_type!=2||r.ib_count||r.vertex_count!=3||r.gs_size==0||
     r.cb_sys_size<48||r.cb_sys_off>frame.arena.size()||r.cb_sys_size>frame.arena.size()-r.cb_sys_off)return false;
  uint32_t flags,indexEndian;int32_t first;float scale[3],offset[3];
  const auto* sys=frame.arena.data()+r.cb_sys_off;
  memcpy(&flags,sys,4);memcpy(&indexEndian,sys+8,4);memcpy(&first,sys+12,4);memcpy(scale,sys+16,12);memcpy(offset,sys+32,12);
  if(indexEndian||first<0||first>0xffffff-3)return false;
  LswDekoExtentPosition vertices[3];
  if(!lsw_deko_extent_positions(regs,codeBE,words,first,3,read,context,vertices))return false;
  float window[3][2];float w=vertices[0].value[3];
  for(unsigned i=0;i<3;++i){const auto& p=vertices[i];
    if(p.mask!=15||p.killed||p.value[3]!=w||!std::isfinite(w)||w<=0)return false;
    float hostW=(flags&32)?w:1.0f/w;
    for(unsigned c=0;c<2;++c){
      if(!std::isfinite(p.value[c])||!std::isfinite(scale[c])||!std::isfinite(offset[c]))return false;
      // Same ordered operations as the translated VS prologue, then viewport.
      float xy=(flags&8)?p.value[c]*hostW:p.value[c];
      float ndc=(xy*scale[c]+offset[c]*hostW)/hostW;
      // The executor uses Deko's default upper-left origin/Y-up convention:
      // its native viewport has negative Y scale (same as GL clip-control).
      window[i][c]=r.viewport[c]+((c==1?-ndc:ndc)+1.0f)*(r.viewport[c+2]*0.5f);
      if(!std::isfinite(window[i][c]))return false;
    }
  }
  // Prove axis alignment: three distinct corners of a non-degenerate rectangle.
  // Xenia's rectangle GS constructs its missing fourth corner inside this box.
  float lo[2],hi[2];for(unsigned c=0;c<2;++c){lo[c]=hi[c]=window[0][c];for(unsigned i=1;i<3;++i){lo[c]=std::min(lo[c],window[i][c]);hi[c]=std::max(hi[c],window[i][c]);}if(!(hi[c]>lo[c]))return false;}
  unsigned corners=0;for(unsigned i=0;i<3;++i){unsigned corner=0;for(unsigned c=0;c<2;++c){if(window[i][c]==hi[c])corner|=1u<<c;else if(window[i][c]!=lo[c])return false;}if(corners&(1u<<corner))return false;corners|=1u<<corner;}
  // One host pixel of conservative raster/rounding margin. Intersect with the
  // recorded scissor; this bounds ownership without changing GPU draw state.
  uint32_t rect[4];for(unsigned c=0;c<2;++c){
    double start=std::max(double(r.scissor[c]),std::floor(double(lo[c]))-1.0);
    double end=std::min(double(r.scissor[c])+r.scissor[c+2],std::ceil(double(hi[c]))+1.0);
    if(start<0||end>UINT32_MAX||end<=start)return false;
    rect[c]=uint32_t(start);rect[c+2]=uint32_t(end-start);
  }
  frame.draw_extents.push_back({index,{rect[0],rect[1],rect[2],rect[3]}});return true;
}
