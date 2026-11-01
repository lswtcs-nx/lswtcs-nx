// Exports Image::ParseImage's bytes without executing guest code.
// This is a clean loader image, not a snapshot of a running Xenia process.
#include <image.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: tcs_image_export input.xex new-output.bin\n";
        return 2;
    }
    const std::filesystem::path input(argv[1]), output(argv[2]);
    if (std::filesystem::exists(output)) {
        std::cerr << "Output already exists; refusing to overwrite\n";
        return 2;
    }
    std::ifstream stream(input, std::ios::binary);
    if (!stream) { std::cerr << "Cannot open input\n"; return 1; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    if (bytes.size() < 0x20 || bytes[0] != 'X' || bytes[1] != 'E' ||
        bytes[2] != 'X' || bytes[3] != '2') {
        std::cerr << "Expected an XEX2 file\n";
        return 1;
    }
    auto image = Image::ParseImage(bytes.data(), bytes.size());
    if (!image.data || !image.size || image.sections.empty()) {
        std::cerr << "Loader did not produce an image\n";
        return 1;
    }
    std::ofstream dest(output, std::ios::binary);
    if (!dest) { std::cerr << "Cannot create output\n"; return 1; }
    dest.write(reinterpret_cast<const char*>(image.data.get()), image.size);
    dest.close();
    if (!dest) { std::cerr << "Output write failed\n"; return 1; }
    std::cout << std::hex << "base=0x" << image.base << " entry=0x"
              << image.entry_point << " bytes=0x" << image.size << '\n';
    for (const auto& section : image.sections)
        std::cout << section.name.substr(0, 8) << " base=0x" << section.base
                  << " bytes=0x" << section.size << '\n';
}
