"""Checks lsw_glsl_fold.h on the shapes SPIRV-Cross prints, and that the values it moves to the CPU equal what the GLSL computed.

No game data or GPU is needed: the text transform is compared with its expected output, and lsw_fetch_derived with a C++ copy of the
GLSL expressions over many fetch words. Pass --corpus DIR to also run it over a folder of captured shaders and report what stays unfolded.
"""
from pathlib import Path
import argparse,json,subprocess,tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler',default='clang++',help='Already installed C++17 compiler executable')
parser.add_argument('--corpus',type=Path,help='Folder of .frag and .vert files, kept out of the report by name')
args=parser.parse_args()
base=Path(__file__).resolve().parents[1]
source=r'''
#include "lsw_glsl_fold.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
// The expressions the fold replaces, as GLSL evaluates them.
static int32_t sext(uint32_t v, int offset, int bits) { return int32_t(v << (32 - offset - bits)) >> (32 - bits); }
static uint32_t uext(uint32_t v, int offset, int bits) { return (v >> offset) & ((1u << bits) - 1u); }
static int fails = 0;
static void same(const std::string& what, const std::string& got, const std::string& want) {
  if (got != want) { ++fails; printf("FAIL %s\n got:  %s\n want: %s\n", what.c_str(), got.c_str(), want.c_str()); }
}
static const std::string kHead = "layout(binding = 4, std140) uniform XeFetchConstants_ps\n{\n    uvec4 fetch_constants[48];\n} xe_uniform_fetch_constants;\n\n";
static const std::string kDecl = "layout(binding = 6, std140) uniform LswFetchDerived\n{\n    vec4 lsw_fetch_derived[32];\n};\n\n";
int main(int argc, char** argv) {
  const std::string fc = "xe_uniform_fetch_constants.fetch_constants";
  // Fetch slot 0: words 0..5 are fetch_constants[0].xyzw, [1].xy. Slot 2: words 12..17 are [3].xyzw, [4].xy.
  same("size x", lsw_glsl_fold_fetch(kHead + "float a = 0.00146484375 / float(bitfieldExtract(" + fc + "[0].z, int(0u), int(13u)) + 1u);\n"),
       kDecl + kHead + "float a = 0.00146484375 * lsw_fetch_derived[0].x;\n");
  same("size y", lsw_glsl_fold_fetch(kHead + "float a = 0.5 / float(bitfieldExtract(" + fc + "[3].z, int(13u), int(13u)) + 1u);\n"),
       kDecl + kHead + "float a = 0.5 * lsw_fetch_derived[2].y;\n");
  same("lod", lsw_glsl_fold_fetch(kHead + "float a = float(bitfieldExtract(int(" + fc + "[1].x), int(12u), int(10u))) * 0.03125;\n"),
       kDecl + kHead + "float a = lsw_fetch_derived[0].z;\n");
  same("exponent", lsw_glsl_fold_fetch(kHead + "float a = ldexp(1.0, bitfieldExtract(int(" + fc + "[3].w), int(13u), int(6u)));\n"),
       kDecl + kHead + "float a = lsw_fetch_derived[2].w;\n");
  // Left alone: another word, other bits, a division into the LOD, and a shader without the block.
  for (const std::string& text : {
         "float a = 1.0 / float(bitfieldExtract(" + fc + "[0].w, int(0u), int(13u)) + 1u);\n",       // word 3, not the size
         "float a = 1.0 / float(bitfieldExtract(" + fc + "[0].z, int(0u), int(11u)) + 1u);\n",       // 3D size bits
         "float a = b / float(bitfieldExtract(int(" + fc + "[1].x), int(12u), int(10u))) * 0.03125;\n",
         "float a = float(bitfieldExtract(int(" + fc + "[1].x), int(0u), int(10u))) * 0.03125;\n",
         "float a = ldexp(2.0, bitfieldExtract(int(" + fc + "[0].w), int(13u), int(6u)));\n",
         "uint a = bitfieldExtract(" + fc + "[1].y, int(9u), int(2u));\n"}) {
    same("left alone", lsw_glsl_fold_fetch(kHead + text), kHead + text);
  }
  const std::string no_block = "float a = 1.0;\n";
  same("no block", lsw_glsl_fold_fetch(no_block), no_block);
  const std::string once = lsw_glsl_fold_fetch(kHead + "float a = ldexp(1.0, bitfieldExtract(int(" + fc + "[0].w), int(13u), int(6u)));\n");
  same("idempotent", lsw_glsl_fold_fetch(once), once);
  // Division on the left of the size stays correct: x / y / n == x / y * (1 / n).
  same("left division", lsw_glsl_fold_fetch(kHead + "float a = x / y / float(bitfieldExtract(" + fc + "[0].z, int(0u), int(13u)) + 1u);\n"),
       kDecl + kHead + "float a = x / y * lsw_fetch_derived[0].x;\n");
  // Values: as the GLSL computed them.
  uint32_t state = 12345;
  auto next = [&] { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
  for (int i = 0; i < 200000; ++i) {
    uint32_t w[6];
    for (uint32_t& v : w) v = next();
    float d[4];
    lsw_fetch_derived(w, d);
    const float width = float(uext(w[2], 0, 13) + 1u), height = float(uext(w[2], 13, 13) + 1u);
    // The size goes through a reciprocal: at most a unit in the last place of a rounding offset.
    const float offset = 0.00146484375f;
    const float dx = std::fabs(offset * d[0] - offset / width), dy = std::fabs(offset * d[1] - offset / height);
    if (dx > 2e-7f * offset / width || dy > 2e-7f * offset / height) { ++fails; printf("FAIL size %08X\n", w[2]); break; }
    if (d[2] != float(sext(w[4], 12, 10)) * 0.03125f) { ++fails; printf("FAIL lod %08X\n", w[4]); break; }
    if (d[3] != std::ldexp(1.0f, sext(w[3], 13, 6))) { ++fails; printf("FAIL exponent %08X\n", w[3]); break; }
  }
  // The optional corpus.
  int files = 0, changed = 0;
  size_t left = 0;
  for (int a = 1; a < argc; ++a) {
    std::ifstream in(argv[a], std::ios::binary);
    std::stringstream text; text << in.rdbuf();
    const std::string before = text.str(), after = lsw_glsl_fold_fetch(before);
    ++files;
    if (after != before) ++changed;
    size_t at = 0;
    while ((at = after.find("bitfieldExtract(int(xe_uniform_fetch", at)) != std::string::npos) { ++left; ++at; }
    at = 0;
    while ((at = after.find("float(bitfieldExtract(xe_uniform_fetch", at)) != std::string::npos) { ++left; ++at; }
    if (lsw_glsl_fold_fetch(after) != after) { ++fails; printf("FAIL not idempotent: file %d\n", files); }
    if (after.find("lsw_fetch_derived") != std::string::npos && after.find("LswFetchDerived") == std::string::npos) { ++fails; printf("FAIL no block: file %d\n", files); }
  }
  printf("{\"fails\": %d, \"corpus_files\": %d, \"corpus_changed\": %d, \"corpus_unfolded\": %zu}\n", fails, files, changed, left);
  return fails ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tcs360-fetch-fold-') as temporary:
    out=Path(temporary)
    (out/'test.cpp').write_text(source,encoding='utf-8')
    exe=out/'test.exe'
    subprocess.run([args.compiler,'-std=c++17','-O1','-D_CRT_SECURE_NO_WARNINGS','-I',str(base/'source/switch/include'),str(out/'test.cpp'),'-o',str(exe)],check=True)
    corpus=[str(p) for p in sorted(args.corpus.glob('*')) if p.suffix in ('.frag','.vert')] if args.corpus else []
    # Stay under the command line limit on Windows: one short corpus is enough for this report.
    run=subprocess.run([str(exe)]+corpus[:300],capture_output=True,text=True)
    print(run.stdout,end='')
    print(json.dumps({'passed':run.returncode==0}))
    raise SystemExit(run.returncode)
