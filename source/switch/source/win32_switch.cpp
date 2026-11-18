// Win32 subset on libnx/newlib: see switch/include/win32/windows.h for the contract.
#include <switch.h>
#include "windows.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>
#include <string>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include "lsw_save_platform.h"
#include "lsw_input_script.h"
std::atomic<uint32_t> g_frame_marks{0};   // tester marks, written to frames.log by gl_backend.cpp
std::atomic<uint32_t> g_capture_request{0};   // capture combos: 1 light, 2 full; taken by gl_backend.cpp
std::atomic<uint32_t> g_ab_toggle_request{0};   // A/B toggle combo (L + R + left stick), taken by gl_backend.cpp (LSWTCS_ABTOGGLE)
std::atomic<bool> g_frame_light{false};   // set by gl_backend.cpp: the frames are light (title, menus, loading); pad layout "auto"

namespace {

thread_local DWORD t_last_error = 0;
std::atomic<DWORD> g_next_tid{1};
thread_local DWORD t_tid = 0;

enum class Kind { File, Dir, Find, Event, Semaphore, Mutex, Timer };

struct Obj { Kind kind; explicit Obj(Kind k) : kind(k) {} virtual ~Obj() = default; };

struct FileObj : Obj {
    int fd;
    std::string path;
    int access;
    bool native_save;
    bool time_marked = false;   // save timestamp written for this handle (save_time_mark)
    off_t position = 0;
    std::mutex mutex;
    FileObj(int f, std::string p, int a) : Obj(Kind::File), fd(f), path(std::move(p)), access(a),
        native_save(path.compare(0, strlen(lsw_save::root), lsw_save::root) == 0) {
        // Horizon cannot commit save data with writable files still open. Preserve a logical
        // handle/position, and open the native file only for the duration of each operation.
        if (native_save) { ::close(fd); fd = -1; }
    }
    ~FileObj() override { if (fd >= 0) ::close(fd); }
};
struct DirObj : Obj { std::string path; explicit DirObj(std::string p) : Obj(Kind::Dir), path(std::move(p)) {} };
struct FindObj : Obj {
    DIR* d; std::string dir, pat;
    FindObj(DIR* dd, std::string di, std::string pa) : Obj(Kind::Find), d(dd), dir(std::move(di)), pat(std::move(pa)) {}
    ~FindObj() override { if (d) closedir(d); }
};

// All waitable state is guarded by one dispatcher lock; any state change notifies every waiter,
// which re-evaluates its own condition (Win32 dispatcher-database model).
std::mutex g_disp;
std::condition_variable g_disp_cv;
using Clock = std::chrono::steady_clock;

struct WaitObj : Obj {
    bool signaled = false, manual = false;
    uint64_t pulse_gen = 0;   // manual-reset PulseEvent: releases waiters that saw the old value
    int waiters = 0;
    LONG count = 0, max = 0;  // semaphore
    DWORD owner = 0; DWORD rec = 0;   // mutex (owner thread id, recursion)
    Clock::time_point due{}; bool armed = false; int64_t period_ms = 0;   // waitable timer
    explicit WaitObj(Kind k) : Obj(k) {}
};

DWORD tid() { if (!t_tid) t_tid = g_next_tid.fetch_add(1); return t_tid; }

Obj* obj(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || (intptr_t)h == -2) return nullptr;
    return static_cast<Obj*>(h);
}
WaitObj* waitable(HANDLE h) {
    Obj* o = obj(h);
    if (!o || o->kind == Kind::File || o->kind == Kind::Dir || o->kind == Kind::Find) return nullptr;
    return static_cast<WaitObj*>(o);
}
FileObj* file(HANDLE h) { Obj* o = obj(h); return (o && o->kind == Kind::File) ? static_cast<FileObj*>(o) : nullptr; }
struct FileAccess {
    FileObj* f;
    std::unique_lock<std::mutex> lock;
    explicit FileAccess(FileObj* object) : f(object) {
        if (!f) return;
        lock = std::unique_lock<std::mutex>(f->mutex);
        if (f->native_save) {
            f->fd = ::open(f->path.c_str(), f->access);
            if (f->fd >= 0 && ::lseek(f->fd, f->position, SEEK_SET) < 0) {
                ::close(f->fd); f->fd = -1;
            }
        }
    }
    FileObj* get() const { return f && f->fd >= 0 ? f : nullptr; }
    ~FileAccess() {
        if (f && f->native_save && f->fd >= 0) {
            const off_t p = ::lseek(f->fd, 0, SEEK_CUR);
            if (p >= 0) f->position = p;
            ::close(f->fd); f->fd = -1;
        }
    }
};

// Timer due -> signalled (caller holds g_disp).
void timer_tick(WaitObj* w, Clock::time_point now) {
    if (w->kind != Kind::Timer || !w->armed || now < w->due) return;
    w->signaled = true;
    if (w->period_ms > 0) { do w->due += std::chrono::milliseconds(w->period_ms); while (w->due <= now); }
    else w->armed = false;
}
bool ready(WaitObj* w, DWORD me, Clock::time_point now) {
    switch (w->kind) {
    case Kind::Event: return w->signaled;
    case Kind::Semaphore: return w->count > 0;
    case Kind::Mutex: return w->owner == 0 || w->owner == me;
    case Kind::Timer: timer_tick(w, now); return w->signaled;
    default: return false;
    }
}
void consume(WaitObj* w, DWORD me) {
    switch (w->kind) {
    case Kind::Event: if (!w->manual) w->signaled = false; break;
    case Kind::Semaphore: --w->count; break;
    case Kind::Mutex: w->owner = me; ++w->rec; break;
    case Kind::Timer: if (!w->manual) w->signaled = false; break;
    default: break;
    }
}

std::string norm(const char* p) {
    std::string s = p ? p : "";
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
void set_err_from_errno(const std::string& path) {
    switch (errno) {
    case ENOENT: {
        size_t sl = path.find_last_of('/');
        struct stat st;
        t_last_error = (sl != std::string::npos && sl > 0 && ::stat(path.substr(0, sl).c_str(), &st) != 0)
                           ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND;
        break;
    }
    case ENOTDIR: t_last_error = ERROR_PATH_NOT_FOUND; break;
    case EEXIST: t_last_error = ERROR_FILE_EXISTS; break;
    default: t_last_error = ERROR_ACCESS_DENIED; break;
    }
}

constexpr uint64_t kFileTimeUnixEpoch = 116444736000000000ull;   // 1601 -> 1970 in 100 ns units
FILETIME to_ft(uint64_t v) { FILETIME f; f.dwLowDateTime = DWORD(v); f.dwHighDateTime = DWORD(v >> 32); return f; }
uint64_t from_ft(const FILETIME* f) { return (uint64_t(f->dwHighDateTime) << 32) | f->dwLowDateTime; }
FILETIME unix_to_ft(time_t t) { return to_ft(uint64_t(t) * 10000000ull + kFileTimeUnixEpoch); }

// Gregorian calendar <-> day count since 1970-01-01 (H. Hinnant's algorithms).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = int64_t(yoe) + era * 400 + (*m <= 2);
}

