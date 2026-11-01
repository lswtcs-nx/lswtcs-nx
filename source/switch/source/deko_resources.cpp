#include "lsw_deko_resources.h"
extern "C" void lswtcs_gil_resource_wait(void (*fn)(void*),void* arg);
namespace {
// Process lifetime: guest workers are currently detached. A static C++
// destructor cannot safely free their lock domain during process-wide exit.
// A future drained-runtime shutdown may release it; repeated preparation fails.
LswDekoResourceAccess* resource_access=nullptr;
std::atomic<bool> prepared{false};
[[noreturn]] void invalid(const char* reason){
  fprintf(stderr,"[DEKO_RESOURCE] %s\n",reason);fflush(stderr);std::abort();
}
}
extern "C" void lsw_deko_resources_prepare(uint8_t* physical,size_t bytes){
  if(prepared.load(std::memory_order_acquire))invalid("resource preparation repeated");
  resource_access=new LswDekoResourceAccess(physical,bytes,lswtcs_gil_resource_wait,lsw_deko_materialize_producer_ranges);
  // Deliberately do not set lsw_guest::write_observer/watched_pages here.
  prepared.store(true,std::memory_order_release);
}
extern "C" bool lsw_deko_capture_enter(){
  if(!prepared.load(std::memory_order_acquire))return false;
  resource_access->lock();return true;
}
extern "C" void lsw_deko_capture_leave(){resource_access->unlock();}
extern "C" void lsw_deko_allocation_lifetime(void* host,size_t bytes){
  if(prepared.load(std::memory_order_acquire)&&!resource_access->reset_lifetime(host,bytes))invalid("allocation lifetime visibility failed");
}
extern "C" void lsw_deko_allocation_commit(void* host,size_t bytes){
  if(prepared.load(std::memory_order_acquire)&&!resource_access->make_visible(host,bytes))invalid("allocation commit visibility failed");
}
extern "C" void lsw_deko_capture_read(const void* host,size_t bytes){
  if(prepared.load(std::memory_order_acquire)&&!resource_access->make_visible(host,bytes))invalid("native capture visibility failed");
}
LswDekoResourceAccess* lsw_deko_recording_resources(){
  if(!prepared.load(std::memory_order_acquire))return nullptr;
  return lsw_guest::write_observer==resource_access->observer()&&
      lsw_guest::watched_pages==resource_access->watched_pages()?resource_access:nullptr;
}
