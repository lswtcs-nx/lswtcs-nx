// Guest-shader translations saved on the SD card: see include/lsw_xlate_cache.h.
//
// sdmc:/lswtcs/glprogs2/xlate.bin: a header (magic, version) and then one record per translation, appended as the game
// asks for new ones. The version is a hash of the NRO's build ID (any rebuild starts the file over: the translator,
// SPIRV-Cross and the GLSL clean-up are all in the binary) and of the settings that change the GLSL. The game is closed
// from the HOME menu, so a record can be cut short: loading stops at the first record whose length or checksum is
// wrong and keeps the ones before it.
#include "lsw_xlate_cache.h"

#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"

#ifdef __SWITCH__
// libnx's entry point (crt0), the module's first byte: NroStart, then the NRO header, which the loader maps with the
// rest of the code. Not switch.ld's __start__: an absolute symbol (0) in this position-independent binary, read through
// a GOT slot with no relocation, so it would point at address 0. Hidden: addressed PC-relative (adrp/add).
extern "C" __attribute__((visibility("hidden"))) char _start[];
#endif

namespace {

// Relative to sdmc:/lswtcs, the working directory (gl_backend.cpp LSW_PROGLIST_DIR, whose boot scan reads only
// <16 hex digits>.glsl3 names there).
constexpr char kDir[] = "glprogs2";
constexpr char kPath[] = "glprogs2/xlate.bin";
constexpr char kTmp[] = "glprogs2/xlate.tmp";
constexpr char kMagic[8] = {'L', 'S', 'W', 'X', 'L', 'A', 'T', '1'};
constexpr uint32_t kRecordMagic = 0x3152584Cu;   // "LXR1"
constexpr uint32_t kMaxRecord = 4u << 20;
constexpr size_t kMaxFile = 64u << 20;           // beyond this the file is started over at the next boot

// Settings read by the translator, SPIRV-Cross's clean-up and the fetch folding (gl_bridge.cc bridge() and get_glsl,
// spirv_shader_translator_fetch.cc, lsw_glsl_fold.h). A new setting that changes the GLSL belongs here.
const char* const kSettings[] = {"LSWTCS_GLVIEWSWZ", "LSWTCS_GLFMZ", "LSWTCS_GLTEXBIAS", "LSWTCS_GLFWDCF",
                                 "LSWTCS_GLFWDLOOPS", "LSWTCS_GLPSPRECISE", "LSWTCS_GLFETCHFOLD", "LSWTCS_VFETCHCLAMP"};

struct FileHead { char magic[8]; uint64_t version; };
struct RecordHead { uint32_t magic, size; uint64_t check; };   // check: XXH3 of the payload
struct RecordFixed {                                            // payload start; then GLSL, units, textures, samplers
  LswXlateId id;
  uint32_t glsl, units, textures, samplers;
};
static_assert(sizeof(LswXlateId) == 32, "LswXlateId has padding");
static_assert(sizeof(RecordFixed) == 48, "RecordFixed has padding");
static_assert(sizeof(LswGlShaderUnit) == 9 * 4, "LswGlShaderUnit has padding");

bool g_on = false;      // loaded and usable
bool g_bad = false;     // a check found a difference: no more hits this session
uint32_t g_checks_left = 0;
std::string g_data;                                // the file as loaded (read-only after lsw_xlate_cache_open)
std::unordered_map<uint64_t, size_t> g_index;      // id key -> offset of its record in g_data
size_t g_file_size = 0;                            // bytes in the file, including what the writer appended

// Writer thread.
std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<std::string> g_queue;
bool g_reset = false;

uint64_t id_key(const LswXlateId& id) { return XXH3_64bits(&id, sizeof id); }

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The NRO header's build ID (elf2nro copies the ELF's --build-id=sha1 note there).
bool nro_build_id(uint8_t out[32]) {
#ifdef __SWITCH__
  const char* h = _start + 0x10;
  if (std::memcmp(h, "NRO0", 4) != 0) return false;
  std::memcpy(out, h + 0x30, 32);
  for (int i = 0; i < 32; ++i)
    if (out[i]) return true;
#endif
  return false;
}

std::string file_head(uint64_t version) {
  FileHead h;
  std::memcpy(h.magic, kMagic, sizeof h.magic);
  h.version = version;
  return std::string(reinterpret_cast<const char*>(&h), sizeof h);
}

