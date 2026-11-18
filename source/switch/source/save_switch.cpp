// Native account save storage. Runs before guest threads, GPU or audio startup.
#include <switch.h>
#include "lsw_save_platform.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
AccountUid user{};
char nickname[64]{};
bool mounted = false;
Result last_error = 0;
const char* stage = "not initialized";
bool fail(const char* where, Result rc) {
    stage = where;
    last_error = rc;
    printf("[SAVEFS] %s failed: 0x%X\n", where, rc);
    return false;
}
}

extern "C" void lsw_boot_mark(const char* what);   // main.cpp: [BOOT] milliseconds since the process started

// log112: copying a save to and from the SD card, to move it between the console and a PC emulator.
//   LSWTCS_SAVEEXPORT=1       at boot, copy the selected user's save to sdmc:/lswtcs/save_export/<date>_<user>/. It only reads
//                             the save: nothing in it is changed, deleted or committed.
//   LSWTCS_SAVEIMPORT=<dir>   at boot, copy the files of <dir> (a bare name is under sdmc:/lswtcs/save_export/) into the save,
//                             only when the save has no files at all; a save with anything in it is left alone. Then commit.
// Both off unless set. [SAVECOPY] lines report every file and the result.
namespace {
struct CopyStats { unsigned files = 0, dirs = 0, failed = 0; unsigned long long bytes = 0; };

bool copy_file(const std::string& from, const std::string& to, CopyStats& st) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) { ++st.failed; printf("[SAVECOPY] cannot read %s\n", from.c_str()); return false; }
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); ++st.failed; printf("[SAVECOPY] cannot write %s\n", to.c_str()); return false; }
    std::vector<char> buf(64 * 1024);
    unsigned long long n = 0;
    bool ok = true;
    for (size_t k; (k = fread(buf.data(), 1, buf.size(), in)) > 0;) {
        if (fwrite(buf.data(), 1, k, out) != k) { ok = false; break; }
        n += k;
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) { ++st.failed; printf("[SAVECOPY] copy failed: %s -> %s\n", from.c_str(), to.c_str()); return false; }
    ++st.files;
    st.bytes += n;
    printf("[SAVECOPY]   %s (%llu bytes)\n", to.c_str(), n);
    return true;
}

// Copies every file and folder under `from` into `to` (which must exist). Reads `from` only.
void copy_tree(const std::string& from, const std::string& to, CopyStats& st, int depth = 0) {
    if (depth > 16) return;
    DIR* d = opendir(from.c_str());
    if (!d) { ++st.failed; printf("[SAVECOPY] cannot open folder %s\n", from.c_str()); return; }
    std::vector<std::pair<std::string, bool>> entries;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string src = from + (from.back() == '/' ? "" : "/") + e->d_name;
        struct stat s{};
        const bool dir = stat(src.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
        entries.emplace_back(e->d_name, dir);
    }
    closedir(d);
    for (auto& [name, dir] : entries) {
        const std::string src = from + (from.back() == '/' ? "" : "/") + name, dst = to + (to.back() == '/' ? "" : "/") + name;
        if (dir) {
            mkdir(dst.c_str(), 0777);
            ++st.dirs;
            copy_tree(src, dst, st, depth + 1);
        } else {
            copy_file(src, dst, st);
        }
    }
}

bool folder_empty(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    bool empty = true;
    while (dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = false; break; }
    closedir(d);
    return empty;
}

void save_export() {
    const char* e = getenv("LSWTCS_SAVEEXPORT");
    if (!e || e[0] != '1') return;
    char stamp[32] = "undated";
    const time_t now = time(nullptr);
    if (const tm* t = localtime(&now); t && t->tm_year >= 120) strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", t);
    std::string who;
    for (const char* p = nickname; *p; ++p) who += (isalnum(static_cast<unsigned char>(*p)) || *p == '-' || *p == '_') ? *p : '_';
    mkdir("sdmc:/lswtcs/save_export", 0777);
    const std::string dst = std::string("sdmc:/lswtcs/save_export/") + stamp + "_" + (who.empty() ? "user" : who);
    if (mkdir(dst.c_str(), 0777) != 0) { printf("[SAVECOPY] export: cannot make %s; the save was not touched\n", dst.c_str()); return; }
    CopyStats st;
    printf("[SAVECOPY] export: %s -> %s (the save is only read)\n", lsw_save::root, dst.c_str());
    copy_tree(lsw_save::root, dst, st);
    printf("[SAVECOPY] export %s: %u files, %u folders, %llu bytes, %u failed\n", st.failed ? "INCOMPLETE" : "done", st.files,
           st.dirs, st.bytes, st.failed);
    fflush(stdout);
}

void save_import() {
    const char* e = getenv("LSWTCS_SAVEIMPORT");
    if (!e || !*e || !strcmp(e, "0")) return;
    std::string src = e;
    if (src.find(':') == std::string::npos) src = "sdmc:/lswtcs/save_export/" + src;
    if (!folder_empty(lsw_save::root)) {
        printf("[SAVECOPY] import skipped: this user's save already has files, and an import never replaces a save\n");
        fflush(stdout);
        return;
    }
    DIR* d = opendir(src.c_str());
    if (!d) { printf("[SAVECOPY] import: no folder %s\n", src.c_str()); fflush(stdout); return; }
    closedir(d);
    CopyStats st;
    printf("[SAVECOPY] import: %s -> %s (the save was empty)\n", src.c_str(), lsw_save::root);
    copy_tree(src, lsw_save::root, st);
    const Result rc = fsdevCommitDevice(lsw_save::mount_name);
    printf("[SAVECOPY] import %s: %u files, %u folders, %llu bytes, %u failed; commit 0x%X\n",
           st.failed ? "INCOMPLETE" : "done", st.files, st.dirs, st.bytes, st.failed, rc);
    fflush(stdout);
}
}  // namespace

