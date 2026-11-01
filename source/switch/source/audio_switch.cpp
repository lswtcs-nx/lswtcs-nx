#include "lsw_guest_memory_access.h"
// Host audio output on the Switch (libnx audout), the counterpart of LSWTCSRuntime/source/host_audio.cpp.
// The game's XAudio render-driver clients hand one frame per XAudioSubmitRenderDriverFrame: 256 samples x 6
// channels (5.1), PLANAR big-endian float at 48 kHz. Frames are queued per client; one output thread mixes
// one frame from every client that has one, downmixes 5.1 to stereo, converts to 16-bit and appends it to
// audout as one buffer (256 stereo samples). When audout releases a buffer, every client whose frame it
// held gets on_end(idx), so the game's mixer is paced by the real device clock as on the PC.
// LSWTCS_AUDIO_OUT=0 disables output (kernel_stubs then paces frames with its 5.33 ms timer).
#include <switch.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void dbg_ram(const char* fmt, ...);
extern "C" void lsw_prof_register(const char* name, int sched_id);

namespace {
constexpr int kMaxClients = 8;
constexpr int kChannels = 6, kSamples = 256;
constexpr uint32_t kFrameFloats = kChannels * kSamples;
constexpr int kBuffers = 6;                        // ~32 ms queued on the device
constexpr uint32_t kBufferBytes = 0x1000;          // audout needs 0x1000-aligned buffers
constexpr uint32_t kDataBytes = kSamples * 2 * sizeof(int16_t);
constexpr size_t kMaxQueued = 32;                  // per client (the game keeps <= 8 in flight)

typedef void (*EndFn)(int idx);

struct Client {
    bool open = false;
    EndFn on_end = nullptr;
    std::deque<std::vector<float>> frames;         // host-order planar frames (FL FR FC LFE BL BR)
};

struct Slot {
    AudioOutBuffer buf{};
    bool queued = false;
    uint8_t clients = 0;                           // bit i = a frame of client i is in this buffer
};

std::mutex g_mx;
std::condition_variable g_cv;
Client g_clients[kMaxClients];
// Played frames are recycled (under g_mx) instead of freed: one 6 KB vector per 5.33 ms frame per client was
// ~190 allocator round trips a second on the audio threads.
std::vector<std::vector<float>> g_spare;
Slot g_slots[kBuffers];
int g_state = -1;                                  // -1 untried, 0 failed/disabled, 1 running
// A/V hold (LSWTCS_AVHOLD, default on): while the render thread holds the picture to build shader
// programs (gl_backend wait_frame_programs), no new buffers go to the device. No buffer released = no
// credit for the game's XAudio engine, so its voices and cutscene streams pause with the picture
// instead of running ahead of it. The few buffers already queued (~6 x 5.33 ms) still play out.
std::atomic<bool> g_hold{false};
std::atomic<uint64_t> g_hold_ns{0};
uint64_t g_hold_count = 0;
float g_peak = 0;   // largest |sample| sent to the device since the last [AUDIO] line (output thread only)
// [AUDIOQ] window stats, for audible jitter that [AUDIO] idle waits cannot show. Output thread only, except the
// submit fields (under g_mx). "Empty" = nothing queued on the device while not holding: an audible gap.
struct AudioWindow {
    uint64_t start_ns = 0, empty_since_ns = 0, empty_ns = 0, empty_max_ns = 0;
    uint32_t empties = 0, min_queued = kBuffers;
};
AudioWindow g_aw;
// Totals for the per-frame columns of frames.log (lsw_audio_stats): audio speed over any stretch of frames.
std::atomic<uint64_t> g_total_played{0}, g_total_empty_ns{0};
uint64_t g_last_submit_ns = 0, g_submit_gap_max_ns = 0;
uint32_t g_client_depth_max = 0;

// Mixes one frame from every client that has one into slot s; returns false if no client had a frame.
bool fill_slot(Slot& s) {
    float l[kSamples] = {}, r[kSamples] = {};
    uint8_t who = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (int i = 0; i < kMaxClients; ++i) {
            Client& c = g_clients[i];
            if (!c.open || c.frames.empty()) continue;
            const std::vector<float>& f = c.frames.front();
            const float* fl = &f[0 * kSamples]; const float* fr = &f[1 * kSamples];
            const float* fc = &f[2 * kSamples]; const float* lfe = &f[3 * kSamples];
            const float* bl = &f[4 * kSamples]; const float* br = &f[5 * kSamples];
            for (int n = 0; n < kSamples; ++n) {   // ITU-style 5.1 -> stereo downmix
                const float c3 = 0.7071f * fc[n] + 0.5f * lfe[n];
                l[n] += fl[n] + c3 + 0.7071f * bl[n];
                r[n] += fr[n] + c3 + 0.7071f * br[n];
            }
            if (g_spare.size() < kMaxClients * kMaxQueued) g_spare.push_back(std::move(c.frames.front()));
            c.frames.pop_front();
            who |= uint8_t(1u << i);
        }
    }
    if (!who) return false;
    int16_t* out = static_cast<int16_t*>(s.buf.buffer);
    for (int n = 0; n < kSamples; ++n) {
        g_peak = std::max(g_peak, std::max(std::fabs(l[n]), std::fabs(r[n])));
        out[2 * n] =int16_t(std::clamp(l[n], -1.0f, 1.0f) * 32767.0f);
        out[2 * n + 1] = int16_t(std::clamp(r[n], -1.0f, 1.0f) * 32767.0f);
    }
    s.buf.data_size = kDataBytes;
    s.clients = who;
    return true;
}

