// Desktop-only GPU texture inputs. Included after xd_texture_source is defined.
// Copies are recorded exactly where sources are consumed, mapped after the
// frame's fence, and written separately from the CPU/ucode capture. Never runs
// unless the opt-in v3 capture has published this exact guest serial.
#include "lsw_frame_capture_vertices.inc"
struct TcsTextureFingerprint {
  unsigned format,width,height,layers,mips;
  uint32_t bytes[16]{};uint64_t hash[16]{};
};
static std::map<ID3D12Resource*,TcsTextureFingerprint> g_tcs_texture_fingerprints;
static void tcs_texture_upload(ID3D12Resource* resource,const LswGpuTexDesc& d,const uint8_t* upload,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* footprints){
  static const bool enabled=[] {const char* e=getenv("LSWTCS_FRAMECAPTURE_GPU");return e && !strcmp(e,"1");}();
  if(!enabled || !upload || d.mips>16)return;
  TcsTextureFingerprint f{d.dxgi,d.width,d.height,d.layers,d.mips};
  for(unsigned m=0;m<d.mips;++m){const auto n=uint64_t(d.mip_row_bytes[m])*d.mip_rows[m];
    if(d.mip_offset[m]>d.data_size || n>d.data_size-d.mip_offset[m])return;
    f.bytes[m]=uint32_t(n);f.hash[m]=14695981039346656037ull;
    if(d.mip_row_bytes[m]>footprints[m].Footprint.RowPitch)return;
    for(unsigned y=0;y<d.mip_rows[m];++y)for(unsigned x=0;x<d.mip_row_bytes[m];++x)
      f.hash[m]=(f.hash[m]^upload[footprints[m].Offset+uint64_t(y)*footprints[m].Footprint.RowPitch+x])*1099511628211ull;
  }
  g_tcs_texture_fingerprints[resource]=f;
}
struct TcsGpuSourceCapture {
  struct Image {tcs_capture::GpuImage wire;ID3D12Resource* readback;UINT pitch;bool target=false;};
  std::vector<Image> images;
  std::vector<tcs_capture::GpuBinding> bindings;
  struct Fingerprint {unsigned record,stage,fetch;TcsTextureFingerprint texture;};
  std::vector<Fingerprint> fingerprints;
  std::map<std::pair<uintptr_t,uint64_t>,uint32_t> ids;
  std::map<uint32_t,uint32_t> producers;
  std::string root;
  uint64_t serial=0;
  bool active=false,failed=false;
  bool presentationKnown=false;
  tcs_capture::Presentation presentation;
  unsigned targetCount=0;
  bool snapshot(tcs_capture::GpuImage wire,ID3D12Resource* res,D3D12_RESOURCE_STATES& state,bool target=false){
    const UINT row=wire.bytes/wire.height,pitch=(row+255)&~255u;
    if(images.size()>=256){failed=true;return false;}
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=uint64_t(pitch)*wire.height;
    rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb=nullptr;
    if(FAILED(g_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_ID3D12Resource,(void**)&rb))){failed=true;return false;}
    const auto previous=state;
    if(previous!=D3D12_RESOURCE_STATE_COPY_SOURCE){barrier(res,previous,D3D12_RESOURCE_STATE_COPY_SOURCE);state=D3D12_RESOURCE_STATE_COPY_SOURCE;}
    D3D12_TEXTURE_COPY_LOCATION dst{},source{};dst.pResource=rb;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint={DXGI_FORMAT(wire.format),wire.width,wire.height,1,pitch};source.pResource=res;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{0,0,0,wire.width,wire.height,1};g_cmd->CopyTextureRegion(&dst,0,0,0,&source,&box);
    if(previous!=D3D12_RESOURCE_STATE_COPY_SOURCE){barrier(res,D3D12_RESOURCE_STATE_COPY_SOURCE,previous);state=previous;}
    images.push_back({wire,rb,pitch,target});return true;
  }
  void begin(uint64_t value,const uint32_t* fetch,bool ordered=false) {
    const char* path=lswtcs_framecapture_peek(value);
    if(!path)return;
    if(active){printf("[FRAMECAPTURE-GPU] previous capture still pending\n");failed=true;return;}
    root=path;root.resize(root.find_last_of("/\\"));serial=value;
    g_tcs_vertex_capture.begin(root,serial);
    images.clear();bindings.clear();fingerprints.clear();ids.clear();producers.clear();targetCount=0;failed=false;active=true;
    presentation={};presentationKnown=fetch!=nullptr;
    if(ordered)presentation.version=2;
    if(fetch)memcpy(presentation.fetch,fetch,sizeof presentation.fetch);
  }
  void present(uint32_t source,uint32_t base,ID3D12Resource* resource,DXGI_FORMAT format,D3D12_RESOURCE_STATES& state,
               uint32_t producer,uint32_t width,uint32_t height){
    if(!active||failed||!presentationKnown)return;
    const auto desc=resource->GetDesc();const uint32_t bpp=xd_dump_bpp(format);
    if(presentation.source||!bpp||!width||!height||width>desc.Width||height>desc.Height||
       desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||
       uint64_t(desc.Width)*desc.Height*bpp>(64u<<20)){failed=true;return;}
    uint32_t id=0;for(const auto& image:images)if(!image.target)++id;
    if(!snapshot({id,base,producer,unsigned(format),UINT(desc.Width),desc.Height,UINT(desc.Width)*desc.Height*bpp,0},resource,state))return;
    presentation.source=source;presentation.image=id;
    presentation.rect[2]=width;presentation.rect[3]=height;
  }
  void produced(uint32_t base,uint32_t record){if(active)producers[base&0x1fffffffu]=record;}
  void draw(const LswGpuFrame& frame,const LswGpuDrawRec& record) {
    if(!active || failed)return;
    if(record.cb_fetch_off>frame.arena.size() || 768>frame.arena.size()-record.cb_fetch_off){failed=true;return;}
    const auto* fetch=reinterpret_cast<const uint32_t*>(frame.arena.data()+record.cb_fetch_off);
    const uint32_t index=uint32_t(&record-frame.draws.data());
    for(unsigned stage=0;stage<2;++stage){
      const unsigned count=stage?record.ps_tex_count:record.vs_tex_count;
      const uint8_t* dims=stage?record.ps_tex_dim:record.vs_tex_dim;
      const uint8_t* slots=stage?record.ps_tex_fetch:record.vs_tex_fetch;
      for(unsigned t=0;t<count && t<32;++t){
        const uint32_t slot=slots[t]&31;uint32_t base,format,swizzle,width,height,dim;
        if(!lsw_gpu_texture_info(fetch+slot*6,&base,&format,&swizzle,&width,&height,&dim)){
          bindings.push_back({index,stage?4u:0u,slot,UINT32_MAX,0});continue;
        }
        if(dims[t]>1){
          const auto* tx=xd_get_texture(fetch+slot*6);
          if(dims[t]==2 || !tx || (dims[t]==3 && !(tx->dim==2 && tx->layers==6)))
            bindings.push_back({index,stage?4u:0u,slot,UINT32_MAX,0});
          continue;
        }
        XdTexSrc src{};if(!xd_texture_source(base,format,width,height,src)){
          // A missing resolved surface may still use a normal CPU texture.
          // Record null only when the same decoder used by the SRV table fails.
          const auto* tx=xd_get_texture(fetch+slot*6);
          if(!tx)bindings.push_back({index,stage?4u:0u,slot,UINT32_MAX,0});
          else {auto fp=g_tcs_texture_fingerprints.find(tx->res);if(fp!=g_tcs_texture_fingerprints.end())
            fingerprints.push_back({index,stage?4u:0u,slot,fp->second});}
          continue;
        }
        const auto desc=src.res->GetDesc();const UINT w=UINT(desc.Width),h=desc.Height;
        const UINT bpp=xd_dump_bpp(src.fmt);
        if(!bpp || !w || !h || uint64_t(w)*h*bpp>(64u<<20) || desc.MipLevels!=1 || desc.DepthOrArraySize!=1){
          printf("[FRAMECAPTURE-GPU] unsupported source base=%08X format=%u %ux%u\n",base,unsigned(src.fmt),w,h);failed=true;return;}
        const auto key=std::make_pair(reinterpret_cast<uintptr_t>(src.res),g_xd_resolve_seq);
        auto it=ids.find(key);uint32_t id;
        if(it==ids.end()){
          id=uint32_t(ids.size());
          auto p=producers.find(base);const uint32_t producer=p==producers.end()?UINT32_MAX:p->second;
          if(!snapshot({id,base,producer,unsigned(src.fmt),w,h,w*h*bpp,0},src.res,*src.state))return;
          ids.emplace(key,id);
        }else id=it->second;
        const uint32_t mapping=xd_resolved_swizzle(swizzle,format,src.fmt,src.swap);
        bindings.push_back({index,stage?4u:0u,slot,id,mapping});
      }
    }
  }
  void checkpoint(const LswGpuFrame& frame,const LswGpuDrawRec& record,XdRt* color){
    if(!active || failed || !color)return;
    static const auto selected=[] {std::vector<unsigned> result;const char* p=getenv("LSWTCS_FRAMECAPTURE_TARGETS");
      while(p && *p && result.size()<32){char* end=nullptr;auto value=strtoul(p,&end,10);if(end==p)break;result.push_back(unsigned(value));p=*end==','?end+1:end;}
      return result;}();
    const unsigned index=unsigned(&record-frame.draws.data());
    if(std::find(selected.begin(),selected.end(),index)==selected.end())return;
    if(color->fmt!=DXGI_FORMAT_R8G8B8A8_UNORM || color->w!=1280){failed=true;return;}
    snapshot({targetCount++,0,index,unsigned(color->fmt),1280,720,1280*720*4,0},color->res,color->state,true);
  }
  void finish() {
    if(!active)return;
    g_tcs_vertex_capture.finish();
    const std::string partial=root+"/gpu-sources.partial",complete=root+"/gpu-sources.bin";
    FILE* file=failed?nullptr:fopen(partial.c_str(),"wb");bool ok=file!=nullptr;
    auto write=[&](const void* data,size_t n){if(ok && fwrite(data,1,n,file)!=n)ok=false;};
    auto chunk=[&](uint32_t tag,uint32_t n){tcs_capture::Chunk c{tag,n};write(&c,sizeof c);};
    tcs_capture::GpuHeader header;header.serial=serial;write(&header,sizeof header);
    for(auto& image:images){
      uint8_t* data=nullptr;D3D12_RANGE range{0,SIZE_T(image.pitch)*image.wire.height};
      if(FAILED(image.readback->Map(0,&range,(void**)&data)) || !data)ok=false;
      if(ok){chunk(image.target?tcs_capture::tag('G','T','G','T'):tcs_capture::tag('G','I','M','G'),sizeof(image.wire)+image.wire.bytes);write(&image.wire,sizeof image.wire);
        const UINT row=image.wire.bytes/image.wire.height;for(UINT y=0;y<image.wire.height;++y)write(data+size_t(y)*image.pitch,row);}
      if(data){D3D12_RANGE written{};image.readback->Unmap(0,&written);}image.readback->Release();
    }
    for(const auto& binding:bindings){chunk(tcs_capture::tag('G','B','N','D'),sizeof binding);write(&binding,sizeof binding);}
    if(presentationKnown){chunk(tcs_capture::tag('P','R','E','S'),sizeof presentation);write(&presentation,sizeof presentation);}
    chunk(tcs_capture::tag('D','O','N','E'),0);
    if(file){ok=(fflush(file)==0)&&ok;ok=(fclose(file)==0)&&ok;}
    ok=ok && rename(partial.c_str(),complete.c_str())==0;
    // Fingerprint the original upload, including each mip, rather than assuming
    // that a fresh CPU decode matches a resource cached earlier in the run.
    FILE* fp=fopen((root+"/gpu-texture-fingerprints.json").c_str(),"wb");bool fpok=fp!=nullptr;
    if(fp){fprintf(fp,"{\"serial\":%llu,\"scope\":\"first-layer bytes used by original D3D12 texture upload\",\"textures\":[",(unsigned long long)serial);
      for(size_t i=0;i<fingerprints.size();++i){const auto& v=fingerprints[i];const auto& t=v.texture;
        fprintf(fp,"%s{\"record\":%u,\"stage\":%u,\"fetch\":%u,\"dxgi\":%u,\"width\":%u,\"height\":%u,\"layers\":%u,\"mips\":[",i?",":"",v.record,v.stage,v.fetch,t.format,t.width,t.height,t.layers);
        for(unsigned m=0;m<t.mips;++m)fprintf(fp,"%s{\"bytes\":%u,\"fnv1a64\":\"%016llx\"}",m?",":"",t.bytes[m],(unsigned long long)t.hash[m]);
        fprintf(fp,"]}");}
      fprintf(fp,"]}\n");fpok=!ferror(fp);fpok=(fclose(fp)==0)&&fpok;}
    printf("[FRAMECAPTURE-GPU] serial=%llu images=%zu bindings=%zu complete=%u\n",(unsigned long long)serial,images.size(),bindings.size(),ok);fflush(stdout);
    if(!fpok){printf("[FRAMECAPTURE-GPU] texture fingerprint write failed\n");fflush(stdout);}
    images.clear();bindings.clear();fingerprints.clear();ids.clear();producers.clear();active=false;
  }
};
static TcsGpuSourceCapture g_tcs_gpu_capture;
static bool tcs_gpu_capture_pending(){return g_tcs_gpu_capture.active;}
static void tcs_gpu_capture_write(){g_tcs_gpu_capture.finish();}
