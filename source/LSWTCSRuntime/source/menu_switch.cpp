// Rev 1 menu adaptation. Filter entry creation (so counts/layout follow), and translate
// the selected action while running the original update. Keep the game's save-slot UI.
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include "lsw_menu_policy.h"
#include <cstdio>

extern "C" PPC_FUNC(__imp__sub_824E8340);
extern "C" PPC_FUNC(__imp__sub_8241AAC8);
extern "C" PPC_FUNC(__imp__sub_8221B800);
extern "C" PPC_FUNC(__imp__sub_82218128);

namespace {
bool active = false;
uint32_t read32(uint8_t* base, uint32_t a) { uint32_t x; lsw_guest::copy(&x, base + ppc_fold(a), 4); return __builtin_bswap32(x); }
uint16_t read16(uint8_t* base, uint32_t a) { uint16_t x; lsw_guest::copy(&x, base + ppc_fold(a), 2); return __builtin_bswap16(x); }
void write32(uint8_t* base, uint32_t a, uint32_t x) { x = __builtin_bswap32(x); lsw_guest::copy(base + ppc_fold(a), &x, 4); }
bool brightness_entry(uint8_t* base) {
    const int16_t id = int16_t(read16(base, 0x82FD3FE4));
    const uint32_t table = read32(base, 0x830C1EF8);
    return id >= 0 && table && read32(base, table + uint32_t(id) * 4) != 0;
}
PPC_FUNC(update_main) {
    const uint32_t menu = ctx.r3.u32;
    const uint32_t visible = read32(base, menu + 0x34);
    const uint32_t original = lsw_menu::main_original_item(visible);
    write32(base, menu + 0x34, original);
    __imp__sub_8221B800(ctx, base);
    if (read32(base, menu + 0x34) == original) write32(base, menu + 0x34, visible);
}
PPC_FUNC(update_options) {
    const uint32_t menu = ctx.r3.u32;
    const uint32_t visible = read32(base, menu + 0x34);
    const bool device_present = read32(base, 0x83175880) != 12;
    const uint32_t original = lsw_menu::options_original_item(visible, brightness_entry(base), device_present);
    write32(base, menu + 0x34, original);
    __imp__sub_82218128(ctx, base);
    if (read32(base, menu + 0x34) == original) write32(base, menu + 0x34, visible);
}
}

// Strong symbols replace the generated weak aliases, including direct game calls.
PPC_FUNC(sub_824E8340) {
    if (active && uint32_t(ctx.lr) == lsw_menu::main_live_draw_return) return;
    __imp__sub_824E8340(ctx, base);
}
PPC_FUNC(sub_8241AAC8) {
    if (active && uint32_t(ctx.lr) == lsw_menu::options_device_draw_return) return;
    __imp__sub_8241AAC8(ctx, base);
}
extern "C" bool lsw_install_switch_menus(uint8_t* base) {
    // Verify the owned build's menu table before using fixed structure/call-site addresses.
    if (read32(base, 0x82E7F044) != 1 || read32(base, 0x82E7F04C) != 0x821328B0 ||
        read32(base, 0x82E7F050) != 0x821307E0 || read32(base, 0x82E7F0A4) != 0x82132B70) {
        printf("[MENU] Unexpected menu table: adaptation refused\n");
        return false;
    }
    PPC_LOOKUP_FUNC(base, 0x824E8340) = sub_824E8340;
    PPC_LOOKUP_FUNC(base, 0x8241AAC8) = sub_8241AAC8;
    PPC_LOOKUP_FUNC(base, 0x8221B800) = update_main;
    PPC_LOOKUP_FUNC(base, 0x82218128) = update_options;
    active = true;
    printf("[MENU] Xbox Live and storage-device entries removed; original New/Load Game slots retained\n");
    return true;
}