// Save timestamps (log65). Horizon's save data file system keeps no file times (stat and fstat give 0), so the Load Game screen,
// which shows a save's last write time (GetFileTime), read 1970. On the first write through a handle into the native save, the
// console's time (UTC) is kept beside the file in "<name>.lswtime" (8 bytes, a Win32 FILETIME) and beside each folder up to the
// root, in the same save transaction. Those files are hidden from enumeration, deleted with their file or folder, and read back
// as the file times. A save written before log65 has none and still reads its old time until it is saved again.
constexpr const char* kSaveTimeExt = ".lswtime";
bool save_path(const std::string& p) { return p.compare(0, strlen(lsw_save::root), lsw_save::root) == 0; }
bool save_time_name(const char* name) {
    const size_t n = strlen(name), e = strlen(kSaveTimeExt);
    return n >= e && strcasecmp(name + n - e, kSaveTimeExt) == 0;
}
std::string save_trim(std::string p) {
    const size_t root = strlen(lsw_save::root);
    while (p.size() > root && p.back() == '/') p.pop_back();
    return p;
}
FILETIME file_time(const std::string& path, time_t mtime) {
    if (save_path(path)) {
        if (FILE* f = fopen((save_trim(path) + kSaveTimeExt).c_str(), "rb")) {
            uint64_t v = 0;
            const bool ok = fread(&v, sizeof v, 1, f) == 1;
            fclose(f);
            if (ok && v) return to_ft(v);
        }
    }
    return unix_to_ft(mtime);
}
void save_time_mark(const std::string& path) {
    FILETIME now; GetSystemTimeAsFileTime(&now);
    const uint64_t v = from_ft(&now);
    const size_t root = strlen(lsw_save::root);
    std::string p = save_trim(path);
    while (p.size() > root) {
        if (FILE* f = fopen((p + kSaveTimeExt).c_str(), "wb")) { fwrite(&v, sizeof v, 1, f); fclose(f); }
        const size_t sl = p.find_last_of('/');
        if (sl == std::string::npos || sl < root) break;
        p.resize(sl);
    }
}
bool wild_match(const char* p, const char* s) {   // '*' and '?', case-insensitive
    if (!*p) return !*s;
    if (*p == '*') { for (; ; ++s) { if (wild_match(p + 1, s)) return true; if (!*s) return false; } }
    if (!*s) return false;
    if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
    return wild_match(p + 1, s + 1);
}
bool find_fill(FindObj* f, WIN32_FIND_DATAA* fd) {
    while (dirent* e = readdir(f->d)) {
        if (!wild_match(f->pat.c_str(), e->d_name)) continue;
        if (save_time_name(e->d_name) && save_path(f->dir)) continue;   // save timestamps stay hidden
        memset(fd, 0, sizeof *fd);
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        struct stat st;
        std::string full = f->dir + e->d_name;
        if (::stat(full.c_str(), &st) == 0) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeHigh = DWORD(uint64_t(st.st_size) >> 32); fd->nFileSizeLow = DWORD(st.st_size);
            fd->ftLastWriteTime = fd->ftCreationTime = fd->ftLastAccessTime = file_time(full, st.st_mtime);
        } else fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        return true;
    }
    t_last_error = ERROR_NO_MORE_FILES;
    return false;
}

}  // namespace

// ---- errors ------------------------------------------------------------------------------------
DWORD GetLastError(void) { return t_last_error; }
void SetLastError(DWORD e) { t_last_error = e; }

