// Private offline DKSH cache. Wire integers are little-endian (Windows / Switch).
// No compilation or replacement shader is attempted on a cache miss.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsw_deko {
struct CacheHeader {
  char magic[8];
  uint32_t version,count;
  uint64_t bytes;
  uint8_t compilerId[32];
};
struct CacheEntry {
  uint64_t key,offset,bytes,codeHash,sourceHash;
  uint32_t stage,bindingCount; // LswGlStage: vertex=0, pixel=1, geometry=2
  uint8_t slots[48]; // Original GL binding -> compact Deko slot, 255=unused.
};
static_assert(sizeof(CacheHeader)==56 && sizeof(CacheEntry)==96,"cache ABI");
inline uint64_t cache_hash(const void* data,size_t bytes){
  uint64_t value=14695981039346656037ull;const auto* p=static_cast<const uint8_t*>(data);
  for(size_t i=0;i<bytes;++i)value=(value^p[i])*1099511628211ull;
  return value;
}
class ShaderCache {
  std::vector<uint8_t> storage;
  std::vector<CacheEntry> entries;
 public:
  bool load(const char* path,const uint8_t compilerId[32],std::string& error){
    storage.clear();entries.clear();error.clear();
    auto fail=[&](const char* why){storage.clear();entries.clear();error=why;return false;};
    FILE* file=fopen(path,"rb");if(!file)return fail("offline DKSH cache missing");
    if(fseek(file,0,SEEK_END)){fclose(file);return fail("cache seek failed");}
    long n=ftell(file);rewind(file);
    if(n<long(sizeof(CacheHeader))||n>64*1024*1024){fclose(file);return fail("cache size invalid");}
    storage.resize(size_t(n));bool read=fread(storage.data(),1,storage.size(),file)==storage.size();
    fclose(file);if(!read)return fail("cache read failed");
    CacheHeader header;memcpy(&header,storage.data(),sizeof header);
    if(memcmp(header.magic,"LSDKSH01",8)||header.version!=1||header.bytes!=storage.size()||
       !header.count||header.count>8192)return fail("cache header invalid");
    if(memcmp(header.compilerId,compilerId,32))return fail("cache compiler/converter identity differs");
    uint64_t tableEnd=sizeof(CacheHeader)+uint64_t(header.count)*sizeof(CacheEntry);
    if(tableEnd>storage.size())return fail("cache table truncated");
    entries.resize(header.count);memcpy(entries.data(),storage.data()+sizeof(CacheHeader),entries.size()*sizeof(CacheEntry));
    uint64_t previousEnd=tableEnd;
    for(size_t i=0;i<entries.size();++i){const auto& e=entries[i];
      if(e.stage>2||e.bindingCount>32||e.offset%256||e.offset<previousEnd||e.offset>storage.size()||
         e.bytes<64||e.bytes>1024*1024||e.bytes>storage.size()-e.offset)return fail("cache entry range/stage invalid");
      if(i&&(entries[i-1].stage>e.stage||(entries[i-1].stage==e.stage&&entries[i-1].key>=e.key)))return fail("cache entries not unique and sorted");
      uint32_t seen=0,count=0;
      for(uint8_t slot:e.slots)if(slot!=255){if(slot>=32||(seen&(1u<<slot)))return fail("cache binding map invalid");seen|=1u<<slot;++count;}
      if(count!=e.bindingCount||(count<32&&seen!=((1u<<count)-1)))return fail("cache binding map incomplete");
      const auto* code=storage.data()+e.offset;
      if(memcmp(code,"DKSH",4)||cache_hash(code,size_t(e.bytes))!=e.codeHash)return fail("cache shader checksum invalid");
      previousEnd=e.offset+e.bytes;
    }
    if(previousEnd!=storage.size())return fail("cache trailing data");
    return true;
  }
  const CacheEntry* find(uint64_t key,uint32_t stage,const void* source,size_t sourceBytes)const{
    size_t low=0,high=entries.size();
    while(low<high){size_t mid=low+(high-low)/2;const auto& e=entries[mid];
      if(e.stage<stage||(e.stage==stage&&e.key<key))low=mid+1;else high=mid;}
    if(low==entries.size()||entries[low].stage!=stage||entries[low].key!=key)return nullptr;
    return entries[low].sourceHash==cache_hash(source,sourceBytes)?&entries[low]:nullptr;
  }
  const uint8_t* code(const CacheEntry& e)const{return storage.data()+e.offset;}
  size_t count()const{return entries.size();}
};
} // namespace lsw_deko
