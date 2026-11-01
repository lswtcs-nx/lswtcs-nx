// Display requests belong to consumed PM4 swaps, independently of later host
// presentation calls. A value copy survives register changes and frame swaps.
#pragma once
#include <cstdint>
struct LswGpuSwap {
  uint32_t physical=0,fetch[6]{};
  bool recorded=false;
};
inline LswGpuSwap lsw_gpu_record_swap(uint32_t physical,const uint32_t* fetch){
  LswGpuSwap swap;swap.physical=physical;swap.recorded=true;
  for(unsigned i=0;i<6;++i)swap.fetch[i]=fetch[i];
  return swap;
}
inline bool lsw_gpu_swap_valid(const LswGpuSwap& swap){
  return swap.recorded&&(swap.fetch[0]&3)==2&&
      (swap.fetch[1]&0x1ffff000u)==(swap.physical&0x1fffffffu);
}
