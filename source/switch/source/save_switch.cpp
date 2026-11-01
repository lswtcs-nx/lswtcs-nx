// Native account save storage. Runs before guest threads, GPU or audio startup.
#include <switch.h>
#include "lsw_save_platform.h"
#include <cstdio>
#include <cstring>

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

extern "C" bool lsw_save_init() {
    if (mounted) return true;
    user = {};
    nickname[0] = 0;
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) return fail("account service", rc);
    bool selected = R_SUCCEEDED(accountGetPreselectedUser(&user)) && accountUidIsValid(&user);
    if (!selected)
        selected = R_SUCCEEDED(accountTrySelectUserWithoutInteraction(&user, false)) && accountUidIsValid(&user);
    if (!selected) {
        PselUserSelectionSettings settings{};
        settings.is_unqualified_user_selectable = 1; // Local users need no linked Nintendo Account.
        rc = pselShowUserSelector(&user, &settings);
        selected = R_SUCCEEDED(rc) && accountUidIsValid(&user);
    }
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
