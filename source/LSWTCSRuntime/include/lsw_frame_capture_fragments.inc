// Selected logo draw only: float PS output and interpolants/sample, without
// blending or depth tests. Original targets/pipeline are rebound immediately.
struct TcsFragmentCapture {
  struct Pass {std::string root;uint64_t serial;unsigned record,mode,count;
    ID3D12DescriptorHeap* heap=nullptr;ID3D12PipelineState* pipeline=nullptr;
    std::vector<ID3D12Resource*> images,readbacks;};
  std::vector<Pass> passes;
  static ID3D12Resource* readback(){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=1280*720*16;d.Height=1;
    d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource=nullptr;g_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_ID3D12Resource,(void**)&resource);return resource;
  }
  static void release(Pass& p){for(auto*& v:p.images)safe_rel(v);for(auto*& v:p.readbacks)safe_rel(v);safe_rel(p.heap);safe_rel(p.pipeline);}
  void draw(const std::string& root,uint64_t serial,const LswGpuFrame& frame,const LswGpuDrawRec& r,ID3D12PipelineState* original){
    const char* flag=getenv("LSWTCS_FRAMECAPTURE_FRAGMENTS");if(!flag || strcmp(flag,"1"))return;
    auto found=g_tcs_vertex_psos.find(original);if(found==g_tcs_vertex_psos.end())return;
    if(r.gs_dxbc || !r.ps_dxbc || r.topology!=4 || r.ps_tex_count!=2 || found->second.NumRenderTargets!=1)return;
    const unsigned record=unsigned(&r-frame.draws.data());
    static const char* source=R"HLSL(
cbuffer Fetch:register(b3){uint4 f[48];};
Texture2DArray<float4> image:register(t1);SamplerState filter:register(s0);
struct Input{float4 uv:TEXCOORD0;float4 fog:TEXCOORD1;float4 color:TEXCOORD2;float4 p:SV_Position;};
struct Output{float4 uv:SV_Target0;float4 fog:SV_Target1;float4 color:SV_Target2;float4 raw:SV_Target3;};
Output main(Input i){Output o;o.uv=i.uv;o.fog=i.fog;o.color=i.color;
float2 coord=i.uv.xy+(1.5/1024.0)/float2((f[0].z&8191)+1,((f[0].z>>13)&8191)+1);
float bias=float(int(f[1].x<<10)>>22)*(1.0/32.0);float scale=exp2(bias);
#ifdef CAPTURE_LEVELS
o.uv=image.SampleLevel(filter,float3(coord,0),0);o.fog=image.SampleLevel(filter,float3(coord,0),1);
o.color=float4(ddx_coarse(coord)*scale,0,0);o.raw=float4(ddy_coarse(coord)*scale,0,0);
#else
o.raw=image.SampleGrad(filter,float3(coord,0),ddx_coarse(coord)*scale,ddy_coarse(coord)*scale);
#endif
return o;}
)HLSL";
    ID3DBlob* debug=nullptr;ID3DBlob* error=nullptr;
    HRESULT hr=D3DCompile(source,strlen(source),"capture-interpolants",nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_IEEE_STRICTNESS,0,&debug,&error);
    if(FAILED(hr)){printf("[FRAMECAPTURE-PS] compile failed %s\n",error?(char*)error->GetBufferPointer():"");safe_rel(error);return;}safe_rel(error);
    ID3DBlob* levels=nullptr;const D3D_SHADER_MACRO defines[]={{"CAPTURE_LEVELS","1"},{nullptr,nullptr}};
    hr=D3DCompile(source,strlen(source),"capture-levels",defines,nullptr,"main","ps_5_0",D3DCOMPILE_IEEE_STRICTNESS,0,&levels,&error);
    if(FAILED(hr)){printf("[FRAMECAPTURE-PS] levels compile failed %s\n",error?(char*)error->GetBufferPointer():"");safe_rel(error);debug->Release();return;}safe_rel(error);
    auto* color=xd_rt_color(r.surf_info,r.color_info);auto* depth=xd_rt_depth(r.surf_info,r.depth_info);
    for(unsigned mode=0;mode<3;++mode){Pass p{root,serial,record,mode,mode?4u:1u};auto pd=found->second;
      ID3DBlob* selected=mode==2?levels:debug;
      pd.PS=mode?D3D12_SHADER_BYTECODE{selected->GetBufferPointer(),selected->GetBufferSize()}:pd.PS;
      pd.NumRenderTargets=p.count;pd.DSVFormat=DXGI_FORMAT_UNKNOWN;pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;
      pd.BlendState={};pd.BlendState.IndependentBlendEnable=TRUE;
      for(unsigned i=0;i<8;++i){pd.RTVFormats[i]=i<p.count?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_UNKNOWN;pd.BlendState.RenderTarget[i].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;}
      hr=g_device->CreateGraphicsPipelineState(&pd,IID_ID3D12PipelineState,(void**)&p.pipeline);
      if(FAILED(hr)){printf("[FRAMECAPTURE-PS] pipeline failed=%08X\n",unsigned(hr));continue;}
      D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=p.count;
      hr=g_device->CreateDescriptorHeap(&hd,IID_ID3D12DescriptorHeap,(void**)&p.heap);if(FAILED(hr)){release(p);continue;}
      D3D12_CPU_DESCRIPTOR_HANDLE handles[4];bool ok=true;
      for(unsigned i=0;i<p.count;++i){D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=1280;d.Height=720;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* image=nullptr;hr=g_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_ID3D12Resource,(void**)&image);
        auto* rb=readback();p.images.push_back(image);p.readbacks.push_back(rb);if(FAILED(hr) || !rb){ok=false;break;}
        handles[i]=p.heap->GetCPUDescriptorHandleForHeapStart();handles[i].ptr+=i*g_xd_rtv_inc;g_device->CreateRenderTargetView(image,nullptr,handles[i]);
      }
      if(!ok){release(p);continue;}
      const float clear[4]={12345,12345,12345,12345};for(unsigned i=0;i<p.count;++i)g_cmd->ClearRenderTargetView(handles[i],clear,0,nullptr);
      g_cmd->OMSetRenderTargets(p.count,handles,FALSE,nullptr);g_cmd->SetPipelineState(p.pipeline);
      if(r.ib_count)g_cmd->DrawIndexedInstanced(r.ib_count,1,0,0,0);else g_cmd->DrawInstanced(r.vertex_count,1,0,0);
      for(unsigned i=0;i<p.count;++i){barrier(p.images[i],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=p.readbacks[i];dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,1280,720,1,1280*16};src.pResource=p.images[i];src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g_cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
      passes.push_back(std::move(p));
    }
    auto ch=xd_rtv(color->view),dh=xd_dsv(depth->view);g_cmd->OMSetRenderTargets(1,&ch,FALSE,&dh);g_cmd->SetPipelineState(original);debug->Release();levels->Release();
  }
  void finish(){
    for(auto& p:passes){bool ok=true;
      for(unsigned i=0;i<p.count;++i){void* cpu=nullptr;D3D12_RANGE range{0,1280*720*16};
        if(FAILED(p.readbacks[i]->Map(0,&range,&cpu))){ok=false;continue;}
        std::string name=p.root+"/fragment-"+std::to_string(p.record)+"-"+std::to_string(p.mode)+"-"+std::to_string(i)+".bin";
        FILE* fp=fopen(name.c_str(),"wb");ok=(fp && fwrite(cpu,1,range.End,fp)==range.End)&&ok;if(fp)ok=(fclose(fp)==0)&&ok;
        D3D12_RANGE empty{};p.readbacks[i]->Unmap(0,&empty);}
      if(ok){const std::string name=p.root+"/fragment-"+std::to_string(p.record)+"-"+std::to_string(p.mode)+".json";
        if(FILE* fp=fopen(name.c_str(),"wb")){fprintf(fp,"{\"serial\":%llu,\"record\":%u,\"mode\":%u,\"targets\":%u}\n",(unsigned long long)p.serial,p.record,p.mode,p.count);fclose(fp);}}
      printf("[FRAMECAPTURE-PS] record=%u mode=%u targets=%u complete=%u\n",p.record,p.mode,p.count,ok);release(p);
    }
    passes.clear();
  }
};
static TcsFragmentCapture g_tcs_fragment_capture;
