// One-run observation of the native libdrm_nouveau BO allocation/ref/free boundary.
// Diagnostic only: no driver call is executed while holding g_mutex.
#include "lsw_bo_check.h"
#include <switch.h>
extern "C" {
#include <nouveau.h>
void _start();
void lsw_log_path(char* out, size_t capacity, const char* name);
int lsw_bo_check_allocator_observed() __attribute__((weak));
int __real_nouveau_bo_new(nouveau_device*, uint32_t, uint32_t, uint64_t, nouveau_bo_config*, nouveau_bo**);
int __real_nouveau_bo_name_ref(nouveau_device*, uint32_t, nouveau_bo**);
void __real_nouveau_bo_ref(nouveau_bo*, nouveau_bo**);
int __real_nouveau_bo_map(nouveau_bo*, uint32_t, nouveau_client*);
int __real_nouveau_bo_wait(nouveau_bo*, uint32_t, nouveau_client*);
}
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace {
using namespace lsw_bo_check;
Ledger g_ledger;
std::mutex g_mutex;
thread_local bool g_inside = false;
std::atomic<bool> g_armed{false};
uint64_t g_run = 0; // set before publishing g_armed / starting graphics workers
// Exact installed v1.0.1 ABI: public BO 80 bytes, private atomic refcnt at +80.
// Qualified against the actual linked nouveau_bo_ref instructions, not just a header.
static_assert(sizeof(nouveau_bo) == 80 && offsetof(nouveau_bo, offset) == 32);
View Read(uintptr_t p) {
    const auto* b = reinterpret_cast<const nouveau_bo*>(p);
    return {reinterpret_cast<uintptr_t>(b->device), b->size, b->offset, b->handle,
            __atomic_load_n(reinterpret_cast<const int32_t*>(p + sizeof(nouveau_bo)), __ATOMIC_ACQUIRE)};
}
Context Here(uintptr_t caller, uintptr_t peer = 0) { return {caller, peer, threadGetCurHandle()}; }
struct Inside { Inside() { g_inside = true; } ~Inside() { g_inside = false; } };
bool Incomplete(Error e) { return e == Error::Unknown || e == Error::Capacity; }
const char* ErrorName(Error e) {
    static const char* names[] = {"none", "unknown-BO-coverage", "table-capacity", "retired-or-reused-storage",
        "immutable-fields-changed", "invalid-reference-count", "free-with-live-references", "live-storage-reallocated",
        "double-free", "BO-storage-realloc"};
    return names[unsigned(e)];
}
void Print(FILE* f, const uintptr_t* candidates = nullptr, size_t count = 0) { // caller owns g_mutex
    const Problem& p = g_ledger.problem;
    const uintptr_t base = reinterpret_cast<uintptr_t>(&_start);
    std::fprintf(f, "[BOCHECK] run=%016llX\n", (unsigned long long)g_run);
    std::fprintf(f, "[BOCHECK] status=%s reason=%s object=%p generation=%llu used=%zu live=%zu events=%llu metadata=%zu\n",
        p.error == Error::None ? "no-violation-observed" : Incomplete(p.error) ? "incomplete" : "violation",
        ErrorName(p.error), reinterpret_cast<void*>(p.object), (unsigned long long)p.record.generation,
        g_ledger.used, g_ledger.live, (unsigned long long)g_ledger.sequence, sizeof(g_ledger));
    std::fprintf(f, "[BOCHECK] expected device=%p handle=%X size=%llu offset=%llX create=+%llX thread=%llX free=+%llX thread=%llX\n",
        reinterpret_cast<void*>(p.record.original.device), p.record.original.handle,
        (unsigned long long)p.record.original.size, (unsigned long long)p.record.original.offset,
        (unsigned long long)(p.record.created.caller ? p.record.created.caller - base : 0),
        (unsigned long long)p.record.created.thread,
        (unsigned long long)(p.record.freed.caller ? p.record.freed.caller - base : 0), (unsigned long long)p.record.freed.thread);
    // A raw driver exception may precede our next hook. Match all saved GPRs to
    // known identities without dereferencing any fault-time pointer or guessing
    // which register contains a BO in this particular build.
    for (size_t i = 0; i < count; ++i) {
        const size_t at = g_ledger.Find(candidates[i]);
        if (at == Ledger::kSlots) continue;
        const Record& r = g_ledger.records[at];
        std::fprintf(f, "[BOCHECK] register=%zu bo=%p gen=%llu state=%u expected_device=%p handle=%X size=%llu offset=%llX create=+%llX thread=%llX free=+%llX thread=%llX\n",
            i, reinterpret_cast<void*>(candidates[i]), (unsigned long long)r.generation, unsigned(r.state),
            reinterpret_cast<void*>(r.original.device), r.original.handle, (unsigned long long)r.original.size,
            (unsigned long long)r.original.offset, (unsigned long long)(r.created.caller ? r.created.caller-base : 0),
            (unsigned long long)r.created.thread, (unsigned long long)(r.freed.caller ? r.freed.caller-base : 0),
            (unsigned long long)r.freed.thread);
    }
    const uint64_t first = g_ledger.sequence > Ledger::kHistory ? g_ledger.sequence - Ledger::kHistory + 1 : 1;
    for (uint64_t n = first; n <= g_ledger.sequence; ++n) {
        const lsw_bo_check::Event& e = g_ledger.history[(n - 1) % Ledger::kHistory];
        std::fprintf(f, "[BOCHECK] event=%llu op=%u bo=%p gen=%llu thread=%llX caller=+%llX peer=%p dev=%p refs=%d handle=%X size=%llu offset=%llX\n",
            (unsigned long long)e.sequence, unsigned(e.op), reinterpret_cast<void*>(e.object), (unsigned long long)e.generation,
            (unsigned long long)e.context.thread, (unsigned long long)(e.context.caller ? e.context.caller - base : 0),
            reinterpret_cast<void*>(e.context.peer), reinterpret_cast<void*>(e.seen.device), e.seen.refs, e.seen.handle,
            (unsigned long long)e.seen.size, (unsigned long long)e.seen.offset);
    }
    std::fprintf(f, "[BOCHECK] limits: boundary observations; internal submission increments not journaled; reused pointer alone does not identify its owner's generation; no pixel/performance validity claim\n");
}
FILE* OpenReport() {
    char path[96];
    lsw_log_path(path, sizeof path, "bocheck.log");
    return std::fopen(path, "w");
}
template<class F> void Observe(F fn) {
    if (g_inside || !g_ledger.enabled.load(std::memory_order_acquire)) return;
    Inside inside;
    Error error;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_ledger.enabled.load(std::memory_order_relaxed)) return;
        fn();
        error = g_ledger.problem.error;
    }
    if (error == Error::None) return;
    // enabled is now false. No driver continuation for a known violation; missing
    // coverage disables observation explicitly and leaves baseline behavior intact.
    std::printf("[BOCHECK] %s: %s; details in bocheck.log\n", Incomplete(error) ? "INCOMPLETE" : "STOP", ErrorName(error));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (FILE* f = OpenReport()) { Print(f); std::fclose(f); }
        else std::printf("[BOCHECK] report file could not be opened\n");
    }
    std::fflush(stdout);
    if (!Incomplete(error)) std::abort();
}
void Access(nouveau_bo* bo, Op op, uintptr_t caller, uintptr_t peer = 0) {
    Observe([&] { g_ledger.Access(reinterpret_cast<uintptr_t>(bo), op, Here(caller, peer), Read); });
}
} // namespace

