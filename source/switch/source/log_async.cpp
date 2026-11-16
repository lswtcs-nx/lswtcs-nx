// Log files and capture files without SD writes on the threads that produce them.
// Run 6 (hardware, 2026-11-01): [SCHEDPROF] flushes stdout while holding the guest scheduler's mutex, the audio output
// thread flushes it every 10 s, and a capture combo's file access held the game for 1-2.6 s. Paths under "alog:/" go to
// a ring in RAM here, so fflush only copies, and one thread does the SD writes. A full ring blocks its writer: slower,
// but nothing is lost. lsw_bg_submit runs slower jobs (PNG encoding and writing) on a second thread.
#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

extern "C" void lsw_prof_register(const char* name, int sched_id);   // hw_profiler.cpp
extern "C" int lsw_core3_begin(const char* name);                      // platform_switch.cpp: fourth core, when there is one
extern "C" int lsw_core3_move_thread(Handle handle, const char* name); // the same for another thread

namespace {
struct Stream {
    int fd = -1;
    char* buf = nullptr;
    size_t cap = 0;
    std::atomic<uint64_t> head{0}, tail{0};   // bytes produced / written: one producer (the FILE's lock) and the writer
};
constexpr int kStreams = 4;
Stream g_streams[kStreams];
Mutex g_write_lock;   // held across an SD write; the crash path takes it with a timeout

size_t ring_size_for(const char* path) {
    if (strstr(path, "stdout")) return size_t(4) << 20;   // a capture's draw log is under 1 MB
    if (strstr(path, "frames")) return size_t(512) << 10;
    return size_t(64) << 10;
}

// Writes what is queued now. Caller holds g_write_lock, or is the crash path after its timeout.
bool stream_write(Stream& s) {
    const uint64_t head = s.head.load(std::memory_order_acquire);
    uint64_t tail = s.tail.load(std::memory_order_relaxed);
    if (head == tail) return false;
    while (tail != head) {
        const size_t at = size_t(tail % s.cap);
        const size_t n = size_t(std::min<uint64_t>(head - tail, s.cap - at));
        const ssize_t w = write(s.fd, s.buf + at, n);
        if (w <= 0) tail += n;   // SD error: drop the bytes, never block the game on them
        else tail += uint64_t(w);
        s.tail.store(tail, std::memory_order_release);
    }
    return true;
}

Thread g_writer;   // log99: moved to core 3 from outside, after env.txt is read (it starts before)
void writer_main(void*) {
    lsw_prof_register("log_writer", -1);
    for (;;) {
        bool any = false;
        mutexLock(&g_write_lock);
        for (Stream& s : g_streams) if (s.fd >= 0) any |= stream_write(s);
        mutexUnlock(&g_write_lock);
        if (!any) svcSleepThread(50000000);
    }
}

int alog_open(struct _reent* r, void* file, const char* path, int flags, int mode) {
    if (const char* colon = strchr(path, ':')) path = colon + 1;
    if (path[0] == '/') ++path;   // relative to the data directory, as before
    for (Stream& s : g_streams) {
        if (s.fd >= 0) continue;
        const size_t cap = ring_size_for(path);
        char* buf = static_cast<char*>(malloc(cap));
        if (!buf) { r->_errno = ENOMEM; return -1; }
        const int fd = open(path, flags, mode);
        if (fd < 0) { free(buf); r->_errno = errno; return -1; }
        s.buf = buf; s.cap = cap;
        s.head.store(0); s.tail.store(0);
        s.fd = fd;
        *static_cast<Stream**>(file) = &s;
        return 0;
    }
    r->_errno = EMFILE;
    return -1;
}
int alog_close(struct _reent*, void* file) {
    Stream& s = **static_cast<Stream**>(file);
    mutexLock(&g_write_lock);
    if (s.fd >= 0) { stream_write(s); close(s.fd); s.fd = -1; free(s.buf); s.buf = nullptr; }
    mutexUnlock(&g_write_lock);
    return 0;
}
ssize_t alog_write(struct _reent*, void* file, const char* ptr, size_t len) {
    Stream& s = **static_cast<Stream**>(file);
    uint64_t head = s.head.load(std::memory_order_relaxed);
    size_t left = len;
    while (left) {
        const uint64_t tail = s.tail.load(std::memory_order_acquire);
        const size_t room = s.cap - size_t(head - tail);
        if (!room) { svcSleepThread(1000000); continue; }
        const size_t at = size_t(head % s.cap);
        const size_t n = std::min(std::min(left, room), s.cap - at);
        memcpy(s.buf + at, ptr, n);
        ptr += n; left -= n; head += n;
        s.head.store(head, std::memory_order_release);
    }
    return ssize_t(len);
}
int alog_fstat(struct _reent*, void*, struct stat* st) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG;
    return 0;
}
devoptab_t g_device;
bool g_device_ok = false;

// ---- background jobs ----------------------------------------------------------------------------
struct Job { size_t bytes; std::function<void()> run; bool capture; };
std::mutex g_job_mx;
std::condition_variable g_job_cv;
std::deque<Job> g_jobs;
size_t g_job_bytes = 0;
constexpr size_t kJobBytesMax = size_t(192) << 20;   // beyond this the caller drops the work

// What one capture's files cost, for [CAPSAVE]. Only the background thread touches these.
struct CaptureCost { uint64_t written, writes, write_ns, longest_ns, file_ns; };
CaptureCost g_cost;
inline uint64_t now_ns() { return armTicksToNs(armGetSystemTick()); }

