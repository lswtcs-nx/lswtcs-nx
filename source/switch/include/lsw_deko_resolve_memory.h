// A resolved host image is not guest RAM. This adapter packs the already
// resolved pixels into exact Xenos destination bytes. GPU completion and the
// producer's lifetime must be established by its caller before invoking it.
#pragma once
#include "lsw_deko_resolve_plan.h"
#include "lsw_deko_resource_owner.h"
#include "lsw_gpu_texture_layout.h"
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>

namespace lsw_deko {
struct ResolveMemory {
  ResolvePlan resolve;
  uint32_t pitch=0,height=0,slice=0,endianXor=0,guestFormat=0;
  bool array=false;
};

// Destination view identity is distinct from producer identity and EDRAM
// attachment identity. Unknown historical layouts cannot alias known ones.
using ResolveViewKey=std::array<uint32_t,12>;
inline ResolveViewKey resolve_view_key(uint32_t base,bool depth,uint32_t width,uint32_t height,const ResolveMemory* memory=nullptr){
  return {base,uint32_t(depth),width,height,memory?1u:0u,
    memory?memory->pitch:0,memory?memory->height:0,memory?uint32_t(memory->array):0,
    memory?memory->slice:0,memory?memory->endianXor:0,memory?memory->guestFormat:0,
    memory?uint32_t(memory->resolve.swap):0};
}

inline bool resolve_memory(const LswGpuDrawRec& record,const LswGpuResolveRegisters* raw,
                           ResolveMemory& out,std::string& error) {
  out={};
  if (!normalize_resolve(record,out.resolve,error)) return false;
  if (!out.resolve.copy) return true;
  if (!raw) {error="resolve guest layout is unknown (raw destination registers missing)";return false;}
  if (out.resolve.destination&3) {error="resolve destination is not word aligned";return false;}
  if ((raw->destination&0x1fffffffu)!=record.rs_dest ||
      (raw->pitch&0x3fff)!=record.rs_dest_pitch || ((raw->pitch>>16)&0x3fff)!=record.rs_dest_height ||
      ((raw->info>>7)&63)!=record.rs_dest_fmt || (raw->control&7)!=record.rs_src ||
      ((raw->control>>4)&7)!=record.rs_sample || ((raw->control>>20)&3)!=record.rs_copy_cmd ||
      ((raw->control>>8)&1)!=bool(record.rs_clear_color) || ((raw->control>>9)&1)!=bool(record.rs_clear_depth) ||
      (record.rs_src<4 && ((raw->info>>24)&1)!=bool(record.rs_dest_swap))) {
    error="resolve guest layout disagrees with recorded operation";return false;
  }
  unsigned endian=raw->info&7;
  if (endian>5) {error="resolve destination endian mode unsupported";return false;}
  constexpr unsigned masks[]={0,1,3,2,7,15};
  out.endianXor=masks[endian];out.array=(raw->info&(1u<<3))!=0;out.slice=(raw->info>>4)&7;
  out.pitch=(out.resolve.storagePitch+31)&~31u;
  out.height=(out.resolve.storageHeight+31)&~31u;
  if (out.array && !out.height) {error="array resolve destination height missing";return false;}
  out.guestFormat=out.resolve.depth?(out.resolve.depthFormat?23:22):out.resolve.destinationFormat;
  // The current executor produces RGBA8 without number-format/exponent-bias
  // conversion. Reject this required work until that executor contract exists.
  if (!out.resolve.depth && ((raw->info>>13)&0x1ff)) {
    error="resolve destination number/exponent conversion is not implemented";return false;
  }
  return true;
}

struct ResolveWord {uint32_t physical,x,y,byteXor;};
template<class Visit> bool visit_resolve_words(const ResolveMemory& memory,Visit&& visit) {
  const auto& op=memory.resolve;
  if (!op.copy) return true;
  const auto& rect=op.destinationRect;
  // Native copy destination pitch/height fields are 14-bit. Reject malformed
  // wider recorded coordinates before Xenia's signed 2D address arithmetic.
  if (!rect.width || !rect.height || (op.destination&3) || !memory.pitch || memory.pitch>0x4000 ||
      (memory.pitch&31) || uint64_t(rect.x)+rect.width>0x4000 ||
      uint64_t(rect.y)+rect.height>0x4000 || memory.endianXor>15 ||
      (memory.array && (!memory.height || memory.height>0x4000 || (memory.height&31) || memory.slice>7))) return false;
  for (uint32_t y=0;y<rect.height;++y) for (uint32_t x=0;x<rect.width;++x) {
    uint32_t dx=rect.x+x,dy=rect.y+y;
    int64_t offset=lsw_gpu_tiled_offset(dx,dy,memory.slice,memory.pitch,memory.height,2,memory.array);
    if (offset<0 || uint64_t(op.destination)+uint64_t(offset)+4>0x20000000ull) return false;
    uint32_t address=(op.destination+uint32_t(offset))^(memory.endianXor&~3u);
    if (uint64_t(address)+4>0x20000000ull) return false;
    if (!visit(ResolveWord{address,dx,dy,memory.endianXor&3u})) return false;
  }
  return true;
}

inline bool resolve_written_ranges(const ResolveMemory& memory,std::vector<GuestRange>& out) {
  std::vector<GuestRange> ranges;
  if (!visit_resolve_words(memory,[&](const ResolveWord& word) {
    ranges.push_back({word.physical,4});return true;
  })) return false;
  std::sort(ranges.begin(),ranges.end(),[](auto a,auto b){return a.first<b.first;});
  out.clear();
  for (auto range:ranges) {
    if (!out.empty() && range.first<=out.back().end()) {
      out.back().bytes=uint32_t(std::max(out.back().end(),range.end())-out.back().first);
    } else out.push_back(range);
  }
  return true;
}

inline uint32_t round_positive_even(double value) {
  auto integer=uint32_t(value);double fraction=value-double(integer);
  return integer+uint32_t(fraction>0.5 || (fraction==0.5 && (integer&1)));
}
inline uint32_t pack_depth24(float host,bool floating) {
  // Native float24 attachments use Xenia's half-range representation, matching
  // normalize_resolve's depth clear. Decode/encode values are independent of
  // the CPU thread's floating-point rounding mode.
  if (!(host>0)) return 0;
  if (!floating) {
    if (host>=1) return 0xffffff;
    return round_positive_even(double(host)*16777215.0);
  }
  double value=std::min(double(host)*2.0,2.0-std::ldexp(1.0,-20));
  if (value<std::ldexp(1.0,-14)) return round_positive_even(std::ldexp(value,34));
  int exponent=0;double fraction=std::frexp(value,&exponent);
  uint32_t mantissa=round_positive_even((fraction*2.0-1.0)*1048576.0);
  if (mantissa==0x100000) {mantissa=0;++exponent;}
  return std::min(0xffffffu,(uint32_t(exponent+14)<<20)|mantissa);
}

// Raw Deko readback formats: RGBA8 (4 bytes) or Z32F/S8/X24 (8 bytes).
// Source coordinates are the image's actual destination pixel coordinates.
// Only bytes inside 'wanted' AND the resolve's written pixels are changed.
// The callback's raw writes must bypass the guest observer to avoid recursion.
inline bool encode_resolve_guest_ranges(const ResolveMemory& memory,const void* readback,size_t bytes,
                                 uint32_t width,uint32_t height,uint32_t rowStride,
                                 uint8_t* guest,size_t guestBytes,std::vector<GuestRange> wanted) {
  const auto& op=memory.resolve;
  if (!op.copy) return true;
  uint32_t bpp=op.depth?8:4;
  const auto& rect=op.destinationRect;
  if (!readback || !guest || wanted.empty() ||
      uint64_t(rect.x)+rect.width>width || uint64_t(rect.y)+rect.height>height ||
      uint64_t(width)*bpp>rowStride || !height ||
      uint64_t(height-1)*rowStride+uint64_t(width)*bpp>bytes) return false;
  for(auto range:wanted)if(!range.valid()||range.end()>guestBytes)return false;
  std::sort(wanted.begin(),wanted.end(),[](auto a,auto b){return a.first<b.first;});
  size_t count=0;
  for(auto range:wanted){
    if(count&&range.first<=wanted[count-1].end())
      wanted[count-1].bytes=uint32_t(std::max(wanted[count-1].end(),range.end())-wanted[count-1].first);
    else wanted[count++]=range;
  }
  wanted.resize(count);
  // Preflight every address before mutating RAM. A failed encoding must not
  // publish a partly valid CPU image or accidentally modify unrelated bytes.
  if (!visit_resolve_words(memory,[&](const ResolveWord& word) {
    return uint64_t(word.physical)+4<=guestBytes;
  })) return false;
  auto* source=static_cast<const uint8_t*>(readback);
  return visit_resolve_words(memory,[&](const ResolveWord& word) {
    auto it=std::lower_bound(wanted.begin(),wanted.end(),word.physical,
      [](auto range,uint32_t first){return range.end()<=first;});
    unsigned mask=0;
    for(;it!=wanted.end()&&it->first<uint64_t(word.physical)+4;++it)
      for(unsigned b=0;b<4;++b)if(word.physical+b>=it->first&&word.physical+b<it->end())mask|=1u<<b;
    if(!mask)return true;
    const uint8_t* pixel=source+size_t(word.y)*rowStride+size_t(word.x)*bpp;
    uint8_t packed[4];
    if (op.depth) {
      float depth;std::memcpy(&depth,pixel,4);
      uint32_t value=(pack_depth24(depth,op.depthFormat!=0)<<8)|pixel[4];
      for (unsigned i=0;i<4;++i) packed[i]=uint8_t(value>>(8*i));
    } else {
      std::memcpy(packed,pixel,4);
      if (op.swap) std::swap(packed[0],packed[2]);
    }
    for (unsigned i=0;i<4;++i) {
      uint32_t address=word.physical+(i^word.byteXor);
      if (mask&(1u<<(i^word.byteXor))) guest[address]=packed[i];
    }
    return true;
  });
}
inline bool encode_resolve_guest(const ResolveMemory& memory,const void* readback,size_t bytes,
                                 uint32_t width,uint32_t height,uint32_t rowStride,
                                 uint8_t* guest,size_t guestBytes,GuestRange wanted) {
  return encode_resolve_guest_ranges(memory,readback,bytes,width,height,rowStride,guest,guestBytes,{wanted});
}
} // namespace lsw_deko