// ---- files -------------------------------------------------------------------------------------
HANDLE CreateFileA(LPCSTR p, DWORD access, DWORD, void*, DWORD disp, DWORD flags, HANDLE) {
    std::string path = norm(p);
    struct stat st;
    bool exists = ::stat(path.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) { t_last_error = ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
        t_last_error = 0;
        return new DirObj(path);
    }
    int of = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING: of |= O_TRUNC; break;
    default: break;
    }
    int fd = ::open(path.c_str(), of, 0666);
    if (fd < 0) { set_err_from_errno(path); return INVALID_HANDLE_VALUE; }
    t_last_error = (exists && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0;
    auto* obj = new FileObj(fd, path, of & O_ACCMODE);
    if (obj->native_save && ((of & O_TRUNC) || (!exists && (of & O_CREAT)))) { save_time_mark(path); obj->time_marked = true; }
    return obj;
}
BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, void*) {
    FileAccess access(file(h)); FileObj* f = access.get();
    if (got) *got = 0;
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    DWORD total = 0;
    while (total < n) {
        ssize_t r = ::read(f->fd, (char*)buf + total, n - total);
        if (r < 0) { if (errno == EINTR) continue; t_last_error = ERROR_ACCESS_DENIED; if (got) *got = total; return FALSE; }
        if (r == 0) break;
        total += DWORD(r);
    }
    if (got) *got = total;
    return TRUE;
}
BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* wrote, void*) {
    FileAccess access(file(h)); FileObj* f = access.get();
    if (wrote) *wrote = 0;
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    DWORD total = 0;
    while (total < n) {
        ssize_t r = ::write(f->fd, (const char*)buf + total, n - total);
        if (r < 0) { if (errno == EINTR) continue; t_last_error = ERROR_ACCESS_DENIED; if (wrote) *wrote = total; return FALSE; }
        if (!r) { t_last_error = ERROR_ACCESS_DENIED; if (wrote) *wrote = total; return FALSE; }
        total += DWORD(r);
    }
    if (f->native_save && ::fsync(f->fd) != 0) { t_last_error = ERROR_ACCESS_DENIED; if (wrote) *wrote = total; return FALSE; }
    if (f->native_save && !f->time_marked) { save_time_mark(f->path); f->time_marked = true; }
    if (wrote) *wrote = total;
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newpos, DWORD method) {
    FileAccess access(file(h)); FileObj* f = access.get();
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    static_assert(sizeof(off_t) == 8, "64-bit file offsets required (GAME.DAT > 2 GB)");
    off_t r = ::lseek(f->fd, off_t(dist.QuadPart), method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) { t_last_error = ERROR_ACCESS_DENIED; return FALSE; }
    if (newpos) newpos->QuadPart = r;
    return TRUE;
}
DWORD SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD method) {
    LARGE_INTEGER d, n;
    d.QuadPart = hi ? ((LONGLONG)*hi << 32) | DWORD(lo) : (LONGLONG)lo;
    if (!SetFilePointerEx(h, d, &n, method)) return INVALID_SET_FILE_POINTER;
    if (hi) *hi = n.HighPart;
    return n.LowPart;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* sz) {
    FileAccess access(file(h)); FileObj* f = access.get(); struct stat st;
    if (!f || ::fstat(f->fd, &st) != 0) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    sz->QuadPart = st.st_size;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
    FileAccess access(file(h)); FileObj* f = access.get();
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    off_t cur = ::lseek(f->fd, 0, SEEK_CUR);
    return cur >= 0 && ::ftruncate(f->fd, cur) == 0;
}
BOOL FlushFileBuffers(HANDLE h) { FileAccess access(file(h)); FileObj* f = access.get(); return f && ::fsync(f->fd) == 0; }
BOOL GetFileTime(HANDLE h, FILETIME* c, FILETIME* a, FILETIME* w) {
    Obj* o = obj(h); struct stat st;
    int rc = -1;
    std::string path;
    if (o && o->kind == Kind::File) {
        FileAccess access(static_cast<FileObj*>(o));
        if (FileObj* f = access.get()) { rc = ::fstat(f->fd, &st); path = f->path; }
    }
    else if (o && o->kind == Kind::Dir) { path = static_cast<DirObj*>(o)->path; rc = ::stat(path.c_str(), &st); }
    if (rc != 0) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    FILETIME t = file_time(path, st.st_mtime);
    if (c) *c = t; if (a) *a = t; if (w) *w = t;
    return TRUE;
}
DWORD GetFileAttributesA(LPCSTR p) {
    std::string path = norm(p); struct stat st;
    if (::stat(path.c_str(), &st) != 0) { set_err_from_errno(path); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS, void* out) {
    std::string path = norm(p); struct stat st;
    if (::stat(path.c_str(), &st) != 0) { set_err_from_errno(path); return FALSE; }
    auto* fa = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(out);
    fa->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    fa->ftCreationTime = fa->ftLastAccessTime = fa->ftLastWriteTime = file_time(path, st.st_mtime);
    fa->nFileSizeHigh = DWORD(uint64_t(st.st_size) >> 32); fa->nFileSizeLow = DWORD(st.st_size);
    return TRUE;
}
BOOL DeleteFileA(LPCSTR p) {
    std::string path = norm(p);
    if (::unlink(path.c_str()) == 0) {
        if (save_path(path)) ::unlink((save_trim(path) + kSaveTimeExt).c_str());
        return TRUE;
    }
    set_err_from_errno(path);
    return FALSE;
}
BOOL CreateDirectoryA(LPCSTR p, void*) {
    std::string path = norm(p);
    if (::mkdir(path.c_str(), 0777) == 0) return TRUE;
    if (errno == EEXIST) t_last_error = ERROR_ALREADY_EXISTS; else set_err_from_errno(path);
    return FALSE;
}
BOOL RemoveDirectoryA(LPCSTR p) {
    std::string path = norm(p);
    if (save_path(path)) {   // the hidden timestamps inside would keep the folder from being empty
        const std::string dir = save_trim(path) + "/";
        if (DIR* d = ::opendir(dir.c_str())) {
            std::vector<std::string> names;
            while (dirent* e = readdir(d)) if (save_time_name(e->d_name)) names.push_back(dir + e->d_name);
            closedir(d);
            for (const std::string& n : names) ::unlink(n.c_str());
        }
    }
    if (::rmdir(path.c_str()) == 0) {
        if (save_path(path)) ::unlink((save_trim(path) + kSaveTimeExt).c_str());
        return TRUE;
    }
    set_err_from_errno(path);
    return FALSE;
}
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* fd) {
    std::string s = norm(pattern);
    size_t sl = s.find_last_of('/');
    std::string dir = sl == std::string::npos ? std::string("./") : s.substr(0, sl + 1);
    std::string pat = sl == std::string::npos ? s : s.substr(sl + 1);
    DIR* d = ::opendir(dir.c_str());
    if (!d) { set_err_from_errno(dir); return INVALID_HANDLE_VALUE; }
    auto* f = new FindObj(d, dir, pat);
    if (!find_fill(f, fd)) { delete f; t_last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    return f;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd) {
    Obj* o = obj(h);
    if (!o || o->kind != Kind::Find) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return find_fill(static_cast<FindObj*>(o), fd);
}
BOOL FindClose(HANDLE h) { Obj* o = obj(h); if (!o || o->kind != Kind::Find) return FALSE; delete o; return TRUE; }
BOOL CloseHandle(HANDLE h) {
    Obj* o = obj(h);
    if (!o) return (intptr_t)h == -2;   // pseudo handle
    if (o->kind == Kind::Event || o->kind == Kind::Semaphore || o->kind == Kind::Mutex || o->kind == Kind::Timer) {
        std::lock_guard<std::mutex> lk(g_disp);   // never free while a waiter is inside the dispatcher
        delete o;
    } else delete o;
    return TRUE;
}

// ---- synchronisation ---------------------------------------------------------------------------
HANDLE CreateEventA(void*, BOOL manual, BOOL initial, LPCSTR) {
    auto* w = new WaitObj(Kind::Event); w->manual = manual; w->signaled = initial; return w;
}
HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR) { return CreateEventA(sa, manual, initial, nullptr); }
BOOL SetEvent(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    { std::lock_guard<std::mutex> lk(g_disp); w->signaled = true; }
    g_disp_cv.notify_all();
    return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    std::lock_guard<std::mutex> lk(g_disp); w->signaled = false; return TRUE;
}
BOOL PulseEvent(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        if (w->manual) { ++w->pulse_gen; w->signaled = false; }   // releases everyone waiting now
        else if (w->waiters > 0) w->signaled = true;             // one waiter consumes it
    }
    g_disp_cv.notify_all();
    return TRUE;
}
HANDLE CreateSemaphoreA(void*, LONG initial, LONG maximum, LPCSTR) {
    auto* w = new WaitObj(Kind::Semaphore); w->count = initial; w->max = maximum; return w;
}
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG* prev) {
    WaitObj* w = waitable(h); if (!w || n <= 0) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        if (prev) *prev = w->count;
        if ((int64_t)w->count + n > w->max) return FALSE;   // ERROR_TOO_MANY_POSTS
        w->count += n;
    }
    g_disp_cv.notify_all();
    return TRUE;
}
HANDLE CreateMutexA(void*, BOOL owner, LPCSTR) {
    auto* w = new WaitObj(Kind::Mutex); if (owner) { w->owner = tid(); w->rec = 1; } return w;
}
BOOL ReleaseMutex(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        if (w->owner != tid()) return FALSE;
        if (--w->rec == 0) w->owner = 0;
    }
    g_disp_cv.notify_all();
    return TRUE;
}
HANDLE CreateWaitableTimerA(void*, BOOL manual, LPCSTR) { auto* w = new WaitObj(Kind::Timer); w->manual = manual; return w; }
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms, void*, void*, BOOL) {
    WaitObj* w = waitable(h); if (!w || !due) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        int64_t rel100ns;
        if (due->QuadPart < 0) rel100ns = -due->QuadPart;
        else { FILETIME now; GetSystemTimeAsFileTime(&now); rel100ns = due->QuadPart - (int64_t)from_ft(&now); if (rel100ns < 0) rel100ns = 0; }
        w->due = Clock::now() + std::chrono::nanoseconds(rel100ns * 100);
        w->armed = true; w->period_ms = period_ms; w->signaled = false;
    }
    g_disp_cv.notify_all();
    return TRUE;
}
BOOL CancelWaitableTimer(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    std::lock_guard<std::mutex> lk(g_disp); w->armed = false; return TRUE;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
    if (n == 0 || n > MAXIMUM_WAIT_OBJECTS) { t_last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
    WaitObj* w[MAXIMUM_WAIT_OBJECTS]; uint64_t gen[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < n; ++i) if (!(w[i] = waitable(hs[i]))) { t_last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
    const DWORD me = tid();
    const auto start = Clock::now();
    const auto deadline = ms == INFINITE ? Clock::time_point::max() : start + std::chrono::milliseconds(ms);
    std::unique_lock<std::mutex> lk(g_disp);
    for (DWORD i = 0; i < n; ++i) { gen[i] = w[i]->pulse_gen; ++w[i]->waiters; }
    DWORD result = WAIT_TIMEOUT;
    for (;;) {
        const auto now = Clock::now();
        auto ok = [&](DWORD i) { return ready(w[i], me, now) || w[i]->pulse_gen != gen[i]; };
        if (all) {
            bool every = true;
            for (DWORD i = 0; i < n && every; ++i) every = ok(i);
            if (every) { for (DWORD i = 0; i < n; ++i) if (w[i]->pulse_gen == gen[i]) consume(w[i], me); result = WAIT_OBJECT_0; break; }
        } else {
            DWORD hit = n;
            for (DWORD i = 0; i < n; ++i) if (ok(i)) { hit = i; break; }
            if (hit < n) { if (w[hit]->pulse_gen == gen[hit]) consume(w[hit], me); result = WAIT_OBJECT_0 + hit; break; }
        }
        if (now >= deadline) break;
        auto wake = deadline;
        for (DWORD i = 0; i < n; ++i) if (w[i]->kind == Kind::Timer && w[i]->armed && w[i]->due < wake) wake = w[i]->due;
        if (wake == Clock::time_point::max()) g_disp_cv.wait(lk);
        else g_disp_cv.wait_until(lk, wake);
    }
    for (DWORD i = 0; i < n; ++i) --w[i]->waiters;
    return result;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!waitable(h)) { t_last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }   // as Win32 for a bad handle
    return WaitForMultipleObjects(1, &h, FALSE, ms);
}

// ---- time --------------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)armGetSystemTick(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = (LONGLONG)armGetSystemTickFreq(); return TRUE; }
ULONGLONG GetTickCount64(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
void Sleep(DWORD ms) { svcSleepThread(ms ? (int64_t)ms * 1000000ll : (int64_t)YieldType_WithoutCoreMigration); }
BOOL SwitchToThread(void) { svcSleepThread(YieldType_WithoutCoreMigration); return TRUE; }
void GetSystemTimeAsFileTime(FILETIME* ft) {
    struct timeval tv; gettimeofday(&tv, nullptr);
    *ft = to_ft(uint64_t(tv.tv_sec) * 10000000ull + uint64_t(tv.tv_usec) * 10ull + kFileTimeUnixEpoch);
}
BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    uint64_t v = from_ft(ft);
    if (v >> 63) return FALSE;
    const int64_t secs1601 = int64_t(v / 10000000ull);
    const int64_t days = secs1601 / 86400 - 134774;   // days 1601-01-01 -> 1970-01-01
    int64_t sod = secs1601 % 86400;
    int64_t y; unsigned m, d; civil_from_days(days, &y, &m, &d);
    st->wYear = WORD(y); st->wMonth = WORD(m); st->wDay = WORD(d);
    st->wDayOfWeek = WORD(((days % 7) + 7 + 4) % 7);   // 1970-01-01 was a Thursday
    st->wHour = WORD(sod / 3600); st->wMinute = WORD(sod / 60 % 60); st->wSecond = WORD(sod % 60);
    st->wMilliseconds = WORD(v / 10000ull % 1000);
    return TRUE;
}
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
    if (st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31 || st->wHour > 23 || st->wMinute > 59 ||
        st->wSecond > 59 || st->wMilliseconds > 999 || st->wYear < 1601) return FALSE;
    const int64_t days = days_from_civil(st->wYear, st->wMonth, st->wDay) + 134774;
    const uint64_t secs = uint64_t(days) * 86400ull + st->wHour * 3600ull + st->wMinute * 60ull + st->wSecond;
    *ft = to_ft(secs * 10000000ull + st->wMilliseconds * 10000ull);
    return TRUE;
}
// The console's time zone (System Settings) via the time service's own rule. Bias = the offset in
// effect now, DST included, and no transition dates (the guest then never switches to daylight
// time itself). Was all zeros: the Load Game screen showed save times in UTC.
DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION* tz) {
    memset(tz, 0, sizeof *tz);
    u64 now = 0;
    TimeCalendarTime ct{}; TimeCalendarAdditionalInfo ci{};
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &now)) || R_FAILED(timeToCalendarTimeWithMyRule(now, &ct, &ci)))
        return TIME_ZONE_ID_UNKNOWN;
    tz->Bias = -ci.offset / 60;   // UTC = local + Bias (minutes)
    for (int i = 0; i < 7 && ci.timezoneName[i]; ++i) tz->StandardName[i] = tz->DaylightName[i] = WCHAR(ci.timezoneName[i]);
    return TIME_ZONE_ID_UNKNOWN;
}
DWORD timeBeginPeriod(DWORD) { return 0; }
DWORD timeEndPeriod(DWORD) { return 0; }