extern "C" void lsw_bo_check_init(uint64_t run) {
    FILE* f = std::fopen("bocheck_once", "rb");
    if (!f) return;
    std::fclose(f);
    if (std::remove("bocheck_once") != 0) { std::printf("[BOCHECK] INCOMPLETE: marker could not be consumed; off\n"); return; }
    if (!lsw_bo_check_allocator_observed || lsw_bo_check_allocator_observed() != 1) {
        std::printf("[BOCHECK] INCOMPLETE: allocator hooks unavailable; off\n"); return;
    }
    g_run = run;
    g_armed.store(true, std::memory_order_release);
    g_ledger.enabled.store(true, std::memory_order_release);
    std::printf("[BOCHECK] run=%016llX one-run native BO observation armed: %zu addresses/%zu events/%zu bytes; not a performance run\n",
        (unsigned long long)g_run, Ledger::kSlots, Ledger::kHistory, sizeof(g_ledger));
    // Persist identity before graphics starts. A HOME/process close need not
    // reach main's finish hook; an absent final report is not checker coverage.
    Inside inside;
    if (FILE* report = OpenReport()) {
        std::fprintf(report, "[BOCHECK] phase=armed; graphics coverage not yet observed; periodic coverage is in stdout.log; final shutdown not certified\n");
        Print(report);
        std::fclose(report);
    } else std::printf("[BOCHECK] initial report file could not be opened; periodic coverage will use stdout.log\n");
}
extern "C" void lsw_bo_check_progress(uint64_t swaps) {
    if (!g_armed.load(std::memory_order_acquire) || g_inside) return;
    Inside inside;
    if (!g_mutex.try_lock()) {
        std::printf("[BOCHECK] run=%016llX phase=running swaps=%llu coverage=busy; no sample\n",
            (unsigned long long)g_run, (unsigned long long)swaps);
        return;
    }
    const auto used = g_ledger.used, live = g_ledger.live;
    const auto events = g_ledger.sequence;
    const auto error = g_ledger.problem.error;
    const bool enabled = g_ledger.enabled.load(std::memory_order_relaxed);
    g_mutex.unlock();
    // Reuse the existing flushed render log; never do file I/O under the
    // observation mutex or dump the full event ring at a periodic boundary.
    std::printf("[BOCHECK] run=%016llX phase=running swaps=%llu enabled=%u used=%zu live=%zu events=%llu reason=%s; final shutdown not certified\n",
        (unsigned long long)g_run, (unsigned long long)swaps, unsigned(enabled),
        used, live, (unsigned long long)events, ErrorName(error));
}
extern "C" void lsw_bo_check_allocated(void* p, uintptr_t caller) {
    if (g_inside || !g_ledger.enabled.load(std::memory_order_acquire) || !g_ledger.MayContain(reinterpret_cast<uintptr_t>(p))) return;
    Observe([&] { g_ledger.Allocated(reinterpret_cast<uintptr_t>(p), Here(caller)); });
}
extern "C" void lsw_bo_check_before_free(void* p, uintptr_t caller) {
    if (g_inside || !g_ledger.enabled.load(std::memory_order_acquire) || !g_ledger.MayContain(reinterpret_cast<uintptr_t>(p))) return;
    Observe([&] { g_ledger.Free(reinterpret_cast<uintptr_t>(p), Here(caller), Read); });
}
extern "C" void lsw_bo_check_before_realloc(void* p, uintptr_t caller) {
    if (g_inside || !g_ledger.enabled.load(std::memory_order_acquire) || !g_ledger.MayContain(reinterpret_cast<uintptr_t>(p))) return;
    Observe([&] { g_ledger.Reallocating(reinterpret_cast<uintptr_t>(p), Here(caller)); });
}
extern "C" void lsw_bo_check_dump(FILE* f, const uintptr_t* candidates, size_t count) {
    if (!g_armed.load(std::memory_order_acquire) || !f || g_inside) return;
    Inside inside;
    if (!g_mutex.try_lock()) { std::fprintf(f, "[BOCHECK] history unavailable: observation in progress\n"); return; }
    Print(f, candidates, count);
    g_mutex.unlock();
}
extern "C" void lsw_bo_check_finish() {
    if (!g_armed.load(std::memory_order_acquire)) return;
    if (FILE* f = OpenReport()) { lsw_bo_check_dump(f, nullptr, 0); std::fclose(f); }
}
extern "C" volatile int lsw_nvc0_rt_compress;   // mesa_compress_edits.py
extern "C" uint32_t lsw_heap_free_mb();          // platform_switch.cpp
extern "C" int lsw_glthread_call(void (*fn)(void*), void* arg);   // mesa_glthread_edits.py: fn on Mesa's GL thread, in order

