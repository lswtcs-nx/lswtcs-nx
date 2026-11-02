// Explicit guest-access boundary for generated accesses and native writers.
// The observer owns range filtering, synchronization and CPU/GPU authority.
// Bind only before guest threads start; drain them before unbinding/destruction.
// Pointer arguments must be the ACTUAL folded host pointers (PPC_HOST/lsw_host),
// so aliases refer to the same backing. This header does not guess guest ranges.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <type_traits>
#include <utility>

namespace lsw_guest {
struct WriteObserver {
  void* context;
  // Called before any bytes are changed. May synchronize/materialize a newer
  // GPU producer and lock its ownership. Null token means an untracked write.
  void* (*begin)(void* context, void* host, size_t bytes) noexcept;
  // Called after the actual write while ownership remains locked. Commit the
  // CPU version here, then release ownership. Failed atomics may conservatively
  // invalidate; a write of identical bytes MUST still be observed.
  void (*end)(void* context, void* token) noexcept;
  // Reads need the same visibility boundary as partial writes. Optional only
  // for a write-only diagnostic observer; a coherent owner must supply both.
  void* (*begin_read)(void* context, const void* host, size_t bytes) noexcept = nullptr;
  void (*end_read)(void* context, void* token) noexcept = nullptr;
};
inline const WriteObserver* write_observer = nullptr;

// Publish this map and the observer before starting guest threads. The map's
// backing and observer live until those threads drain. A set page is a
// conservative interest flag, never an ownership/version decision. The owner
// still checks exact ranges under its lock. Establish interest before guest
// threads start (the owner may initially watch the entire physical backing).
// Adding interest later requires quiescing writers: setting a bit alone cannot
// observe a native bulk write that already passed the filter. Leaving bits set
// is safe. Non-guest SIMD/register and native host-buffer accesses bypass.
struct WatchedPages {
  uintptr_t first;
  size_t bytes;
  const std::atomic<uint64_t>* words; // one bit per 4096-byte page
};
inline const WatchedPages* watched_pages = nullptr;
inline bool watched(const void* host, size_t bytes) noexcept {
  if (!bytes) return false;
  const auto* map = watched_pages;
  if (!map) return true; // observer can do its own filtering (tests/diagnostics)
  uintptr_t p = reinterpret_cast<uintptr_t>(host);
  if (p < map->first) {
    uintptr_t gap = map->first - p;
    if (bytes <= gap) return false;
    bytes -= gap; p = map->first;
  }
  uintptr_t offset = p - map->first;
  if (offset >= map->bytes) return false;
  bytes = bytes < map->bytes - offset ? bytes : map->bytes - offset;
  size_t begin = offset >> 12, end = (offset + bytes - 1) >> 12;
  size_t word = begin >> 6, last = end >> 6;
  uint64_t mask = ~uint64_t(0) << (begin & 63);
  for (;;) {
    if (word == last) mask &= ~uint64_t(0) >> (63 - (end & 63));
    if (map->words[word].load(std::memory_order_acquire) & mask) return true;
    if (word == last) return false;
    ++word; mask = ~uint64_t(0);
  }
}

#if defined(__SWITCH__) && !defined(LSW_GUEST_MEMORY_TRACKING)
// The Mesa build binds no observer, so a scope has nothing to do, and it was still a call per access: the walker's register
// reads, and ~11 % of the audio thread's samples in the native reverb (run 8).
class WriteScope {
public:
  WriteScope(void*, size_t) noexcept {}
  WriteScope(const WriteScope&) = delete;
  WriteScope& operator=(const WriteScope&) = delete;
};
class ReadScope {
public:
  ReadScope(const void*, size_t) noexcept {}
  ReadScope(const ReadScope&) = delete;
  ReadScope& operator=(const ReadScope&) = delete;
};
#else
class WriteScope {
  const WriteObserver* observer_;
  void* token_;
public:
  WriteScope(void* host, size_t bytes) noexcept
      : observer_(write_observer), token_(nullptr) {
    if (observer_ && watched(host, bytes)) token_ = observer_->begin(observer_->context, host, bytes);
  }
  ~WriteScope() {
    if (token_) observer_->end(observer_->context, token_);
  }
  WriteScope(const WriteScope&) = delete;
  WriteScope& operator=(const WriteScope&) = delete;
};

class ReadScope {
  const WriteObserver* observer_;
  void* token_;
public:
  ReadScope(const void* host, size_t bytes) noexcept
      : observer_(write_observer), token_(nullptr) {
    if (observer_ && observer_->begin_read && watched(host, bytes))
      token_ = observer_->begin_read(observer_->context, host, bytes);
  }
  ~ReadScope() { if (token_) observer_->end_read(observer_->context, token_); }
  ReadScope(const ReadScope&) = delete;
  ReadScope& operator=(const ReadScope&) = delete;
};
#endif

template<class T> inline T load(const void* host) noexcept {
  static_assert(std::is_integral<T>::value, "scalar guest load requires an integer");
  ReadScope read(host, sizeof(T));
  return *static_cast<const volatile T*>(host);
}

template<class T> inline void store(void* host, T value) noexcept {
  static_assert(std::is_integral<T>::value, "scalar guest store requires an integer");
  WriteScope write(host, sizeof(T));
  *static_cast<volatile T*>(host) = value;
}
template<class F> inline auto write(void* host, size_t bytes, F&& operation)
    noexcept(noexcept(operation())) -> decltype(operation()) {
  WriteScope scope(host, bytes);
  return operation();
}
template<class F> inline auto read(const void* host, size_t bytes, F&& operation)
    noexcept(noexcept(operation())) -> decltype(operation()) {
  ReadScope scope(host, bytes);
  return operation();
}
inline void* copy(void* destination, const void* source, size_t bytes) noexcept {
  // Bulk operations and decoder calls may nest accesses. A bound owner must
  // support same-thread nesting and a single global range-lock order; acquiring
  // unrelated per-resource locks here would deadlock overlapping copies.
  ReadScope read(source, bytes);
  WriteScope write(destination, bytes);
  return std::memcpy(destination, source, bytes);
}
inline void* move(void* destination, const void* source, size_t bytes) noexcept {
  ReadScope read(source, bytes);
  WriteScope write(destination, bytes);
  return std::memmove(destination, source, bytes);
}
inline void* fill(void* destination, int value, size_t bytes) noexcept {
  WriteScope write(destination, bytes);
  return std::memset(destination, value, bytes);
}
inline int compare(const void* a, const void* b, size_t bytes) noexcept {
  ReadScope first(a, bytes), second(b, bytes);
  return std::memcmp(a, b, bytes);
}
inline const void* find(const void* source, int value, size_t bytes) noexcept {
  // A bounded string search must not treat the rest of a 512MB guest view as
  // read input. Observe each page before searching it and stop at the match.
  auto* p = static_cast<const uint8_t*>(source);
  while (bytes) {
    size_t n = 4096 - (reinterpret_cast<uintptr_t>(p) & 4095);
    if (n > bytes) n = bytes;
    ReadScope scope(p, n);
    if (const void* found = std::memchr(p, value, n)) return found;
    p += n; bytes -= n;
  }
  return nullptr;
}
template<class T, class A, class B>
inline bool compare_exchange(T* destination, A expected, B value) noexcept {
  ReadScope read(destination, sizeof(T));
  WriteScope write(destination, sizeof(T));
  return __sync_bool_compare_and_swap(destination, T(expected), T(value));
}
} // namespace lsw_guest