// ---- threads / process -------------------------------------------------------------------------
DWORD GetCurrentThreadId(void) { return tid(); }
DWORD GetCurrentProcessId(void) { return 1; }
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
HANDLE OpenThread(DWORD, BOOL, DWORD) { t_last_error = ERROR_NOT_SUPPORTED; return nullptr; }
DWORD SuspendThread(HANDLE) { return (DWORD)-1; }
DWORD ResumeThread(HANDLE) { return (DWORD)-1; }
BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
uintptr_t SetThreadAffinityMask(HANDLE, uintptr_t) { return 1; }
BOOL GetThreadContext(HANDLE, CONTEXT*) { return FALSE; }
BOOL SetThreadContext(HANDLE, const CONTEXT*) { return FALSE; }
USHORT RtlCaptureStackBackTrace(ULONG, ULONG, void**, ULONG* hash) { if (hash) *hash = 0; return 0; }

// ---- modules -----------------------------------------------------------------------------------
// The runtime loads XInput dynamically (kernel_stubs lsw_host_pad). On Switch "xinput1_4.dll" resolves
// to libnx HID pads (players 1 and 2, log70) with the same XINPUT_STATE layout and button bits.
namespace {
PadState g_pad;   // one player (LSWTCS_PLAYERS=1): No1 + handheld merged; also the "module" handle LoadLibraryA hands out
std::once_flag g_pad_once;
// Local co-op (log70). The game takes two players: the second joins with START on the second pad. With LSWTCS_PLAYERS=2 (the
// default) the handheld Joy-Cons, controller 1 and controller 2 are read separately, and the players are the connected ones in that
// order: a lone controller or the handheld is always player 1, a second one is player 2. A single Joy-Con is held sideways (hold type
// horizontal); its face buttons are taken by position. LSWTCS_PLAYERS=1: one merged pad, as before log70. [PADS] logs who is who.
int g_players = 2;
PadState g_src[3];   // handheld, No1, No2
const char* const kSrcName[3] = {"handheld", "controller 1", "controller 2"};
struct XiState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
std::atomic<bool> g_applet_busy{false};
bool g_by_label = false;   // face buttons by label (LSWTCS_PADLAYOUT), decided on player 1's polls

bool single_joycon(u32 style);
void pads_init() {
    const char* e = getenv("LSWTCS_PLAYERS");
    g_players = e && atoi(e) == 1 ? 1 : 2;
    if (g_players == 1) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
        return;
    }
    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    const Result rc = hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    padInitialize(&g_src[0], HidNpadIdType_Handheld);
    padInitialize(&g_src[1], HidNpadIdType_No1);
    padInitialize(&g_src[2], HidNpadIdType_No2);
    // Detached Joy-Cons start as one controller in two hands (log72). The system keeps the single assignment the controller screen
    // gave them in an earlier session: run 71 booted with the right Joy-Con as player 1 and the left one as player 2, for a tester
    // holding both, and the game started for two. Two players with a Joy-Con each come only from the controller screen
    // (L + R + Plus), then player 2's Start in the game.
    hidSetNpadJoyAssignmentModeDual(HidNpadIdType_No1);
    hidSetNpadJoyAssignmentModeDual(HidNpadIdType_No2);
    padUpdate(&g_src[1]);
    padUpdate(&g_src[2]);
    const u32 s1 = padGetStyleSet(&g_src[1]), s2 = padGetStyleSet(&g_src[2]);
    const u32 lr = HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight;
    if (padIsConnected(&g_src[1]) && padIsConnected(&g_src[2]) && single_joycon(s1) && single_joycon(s2) && ((s1 | s2) & lr) == lr) {
        const Result mrc = hidMergeSingleJoyAsDualJoy(HidNpadIdType_No1, HidNpadIdType_No2);
        printf("[PADS] two single Joy-Cons at start (styles 0x%X, 0x%X) joined as one controller (rc 0x%X)\n", s1, s2, mrc);
        fflush(stdout);
    }
    printf("[PADS] two players: the connected ones of handheld, controller 1, controller 2, in that order; single Joy-Cons sideways "
           "(rc 0x%X); L + R + Plus, or SL + SR + Plus/Minus on a single Joy-Con, opens the controller screen\n", rc);
    fflush(stdout);
}