// ---- log78: a pool of freed texture storage ----
// Run 79: every new texture's storage was a new buffer object, and on the Switch nouveau_bo_new is a memalign, a memset and two
// calls to the GPU service (nvMapCreate, nvAddressSpaceMap); the render thread spent about 0.6 s of the session there
// (nvc0_miptree_create), the biggest share of a small texture's cost. Freed block-linear buffers (textures and render targets: a
// memory kind other than pitch) of up to 4 MB are now kept, up to 32 MB, and handed to the next request with the same size,
// alignment, flags, kind and tile mode. log79: in any domain. libdrm on the Switch reports no VRAM, so Mesa puts textures in GART;
// run 80's VRAM-only filter matched nothing. Not kept while the heap has less than 256 MB free. Release waits for the buffer's fence first, as libdrm's nouveau_bo_del does, so the GPU
// is done with it. The contents are not cleared: GL leaves new texture storage undefined, and every texture is uploaded whole.
// Mirrors libdrm_nouveau 1.0.1's private BO (private.h); the first buffer the pool sees checks the layout, and the pool turns itself
// off if it does not match. LSWTCS_BOPOOL=0 turns it off. Not used while the BO checker above is armed.
namespace {
struct BoPriv { nouveau_bo base; int32_t refcnt; void* map_addr; uint32_t name; uint32_t access; NvMap map; NvFence fence; };
static_assert(offsetof(BoPriv, refcnt) == 80, "libdrm_nouveau 1.0.1 layout");
struct PoolKey {
    uint64_t size; uint32_t flags, align, memtype, tile_mode;
    bool operator==(const PoolKey& o) const {
        return size == o.size && flags == o.flags && align == o.align && memtype == o.memtype && tile_mode == o.tile_mode;
    }
};
struct PoolKeyHash {
    size_t operator()(const PoolKey& k) const {
        return size_t(k.size * 1000003u) ^ (size_t(k.flags) << 1) ^ (size_t(k.align) << 7) ^ (size_t(k.memtype) << 13) ^ (size_t(k.tile_mode) << 21);
    }
};
std::mutex g_pool_mtx;
std::unordered_map<nouveau_bo*, PoolKey> g_pool_owned;   // buffers made through the pool path, live or kept
std::unordered_multimap<PoolKey, nouveau_bo*, PoolKeyHash> g_pool_free;
uint64_t g_pool_bytes = 0, g_pool_hits = 0, g_pool_misses = 0, g_pool_kept = 0, g_pool_dropped = 0;
int g_pool_state = -1;   // -1: not checked yet, 0: off, 1: on
constexpr uint64_t kPoolMaxBo = 4ull << 20, kPoolCap = 48ull << 20;   // log81: 48 MB (was 32)
constexpr uint32_t kPoolMinFreeMb = 256;
// log81: storage made ahead. Run 83 (log80): a new texture's upload was 85-95 % "waiting for Mesa's GL thread", and that wait is
// mostly the GL thread's work run on the render thread by glthread's finish: the texture's storage (nouveau_bo_new), of which 75 %
// is the GPU service calls (nvMapCreate, nvAddressSpaceMap), about 0.8 ms a texture. The pool only had storage once textures were
// evicted (run 83: first keep at Rescue_A, 7 minutes in). Now each storage kind (size, alignment, flags, memory kind, tile mode)
// remembers the most made in one replay (up to 16), over sessions (bopool.bin, gl_backend.cpp), and in light replays the GL thread
// makes one such buffer ahead (lsw_glthread_call), smallest kinds first, until the pool holds that many of each, within its 48 MB
// and while the heap has 384 MB free. LSWTCS_BOPOOLFILL=0 turns that off.
struct Demand { uint32_t frame = 0, peak = 0; };
std::unordered_map<PoolKey, Demand, PoolKeyHash> g_pool_demand;   // under g_pool_mtx
nouveau_device* g_pool_dev = nullptr;                              // under g_pool_mtx: the device textures are made on
uint64_t g_pool_made = 0;                                          // made ahead
bool g_refill_busy = false;                                        // a refill job is queued (render thread sets, job clears)
PoolKey g_refill_key{};
constexpr uint32_t kPeakMax = 16, kRefillMinFreeMb = 384;
bool pool_wanted() {
    static const bool on = [] { const char* e = getenv("LSWTCS_BOPOOL"); return !(e && e[0] == '0'); }();
    return on && g_pool_state != 0 && !g_ledger.enabled.load(std::memory_order_relaxed);
}
bool pool_eligible(uint32_t flags, uint64_t size, const nouveau_bo_config* config) {
    return config && config->nvc0.memtype != 0 && !(flags & NOUVEAU_BO_CONTIG) && size && size <= kPoolMaxBo &&
           !(lsw_nvc0_rt_compress & 1);
}
// The layout check, on a buffer just made by libdrm: its NvMap handle is the public handle, the fence is unset, the CPU copy exists.
bool pool_layout_ok(nouveau_bo* bo) {
    const BoPriv* p = reinterpret_cast<const BoPriv*>(bo);
    return p->map.handle == bo->handle && p->fence.id == UINT32_MAX && p->map_addr &&
           (reinterpret_cast<uintptr_t>(p->map_addr) & 0xFFF) == 0;
}
void pool_release(nouveau_bo* bo, const PoolKey& k) {
    BoPriv* p = reinterpret_cast<BoPriv*>(bo);
    if (int32_t(p->fence.id) >= 0) {   // as nouveau_bo_fence_wait
        nvFenceWait(&p->fence, -1);
        p->fence.id = UINT32_MAX;
        p->fence.value = 0;
    }
    p->access = 0;
    const bool room = lsw_heap_free_mb() >= kPoolMinFreeMb;
    bool keep;
    {
        std::lock_guard<std::mutex> lk(g_pool_mtx);
        keep = room && g_pool_bytes + k.size <= kPoolCap;
        if (keep) { g_pool_free.emplace(k, bo); g_pool_bytes += k.size; ++g_pool_kept; }
        else { g_pool_owned.erase(bo); ++g_pool_dropped; }
    }
    // log80: says when the pool starts working, so a later crash log shows whether it had (run 81's crash came before any keep).
    static bool first = true;
    if (keep && first) { first = false; std::printf("[BOPOOL] first freed texture storage kept (%llu KB)\n", (unsigned long long)(k.size >> 10)); }
    if (!keep) { nouveau_bo* q = bo; __real_nouveau_bo_ref(nullptr, &q); }   // the last reference: libdrm deletes it
}
} // namespace

