// PGO counts on the console (log93, build 2 of the plan in CPU_ROADMAP.md, "rc-guide material study").
//
// Compiled only with -DLSW_PGO=generate. The game code and the runtime are built with -fprofile-generate and
// -fprofile-info-section=gcov_info: every object registers its counters in the section "gcov_info", and nothing is written at
// exit by libgcov (its exit-time files assume a host file system, and the game is often closed from HOME). This file streams
// all of them into one file with GCC's __gcov_info_to_gcda() (gcov.h). The method is sa2-nx's and renut-nx's (their docs;
// no code taken). `tools/pgo_merge.py` turns the streams into .gcda files on the PC (gcov-tool merge-stream).
//
// Each launch keeps one file, sdmc:/lswtcs/pgo/lswtcs_<run id>.stream, replaced by every new snapshot (counts only grow, so the
// latest holds everything; merging several snapshots of one launch would count it twice). Snapshots: every 3 minutes on a
// background thread, and at exit. Different launches make different files, and the merge adds them.
#include <switch.h>
extern "C" {   // GCC's gcov.h has no extern "C" of its own; libgcov's functions are C symbols
#include <gcov.h>
}
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

extern "C" const struct gcov_info* const __start_gcov_info[] __attribute__((weak));
extern "C" const struct gcov_info* const __stop_gcov_info[] __attribute__((weak));

namespace {
Mutex g_pgo_mutex;
unsigned long long g_pgo_run;
unsigned g_pgo_snapshots;

void dump_fn(const void* data, unsigned length, void* arg) { fwrite(data, 1, length, static_cast<FILE*>(arg)); }
void filename_fn(const char* name, void* arg) { __gcov_filename_to_gcfn(name, dump_fn, arg); }
void* allocate_fn(unsigned length, void*) { return malloc(length); }   // small, a few per object and snapshot

void snapshot(const char* why) {
    if (!__start_gcov_info || !__stop_gcov_info) return;
    mutexLock(&g_pgo_mutex);
    char path[128], tmp[136];
    snprintf(path, sizeof path, "sdmc:/lswtcs/pgo/lswtcs_%016llX.stream", g_pgo_run);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    const uint64_t t0 = armGetSystemTick();
    FILE* f = fopen(tmp, "wb");
    if (!f) {
        printf("[PGO] cannot open %s\n", tmp);
        mutexUnlock(&g_pgo_mutex);
        return;
    }
    static char buffer[1 << 20];
    setvbuf(f, buffer, _IOFBF, sizeof buffer);
    unsigned objects = 0;
    for (const gcov_info* const* p = __start_gcov_info; p < __stop_gcov_info; ++p, ++objects)
        __gcov_info_to_gcda(*p, filename_fn, dump_fn, allocate_fn, f);
    const long bytes = ftell(f);
    const bool ok = fclose(f) == 0;
    remove(path);
    const bool renamed = ok && rename(tmp, path) == 0;
    ++g_pgo_snapshots;
    printf("[PGO] snapshot %u (%s): %u objects, %.1f MB, %llu ms, %s\n", g_pgo_snapshots, why, objects, bytes / 1048576.0,
           (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull), renamed ? path : "WRITE FAILED");
    fflush(stdout);
    mutexUnlock(&g_pgo_mutex);
}

void pgo_thread(void*) {
    for (;;) {
        svcSleepThread(180ll * 1000000000ll);
        snapshot("every 3 minutes");
    }
}
}  // namespace

extern "C" void lsw_pgo_start(unsigned long long run_id) {
    g_pgo_run = run_id;
    mutexInit(&g_pgo_mutex);
    mkdir("sdmc:/lswtcs/pgo", 0777);
    const long n = (__start_gcov_info && __stop_gcov_info) ? long(__stop_gcov_info - __start_gcov_info) : -1;
    printf("[PGO] instrumented build: %ld objects registered; counts to sdmc:/lswtcs/pgo/lswtcs_%016llX.stream\n", n, run_id);
    static Thread t;
    // Lowest application priority, core 2 (the audio/log core has the most room); the write is a few seconds a snapshot.
    if (R_SUCCEEDED(threadCreate(&t, pgo_thread, nullptr, nullptr, 0x10000, 0x3F, 2))) threadStart(&t);
}

extern "C" void lsw_pgo_exit() { snapshot("exit"); }