// Player `slot`'s source among the connected ones (handheld, No1, No2), or null.
PadState* player_source(DWORD slot, int* src) {
    for (PadState& p : g_src) padUpdate(&p);
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        if (!padIsConnected(&g_src[i])) continue;
        if (n++ == int(slot)) { *src = i; return &g_src[i]; }
    }
    return nullptr;
}

bool single_joycon(u32 style) {
    return (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) && !(style & HidNpadStyleSet_NpadFullCtrl);
}

// [PADS] when the players change (connected sources and their styles).
void log_players() {
    static u64 last = ~0ull;
    u64 now = 0;
    for (int i = 0; i < 3; ++i) now |= u64(padIsConnected(&g_src[i]) ? (padGetStyleSet(&g_src[i]) & 0xFF) | 0x100 : 0) << (i * 9);
    if (now == last) return;
    last = now;
    char b[256];
    int k = snprintf(b, sizeof b, "[PADS]");
    int player = 0;
    for (int i = 0; i < 3 && k < 200; ++i) {
        if (!padIsConnected(&g_src[i])) continue;
        const u32 style = padGetStyleSet(&g_src[i]);
        const char* kind = style & HidNpadStyleTag_NpadHandheld ? "handheld" : style & HidNpadStyleTag_NpadFullKey ? "Pro Controller"
                         : style & HidNpadStyleTag_NpadJoyDual ? "two Joy-Cons" : style & HidNpadStyleTag_NpadJoyLeft ? "left Joy-Con sideways"
                         : style & HidNpadStyleTag_NpadJoyRight ? "right Joy-Con sideways" : "other";
        k += snprintf(b + k, sizeof b - k, " %s %s (%s, style 0x%X);", ++player <= g_players ? (player == 1 ? "player 1:" : "player 2:") : "unused:",
                      kSrcName[i], kind, style);
    }
    if (!player) snprintf(b + k, sizeof b - k, " no controller");
    printf("%s\n", b);
    fflush(stdout);
}

