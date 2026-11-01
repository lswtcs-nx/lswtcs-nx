#pragma once
#include <cstdint>

namespace lsw_menu {
constexpr uint32_t main_live_draw_return = 0x82219054;
constexpr uint32_t options_device_draw_return = 0x822188B8;
inline uint32_t main_original_item(uint32_t visible) { return visible >= 2 ? visible + 1 : visible; }
inline uint32_t options_original_item(uint32_t visible, bool brightness, bool device_present) {
    const uint32_t device_row = brightness ? 4 : 3;
    return device_present && visible >= device_row ? visible + 1 : visible;
}
}