// [HEAPGL] (gl_backend.cpp): buffers handed out from the pool, made new, kept, too many to keep, MB kept now, made ahead.
extern "C" void lsw_bo_pool_stats(uint64_t* hits, uint64_t* misses, uint64_t* kept, uint64_t* dropped, uint64_t* bytes, int* state,
                                  uint64_t* made) {
    std::lock_guard<std::mutex> lk(g_pool_mtx);
    *hits = g_pool_hits; *misses = g_pool_misses; *kept = g_pool_kept; *dropped = g_pool_dropped; *bytes = g_pool_bytes; *state = g_pool_state;
    *made = g_pool_made;
}

// log81: the demand, saved and read by gl_backend.cpp (bopool.bin).
struct LswBoPoolDemand { uint64_t size; uint32_t flags, align, memtype, tile_mode, peak, pad; };
extern "C" size_t lsw_bo_pool_demand(LswBoPoolDemand* out, size_t max) {
    std::lock_guard<std::mutex> lk(g_pool_mtx);
    size_t n = 0;
    for (const auto& kv : g_pool_demand) {
        if (n >= max) break;
        if (!kv.second.peak) continue;
        const PoolKey& k = kv.first;
        out[n++] = {k.size, k.flags, k.align, k.memtype, k.tile_mode, kv.second.peak, 0};
    }
    return n;
}
extern "C" void lsw_bo_pool_seed(const LswBoPoolDemand* in, size_t n) {
    std::lock_guard<std::mutex> lk(g_pool_mtx);
    for (size_t i = 0; i < n; ++i) {
        if (!in[i].size || in[i].size > kPoolMaxBo || !in[i].memtype) continue;
        Demand& d = g_pool_demand[PoolKey{in[i].size, in[i].flags, in[i].align, in[i].memtype, in[i].tile_mode}];
        d.peak = std::max(d.peak, std::min(in[i].peak, kPeakMax));
    }
}
namespace {
// On Mesa's GL thread (lsw_glthread_call), in order with the GL commands: one buffer of g_refill_key, into the pool.
void refill_job(void*) {
    const PoolKey k = g_refill_key;
    nouveau_device* dev;
    { std::lock_guard<std::mutex> lk(g_pool_mtx); dev = g_pool_dev; }
    nouveau_bo* bo = nullptr;
    int rc = -1;
    if (dev) {
        nouveau_bo_config cfg{};
        cfg.nvc0.memtype = k.memtype;
        cfg.nvc0.tile_mode = k.tile_mode;
        rc = __real_nouveau_bo_new(dev, k.flags, k.align, k.size, &cfg, &bo);
    }
    bool keep = false;
    {
        std::lock_guard<std::mutex> lk(g_pool_mtx);
        if (!rc && bo) {
            keep = g_pool_bytes + k.size <= kPoolCap;
            if (keep) { g_pool_owned[bo] = k; g_pool_free.emplace(k, bo); g_pool_bytes += k.size; ++g_pool_made; }
            else g_pool_owned.erase(bo);
        }
        g_refill_busy = false;
    }
    if (!rc && bo && !keep) { nouveau_bo* q = bo; __real_nouveau_bo_ref(nullptr, &q); }
    static bool first = true;
    if (keep && first) { first = false; std::printf("[BOPOOL] first texture storage made ahead (%llu KB)\n", (unsigned long long)(k.size >> 10)); }
}
} // namespace

