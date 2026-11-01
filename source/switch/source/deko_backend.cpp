// Live consumer of the game's existing frame recorder. No captured draw arrays.
#include <switch.h>
#include <deko3d.h>
#include "lsw_deko_present.inc"
#include "lsw_gpu_frame.h"
#include "lsw_gl_shader.h"
#include "lsw_deko_frame_plan.h"
#include "lsw_deko_draw_extent.h"
#include "lsw_deko_record_textures.h"
#include "lsw_deko_submission.h"
#include "lsw_deko_resolve_memory.h"
#include "lsw_deko_executor.h"
#include "lsw_deko_shader_cache_id.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

extern "C" void lswtcs_gil_blocking(void (*)(void*),void*);
extern "C" void lswtcs_gil_resource_wait(void (*)(void*),void*);
// Each resolve owns a distinct image. Frame execution refs retain older results
// even when a later resolve targets the same guest base. Physical retirement is
// separately gated by actual queue completion.
struct LswDekoResolvedImage {
  DkImage image{};DkMemBlock storage{};
  bool depth=false,swap=false,layoutKnown=false;
  unsigned id=0,width=0,height=0;uint64_t lastUse=0,bytes=0;
  uint64_t key=0;
  lsw_deko::ResolveMemory memory;
};
struct LswDekoFrameExecution {
  uint32_t executed=0;
  bool presented=false;
  std::vector<std::shared_ptr<LswDekoResolvedImage>> resolves;
};
namespace {
constexpr unsigned Width=1280,Height=720,Frames=2;
constexpr unsigned SharedBytes=512u<<20,CodeBytes=2u<<20,CommandBytes=8u<<20;
constexpr unsigned UploadBytes=128u<<20,ImageDescriptorCount=8192,SamplerDescriptorCount=4096;
// Deko texture handles reserve 20 bits for an image and 12 for a sampler.
// Image cache policy must never become a sampler-pool maximum-index command.
static_assert(dkMakeImageHandle(ImageDescriptorCount-1)==ImageDescriptorCount-1);
static_assert((dkMakeSamplerHandle(SamplerDescriptorCount-1)>>20)==SamplerDescriptorCount-1);
constexpr unsigned SamplerOffset=ImageDescriptorCount*sizeof(DkImageDescriptor);
constexpr const char* App="sdmc:/switch/lswtcs-deko-draw/";
static DkDevice device;
static DkQueue queue;
static DkCmdBuf cmds;
static DkSwapchain swapchain;
static unsigned errors,debugMessages,codeOffset,imageCount,samplerCount;
static FILE* report;
static std::atomic<bool> failed{false},stopping{false},finished{false};
static std::mutex handoffMutex;
static std::condition_variable handoffCv;
static uint64_t posted=0,completed=0,presentations=0;
static lsw_deko::SubmissionTimeline submissions;
static uint64_t activeSubmission=0;
static bool job=false;
struct MaterializeJob {
  LswGpuFrame* frame=nullptr;uint32_t end=0,record=~0u;
  uint8_t* physical=nullptr;size_t physicalBytes=0;lsw_deko::GuestRange wanted{};
  std::vector<lsw_deko::GuestRange> ranges;
  LswDekoProducerRef producer;
  LswDekoProducerRef* captured=nullptr;
};
static MaterializeJob borrowed;
static DkMemBlock codeMem,sharedMem,descriptors,readbackMem,timingMem;
static bool gpuTimings=false;
static bool writebackCheck=false;
static DkImage sharedImage,display[Frames],null2d,nullCube;
static std::vector<DkMemBlock> allocations;
static lsw_deko::ShaderCache shaderCache;
static uint64_t textureBytes=0;
struct Slot {DkCmdBuf commands{};DkMemBlock command{},upload{};DkFence fence{};bool pending=false,timed=false;unsigned used=0;uint64_t serial=0,submission=0;};
static Slot slots[Frames];
static Slot* activeSlot;
struct NativeShader {DkShader shader;const lsw_deko::CacheEntry* entry;};
static std::map<const LswGlShader*,NativeShader> nativeShaders;
using Surface=LswDekoResolvedImage;
static std::map<lsw_deko::ResolveViewKey,std::shared_ptr<Surface>> surfaces;
static std::vector<std::shared_ptr<Surface>> resolvedImages;
static uint64_t resolvedBytes=0;
static uint64_t producerSequence=0;
struct EdramImage {DkImage image{};DkMemBlock storage{};uint64_t bytes=0,lastUse=0;};
static std::map<lsw_deko::EdramView,EdramImage> edramImages;
static lsw_deko::EdramOwner edramOwner;
static uint64_t edramBytes=0;
struct Texture {DkImage image;DkMemBlock storage;LswGpuTexDesc identity;uint64_t hash,lastUse;unsigned id;std::vector<uint8_t> bytes;};
static std::vector<Texture> textures;
static unsigned nextImageId=1;
static std::vector<unsigned> freeImageDescriptors;
static std::map<std::tuple<unsigned,unsigned,bool>,unsigned> imageViews;
static std::map<std::vector<uint8_t>,unsigned> samplers;
static unsigned null2dId,nullCubeId;

static void log(const char* fmt,...){
  if(!report)return;
  va_list args;va_start(args,fmt);vfprintf(report,fmt,args);va_end(args);fflush(report);
}
static bool fail(const char* why){
  log("FAIL frame=%llu %s\n",(unsigned long long)presentations,why);
  {std::lock_guard<std::mutex> lock(handoffMutex);failed=true;}
  handoffCv.notify_all();return false;
}
static void rendererFinished(){
  {std::lock_guard<std::mutex> lock(handoffMutex);finished=true;}
  handoffCv.notify_all();
}
static void debug(void*,const char* context,DkResult result,const char* message){
  ++debugMessages;if(result!=DkResult_Success){++errors;failed=true;}
  log("DEKO result=%u context=%s message=%s\n",unsigned(result),context?context:"",message?message:"");
}
static DkMemBlock memory(unsigned bytes,unsigned flags){
  DkMemBlockMaker m;dkMemBlockMakerDefaults(&m,device,(std::max(bytes,4096u)+4095)&~4095u);m.flags=flags;
  auto block=dkMemBlockCreate(&m);if(!block)fail("GPU memory allocation failed");else allocations.push_back(block);return block;
}
#include "lsw_deko_draw.inc"
static bool image(DkImage& out,DkImageType type,DkImageFormat format,unsigned w,unsigned h,unsigned layers,unsigned mips,unsigned flags,DkMemBlock* storage=nullptr){
  DkImageLayoutMaker maker;dkImageLayoutMakerDefaults(&maker,device);maker.type=type;maker.format=format;
  maker.dimensions[0]=w;maker.dimensions[1]=h;maker.dimensions[2]=layers;maker.mipLevels=mips;maker.flags=flags;
  imageLayoutFix(maker,format==DkImageFormat_RGBA_BC1||format==DkImageFormat_RGBA_BC2||format==DkImageFormat_RGBA_BC3?4:1);
  DkImageLayout layout;dkImageLayoutInitialize(&layout,&maker);if(errors)return false;
  auto block=memory(dkImageLayoutGetSize(&layout),DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image);
  if(!block)return false;if(storage)*storage=block;dkImageInitialize(&out,&layout,block,0);return !errors;
}
static void barrier(){dkCmdBufBarrier(cmds,DkBarrier_Full,DkInvalidateFlags_Image|DkInvalidateFlags_L2Cache);}
static unsigned upload(const void* source,unsigned bytes,unsigned align=256){
  unsigned offset=(activeSlot->used+align-1)&~(align-1);
  if(offset>UploadBytes||bytes>UploadBytes-offset){fail("frame exceeds upload budget");return ~0u;}
  if(bytes)memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(activeSlot->upload))+offset,source,bytes);
  activeSlot->used=offset+bytes;return offset;
}
static DkGpuAddr address(unsigned offset){return dkMemBlockGetGpuAddr(activeSlot->upload)+offset;}
static unsigned descriptor(const DkImage& source,unsigned id,unsigned mapping,bool depth=false){
  auto key=std::make_tuple(id,mapping,depth);auto found=imageViews.find(key);if(found!=imageViews.end())return found->second;
  if(freeImageDescriptors.empty()&&imageCount>=ImageDescriptorCount){fail("image descriptor budget exhausted");return 0;}
  unsigned index;
  if(freeImageDescriptors.empty())index=imageCount++;else{index=freeImageDescriptors.back();freeImageDescriptors.pop_back();}
  DkImageView view;dkImageViewDefaults(&view,&source);swizzle(view,mapping,depth);
  dkImageDescriptorInitialize(static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(descriptors))+index,&view,false,false);
  imageViews.emplace(key,index);return index;
}
static unsigned sampler(const uint32_t* words,const LswGpuSamplerBinding& binding){
  LswGpuSamplerDesc source{};lsw_gpu_sampler(words,&binding,&source);
  std::vector<uint8_t> key(sizeof source);memcpy(key.data(),&source,sizeof source);
  auto found=samplers.find(key);if(found!=samplers.end())return found->second;
  if(samplerCount>=SamplerDescriptorCount){fail("sampler descriptor budget exhausted");return 0;}
  constexpr DkWrapMode wraps[]={DkWrapMode_Repeat,DkWrapMode_Repeat,DkWrapMode_MirroredRepeat,DkWrapMode_ClampToEdge,DkWrapMode_ClampToBorder,DkWrapMode_MirrorClampToEdge};
  if(source.address_u>=std::size(wraps)||source.address_v>=std::size(wraps)||source.address_w>=std::size(wraps)){fail("unsupported sampler address mode");return 0;}
  DkSampler s;dkSamplerDefaults(&s);s.magFilter=source.mag_linear?DkFilter_Linear:DkFilter_Nearest;
  s.minFilter=source.min_linear?DkFilter_Linear:DkFilter_Nearest;s.mipFilter=source.mip_linear?DkMipFilter_Linear:DkMipFilter_Nearest;
  s.maxAnisotropy=source.max_aniso;s.wrapMode[0]=wraps[source.address_u];s.wrapMode[1]=wraps[source.address_v];s.wrapMode[2]=wraps[source.address_w];
  for(unsigned i=0;i<4;++i)s.borderColor[i].value_f=source.border[i];
  unsigned index=samplerCount++;
  auto* dest=reinterpret_cast<DkSamplerDescriptor*>(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(descriptors))+SamplerOffset);
  dkSamplerDescriptorInitialize(dest+index,&s);samplers.emplace(std::move(key),index);return index;
}
static NativeShader* shader(const void* pointer){
  if(!pointer){fail("missing vertex/required shader");return nullptr;}
  const auto* source=static_cast<const LswGlShader*>(pointer);
  auto found=nativeShaders.find(source);if(found!=nativeShaders.end())return &found->second;
  auto* entry=shaderCache.find(source->key,unsigned(source->stage),source->glsl.data(),source->glsl.size());
  if(!entry){log("SHADER_MISS stage=%u key=%016llX source_hash=%016llX\n",unsigned(source->stage),(unsigned long long)source->key,(unsigned long long)lsw_deko::cache_hash(source->glsl.data(),source->glsl.size()));
    char path[192];snprintf(path,sizeof path,"%smissing-%u-%016llx.glsl",App,unsigned(source->stage),(unsigned long long)source->key);
    if(FILE* f=fopen(path,"wb")){fwrite(source->glsl.data(),1,source->glsl.size(),f);fclose(f);}fail("offline shader missing or source differs");return nullptr;}
  unsigned padded=(unsigned(entry->bytes)+DK_SHADER_CODE_ALIGNMENT-1)&~(DK_SHADER_CODE_ALIGNMENT-1);
  if(padded>CodeBytes-DK_SHADER_CODE_UNUSABLE_SIZE-codeOffset){fail("shader code budget exhausted");return nullptr;}
  memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(codeMem))+codeOffset,shaderCache.code(*entry),size_t(entry->bytes));
  NativeShader native{};native.entry=entry;DkShaderMaker maker;dkShaderMakerDefaults(&maker,codeMem,codeOffset);
  dkShaderInitialize(&native.shader,&maker);codeOffset+=padded;
  if(!dkShaderIsValid(&native.shader)){fail("DKSH rejected");return nullptr;}
  log("SHADER_LOAD stage=%u key=%016llX bytes=%llu\n",unsigned(source->stage),(unsigned long long)source->key,(unsigned long long)entry->bytes);
  return &nativeShaders.emplace(source,native).first->second;
}
static std::shared_ptr<Surface> surface(unsigned base,bool depth,unsigned width,unsigned height,const lsw_deko::ResolveMemory* memoryLayout=nullptr){
  if(!width||!height||width>8192||height>8192){fail("resolved view extent unsupported");return {};}
  auto key=lsw_deko::resolve_view_key(base,depth,width,height,memoryLayout);auto found=surfaces.find(key);
  if(found==surfaces.end()&&surfaces.size()>=64){fail("resolved view history budget exhausted");return {};}
  if(producerSequence==UINT64_MAX){fail("resolve producer identity exhausted");return {};}
  auto s=std::make_shared<Surface>();s->key=++producerSequence;s->depth=depth;s->id=nextImageId++;s->width=width;s->height=height;s->lastUse=activeSubmission;
  if(!image(s->image,DkImageType_2DArray,depth?DkImageFormat_ZF32_X24S8:DkImageFormat_RGBA8_Unorm,width,height,1,1,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine,&s->storage))return {};
  s->bytes=dkMemBlockGetSize(s->storage);
  if(s->bytes>(256u<<20)-std::min<uint64_t>(resolvedBytes,256u<<20)){fail("retained resolve image budget exhausted");return {};}
  DkImageView view;dkImageViewDefaults(&view,&s->image);
  if(found!=surfaces.end()){
    // Preserve untouched GPU pixels without mutating the older image version.
    auto& previous=*found->second;previous.lastUse=activeSubmission;
    DkImageView src;dkImageViewDefaults(&src,&previous.image);DkImageRect rect={0,0,0,width,height,1};
    barrier();dkCmdBufCopyImage(cmds,&src,&rect,&view,&rect,0);
  }else{
    DkScissor full={0,0,width,height};dkCmdBufSetScissors(cmds,0,&full,1);
    if(depth){dkCmdBufBindRenderTargets(cmds,nullptr,0,&view);dkCmdBufClearDepthStencil(cmds,true,0,255,0);}
    else{const DkImageView* cp[]={&view};dkCmdBufBindRenderTargets(cmds,cp,1,nullptr);dkCmdBufClearColorFloat(cmds,0,DkColorMask_RGBA,0,0,0,0);}
  }
  barrier();
  resolvedBytes+=s->bytes;resolvedImages.push_back(s);surfaces[key]=s;return s;
}
static EdramImage* edramImage(const lsw_deko::EdramView& view){
  auto found=edramImages.find(view);if(found!=edramImages.end()){found->second.lastUse=activeSubmission;return &found->second;}
  // Ownership and completion are both required before reclaiming an old view.
  // Transfers consume current owners; they must remain alive until submitted.
  for(auto it=edramImages.begin();it!=edramImages.end();){
    if(edramOwner.retains(it->first)||!submissions.can_retire(it->second.lastUse)){++it;continue;}
    allocations.erase(std::remove(allocations.begin(),allocations.end(),it->second.storage),allocations.end());
    dkMemBlockDestroy(it->second.storage);edramBytes-=it->second.bytes;it=edramImages.erase(it);
  }
  if(edramImages.size()>=32||uint64_t(view.width())*view.height()*(view.depth?8:4)>(192u<<20)-std::min<uint64_t>(edramBytes,192u<<20)){
    fail("EDRAM view storage budget exhausted");return nullptr;
  }
  EdramImage target;
  if(!image(target.image,DkImageType_2D,view.depth?DkImageFormat_ZF32_X24S8:DkImageFormat_RGBA8_Unorm,
            view.width(),view.height(),1,1,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine,&target.storage))return nullptr;
  target.bytes=dkMemBlockGetSize(target.storage);target.lastUse=activeSubmission;
  if(target.bytes>(192u<<20)-std::min<uint64_t>(edramBytes,192u<<20)){fail("EDRAM native layout exceeds storage budget");return nullptr;}
  auto* out=&edramImages.emplace(view,target).first->second;edramBytes+=target.bytes;
  // Only previously unowned tiles may use these deterministic initial values.
  // Every owned tile is transferred (or fully overwritten) before acquisition.
  DkImageView native;dkImageViewDefaults(&native,&out->image);DkScissor all={0,0,view.width(),view.height()};
  dkCmdBufSetScissors(cmds,0,&all,1);
  if(view.depth){dkCmdBufBindRenderTargets(cmds,nullptr,0,&native);dkCmdBufClearDepthStencil(cmds,true,1,255,0);}
  else{const DkImageView* color[]={&native};dkCmdBufBindRenderTargets(cmds,color,1,nullptr);dkCmdBufClearColorFloat(cmds,0,DkColorMask_RGBA,0,0,0,0);}
  barrier();return out;
}
static bool acquireEdram(const std::vector<lsw_deko::EdramUse>& uses){
  lsw_deko::EdramOwner::Plan plan;std::string error;
  if(!edramOwner.prepare(uses,plan,error))return fail(error.c_str());
  // Preflight every view before recording ownership transfers. Keep ownership
  // unchanged on allocation/failure; no guest draw/copy has run at this point.
  for(const auto& use:uses)if(!edramImage(use.view))return false;
  if(!plan.transfers.empty())barrier();
  for(const auto& transfer:plan.transfers){
    auto from=edramImages.find(transfer.from),to=edramImages.find(transfer.to);
    if(from==edramImages.end()||to==edramImages.end())return fail("EDRAM owner image missing");
    from->second.lastUse=to->second.lastUse=activeSubmission;
    DkImageView src,dst;dkImageViewDefaults(&src,&from->second.image);dkImageViewDefaults(&dst,&to->second.image);
    const auto& a=transfer.source;const auto& b=transfer.destination;
    DkImageRect r={a.x,a.y,0,a.width,a.height,1},s={b.x,b.y,0,b.width,b.height,1};
    dkCmdBufCopyImage(cmds,&src,&r,&dst,&s,0);
  }
  if(!plan.transfers.empty())barrier();if(failed)return false;
  edramOwner.commit(plan);return true;
}
static bool bindEdram(const lsw_deko::EdramDraw& draw){
  if(draw.colorMask>15||(draw.colorMask&(draw.colorMask+1)))return fail("sparse EDRAM color slots require native output mapping");
  std::vector<lsw_deko::EdramUse> uses;for(unsigned i=0;i<4;++i)if(draw.colorMask&(1u<<i))uses.push_back(draw.colors[i]);
  if(draw.depthUsed)uses.push_back(draw.depth);if(!acquireEdram(uses))return false;
  DkImageView colors[4],depth;const DkImageView* pointers[4]={};unsigned count=0;
  for(unsigned i=0;i<4;++i)if(draw.colorMask&(1u<<i)){
    dkImageViewDefaults(&colors[i],&edramImages.at(draw.colors[i].view).image);pointers[i]=&colors[i];count=i+1;
  }
  if(draw.depthUsed)dkImageViewDefaults(&depth,&edramImages.at(draw.depth.view).image);
  dkCmdBufBindRenderTargets(cmds,pointers,count,draw.depthUsed?&depth:nullptr);return !failed;
}
static bool clearResolve(const LswGpuDrawRec& source,const lsw_deko::ResolvePlan& op){
  if(!op.clearColor&&!op.clearDepth)return true;
  // Execute in guest pipeline order. Separate acquisitions also make a color
  // clear overwriting the same backing as depth explicit, rather than binding
  // two conflicting native images simultaneously.
  for(bool depth:{true,false}){
    if(depth?!op.clearDepth:!op.clearColor)continue;
    lsw_deko::EdramUse use;std::string error;
    if(!lsw_deko::edram_use(source.surf_info,depth?source.depth_info:source.color_info,depth,op.source,true,use,error))return fail(error.c_str());
    if(!acquireEdram({use}))return false;
    DkImageView view;dkImageViewDefaults(&view,&edramImages.at(use.view).image);const DkImageView* color[]={&view};
    barrier();dkCmdBufBindRenderTargets(cmds,depth?nullptr:color,depth?0:1,depth?&view:nullptr);
    const auto& r=use.samples;DkScissor rect={r.x,r.y,r.width,r.height};dkCmdBufSetScissors(cmds,0,&rect,1);
    if(depth)dkCmdBufClearDepthStencil(cmds,true,op.depthClear,255,op.stencilClear);
    else{unsigned v=op.colorClear;dkCmdBufClearColorFloat(cmds,0,DkColorMask_RGBA,float(v&255)/255,float((v>>8)&255)/255,float((v>>16)&255)/255,float(v>>24)/255);}
    barrier();
  }
  barrier();return !failed;
}
static bool resolve(const LswGpuFrame& frame,unsigned record,const lsw_deko::ResolvePlan& op){
  const auto& source=frame.draws[record];
  if(op.copy){
    const auto& r=op.source;const auto& toRect=op.destinationRect;
    unsigned storageWidth=std::max(op.storagePitch,op.requiredWidth),storageHeight=std::max(op.storageHeight,op.requiredHeight);
    if(op.sampleCount>1&&!op.average&&(op.sample!=0||op.scaleX!=1))return fail("normalized sample selection not implemented by executor");
    lsw_deko::EdramUse use;std::string attachmentError;
    if(!lsw_deko::edram_use(source.surf_info,op.depth?source.depth_info:source.color_info,op.depth,r,false,use,attachmentError)||!acquireEdram({use}))return attachmentError.empty()?false:fail(attachmentError.c_str());
    const LswGpuResolveRegisters* raw=nullptr;
    for(const auto& candidate:frame.resolve_registers)if(candidate.record==record){if(raw)return fail("duplicate resolve layout metadata");raw=&candidate;}
    std::string error;lsw_deko::ResolveMemory memoryLayout;
    bool known=lsw_deko::resolve_memory(source,raw,memoryLayout,error);
    // Historical inputs may render without raw registers, but can never be
    // materialized under an invented destination layout.
    if(raw&&!known)return fail(error.c_str());
    auto dest=surface(op.destination,op.depth,storageWidth,storageHeight,known?&memoryLayout:nullptr);
    if(!dest)return false;dest->swap=op.swap;dest->layoutKnown=known;dest->memory=std::move(memoryLayout);
    frame.deko_execution->resolves[record]=dest;
    DkImageView src,dst;dkImageViewDefaults(&dst,&dest->image);
    dkImageViewDefaults(&src,&edramImages.at(use.view).image);
    barrier();
    DkImageRect to={toRect.x,toRect.y,0,toRect.width,toRect.height,1};
    if(op.sampleCount>1&&!op.average){
      for(unsigned row=0;row<r.height;++row){DkImageRect from={r.x,r.y*op.scaleY+row*op.scaleY,0,r.width,1,1};
        DkImageRect line=to;line.y+=row;line.height=1;dkCmdBufCopyImage(cmds,&src,&from,&dst,&line,0);}
    }else{DkImageRect from={r.x*op.scaleX,r.y*op.scaleY,0,r.width*op.scaleX,r.height*op.scaleY,1};
      // Exact transfers preserve bits, including native stencil. The 2D blit
      // interprets ZF32_X24S8 as RG32_FLOAT and can change the stencil word.
      if(op.average)dkCmdBufBlitImage(cmds,&src,&from,&dst,&to,DkBlitFlag_FilterLinear,0);
      else dkCmdBufCopyImage(cmds,&src,&from,&dst,&to,0);}
    barrier();
  }
  return clearResolve(source,op);
}
static Texture* texturePixels(const LswGpuTexDesc& d,const uint8_t* raw,size_t bytes,uint64_t hash){
  if(!raw||!d.data_size||d.data_size!=bytes){fail("texture pixels differ from owned extent");return nullptr;}
  for(auto& t:textures)if(t.hash==hash&&lsw_deko_same_texture(t.identity,d)&&t.bytes.size()==d.data_size&&!memcmp(t.bytes.data(),raw,d.data_size)){t.lastUse=activeSubmission;return &t;}
  if(textures.size()>=1024||textureBytes+d.data_size>(256u<<20)){fail("texture cache budget exhausted");return nullptr;}
  if(!d.width||!d.height||!d.layers||!d.mips||d.mips>16||(d.dim!=0&&d.dim!=2)||(d.dim==2&&d.layers!=6)){fail("unsupported decoded texture shape");return nullptr;}
  Texture t{};t.hash=hash;t.identity=d;t.id=nextImageId++;t.lastUse=activeSubmission;
  auto format=textureFormat(d.dxgi);if(errors)return nullptr;
  unsigned flags=d.dxgi==28?DkImageFlags_Usage2DEngine:0;
  if(!image(t.image,d.dim==2?DkImageType_Cubemap:DkImageType_2DArray,format,d.width,d.height,d.layers,d.mips,flags,&t.storage))return nullptr;
  for(unsigned layer=0;layer<d.layers;++layer)for(unsigned mip=0;mip<d.mips;++mip){
    unsigned bytes=d.mip_row_bytes[mip],rows=d.mip_rows[mip],stride=(bytes+31)&~31u;
    uint64_t start=uint64_t(layer)*d.layer_bytes+d.mip_offset[mip];
    if(!bytes||!rows||start+uint64_t(bytes)*rows>d.data_size||uint64_t(stride)*rows>UploadBytes){fail("texture mip outside snapshot");return nullptr;}
    unsigned offset=upload(nullptr,0,32);if(offset==~0u)return nullptr;
    if(stride*rows>UploadBytes-offset){fail("texture upload budget exceeded");return nullptr;}
    auto* dest=static_cast<uint8_t*>(dkMemBlockGetCpuAddr(activeSlot->upload))+offset;
    for(unsigned row=0;row<rows;++row){memcpy(dest+row*stride,raw+start+row*bytes,bytes);memset(dest+row*stride+bytes,0,stride-bytes);}
    activeSlot->used=offset+stride*rows;
    DkImageView view;dkImageViewDefaults(&view,&t.image);view.mipLevelOffset=mip;view.mipLevelCount=1;view.layerOffset=layer;view.layerCount=1;
    DkCopyBuf source={address(offset),stride,0};DkImageRect rect={0,0,0,std::max(1u,d.width>>mip),std::max(1u,d.height>>mip),1};
    dkCmdBufCopyBufferToImage(cmds,&source,&view,&rect,0);
  }
  barrier();textureBytes+=d.data_size;t.bytes.assign(raw,raw+d.data_size);textures.push_back(std::move(t));
  log("TEXTURE_LOAD id=%u width=%u height=%u mips=%u layers=%u dxgi=%u bytes=%u\n",textures.back().id,d.width,d.height,d.mips,d.layers,d.dxgi,d.data_size);
  return &textures.back();
}
static Texture* texture(const LswGpuFrame& frame,const LswGpuTextureSnapshot& snap){
  if(snap.offset>frame.arena.size()||snap.image.data_size>frame.arena.size()-snap.offset){fail("texture snapshot outside arena");return nullptr;}
  return texturePixels(snap.image,frame.arena.data()+snap.offset,snap.image.data_size,snap.hash);
}
static bool presentationSource(const LswGpuFrame& frame,DkImageView& front,DkImageRect& from,bool& opaque){
  opaque=false;
  const auto& source=frame.front;const auto& image=source.image;
  if(!image.ok||image.dxgi!=28||!image.width||!image.height||image.mips!=1||image.layers!=1||image.dim)
    return fail("presentation has no valid recorded frontbuffer view");
  Surface* resolved=nullptr;
  if(source.producer){
    const auto& retained=source.producer;
    if(!retained->image||retained->image->key!=retained->key)return fail("frontbuffer producer identity differs");
    resolved=retained->image.get();
  }else if(source.producer_record!=~0u){
    if(!frame.deko_execution||source.producer_record>=frame.deko_execution->resolves.size()||
       !frame.deko_execution->resolves[source.producer_record])return fail("frontbuffer has no exact executed producer");
    resolved=frame.deko_execution->resolves[source.producer_record].get();
  }
  if(resolved){
    if(resolved->depth||resolved->width!=image.width||resolved->height!=image.height)return fail("frontbuffer producer view differs");
    unsigned mapping=lsw_deko_image_mapping(image.component_mapping,resolved->swap);
    if(!lsw_deko_blit_mapping(mapping))return fail("frontbuffer requires composed channel mapping");
    resolved->lastUse=activeSubmission;dkImageViewDefaults(&front,&resolved->image);
    constexpr DkImageFormat formats[]={DkImageFormat_None,DkImageFormat_RGBA8_Unorm,DkImageFormat_BGRA8_Unorm,
        DkImageFormat_RGBA8_Unorm,DkImageFormat_BGRA8_Unorm};
    front.format=formats[lsw_deko_blit_format_index(mapping)];
    opaque=lsw_deko_blit_format_index(mapping)>2;
  }else{
    auto* tex=texturePixels(image,source.pixels.data(),source.pixels.size(),lsw_deko::cache_hash(source.pixels.data(),source.pixels.size()));
    if(!tex)return false;dkImageViewDefaults(&front,&tex->image);
  }
  front.layerCount=1;from={0,0,0,image.width,image.height,1};return !failed;
}
static bool bindTextures(const LswGpuFrame& frame,unsigned record,const LswGpuDrawRec& r,const LswGlShader* source,NativeShader* native){
  if(!native)return true;
  if(r.cb_fetch_off>frame.arena.size()||768>frame.arena.size()-r.cb_fetch_off)return fail("fetch constants outside arena");
  const auto* fetch=reinterpret_cast<const uint32_t*>(frame.arena.data()+r.cb_fetch_off);
  DkStage stage=source->stage==kLswGlVertex?DkStage_Vertex:source->stage==kLswGlPixel?DkStage_Fragment:DkStage_Geometry;
  // Shared vertex memory is a texture buffer in the compact binding map.
  if(native->entry->slots[40]!=255)dkCmdBufBindTexture(cmds,stage,native->entry->slots[40],dkMakeTextureHandle(0,0));
  for(const auto& unit:source->units){
    if(unit.unit>=48||unit.fetch_constant>=32||native->entry->slots[unit.unit]==255)return fail("offline shader binding differs");
    const auto* words=fetch+unit.fetch_constant*6;
    unsigned base=0,format=0,mapping=0,width=0,height=0,dim=0;
    bool valid=lsw_gpu_texture_info(words,&base,&format,&mapping,&width,&height,&dim)!=0;
    unsigned viewIndex=null2dId;
    bool cube=unit.dimension==3;
    const LswGpuTextureRef* ref=nullptr;
    for(const auto& candidate:frame.texture_refs)if(candidate.record==record&&candidate.fetch==unit.fetch_constant){ref=&candidate;break;}
    // Recorded authority always wins over cache history. A CPU snapshot cannot
    // be replaced by a surface another frame later produced at the same base.
    if(ref&&ref->snapshot<frame.texture_snapshots.size()){
      const auto& snap=frame.texture_snapshots[ref->snapshot];
      if(cube!=(snap.image.dim==2))viewIndex=cube?nullCubeId:null2dId;
      else{auto* tex=texture(frame,snap);if(!tex)return false;viewIndex=descriptor(tex->image,tex->id,snap.image.component_mapping);}
    }else if(valid&&ref&&ref->snapshot==~0u&&!cube&&dim==1){
      Surface* resolved=nullptr;
      if(ref->producer!=~0u){
        if(ref->producer>=frame.texture_producers.size())return fail("recorded texture producer reference outside frame");
        const auto& retained=frame.texture_producers[ref->producer];
        if(!retained||!retained->image||retained->image->key!=retained->key)return fail("recorded texture producer identity differs");
        resolved=retained->image.get();
      }else{
        if(!frame.deko_execution||ref->producer_record>=record||ref->producer_record>=frame.deko_execution->resolves.size()||
           !frame.deko_execution->resolves[ref->producer_record])return fail("recorded texture has no exact executed producer");
        resolved=frame.deko_execution->resolves[ref->producer_record].get();
      }
      resolved->lastUse=activeSubmission;
      unsigned mapped=format==2&&!resolved->depth?0x688:lsw_deko_image_mapping(mapping,resolved->swap,resolved->depth);
      viewIndex=descriptor(resolved->image,resolved->id,mapped,resolved->depth);
    }else if(!valid||(cube&&dim!=3)||(!cube&&dim==3))viewIndex=cube?nullCubeId:null2dId;
    else return fail("valid texture has neither owned pixels nor supported GPU source");
    unsigned samplerIndex=sampler(words,unit.sampler);
    if(failed)return false;
    dkCmdBufBarrier(cmds,DkBarrier_None,DkInvalidateFlags_Descriptors|DkInvalidateFlags_Image|DkInvalidateFlags_Shader);
    dkCmdBufBindTexture(cmds,stage,native->entry->slots[unit.unit],dkMakeTextureHandle(viewIndex,samplerIndex));
  }
  return !failed;
}
static bool waitSlot(Slot& slot,uint64_t& ns){
  if(dkQueueIsInErrorState(queue))return fail("cannot observe completion on an errored GPU queue");
  if(!slot.pending)return true;
  uint64_t start=gp_now();
  const char* e=getenv("LSWTCS_DEKO_EMULATOR");int64_t bound=e&&e[0]=='1'?60000000000ll:2000000000ll;
  auto result=dkFenceWait(&slot.fence,bound);ns+=gp_now()-start;
  if(dkQueueIsInErrorState(queue))return fail("GPU queue errored during completion wait");
  if(result!=DkResult_Success){log("FENCE serial=%llu submission=%llu result=%u\n",(unsigned long long)slot.serial,(unsigned long long)slot.submission,unsigned(result));return fail("GPU completion wait failed");}
  if(!submissions.observe_completion(slot.submission))return fail("fence has no submitted completion identity");
  if(gpuTimings&&slot.timed){
    const auto* values=static_cast<const uint64_t*>(dkMemBlockGetCpuAddr(timingMem))+(unsigned(&slot-slots)*4);
    // Deko reports [counter,timestamp] pairs; use the SDK's tick conversion.
    if(slot.serial<=8||slot.serial%120==0)log("GPU_INTERVAL serial=%llu submission=%llu start_tick=%llu end_tick=%llu ns=%llu instrumented=1 emulator=%s\n",(unsigned long long)slot.serial,(unsigned long long)slot.submission,
      (unsigned long long)values[1],(unsigned long long)values[3],(unsigned long long)(values[3]>=values[1]?dkTimestampToNs(values[3]-values[1]):0),e?e:"0");
  }
  slot.pending=false;return true;
}
static bool submitSlot(uint64_t serial,bool presentNext=false){
  if(dkQueueIsInErrorState(queue))return fail("GPU queue errored before submission");
  if(activeSlot->pending||activeSubmission!=submissions.next())return fail("command slot reused without completion");
  dkCmdBufSignalFence(cmds,&activeSlot->fence,true);
  auto commands=dkCmdBufFinishList(cmds);if(failed)return false;
  if(!submissions.submit(activeSubmission))return fail("submission identity out of order");
  activeSlot->submission=activeSubmission;activeSlot->serial=serial;activeSlot->pending=true;
  dkQueueSubmitCommands(queue,commands);
  // Submitting command lists may only append to the queue. A CPU fence wait
  // cannot depend on a later presentation to kick non-present work to the GPU.
  if(!presentNext)dkQueueFlush(queue);
  if(dkQueueIsInErrorState(queue))return fail("GPU queue errored during submission/flush");
  return !failed;
}
static void retireResolveImages(){
  for(auto it=resolvedImages.begin();it!=resolvedImages.end();){
    auto& image=**it;
    // The retirement list's own reference is the last one. Frame refs and the
    // latest-base compatibility map must release their logical ownership first.
    if(it->use_count()!=1||!submissions.can_retire(image.lastUse)){++it;continue;}
    for(auto view=imageViews.begin();view!=imageViews.end();)if(std::get<0>(view->first)==image.id){freeImageDescriptors.push_back(view->second);view=imageViews.erase(view);}else ++view;
    allocations.erase(std::remove(allocations.begin(),allocations.end(),image.storage),allocations.end());
    dkMemBlockDestroy(image.storage);resolvedBytes-=image.bytes;it=resolvedImages.erase(it);
  }
}
static bool beginCommands(uint64_t& waitNs){
  activeSlot=&slots[submissions.submitted()%Frames];if(!waitSlot(*activeSlot,waitNs))return false;
  activeSubmission=submissions.next();activeSlot->used=0;activeSlot->timed=false;cmds=activeSlot->commands;dkCmdBufClear(cmds);
  retireResolveImages();return !failed;
}
static bool initialize(){
  report=fopen("sdmc:/switch/lswtcs-deko-draw/probe.log","w");if(!report)return false;
  log("LIVE_DEKO version=0.0.7 game_frame_recorder=1 capture_replay=0\n");
  std::string error;if(!shaderCache.load("sdmc:/switch/lswtcs-deko-draw/deko-shaders.bin",LswDekoCompilerId,error))return fail(error.c_str());
  DkDeviceMaker dm;dkDeviceMakerDefaults(&dm);dm.cbDebug=debug;device=dkDeviceCreate(&dm);if(!device)return fail("device creation failed");
  DkQueueMaker qm;dkQueueMakerDefaults(&qm,device);qm.flags=DkQueueFlags_Graphics;queue=dkQueueCreate(&qm);if(!queue)return fail("queue creation failed");
  codeMem=memory(CodeBytes,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached|DkMemBlockFlags_Code);
  sharedMem=memory(SharedBytes,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image);
  descriptors=memory(SamplerOffset+SamplerDescriptorCount*sizeof(DkSamplerDescriptor),DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
  if(!codeMem||!sharedMem||!descriptors)return false;
  readbackMem=memory(Width*Height*4,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);if(!readbackMem)return false;
  const char* timing=getenv("LSWTCS_DEKO_TIMINGS");gpuTimings=timing&&timing[0]=='1';
  if(gpuTimings){timingMem=memory(4096,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);if(!timingMem)return false;}
  memset(dkMemBlockGetCpuAddr(sharedMem),0,SharedBytes);
  for(auto& slot:slots){
    slot.command=memory(CommandBytes,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
    slot.upload=memory(UploadBytes,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
    if(!slot.command||!slot.upload)return false;
    DkCmdBufMaker maker;dkCmdBufMakerDefaults(&maker,device);slot.commands=dkCmdBufCreate(&maker);if(!slot.commands)return fail("command buffer creation failed");
    dkCmdBufAddMemory(slot.commands,slot.command,0,CommandBytes);
  }
  activeSlot=&slots[0];cmds=activeSlot->commands;
  DkImageLayoutMaker lm;dkImageLayoutMakerDefaults(&lm,device);lm.type=DkImageType_Buffer;lm.format=DkImageFormat_R32_Uint;lm.dimensions[0]=SharedBytes/4;
  DkImageLayout layout;dkImageLayoutInitialize(&layout,&lm);if(dkImageLayoutGetSize(&layout)>SharedBytes)return fail("shared image layout too large");
  dkImageInitialize(&sharedImage,&layout,sharedMem,0);DkImageView view;dkImageViewDefaults(&view,&sharedImage);
  dkImageDescriptorInitialize(static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(descriptors)),&view,false,false);imageCount=1;
  LswGpuSamplerBinding binding{};uint32_t fetch[6]{};sampler(fetch,binding);
  const unsigned render=DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine;
  if(!image(null2d,DkImageType_2DArray,DkImageFormat_RGBA8_Unorm,1,1,1,1,0)||!image(nullCube,DkImageType_Cubemap,DkImageFormat_RGBA8_Unorm,1,1,6,1,0))return false;
  uint8_t zeros[256]{};unsigned zero=upload(zeros,sizeof zeros);
  for(auto* im:{&null2d,&nullCube})for(unsigned face=0;face<(im==&nullCube?6u:1u);++face){
    dkImageViewDefaults(&view,im);view.layerOffset=face;view.layerCount=1;
    DkCopyBuf source={address(zero),32,0};DkImageRect rect={0,0,0,1,1,1};dkCmdBufCopyBufferToImage(cmds,&source,&view,&rect,0);}
  null2dId=descriptor(null2d,nextImageId++,0x688);nullCubeId=descriptor(nullCube,nextImageId++,0x688);
  barrier();dkQueueSubmitCommands(queue,dkCmdBufFinishList(cmds));dkQueueWaitIdle(queue);dkCmdBufClear(cmds);
  const DkImage* images[Frames];for(unsigned i=0;i<Frames;++i){if(!image(display[i],DkImageType_2D,DkImageFormat_RGBA8_Unorm,Width,Height,1,1,render|DkImageFlags_UsagePresent))return false;images[i]=&display[i];}
  DkSwapchainMaker sm;dkSwapchainMakerDefaults(&sm,device,nwindowGetDefault(),images,Frames);swapchain=dkSwapchainCreate(&sm);if(!swapchain)return fail("swapchain creation failed");
  dkSwapchainSetSwapInterval(swapchain,1);log("READY cache_variants=%u completion_slots=2 image_descriptors=%u sampler_descriptors=%u\n",unsigned(shaderCache.count()),ImageDescriptorCount,SamplerDescriptorCount);return !failed;
}
static bool render(LswGpuFrame& frame,uint32_t end,bool present){
  uint64_t start=gp_now(),waitNs=0;if(frame.texture_snapshot_error)return fail("recording texture snapshot budget/error");
  if(end>frame.draws.size())return fail("execution prefix exceeds recorded commands");
  if(!frame.deko_execution)frame.deko_execution=std::make_shared<LswDekoFrameExecution>();
  auto& execution=*frame.deko_execution;
  if(execution.executed>frame.draws.size())return fail("frame execution lifetime invalid");
  if(execution.presented){
    if(!present&&end<=execution.executed)return true;
    if(end!=execution.executed)return fail("presented frame command extent changed");
  }
  const unsigned begin=execution.executed;
  if(end<begin){if(present)return fail("presentation omits executed commands");return true;}
  if(end==begin&&!present)return true;
  execution.resolves.resize(frame.draws.size());
  if(!beginCommands(waitNs))return false;
  // Completion is the safety proof. Keeping the most recently submitted working
  // set is only cache policy, independent of presentation/guest-frame numbering.
  // Image storage and ALL its descriptor views retire under the same proof.
  if(textureBytes>(64u<<20)){
    unsigned retired=0;
    for(auto it=textures.begin();it!=textures.end();){
      if(!submissions.can_retire(it->lastUse)||it->lastUse>=submissions.submitted()){++it;continue;}
      for(auto view=imageViews.begin();view!=imageViews.end();)if(std::get<0>(view->first)==it->id){freeImageDescriptors.push_back(view->second);view=imageViews.erase(view);}else ++view;
      allocations.erase(std::remove(allocations.begin(),allocations.end(),it->storage),allocations.end());
      dkMemBlockDestroy(it->storage);textureBytes-=it->identity.data_size;it=textures.erase(it);++retired;
    }
    if(retired)log("TEXTURE_RETIRE submission=%llu completed_submission=%llu count=%u remaining_bytes=%llu completion_proven=1\n",(unsigned long long)activeSubmission,(unsigned long long)submissions.completed(),retired,(unsigned long long)textureBytes);
  }
  if(gpuTimings){activeSlot->timed=true;dkCmdBufReportCounter(cmds,DkCounter_TimestampPipelineTop,dkMemBlockGetGpuAddr(timingMem)+(activeSlot-slots)*32);}
  std::string error;lsw_deko::FrameConstants constants(frame);std::vector<FrameRecord> plans(frame.draws.size());
  std::vector<lsw_deko::ResolvePlan> transfers(frame.draws.size());
  std::vector<lsw_deko::EdramDraw> attachments(frame.draws.size());
  std::vector<std::array<NativeShader*,3>> shaderPlans(frame.draws.size());
  for(unsigned i=begin;i<end;++i){const auto& source=frame.draws[i];auto& plan=plans[i];
    if(source.kind&&frame.serial<=2)log("BOOT_RESOLVE serial=%llu record=%u surf=%08X src=%u sample=%u cmd=%u rect=%d/%d/%d/%d dest=%08X pitch=%u height=%u fmt=%u offset=%d/%d clear=%u/%u value=%08X/%08X\n",
      (unsigned long long)frame.serial,i,source.surf_info,source.rs_src,source.rs_sample,source.rs_copy_cmd,source.rs_rect[0],source.rs_rect[1],source.rs_rect[2],source.rs_rect[3],source.rs_dest,source.rs_dest_pitch,source.rs_dest_height,source.rs_dest_fmt,source.rs_dest_x,source.rs_dest_y,source.rs_clear_color,source.rs_clear_depth,source.rs_color_clear,source.rs_depth_clear);
    if(!lsw_deko::draw_plan(source,plan,error,&transfers[i])){log("STATE_MISS serial=%llu record=%u kind=%u surf=%08X mode=%u color=%08X/%08X/%08X/%08X depth=%08X\n",(unsigned long long)frame.serial,i,source.kind,source.surf_info,source.edram_mode,source.color_info_mrt[0],source.color_info_mrt[1],source.color_info_mrt[2],source.color_info_mrt[3],source.depth_info);
      char path[192];snprintf(path,sizeof path,"%sunsupported-records.bin",App);if(FILE* f=fopen(path,"wb")){uint32_t header[2]={sizeof(LswGpuDrawRec),uint32_t(frame.draws.size())};fwrite(header,1,sizeof header,f);fwrite(frame.draws.data(),sizeof(LswGpuDrawRec),frame.draws.size(),f);fclose(f);}return fail(error.c_str());}
    if(plan.kind==3){log("NO_ATTACHMENT serial=%llu record=%u shared_plan=1\n",(unsigned long long)frame.serial,i);continue;}
    if(source.kind)continue;
    if(!lsw_deko_frame_attachments(frame,i,attachments[i],error))return fail(error.c_str());
    if(!constants.draw(source,plan,error))return fail(error.c_str());
    auto& shaders=shaderPlans[i];shaders[0]=shader(source.vs_dxbc);
    if(source.ps_size)shaders[1]=shader(source.ps_dxbc);if(source.gs_size)shaders[2]=shader(source.gs_dxbc);
    if(failed)return false;
  }
  if(frame.arena.size()>UploadBytes||constants.data.size()>UploadBytes-frame.arena.size()-std::min<size_t>(255,UploadBytes-frame.arena.size()))return fail("recorded frame exceeds upload budget");
  unsigned arenaOffset=upload(frame.arena.data(),unsigned(frame.arena.size()));
  unsigned constantOffset=upload(constants.data.data(),unsigned(constants.data.size()));if(failed)return false;
  auto ranges=[&](const LswGpuRange* source,unsigned count){
    if(count)barrier();
    for(unsigned i=0;i<count;++i){const auto& range=source[i];if(range.arena_off==kLswRangeResident)continue;
      if(range.phys>SharedBytes||range.size>SharedBytes-range.phys||range.arena_off>frame.arena.size()||range.size>frame.arena.size()-range.arena_off||(range.phys|range.size|range.arena_off)&3)return fail("vertex range outside owned frame");
      dkCmdBufCopyBuffer(cmds,address(arenaOffset+range.arena_off),dkMemBlockGetGpuAddr(sharedMem)+range.phys,range.size);}
    if(count)barrier();return true;};
  if(!begin&&!ranges(frame.prelude.data(),unsigned(frame.prelude.size())))return false;
  dkCmdBufBindImageDescriptorSet(cmds,dkMemBlockGetGpuAddr(descriptors),ImageDescriptorCount);
  dkCmdBufBindSamplerDescriptorSet(cmds,dkMemBlockGetGpuAddr(descriptors)+SamplerOffset,SamplerDescriptorCount);
  dkCmdBufBarrier(cmds,DkBarrier_Full,DkInvalidateFlags_Shader|DkInvalidateFlags_Descriptors);
  unsigned draws=0,resolves=0;
  for(unsigned i=begin;i<end;++i){const auto& source=frame.draws[i];auto& plan=plans[i];
    if(plan.kind==4){if(!resolve(frame,i,transfers[i]))return false;++resolves;continue;}
    if(source.first_range>frame.ranges.size()||source.range_count>frame.ranges.size()-source.first_range)return fail("draw ranges outside frame");
    if(!ranges(frame.ranges.data()+source.first_range,source.range_count))return false;
    if(plan.kind==3)continue;
    if(!bindEdram(attachments[i]))return false;
    auto& native=shaderPlans[i];const DkShader* stages[3];unsigned stageCount=0;
    for(auto* s:native)if(s)stages[stageCount++]=&s->shader;
    dkCmdBufBindShaders(cmds,DkStageFlag_GraphicsMask,stages,stageCount);
    DkBufExtents cb[6];for(unsigned c=0;c<6;++c)cb[c]={address(constantOffset+plan.cbOffset[c]),plan.cbSize[c]};
    for(auto stage:{DkStage_Vertex,DkStage_Fragment,DkStage_Geometry})dkCmdBufBindUniformBuffers(cmds,stage,0,cb,6);
    unsigned textureRecord=source.tile_repeat?source.tile_repeat-1:i;
    if(textureRecord>i)return fail("tile repeat refers to future record");
    if(!bindTextures(frame,textureRecord,source,static_cast<const LswGlShader*>(source.vs_dxbc),native[0])||
       !bindTextures(frame,textureRecord,source,static_cast<const LswGlShader*>(source.ps_dxbc),native[1]))return false;
    state(plan);if(errors)return fail("unsupported draw state");
    if(plan.indexCount){uint64_t bytes=uint64_t(plan.indexCount)*(plan.index32?4:2);unsigned limit=source.ib_shm?SharedBytes:unsigned(frame.arena.size());
      if(plan.indexOffset>limit||bytes>limit-plan.indexOffset)return fail("index buffer outside owned frame");
      DkGpuAddr indices=source.ib_shm?dkMemBlockGetGpuAddr(sharedMem)+plan.indexOffset:address(arenaOffset+plan.indexOffset);
      dkCmdBufBindIdxBuffer(cmds,plan.index32?DkIdxFormat_Uint32:DkIdxFormat_Uint16,indices);
      dkCmdBufDrawIndexed(cmds,primitive(plan.topology),plan.indexCount,1,0,0,0);
    }else dkCmdBufDraw(cmds,primitive(plan.topology),plan.vertexCount,1,0,0);++draws;
  }
  if(!present){
    if(gpuTimings)dkCmdBufReportCounter(cmds,DkCounter_Timestamp,dkMemBlockGetGpuAddr(timingMem)+(activeSlot-slots)*32+16);
    barrier();if(!submitSlot(frame.serial))return false;execution.executed=end;
    log("PREFIX serial=%llu begin=%u end=%u submission=%llu presentation=0\n",(unsigned long long)frame.serial,begin,end,(unsigned long long)activeSubmission);return true;
  }
  DkImageView front;DkImageRect frontRect;bool opaque;
  if(!presentationSource(frame,front,frontRect,opaque))return false;
  uint64_t recordEnd=gp_now();int imageSlot=dkQueueAcquireImage(queue,swapchain);
  uint64_t acquired=gp_now();if(imageSlot<0||imageSlot>=int(Frames))return fail("swapchain acquire failed");
  DkImageView destination;dkImageViewDefaults(&destination,&display[imageSlot]);DkImageRect rect={0,0,0,Width,Height,1};
  barrier();dkCmdBufBlitImage(cmds,&front,&frontRect,&destination,&rect,DkBlitFlag_FilterNearest,0);barrier();
  if(opaque){lsw_deko_clear_present_alpha(cmds,destination,Width,Height);barrier();}
  bool capture=false;
  if(FILE* trigger=fopen("sdmc:/lswtcs/deko_shot_now","rb")){fclose(trigger);remove("sdmc:/lswtcs/deko_shot_now");capture=true;}
  if(capture){DkCopyBuf dest={dkMemBlockGetGpuAddr(readbackMem),Width*4,0};dkCmdBufCopyImageToBuffer(cmds,&destination,&rect,&dest,0);barrier();}
  if(gpuTimings)dkCmdBufReportCounter(cmds,DkCounter_Timestamp,dkMemBlockGetGpuAddr(timingMem)+(activeSlot-slots)*32+16);
  if(!submitSlot(frame.serial,true))return false;
  execution.executed=end;execution.presented=true;
  dkQueuePresentImage(queue,swapchain,imageSlot);++presentations;
  if(capture){
    uint64_t captureWait=0;if(!waitSlot(*activeSlot,captureWait))return false;
    char path[192];snprintf(path,sizeof path,"%slive-%06llu.rgba",App,(unsigned long long)frame.serial);
    FILE* f=fopen(path,"wb");bool ok=f&&fwrite(dkMemBlockGetCpuAddr(readbackMem),1,Width*Height*4,f)==Width*Height*4;if(f)fclose(f);
    log("CAPTURE serial=%llu path=%s ok=%u completion_wait_ns=%llu excluded_from_performance=1\n",(unsigned long long)frame.serial,path,unsigned(ok),(unsigned long long)captureWait);
    if(!ok)return fail("live readback write failed");
  }
  if(presentations<=8||presentations%120==0)log("FRAME n=%llu serial=%llu submission=%llu completed_submission=%llu draws=%u resolves=%u arena=%u upload=%u shaders=%u textures=%u record_ns=%llu completion_wait_ns=%llu acquire_ns=%llu submit_present_ns=%llu errors=%u debug=%u\n",
    (unsigned long long)presentations,(unsigned long long)frame.serial,(unsigned long long)activeSubmission,(unsigned long long)submissions.completed(),draws,resolves,unsigned(frame.arena.size()),activeSlot->used,unsigned(nativeShaders.size()),unsigned(textures.size()),
    (unsigned long long)(recordEnd-start-waitNs),(unsigned long long)waitNs,(unsigned long long)(acquired-recordEnd),(unsigned long long)(gp_now()-acquired),errors,debugMessages);
  return !failed;
}
static bool materialize(MaterializeJob& request){
  LswDekoProducerRef retained=request.producer;
  if(request.frame){
    auto& frame=*request.frame;
    if(!render(frame,request.end,false))return false;
    if(request.record==~0u)return true;
    auto& execution=*frame.deko_execution;
    if(request.record>=execution.resolves.size()||!execution.resolves[request.record])return fail("readback has no executed resolve producer");
    auto image=execution.resolves[request.record];
    retained=std::make_shared<const LswDekoProducer>(LswDekoProducer{
        image->key,frame.serial,request.record,image->width,image->height,image->memory,image});
  }
  if(!retained||!retained->image)return fail("missing retained resolve producer");
  auto& producer=*retained->image;
  if(!retained->key||producer.key!=retained->key||producer.width!=retained->width||producer.height!=retained->height)
    return fail("retained resolve producer identity differs");
  if(!producer.layoutKnown)return fail("readback resolve destination layout unknown");
  if(request.captured){*request.captured=retained;return true;}
  auto ranges=request.ranges;
  if(ranges.empty())ranges.push_back(request.wanted);
  if(!request.physical)return fail("readback guest destination invalid");
  for(auto range:ranges)if(!range.valid()||range.end()>request.physicalBytes)return fail("readback guest destination invalid");
  // A raw image copy does not convert native depth/stencil into guest bits.
  // Transfer first; CPU packing uses the retained producer's exact metadata.
  unsigned stride=(producer.width*(producer.depth?8:4)+31)&~31u;
  uint64_t bytes=uint64_t(stride)*producer.height;
  if(bytes>UINT32_MAX)return fail("readback exceeds transfer domain");
  uint64_t waitNs=0;if(!beginCommands(waitNs))return false;
  auto transfer=memory(unsigned(bytes),DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);if(!transfer)return false;
  DkImageView source;dkImageViewDefaults(&source,&producer.image);source.layerCount=1;
  DkImageRect rect={0,0,0,producer.width,producer.height,1};
  DkCopyBuf destination={dkMemBlockGetGpuAddr(transfer),stride,0};
  producer.lastUse=activeSubmission;
  barrier();dkCmdBufCopyImageToBuffer(cmds,&source,&rect,&destination,0);barrier();
  if(!submitSlot(retained->serial)||!waitSlot(*activeSlot,waitNs))return false;
  if(producer.depth&&writebackCheck){
    const auto& rect=producer.memory.resolve.destinationRect;
    uint32_t words[2];memcpy(words,static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(transfer))+size_t(rect.y)*stride+size_t(rect.x)*8,8);
    log("DEPTH_NATIVE record=%u low=%08X high=%08X\n",retained->record,words[0],words[1]);
  }
  bool ok=lsw_deko::encode_resolve_guest_ranges(retained->memory,dkMemBlockGetCpuAddr(transfer),size_t(bytes),
      producer.width,producer.height,stride,request.physical,request.physicalBytes,std::move(ranges));
  // Completion above proves the transfer allocation can be destroyed now.
  allocations.erase(std::remove(allocations.begin(),allocations.end(),transfer),allocations.end());dkMemBlockDestroy(transfer);
  log("MATERIALIZE serial=%llu record=%u image=%u submission=%llu completed=%llu spans=%u ok=%u\n",(unsigned long long)retained->serial,retained->record,producer.id,(unsigned long long)activeSubmission,(unsigned long long)submissions.completed(),unsigned(request.ranges.empty()?1:request.ranges.size()),unsigned(ok));
  return ok?true:fail("resolved guest-byte encoding failed");
}
#include "lsw_deko_writeback_check.inc"
// No guest producer may be running while this diagnostic tears down shared
// runtime state. Its caller returns through the platform's joined main path.
static bool shutdownDiagnostic(){
  log("SHUTDOWN begin submissions=%llu completed=%llu\n",(unsigned long long)submissions.submitted(),(unsigned long long)submissions.completed());
  if(queue){
    uint64_t waited=0;
    for(auto& slot:slots)if(!waitSlot(slot,waited)){
      log("SHUTDOWN deferred=1 reason=incomplete_submission\n");return false;
    }
    if(dkQueueIsInErrorState(queue)){
      log("SHUTDOWN deferred=1 reason=queue_error\n");return false;
    }
    dkQueueWaitIdle(queue);
    if(dkQueueIsInErrorState(queue)){
      log("SHUTDOWN deferred=1 reason=idle_queue_error\n");return false;
    }
    log("SHUTDOWN queue_idle=1\n");
    dkQueueDestroy(queue);queue=nullptr;
  }
  for(auto& slot:slots)if(slot.commands){dkCmdBufDestroy(slot.commands);slot.commands=nullptr;}
  cmds=nullptr;
  if(swapchain){dkSwapchainDestroy(swapchain);swapchain=nullptr;}
  // Images/views are non-owning Deko values. Destroy GPU backing once, after
  // the queue and command buffers no longer retain commands referencing it.
  surfaces.clear();resolvedImages.clear();textures.clear();edramImages.clear();edramOwner.reset();edramBytes=0;
  imageViews.clear();nativeShaders.clear();samplers.clear();
  for(auto it=allocations.rbegin();it!=allocations.rend();++it)dkMemBlockDestroy(*it);
  allocations.clear();
  if(device){dkDeviceDestroy(device);device=nullptr;}
  log("SHUTDOWN complete=1 errors=%u debug=%u\n",errors,debugMessages);
  return !errors;
}
static void renderThread(){
  svcSetThreadCoreMask(CUR_THREAD_HANDLE,1,1u<<1);
  if(!initialize()){failed=true;rendererFinished();return;}
  while(!stopping){
    {std::unique_lock<std::mutex> lock(handoffMutex);handoffCv.wait(lock,[]{return job||stopping||failed;});if(stopping||failed)break;}
    bool ok;
    if(borrowed.frame||borrowed.producer)ok=materialize(borrowed);
    else{gpu_frame_take();ok=render(g_gpu_frame_render,uint32_t(g_gpu_frame_render.draws.size()),true);}
    {std::lock_guard<std::mutex> lock(handoffMutex);completed=posted;job=false;borrowed={};}
    handoffCv.notify_all();if(!ok)break;
    const char* limit=getenv("LSWTCS_DEKO_LIMIT");if(limit&&presentations>=strtoull(limit,nullptr,10)){stopping=true;break;}
  }
  for(auto& slot:slots){uint64_t wait=0;if(!waitSlot(slot,wait))break;}
  if(queue&&!failed)dkQueueWaitIdle(queue);
  log("END presentations=%llu submissions=%llu completed_submission=%llu errors=%u debug=%u failed=%u\n",(unsigned long long)presentations,(unsigned long long)submissions.submitted(),(unsigned long long)submissions.completed(),errors,debugMessages,unsigned(failed.load()));
  rendererFinished();
}
} // namespace
// Invoked after platform environment/log setup, BEFORE guest allocation or any
// game/audio workers. -1 selects the ordinary live route. Never calls exit().
int lsw_deko_startup_check(){
  const char* check=getenv("LSWTCS_DEKO_VALIDATE_WRITEBACK");
  bool selected=check&&check[0]=='1';
  if(FILE* trigger=fopen("sdmc:/switch/lswtcs-deko-draw/validate-writeback","rb")){fclose(trigger);selected=true;}
  if(!selected)return -1;
  printf("[DEKO] startup writeback diagnostic; guest_started=0\n");fflush(stdout);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE,1,1u<<1);
  writebackCheck=true;
  if(!initialize())failed=true;
  log("MODE diagnostic=1 guest_started=0\n");
  if(!failed&&!validateWriteback())failed=true;
  if(!shutdownDiagnostic())failed=true;
  int result=failed?1:0;
  log("DIAGNOSTIC_RETURN result=%d guest_started=0\n",result);
  if(report){fclose(report);report=nullptr;}
  printf("[DEKO] startup diagnostic returned %d; returning to platform main\n",result);fflush(stdout);
  return result;
}
static bool dispatchBorrowed(MaterializeJob request){
  struct Wait {MaterializeJob request;bool ok=false;} state{request};
  // Unlike ordinary presentation's optional A/B yielding, this path must leave
  // the cooperative turn while blocked. Nested/off-rotation callers stay off.
  lswtcs_gil_resource_wait([](void* opaque){
    auto& state=*static_cast<Wait*>(opaque);
    std::unique_lock<std::mutex> lock(handoffMutex);
    handoffCv.wait(lock,[]{return !job||failed||stopping||finished;});
    if(failed||stopping||finished)return;
    borrowed=state.request;uint64_t ticket=++posted;job=true;handoffCv.notify_all();
    handoffCv.wait(lock,[&]{return completed>=ticket||failed||stopping||finished;});
    // Even on GPU failure, keep the caller's borrowed frame alive until the
    // render thread has stopped reading it. Failure cannot revoke a live loan.
    handoffCv.wait(lock,[&]{return completed>=ticket||finished;});
    state.ok=completed>=ticket&&!failed&&!stopping;
  },&state);
  return state.ok;
}
bool lsw_deko_execute_prefix(LswGpuFrame& frame,uint32_t end){
  if(end>frame.draws.size())return false;
  MaterializeJob request;request.frame=&frame;request.end=end;return dispatchBorrowed(request);
}
bool lsw_deko_materialize_resolve(LswGpuFrame& frame,uint32_t record,uint8_t* physical,size_t bytes,lsw_deko::GuestRange wanted){
  if(record>=frame.draws.size()||frame.draws[record].kind!=1||!physical||!wanted.valid()||wanted.end()>bytes)return false;
  MaterializeJob request;request.frame=&frame;request.end=record+1;request.record=record;
  request.physical=physical;request.physicalBytes=bytes;request.wanted=wanted;return dispatchBorrowed(request);
}
bool lsw_deko_capture_resolve(LswGpuFrame& frame,uint32_t record,LswDekoProducerRef& out){
  out.reset();
  if(record>=frame.draws.size()||frame.draws[record].kind!=1)return false;
  MaterializeJob request;request.frame=&frame;request.end=record+1;request.record=record;request.captured=&out;
  if(dispatchBorrowed(request)&&out)return true;
  out.reset();return false;
}
bool lsw_deko_materialize_producer(LswDekoProducerRef producer,uint8_t* physical,size_t bytes,lsw_deko::GuestRange wanted){
  if(!producer||!producer->image||!physical||!wanted.valid()||wanted.end()>bytes)return false;
  MaterializeJob request;request.producer=std::move(producer);
  request.physical=physical;request.physicalBytes=bytes;request.wanted=wanted;return dispatchBorrowed(request);
}
bool lsw_deko_materialize_producer_ranges(LswDekoProducerRef producer,uint8_t* physical,size_t bytes,const std::vector<lsw_deko::GuestRange>& wanted){
  if(!producer||!producer->image||!physical||wanted.empty())return false;
  for(auto range:wanted)if(!range.valid()||range.end()>bytes)return false;
  MaterializeJob request;request.producer=std::move(producer);request.ranges=wanted;
  request.physical=physical;request.physicalBytes=bytes;return dispatchBorrowed(request);
}
void gpu_d3d12_init(){
  // Must match the source identity used by offline preparation, before first prepare.
  setenv("LSWTCS_GLFMZ","0",1);
  const char* pad=getenv("LSWTCS_CBPAD");if(pad&&atoi(pad)==0){failed=true;finished=true;printf("[DEKO] CBPAD=0 unsupported by owned constant upload\n");return;}
  printf("[DEKO] live backend 0.0.7, offline DKSH, two completion slots\n");fflush(stdout);
  std::thread(renderThread).detach();
}
void lsw_deko_present(uint8_t* base){
  LswDekoCaptureGuard resource_capture;
  auto outcome=[](){if(!failed&&!stopping)return;
    lswtcs_gil_resource_wait([](void*){std::unique_lock<std::mutex> lock(handoffMutex);handoffCv.wait(lock,[]{return finished.load();});},nullptr);
    fflush(stdout);fflush(stderr);std::exit(failed?1:0);};
  outcome();
  // Block off the guest scheduler until this frame's CPU-owned data has been
  // uploaded. The GPU may still execute up to two independently fenced slots.
  bool behind;{std::lock_guard<std::mutex> lock(handoffMutex);behind=completed!=posted;}
  if(behind)lswtcs_gil_resource_wait([](void*){std::unique_lock<std::mutex> lock(handoffMutex);handoffCv.wait(lock,[]{return completed==posted||failed||stopping;});},nullptr);
  outcome();
  // The resource lock prevents publication/source mutation, and the previous
  // job has returned its loan. Do not hold the frame mutex during writeback:
  // dispatch may need the render thread. A repeat present owns the render frame.
  LswGpuFrame* target;LswGpuSwap display;
  {std::lock_guard<std::mutex> lock(g_gpu_frame_mtx);
    target=g_gpu_frame_ready?&g_gpu_frame_present:&g_gpu_frame_render;
    display=g_gpu_swap;}
  // A consumed display event without draws may supersede a pending draw frame.
  // Before the first consumed swap there is no display request to execute.
  // A later VdSwap call is only a wakeup; it may describe the next buffer.
  if(!display.recorded)return;
  if(!lsw_gpu_swap_valid(display)){fail("ordered display fetch differs from its swap packet");outcome();return;}
  if(!lsw_deko_capture_front(*target,display.fetch,base+0x80000000ull,lsw_deko_recording_resources())){
    fail("frontbuffer capture has no supported authoritative source");outcome();return;
  }
  // Reserve and publish under the same lock as prefix jobs. A prefix arriving
  // after the earlier wait must not be overwritten by this presentation.
  lswtcs_gil_resource_wait([](void*){
    std::unique_lock<std::mutex> lock(handoffMutex);
    handoffCv.wait(lock,[]{return !job||failed||stopping||finished;});
    if(failed||stopping||finished)return;
    borrowed={};uint64_t ticket=++posted;job=true;handoffCv.notify_all();
    // Keep this published frame alive until take/execution have released it.
    handoffCv.wait(lock,[&]{return completed>=ticket||finished;});
  },nullptr);
  outcome();
  if(!appletMainLoop()){stopping=true;handoffCv.notify_all();outcome();}
}
bool gpu_d3d12_poll(){return !failed&&!stopping&&appletMainLoop();}
extern "C" void lswtcs_set_pending_shad(const char*){}
extern "C" int lswtcs_pathb_compile(const char*,uint32_t){return 0;}
