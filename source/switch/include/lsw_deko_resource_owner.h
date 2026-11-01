// Recording-side authority for canonical physical backing ranges. This is not
// an image cache: producer keys refer to immutable resolve metadata, including
// layout/format and command order. A texture view is a separate interpretation.
//
// The integration must serialize ALL access/capture through one reentrant lock,
// establish watch interest before writers start and supply real GPU writeback.
// This class deliberately does not bind the production observer by itself.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lsw_deko {
struct GuestRange {
  uint32_t first, bytes;
  uint64_t end() const { return uint64_t(first) + bytes; }
  bool valid() const { return bytes && end() <= 0x20000000ull; }
};

// Input is the actual folded host pointer, never an independently masked guest
// address. Native operations straddling backing boundaries are clipped safely.
inline bool canonical_range(const void* backing, size_t backing_bytes,
                            const void* host, size_t bytes, GuestRange& out) {
  if (!bytes || !backing_bytes || backing_bytes > 0x20000000ull) return false;
  auto first = reinterpret_cast<uintptr_t>(backing);
  auto p = reinterpret_cast<uintptr_t>(host);
  if (p < first) {
    uintptr_t gap = first - p;
    if (bytes <= gap) return false;
    bytes -= gap; p = first;
  }
  uintptr_t offset = p - first;
  if (offset >= backing_bytes) return false;
  bytes = std::min(bytes, backing_bytes - offset);
  out = {uint32_t(offset), uint32_t(bytes)};
  return true;
}

enum class GuestProducer { Cpu, Gpu };
struct GuestSource {
  GuestRange range;
  GuestProducer kind;
  uint64_t version, key;
  bool ram_current;
};

class ResourceOwner {
  // Only GPU-owned spans need persistent entries. CPU bytes are snapshotted at
  // capture; a conservative global CPU epoch avoids a per-store interval tree
  // over the whole 512MB backing. Unrelated CPU writes may change its identity,
  // which is safe: immutable uploads still deduplicate by exact owned bytes.
  std::vector<GuestSource> gpu_;
  uint64_t sequence_ = 0, cpu_version_ = 0;

  static bool same(const GuestSource& a, const GuestSource& b) {
    return a.kind == b.kind && a.version == b.version && a.key == b.key &&
           a.ram_current == b.ram_current;
  }
  void replace(GuestRange range, const GuestSource* replacement) {
    if (!replacement) {
      auto it = std::lower_bound(gpu_.begin(), gpu_.end(), range.first,
        [](const auto& source, uint32_t first) { return source.range.end() <= first; });
      if (it == gpu_.end() || it->range.first >= range.end()) return;
    }
    std::vector<GuestSource> next;
    next.reserve(gpu_.size() + 3);
    for (auto source : gpu_) {
      if (source.range.end() <= range.first || source.range.first >= range.end()) {
        next.push_back(source); continue;
      }
      if (source.range.first < range.first) {
        auto left = source; left.range.bytes = range.first - source.range.first;
        next.push_back(left);
      }
      if (source.range.end() > range.end()) {
        auto right = source;
        right.range = {uint32_t(range.end()), uint32_t(source.range.end() - range.end())};
        next.push_back(right);
      }
    }
    if (replacement) next.push_back(*replacement);
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
      return a.range.first < b.range.first;
    });
    gpu_.clear();
    for (const auto& source : next) {
      if (!gpu_.empty() && gpu_.back().range.end() == source.range.first &&
          same(gpu_.back(), source)) gpu_.back().range.bytes += source.range.bytes;
      else gpu_.push_back(source);
    }
  }