// Render thread, once per replay: closes the replay's demand count; in a light replay, may queue one buffer to make ahead.
extern "C" void lsw_bo_pool_frame(int light) {
    static const bool fill = [] { const char* e = getenv("LSWTCS_BOPOOLFILL"); return !(e && e[0] == '0'); }();
    static uint32_t tick = 0;
    bool queue = false;
    {
        std::lock_guard<std::mutex> lk(g_pool_mtx);
        for (auto& kv : g_pool_demand) {
            kv.second.peak = std::max(kv.second.peak, std::min(kv.second.frame, kPeakMax));
            kv.second.frame = 0;
        }
        if (!fill || !light || g_refill_busy || g_pool_state != 1 || !g_pool_dev || (++tick & 1)) return;
        // The kinds within the budget, smallest first; the first one short of its peak.
        std::vector<std::pair<PoolKey, uint32_t>> order;
        order.reserve(g_pool_demand.size());
        for (const auto& kv : g_pool_demand) if (kv.second.peak) order.emplace_back(kv.first, kv.second.peak);
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first.size < b.first.size; });
        uint64_t budget = 0;
        for (const auto& [k, peak] : order) {
            budget += k.size * peak;
            if (budget > kPoolCap) break;
            if (g_pool_free.count(k) < peak) {
                if (g_pool_bytes + k.size > kPoolCap) break;
                g_refill_key = k;
                queue = true;
                break;
            }
        }
        if (queue) g_refill_busy = true;
    }
    if (!queue) return;
    if (lsw_heap_free_mb() < kRefillMinFreeMb) {
        std::lock_guard<std::mutex> lk(g_pool_mtx);
        g_refill_busy = false;
        return;
    }
    lsw_glthread_call(refill_job, nullptr);
}

