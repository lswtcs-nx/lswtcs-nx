// Fetch-constant values a texture fetch recomputes per pixel, computed once per draw on the CPU instead.
//
// Xenia's translator re-derives them from the fetch constant with bitfield extracts: the coordinate rounding offset (a division
// by the texture's size), the LOD bias and the exponent scale. They are constant within a draw, and the heaviest pixel shaders
// of run 2 do 22 extracts, 10 divisions and 5 ldexp per pixel for them (`A2D183873C322D09`). Here the GLSL reads them from a
// uniform block of its own instead (binding 6). It is done on the text, not in the translator, so the program list saved on the
// SD card by earlier sessions can be rewritten the same way at boot and keeps finding its programs ready.
//
// Only the exact shapes SPIRV-Cross prints are replaced; any other use of the fetch constants stays as it was.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

constexpr uint32_t kLswFetchDerivedBinding = 6, kLswFetchDerivedBytes = 32 * 4 * sizeof(float);

// The four values of one fetch constant (6 words). Words 0 and 1 of a vertex fetch can sit in the same slot, and no texture reads those.
inline void lsw_fetch_derived(const uint32_t* words, float out[4]) {
  const uint32_t width = (words[2] & 0x1FFFu) + 1u, height = ((words[2] >> 13) & 0x1FFFu) + 1u;
  const int32_t lod = int32_t(words[4] << 10) >> 22;         // bits 12:21, signed
  const int32_t exponent = int32_t(words[3] << 13) >> 26;    // bits 13:18, signed
  out[0] = 1.0f / float(width);
  out[1] = 1.0f / float(height);
  out[2] = float(lod) * 0.03125f;
  out[3] = std::ldexp(1.0f, exponent);
}

namespace lsw_fold_detail {
constexpr char kFetch[] = "xe_uniform_fetch_constants.fetch_constants[";
inline bool at(const std::string& s, size_t p, const char* text) { return s.compare(p, std::strlen(text), text) == 0; }
// "xe_uniform_fetch_constants.fetch_constants[N].c" at p: returns the dword index of the fetch block, or -1. end = just after it.
inline int word_at(const std::string& s, size_t p, size_t& end) {
  if (!at(s, p, kFetch)) return -1;
  p += sizeof kFetch - 1;
  const size_t digits = p;
  int vec = 0;
  while (p < s.size() && std::isdigit(uint8_t(s[p]))) vec = vec * 10 + (s[p++] - '0');
  if (p == digits || !at(s, p, "].") || p + 2 >= s.size()) return -1;
  const char* const swizzle = "xyzw";
  const char* c = std::strchr(swizzle, s[p + 2]);
  if (!c || !*c || p + 3 >= s.size() || std::isalnum(uint8_t(s[p + 3])) || s[p + 3] == '_') return -1;
  end = p + 3;
  return vec * 4 + int(c - swizzle);
}
inline char previous_char(const std::string& s, size_t p) {
  while (p > 0 && s[p - 1] == ' ') --p;
  return p ? s[p - 1] : '\0';
}
}  // namespace lsw_fold_detail

// Returns the source with those expressions replaced and the block declared. Idempotent. LSWTCS_GLFETCHFOLD=0 leaves it alone.
inline std::string lsw_glsl_fold_fetch(const std::string& src) {
  using namespace lsw_fold_detail;
  static const bool on = [] { const char* e = std::getenv("LSWTCS_GLFETCHFOLD"); return !(e && e[0] == '0'); }();
  if (!on || src.find("LswFetchDerived") != std::string::npos) return src;
  const size_t block = src.find("XeFetchConstants");
  if (block == std::string::npos) return src;
  std::string out;
  out.reserve(src.size());
  size_t copied = 0, cursor = 0;
  bool any = false;
  char field[64];
  auto replace = [&](size_t from, size_t to, int word, char component) {
    out.append(src, copied, from - copied);
    std::snprintf(field, sizeof field, "lsw_fetch_derived[%d].%c", word / 6, component);
    out += field;
    copied = cursor = to;
    any = true;
  };
  static const char kExtract[] = "bitfieldExtract(";
  for (;;) {
    const size_t e = src.find(kExtract, cursor);
    if (e == std::string::npos) break;
    cursor = e + sizeof kExtract - 1;
    size_t arg = cursor, end = 0;
    const bool signed_arg = at(src, arg, "int(");
    if (signed_arg) arg += 4;
    const int word = word_at(src, arg, end);
    if (word < 0) continue;
    if (signed_arg) { if (!at(src, end, ")")) continue; ++end; }
    if (!signed_arg) {
      // <literal> / float(bitfieldExtract(FC, int(0u), int(13u)) + 1u): width; int(13u), int(13u): height.
      const bool x = at(src, end, ", int(0u), int(13u)) + 1u)"), y = at(src, end, ", int(13u), int(13u)) + 1u)");
      static const char kDiv[] = " / float(";
      const size_t div_len = sizeof kDiv - 1;
      if ((!x && !y) || word % 6 != 2 || e < div_len || !at(src, e - div_len, kDiv)) continue;
      // x / n is x * (1 / n) whatever stands on the left: the two operators have the same precedence.
      out.append(src, copied, e - div_len - copied);
      out += " * ";
      copied = e;
      const size_t to = end + (x ? std::strlen(", int(0u), int(13u)) + 1u)") : std::strlen(", int(13u), int(13u)) + 1u)"));
      replace(e, to, word, x ? 'x' : 'y');
    } else if (at(src, end, ", int(12u), int(10u))) * 0.03125") && word % 6 == 4) {
      // float(bitfieldExtract(int(FC), int(12u), int(10u))) * 0.03125: LOD bias.
      static const char kFloat[] = "float(";
      const size_t from = e - (sizeof kFloat - 1);
      if (e < sizeof kFloat - 1 || !at(src, from, kFloat)) continue;
      const char before = previous_char(src, from);
      if (before == '/' || before == '*' || before == '%') continue;
      replace(from, end + std::strlen(", int(12u), int(10u))) * 0.03125"), word, 'z');
    } else if (at(src, end, ", int(13u), int(6u)))") && word % 6 == 3) {
      // ldexp(1.0, bitfieldExtract(int(FC), int(13u), int(6u))): exponent scale.
      static const char kLdexp[] = "ldexp(1.0, ";
      const size_t from = e - (sizeof kLdexp - 1);
      if (e < sizeof kLdexp - 1 || !at(src, from, kLdexp)) continue;
      replace(from, end + std::strlen(", int(13u), int(6u)))"), word, 'w');
    }
  }
  if (!any) return src;
  out.append(src, copied, std::string::npos);
  // One name in every stage, unlike the translator's blocks: a program with it in both stages has one block, not two claiming one member.
  char decl[128];
  std::snprintf(decl, sizeof decl, "layout(binding = %u, std140) uniform LswFetchDerived\n{\n    vec4 lsw_fetch_derived[32];\n};\n\n", kLswFetchDerivedBinding);
  const size_t layout = out.rfind("layout(", out.find("XeFetchConstants"));
  out.insert(layout == std::string::npos ? 0 : layout, decl);
  return out;
}
