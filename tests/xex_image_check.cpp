// Host driver for tests/xex_image_check.py: runs the runtime's XEX image builder (lsw_xex_image.cpp) on a default.xex with
// tiny-AES-c instead of the Switch's hardware AES, and writes the image the runtime would load.
// Usage: xex_image_check <default.xex> <image out> <header out>
#include "lsw_xex_image.h"

#include <cstdio>
#include <vector>

extern "C" {
#include "aes.h"
}

struct TinyAes : lsw_xex::Decryptor {
    AES_ctx ctx{};
    void start(const uint8_t key[16]) override { static const uint8_t iv[16] = {}; AES_init_ctx_iv(&ctx, key, iv); }
    void decrypt(uint8_t* data, size_t size) override { AES_CBC_decrypt_buffer(&ctx, data, size); }
};

static bool read_all(const char* path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    out->resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

static bool write_all(const char* path, const uint8_t* data, size_t size) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: %s default.xex image.out header.out\n", argv[0]); return 2; }
    std::vector<uint8_t> xex;
    if (!read_all(argv[1], &xex)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    lsw_xex::Info info;
    if (const char* e = lsw_xex::parse(xex.data(), xex.size(), &info)) { std::fprintf(stderr, "parse: %s\n", e); return 1; }
    const std::vector<uint8_t> header(xex.begin(), xex.begin() + info.header_size);
    std::vector<uint8_t> image(info.image_size);
    TinyAes aes;
    if (const char* e = lsw_xex::build_image(xex.data(), xex.size(), info, image.data(), image.size(), aes)) {
        std::fprintf(stderr, "build: %s\n", e);
        return 1;
    }
    unsigned thunks = 0, records = 0;
    if (const char* e = lsw_xex::resolve_like_snapshot(header.data(), header.size(), image.data(), info.load_address,
                                                       image.size(), &thunks, &records)) {
        std::fprintf(stderr, "imports: %s\n", e);
        return 1;
    }
    std::printf("header 0x%X, image 0x%X at 0x%08X, entry 0x%08X, encryption %u, compression %u, %u thunks, %u records\n",
                info.header_size, info.image_size, info.load_address, info.entry_point, info.encryption, info.compression,
                thunks, records);
    return write_all(argv[2], image.data(), image.size()) && write_all(argv[3], header.data(), header.size()) ? 0 : 1;
}