// A buffer audout finished playing: its clients may render their next frame.
void released(AudioOutBuffer* b) {
    for (Slot& s : g_slots) {
        if (&s.buf != b) continue;
        s.queued = false;
        EndFn ends[kMaxClients] = {};
        {
            std::lock_guard<std::mutex> lk(g_mx);
            for (int i = 0; i < kMaxClients; ++i)
                if ((s.clients & (1u << i)) && g_clients[i].open) ends[i] = g_clients[i].on_end;
        }
        for (int i = 0; i < kMaxClients; ++i) if (ends[i]) ends[i](i);
        s.clients = 0;
        return;
    }
}

void output_thread() {
    // Core 2 (idle outside shader builds), above the compile worker there.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
    lsw_prof_register("audio_output", -1);
    uint64_t played = 0, underruns = 0;
    for (;;) {
        // Collect what the device finished.
        AudioOutBuffer* rel = nullptr;
        u32 count = 0;
        while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &count)) && count && rel) {
            released(rel);
            ++played;
            rel = nullptr;
        }
        // Queue every free slot we have data for.
        const bool hold = g_hold.load(std::memory_order_relaxed);
        for (Slot& s : g_slots) {
            if (s.queued) continue;
            if (hold || !fill_slot(s)) break;
            if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&s.buf))) s.queued = true;
            else released(&s.buf);   // keep the client's credit chain alive
        }
        // Filling may stop at an empty slot before later slots still playing.
        const bool any_queued = std::any_of(g_slots, g_slots + kBuffers,
                                          [](const Slot& s) { return s.queued; });
        {
            const uint64_t now = armTicksToNs(armGetSystemTick());
            const uint32_t queued = uint32_t(std::count_if(g_slots, g_slots + kBuffers,
                                                           [](const Slot& s) { return s.queued; }));
            g_aw.min_queued = std::min(g_aw.min_queued, queued);
            if (!any_queued && !hold) {
                if (!g_aw.empty_since_ns) { g_aw.empty_since_ns = now; ++g_aw.empties; }
            } else if (g_aw.empty_since_ns) {
                const uint64_t gap = now - g_aw.empty_since_ns;
                g_aw.empty_ns += gap;
                g_total_empty_ns.fetch_add(gap, std::memory_order_relaxed);
                g_aw.empty_max_ns = std::max(g_aw.empty_max_ns, gap);
                g_aw.empty_since_ns = 0;
            }
        }
        if (any_queued) {
            // Wait for the device to release one (it plays 5.33 ms per buffer).
            rel = nullptr; count = 0;
            if (R_SUCCEEDED(audoutWaitPlayFinish(&rel, &count, 20000000ull)) && count && rel) {
                released(rel);
                ++played;
            }
        } else {
            // Nothing playing: wait for a submitted frame (or the end of an A/V hold).
            std::unique_lock<std::mutex> lk(g_mx);
            const auto ready = [] {
                return !g_hold.load(std::memory_order_relaxed) &&
                    std::any_of(g_clients, g_clients + kMaxClients,
                                [](const Client& c) { return c.open && !c.frames.empty(); });
            };
            if (!ready()) {
                if (!g_hold.load(std::memory_order_relaxed)) ++underruns;
                g_cv.wait_for(lk, std::chrono::milliseconds(20), ready);
            }
        }
        g_total_played.store(played, std::memory_order_relaxed);
        static uint64_t next_log = 1875;   // ~10 s of buffers
        if (played >= next_log) {
            next_log = played + 1875;
            printf("[AUDIO] %llu buffers played, %llu idle waits, A/V holds %llu (%llu ms), peak %.4f\n", (unsigned long long)played,
                   (unsigned long long)underruns, (unsigned long long)g_hold_count,
                   (unsigned long long)(g_hold_ns.load() / 1000000ull), double(g_peak));
            g_peak = 0;
            uint64_t submit_gap_ns;
            uint32_t depth_max;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                submit_gap_ns = g_submit_gap_max_ns; depth_max = g_client_depth_max;
                g_submit_gap_max_ns = 0; g_client_depth_max = 0;
            }
            const uint64_t now = armTicksToNs(armGetSystemTick());
            printf("[AUDIOQ] t=%llums window=%llums empties=%u empty=%llums empty_max=%llums min_queued=%u/%d "
                   "submit_gap_max=%.1fms client_depth_max=%u\n",
                   (unsigned long long)(now / 1000000ull),
                   (unsigned long long)(g_aw.start_ns ? (now - g_aw.start_ns) / 1000000ull : 0ull), g_aw.empties,
                   (unsigned long long)(g_aw.empty_ns / 1000000ull), (unsigned long long)(g_aw.empty_max_ns / 1000000ull),
                   g_aw.min_queued, kBuffers, submit_gap_ns / 1e6, depth_max);
            g_aw = AudioWindow{};
            g_aw.start_ns = now;
            fflush(stdout);
        }
    }
}

