// Session 98 (stance): the game's per-character in-ring stance table, read at run time from the player's own default.xex.
//
// What the table is (out\s98\stance\notes.md): sub_8273AE20 (called from sub_8273C1C0, in the ring) gives each wrestler at the match
// start one act of mot00 CA01 top 30 (29000-29022) from rows {u16 character id, u16 act, u16 act, u16 act} (big-endian) at guest
// 0x82029A58 (the function's lis+addi), ended by an id of 0xFFFF; its scan assumes the first row's id is 100.
//
// How it is read: the XEX2 container (layout as in the ReXGlue SDK's xex_module.cpp / xex2_info.h, Xenia's): header_size at +8, the
// security info offset at +0x10 (aes_key at +0x150, load_address at +0x110), the optional headers {key, value} from +0x18 (count at
// +0x14); FILE_FORMAT_INFO (key 0x3FF) = {u32 info_size, u16 encryption (0 none, 1 AES-CBC), u16 compression (0 none, 1 basic), then for
// basic {u32 data_size, u32 zero_size} blocks}; IMAGE_BASE_ADDRESS (key 0x10201, inline). Encrypted data is one AES-128-CBC stream
// (IV 0) over the data parts of the blocks, its key = AES-128 decryption of aes_key with the retail XEX key. Only the 16-byte blocks
// holding the table are decrypted (CBC: plain = D(c[i]) ^ c[i-1]). The retail key is not in this repo: the build passes
// WWE_XEX_RETAIL_KEY (16 comma-separated bytes) taken from the SDK source (thirdparty\rexglue-sdk-src\src\system\xex_module.cpp);
// built without it, FromXex fails and the viewer falls back. LZX ("normal") compression is not handled (this disc's xex is basic).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace assets::stance_table {

constexpr uint32_t kImageBase = 0x82000000;  // (the image base the guest addresses assume; FromXex reads the header's own)
constexpr uint32_t kTableAddr = 0x82029A58;
constexpr size_t kMaxRows = 200;              // the end marker must come within this many rows (this disc: 103)

// Rows from `n` bytes at the table's start. Strict: first id 100, every act 0 or 29000-29022, ids ascending, the 0xFFFF end within
// kMaxRows rows; anything else = not this table (nothing is kept).
inline bool ParseRows(const uint8_t* p, size_t n, std::map<int, std::vector<int>>& rows, std::string& why) {
  rows.clear();
  int prev = -1;
  for (size_t r = 0; r < kMaxRows && (r + 1) * 8 <= n; ++r) {
    auto h = [&](int k) { return int(p[r * 8 + 2 * k]) << 8 | p[r * 8 + 2 * k + 1]; };
    if (h(0) == 0xFFFF) {
      if (r == 0) break;
      return true;
    }
    if ((r == 0 && h(0) != 100) || h(0) <= prev) break;
    prev = h(0);
    std::vector<int>& acts = rows[h(0)];
    for (int k = 1; k < 4; ++k) {
      if (h(k) == 0) continue;
      if (h(k) < 29000 || h(k) > 29022) return rows.clear(), why = "stance table: act " + std::to_string(h(k)) + " out of range", false;
      acts.push_back(h(k));
    }
  }
  rows.clear();
  why = "stance table not found (first id / order / end marker)";
  return false;
}

namespace aes {  // AES-128 decryption (FIPS-197), our own small implementation; tables built at first use
struct Tables {
  uint8_t s[256], is[256];
  bool ok = false;
  static uint8_t Xt(uint8_t a) { return uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
  static uint8_t Mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = Xt(a); b >>= 1; }
    return r;
  }
  Tables() {
    for (int i = 0; i < 256; ++i) {
      uint8_t inv = 0;
      if (i)
        for (int j = 1; j < 256; ++j)
          if (Mul(uint8_t(i), uint8_t(j)) == 1) { inv = uint8_t(j); break; }
      uint8_t x = inv, y = inv;
      for (int k = 0; k < 4; ++k) { y = uint8_t((y << 1) | (y >> 7)); x ^= y; }
      x ^= 0x63;
      s[i] = x, is[x] = uint8_t(i);
    }
    // self-test: FIPS-197 C.1 (key 00..0f, cipher 69c4e0d86a7b0430d8cdb78070b4c55a -> plain 00112233..ff)
    uint8_t key[16], ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a}, pt[16];
    for (int i = 0; i < 16; ++i) key[i] = uint8_t(i);
    uint8_t rk[176];
    Expand(key, rk);
    Decrypt(rk, ct, pt);
    ok = true;
    for (int i = 0; i < 16; ++i) ok = ok && pt[i] == uint8_t(i * 0x11);
  }
  void Expand(const uint8_t* key, uint8_t* rk) const {
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
      if (i % 16 == 0) {
        const uint8_t t0 = t[0];
        t[0] = uint8_t(s[t[1]] ^ rcon), t[1] = s[t[2]], t[2] = s[t[3]], t[3] = s[t0];
        rcon = Xt(rcon);
      }
      for (int k = 0; k < 4; ++k) rk[i + k] = uint8_t(rk[i - 16 + k] ^ t[k]);
    }
  }
  void Decrypt(const uint8_t* rk, const uint8_t* in, uint8_t* out) const {
    uint8_t st[16], tmp[16];
    for (int i = 0; i < 16; ++i) st[i] = uint8_t(in[i] ^ rk[160 + i]);
    for (int round = 9; round >= 0; --round) {
      for (int c = 0; c < 4; ++c)  // InvShiftRows + InvSubBytes (byte index = column * 4 + row)
        for (int r = 0; r < 4; ++r) tmp[c * 4 + r] = is[st[((c - r + 4) % 4) * 4 + r]];
      for (int i = 0; i < 16; ++i) st[i] = uint8_t(tmp[i] ^ rk[round * 16 + i]);
      if (round == 0) break;
      for (int c = 0; c < 4; ++c) {  // InvMixColumns
        const uint8_t a0 = st[c * 4], a1 = st[c * 4 + 1], a2 = st[c * 4 + 2], a3 = st[c * 4 + 3];
        st[c * 4 + 0] = uint8_t(Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9));
        st[c * 4 + 1] = uint8_t(Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13));
        st[c * 4 + 2] = uint8_t(Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11));
        st[c * 4 + 3] = uint8_t(Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14));
      }
    }
    std::memcpy(out, st, 16);
  }
};
inline const Tables& T() {
  static const Tables t;
  return t;
}
}  // namespace aes

