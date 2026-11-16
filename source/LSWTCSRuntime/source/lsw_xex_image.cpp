// See lsw_xex_image.h. XEX2 layout as documented by Xenia (xex_module.cc) and XenonRecomp (XenonUtils/xex.h).
#include "lsw_xex_image.h"

#include <cstring>

namespace lsw_xex {

const uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

namespace {

constexpr uint32_t kOptFileFormat = 0x000003FF;
constexpr uint32_t kOptEntryPoint = 0x00010100;
constexpr uint32_t kOptImageBase = 0x00010201;
constexpr uint32_t kOptImports = 0x000103FF;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// The optional header with this key: its inline value (low byte 0 or 1) or the offset of its data in the header.
bool opt_header(const uint8_t* xex, size_t size, uint32_t key, uint32_t* value) {
    if (size < 0x18) return false;
    const uint32_t count = be32(xex + 0x14);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 0x18 + size_t(i) * 8;
        if (at + 8 > size) return false;
        if (be32(xex + at) == key) { *value = be32(xex + at + 4); return true; }
    }
    return false;
}

}  // namespace

const char* parse(const uint8_t* xex, size_t size, Info* out) {
    if (size < 0x18 || std::memcmp(xex, "XEX2", 4) != 0) return "not an XEX2 file";
    Info info;
    info.header_size = be32(xex + 0x08);
    const uint32_t security = be32(xex + 0x10);
    if (info.header_size < 0x18 || info.header_size >= size || security + 0x160 > info.header_size)
        return "XEX header out of range";
    info.image_size = be32(xex + security + 0x04);
    info.load_address = be32(xex + security + 0x110);
    std::memcpy(info.session_key_encrypted, xex + security + 0x150, 16);
    uint32_t v = 0;
    if (opt_header(xex, size, kOptImageBase, &v)) info.load_address = v;
    if (opt_header(xex, size, kOptEntryPoint, &v)) info.entry_point = v;
    if (!opt_header(xex, size, kOptFileFormat, &v) || v + 8 > info.header_size) return "XEX has no file format header";
    info.encryption = be16(xex + v + 4);
    info.compression = be16(xex + v + 6);
    if (info.encryption > 1) return "unsupported XEX encryption";
    if (info.compression > 1) return "unsupported XEX compression (only none and basic)";
    *out = info;
    return nullptr;
}

const char* build_image(uint8_t* xex, size_t size, const Info& info, uint8_t* image, size_t capacity, Decryptor& aes) {
    if (info.image_size > capacity) return "XEX image larger than the guest image area";
    uint8_t* payload = xex + info.header_size;
    const size_t payload_size = size - info.header_size;
    if (info.encryption == 1) {
        if (payload_size % 16) return "encrypted XEX payload is not a whole number of AES blocks";
        uint8_t session[16];
        std::memcpy(session, info.session_key_encrypted, 16);
        aes.start(kRetailKey);
        aes.decrypt(session, 16);
        aes.start(session);
        aes.decrypt(payload, payload_size);
    }
    std::memset(image, 0, info.image_size);
    if (info.compression == 0) {
        if (payload_size < info.image_size) return "XEX payload shorter than its image";
        std::memcpy(image, payload, info.image_size);
        return nullptr;
    }
    // Basic compression: blocks of (stored bytes, zero bytes) right after the file format header's first 8 bytes.
    uint32_t ff = 0;
    opt_header(xex, size, kOptFileFormat, &ff);
    const uint32_t info_size = be32(xex + ff);
    if (info_size < 16 || ff + info_size > info.header_size) return "bad basic compression table";
    const uint32_t blocks = info_size / 8 - 1;
    size_t in = 0, out = 0;
    for (uint32_t b = 0; b < blocks; ++b) {
        const uint32_t data = be32(xex + ff + 8 + b * 8), zero = be32(xex + ff + 12 + b * 8);
        if (in + data > payload_size || out + data + zero > info.image_size) return "basic compression block out of range";
        std::memcpy(image + out, payload + in, data);   // the zero run is already zero
        in += data;
        out += data + zero;
    }
    if (out != info.image_size) return "basic compression blocks do not fill the image";
    return nullptr;
}

const char* resolve_like_snapshot(const uint8_t* header, size_t header_size, uint8_t* image, uint32_t image_base,
                                  size_t image_size, unsigned* thunks, unsigned* records) {
    *thunks = *records = 0;
    uint32_t at = 0;
    if (!opt_header(header, header_size, kOptImports, &at) || at + 12 > header_size) return "XEX has no import table";
    const uint32_t table_size = be32(header + at), strings = be32(header + at + 4);
    if (at + table_size > header_size) return "XEX import table out of range";
    static const uint8_t kThunk[16] = {0x44, 0x00, 0x00, 0x42, 0x4E, 0x80, 0x00, 0x20,   // sc 2; blr
                                       0x60, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00};  // nop; nop
    for (uint32_t lib = at + 12 + strings; lib + 40 <= at + table_size;) {
        const uint32_t lib_size = be32(header + lib);
        const uint16_t count = be16(header + lib + 38);
        if (lib_size < 40 || lib + 40 + count * 4u > at + table_size) return "XEX import library out of range";
        for (uint16_t i = 0; i < count; ++i) {
            const uint32_t addr = be32(header + lib + 40 + i * 4u);
            if (addr < image_base || addr - image_base + 16 > image_size) return "XEX import record outside the image";
            uint8_t* p = image + (addr - image_base);
            if (p[0] != 0) { std::memcpy(p, kThunk, 16); ++*thunks; }   // record type in the top byte: 1 = thunk
            else { put32(p, 0xDEADC0DE); ++*records; }                  // variable or import address entry
        }
        lib += lib_size;
    }
    // .XBMOVIE: the one word outside .data and the import table where the snapshot differs (0x1800 in the clean image).
    constexpr uint32_t kMovieWord = 0x83284B04;
    if (kMovieWord >= image_base && kMovieWord - image_base + 4 <= image_size) {
        uint8_t* p = image + (kMovieWord - image_base);
        if (be32(p) == 0x00001800) put32(p, 0);
    }
    return nullptr;
}

}  // namespace lsw_xex
