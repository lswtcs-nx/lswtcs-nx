// Host driver for tests/xlate_cache_check.py: runs the Switch build's translation cache
// (source/switch/source/gl_xlate_cache.cc) on a PC, one "session" per process, in the current directory.
//
//   xlate_cache_check <build id byte> put        saves translations A and B
//   xlate_cache_check <build id byte> find       finds A and B and checks every field; then A is no longer found
//                                                after a check that differs (and the file is started over)
//   xlate_cache_check <build id byte> none       expects neither to be found
//
// The cache reads the build ID from the NRO header at _start + 0x10 (libnx crt0); here it is this array.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "lsw_xlate_cache.h"

extern "C" char _start[0x80];
char _start[0x80];

namespace {

LswXlateId id_a() { return {0x1111222233334444ull, 0x8000000055556666ull, 0x40000ull, 214u, kLswGlPixel}; }
LswXlateId id_b() { return {0x7777888899990000ull, 0x0000AAAABBBBCCCCull, 0x12ull, 88u, kLswGlVertex}; }

LswXlateEntry entry_a() {
  LswXlateEntry e;
  e.glsl = "#version 430\nvoid main() { /* A */ }\n";
  for (uint32_t i = 0; i < 2; ++i) {
    LswGlShaderUnit u;
    u.unit = i;
    u.fetch_constant = 3 + i;
    u.dimension = 1;
    u.is_signed = i;
    u.sampler = {3 + i, 1, 1, 2, 0};
    e.units.push_back(u);
  }
  e.textures = {3u | 1u << 5, 4u | 1u << 5 | 1u << 7, 9u | 3u << 5};
  e.samplers = {3u | 1u << 5 | 1u << 7 | 2u << 9};
  return e;
}

LswXlateEntry entry_b() {
  LswXlateEntry e;
  e.glsl = "#version 430\n" + std::string(50000, 'b') + "\n";   // a large shader
  return e;
}

bool same(const LswXlateEntry& x, const LswXlateEntry& y) {
  return x.glsl == y.glsl && x.textures == y.textures && x.samplers == y.samplers && x.units.size() == y.units.size() &&
         std::memcmp(x.units.data(), y.units.data(), x.units.size() * sizeof(LswGlShaderUnit)) == 0;
}

void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(400)); }   // the writer thread

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  std::memcpy(_start + 0x10, "NRO0", 4);
  std::memset(_start + 0x40, std::atoi(argv[1]), 20);   // build ID
  const std::string mode = argv[2];
  lsw_xlate_cache_open();
  if (mode == "put") {
    lsw_xlate_cache_put(id_a(), entry_a());
    lsw_xlate_cache_put(id_b(), entry_b());
    settle();
    printf("RESULT put\n");
    return 0;
  }
  LswXlateEntry a, b;
  const bool fa = lsw_xlate_cache_find(id_a(), a), fb = lsw_xlate_cache_find(id_b(), b);
  if (mode == "none") {
    settle();
    printf("RESULT none %s\n", !fa && !fb ? "ok" : "FOUND");
    return !fa && !fb ? 0 : 1;
  }
  if (mode != "find") return 2;
  LswXlateId other = id_a();
  other.modification ^= 1;   // same microcode, another modification
  LswXlateEntry o;
  const bool fo = lsw_xlate_cache_find(other, o);
  const bool ok = fa && fb && !fo && same(a, entry_a()) && same(b, entry_b());
  const bool c1 = lsw_xlate_cache_check_next(), c2 = lsw_xlate_cache_check_next(), c3 = lsw_xlate_cache_check_next();
  lsw_xlate_cache_checked(id_b(), b, entry_b());   // same: stays on
  const bool still = lsw_xlate_cache_find(id_a(), a);
  LswXlateEntry changed = entry_a();
  changed.textures[1] ^= 1u << 7;
  lsw_xlate_cache_checked(id_a(), a, changed);     // differs: off for the session, file started over
  const bool after = lsw_xlate_cache_find(id_b(), b);
  settle();
  printf("RESULT find %s checks %d%d%d still %d after %d\n", ok ? "ok" : "WRONG", c1, c2, c3, still, after);
  return ok && c1 && c2 && !c3 && still && !after ? 0 : 1;
}
