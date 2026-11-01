// Shared physical attachment ownership. Xenos EDRAM is 2048 wrapping tiles,
// each containing 80x16 32-bit samples. A host image is a view of those tiles,
// not independent storage identified by a render-target base or MRT slot.
#pragma once
#include "lsw_deko_resolve_plan.h"
#include <array>
#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

namespace lsw_deko {
constexpr unsigned EdramTiles=2048,EdramTileWidth=80,EdramTileHeight=16;
struct EdramView {
  unsigned base=0,pitchTiles=0,samples=0,format=0;bool depth=false;
  auto identity()const{return std::make_tuple(base,pitchTiles,samples,format,depth);}
  bool operator<(const EdramView& other)const{return identity()<other.identity();}
  bool operator==(const EdramView& other)const{return identity()==other.identity();}
  unsigned width()const{return pitchTiles*EdramTileWidth;}
  // Narrow pitches can describe more rows than a native 2D image supports.
  // Keep that boundary explicit; the remaining tiles are reachable via other
  // base/pitch views, rather than allocating an oversized native image.
  unsigned height()const{return std::min(16384u,((EdramTiles+pitchTiles-1)/pitchTiles)*EdramTileHeight);}
};
inline bool edram_view(unsigned surface,unsigned info,bool depth,EdramView& out,std::string& error){
  out={};unsigned pitch=surface&0x3fff,samples=(surface>>16)&3;
  if(!pitch||pitch>8192||samples>2){error="EDRAM pitch/sample organization unsupported";return false;}
  out={info&2047u,((pitch<<(samples==2))+79)/80,samples,(info>>16)&(depth?1u:15u),depth};
  if(out.width()>8192||(!depth&&out.format!=0)){
    error="EDRAM attachment format/extent unsupported";return false;
  }
  return true;
}
struct EdramUse {
  EdramView view;ResolveRect samples;
  // A clear may discard old contents only for tiles it overwrites completely.
  // Draws and resolve reads always preserve the entire acquired tile.
  bool clear=false;
};
inline bool edram_use(unsigned surface,unsigned info,bool depth,ResolveRect pixels,bool clear,
                      EdramUse& out,std::string& error){
  out={};if(!edram_view(surface,info,depth,out.view,error))return false;
  unsigned sx=out.view.samples==2?2:1,sy=out.view.samples?2:1;
  if(uint64_t(pixels.x)+pixels.width>(surface&0x3fff)||
     uint64_t(pixels.y)+pixels.height>UINT32_MAX/sy){error="EDRAM rectangle exceeds surface geometry";return false;}
  out.samples={pixels.x*sx,pixels.y*sy,pixels.width*sx,pixels.height*sy};out.clear=clear;
  return true;
}
struct EdramTransfer {EdramView from,to;ResolveRect source,destination;};
class EdramOwner {
  struct Tile {bool valid=false;EdramView view;};
  std::array<Tile,EdramTiles> tiles{};
 public:
  struct Change {unsigned tile;EdramView view;};
  struct Plan {std::vector<EdramTransfer> transfers;std::vector<Change> changes;};
  bool retains(const EdramView& view)const{
    return std::any_of(tiles.begin(),tiles.end(),[&](const Tile& tile){return tile.valid&&tile.view==view;});
  }
  void reset(){tiles={};}
  bool prepare(const std::vector<EdramUse>& uses,Plan& out,std::string& error)const{
    out={};error.clear();Plan pending;std::array<int,EdramTiles> acquired;acquired.fill(-1);
    auto fail=[&](const char* message){error=message;return false;};
    for(unsigned index=0;index<uses.size();++index){const auto& use=uses[index];const auto& view=use.view;const auto& r=use.samples;
      if(!view.pitchTiles||view.pitchTiles>102||view.base>=EdramTiles||view.samples>2||(!view.depth&&view.format)||view.format>1)
        return fail("invalid normalized EDRAM view");
      if(!r.width||!r.height)continue;
      if(uint64_t(r.x)+r.width>view.width()||uint64_t(r.y)+r.height>view.height())
        return fail("EDRAM rectangle exceeds backing capacity");
      unsigned firstX=r.x/80,lastX=(r.x+r.width-1)/80,firstY=r.y/16,lastY=(r.y+r.height-1)/16;
      for(unsigned y=firstY;y<=lastY;++y)for(unsigned x=firstX;x<=lastX;++x){
        unsigned local=y*view.pitchTiles+x;if(local>=EdramTiles)return fail("EDRAM view aliases itself beyond one physical backing");
        unsigned physical=(view.base+local)&2047u;
        if(acquired[physical]>=0){
          if(!(uses[unsigned(acquired[physical])].view==view))return fail("simultaneous EDRAM attachments overlap");
          // Repeated same-view requests are safe but must not hide a later
          // preserving use behind an earlier discard request.
        }else acquired[physical]=int(index);
        const auto& old=tiles[physical];
        bool discard=use.clear&&r.x<=x*80&&uint64_t(r.x)+r.width>=(x+1)*80&&
                     r.y<=y*16&&uint64_t(r.y)+r.height>=(y+1)*16;
        if(old.valid&&!(old.view==view)&&!discard){
          // Native ZF32+S8 isn't the guest's packed 24+8 bits. A raw image copy
          // across aspects/depth formats is incorrect.
          // Keep this unsupported boundary explicit until an exact converter
          // exists; never select stale contents from a separate image.
          // These executor images already store packed sample coordinates.
          // XeEdramOffsetBytes applies MSAA before tile/intra-tile addressing;
          // equal sample-space coordinates therefore preserve bits across
          // 1x/2x/4x views of the same aspect/encoding.
          if(old.view.depth!=view.depth||old.view.format!=view.format)
            return fail("EDRAM reinterpretation requires packed-sample conversion");
          unsigned prior=(physical+EdramTiles-old.view.base)&2047u;
          EdramTransfer transfer{old.view,view,{(prior%old.view.pitchTiles)*80,(prior/old.view.pitchTiles)*16,80,16},{x*80,y*16,80,16}};
          // Merge adjacent tiles only when contiguous in both image rows.
          if(!pending.transfers.empty()){auto& previous=pending.transfers.back();
            if(previous.from==transfer.from&&previous.to==view&&previous.source.y==transfer.source.y&&
               previous.destination.y==transfer.destination.y&&previous.source.x+previous.source.width==transfer.source.x&&
               previous.destination.x+previous.destination.width==transfer.destination.x){
              previous.source.width+=80;previous.destination.width+=80;
            }else pending.transfers.push_back(transfer);
          }else pending.transfers.push_back(transfer);
        }
        pending.changes.push_back({physical,view});
      }
    }
    out=std::move(pending);return true;
  }
  // Call only after every acquisition transfer was recorded successfully.
  void commit(const Plan& plan){for(const auto& change:plan.changes)tiles[change.tile]={true,change.view};}
};
struct EdramDraw {std::array<EdramUse,4> colors{};EdramUse depth{};unsigned colorMask=0;bool depthUsed=false;};
inline bool edram_draw(const LswGpuDrawRec& r,EdramDraw& out,std::string& error,const uint32_t* derivedRect=nullptr){
  out={};error.clear();
  if((r.edram_mode!=4&&r.edram_mode!=5)||r.ps_rt_mask>15){error="EDRAM draw mode/output mask unsupported";return false;}
  // Recorder scissor is x/y/width/height; it bounds all possible raster writes.
  // Clamp it to pitch, rather than treating the display size as backing size.
  unsigned pitch=r.surf_info&0x3fff;
  ResolveRect rect{std::min(r.scissor[0],pitch),r.scissor[1],0,r.scissor[3]};
  rect.width=std::min(r.scissor[2],pitch-rect.x);
  if(derivedRect){
    uint64_t right=std::min(uint64_t(rect.x)+rect.width,uint64_t(derivedRect[0])+derivedRect[2]);
    uint64_t bottom=std::min(uint64_t(rect.y)+rect.height,uint64_t(derivedRect[1])+derivedRect[3]);
    rect.x=std::max(rect.x,derivedRect[0]);rect.y=std::max(rect.y,derivedRect[1]);
    rect.width=uint32_t(right>rect.x?right-rect.x:0);rect.height=uint32_t(bottom>rect.y?bottom-rect.y:0);
  }
  for(unsigned slot=0;slot<4;++slot){
    if(!r.ps_size||r.edram_mode!=4||!(r.ps_rt_mask&(1u<<slot))||!((r.color_mask>>(4*slot))&15))continue;
    if(!edram_use(r.surf_info,r.color_info_mrt[slot],false,rect,false,out.colors[slot],error))return false;
    out.colorMask|=1u<<slot;
  }
  out.depthUsed=(r.depthcontrol&7)!=0;
  if(out.depthUsed&&!edram_use(r.surf_info,r.depth_info,true,rect,false,out.depth,error))return false;
  // Deko's pinned binding entry point dereferences every slot below its count;
  // it has no nullable holes. Reject sparse slots before allocating/claiming,
  // rather than binding a guessed dummy or aliasing one image into two slots.
  if(out.colorMask&(out.colorMask+1)){error="sparse EDRAM color slots require native output mapping";return false;}
  // Validate geometry even for draws with no writable/tested attachment.
  EdramView checked;return edram_view(r.surf_info,r.depth_info,true,checked,error);
}
} // namespace lsw_deko