// The system's controller screen (connect, split Joy-Cons with SL + SR), on its own thread so the game's thread does not wait.
void show_controller_applet() {
    bool idle = false;
    if (!g_applet_busy.compare_exchange_strong(idle, true)) return;
    std::thread([] {
        HidLaControllerSupportArg arg;
        hidLaCreateControllerSupportArg(&arg);
        arg.hdr.player_count_min = 2;   // the screen shows only while fewer players are connected
        arg.hdr.player_count_max = 2;
        HidLaControllerSupportResultInfo info{};
        const Result rc = hidLaShowControllerSupport(&info, &arg);
        printf("[PADS] controller screen closed: rc 0x%X, %d players\n", rc, int(info.player_count));
        fflush(stdout);
        g_applet_busy.store(false);
    }).detach();
}

// A single Joy-Con held sideways. The face buttons follow the layout like a full controller, by where they sit: by position the
// bottom button is the 360's A (play); by label the right one is (menus, as A sits on the right of a Switch controller), the
// bottom one B, the top one X, the left one Y. Sideways, the left Joy-Con's Left, Down, Up, Right sit bottom, right, left, top,
// and the right Joy-Con's A, X, B, Y. Minus or Plus is Start, L or R Back, SL and SR the bumpers, ZL or ZR the left trigger.
// The stick is the 360's left stick, turned a quarter: the system reports it as for an upright Joy-Con (run 70, the tester:
// tilting the right Joy-Con's stick right moved up, down moved right; the left one's left moved up, up moved right).
// LSWTCS_JOYROT=0 passes it as the system gives it.
void map_single_joycon(const PadState& pad, u32 style, bool by_label, XiState& st) {
    static const bool rot = [] { const char* e = getenv("LSWTCS_JOYROT"); return !(e && e[0] == '0'); }();
    struct M { u64 nx; WORD xi; };
    static const M kLeft[] = {{HidNpadButton_Minus, 0x0010}, {HidNpadButton_L, 0x0020}, {HidNpadButton_StickL, 0x0040},
                              {HidNpadButton_LeftSL, 0x0100}, {HidNpadButton_LeftSR, 0x0200}};
    static const M kRight[] = {{HidNpadButton_Plus, 0x0010}, {HidNpadButton_R, 0x0020}, {HidNpadButton_StickR, 0x0040},
                               {HidNpadButton_RightSL, 0x0100}, {HidNpadButton_RightSR, 0x0200}};
    const bool left = style & HidNpadStyleTag_NpadJoyLeft;
    const u64 b = padGetButtons(&pad);
    for (const M& m : left ? kLeft : kRight) if (b & m.nx) st.buttons |= m.xi;
    // Sideways positions: bottom, right, left, top.
    const u64 bottom = left ? HidNpadButton_Left : HidNpadButton_A, right = left ? HidNpadButton_Down : HidNpadButton_X;
    const u64 lpos = left ? HidNpadButton_Up : HidNpadButton_B, top = left ? HidNpadButton_Right : HidNpadButton_Y;
    if (by_label) {
        if (b & right) st.buttons |= 0x1000;
        if (b & bottom) st.buttons |= 0x2000;
        if (b & top) st.buttons |= 0x4000;
        if (b & lpos) st.buttons |= 0x8000;
    } else {
        if (b & bottom) st.buttons |= 0x1000;
        if (b & right) st.buttons |= 0x2000;
        if (b & lpos) st.buttons |= 0x4000;
        if (b & top) st.buttons |= 0x8000;
    }
    st.lt = (b & (left ? HidNpadButton_ZL : HidNpadButton_ZR)) ? 255 : 0;
    const HidAnalogStickState s = padGetStickPos(&pad, left ? 0 : 1);
    int32_t x = s.x, y = s.y;
    if (rot) { const int32_t t = x; if (left) { x = -y; y = t; } else { x = y; y = -t; } }
    st.lx = SHORT(x < -32767 ? -32767 : x > 32767 ? 32767 : x);
    st.ly = SHORT(y < -32767 ? -32767 : y > 32767 ? 32767 : y);
}

// A full controller (handheld, Pro Controller, two Joy-Cons).
void map_full(const PadState& pad, u64 b, XiState& st) {
    static const struct { u64 nx; WORD xi; } kMap[] = {
        {HidNpadButton_Up, 0x0001}, {HidNpadButton_Down, 0x0002}, {HidNpadButton_Left, 0x0004},
        {HidNpadButton_Right, 0x0008}, {HidNpadButton_Plus, 0x0010}, {HidNpadButton_Minus, 0x0020},
        {HidNpadButton_StickL, 0x0040}, {HidNpadButton_StickR, 0x0080}, {HidNpadButton_L, 0x0100},
        {HidNpadButton_R, 0x0200}};
    for (auto& m : kMap) if (b & m.nx) st.buttons |= m.xi;
    if (g_by_label) {
        if (b & HidNpadButton_A) st.buttons |= 0x1000;
        if (b & HidNpadButton_B) st.buttons |= 0x2000;
        if (b & HidNpadButton_X) st.buttons |= 0x4000;
        if (b & HidNpadButton_Y) st.buttons |= 0x8000;
    } else {
        if (b & HidNpadButton_A) st.buttons |= 0x2000;
        if (b & HidNpadButton_B) st.buttons |= 0x1000;
        if (b & HidNpadButton_X) st.buttons |= 0x8000;
        if (b & HidNpadButton_Y) st.buttons |= 0x4000;
    }
    st.lt = (b & HidNpadButton_ZL) ? 255 : 0;
    st.rt = (b & HidNpadButton_ZR) ? 255 : 0;
    const HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
    // Right stick pitch inverted (user's choice 2026-10-11). Clamped so -(-32768) cannot overflow.
    st.lx = SHORT(l.x); st.ly = SHORT(l.y); st.rx = SHORT(r.x); st.ry = SHORT(-(r.y < -32767 ? -32767 : int32_t(r.y)));
}

extern "C" void lsw_log_async_drain();   // log_async.cpp