extern "C" bool lsw_save_init() {
    if (mounted) return true;
    user = {};
    nickname[0] = 0;
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) return fail("account service", rc);
    // [BOOT] (log68): which way the user came, so the boot's time is not mistaken for the runtime's when the selector was on screen.
    const char* how = "user preselected by the system";
    bool selected = R_SUCCEEDED(accountGetPreselectedUser(&user)) && accountUidIsValid(&user);
    if (!selected) {
        how = "the only user, without the selector";
        selected = R_SUCCEEDED(accountTrySelectUserWithoutInteraction(&user, false)) && accountUidIsValid(&user);
    }
    if (!selected) {
        lsw_boot_mark("user selector shown");
        how = "user picked in the selector";
        PselUserSelectionSettings settings{};
        settings.is_unqualified_user_selectable = 1; // Local users need no linked Nintendo Account.
        rc = pselShowUserSelector(&user, &settings);
        selected = R_SUCCEEDED(rc) && accountUidIsValid(&user);
    }
    if (selected) lsw_boot_mark(how);
    if (!selected) {
        accountExit();
        return fail("user selection (cancelled or unavailable)", R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_NotFound));
    }
    AccountProfile profile{};
    rc = accountGetProfile(&profile, user);
    if (R_SUCCEEDED(rc)) {
        AccountProfileBase pb{};
        rc = accountProfileGet(&profile, nullptr, &pb);
        if (R_SUCCEEDED(rc)) snprintf(nickname, sizeof nickname, "%.*s", int(sizeof pb.nickname), pb.nickname);
        accountProfileClose(&profile);
    }
    accountExit();
    if (R_FAILED(rc)) return fail("selected user profile", rc);

    // Always use TCS's explicit ID, including when hbmenu runs under another title.
    rc = fsdevMountSaveData(lsw_save::mount_name, lsw_save::application_id, user);
    if (R_FAILED(rc) && R_MODULE(rc) == 2 && (R_DESCRIPTION(rc) == 1002 || R_DESCRIPTION(rc) == 1)) {
        FsSaveDataAttribute attr{};
        attr.application_id = lsw_save::application_id;
        attr.uid = user;
        attr.save_data_type = FsSaveDataType_Account;
        FsSaveDataCreationInfo creation{};
        creation.save_data_size = lsw_save::data_bytes;
        creation.journal_size = lsw_save::journal_bytes;
        creation.owner_id = lsw_save::application_id;
        creation.save_data_space_id = FsSaveDataSpaceId_User;
        FsSaveDataMetaInfo meta{};
        const Result create_rc = fsCreateSaveDataFileSystem(&attr, &creation, &meta);
        printf("[SAVEFS] create native account container: 0x%X (%lld + %lld bytes journal)\n",
               create_rc, static_cast<long long>(creation.save_data_size), static_cast<long long>(creation.journal_size));
        // Reopen even after a creation race; never delete or recreate existing save data.
        rc = fsdevMountSaveData(lsw_save::mount_name, lsw_save::application_id, user);
        if (R_FAILED(rc)) return fail("native save creation/mount", R_FAILED(create_rc) ? create_rc : rc);
    }
    if (R_FAILED(rc)) return fail("native save mount (permissions or storage)", rc);
    mounted = true;
    stage = "ready";
    last_error = 0;
    printf("[SAVEFS] native account storage ready; application=%016llX; selected user '%s'\n",
           static_cast<unsigned long long>(lsw_save::application_id), nickname);
    save_export();   // log112: before the game opens anything, so the copy is the save as it was at launch
    save_import();
    return true;
}

extern "C" uint32_t lsw_save_commit() {
    if (!mounted) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    const Result rc = fsdevCommitDevice(lsw_save::mount_name);
    if (R_FAILED(rc)) fail("native save commit", rc);
    printf("[SAVEFS] commit: 0x%X\n", rc);
    return rc;
}
extern "C" bool lsw_save_space(uint64_t* total, uint64_t* available) {
    FsFileSystem* fs = mounted ? fsdevGetDeviceFileSystem(lsw_save::mount_name) : nullptr;
    s64 t = 0, a = 0;
    if (!fs || R_FAILED(fsFsGetTotalSpace(fs, "/", &t)) || R_FAILED(fsFsGetFreeSpace(fs, "/", &a))) return false;
    *total = t > 0 ? uint64_t(t) : 0;
    *available = a > 0 ? uint64_t(a) : 0;
    return true;
}
extern "C" int lsw_save_profile(char* name, int capacity, uint8_t uid[16]) {
    if (!mounted) return 0;
    if (capacity > 0) snprintf(name, capacity, "%s", nickname);
    memcpy(uid, &user, 16);
    return 1;
}
extern "C" void lsw_save_shutdown() {
    // Game flush/close calls commit transactions. Do not commit arbitrary unfinished writes on exit.
    if (mounted) fsdevUnmountDevice(lsw_save::mount_name);
    mounted = false;
}
extern "C" uint32_t lsw_save_last_error() { return last_error; }
extern "C" const char* lsw_save_error_stage() { return stage; }
