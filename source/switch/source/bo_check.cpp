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
#include <mutex>

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
extern "C" int __wrap_nouveau_bo_new(nouveau_device* d, uint32_t flags, uint32_t align, uint64_t size,
                                      nouveau_bo_config* config, nouveau_bo** out) {
    // A render target of states Z and J: a big-page mapping, as deko3d gives compressed images. Horizon may otherwise map a compressed kind without compression.
    if ((lsw_nvc0_rt_compress & 1) && config) {
        align = 0x20000;
        size = (size + 0x1FFFF) & ~uint64_t(0x1FFFF);
        printf("[BOKIND] render target kind 0x%02X, %llu KB, aligned 128 KB\n", unsigned(config->nvc0.memtype), (unsigned long long)(size >> 10));
    }
    const int rc = __real_nouveau_bo_new(d, flags, align, size, config, out);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!rc && out && *out) Observe([&] { g_ledger.Created(reinterpret_cast<uintptr_t>(*out), reinterpret_cast<uintptr_t>(d), Read(reinterpret_cast<uintptr_t>(*out)), Here(caller)); });
    return rc;
}
extern "C" int __wrap_nouveau_bo_name_ref(nouveau_device* d, uint32_t name, nouveau_bo** out) {
    const int rc = __real_nouveau_bo_name_ref(d, name, out);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!rc && out && *out) Observe([&] { g_ledger.Created(reinterpret_cast<uintptr_t>(*out), reinterpret_cast<uintptr_t>(d), Read(reinterpret_cast<uintptr_t>(*out)), Here(caller)); });
    return rc;
}
extern "C" void __wrap_nouveau_bo_ref(nouveau_bo* bo, nouveau_bo** out) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
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
