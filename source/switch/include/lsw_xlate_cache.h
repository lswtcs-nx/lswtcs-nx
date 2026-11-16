// Guest-shader translations saved on the SD card (gl_xlate_cache.cc), so a shader translated in an earlier session
// is not translated again. Xenos microcode -> SPIR-V -> GLSL costs about 18-58 ms per shader on the Switch CPU, and
// an area's first frames ask for dozens at once: the hub's 46 new shaders made an 820-890 ms frame in runs 111-113,
// every session, because gl_bridge.cc kept translations for the session only. Used by gl_bridge.cc's get_glsl, on
// the thread that prepares draws.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "lsw_gl_shader.h"

// Which translation: the guest microcode and the translator modification.
struct LswXlateId {
  uint64_t ucode_xxh;      // XXH3 of the guest (big-endian) microcode: Xenia's name for it, as in [SHADERNEW]
  uint64_t ucode_hash;     // Shader::ucode_data_hash (gl_bridge.cc hash_ucode, with the pixel-shader bit)
  uint64_t modification;
  uint32_t dwords;
  uint32_t stage;          // LswGlStage
};

struct LswXlateEntry {
  std::string glsl;
  std::vector<LswGlShaderUnit> units;
  // The translator's bindings for this translation (SpirvShader::TextureBinding / SamplerBinding, packed):
  // textures: fetch constant | dimension << 5 | signed << 7; samplers: fetch | mag << 5 | min << 7 | mip << 9 | aniso << 11.
  std::vector<uint32_t> textures, samplers;
};

// Loads sdmc:/lswtcs/glprogs2/xlate.bin. A file from another build (the NRO's build ID) or with other translation
// settings is started over. LSWTCS_GLXLATE=0 turns the cache off.
void lsw_xlate_cache_open();
// The saved translation of `id`, if any.
bool lsw_xlate_cache_find(const LswXlateId& id, LswXlateEntry& out);
// Whether this hit is also translated and compared: the first LSWTCS_GLXLATECHECK hits of a session (default 8).
bool lsw_xlate_cache_check_next();
// Result of such a comparison. A difference turns the cache off for the session and starts the file over.
void lsw_xlate_cache_checked(const LswXlateId& id, const LswXlateEntry& saved, const LswXlateEntry& fresh);
// Saves a new translation (written by a background thread).
void lsw_xlate_cache_put(const LswXlateId& id, const LswXlateEntry& e);