// `start`: written as the whole file before appending (the header, plus the good records when the old file ended
// in a cut record); empty = append to the file as it is.
void writer_main(std::string start, uint64_t version) {
  auto replace = [](const std::string& bytes) {
    FILE* t = fopen(kTmp, "wb");
    if (!t) return false;
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), t) == bytes.size();
    fclose(t);
    if (!ok) { remove(kTmp); return false; }
    remove(kPath);   // the SD card's rename does not replace (gl_backend.cpp program_order_save)
    if (rename(kTmp, kPath) != 0) { remove(kTmp); return false; }
    return true;
  };
  if (!start.empty() && !replace(start)) {
    printf("[XLATECACHE] could not write %s: nothing is saved this session\n", kPath);
    fflush(stdout);
    return;
  }
  std::string().swap(start);
  FILE* f = fopen(kPath, "ab");
  if (!f) {
    printf("[XLATECACHE] could not open %s: nothing is saved this session\n", kPath);
    fflush(stdout);
    return;
  }
  bool full = false;
  for (;;) {
    std::string rec;
    bool reset = false;
    {
      std::unique_lock<std::mutex> lk(g_mtx);
      g_cv.wait(lk, [] { return g_reset || !g_queue.empty(); });
      if (g_reset) {
        g_reset = false;
        reset = true;
      } else {
        rec = std::move(g_queue.front());
        g_queue.pop_front();
      }
    }
    if (reset) {
      fclose(f);
      replace(file_head(version));
      g_file_size = sizeof(FileHead);
      full = false;
      if (!(f = fopen(kPath, "ab"))) return;
      continue;
    }
    if (full) continue;
    if (g_file_size + rec.size() > kMaxFile) {
      full = true;
      printf("[XLATECACHE] %s reached %zu MB: no more translations saved this session\n", kPath, kMaxFile >> 20);
      fflush(stdout);
      continue;
    }
    // One record per write, flushed: closing the game from the HOME menu loses at most the record being written.
    if (fwrite(rec.data(), 1, rec.size(), f) == rec.size() && fflush(f) == 0) g_file_size += rec.size();
  }
}

// Reads the record at `off` of g_data; false if it is cut short or damaged. `r` = its fixed part, `next` = the offset
// after it.
bool parse_record(size_t off, size_t& next, RecordFixed& r) {
  if (g_data.size() - off < sizeof(RecordHead)) return false;
  RecordHead h;
  std::memcpy(&h, g_data.data() + off, sizeof h);
  if (h.magic != kRecordMagic || h.size < sizeof(RecordFixed) || h.size > kMaxRecord ||
      g_data.size() - off - sizeof h < h.size)
    return false;
  const char* p = g_data.data() + off + sizeof h;
  if (XXH3_64bits(p, h.size) != h.check) return false;
  std::memcpy(&r, p, sizeof r);
  const uint64_t need = sizeof(RecordFixed) + uint64_t(r.glsl) + uint64_t(r.units) * sizeof(LswGlShaderUnit) +
                        (uint64_t(r.textures) + r.samplers) * 4u;
  if (need > h.size) return false;
  next = off + sizeof h + h.size;
  return true;
}

}  // namespace

void lsw_xlate_cache_open() {
  const char* e = getenv("LSWTCS_GLXLATE");
  if (e && e[0] == '0') {
    printf("[XLATECACHE] off (LSWTCS_GLXLATE=0)\n");
    fflush(stdout);
    return;
  }
  const char* c = getenv("LSWTCS_GLXLATECHECK");
  g_checks_left = c ? uint32_t(atoi(c)) : 8u;
  uint8_t build_id[32];
  if (!nro_build_id(build_id)) {
    printf("[XLATECACHE] off: no build ID in the NRO header\n");
    fflush(stdout);
    return;
  }
  std::string v(reinterpret_cast<const char*>(build_id), sizeof build_id);
  for (const char* n : kSettings) {
    const char* x = getenv(n);
    v += n;
    v += '=';
    v += x ? x : "(unset)";
    v += '\n';
  }
  const uint64_t version = XXH3_64bits(v.data(), v.size());

  const auto t0 = std::chrono::steady_clock::now();
  mkdir(kDir, 0777);
  const char* reason = nullptr;   // why the file is started over
  if (FILE* f = fopen(kPath, "rb")) {
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < long(sizeof(FileHead))) {
      reason = "too short";
    } else if (size_t(size) > kMaxFile) {
      reason = "too large";
    } else {
      g_data.resize(size_t(size));
      if (fread(&g_data[0], 1, g_data.size(), f) != g_data.size()) reason = "read error";
    }
    fclose(f);
  } else {
    reason = "none yet";
  }
  if (!reason) {
    FileHead h;
    std::memcpy(&h, g_data.data(), sizeof h);
    if (std::memcmp(h.magic, kMagic, sizeof h.magic) != 0) reason = "not a translation cache";
    else if (h.version != version) reason = "another build or other translation settings";
  }
  std::string start;   // what the writer writes as the new file first; empty = append
  size_t good = 0;
  if (reason) {
    std::string().swap(g_data);
    start = file_head(version);
    g_file_size = start.size();
  } else {
    size_t off = sizeof(FileHead), next = 0;
    RecordFixed r;
    while (parse_record(off, next, r)) {
      g_index[id_key(r.id)] = off;
      off = next;
    }
    good = off;
    if (good != g_data.size()) start = g_data.substr(0, good);   // drop the cut record
    g_file_size = good;
  }
  printf("[XLATECACHE] %s: %zu translations (%zu KB) in %.1f ms%s%s%s%s\n", kPath, g_index.size(), good >> 10,
         ms_since(t0), reason ? ", started over (" : "", reason ? reason : "", reason ? ")" : "",
         !reason && good != g_data.size() ? ", a cut record at the end dropped" : "");
  fflush(stdout);
  g_on = true;
  std::thread(writer_main, std::move(start), version).detach();
}

