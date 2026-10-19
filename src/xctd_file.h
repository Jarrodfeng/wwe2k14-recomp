// wwe2k14 - unpack a whole XCTD-compressed pac (first word 0x0FF512ED) with the native LZX block decoder
// (src/native_lzx.h). Session 34: shared by tools/wm30/xctd_unpack.cpp and the launcher's 30 YEARS list
// (src/launcher/wm30_list.h). No SDK headers.
//
// Header (big-endian; docs/systems/file_loading.md "XCTD file header", checked on real pacs in Session 34): +0x00
// magic, +0x0C flags: bits 0-3 window 1 << (n + 15), bits 6-21 block count (0 = stored, data at +16), bits 22-23 width
// index (20 or 32 bits); from +0x10 an MSB-first table of each block's uncompressed size; the blocks follow the table
// (((count x width + 31) / 32 + 4) x 4 bytes in). A block is a run of chunks (big-endian u16 length + data), one chunk
// per 32 KB of output (a zero first length = stored block), so its compressed length is found by walking them.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "native_lzx.h"

namespace wwe2k14::xctd {

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

inline bool IsXctd(const std::vector<uint8_t>& in) { return in.size() >= 16 && Be32(in.data()) == 0x0FF512EDu; }

// `in` (a whole file) -> `out`. A file that is not XCTD is copied. False + `error` on any surprise.
inline bool Unpack(const std::vector<uint8_t>& in_file, std::vector<uint8_t>& out, std::string& error) {
  out.clear();
  if (!IsXctd(in_file)) {
    out = in_file;
    return true;
  }
  std::vector<uint8_t> in(in_file);
  const size_t size = in.size();
  in.resize(size + 16, 0);  // the decoder may read a few bytes past a chunk
  const uint32_t flags = Be32(in.data() + 12);
  const uint32_t window = 1u << ((flags & 15) + 15);
  const uint32_t count = (flags >> 6) & 0xFFFF;
  const uint32_t width = ((flags >> 22) & 3) == 0 ? 20 : 32;
  if (count == 0) {
    out.assign(in.begin() + 16, in.begin() + std::ptrdiff_t(size));
    return true;
  }
  const size_t table_end = size_t((count * width + 31) / 32 + 4) * 4;
  if (table_end > size) {
    error = "XCTD block table runs past the file";
    return false;
  }
  std::vector<uint32_t> sizes(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < width; ++b) {
      const uint32_t bit = i * width + b;
      v = v << 1 | ((in[16 + bit / 8] >> (7 - bit % 8)) & 1);
    }
    sizes[i] = v;
  }
  size_t ip = table_end;
  auto st = std::make_unique<native_lzx::State>();
  for (uint32_t i = 0; i < count; ++i) {
    size_t end = ip;
    if (end + 2 <= size && in[end] == 0 && in[end + 1] == 0) {
      end += 2 + sizes[i];
    } else {
      for (uint32_t done = 0; done < sizes[i]; done += 32768) {
        if (end + 2 > size) break;
        end += 2 + (uint32_t(in[end]) << 8 | in[end + 1]);
      }
    }
    if (end > size) {
      error = "XCTD block " + std::to_string(i) + " runs past the file";
      return false;
    }
    const size_t at = out.size();
    out.resize(at + sizes[i] + 8);
    const native_lzx::Result r =
        native_lzx::DecodeBlock(in.data() + ip, uint32_t(end - ip), sizes[i], window, out.data() + at, *st);
    if (r.error != native_lzx::kOk || r.produced < sizes[i]) {
      error = "XCTD block " + std::to_string(i) + ": LZX error " + std::to_string(r.error);
      return false;
    }
    out.resize(at + sizes[i]);
    ip = end;
  }
  return true;
}

}  // namespace wwe2k14::xctd
