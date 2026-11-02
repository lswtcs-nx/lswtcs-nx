"""Checks lsw_png_write.h: chunk framing, CRCs, filter Up and pixels, read back with Python's zlib.

No game data or GPU is needed. A PC has no zlib to link here, so the test supplies a small zlib.h whose deflate emits stored blocks, in pieces
that end on awkward output sizes. That exercises the encoder around zlib, and Python's zlib checks the stream; zlib's own compression is not under test.
"""
from pathlib import Path
import argparse,json,random,struct,subprocess,tempfile,zlib

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler',default='clang++',help='Already installed C++17 compiler executable')
args=parser.parse_args()
base=Path(__file__).resolve().parents[1]
fake_zlib=r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
typedef unsigned int uInt;
typedef unsigned char Bytef;
struct z_stream { Bytef* next_in; uInt avail_in; Bytef* next_out; uInt avail_out; void* state; };
enum { Z_OK = 0, Z_STREAM_END = 1, Z_BUF_ERROR = -5, Z_NO_FLUSH = 0, Z_FINISH = 4 };
inline uint32_t crc32(uint32_t crc, const unsigned char* p, uInt n) {
  if (!p) return 0;
  crc = ~crc;
  while (n--) { crc ^= *p++; for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
  return ~crc;
}
struct FakeZ { std::vector<unsigned char> in, out; size_t sent = 0; bool built = false; };
inline int deflateInit(z_stream* z, int) { z->state = new FakeZ; return Z_OK; }
inline int deflateEnd(z_stream* z) { delete static_cast<FakeZ*>(z->state); return Z_OK; }
inline int deflate(z_stream* z, int flush) {
  FakeZ& s = *static_cast<FakeZ*>(z->state);
  s.in.insert(s.in.end(), z->next_in, z->next_in + z->avail_in);
  z->next_in += z->avail_in;
  z->avail_in = 0;
  if (flush != Z_FINISH) return Z_OK;
  if (!s.built) {
    s.built = true;
    s.out = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (unsigned char c : s.in) { a = (a + c) % 65521; b = (b + a) % 65521; }
    size_t pos = 0;
    do {
      const size_t n = s.in.size() - pos < 65535 ? s.in.size() - pos : 65535;
      const unsigned char head[5] = {(unsigned char)(pos + n == s.in.size()), (unsigned char)n, (unsigned char)(n >> 8), (unsigned char)~n, (unsigned char)(~n >> 8)};
      s.out.insert(s.out.end(), head, head + 5);
      s.out.insert(s.out.end(), s.in.begin() + pos, s.in.begin() + pos + n);
      pos += n;
    } while (pos < s.in.size());
    const uint32_t ad = (b << 16) | a;
    for (int k = 3; k >= 0; --k) s.out.push_back((unsigned char)(ad >> (8 * k)));
  }
  while (z->avail_out && s.sent < s.out.size()) { *z->next_out++ = s.out[s.sent++]; --z->avail_out; }
  return s.sent == s.out.size() ? Z_STREAM_END : Z_OK;
}
'''
source=r'''
#include "lsw_png_write.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc, char** argv) {
  const uint32_t w = strtoul(argv[2], nullptr, 10), h = strtoul(argv[3], nullptr, 10);
  std::vector<uint8_t> px(size_t(w) * h * 4);
  FILE* in = fopen(argv[4], "rb");
  if (!px.empty() && fread(px.data(), 1, px.size(), in) != px.size()) return 2;
  fclose(in);
  FILE* f = fopen(argv[1], "wb");
  size_t pieces = 0;
  const bool ok = lsw_png_encode(px.data(), w, h, [&](const uint8_t* d, size_t n) { ++pieces; return fwrite(d, 1, n, f) == n; });
  fclose(f);
  // A sink that gives up must stop the encoder without a crash.
  size_t calls = 0;
  const bool stopped = !lsw_png_encode(px.data(), w, h, [&](const uint8_t*, size_t) { ++calls; return false; });
  printf("%d %zu %d %zu\n", ok, pieces, stopped, calls);
  return 0;
}
'''

def read_png(data):
    assert data[:8]==bytes([137,80,78,71,13,10,26,10]),'signature'
    pos=8;idat=b'';seen=[];sizes=[]
    while pos<len(data):
        size,kind=struct.unpack('>I4s',data[pos:pos+8])
        body=data[pos+8:pos+8+size]
        crc=struct.unpack('>I',data[pos+8+size:pos+12+size])[0]
        assert crc==zlib.crc32(kind+body),f'crc of {kind}'
        seen.append(kind)
        if kind==b'IHDR': w,h,depth,colour=struct.unpack('>IIBB',body[:10])
        if kind==b'IDAT': idat+=body;sizes.append(size)
        pos+=12+size
    assert pos==len(data) and seen[0]==b'IHDR' and seen[-1]==b'IEND','chunk order'
    assert depth==8 and colour==6,'format'
    raw=zlib.decompress(idat)
    stride=w*4
    assert len(raw)==(stride+1)*h,'raw size'
    out=bytearray();above=bytes(stride)
    for y in range(h):
        line=raw[y*(stride+1):(y+1)*(stride+1)]
        assert line[0]==2,'filter'
        now=bytes((line[1+x]+above[x])&255 for x in range(stride))
        out+=now;above=now
    return w,h,bytes(out),sizes

results=[]
rng=random.Random(360)
cases=[(1,1,'noise'),(3,2,'noise'),(64,64,'flat'),(1280,90,'noise'),(640,360,'gradient')]
with tempfile.TemporaryDirectory(prefix='tcs360-png-') as temporary:
    out=Path(temporary)
    (out/'zlib.h').write_text(fake_zlib,encoding='utf-8')
    (out/'test.cpp').write_text(source,encoding='utf-8')
    exe=out/'test.exe'
    subprocess.run([args.compiler,'-std=c++17','-O1','-D_CRT_SECURE_NO_WARNINGS','-I',str(out),'-I',str(base/'source/switch/include'),str(out/'test.cpp'),'-o',str(exe)],check=True)
    for w,h,kind in cases:
        if kind=='noise': pixels=rng.randbytes(w*h*4)
        elif kind=='flat': pixels=bytes([9,8,7,255])*(w*h)
        else: pixels=bytes((x*y+c)&255 for y in range(h) for x in range(w) for c in range(4))
        (out/'in.bin').write_bytes(pixels)
        run=subprocess.run([str(exe),str(out/'t.png'),str(w),str(h),str(out/'in.bin')],capture_output=True,text=True,check=True)
        ok,pieces,stopped,calls=[int(v) for v in run.stdout.split()]
        rw,rh,back,sizes=read_png((out/'t.png').read_bytes())
        passed=bool(ok and stopped and calls==1 and (rw,rh)==(w,h) and back==pixels and all(s<=256*1024 for s in sizes) and pieces==len(sizes))
        results.append({'w':w,'h':h,'kind':kind,'pieces':pieces,'passed':passed})
print(json.dumps({'cases':results,'passed':all(r['passed'] for r in results)}))
raise SystemExit(0 if all(r['passed'] for r in results) else 1)
