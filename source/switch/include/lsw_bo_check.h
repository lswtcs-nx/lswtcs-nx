// Temporary native BO lifecycle observation. Caller serializes mutations; driver calls
// run outside that lock. Keys never disappear, so unrelated allocator traffic has a
// lock-free negative lookup. No inferred reference ledger: read the driver's count.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lsw_bo_check {
enum class State : uint8_t { Empty, Live, Freed, Foreign };
enum class Op : uint8_t { Create, Reference, ReferenceDone, Map, Wait, Free, Allocate, Reallocate };
enum class Error : uint8_t { None, Unknown, Capacity, Retired, Changed, Refcount, EarlyFree, LiveAllocation, DoubleFree, Reallocated };
struct View { uintptr_t device = 0; uint64_t size = 0, offset = 0; uint32_t handle = 0; int32_t refs = 0; };
struct Context { uintptr_t caller = 0, peer = 0; uint64_t thread = 0; };
struct Record {
    View original{};
    uint64_t generation = 0;
    Context created{}, freed{};
    State state = State::Empty;
};
struct Event {
    uint64_t sequence = 0, generation = 0;
    uintptr_t object = 0;
    Context context{};
    View seen{};
    Op op = Op::Create;
};
struct Problem {
    Error error = Error::None;
    uintptr_t object = 0;
    Record record{};
    Event event{};
};
class Ledger {
public:
    static constexpr size_t kSlots = 4096, kHistory = 512;
    std::atomic<bool> enabled{false};
    std::atomic<uintptr_t> keys[kSlots]{};
    Record records[kSlots]{};
    Event history[kHistory]{};
    Problem problem{};
    uint64_t sequence = 0, generations = 0, observed = 0;
    size_t used = 0, live = 0;

    size_t Find(uintptr_t object) const {
        if (!object) return kSlots;
        size_t at = ((object >> 4) * uint64_t(11400714819323198485ull)) & (kSlots - 1);
        for (size_t n = 0; n < kSlots; ++n, at = (at + 1) & (kSlots - 1)) {
            const uintptr_t key = keys[at].load(std::memory_order_acquire);
            if (key == object) return at;
            if (!key) break;
        }
        return kSlots;
    }
    // Includes tombstones: heap reuse of former BO storage must be observed.
    bool MayContain(uintptr_t object) const { return Find(object) != kSlots; }
    void Note(uintptr_t object, Op op, Context c, View seen = {}) {
        const size_t at = Find(object);
        Event e{++sequence, at < kSlots ? records[at].generation : 0, object, c, seen, op};
        history[(sequence - 1) % kHistory] = e;
        ++observed;
    }
    void Stop(Error error, uintptr_t object) {
        if (problem.error != Error::None) return;
        problem.error = error;
        problem.object = object;
        const size_t at = Find(object);
        if (at < kSlots) problem.record = records[at];
        if (sequence) problem.event = history[(sequence - 1) % kHistory];
        enabled.store(false, std::memory_order_release);
    }
    void Created(uintptr_t object, uintptr_t device, View v, Context c) {
        if (!enabled.load(std::memory_order_relaxed) || !object) return;
        size_t at = Find(object);
        if (at == kSlots) {
            at = ((object >> 4) * uint64_t(11400714819323198485ull)) & (kSlots - 1);
            size_t n = 0;
            while (n < kSlots && keys[at].load(std::memory_order_relaxed)) {
                ++n; at = (at + 1) & (kSlots - 1);
            }
            if (n == kSlots) { Note(object, Op::Create, c, v); Stop(Error::Capacity, object); return; }
            ++used;
            keys[at].store(object, std::memory_order_release);
        }
        Record& r = records[at];
        if (r.state == State::Live) { Note(object, Op::Create, c, v); Stop(Error::LiveAllocation, object); return; }
        r = {};
        r.original = v; r.created = c; r.generation = ++generations; r.state = State::Live;
        ++live;
        Note(object, Op::Create, c, v);
        if (!device || v.device != device) Stop(Error::Changed, object);
        else if (v.refs <= 0) Stop(Error::Refcount, object);
    }
    // Reader is called only while a recognized allocation is live, under caller's
    // lock, shared with the allocator observer. Retired pointers are never read.
    template<class Reader> void Access(uintptr_t object, Op op, Context c, Reader read) {
        if (!enabled.load(std::memory_order_relaxed) || !object) return;
        const size_t at = Find(object);
        if (at == kSlots) { Note(object, op, c); Stop(Error::Unknown, object); return; }
        Record& r = records[at];
        if (r.state != State::Live) { Note(object, op, c); Stop(Error::Retired, object); return; }
        const View v = read(object);
        Note(object, op, c, v);
        if (v.device != r.original.device || v.handle != r.original.handle ||
            v.size != r.original.size || v.offset != r.original.offset) Stop(Error::Changed, object);
        else if (v.refs < 0 || (v.refs == 0 && op != Op::Free)) Stop(Error::Refcount, object);
    }
    template<class Reader> void Free(uintptr_t object, Context c, Reader read) {
        if (!enabled.load(std::memory_order_relaxed)) return;
        const size_t at = Find(object);
        if (at == kSlots) return;
        Record& r = records[at];
        if (r.state == State::Freed) { Note(object, Op::Free, c); Stop(Error::DoubleFree, object); return; }
        if (r.state == State::Foreign) { Note(object, Op::Free, c); return; }
        Access(object, Op::Free, c, read);
        if (!enabled.load(std::memory_order_relaxed)) return;
        if (history[(sequence - 1) % kHistory].seen.refs != 0) { Stop(Error::EarlyFree, object); return; }
        r.freed = c; r.state = State::Freed; --live;
    }
    void Allocated(uintptr_t object, Context c) {
        if (!enabled.load(std::memory_order_relaxed)) return;
        const size_t at = Find(object);
        if (at == kSlots) return;
        Note(object, Op::Allocate, c);
        if (records[at].state == State::Live) { Stop(Error::LiveAllocation, object); return; }
        records[at].state = State::Foreign;
    }
    void Reallocating(uintptr_t object, Context c) {
        if (!enabled.load(std::memory_order_relaxed)) return;
        const size_t at = Find(object);
        if (at == kSlots || records[at].state == State::Foreign) return;
        Note(object, Op::Reallocate, c);
        Stop(Error::Reallocated, object);
    }
};
static_assert(sizeof(Ledger) <= 768 * 1024, "bounded diagnostic metadata");
} // namespace lsw_bo_check