bool lsw_xlate_cache_find(const LswXlateId& id, LswXlateEntry& out) {
  if (!g_on || g_bad) return false;
  const auto it = g_index.find(id_key(id));
  if (it == g_index.end()) return false;
  const char* p = g_data.data() + it->second + sizeof(RecordHead);
  RecordFixed r;
  std::memcpy(&r, p, sizeof r);
  if (std::memcmp(&r.id, &id, sizeof id) != 0) return false;
  p += sizeof r;
  out.glsl.assign(p, r.glsl);
  p += r.glsl;
  out.units.resize(r.units);
  std::memcpy(out.units.data(), p, r.units * sizeof(LswGlShaderUnit));
  p += r.units * sizeof(LswGlShaderUnit);
  out.textures.resize(r.textures);
  std::memcpy(out.textures.data(), p, r.textures * 4u);
  p += r.textures * 4u;
  out.samplers.resize(r.samplers);
  std::memcpy(out.samplers.data(), p, r.samplers * 4u);
  return true;
}

bool lsw_xlate_cache_check_next() {
  if (!g_checks_left) return false;
  --g_checks_left;
  return true;
}

void lsw_xlate_cache_checked(const LswXlateId& id, const LswXlateEntry& saved, const LswXlateEntry& fresh) {
  const char* diff = nullptr;
  if (saved.glsl != fresh.glsl) diff = "GLSL";
  else if (saved.units.size() != fresh.units.size() ||
           std::memcmp(saved.units.data(), fresh.units.data(), saved.units.size() * sizeof(LswGlShaderUnit)) != 0)
    diff = "texture units";
  else if (saved.textures != fresh.textures) diff = "texture bindings";
  else if (saved.samplers != fresh.samplers) diff = "sampler bindings";
  if (!diff) {
    printf("[XLATECACHE] check %016llX_%016llX: same as translated\n", (unsigned long long)id.ucode_xxh,
           (unsigned long long)id.modification);
    fflush(stdout);
    return;
  }
  printf("[XLATECACHE] check %016llX_%016llX: the saved translation differs (%s): not used for the rest of the "
         "session, file started over\n", (unsigned long long)id.ucode_xxh, (unsigned long long)id.modification, diff);
  fflush(stdout);
  g_bad = true;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_reset = true;
  }
  g_cv.notify_one();
}

void lsw_xlate_cache_put(const LswXlateId& id, const LswXlateEntry& e) {
  if (!g_on) return;
  RecordFixed r;
  r.id = id;
  r.glsl = uint32_t(e.glsl.size());
  r.units = uint32_t(e.units.size());
  r.textures = uint32_t(e.textures.size());
  r.samplers = uint32_t(e.samplers.size());
  std::string payload(reinterpret_cast<const char*>(&r), sizeof r);
  payload += e.glsl;
  payload.append(reinterpret_cast<const char*>(e.units.data()), e.units.size() * sizeof(LswGlShaderUnit));
  payload.append(reinterpret_cast<const char*>(e.textures.data()), e.textures.size() * 4u);
  payload.append(reinterpret_cast<const char*>(e.samplers.data()), e.samplers.size() * 4u);
  if (payload.size() > kMaxRecord) return;
  RecordHead h{kRecordMagic, uint32_t(payload.size()), XXH3_64bits(payload.data(), payload.size())};
  std::string rec(reinterpret_cast<const char*>(&h), sizeof h);
  rec += payload;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_queue.push_back(std::move(rec));
  }
  g_cv.notify_one();
}
