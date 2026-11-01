// Ordered access to the production executor. The caller owns/serializes the
// borrowed frame and guest destination until return. Do not call from the GPU
// thread. Recording must finish every record through 'end' before lending it;
// keep the arena and earlier records intact (tile_repeat may refer to them).
#pragma once
#include "lsw_gpu_frame.h"
#include "lsw_deko_resource_owner.h"
#include "lsw_deko_resolve_memory.h"
#include <map>
#include <memory>

struct LswDekoFrameExecution;
struct LswDekoResolvedImage;
// A producer's command identity and guest interpretation are immutable. The
// opaque native image has independently fenced last-use bookkeeping. Keeping
// this handle never keeps a mutable frame/arena or a guest RAM pointer alive.
struct LswDekoProducer {
  const uint64_t key,serial;
  const uint32_t record,width,height;
  const lsw_deko::ResolveMemory memory;
  const std::shared_ptr<LswDekoResolvedImage> image;
};
using LswDekoProducerRef=std::shared_ptr<const LswDekoProducer>;

// Caller serializes this registry with its ResourceOwner. Owner spans name
// producer keys; frame/texture references may keep older producers after the
// owner no longer needs them. Pruning is logical release, not GPU destruction.
class LswDekoProducers {
  std::map<uint64_t,LswDekoProducerRef> producers_;
public:
  LswDekoProducerRef find(uint64_t key) const {
    auto it=producers_.find(key);return it==producers_.end()?LswDekoProducerRef{}:it->second;
  }
  bool publish(lsw_deko::ResourceOwner& owner,LswDekoProducerRef producer) {
    if(!producer||!producer->key||!producer->image||!producer->memory.resolve.copy||
       producers_.count(producer->key))return false;
    std::vector<lsw_deko::GuestRange> written;
    if(!lsw_deko::resolve_written_ranges(producer->memory,written)||written.empty())return false;
    if(!owner.publish_gpu(written,producer->key))return false;
    prune(owner);producers_.emplace(producer->key,std::move(producer));return true;
  }
  void prune(const lsw_deko::ResourceOwner& owner) {
    for(auto it=producers_.begin();it!=producers_.end();)
      if(!owner.retains_gpu_key(it->first))it=producers_.erase(it);else ++it;
  }
  size_t size() const {return producers_.size();}
};
// Execute only the as-yet unexecuted prefix, without presentation. The frame's
// execution state moves with it through build/present/render and resets on clear.
bool lsw_deko_execute_prefix(LswGpuFrame& frame,uint32_t end);
// Execute the required prefix, wait for a fenced copy of this resolve's OWN
// retained image version, then encode only its intersection with wanted.
// Returns false before guest mutation on invalid layout/readback/failed fence.
bool lsw_deko_materialize_resolve(LswGpuFrame& frame,uint32_t record,
    uint8_t* physical,size_t physicalBytes,lsw_deko::GuestRange wanted);
// Execute the required prefix and retain its exact result. No readback or GPU
// fence is required just to capture an image dependency. Do not call on GPU thread.
bool lsw_deko_capture_resolve(LswGpuFrame& frame,uint32_t record,LswDekoProducerRef& out);
// Materialize this exact version even after its original frame has been reused.
bool lsw_deko_materialize_producer(LswDekoProducerRef producer,
    uint8_t* physical,size_t physicalBytes,lsw_deko::GuestRange wanted);
// One fenced image transfer, with exact disjoint guest spans. Padding and bytes
// now owned by a newer CPU/GPU producer must survive composition unchanged.
bool lsw_deko_materialize_producer_ranges(LswDekoProducerRef producer,
    uint8_t* physical,size_t physicalBytes,const std::vector<lsw_deko::GuestRange>& wanted);