// log81: GPU submits. Runs 82-83: freezes of 1.9 and 3.6 s with Mesa's GL thread blocked in nvGpuChannelKickoff (88 % of its samples)
// and the GPU load reading 0 %. Counts every submit and its time; one of 100 ms or more prints [GPUSUBMIT] with the channel's
// error notification and error info, which say whether the GPU faulted (a fault would point at a buffer freed too early).
uint64_t g_kick_n = 0, g_kick_ns = 0, g_kick_slow = 0, g_kick_max = 0;   // GL thread (one channel per pushbuf; benign races)
extern "C" Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
extern "C" Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
    const uint32_t entries = c ? c->num_entries : 0;
    const uint64_t t0 = armTicksToNs(armGetSystemTick());
    const Result rc = __real_nvGpuChannelKickoff(c);
    const uint64_t ns = armTicksToNs(armGetSystemTick()) - t0;
    ++g_kick_n;
    g_kick_ns += ns;
    if (ns >= 16000000) ++g_kick_slow;
    if (ns > g_kick_max) g_kick_max = ns;
    if (ns >= 100000000) {
        static uint32_t logged = 0;
        if (logged++ < 50) {
            NvNotification note{};
            NvError err{};
            const Result rn = nvGpuChannelGetErrorNotification(c, &note);
            const Result re = nvGpuChannelGetErrorInfo(c, &err);
            std::printf("[GPUSUBMIT] kickoff took %.1f ms (rc 0x%X, %u entries, syncpoint %u value %u+%u) | error notification rc 0x%X "
                        "type %u info16 %u status 0x%X | error info rc 0x%X type %u info %08X %08X %08X %08X\n",
                        ns / 1e6, rc, entries, c->fence.id, c->fence.value, c->fence_incr, rn, note.info32, note.info16, note.status,
                        re, err.type, err.info[0], err.info[1], err.info[2], err.info[3]);
            std::fflush(stdout);
        }
    }
    return rc;
}
extern "C" void lsw_gpu_kick_stats(uint64_t* n, uint64_t* ns, uint64_t* slow, uint64_t* max_ns) {
    *n = g_kick_n; *ns = g_kick_ns; *slow = g_kick_slow; *max_ns = g_kick_max;
}

