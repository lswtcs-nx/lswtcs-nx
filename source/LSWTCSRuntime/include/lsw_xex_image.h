// Builds the game's loaded image from default.xex (log107), so a player needs only the files of their own disc.
//
// The image the port was developed on, image.bin, was a snapshot of Xenia running the game. Outside .data it differs from
// the clean loader image only by Xenia's import resolution: every record of the XEX import table is rewritten (a thunk in
// .xidata becomes "sc 2; blr; nop; nop", a record in .idata becomes 0xDEADC0DE), and one word of .XBMOVIE holds 0. .data
// itself is replaced by the clean bytes at boot (LSWTCS_PRISTINE=1), and the 13 kernel-variable records are bound again
// from the header (kernel_stubs.cpp). So the clean image plus these rewrites gives the guest the same memory at boot as
// image.bin did; tests/xex_image_check.py checks that against a reference image.bin.
//
// Supports the XEX2 forms this game uses: retail encryption (AES-128-CBC) or none, and no or basic compression. The
// caller supplies the AES decryption (libnx's hardware AES on the Switch).
#pragma once
#include <cstddef>
#include <cstdint>

namespace lsw_xex {

struct Info {
    uint32_t header_size = 0;    // bytes of XEX header before the payload (also the size of xex_header.bin)
    uint32_t image_size = 0;     // bytes of the loaded image
    uint32_t load_address = 0;   // guest address of the image
    uint32_t entry_point = 0;
    uint16_t encryption = 0;     // 0 none, 1 retail
    uint16_t compression = 0;    // 0 none, 1 basic (2 = LZX, 3 = delta: not supported)
    uint8_t session_key_encrypted[16] = {};
};

// AES-128-CBC decryption, zero IV at start, streaming: each call continues the chain of the previous one. In place.
struct Decryptor {
    virtual ~Decryptor() = default;
    virtual void start(const uint8_t key[16]) = 0;
    virtual void decrypt(uint8_t* data, size_t size) = 0;   // size is a multiple of 16
};

// The retail key that wraps each XEX's session key (public, as in Xenia and XenonRecomp).
extern const uint8_t kRetailKey[16];

// Reads the header. Returns nullptr, or a short English error.
const char* parse(const uint8_t* xex, size_t size, Info* out);

// Decrypts the payload in place (xex is modified) and writes the image to image[0, info.image_size). Returns nullptr or an error.
const char* build_image(uint8_t* xex, size_t size, const Info& info, uint8_t* image, size_t capacity, Decryptor& aes);

// Applies the import resolution and snapshot words described above. thunks/records count what was written.
const char* resolve_like_snapshot(const uint8_t* header, size_t header_size, uint8_t* image, uint32_t image_base,
                                  size_t image_size, unsigned* thunks, unsigned* records);

}  // namespace lsw_xex
