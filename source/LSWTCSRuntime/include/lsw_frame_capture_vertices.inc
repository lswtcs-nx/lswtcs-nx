// Desktop-only VS stream output for selected records. The diagnostic repeats
// the VS with rasterization disabled, then restores the original pipeline.
#include <d3d12shader.h>
static bool tcs_vertex_capture_enabled(){
  static const char* value=getenv("LSWTCS_FRAMECAPTURE_VERTICES");return value && value[0];
}
static std::map<ID3D12PipelineState*,D3D12_GRAPHICS_PIPELINE_STATE_DESC> g_tcs_vertex_psos;
static void tcs_vertex_pso(ID3D12PipelineState* pso,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc){
  if(pso && tcs_vertex_capture_enabled())g_tcs_vertex_psos[pso]=desc;
}
#include "lsw_frame_capture_fragments.inc"
struct TcsVertexCapture {
  struct Field {std::string semantic;unsigned index,first,count;};
  struct Output {unsigned record,stride,capacity;ID3D12Resource* data;ID3D12Resource* counter;
    ID3D12Resource* zero;ID3D12Resource* readback;ID3D12PipelineState* pipeline;std::vector<Field> fields;};
  std::vector<Output> outputs;
  std::string root;uint64_t serial=0;bool active=false;
  void begin(const std::string& path,uint64_t frame){root=path;serial=frame;active=tcs_vertex_capture_enabled();}
  static bool selected(unsigned record){
    const char* p=getenv("LSWTCS_FRAMECAPTURE_VERTICES");
    while(p && *p){char* end=nullptr;auto value=strtoul(p,&end,10);if(end==p)break;
      if(value==record)return true;p=*end==','?end+1:end;}
    return false;
  }
  static ID3D12Resource* buffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource=nullptr;
    g_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_ID3D12Resource,(void**)&resource);
    return resource;
  }
  static void release(Output& o){safe_rel(o.data);safe_rel(o.counter);safe_rel(o.zero);safe_rel(o.readback);safe_rel(o.pipeline);}
  void draw(const LswGpuFrame& frame,const LswGpuDrawRec& record,ID3D12PipelineState* original){
    const unsigned index=unsigned(&record-frame.draws.data());
    if(!active || !selected(index))return;
    g_tcs_fragment_capture.draw(root,serial,frame,record,original);
    if(record.gs_dxbc || record.topology!=4){printf("[FRAMECAPTURE-VS] record=%u unsupported GS/topology\n",index);return;}
    auto found=g_tcs_vertex_psos.find(original);if(found==g_tcs_vertex_psos.end())return;
    ID3D12ShaderReflection* reflection=nullptr;
    auto pd=found->second;
    HRESULT hr=D3DReflect(pd.VS.pShaderBytecode,pd.VS.BytecodeLength,IID_ID3D12ShaderReflection,(void**)&reflection);
    if(FAILED(hr) || !reflection){printf("[FRAMECAPTURE-VS] reflection failed=%08X\n",unsigned(hr));return;}
    D3D12_SHADER_DESC desc{};reflection->GetDesc(&desc);Output out{};out.record=index;
    for(unsigned i=0;i<desc.OutputParameters;++i){D3D12_SIGNATURE_PARAMETER_DESC s{};
      if(FAILED(reflection->GetOutputParameterDesc(i,&s)))continue;
      unsigned bit=0;while(bit<4){while(bit<4 && !(s.Mask&(1u<<bit)))++bit;unsigned first=bit;
        while(bit<4 && (s.Mask&(1u<<bit)))++bit;
        if(bit>first){out.fields.push_back({s.SemanticName,s.SemanticIndex,first,bit-first});out.stride+=4*(bit-first);}}
    }
    reflection->Release();
    unsigned vertices=record.ib_count?record.ib_count:record.vertex_count;
    if(!out.stride || vertices>100000 || uint64_t(vertices)*out.stride>(64u<<20))return;
    out.capacity=vertices*out.stride;
    std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
    for(auto& f:out.fields)entries.push_back({0,f.semantic.c_str(),f.index,BYTE(f.first),BYTE(f.count),0});
    pd.StreamOutput={entries.data(),UINT(entries.size()),&out.stride,1,D3D12_SO_NO_RASTERIZED_STREAM};
    pd.PS={};pd.NumRenderTargets=0;pd.DSVFormat=DXGI_FORMAT_UNKNOWN;
    for(auto& format:pd.RTVFormats)format=DXGI_FORMAT_UNKNOWN;
    pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;
    hr=g_device->CreateGraphicsPipelineState(&pd,IID_ID3D12PipelineState,(void**)&out.pipeline);
    if(FAILED(hr)){printf("[FRAMECAPTURE-VS] pipeline failed=%08X\n",unsigned(hr));return;}
    out.data=buffer(out.capacity,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_STREAM_OUT);
    out.counter=buffer(8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    out.zero=buffer(8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    out.readback=buffer(out.capacity+8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    if(!out.data || !out.counter || !out.zero || !out.readback){release(out);return;}
    void* cpu=nullptr;D3D12_RANGE empty{};hr=out.zero->Map(0,&empty,&cpu);
    if(FAILED(hr)){release(out);return;}memset(cpu,0,8);out.zero->Unmap(0,nullptr);
    g_cmd->CopyBufferRegion(out.counter,0,out.zero,0,8);
    barrier(out.counter,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW view{out.data->GetGPUVirtualAddress(),out.capacity,out.counter->GetGPUVirtualAddress()};
    g_cmd->SetPipelineState(out.pipeline);g_cmd->SOSetTargets(0,1,&view);
    if(record.ib_count)g_cmd->DrawIndexedInstanced(record.ib_count,1,0,0,0);
    else g_cmd->DrawInstanced(record.vertex_count,1,0,0);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW unbound{};g_cmd->SOSetTargets(0,1,&unbound);g_cmd->SetPipelineState(original);
    barrier(out.data,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(out.counter,D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_cmd->CopyBufferRegion(out.readback,0,out.data,0,out.capacity);
    g_cmd->CopyBufferRegion(out.readback,out.capacity,out.counter,0,8);
    // Preserve the actual bytecode alongside the float outputs for disassembly.
    for(unsigned stage=0;stage<2;++stage){const auto& code=stage?found->second.PS:found->second.VS;
      if(!code.BytecodeLength)continue;
      const std::string stem=root+"/vertex-"+std::to_string(index)+(stage?".ps":".vs");
      if(FILE* fp=fopen((stem+".dxbc").c_str(),"wb")){fwrite(code.pShaderBytecode,1,code.BytecodeLength,fp);fclose(fp);}
      ID3DBlob* text=nullptr;if(SUCCEEDED(D3DDisassemble(code.pShaderBytecode,code.BytecodeLength,0,nullptr,&text))){
        if(FILE* fp=fopen((stem+".txt").c_str(),"wb")){fwrite(text->GetBufferPointer(),1,text->GetBufferSize(),fp);fclose(fp);}text->Release();}}
    outputs.push_back(std::move(out));
  }
  void finish(){
    if(!active)return;
    g_tcs_fragment_capture.finish();
    for(auto& o:outputs){uint8_t* cpu=nullptr;D3D12_RANGE range{0,SIZE_T(o.capacity)+8};
      if(SUCCEEDED(o.readback->Map(0,&range,(void**)&cpu))){uint64_t filled=0;memcpy(&filled,cpu+o.capacity,8);
        bool ok=filled && filled<=o.capacity && filled%o.stride==0;
        const std::string stem=root+"/vertex-"+std::to_string(o.record);
        if(ok){FILE* fp=fopen((stem+".bin").c_str(),"wb");ok=fp && fwrite(cpu,1,filled,fp)==filled;if(fp)ok=(fclose(fp)==0)&&ok;}
        if(ok){FILE* fp=fopen((stem+".json").c_str(),"wb");ok=fp!=nullptr;
          if(fp){fprintf(fp,"{\"serial\":%llu,\"record\":%u,\"stride\":%u,\"vertices\":%llu,\"fields\":[",(unsigned long long)serial,o.record,o.stride,(unsigned long long)(filled/o.stride));
            for(size_t i=0;i<o.fields.size();++i){const auto& f=o.fields[i];fprintf(fp,"%s{\"semantic\":\"%s\",\"index\":%u,\"first\":%u,\"count\":%u}",i?",":"",f.semantic.c_str(),f.index,f.first,f.count);}
            fprintf(fp,"]}\n");ok=!ferror(fp);ok=(fclose(fp)==0)&&ok;}}
        D3D12_RANGE written{};o.readback->Unmap(0,&written);
        printf("[FRAMECAPTURE-VS] record=%u vertices=%llu stride=%u complete=%u\n",o.record,(unsigned long long)(filled/o.stride),o.stride,ok);
      }
      release(o);
    }
    outputs.clear();active=false;fflush(stdout);
  }
};
static TcsVertexCapture g_tcs_vertex_capture;
