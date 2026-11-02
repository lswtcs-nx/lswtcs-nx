// PNG encoder for capture files: filter Up, zlib level 1, and each 256 KB of output handed to `sink` in one piece.
// Run 10's two full captures took about 100 s each to reach the SD card, with most slow frames of the session inside those windows. On its images,
// level 1 with this filter leaves about 40 % of the bytes (29 % of resolve dumps, 61 % of history frames), and filter Up is the best of None, Sub and Up there.
// `sink(const uint8_t*, size_t)` returns false to give up. No lock, no file: tests/run_png_write.py runs it on a PC.
#pragma once
#include <zlib.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

inline void lsw_png_be32(uint8_t* out, uint32_t v) {
  out[0] = uint8_t(v >> 24); out[1] = uint8_t(v >> 16); out[2] = uint8_t(v >> 8); out[3] = uint8_t(v);
}
// A complete chunk at `out`, whose data is already at out + 8. Returns the bytes used.
inline size_t lsw_png_finish_chunk(uint8_t* out, const char* type, size_t data_size) {
  lsw_png_be32(out, uint32_t(data_size));
  std::memcpy(out + 4, type, 4);
  lsw_png_be32(out + 8 + data_size, uint32_t(crc32(crc32(0, nullptr, 0), out + 4, uInt(4 + data_size))));
  return 12 + data_size;
}

template <class Sink>
bool lsw_png_encode(const uint8_t* rgba, uint32_t w, uint32_t h, Sink&& sink) {
  constexpr size_t kData = size_t(256) << 10, kHead = 8 + 12 + 13, kTail = 12 + 12;
  const size_t stride = size_t(w) * 4;
  std::unique_ptr<uint8_t[]> store(new (std::nothrow) uint8_t[kHead + 8 + kData + kTail]);
  std::unique_ptr<uint8_t[]> row(new (std::nothrow) uint8_t[stride + 1]);
  if (!store || !row) return false;
  uint8_t* const buf = store.get();
  static const uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  std::memcpy(buf, kSignature, 8);
  uint8_t* const ihdr = buf + 8 + 8;
  lsw_png_be32(ihdr, w);
  lsw_png_be32(ihdr + 4, h);
  const uint8_t format[5] = {8, 6, 0, 0, 0};
  std::memcpy(ihdr + 8, format, 5);
  size_t chunk = 8 + lsw_png_finish_chunk(buf + 8, "IHDR", 13);   // where the current IDAT starts: after the header, in the first piece only

  z_stream z;
  std::memset(&z, 0, sizeof z);
  if (deflateInit(&z, 1) != Z_OK) return false;
  z.next_out = buf + chunk + 8;
  z.avail_out = uInt(kData);
  auto put = [&](bool last) {
    size_t end = chunk + lsw_png_finish_chunk(buf + chunk, "IDAT", kData - z.avail_out);
    if (last) end += lsw_png_finish_chunk(buf + end, "IEND", 0);
    chunk = 0;
    z.next_out = buf + 8;
    z.avail_out = uInt(kData);
    return sink(static_cast<const uint8_t*>(buf), end);
  };
  bool ok = true;
  row[0] = 2;   // filter Up; the row above the first one counts as zeros
  for (uint32_t y = 0; ok && y < h; ++y) {
    const uint8_t* const cur = rgba + y * stride;
    if (y) {
      const uint8_t* const above = cur - stride;
      for (size_t x = 0; x < stride; ++x) row[1 + x] = uint8_t(cur[x] - above[x]);
    } else {
      std::memcpy(row.get() + 1, cur, stride);
    }
    z.next_in = row.get();
    z.avail_in = uInt(stride + 1);
    while (ok && z.avail_in) {
      if (deflate(&z, Z_NO_FLUSH) != Z_OK) ok = false;
      else if (!z.avail_out) ok = put(false);
    }
  }
  while (ok) {
    const int rc = deflate(&z, Z_FINISH);
    if (rc == Z_STREAM_END) { ok = put(true); break; }
    if (z.avail_out || (rc != Z_OK && rc != Z_BUF_ERROR)) ok = false;   // Z_FINISH stops short only when the output is full
    else ok = put(false);
  }
  deflateEnd(&z);
  return ok;
}
