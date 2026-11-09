// Session 96 (asset runtime loader): reads a pac from the player's own game files (CLAUDE.md rules 1 and 2). Header-only,
// no SDK headers. Chain, proven by tools\wm30\pac.py: whole-file XCTD unpack -> EPAC (16 KB header) -> PACH archives one
// after another (each padded to 2 KB) -> entries decoded by their tag ('ZLIB', 'BPE ', else plain); an entry may be
// another PACH (nested). PACH: 'PACH', LE32 count, count x (LE32 id, LE32 offset, LE32 size), data at 8 + 12*count + offset.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../launcher/disc_image.h"
#include "../native_bpe.h"
#include "../xctd_file.h"

// Needs stb_image.h (stbi_zlib_decode_buffer) included before this header, once per program with STB_IMAGE_IMPLEMENTATION

namespace assets {
using Bytes = std::vector<uint8_t>;

inline uint32_t Le32(const Bytes& b, size_t o) {
  uint32_t v = 0;
  if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
  return v;
}
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline float BeF(const uint8_t* p) {
  const uint32_t v = Be32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

namespace pac {

struct Ent {
  uint32_t id = 0;
  size_t off = 0, size = 0;  // into the owning buffer
};

// One PACH archive inside a buffer.
struct Pach {
  size_t base = 0, end = 0;  // [base, end) in the buffer
  std::vector<Ent> ents;
};

// The whole pac, unpacked.
struct Pac {
  Bytes bytes;
  std::vector<Pach> pachs;  // the top-level PACH archives ("p0", "p1", ...)
};

// Parse the PACH at b[base...]; false when it is not one or runs past the buffer.
inline bool ParsePach(const Bytes& b, size_t base, size_t limit, Pach& out) {
  out = {};
  if (base + 8 > limit || std::memcmp(b.data() + base, "PACH", 4) != 0) return false;
  const uint32_t count = Le32(b, base + 4);
  if (count > 100000 || base + 8 + 12 * size_t(count) > limit) return false;
  const size_t first = base + 8 + 12 * size_t(count);
  out.base = base, out.end = first;
  for (uint32_t i = 0; i < count; ++i) {
    Ent e;
    e.id = Le32(b, base + 8 + 12 * size_t(i));
    e.off = first + Le32(b, base + 8 + 12 * size_t(i) + 4);
    e.size = Le32(b, base + 8 + 12 * size_t(i) + 8);
    if (e.off > limit || e.size > limit - e.off) return false;
    out.end = std::max(out.end, e.off + e.size);
    out.ents.push_back(e);
  }
  return true;
}

// Raw file bytes -> the pac. A game pac is XCTD-wrapped; a community character pac (Session 98) is NOT: it starts with 'EPAC' or
// 'EPK8' directly (same 16 KB header, PACH at 0x4000). The bytes are untrusted: every offset is bounds-checked by ParsePach.
inline bool OpenBytes(Bytes raw, const std::string& rel, Pac& pac, std::string& err) {
  pac = {};
  if (!wwe2k14::xctd::IsXctd(raw)) {
    pac.bytes = std::move(raw);  // not compressed
  } else {
    std::string e2;
    if (!wwe2k14::xctd::Unpack(raw, pac.bytes, e2)) return err = rel + ": " + e2, false;
  }
  raw = Bytes();
  const Bytes& b = pac.bytes;
  if (b.size() < 16) return err = rel + ": too small", false;
  const bool headed = std::memcmp(b.data(), "EPAC", 4) == 0 || std::memcmp(b.data(), "EPK8", 4) == 0;
  size_t pos = headed ? 0x4000 : 0;
  while (pos + 8 <= b.size()) {
    Pach p;
    if (std::memcmp(b.data() + pos, "PACH", 4) != 0 || !ParsePach(b, pos, b.size(), p)) {
      pos += 0x800;
      continue;
    }
    pos = (p.end + 0x7FF) & ~size_t(0x7FF);
    pac.pachs.push_back(std::move(p));
  }
  if (pac.pachs.empty()) return err = rel + ": no PACH found", false;
  return true;
}

// The whole file, unpacked (XCTD -> EPAC -> top-level PACHs).
inline bool Open(launcher::GameFiles& files, const std::string& rel, Pac& pac, std::string& err) {
  pac = {};
  Bytes raw;
  if (!files.Exists(rel)) return err = "missing " + rel, false;
  if (!files.Stream(rel, [&](const uint8_t* p, size_t n) { raw.insert(raw.end(), p, p + n); }))
    return err = "read failed " + rel, false;
  return OpenBytes(std::move(raw), rel, pac, err);
}

// Decode a leaf by its tag ('ZLIB', 'BPE ', else a copy). Empty on failure.
inline Bytes Decode(const uint8_t* d, size_t n) {
  if (n >= 16 && std::memcmp(d, "ZLIB", 4) == 0) {
    // 16-byte head: tag, version, packed size, size; zlib stream follows.
    uint32_t packed = 0, full = 0;
    std::memcpy(&packed, d + 8, 4), std::memcpy(&full, d + 12, 4);
    if (full == 0 || full > (256u << 20) || size_t(packed) + 16 > n) return {};
    Bytes out(full);
    const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), int(full),
                                            reinterpret_cast<const char*>(d + 16), int(packed));
    if (got != int(full)) return {};
    return out;
  }
  if (n >= 16 && std::memcmp(d, "BPE ", 4) == 0) {
    const uint32_t full = wwe2k14::native_bpe::ExpandedSize(d);
    if (full == 0 || full > (256u << 20)) return {};
    Bytes out(full + 16);
    const auto r = wwe2k14::native_bpe::DecodeReference(d, out.data(), full + 16, uint32_t(n - 16));
    if (r.error != 0 || r.produced < full) return {};
    out.resize(full);
    return out;
  }
  return Bytes(d, d + n);
}

inline const Ent* Find(const Pach& p, uint32_t id) {
  for (const Ent& e : p.ents)
    if (e.id == id) return &e;
  return nullptr;
}

// Decoded entry `id` of a PACH living in `buf`.
inline Bytes Get(const Bytes& buf, const Pach& p, uint32_t id) {
  const Ent* e = Find(p, id);
  return e ? Decode(buf.data() + e->off, e->size) : Bytes();
}

// Decoded nested PACH entry `id` -> its own buffer + parsed table.
inline bool GetNested(const Bytes& buf, const Pach& p, uint32_t id, Bytes& nbuf, Pach& np) {
  nbuf = Get(buf, p, id);
  return !nbuf.empty() && ParsePach(nbuf, 0, nbuf.size(), np);
}

}  // namespace pac
}  // namespace assets
