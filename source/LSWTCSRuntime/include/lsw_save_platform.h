#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace lsw_save {
// Keep this identity identical in the HOME launcher and native account saves.
constexpr uint64_t application_id = 0x054C535443530000ull;
constexpr int64_t data_bytes = 16ll << 20;
constexpr int64_t journal_bytes = 16ll << 20;
constexpr const char* mount_name = "tcssave";
constexpr const char* root = "tcssave:/";

inline bool package_name(const std::string& s) {
    if (s.empty() || s.size() > 42 || s == "." || s == "..") return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    return true;
}

// Content paths must remain inside their selected slot. Never fall back to disc paths.
inline bool relative_path(std::string& s) {
    for (char& c : s) if (c == '\\') c = '/';
    if (s.find(':') != std::string::npos) return false;
    size_t p = 0;
    while (p <= s.size()) {
        const size_t end = s.find('/', p);
        const auto part = s.substr(p, end == std::string::npos ? end : end - p);
        if (part == ".." || part == ".") return false;
        if (end == std::string::npos) break;
        p = end + 1;
    }
    return true;
}
}

#ifdef __SWITCH__
extern "C" {
bool lsw_save_init();
void lsw_save_shutdown();
uint32_t lsw_save_commit();
bool lsw_save_space(uint64_t* total, uint64_t* available);
int lsw_save_profile(char* name, int capacity, uint8_t uid[16]);
uint32_t lsw_save_last_error();
const char* lsw_save_error_stage();
}
#endif
