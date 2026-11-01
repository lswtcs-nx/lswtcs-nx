// Included only by the desktop recorder. Disabled unless LSWTCS_FRAMECAPTURE
// names a new output directory. The trigger arms at a swap and records the NEXT
// complete frame; it never changes gameplay, quality, saves or shader selection.
#include "lsw_frame_capture.h"
#include <direct.h>
#include <atomic>
#include <string>
#include <map>
#include <mutex>
namespace {
struct TcsFrameCapture {
  FILE* file = nullptr;
  bool active = false, attempted = false, failed = false, compact = false;
  unsigned captured = 0;
  uint64_t next_at = 0;
  std::string root;
  std::map<uint64_t,std::string> references;
  std::mutex reference_mutex;
  std::atomic<uint64_t> reference_serial{0};
  struct Texture { LswGpuTexDesc desc; std::vector<uint8_t> bytes; uint64_t hash; };
  std::vector<Texture> textures;
  bool bytes(const void* p, size_t n) {
    if (!file) return false;
    if (n && fwrite(p,1,n,file)!=n) {
      fclose(file); file=nullptr; active=false;
      printf("[FRAMECAPTURE] write failed; partial capture retained\n");
      return false;
    }
    return true;
  }
  bool chunk(uint32_t kind, uint64_t n) {
    if (n > UINT32_MAX) { if(file) fclose(file); file=nullptr; active=false; return false; }
    tcs_capture::Chunk c{kind,uint32_t(n)}; return bytes(&c,sizeof c);
  }
  void boundary(const LswGpuFrame& f, const uint8_t* phys) {
    using namespace tcs_capture;
    static const char* path=getenv("LSWTCS_FRAMECAPTURE");
    static uint64_t at=[] { const char* e=getenv("LSWTCS_FRAMECAPTURE_AT"); return e?strtoull(e,nullptr,10):0; }();
    static unsigned count=[] { const char* e=getenv("LSWTCS_FRAMECAPTURE_COUNT"); return e?unsigned(strtoul(e,nullptr,10)):1u; }();
    static unsigned stride=[] { const char* e=getenv("LSWTCS_FRAMECAPTURE_STRIDE"); return e?unsigned(strtoul(e,nullptr,10)):1u; }();
    if(!path || !path[0] || failed) return;
    // The previous captured frame has been published, but must not be replaced
    // before its paired render/readback has started. No lock needed by renderer
    // is held here. Diagnostic-only pacing; never enabled in the shipping game.
    if(reference_serial.load(std::memory_order_acquire)) {
      const ULONGLONG start=GetTickCount64();
      while(reference_serial.load(std::memory_order_acquire) && GetTickCount64()-start<10000) Sleep(1);
      if(reference_serial.load(std::memory_order_acquire)) {failed=true;printf("[FRAMECAPTURE] paired renderer did not consume capture; stopping capture\n");fflush(stdout);return;}
    }
    if (active) {
      Frame fr{f.serial,f.swap_fb,uint32_t(f.arena.size()),uint32_t(f.ranges.size()),
               uint32_t(f.draws.size()),uint32_t(f.prelude.size()),0};
      uint64_t n=sizeof fr+f.arena.size()+(f.ranges.size()+f.prelude.size())*sizeof(LswGpuRange)+f.draws.size()*sizeof(LswGpuDrawRec);
      if (chunk(tag('F','R','A','M'),n)) {
        bytes(&fr,sizeof fr); bytes(f.arena.data(),f.arena.size());
        bytes(f.ranges.data(),f.ranges.size()*sizeof(LswGpuRange));
        for (auto r:f.draws) {
          r.vs_dxbc=r.ps_dxbc=r.gs_dxbc=nullptr;
          bytes(&r,sizeof r);
        }
        bytes(f.prelude.data(),f.prelude.size()*sizeof(LswGpuRange));
        if(!f.resolve_registers.empty()) {
          chunk(tag('R','R','E','G'),f.resolve_registers.size()*sizeof(LswGpuResolveRegisters));
          bytes(f.resolve_registers.data(),f.resolve_registers.size()*sizeof(LswGpuResolveRegisters));
        }
        if(!f.draw_extents.empty()) {
          chunk(tag('D','E','X','T'),f.draw_extents.size()*sizeof(LswGpuDrawExtent));
          bytes(f.draw_extents.data(),f.draw_extents.size()*sizeof(LswGpuDrawExtent));
        }
        chunk(tag('D','O','N','E'),0);
      }
      bool ok=file && fflush(file)==0;
      if (file) { ok=(fclose(file)==0)&&ok; file=nullptr; }
      active=false;
      if(ok && rename((root+"/capture.partial").c_str(),(root+"/capture.bin").c_str())==0) {
        {std::lock_guard<std::mutex> guard(reference_mutex);references.emplace(f.serial,root+"/reference.png");}
        reference_serial.store(f.serial,std::memory_order_release);
        ++captured;
        printf("[FRAMECAPTURE] complete serial=%llu records=%zu ranges=%zu textures=%zu -> %s\n",
          (unsigned long long)f.serial,f.draws.size(),f.ranges.size(),textures.size(),root.c_str());
      } else {failed=true;printf("[FRAMECAPTURE] incomplete capture; do not use the partial file\n");}
      textures.clear(); textures.shrink_to_fit(); fflush(stdout);
    }
    if(failed || captured>=count || f.serial<next_at) return;
    if(!attempted) {
      if(!(at && f.serial>=at) && GetFileAttributesA("framecapture_now")==INVALID_FILE_ATTRIBUTES) return;
      attempted=true;DeleteFileA("framecapture_now");
      const char* mode=getenv("LSWTCS_FRAMECAPTURE_COMPACT");compact=mode && !strcmp(mode,"1");
      if(!count || count>120 || !stride || stride>600 || (count>1 && !compact)) {failed=true;printf("[FRAMECAPTURE] invalid sequence settings; compact required for multiple frames\n");return;}
      if(count>1 && _mkdir(path)!=0) {failed=true;printf("[FRAMECAPTURE] output directory must be new: %s\n",path);return;}
    }
    root=path;
    if(count>1){char suffix[32];snprintf(suffix,sizeof suffix,"/frame-%04u",captured);root+=suffix;}
    if (_mkdir(root.c_str())!=0) {failed=true; printf("[FRAMECAPTURE] frame directory must be new: %s\n",root.c_str()); return; }
    file=fopen((root+"/capture.partial").c_str(),"wb");
    if(!file){failed=true;return;}
    Header h;
    if(compact){h.version=2;h.magic[7]='2';const char* gpu=getenv("LSWTCS_FRAMECAPTURE_GPU");if(gpu && !strcmp(gpu,"1")){h.version=3;h.magic[7]='3';}}
    if(bytes(&h,sizeof h) && (compact || (chunk(tag('P','H','Y','S'),0x20000000) && bytes(phys,0x20000000)))) {
      next_at=f.serial+stride;
      active=true; printf("[FRAMECAPTURE] armed after serial=%llu; recording next complete frame\n",(unsigned long long)f.serial); fflush(stdout);
    }
  }
  void draw(uint32_t index, uint32_t endian, const uint32_t* regs,
            const void* vs,uint32_t nv,const void* ps,uint32_t np,
            const LswGpuDraw& d,const LswGpuDrawRec& record,const uint8_t* phys) {
    if(!active) return;
    using namespace tcs_capture;
    Input in{index,endian,nv,np};
    if(!chunk(tag('D','R','A','W'),sizeof in+0x6000*4ull+(uint64_t(nv)+np)*4)) return;
    bytes(&in,sizeof in); bytes(regs,0x6000*4); bytes(vs,size_t(nv)*4); bytes(ps,size_t(np)*4);
    if(compact) {
      VertexInput head{index,d.vfetch_count+(record.ib_shm && record.ib_count?1u:0u)};
      uint64_t payload=sizeof head;
      for(unsigned i=0;i<d.vfetch_count;++i)payload+=sizeof(VertexRange)+d.vfetch_size[i];
      const uint32_t index_bytes=record.ib_count*(record.ib_32bit?4u:2u);
      if(record.ib_shm && record.ib_count)payload+=sizeof(VertexRange)+index_bytes;
      if(!chunk(tag('V','D','A','T'),payload))return;
      bytes(&head,sizeof head);
      auto range=[&](uint32_t address,uint32_t n){VertexRange r{address&0x1fffffffu,n};
        if(!n || uint64_t(r.address)+n>0x20000000ull){failed=true;active=false;return false;}
        return bytes(&r,sizeof r)&&bytes(phys+r.address,n);};
      for(unsigned i=0;i<d.vfetch_count;++i)if(!range(d.vfetch_addr[i],d.vfetch_size[i]))return;
      if(record.ib_shm && record.ib_count && !range(record.ib_off,index_bytes))return;
    }
    bool seen[32]={};
    for(uint32_t stage=0;stage<2;++stage) {
      auto* bindings=stage?d.ps_tex:d.vs_tex;
      uint32_t count=stage?d.ps_tex_count:d.vs_tex_count;
      for(uint32_t i=0;i<count && i<32;++i) {
        uint32_t fetch=bindings[i].fetch_constant;
        if(fetch>=32 || seen[fetch]) continue;
        seen[fetch]=true;
        LswGpuTexDesc desc{};
        if(!lsw_gpu_decode_texture(d.fetch+fetch*6,phys,&desc) || !desc.ok) {
          TextureRef ref{index,fetch,UINT32_MAX};
          chunk(tag('T','R','E','F'),sizeof ref); bytes(&ref,sizeof ref); continue;
        }
        const uint8_t* data=desc.data; desc.data=nullptr;
        uint64_t h=lsw_hash_bytes(data,desc.data_size);
        uint32_t id=0;
        for(;id<textures.size();++id) {
          const auto& t=textures[id];
          if(t.hash==h && !memcmp(&t.desc,&desc,sizeof desc) && t.bytes.size()==desc.data_size &&
             !memcmp(t.bytes.data(),data,desc.data_size)) break;
        }
        if(id==textures.size()) {
          textures.push_back({desc,std::vector<uint8_t>(data,data+desc.data_size),h});
          if(!chunk(tag('T','E','X','D'),4+sizeof desc+desc.data_size)) return;
          bytes(&id,4); bytes(&desc,sizeof desc); bytes(data,desc.data_size);
        }
        TextureRef ref{index,fetch,id};
        chunk(tag('T','R','E','F'),sizeof ref); bytes(&ref,sizeof ref);
      }
    }
  }
};
TcsFrameCapture g_tcs_frame_capture;
}
extern "C" const char* lswtcs_framecapture_peek(uint64_t serial) {
  const char* mode=getenv("LSWTCS_FRAMECAPTURE_GPU");
  if(!mode || strcmp(mode,"1") || !serial || g_tcs_frame_capture.reference_serial.load(std::memory_order_acquire)!=serial)return nullptr;
  std::lock_guard<std::mutex> guard(g_tcs_frame_capture.reference_mutex);
  auto it=g_tcs_frame_capture.references.find(serial);
  return it==g_tcs_frame_capture.references.end()?nullptr:it->second.c_str();
}
static void tcs_capture_boundary(const LswGpuFrame& f) {
  g_tcs_frame_capture.boundary(f,g_base+0x80000000ull);
}
extern "C" const char* lswtcs_framecapture_reference(uint64_t serial) {
  uint64_t expected=serial;
  if(serial && g_tcs_frame_capture.reference_serial.compare_exchange_strong(expected,0,std::memory_order_acq_rel)) {
    std::lock_guard<std::mutex> guard(g_tcs_frame_capture.reference_mutex);
    return g_tcs_frame_capture.references.at(serial).c_str();
  }
  return nullptr;
}
