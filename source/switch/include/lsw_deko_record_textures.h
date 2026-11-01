// Called on the recording guest thread while its register/texture inputs are
// current. The render thread consumes owned arena bytes and never decodes from
// guest memory. Decoder scratch is copied before the next decode.
#pragma once
#include "lsw_gpu_frame.h"
#include "lsw_gl_shader.h"
#include "lsw_deko_shader_cache.h"
#include "lsw_deko_resolve_plan.h"
#include "lsw_deko_resources.h"

inline bool lsw_deko_texture_view_matches(const LswGpuTextureLayout& layout,const LswDekoProducer& producer){
  return lsw_deko_resolve_view_matches(layout,producer.memory,producer.width,producer.height);
}
inline bool lsw_deko_publish_resolve(LswDekoResourceAccess& access,LswGpuFrame& frame,uint32_t record){
  if(record>=frame.draws.size())return false;
  lsw_deko::ResolvePlan plan;std::string error;
  if(!lsw_deko::normalize_resolve(frame.draws[record],plan,error))return false;
  if(!plan.copy)return true;
  LswDekoProducerRef producer;
  return lsw_deko_capture_resolve(frame,record,producer)&&access.publish(std::move(producer));
}

inline bool lsw_deko_same_texture(const LswGpuTexDesc& a,const LswGpuTexDesc& b){
  return a.ok==b.ok&&a.dxgi==b.dxgi&&a.width==b.width&&a.height==b.height&&a.layers==b.layers&&
    a.dim==b.dim&&a.block==b.block&&a.bpb==b.bpb&&a.row_bytes==b.row_bytes&&a.rows==b.rows&&
    a.mips==b.mips&&a.layer_bytes==b.layer_bytes&&a.data_size==b.data_size&&a.component_mapping==b.component_mapping&&
    !memcmp(a.mip_row_bytes,b.mip_row_bytes,sizeof a.mip_row_bytes)&&
    !memcmp(a.mip_rows,b.mip_rows,sizeof a.mip_rows)&&!memcmp(a.mip_offset,b.mip_offset,sizeof a.mip_offset);
}
// Caller holds the resource guard until decoded scratch has been copied. Both
// shader textures and presentation use this exact source/visibility decision.
inline bool lsw_deko_texture_input(const uint32_t* words,const uint8_t* physical,LswDekoResourceAccess* access,
                                   LswGpuTexDesc& decoded,LswDekoProducerRef& retained){
  decoded={};retained.reset();
  if(access){
    LswGpuTextureLayout layout{};lsw_deko::TextureAuthority authority;
    if(!lsw_gpu_plan_texture(words,&layout)||layout.image.data_size>64*1024*1024||
       !access->texture_source(layout,authority,retained))return false;
    if(retained&&lsw_deko_texture_view_matches(layout,*retained)){decoded=layout.image;return true;}
    retained.reset();
    if(authority.kind!=lsw_deko::TextureAuthority::Cpu&&!access->texture_visible(layout))return false;
  }
  return lsw_gpu_decode_texture(words,physical,&decoded)&&decoded.ok&&decoded.data&&
      decoded.data_size&&decoded.data_size<=64*1024*1024;
}

