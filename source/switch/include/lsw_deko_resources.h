// One serialization domain for capture, guest access and allocation lifetimes.
// Prepare before guest writers start. The caller must drain writers before
// changing observer/page-map publication or destroying this object.
#pragma once
#include "lsw_deko_executor.h"
#include "lsw_deko_texture_sources.h"
#include "lsw_guest_memory_access.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

class LswDekoResourceAccess {
public:
  using Wait=void (*)(void (*)(void*),void*);
  using Materialize=bool (*)(LswDekoProducerRef,uint8_t*,size_t,const std::vector<lsw_deko::GuestRange>&);
private:
  uint8_t* physical_;
  size_t bytes_;
  Wait wait_;
  Materialize materialize_;
  std::recursive_mutex mutex_;
  lsw_deko::ResourceOwner owner_;
  LswDekoProducers producers_;
  std::vector<lsw_deko::GuestRange> writes_; // protected across the actual CPU mutation
  std::vector<std::atomic<uint64_t>> page_bits_;
  lsw_guest::WatchedPages pages_;
  lsw_guest::WriteObserver observer_;
  bool materializing_=false;

  [[noreturn]] static void stop(const char* reason) noexcept {
    fprintf(stderr,"[DEKO_RESOURCE] %s\n",reason);fflush(stderr);std::abort();
  }
  static size_t watchWords(size_t bytes) {
    if(!bytes||bytes>0x20000000ull)stop("invalid watched backing extent");
    return ((bytes+4095)/4096+63)/64;
  }
  bool prepare_ranges(const std::vector<lsw_deko::GuestRange>& ranges) {
    if(materializing_)stop("observed guest access inside raw GPU writeback");
    std::map<uint64_t,std::vector<lsw_deko::GuestRange>> pending;
    std::vector<lsw_deko::GuestSource> sources;
    for(auto range:ranges){
      if(range.end()>bytes_||!owner_.capture(range,sources))return false;
      for(const auto& source:sources)if(!source.ram_current)pending[source.key].push_back(source.range);
    }
    for(auto& entry:pending){
      auto producer=producers_.find(entry.first);if(!producer)return false;
      materializing_=true;
      bool ok=materialize_(std::move(producer),physical_,bytes_,entry.second);
      materializing_=false;if(!ok)return false;
      // Mark only the ranges actually transferred, retaining producer identity.
      for(auto range:entry.second)if(!owner_.prepare_cpu(range,[&](const lsw_deko::GuestSource& source){
        return source.key==entry.first;
      }))return false;
    }
    return true;
  }
  bool prepare(lsw_deko::GuestRange range){
    if(!range.valid()||range.end()>bytes_)return false;
    if(owner_.ram_current(range))return true;
    return prepare_ranges({range});
  }
  static void* begin(void* context,const void* host,size_t bytes,bool write) noexcept {
    auto& self=*static_cast<LswDekoResourceAccess*>(context);
    lsw_deko::GuestRange range;
    if(!lsw_deko::canonical_range(self.physical_,self.bytes_,host,bytes,range))return nullptr;
    self.lock();
    if(!self.prepare(range))stop("GPU materialization failed before CPU access");
    if(write)self.writes_.push_back(range);
    return &self;
  }
  static void* beginWrite(void* c,void* p,size_t n) noexcept {return begin(c,p,n,true);}
  static void* beginRead(void* c,const void* p,size_t n) noexcept {return begin(c,p,n,false);}
  static void endWrite(void* context,void*) noexcept {
    auto& self=*static_cast<LswDekoResourceAccess*>(context);
    if(self.writes_.empty())stop("unbalanced guest write scope");
    auto range=self.writes_.back();self.writes_.pop_back();
    if(!self.owner_.commit_cpu(range))stop("CPU mutation has no prepared ownership");
    self.producers_.prune(self.owner_);
    self.mutex_.unlock();
  }
  static void endRead(void* context,void*) noexcept {
    static_cast<LswDekoResourceAccess*>(context)->mutex_.unlock();
  }
public:
  LswDekoResourceAccess(uint8_t* physical,size_t bytes,Wait wait,Materialize materialize)
      :physical_(physical),bytes_(bytes),wait_(wait),materialize_(materialize),
       page_bits_(watchWords(bytes)),
       pages_{reinterpret_cast<uintptr_t>(physical),bytes,page_bits_.data()},
       observer_{this,beginWrite,endWrite,beginRead,endRead} {
    if(!physical||!bytes||bytes>0x20000000ull||!wait||!materialize)stop("invalid resource-access initialization");
    // Fixed interest before writers start; never add watch bits behind a writer.
    for(auto& bits:page_bits_)bits.store(~uint64_t(0),std::memory_order_relaxed);
  }
  void lock() {
    if(mutex_.try_lock())return;
    wait_([](void* self){static_cast<LswDekoResourceAccess*>(self)->mutex_.lock();},this);
  }
  void unlock(){mutex_.unlock();}
  class Guard {
    LswDekoResourceAccess& access_;
  public:
    explicit Guard(LswDekoResourceAccess& access):access_(access){access_.lock();}
    ~Guard(){access_.unlock();}
    Guard(const Guard&)=delete;Guard& operator=(const Guard&)=delete;
  };
  const lsw_guest::WriteObserver* observer() const{return &observer_;}
  const lsw_guest::WatchedPages* watched_pages() const{return &pages_;}
  bool publish(LswDekoProducerRef producer) {
    Guard guard(*this);
    return writes_.empty()&&!materializing_&&producers_.publish(owner_,std::move(producer));
  }
  bool capture(lsw_deko::GuestRange range,std::vector<lsw_deko::GuestSource>& out) {
    Guard guard(*this);return owner_.capture(range,out);
  }
  bool texture_source(const LswGpuTextureLayout& layout,lsw_deko::TextureAuthority& authority,LswDekoProducerRef& producer) {
    Guard guard(*this);producer.reset();
    if(!lsw_deko::texture_authority(owner_,layout,authority))return false;
    if(authority.kind==lsw_deko::TextureAuthority::Gpu){producer=producers_.find(authority.key);return bool(producer);}
    return true;
  }
  bool texture_visible(const LswGpuTextureLayout& layout) {
    Guard guard(*this);
    std::vector<lsw_deko::GuestRange> ranges;
    if(!lsw_gpu_visit_texture_blocks(&layout,[](void* context,const LswGpuTextureBlock* block){
      static_cast<std::vector<lsw_deko::GuestRange>*>(context)->push_back({block->physical,block->bytes});return true;
    },&ranges)||ranges.empty())return false;
    std::sort(ranges.begin(),ranges.end(),[](auto a,auto b){return a.first<b.first;});
    size_t count=0;
    for(auto range:ranges){
      if(count&&range.first<=ranges[count-1].end())
        ranges[count-1].bytes=uint32_t(std::max(ranges[count-1].end(),range.end())-ranges[count-1].first);
      else ranges[count++]=range;
    }
    ranges.resize(count);return prepare_ranges(ranges);
  }
  // A declared allocation lifetime is not permission to discard pending GPU
  // contents. Preserve visibility first, then reset identity under this lock.
  // This does not implement the allocator or prove its overlap semantics.
  bool reset_lifetime(const void* host,size_t bytes) {
    lsw_deko::GuestRange range;if(!lsw_deko::canonical_range(physical_,bytes_,host,bytes,range))return true;
    Guard guard(*this);
    if(!writes_.empty()||!prepare(range)||!owner_.reset_allocation(range))return false;
    producers_.prune(owner_);return true;
  }
  bool make_visible(const void* host,size_t bytes) {
    lsw_deko::GuestRange range;if(!lsw_deko::canonical_range(physical_,bytes_,host,bytes,range))return true;
    Guard guard(*this);return prepare(range);
  }
};

// Production lifecycle: preparation installs no observer. Full source/view and
// lifetime integration must pass before publishing these callbacks to writers.
extern "C" void lsw_deko_resources_prepare(uint8_t* physical,size_t bytes);
extern "C" bool lsw_deko_capture_enter();
extern "C" void lsw_deko_capture_leave();
extern "C" void lsw_deko_allocation_lifetime(void* host,size_t bytes);
extern "C" void lsw_deko_allocation_commit(void* host,size_t bytes);
extern "C" void lsw_deko_capture_read(const void* host,size_t bytes);
// This migration gate opens only when both startup-published pointers match.
// No environment switch or recording hook may activate observation by itself.
LswDekoResourceAccess* lsw_deko_recording_resources();
class LswDekoCaptureGuard {
  bool held_;
public:
  LswDekoCaptureGuard():held_(lsw_deko_capture_enter()){}
  ~LswDekoCaptureGuard(){if(held_)lsw_deko_capture_leave();}
  LswDekoCaptureGuard(const LswDekoCaptureGuard&)=delete;
  LswDekoCaptureGuard& operator=(const LswDekoCaptureGuard&)=delete;
};