// Scripted input (LSWTCS_INPUTSCRIPT, include/lsw_input_script.h): a file of steps replaces player 1's pad while it runs, for
// automated runs on the console or in an emulator. A bare name is read from sdmc:/lswtcs/tests/. [SCRIPT] lines report each
// step and the result ("done" or "failed"); after either, the pad is the player's again.
extern "C" void lsw_level_name(char* out, size_t capacity);   // native_crt.cpp
extern "C" uint64_t lsw_load_episodes_done();                 // kernel_stubs.cpp ([LOADEP])
LswInputScript g_script;
bool g_script_on = false;
void script_init() {
    const char* e = getenv("LSWTCS_INPUTSCRIPT");
    if (!e || !*e || !strcmp(e, "0")) return;
    std::string path = e;
    if (path.find(':') == std::string::npos && path[0] != '/') path = "sdmc:/lswtcs/tests/" + path;
    std::string text;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        fclose(f);
    } else {
        printf("[SCRIPT] failed: cannot open %s\n", path.c_str());
        fflush(stdout);
        return;
    }
    if (!g_script.load(text)) {
        printf("[SCRIPT] failed: %s: %s\n", path.c_str(), g_script.error().c_str());
        fflush(stdout);
        return;
    }
    g_script_on = true;
    printf("[SCRIPT] %s: %zu steps; player 1's pad follows the script until it ends\n", path.c_str(), g_script.steps());
    fflush(stdout);
}
// Pad recording (LSWTCS_INPUTRECORD=1): a [PADREC] line whenever player 1's 360-pad state changes (sticks in steps of 1/16),
// with the time, light or play frames and the level, so tools/padrec_to_script.py can turn one played route into a script
// whose waits follow the game's loads instead of the clock.
void record_poll(const XiState& st) {
    static const bool on = [] { const char* e = getenv("LSWTCS_INPUTRECORD"); return e && e[0] == '1'; }();
    if (!on) return;
    static XiState last{};
    static bool last_light = false, first = true;
    static char last_level[96] = {};
    auto q = [](SHORT v) { return int(v) / 2048; };   // 1/16 of the range
    const bool light = g_frame_light.load(std::memory_order_relaxed);
    char level[96];
    lsw_level_name(level, sizeof level);
    const bool same = !first && st.buttons == last.buttons && (st.lt > 127) == (last.lt > 127) && (st.rt > 127) == (last.rt > 127) &&
                      q(st.lx) == q(last.lx) && q(st.ly) == q(last.ly) && q(st.rx) == q(last.rx) && q(st.ry) == q(last.ry) &&
                      light == last_light && !strcmp(level, last_level);
    if (same) return;
    first = false;
    last = st;
    last_light = light;
    snprintf(last_level, sizeof last_level, "%s", level);
    printf("[PADREC] %llu ms buttons 0x%04X lt %d rt %d l %d %d r %d %d %s level %s\n",
           (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000ull), st.buttons, st.lt > 127, st.rt > 127,
           q(st.lx), q(st.ly), q(st.rx), q(st.ry), light ? "light" : "play", level[0] ? level : "?");
}

void script_poll(XiState& st) {
    static std::once_flag once;
    std::call_once(once, script_init);
    if (!g_script_on || g_script.finished()) { record_poll(st); return; }
    LswScriptWorld w;
    w.now_ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
    w.light = g_frame_light.load(std::memory_order_relaxed);
    char level[96];
    lsw_level_name(level, sizeof level);
    w.level = level;
    w.loads = lsw_load_episodes_done();
    LswPadOut pad{};
    pad.buttons = st.buttons; pad.lt = st.lt; pad.rt = st.rt; pad.lx = st.lx; pad.ly = st.ly; pad.rx = st.rx; pad.ry = st.ry;
    LswScriptActions act;
    std::vector<std::string> log;
    g_script.poll(w, &pad, &act, &log);
    st.buttons = pad.buttons; st.lt = pad.lt; st.rt = pad.rt; st.lx = pad.lx; st.ly = pad.ly; st.rx = pad.rx; st.ry = pad.ry;
    for (int i = 0; i < act.marks; ++i) {
        const uint32_t n = g_frame_marks.fetch_add(1, std::memory_order_relaxed) + 1;
        printf("[MARK] %u at %llu ms (script)\n", n, (unsigned long long)w.now_ms);
    }
    if (act.capture) g_capture_request.store(uint32_t(act.capture), std::memory_order_relaxed);
    for (const std::string& line : log) printf("%s\n", line.c_str());
    if (!log.empty() || act.marks) {
        fflush(stdout);
        if (g_script.finished()) lsw_log_async_drain();
    }
}