inline bool lsw_deko_capture_front(LswGpuFrame& frame,const uint32_t* words,const uint8_t* physical,LswDekoResourceAccess* access){
  std::unique_ptr<LswDekoResourceAccess::Guard> guard;
  if(access)guard=std::make_unique<LswDekoResourceAccess::Guard>(*access);
  LswGpuFrontbuffer next;LswGpuTextureLayout layout{};
  if(!words||!lsw_gpu_plan_texture(words,&layout)||layout.image.dxgi!=28||layout.image.dim||
     layout.image.layers!=1||layout.image.mips!=1||layout.image.data_size>64*1024*1024)return false;
  // Until observation is activated, retain only an exact full COPY from this
  // frame. Never let an unrelated cached surface or clear-only record win.
  if(!access){
    for(uint32_t i=0;i<frame.draws.size();++i){const auto& r=frame.draws[i];
      if(r.kind!=1||r.rs_dest!=layout.levels[0].base)continue;
      lsw_deko::ResolvePlan plan;std::string error;if(!lsw_deko::normalize_resolve(r,plan,error))return false;
      if(!plan.copy)continue;
      next.producer_record=i;
    }
    if(next.producer_record!=~0u){
      const auto& r=frame.draws[next.producer_record];lsw_deko::ResolvePlan plan;std::string error;
      if(!lsw_deko::normalize_resolve(r,plan,error))return false;
      const LswGpuResolveRegisters* raw=nullptr;
      for(const auto& candidate:frame.resolve_registers)if(candidate.record==next.producer_record){if(raw)return false;raw=&candidate;}
      lsw_deko::ResolveMemory memory;
      if(!lsw_deko::resolve_memory(r,raw,memory,error)||
         !lsw_deko_resolve_view_matches(layout,memory,std::max(plan.storagePitch,plan.requiredWidth),std::max(plan.storageHeight,plan.requiredHeight))||
         plan.destinationRect.x||plan.destinationRect.y||plan.destinationRect.width!=layout.image.width||
         plan.destinationRect.height!=layout.image.height||
         !lsw_deko_blit_mapping(lsw_deko_image_mapping(layout.image.component_mapping,plan.swap)))return false;
      next.image=layout.image;frame.front=std::move(next);return true;
    }
  }
  if(!lsw_deko_texture_input(words,physical,access,next.image,next.producer))return false;
  if(next.image.dxgi!=28||next.image.dim||next.image.layers!=1||next.image.mips!=1)return false;
  if(next.producer&&!lsw_deko_blit_mapping(lsw_deko_image_mapping(next.image.component_mapping,next.producer->memory.resolve.swap))){
    if(!access||!access->texture_visible(layout)||!lsw_gpu_decode_texture(words,physical,&next.image)||!next.image.ok)return false;
    next.producer.reset();
  }
  if(!next.producer){
    if(!next.image.data||uint64_t(next.image.width)*next.image.height*4!=next.image.data_size)return false;
    unsigned mapping[4];for(unsigned c=0;c<4;++c){mapping[c]=(next.image.component_mapping>>(3*c))&7;if(mapping[c]>5)return false;}
    next.pixels.resize(next.image.data_size);
    for(size_t p=0;p<next.pixels.size();p+=4)for(unsigned c=0;c<4;++c)
      next.pixels[p+c]=mapping[c]<4?next.image.data[p+mapping[c]]:mapping[c]==5?255:0;
    // The 2D engine ignores texture-descriptor swizzles. CPU pixels are already
    // in presentation order; GPU sources use an explicit RGBA/BGRA format view.
    next.image.component_mapping=0x1688;
  }
  next.image.data=nullptr;frame.front=std::move(next);return true;
}
inline void lsw_deko_record_textures(LswGpuFrame& frame,const LswGpuDraw& draw,const uint8_t* physical,
                                   LswDekoResourceAccess* access=nullptr){
  // The caller's native capture guard spans shader preparation and this whole
  // snapshot. Direct users of this function also serialize its source decision.
  std::unique_ptr<LswDekoResourceAccess::Guard> guard;
  if(access)guard=std::make_unique<LswDekoResourceAccess::Guard>(*access);
  const uint32_t record=uint32_t(frame.draws.size()-1);
  uint32_t seen=0;
  for(const void* pointer:{draw.vs_dxbc,draw.ps_dxbc}){
    if(!pointer)continue;
    const auto* shader=static_cast<const LswGlShader*>(pointer);
    for(const auto& unit:shader->units){
      const unsigned fetch=unit.fetch_constant;
      if(fetch>=32){frame.texture_snapshot_error=true;return;}
      if(seen&(1u<<fetch))continue;
      seen|=1u<<fetch;
      const uint32_t* words=draw.fetch+fetch*6;
      uint32_t base=0,format=0,swizzle=0,width=0,height=0,dimension=0;
      bool valid=lsw_gpu_texture_info(words,&base,&format,&swizzle,&width,&height,&dimension)!=0;
      LswGpuTexDesc decoded{};
      if(valid&&access){
        LswDekoProducerRef retained;
        if(!lsw_deko_texture_input(words,physical,access,decoded,retained)){
          frame.texture_snapshot_error=true;return;
        }
        if(retained){
          uint32_t index=0;
          for(;index<frame.texture_producers.size();++index)if(frame.texture_producers[index]==retained)break;
          if(index==frame.texture_producers.size())frame.texture_producers.push_back(std::move(retained));
          frame.texture_refs.push_back({record,fetch,~0u,~0u,index});continue;
        }
      }
      uint32_t producer=~0u;
      // Migration-only path while observation is unbound. Once activated, the
      // range authority above is the sole decision, even for a CPU overwrite.
      if(valid&&!access)for(uint32_t i=0;i<record;++i)if(frame.draws[i].kind==1&&
          (frame.draws[i].rs_dest&0x1fffffffu)==(base&0x1fffffffu)){
        lsw_deko::ResolvePlan plan;std::string error;
        if(!lsw_deko::normalize_resolve(frame.draws[i],plan,error)){frame.texture_snapshot_error=true;return;}
        if(plan.copy)producer=i;
      }
      if(producer!=~0u){frame.texture_refs.push_back({record,fetch,~0u,producer});continue;}
      if(!valid||(!access&&!lsw_gpu_decode_texture(words,physical,&decoded))||!decoded.ok){
        // Invalid/mismatched fetches can legitimately be null. A valid decode
        // failure must be explained by a GPU source or fail in the renderer.
        frame.texture_refs.push_back({record,fetch,~1u});continue;}
      if(!decoded.data||!decoded.data_size||decoded.data_size>64*1024*1024){frame.texture_snapshot_error=true;return;}
      const uint64_t hash=lsw_deko::cache_hash(decoded.data,decoded.data_size);
      LswGpuTexDesc identity=decoded;identity.data=nullptr;
      uint32_t index=0;
      for(;index<frame.texture_snapshots.size();++index){const auto& cached=frame.texture_snapshots[index];
        if(cached.hash==hash&&lsw_deko_same_texture(cached.image,identity)&&
           !memcmp(frame.arena.data()+cached.offset,decoded.data,decoded.data_size))break;}
      if(index==frame.texture_snapshots.size()){
        if(frame.arena.size()+decoded.data_size+256>64*1024*1024||index>=1024){frame.texture_snapshot_error=true;return;}
        uint32_t offset=frame.alloc_uninit(decoded.data_size,32);
        memcpy(frame.arena.data()+offset,decoded.data,decoded.data_size);
        frame.texture_snapshots.push_back({identity,offset,hash});
      }
      frame.texture_refs.push_back({record,fetch,index});
    }
  }
}