bool engine_up() {
    if (g_state >= 0) return g_state == 1;
    g_state = 0;
    const char* e = getenv("LSWTCS_AUDIO_OUT");
    if (e && e[0] == '0') { printf("[AUDIO] host output disabled (LSWTCS_AUDIO_OUT=0)\n"); return false; }
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) { printf("[AUDIO] audoutInitialize failed 0x%X\n", rc); fflush(stdout); return false; }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) { printf("[AUDIO] audoutStartAudioOut failed 0x%X\n", rc); fflush(stdout); audoutExit(); return false; }
    for (Slot& s : g_slots) {
        s.buf.buffer = aligned_alloc(kBufferBytes, kBufferBytes);
        if (!s.buf.buffer) { printf("[AUDIO] buffer alloc failed\n"); return false; }
        memset(s.buf.buffer, 0, kBufferBytes);
        s.buf.buffer_size = kBufferBytes;
        s.buf.data_size = kDataBytes;
        s.buf.data_offset = 0;
    }
    std::thread(output_thread).detach();
    g_state = 1;
    printf("[AUDIO] audout ready: %u Hz, %u channels, format %d (5.1 float frames downmixed to stereo s16)\n",
           audoutGetSampleRate(), audoutGetChannelCount(), int(audoutGetPcmFormat()));
    fflush(stdout);
    return true;
}
}  // namespace

extern "C" void lsw_audio_stats(uint64_t* played, uint64_t* empty_ns) {
    *played = g_total_played.load(std::memory_order_relaxed);
    *empty_ns = g_total_empty_ns.load(std::memory_order_relaxed);
}

// Returns true if host output exists; on_end(idx) is then called once per played frame.
bool host_audio_open(int idx, EndFn on_end) {
    if (idx < 0 || idx >= kMaxClients || !engine_up()) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    g_clients[idx].open = true;
    g_clients[idx].on_end = on_end;
    g_clients[idx].frames.clear();
    dbg_ram("[AUDIO] client %d opened (audout)\n", idx);
    return true;
}

// frame_be = host-mapped guest pointer to 256x6 planar big-endian floats.
void host_audio_submit(int idx, const uint8_t* frame_be) {
    if (idx < 0 || idx >= kMaxClients) return;
    std::vector<float> f;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_spare.empty()) { f = std::move(g_spare.back()); g_spare.pop_back(); }
    }
    f.resize(kFrameFloats);
    const uint32_t* in = reinterpret_cast<const uint32_t*>(frame_be);
    uint32_t* o = reinterpret_cast<uint32_t*>(f.data());
    lsw_guest::read(frame_be, kFrameFloats * 4, [&] {
        for (uint32_t i = 0; i < kFrameFloats; ++i) o[i] = __builtin_bswap32(in[i]);
    });
    EndFn drop = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        Client& c = g_clients[idx];
        if (!c.open) return;
        if (c.frames.size() >= kMaxQueued) drop = c.on_end;   // should not happen: count it as played
        else c.frames.push_back(std::move(f));
        const uint64_t now = armTicksToNs(armGetSystemTick());
        if (g_last_submit_ns) g_submit_gap_max_ns = std::max(g_submit_gap_max_ns, now - g_last_submit_ns);
        g_last_submit_ns = now;
        g_client_depth_max = std::max(g_client_depth_max, uint32_t(c.frames.size()));
    }
    if (drop) drop(idx);
    g_cv.notify_one();
}

// Render thread: pause (on) / resume (off) the device feed around a shader-program hold.
void host_audio_hold(bool on) {
    static const bool enabled = [] { const char* e = getenv("LSWTCS_AVHOLD"); return !(e && e[0] == '0'); }();
    static uint64_t t0 = 0;
    if (!enabled || g_state != 1 || g_hold.load() == on) return;
    const uint64_t now = armTicksToNs(armGetSystemTick());
    if (on) { t0 = now; ++g_hold_count; }
    else g_hold_ns.fetch_add(now - t0);
    {   // The readiness predicate and every writer must share the wait mutex.
        std::lock_guard<std::mutex> lk(g_mx);
        g_hold.store(on);
    }
    if (!on) g_cv.notify_one();
}

void host_audio_close(int idx) {
    if (idx < 0 || idx >= kMaxClients) return;
    std::lock_guard<std::mutex> lk(g_mx);
    g_clients[idx].open = false;
    g_clients[idx].on_end = nullptr;
    g_clients[idx].frames.clear();
}