public:
  uint64_t sequence() const { return sequence_; }
  size_t gpu_spans() const { return gpu_.size(); }
  bool retains_gpu_key(uint64_t key) const {
    return key && std::any_of(gpu_.begin(),gpu_.end(),[&](const auto& source){return source.key==key;});
  }
  // Common CPU access must not allocate just to discover no pending GPU bytes.
  bool ram_current(GuestRange range) const {
    if(!range.valid())return false;
    auto first=std::lower_bound(gpu_.begin(),gpu_.end(),range.first,
      [](const auto& source,uint32_t address){return source.range.end()<=address;});
    for(;first!=gpu_.end()&&first->range.first<range.end();++first)
      if(!first->ram_current)return false;
    return true;
  }

  // Copy complete ordered dependencies, including mixed CPU/GPU regions and
  // interior offsets. Never select a producer by base address alone. Returned
  // values survive subsequent writes; the caller owns the producer metadata.
  bool capture(GuestRange range, std::vector<GuestSource>& out) const {
    if (!range.valid()) return false;
    out.clear();
    uint32_t cursor = range.first;
    for (auto source : gpu_) {
      if (source.range.end() <= cursor) continue;
      if (source.range.first >= range.end()) break;
      if (source.range.first > cursor)
        out.push_back({{cursor, source.range.first - cursor}, GuestProducer::Cpu,
                       cpu_version_, 0, true});
      uint32_t end = uint32_t(std::min(source.range.end(), range.end()));
      source.range = {std::max(cursor, source.range.first), 0};
      source.range.bytes = end - source.range.first;
      out.push_back(source); cursor = end;
    }
    if (cursor < range.end())
      out.push_back({{cursor, uint32_t(range.end() - cursor)}, GuestProducer::Cpu,
                     cpu_version_, 0, true});
    return true;
  }

  // Call after recording an ordered GPU producer, even before submission.
  // Written ranges must be exact guest byte spans (for example tiled blocks or
  // linear rows), not a bounding box including untouched pitch padding. Capture
  // previous dependencies first if the executor needs to initialize its image.
  // One key/version covers all rows of a resolve. Key zero is reserved for RAM.
  bool publish_gpu(const std::vector<GuestRange>& written, uint64_t key) {
    if (!key || written.empty() || sequence_ == UINT64_MAX) return false;
    for (auto range : written) if (!range.valid()) return false;
    uint64_t version = ++sequence_;
    for (auto range : written) {
      GuestSource source{range, GuestProducer::Gpu, version, key, false};
      replace(range, &source);
    }
    return true;
  }

  // Runs BEFORE a CPU read or mutation, while ownership remains locked. The
  // callback must execute/wait any unsubmitted command prefix and encode the
  // producer's actual guest layout into precisely this RAM span. It must use a
  // raw writeback, without reentering the observer/owner or recording new work.
  // False prevents the caller's CPU access; it is never permission to use RAM.
  // Materialization changes visibility only, not the producer/version identity.
  template<class Materialize>
  bool prepare_cpu(GuestRange range, Materialize&& materialize) {
    if (!range.valid()) return false;
    if (ram_current(range)) return true;
    std::vector<GuestSource> sources;
    if (!capture(range, sources)) return false;
    for (auto source : sources) {
      if (source.ram_current) continue;
      if (!materialize(source)) return false;
      source.ram_current = true;
      replace(source.range, &source);
    }
    return true;
  }

  // Call AFTER the actual CPU mutation, under the SAME lock as prepare_cpu.
  // Same-byte stores and conservatively failed atomics still advance identity.
  // A failed precondition is an integration error; it must stop validation.
  bool commit_cpu(GuestRange written) {
    if (!written.valid() || sequence_ == UINT64_MAX) return false;
    for (const auto& source : gpu_)
      if (source.range.first < written.end() && source.range.end() > written.first &&
          !source.ram_current) return false;
    cpu_version_ = ++sequence_;
    replace(written, nullptr);
    return true;
  }

  // Allocation lifetime change ONLY, after readers/writers are quiesced. Old
  // contents may be discarded by that contract. Never use for a normal write.
  bool reset_allocation(GuestRange range) {
    if (!range.valid() || sequence_ == UINT64_MAX) return false;
    cpu_version_ = ++sequence_;
    replace(range, nullptr);
    return true;
  }
};
} // namespace lsw_deko