// The table's bytes (kMaxRows * 8) from a whole default.xex. False + why when the file is not a supported XEX2.
inline bool FromXex(const std::vector<uint8_t>& x, std::vector<uint8_t>& out, std::string& why) {
  auto be32 = [&](size_t o) -> uint32_t { return o + 4 <= x.size() ? uint32_t(x[o]) << 24 | uint32_t(x[o + 1]) << 16 | uint32_t(x[o + 2]) << 8 | x[o + 3] : 0; };
  auto be16 = [&](size_t o) -> uint32_t { return o + 2 <= x.size() ? uint32_t(x[o]) << 8 | x[o + 1] : 0; };
  if (x.size() < 0x200 || std::memcmp(x.data(), "XEX2", 4) != 0) return why = "default.xex: not an XEX2", false;
  const uint32_t header_size = be32(8), sec = be32(0x10), nopt = be32(0x14);
  if (header_size >= x.size() || sec + 0x184 > x.size() || nopt > 64) return why = "default.xex: bad header", false;
  uint32_t ffi = 0, base = 0;
  for (uint32_t i = 0; i < nopt; ++i) {
    const uint32_t key = be32(0x18 + 8 * i), val = be32(0x18 + 8 * i + 4);
    if (key == 0x000003FF) ffi = val;
    if (key == 0x00010201) base = val;
  }
  if (!base) base = be32(sec + 0x110);
  if (!ffi || ffi + 8 > x.size()) return why = "default.xex: no file format info", false;
  const uint32_t info_size = be32(ffi), enc = be16(ffi + 4), comp = be16(ffi + 6);
  if (enc > 1 || comp > 1) return why = "default.xex: unsupported packing (encryption " + std::to_string(enc) + ", compression " + std::to_string(comp) + ")", false;
  if (kTableAddr < base) return why = "default.xex: image base above the table", false;
  const uint64_t want = kTableAddr - base, want_end = want + kMaxRows * 8;
  std::vector<uint8_t> img(kMaxRows * 8, 0);  // (zero-fill parts of the image stay 0)
  // image offset -> stream offset: the data parts lie back to back from header_size (the zero parts are not stored)
  struct Run { uint64_t img, stream, len; };
  std::vector<Run> runs;
  if (comp == 0) runs.push_back({0, 0, uint64_t(x.size() - header_size)});
  else {
    const uint32_t nblocks = info_size >= 8 ? (info_size - 8) / 8 : 0;
    uint64_t io = 0, so = 0;
    for (uint32_t b = 0; b < nblocks && b < 4096; ++b) {
      const uint32_t data = be32(ffi + 8 + 8 * b), zero = be32(ffi + 8 + 8 * b + 4);
      runs.push_back({io, so, data});
      io += uint64_t(data) + zero, so += data;
    }
  }
  uint8_t session[16];
  uint8_t rk[176];
  const auto& t = aes::T();
  if (enc == 1) {
#ifdef WWE_XEX_RETAIL_KEY
    if (!t.ok) return why = "AES self-test failed", false;
    const uint8_t retail[16] = {WWE_XEX_RETAIL_KEY};
    t.Expand(retail, rk);
    t.Decrypt(rk, &x[sec + 0x150], session);  // (CBC with IV 0 on one block = ECB)
    t.Expand(session, rk);
#else
    return why = "built without the XEX key (WWE_XEX_RETAIL_KEY)", false;
#endif
  }
  bool any = false;
  for (const Run& r : runs) {
    const uint64_t a = std::max(r.img, want), b = std::min(r.img + r.len, want_end);
    if (a >= b) continue;
    for (uint64_t blk = (r.stream + (a - r.img)) & ~uint64_t(15); blk < r.stream + (b - r.img); blk += 16) {
      const uint64_t fo = header_size + blk;
      if (fo + 16 > x.size()) return why = "default.xex: data shorter than its blocks", false;
      uint8_t plain[16];
      if (enc == 1) {
        t.Decrypt(rk, &x[fo], plain);
        for (int k = 0; k < 16; ++k) plain[k] ^= blk >= 16 ? x[fo - 16 + k] : 0;
      } else
        std::memcpy(plain, &x[fo], 16);
      for (int k = 0; k < 16; ++k) {
        const uint64_t s = blk + k;
        if (s < r.stream + (a - r.img) || s >= r.stream + (b - r.img)) continue;
        img[size_t(r.img + (s - r.stream) - want)] = plain[k];
      }
      any = true;
    }
  }
  if (!any) return why = "default.xex: the table address is not in the image data", false;
  out = std::move(img);
  return true;
}

}  // namespace assets::stance_table
