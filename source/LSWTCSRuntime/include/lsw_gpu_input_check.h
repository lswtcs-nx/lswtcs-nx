#pragma once
// Temporary opt-in input-reuse diagnostic. No rendering or resource ownership policy.
#include "lsw_gpu_bridge.h"
#include <cstdint>

struct LswGpuInputSignature {
  uint64_t state=0, vertices=0, indices=0, system=0, bytes=0;
  uint32_t status=0; // 0 absent, 1 complete, 2 invalid input, 3 hashing budget exhausted
};
struct LswGpuInputMismatch {
  uint32_t record=~0u, reference=~0u, source=0, difference=0;
  LswGpuInputSignature expected{}, actual{};
};
inline uint32_t lsw_gpu_input_difference(const LswGpuInputSignature& a,
                                        const LswGpuInputSignature& b, bool system) {
  if(a.status!=1 || b.status!=1)return 0; // incomplete is unknown, never equal-proof
  return (a.state!=b.state?1u:0u)|(a.vertices!=b.vertices?2u:0u)|
      (a.indices!=b.indices?4u:0u)|(system && a.system!=b.system?8u:0u);
}
inline bool lsw_gpu_input_note(LswGpuInputMismatch& out,uint32_t record,uint32_t reference,
    uint32_t source,const LswGpuInputSignature& expected,const LswGpuInputSignature& actual,bool system) {
  const uint32_t difference=lsw_gpu_input_difference(expected,actual,system);
  if(out.difference || !difference)return false;
  out={record,reference,source,difference,expected,actual};return true;
}

// Hasher is supplied by the recorder, including the existing guest-read boundary.
// Preflight all ranges and cost BEFORE reading any sample data.
template<class Hash>
LswGpuInputSignature lsw_gpu_input_signature(const uint32_t* regs,const LswGpuDraw& d,
    uint32_t initiator,uint32_t dma_base,uint32_t dma_size,const uint8_t* inline_idx,
    uint32_t inline_words,const uint8_t* phys,uint64_t phys_size,uint64_t budget,Hash hash) {
  LswGpuInputSignature s{};s.status=2;
  if(!regs || !phys || d.vfetch_count>96 || d.vs_float_count>256 || d.ps_float_count>256 ||
      d.vs_tex_count>32 || d.ps_tex_count>32 || d.vs_smp_count>32 || d.ps_smp_count>32 ||
      d.system_constants_size>2048 || !d.fetch || !d.bool_loop ||
      (d.vs_float_count && !d.vs_float)||(d.ps_float_count && !d.ps_float)||
      (d.system_constants_size && !d.system_constants))return s;
  const uint32_t src=(initiator>>6)&3,esize=(initiator&(1u<<11))?4u:2u;
  uint32_t ibytes=src<=1?(initiator>>16)*esize:0;
  if(src==1 && uint64_t(ibytes)>uint64_t(inline_words)*4)ibytes=inline_words*4;
  const uint32_t ip=(dma_base&0x1fffffffu)&~(esize-1);
  if((src==0 && uint64_t(ip)+ibytes>phys_size)||(src==1 && ibytes && !inline_idx))return s;
  uint64_t cost=uint64_t(d.vs_float_count+d.ps_float_count)*16+160+768+d.system_constants_size+ibytes;
  cost+=16+25*4+12+uint64_t(d.vfetch_count)*8+
      uint64_t(d.vs_tex_count+d.ps_tex_count)*sizeof(LswGpuTexBinding)+
      uint64_t(d.vs_smp_count+d.ps_smp_count)*sizeof(LswGpuSamplerBinding);
  for(uint32_t i=0;i<d.vfetch_count;++i) {
    const uint32_t p=d.vfetch_addr[i]&0x1fffffffu,n=d.vfetch_size[i];
    if(!n || n>0x1000000u || uint64_t(p)+n>phys_size)return s;
    cost+=n;
  }
  s.bytes=cost;if(cost>budget){s.status=3;return s;}
  uint64_t h=0x6c7377746373696eull;
  const uint64_t shaders[]={d.vs_key,d.ps_key};h=hash(shaders,sizeof shaders,h);
  const uint32_t fields[]={initiator,dma_size,d.gs_key_bits,d.ps_writes_depth,d.ps_color_targets,
    d.vs_float_count,d.ps_float_count,d.vs_tex_count,d.ps_tex_count,d.vs_smp_count,d.ps_smp_count,
    regs[0x2000],regs[0x2001],regs[0x2002],regs[0x2003],regs[0x2004],regs[0x2005],
    regs[0x2200],regs[0x2201],regs[0x2209],regs[0x220a],regs[0x220b],regs[0x2104],
    regs[0x210d],regs[0x210c],regs[0x2205]};
  h=hash(fields,sizeof fields,h);
  h=hash(d.vs_float,size_t(d.vs_float_count)*16,h);h=hash(d.ps_float,size_t(d.ps_float_count)*16,h);
  h=hash(d.bool_loop,160,h);h=hash(d.fetch,768,h);
  h=hash(d.vs_tex,size_t(d.vs_tex_count)*sizeof(LswGpuTexBinding),h);
  h=hash(d.ps_tex,size_t(d.ps_tex_count)*sizeof(LswGpuTexBinding),h);
  h=hash(d.vs_smp,size_t(d.vs_smp_count)*sizeof(LswGpuSamplerBinding),h);
  s.state=hash(d.ps_smp,size_t(d.ps_smp_count)*sizeof(LswGpuSamplerBinding),h);
  h=0x766572746578ull;
  for(uint32_t i=0;i<d.vfetch_count;++i) {
    const uint32_t span[]={d.vfetch_addr[i]&0x1fffffffu,d.vfetch_size[i]};
    h=hash(span,sizeof span,h);h=hash(phys+span[0],span[1],h);
  }
  s.vertices=h;
  const uint32_t index[]={src,src==0?ip:0u,ibytes};
  h=hash(index,sizeof index,0x696e646578ull);
  s.indices=ibytes?hash(src==0?phys+ip:inline_idx,ibytes,h):h;
  s.system=hash(d.system_constants,d.system_constants_size,0x737973ull);s.status=1;
  return s;
}

extern "C" void lswtcs_inputcheck_captured(uint64_t serial);
