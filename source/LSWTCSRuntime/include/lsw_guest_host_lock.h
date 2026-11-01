// Native locks whose holder may wait for observed guest-memory visibility must
// not block the cooperative guest turn. Use the existing mandatory host wait.
#pragma once
#ifdef LSW_GUEST_MEMORY_TRACKING
extern "C" void lswtcs_gil_resource_wait(void (*fn)(void*),void* arg);
#endif
namespace lsw_guest {
template<class Lock> inline void lock_host(Lock& lock) {
#ifdef LSW_GUEST_MEMORY_TRACKING
  if(lock.try_lock())return;
  lswtcs_gil_resource_wait([](void* value){static_cast<Lock*>(value)->lock();},&lock);
#else
  lock.lock();
#endif
}
}