DWORD WINAPI nx_xinput_get_state(DWORD slot, void* out) {
    std::call_once(g_pad_once, pads_init);
    if (slot >= DWORD(g_players)) return 1167;   // ERROR_DEVICE_NOT_CONNECTED
    PadState* pad = &g_pad;
    if (g_players == 1) {
        padUpdate(&g_pad);
    } else {
        int src = -1;
        pad = player_source(slot, &src);
        if (slot == 0) log_players();
        if (!pad) {
            if (slot != 0) return 1167;
            pad = &g_src[1];   // player 1 stays present for the game, idle until a controller connects
        }
    }
    XiState st{};
    const u64 b = padGetButtons(pad);
    if (slot == 0) {   // log85: [PAD] the first 60 presses, on the card at once (run 87: skipping the movie ended the process silently)
        static u64 prev = 0;
        static int presses = 0;
        const u64 down = b & ~prev;
        prev = b;
        if (down && presses < 60) {
            ++presses;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            printf("[PAD] press 0x%llX (held 0x%llX) #%d at %lld ms\n", (unsigned long long)down, (unsigned long long)b, presses, (long long)ms);
            fflush(stdout);
            lsw_log_async_drain();
        }
    }
    const u32 style = padGetStyleSet(pad);
    // Face buttons, LSWTCS_PADLAYOUT (log65):
    //   position  Switch A/B/X/Y act as the 360 pad's B/A/Y/X: the bottom button (Switch B) is the 360's A, as in the 360's
    //             layout (user's choice 2026-10-11; the default).
    //   label     Switch A is the 360's A: confirm on A as on the Switch, and the game's button letters match the Switch's.
    //   auto      label while the frames are light (under 150 draws for 30 frames: title, menus, load and save screens, loading),
    //             position in play. The in-level pause menu draws the level behind it, so it stays by position.
    // The layout changes only while no face button of player 1 is held, so a held button never turns into another. Hold L + R and
    // press Minus for the next layout ([PADLAYOUT]). Player 2 follows player 1's layout, single Joy-Cons too (log71).
    if (slot == 0) {
        static int layout = [] {
            const char* e = getenv("LSWTCS_PADLAYOUT");
            return e && !strcasecmp(e, "label") ? 1 : e && !strcasecmp(e, "auto") ? 2 : 0;
        }();
        static const char* const kLayoutName[3] = {"position", "label", "auto"};
        static u64 last_switch_ns = 0;
        static bool first = true;
        if (first) { first = false; g_by_label = layout == 1; }
        static uint32_t lines = 0;
        const u64 now_ns = armTicksToNs(armGetSystemTick());
        const u64 lr = HidNpadButton_L | HidNpadButton_R;
        if ((b & lr) == lr && (b & HidNpadButton_Minus) && now_ns - last_switch_ns > 1000000000ull) {
            last_switch_ns = now_ns;
            layout = (layout + 1) % 3;
            printf("[PADLAYOUT] layout %s (L + R + Minus)\n", kLayoutName[layout]);
            fflush(stdout);
        }
        // The D-pad too: on a sideways left Joy-Con those are the face buttons.
        const u64 face = HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_Up | HidNpadButton_Down |
                         HidNpadButton_Left | HidNpadButton_Right;
        const bool want = layout == 1 || (layout == 2 && g_frame_light.load(std::memory_order_relaxed));
        if (want != g_by_label && !(b & face)) {
            g_by_label = want;
            if (lines++ < 400) { printf("[PADLAYOUT] face buttons by %s (layout %s)\n", g_by_label ? "label" : "position", kLayoutName[layout]); fflush(stdout); }
        }
    }
    if (g_players > 1 && single_joycon(style)) map_single_joycon(*pad, style, g_by_label, st);
    else map_full(*pad, b, st);
    // Controller screen (log70): L + R + Plus on a full controller, SL + SR + Plus or Minus on a single Joy-Con, any player. Plus is
    // also Start, so the game pauses behind it.
    if (g_players > 1) {
        const u64 lr = HidNpadButton_L | HidNpadButton_R;
        const bool sl = b & HidNpadButton_AnySL, sr = b & HidNpadButton_AnySR;
        if (((b & lr) == lr && (b & HidNpadButton_Plus)) || (sl && sr && (b & (HidNpadButton_Plus | HidNpadButton_Minus))))
            show_controller_applet();
    }
    if (slot == 0) {
        // Capture combos (LSWTCS_CAPCOMBO, default on), right stick clicked last, at most once per 3 s:
        // ZL + ZR: full capture, screenshot, per-draw [GLDRAWCAP] log, the frame's GLSL, the frame history (LSWTCS_GLHISTORY)
        // and every resolve/render target as PNG. L + R: the same without the targets. The files reach the SD card in the
        // background, while the game runs: [CAPSAVE] says when they are done.
        // Only a request here: the render thread takes it at its next replay and does the logging, because file access
        // from this thread held the game for 1-2.6 s per capture and the trigger files were polled every 30 replays.
        {
            static const bool combo_on = [] { const char* e = getenv("LSWTCS_CAPCOMBO"); return !(e && e[0] == '0'); }();
            static u64 last_ns = 0;
            const u64 triggers = HidNpadButton_ZL | HidNpadButton_ZR;
            const u64 shoulders = HidNpadButton_L | HidNpadButton_R;
            const u64 now_ns = armTicksToNs(armGetSystemTick());
            if (combo_on && (b & HidNpadButton_StickR) && now_ns - last_ns > 3000000000ull) {
                const bool full = (b & triggers) == triggers;
                if (full || (b & shoulders) == shoulders) {
                    last_ns = now_ns;
                    g_capture_request.store(full ? 2 : 1, std::memory_order_relaxed);
                }
            }
        }
        // A/B toggle combo (log65): hold L + R and click the left stick -> the next state of LSWTCS_ABTOGGLE, then back to the
        // profile's states (gl_backend.cpp, [ABTOGGLE]). For a fault that shows in one place, such as the studs over the legs.
        {
            static u64 last_toggle_ns = 0;
            const u64 want = HidNpadButton_L | HidNpadButton_R | HidNpadButton_StickL;
            const u64 now_ns = armTicksToNs(armGetSystemTick());
            if ((b & want) == want && now_ns - last_toggle_ns > 1000000000ull) {
                last_toggle_ns = now_ns;
                g_ab_toggle_request.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Mark combo: hold ZL + ZR and click the left stick -> "# mark N" in frames.log and [MARK] in stdout.log, so a
        // tester can flag a moment (visual fault, audio jitter, combat start) without noting the time. Logging only.
        {
            static u64 last_mark_ns = 0;
            const u64 want = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_StickL;
            const u64 now_ns = armTicksToNs(armGetSystemTick());
            if ((b & want) == want && now_ns - last_mark_ns > 1000000000ull) {
                last_mark_ns = now_ns;
                const uint32_t n = g_frame_marks.fetch_add(1, std::memory_order_relaxed) + 1;
                printf("[MARK] %u at %llu ms\n", n, (unsigned long long)(now_ns / 1000000ull));
                fflush(stdout);
            }
        }
        script_poll(st);
    }
    memcpy(out, &st, sizeof st);
    return ERROR_SUCCESS;
}
}
HMODULE LoadLibraryA(LPCSTR name) {
    return (name && !strcasecmp(name, "xinput1_4.dll")) ? (HMODULE)&g_pad : nullptr;
}
void* GetProcAddress(HMODULE m, LPCSTR name) {
    if (m == (HMODULE)&g_pad && name && !strcmp(name, "XInputGetState")) return (void*)&nx_xinput_get_state;
    return nullptr;
}
HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE* out) { if (out) *out = nullptr; return FALSE; }
DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD n) { if (buf && n) buf[0] = 0; return 0; }

// ---- memory ------------------------------------------------------------------------------------
BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD* old) { if (old) *old = PAGE_READWRITE; return FALSE; }
void* VirtualAlloc(void* p, SIZE_T n, DWORD type, DWORD) {
    if (p || !(type & MEM_COMMIT)) return nullptr;   // address-specific reservations: Switch guest window only
    void* m = aligned_alloc(0x1000, (n + 0xFFF) & ~SIZE_T(0xFFF));
    if (m) memset(m, 0, n);
    return m;
}
BOOL VirtualFree(void* p, SIZE_T, DWORD) { free(p); return TRUE; }

// ---- exceptions / input / UI -------------------------------------------------------------------
void* AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER) { return (void*)1; }
SHORT GetAsyncKeyState(int) { return 0; }
DWORD GetWindowThreadProcessId(HWND, DWORD* pid) { if (pid) *pid = 0; return 0; }
HWND GetForegroundWindow(void) { return nullptr; }
