// S105 SKB3 (note docs\research\s105\SKB3.md, docs\formats\evtobj_pac.md "S105 SKB3"): a RANGED reader for an XCTD pac (first word 0x0FF512ED), for the disc's pac\evt\evtobj.pac
// (162 MB on disc, 181 MB unpacked): index every block's file offset with small ranged reads (seek + the 2-byte chunk headers; ~21 ms for 1241 blocks), then decode ONLY the
// blocks a byte range touches. Every XCTD block decodes from a fresh LZX state (native_lzx.h), so any block can be decoded alone, which is what the game's own read job does
// (sub_82CA0120, docs\systems\file_loading.md). A prop PACH is 2-4 blocks plus block 0 for the EPAC directory: ~25 ms instead of the whole 1.1 s unpack.
// Layout, walked exactly as xctd_file.h Unpack does (the same gap rule), so the output bytes are identical to the whole unpack's. No SDK headers, no game bytes in this file.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../native_lzx.h"

namespace wwe2k14::xctd_ranged {

// Reads exactly n bytes at `off` of the (compressed) file into out; false on any failure.
using ReadFn = std::function<bool(uint64_t off, uint8_t* out, size_t n)>;

struct Index {
  uint64_t file_size = 0, out_total = 0;       // compressed size; the unpacked size
  uint32_t count = 0, window = 0;
  std::vector<uint32_t> sizes;                 // each block's unpacked size
  std::vector<uint64_t> start, end, out_off;   // each block: [start, end) in the file; its offset in the unpacked bytes
  size_t reads = 0;                            // ranged reads the build made (the proof counts it)
};

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Builds the block index. Refused (false + err) unless the walk ends exactly at `file_size` (a modded / odd pac then takes the caller's whole-file path), the file is XCTD with a block table
// (count > 0) and every size is sane.
inline bool BuildIndex(const ReadFn& rd, uint64_t file_size, Index& ix, std::string& err, bool honor_gap = true /* S105 SKB3 fix L4: false = TEST ONLY (the unit's skip_gap twin plants a reader that ignores the writer's zero gap) */) {
  ix = {};
  ix.file_size = file_size;
  auto read = [&](uint64_t at, uint8_t* o, size_t n) { ++ix.reads; return at + n <= file_size && rd(at, o, n); };
  uint8_t h[16];
  if (!read(0, h, 16) || Be32(h) != 0x0FF512EDu) return err = "not an XCTD file", false;
  const uint32_t flags = Be32(h + 12);
  ix.window = 1u << ((flags & 15) + 15);
  ix.count = (flags >> 6) & 0xFFFF;
  const uint32_t width = ((flags >> 22) & 3) == 0 ? 20 : 32;
  if (ix.count == 0) return err = "XCTD stored file (no block table)", false;
  const uint64_t table_end = uint64_t((uint64_t(ix.count) * width + 31) / 32 + 4) * 4;
  if (table_end > file_size) return err = "XCTD block table runs past the file", false;
  std::vector<uint8_t> tab(static_cast<size_t>(table_end), uint8_t(0));
  if (!read(0, tab.data(), tab.size())) return err = "XCTD table read failed", false;
  ix.sizes.resize(ix.count);
  for (uint32_t i = 0; i < ix.count; ++i) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < width; ++b) {
      const uint64_t bit = uint64_t(i) * width + b;
      v = v << 1 | ((tab[16 + size_t(bit / 8)] >> (7 - bit % 8)) & 1);
    }
    if (v > (64u << 20)) return err = "XCTD block " + std::to_string(i) + " claims " + std::to_string(v) + " bytes", false;
    ix.sizes[i] = v;
  }
  ix.start.resize(ix.count), ix.end.resize(ix.count), ix.out_off.resize(ix.count);
  uint64_t ip = table_end, out = 0;
  for (uint32_t i = 0; i < ix.count; ++i) {
    // the writer never starts a block in the last few bytes before a 64 KB boundary of the file: zeros fill them and the block starts on the boundary (xctd_file.h, Session 95)
    const uint64_t gap = (0x10000 - ip % 0x10000) % 0x10000;
    bool zeros = honor_gap && gap > 0 && gap < 256 && ip + gap <= file_size;
    if (zeros) {
      uint8_t g[256];
      if (!read(ip, g, size_t(gap))) return err = "XCTD gap read failed", false;
      for (uint64_t k = 0; k < gap; ++k) if (g[k]) { zeros = false; break; }
    }
    const uint64_t s = zeros ? ip + gap : ip;
    uint64_t e = s;
    uint8_t c[2];
    if (!read(e, c, 2)) return err = "XCTD chunk header read failed (block " + std::to_string(i) + ")", false;
    if (c[0] == 0 && c[1] == 0) {
      e += 2 + ix.sizes[i];
    } else {
      for (uint32_t d = 0; d < ix.sizes[i]; d += 32768) {
        if (!read(e, c, 2)) return err = "XCTD chunk header read failed (block " + std::to_string(i) + ")", false;
        e += 2 + (uint32_t(c[0]) << 8 | c[1]);
      }
    }
    if (e > file_size) return err = "XCTD block " + std::to_string(i) + " runs past the file", false;
    ix.start[i] = s, ix.end[i] = e, ix.out_off[i] = out;
    out += ix.sizes[i];
    ip = e;
  }
  if (ip != file_size) return err = "XCTD block walk ends at " + std::to_string(ip) + " of " + std::to_string(file_size), false;
  ix.out_total = out;
  return true;
}

// The unpacked bytes [off, off + len) (a range past the unpacked size is an error), decoding only the blocks that cover them.
inline bool DecodeRange(const ReadFn& rd, const Index& ix, uint64_t off, uint64_t len, std::vector<uint8_t>& out, std::string& err) {
  out.clear();
  if (off + len > ix.out_total || ix.count == 0) return err = "XCTD range past the unpacked size", false;
  out.resize(size_t(len));
  if (len == 0) return true;
  uint32_t i = uint32_t(std::upper_bound(ix.out_off.begin(), ix.out_off.end(), off) - ix.out_off.begin()) - 1;
  auto st = std::make_unique<native_lzx::State>();
  std::vector<uint8_t> in, blk;
  for (uint64_t pos = off; pos < off + len && i < ix.count; ++i) {
    const size_t clen = size_t(ix.end[i] - ix.start[i]);
    in.assign(clen + 16, 0);   // the decoder may read a few bytes past a chunk
    if (!rd(ix.start[i], in.data(), clen)) return err = "XCTD block " + std::to_string(i) + " read failed", false;
    blk.assign(size_t(ix.sizes[i]) + 8, 0);
    const native_lzx::Result r = native_lzx::DecodeBlock(in.data(), uint32_t(clen), ix.sizes[i], ix.window, blk.data(), *st);
    if (r.error != native_lzx::kOk || r.produced < ix.sizes[i]) return err = "XCTD block " + std::to_string(i) + ": LZX error " + std::to_string(r.error), false;
    const uint64_t b0 = ix.out_off[i], b1 = b0 + ix.sizes[i];
    const uint64_t from = std::max(pos, b0), to = std::min(off + len, b1);
    std::memcpy(out.data() + (from - off), blk.data() + (from - b0), size_t(to - from));
    pos = to;
  }
  return true;
}

}  // namespace wwe2k14::xctd_ranged