void job_main() {
    // Core 1, below the render thread: it waits for the GPU for much of a frame. On core 2, under the audio mix and Mesa's
    // worker, one full capture was not done a minute later (run 7), and leaving the game lost the rest.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1u << 1);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3A);
    lsw_core3_begin("bg_files");
    lsw_prof_register("bg_files", -1);
    uint64_t done = 0, batch_ns = 0, batch_bytes = 0;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_job_mx);
            g_job_cv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        if (job.capture && !batch_bytes) {
            batch_ns = now_ns();
            g_cost = CaptureCost{};
        }
        job.run();
        ++done;
        if (job.capture) batch_bytes += job.bytes;
        bool empty;
        {
            std::lock_guard<std::mutex> lk(g_job_mx);
            g_job_bytes -= job.bytes;
            empty = g_jobs.empty();
        }
        if (empty && batch_bytes) {
            printf("[CAPSAVE] all capture files written (%llu so far): %.0f MB in %.1f s; %.0f MB on the card, %llu writes, %.1f s writing, longest %.0f ms, %.1f s open/size/close\n",
                   (unsigned long long)done, batch_bytes / 1048576.0, (now_ns() - batch_ns) / 1e9, g_cost.written / 1048576.0, (unsigned long long)g_cost.writes,
                   g_cost.write_ns / 1e9, g_cost.longest_ns / 1e6, g_cost.file_ns / 1e9);
            fflush(stdout);
            batch_bytes = 0;
        }
    }
}
}  // namespace

// Capture files, on the background thread. Setting the size first spares the file system growing the file on every write; the guess may be high or low.
int lsw_capture_open(const char* path, size_t size_guess) {
    const uint64_t t = now_ns();
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0 && size_guess) (void)!ftruncate(fd, off_t(size_guess));
    g_cost.file_ns += now_ns() - t;
    return fd;
}
bool lsw_capture_write(int fd, const uint8_t* data, size_t size) {
    const uint64_t t = now_ns();
    size_t left = size;
    while (left) {
        const ssize_t w = write(fd, data, left);
        if (w <= 0) return false;
        data += w;
        left -= size_t(w);
    }
    const uint64_t took = now_ns() - t;
    g_cost.written += size;
    ++g_cost.writes;
    g_cost.write_ns += took;
    g_cost.longest_ns = std::max(g_cost.longest_ns, took);
    return true;
}
void lsw_capture_close(int fd, size_t final_size) {
    const uint64_t t = now_ns();
    (void)!ftruncate(fd, off_t(final_size));
    close(fd);
    g_cost.file_ns += now_ns() - t;
}

extern "C" bool lsw_log_async_init() {
    mutexInit(&g_write_lock);
    g_device.name = "alog";
    g_device.structSize = sizeof(Stream*);
    g_device.open_r = alog_open;
    g_device.close_r = alog_close;
    g_device.write_r = alog_write;
    g_device.fstat_r = alog_fstat;
    if (AddDevice(&g_device) < 0) return false;
    // Core 2 at 0x2C: below the audio output thread (0x2B), above the shader-compile worker (0x2D), which could otherwise
    // keep the core for seconds. It spends its time waiting on the SD card.
    if (R_FAILED(threadCreate(&g_writer, writer_main, nullptr, nullptr, 0x8000, 0x2C, 2)) || R_FAILED(threadStart(&g_writer))) return false;
    g_device_ok = true;
    return true;
}

// Called once env.txt is read: the log writer moves to core 3 when the fourth core is on.
extern "C" void lsw_log_async_core3() {
    if (g_device_ok) lsw_core3_move_thread(g_writer.handle, "log_writer");
}

// `name` as "alog:/name" when the ring is up, else unchanged.
extern "C" void lsw_log_async_path(char* out, size_t capacity, const char* name) {
    snprintf(out, capacity, g_device_ok ? "alog:/%s" : "%s", name);
}

// Before the process ends: everything queued goes to the SD card. The crash path waits at most 200 ms for the writer.
extern "C" void lsw_log_async_drain() {
    if (!g_device_ok) return;
    bool locked = false;
    for (int i = 0; i < 200 && !(locked = mutexTryLock(&g_write_lock)); ++i) svcSleepThread(1000000);
    for (Stream& s : g_streams) if (s.fd >= 0) stream_write(s);
    if (locked) mutexUnlock(&g_write_lock);
}

size_t lsw_bg_queued_bytes() {
    std::lock_guard<std::mutex> lk(g_job_mx);
    return g_job_bytes;
}

// Runs `run` on the background thread, one job at a time, so its files do not compete for the SD card; false = over the
// memory bound. `bytes` is what the job keeps in memory.
static bool bg_submit(size_t bytes, std::function<void()> run, bool capture) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(job_main).detach(); });
    {
        std::lock_guard<std::mutex> lk(g_job_mx);
        if (g_job_bytes + bytes > kJobBytesMax) return false;
        g_job_bytes += bytes;
        g_jobs.push_back({bytes, std::move(run), capture});
    }
    g_job_cv.notify_one();
    return true;
}
// A job with bytes > 0 counts as a capture's in [CAPSAVE].
bool lsw_bg_submit(size_t bytes, std::function<void()> run) { return bg_submit(bytes, std::move(run), bytes != 0); }
// The same queue and bound for file work that is not a capture.
bool lsw_bg_submit_other(size_t bytes, std::function<void()> run) { return bg_submit(bytes, std::move(run), false); }
