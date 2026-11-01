// Included in kernel_stubs.cpp for the desktop regression entry point. Calls the
// actual XAM/NT implementations, with real host files in a fresh private fixture.
#ifndef __SWITCH__
extern "C" bool lswtcs_save_api_regression(uint8_t* base) {
    const char* dir = getenv("LSWTCS_SAVEDIR");
    const char* mode = getenv("LSWTCS_SAVE_API_TEST");
    if (!dir || !strstr(dir, "tcs360-save-regression-") || !mode ||
        (strcmp(mode, "create") && strcmp(mode, "verify"))) return false;
    unsigned checks = 0;
#define SAVE_CHECK(x) do { ++checks; if (!(x)) { printf("[SAVE-TEST] FAIL line %d: %s\n", __LINE__, #x); return false; } } while (0)
    constexpr uint32_t arena = 0x90000000;
    constexpr uint32_t root = arena + 0x100, data = arena + 0x200, out = arena + 0x400;
    constexpr uint32_t attr = arena + 0x500, str = arena + 0x520, path = arena + 0x540;
    constexpr uint32_t iosb = arena + 0x600, bytes = arena + 0x700, buffer = arena + 0x800;
    lsw_guest::fill(base + arena, 0, 0x10000);
    lsw_guest::copy(base + root, "save", 5);
    PPCContext ctx{};
    auto reset = [&] { ctx = {}; ctx.r1.u32 = arena + 0xF000; };
    auto package = [&](const char* name) {
        lsw_guest::fill(base + data, 0, kXContentDataSize);
        PPC_STORE_U32(data, 1); PPC_STORE_U32(data + 4, 1);
        lsw_guest::copy(base + data + 0x108, name, strlen(name) + 1);
    };
    auto mount = [&](uint32_t flags) {
        reset(); ctx.r3.u32 = 0; ctx.r4.u32 = root; ctx.r5.u32 = data;
        ctx.r6.u32 = flags; ctx.r7.u32 = out;
        __imp__XamContentCreateEx(ctx, base);
        return ctx.r3.u32;
    };
    auto close_package = [&] { reset(); ctx.r3.u32 = root; __imp__XamContentClose(ctx, base); return ctx.r3.u32; };
    const bool create = strcmp(mode, "create") == 0;
    const char* slots[] = {"lswtcs_test_slot_1", "lswtcs_test_slot_2"};
    for (unsigned slot = 0; slot != 2; ++slot) {
        package(slots[slot]);
        SAVE_CHECK(mount(create ? 1 : 3) == 0);
        SAVE_CHECK(PPC_LOAD_U32(out) == (create ? 1u : 2u));
        const char* filename = "save:\\data.bin";
        lsw_guest::copy(base + path, filename, strlen(filename) + 1);
        PPC_STORE_U32(attr + 4, str); PPC_STORE_U16(str, uint16_t(strlen(filename))); PPC_STORE_U32(str + 4, path);
        reset(); ctx.r3.u32 = out; ctx.r5.u32 = attr; ctx.r6.u32 = iosb; ctx.r10.u32 = create ? 2 : 1;
        __imp__NtCreateFile(ctx, base);
        SAVE_CHECK(ctx.r3.u32 == 0);
        const uint32_t handle = PPC_LOAD_U32(out);
        const char* expected = slot ? "SLOT_TWO" : "SLOT_ONE";
        if (create) {
            lsw_guest::copy(base + bytes, "OLD_SAVE", 8);
            reset(); ctx.r3.u32 = handle; ctx.r7.u32 = iosb; ctx.r8.u32 = bytes; ctx.r9.u32 = 8;
            __imp__NtWriteFile(ctx, base);
            SAVE_CHECK(ctx.r3.u32 == 0 && PPC_LOAD_U32(iosb + 4) == 8);
            PPC_STORE_U64(out + 0x10, 0);
            reset(); ctx.r3.u32 = handle; ctx.r7.u32 = iosb; ctx.r8.u32 = buffer; ctx.r9.u32 = 8; ctx.r10.u32 = out + 0x10;
            __imp__NtReadFile(ctx, base);
            SAVE_CHECK(ctx.r3.u32 == 0 && lsw_guest::compare(base + buffer, "OLD_SAVE", 8) == 0);
            lsw_guest::copy(base + bytes, expected, 8);
            reset(); ctx.r3.u32 = handle; ctx.r7.u32 = iosb; ctx.r8.u32 = bytes; ctx.r9.u32 = 8; ctx.r10.u32 = out + 0x10;
            __imp__NtWriteFile(ctx, base);
            SAVE_CHECK(ctx.r3.u32 == 0 && PPC_LOAD_U32(iosb + 4) == 8);
            PPC_STORE_U64(out + 0x10, 8);
            reset(); ctx.r3.u32 = handle; ctx.r4.u32 = iosb; ctx.r5.u32 = out + 0x10; ctx.r6.u32 = 8; ctx.r7.u32 = 20;
            __imp__NtSetInformationFile(ctx, base);
            SAVE_CHECK(ctx.r3.u32 == 0);
            reset(); ctx.r3.u32 = handle; ctx.r4.u32 = iosb;
            __imp__NtFlushBuffersFile(ctx, base);
            SAVE_CHECK(ctx.r3.u32 == 0 && PPC_LOAD_U32(iosb) == 0);
        }
        PPC_STORE_U64(out + 0x10, 0);
        reset(); ctx.r3.u32 = handle; ctx.r7.u32 = iosb; ctx.r8.u32 = buffer; ctx.r9.u32 = 8; ctx.r10.u32 = out + 0x10;
        __imp__NtReadFile(ctx, base);
        SAVE_CHECK(ctx.r3.u32 == 0 && PPC_LOAD_U32(iosb + 4) == 8);
        SAVE_CHECK(lsw_guest::compare(base + buffer, expected, 8) == 0);
        const char* escape = "save:\\..\\another_slot\\data.bin";
        lsw_guest::copy(base + path, escape, strlen(escape) + 1); PPC_STORE_U16(str, uint16_t(strlen(escape)));
        reset(); ctx.r3.u32 = out; ctx.r5.u32 = attr; ctx.r6.u32 = iosb; ctx.r10.u32 = 2;
        __imp__NtCreateFile(ctx, base); SAVE_CHECK(ctx.r3.u32 != 0);
        reset(); ctx.r3.u32 = handle; __imp__NtClose(ctx, base); SAVE_CHECK(ctx.r3.u32 == 0);
        reset(); ctx.r3.u32 = root; __imp__XamContentFlush(ctx, base); SAVE_CHECK(ctx.r3.u32 == 0);
        SAVE_CHECK(close_package() == 0);
        // Creating an existing slot must fail without changing it (overwrite needs the game's confirmation).
        if (create) { SAVE_CHECK(mount(1) == 0xB7u); }
    }
    reset(); ctx.r3.u32 = 0; ctx.r4.u32 = 1; ctx.r5.u32 = 1; ctx.r7.u32 = 64;
    ctx.r8.u32 = out; ctx.r9.u32 = out + 4;
    __imp__XamContentCreateEnumerator(ctx, base);
    SAVE_CHECK(ctx.r3.u32 == 0 && PPC_LOAD_U32(out) == 64 * kXContentDataSize);
    const uint32_t enumeration = PPC_LOAD_U32(out + 4);
    reset(); ctx.r3.u32 = enumeration; ctx.r5.u32 = arena + 0x2000;
    ctx.r6.u32 = 64 * kXContentDataSize; ctx.r7.u32 = out;
    __imp__XamEnumerate(ctx, base);
    SAVE_CHECK(ctx.r3.u32 == 0 && PPC_LOAD_U32(out) == 2);
    reset(); ctx.r3.u32 = enumeration; ctx.r5.u32 = arena + 0x2000;
    ctx.r6.u32 = 64 * kXContentDataSize; ctx.r7.u32 = out;
    __imp__XamEnumerate(ctx, base);
    SAVE_CHECK(ctx.r3.u32 == 0x12u && PPC_LOAD_U32(out) == 0);
    package("..");
    SAVE_CHECK(mount(4) == 0x7Bu);
    package("../another_user");
    SAVE_CHECK(mount(4) == 0x7Bu);
    reset(); ctx.r3.u32 = 0xDEADBEEFu; ctx.r4.u32 = iosb;
    __imp__NtFlushBuffersFile(ctx, base);
    SAVE_CHECK(ctx.r3.u32 == 0xC0000008u && PPC_LOAD_U32(iosb) == 0xC0000008u);
    printf("[SAVE-TEST] %s: %u actual XAM/NT checks passed\n", mode, checks);
#undef SAVE_CHECK
    return true;
}
#endif