extern "C" int __wrap_nouveau_bo_new(nouveau_device* d, uint32_t flags, uint32_t align, uint64_t size,
                                      nouveau_bo_config* config, nouveau_bo** out) {
    const bool pooled = out && pool_wanted() && pool_eligible(flags, size, config);
    PoolKey key{};
    if (pooled) {
        key = {(size + 0xFFF) & ~uint64_t(0xFFF), flags, align < 0x1000 ? 0x1000u : align, config->nvc0.memtype, config->nvc0.tile_mode};
        nouveau_bo* got = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_pool_mtx);
            ++g_pool_demand[key].frame;
            if (!g_pool_dev) g_pool_dev = d;
            auto it = g_pool_free.find(key);
            if (it != g_pool_free.end()) { got = it->second; g_pool_free.erase(it); g_pool_bytes -= key.size; ++g_pool_hits; }
            else ++g_pool_misses;
        }
        if (got) {   // still holds the one reference the pool kept
            static bool first = true;
            if (first) { first = false; std::printf("[BOPOOL] first texture storage reused (%llu KB)\n", (unsigned long long)(key.size >> 10)); }
            *out = got;
            return 0;
        }
    }
    // A render target of states Z and J: a big-page mapping, as deko3d gives compressed images. Horizon may otherwise map a compressed kind without compression.
    if ((lsw_nvc0_rt_compress & 1) && config) {
        align = 0x20000;
        size = (size + 0x1FFFF) & ~uint64_t(0x1FFFF);
        printf("[BOKIND] render target kind 0x%02X, %llu KB, aligned 128 KB\n", unsigned(config->nvc0.memtype), (unsigned long long)(size >> 10));
    }
    const int rc = __real_nouveau_bo_new(d, flags, align, size, config, out);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!rc && out && *out) {
        // A new buffer at an address the pool knew means libdrm freed the old one without passing here: forget it.
        bool track = false;
        {
            std::lock_guard<std::mutex> lk(g_pool_mtx);
            if (pooled && g_pool_state < 0) {
                g_pool_state = pool_layout_ok(*out) ? 1 : 0;
                std::printf("[BOPOOL] texture storage pool %s (libdrm private layout %s)\n", g_pool_state ? "on" : "OFF",
                            g_pool_state ? "as expected" : "differs");
            }
            track = pooled && g_pool_state == 1;
            if (track) g_pool_owned[*out] = key; else g_pool_owned.erase(*out);
        }
        Observe([&] { g_ledger.Created(reinterpret_cast<uintptr_t>(*out), reinterpret_cast<uintptr_t>(d), Read(reinterpret_cast<uintptr_t>(*out)), Here(caller)); });
    }
    return rc;
}
extern "C" int __wrap_nouveau_bo_name_ref(nouveau_device* d, uint32_t name, nouveau_bo** out) {
    const int rc = __real_nouveau_bo_name_ref(d, name, out);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!rc && out && *out) { std::lock_guard<std::mutex> lk(g_pool_mtx); g_pool_owned.erase(*out); }   // log78 pool: not a pooled buffer
    if (!rc && out && *out) Observe([&] { g_ledger.Created(reinterpret_cast<uintptr_t>(*out), reinterpret_cast<uintptr_t>(d), Read(reinterpret_cast<uintptr_t>(*out)), Here(caller)); });
    return rc;
}
extern "C" void __wrap_nouveau_bo_ref(nouveau_bo* bo, nouveau_bo** out) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (out && *out && *out != bo && g_pool_state == 1 && pool_wanted()) {   // log78 pool: the last reference of a pooled buffer
        nouveau_bo* const last = *out;
        PoolKey key{};
        bool owned;
        {
            std::lock_guard<std::mutex> lk(g_pool_mtx);
            auto it = g_pool_owned.find(last);
            owned = it != g_pool_owned.end();
            if (owned) key = it->second;
        }
        if (owned && __atomic_load_n(&reinterpret_cast<BoPriv*>(last)->refcnt, __ATOMIC_ACQUIRE) == 1) {
            nouveau_bo* taken = nullptr;
            if (bo) __real_nouveau_bo_ref(bo, &taken);   // +1 on the new one, nothing released
            *out = bo;
            pool_release(last, key);
            return;
        }
    }
    nouveau_bo* previous = nullptr;
    if (g_ledger.enabled.load(std::memory_order_acquire)) {
        previous = out ? *out : nullptr;
        Access(bo, Op::Reference, caller, reinterpret_cast<uintptr_t>(out));
        if (previous != bo) Access(previous, Op::Reference, caller, reinterpret_cast<uintptr_t>(out));
    }
    __real_nouveau_bo_ref(bo, out);
    if (bo || previous) Observe([&] { g_ledger.Note(reinterpret_cast<uintptr_t>(bo ? bo : previous), Op::ReferenceDone,
                                                  Here(caller, reinterpret_cast<uintptr_t>(out))); });
}
extern "C" int __wrap_nouveau_bo_map(nouveau_bo* bo, uint32_t access, nouveau_client* client) {
    Access(bo, Op::Map, reinterpret_cast<uintptr_t>(__builtin_return_address(0)), reinterpret_cast<uintptr_t>(client));
    return __real_nouveau_bo_map(bo, access, client);
}
extern "C" int __wrap_nouveau_bo_wait(nouveau_bo* bo, uint32_t access, nouveau_client* client) {
    Access(bo, Op::Wait, reinterpret_cast<uintptr_t>(__builtin_return_address(0)), reinterpret_cast<uintptr_t>(client));
    return __real_nouveau_bo_wait(bo, access, client);
}
