// Session 98 (Superstar Importer core): a small zlib compressor (RFC 1950 wrapper around RFC 1951 deflate: LZ77 with hash
// chains, fixed Huffman codes). Our own code, std only. Used for the string pac's name tables (ZLIB leaves). The output is a
// standard zlib stream (any inflater reads it; the importer's self test reads it back with char_mods::Inflate).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace importer::deflate {

namespace detail {
struct BitOut {
  std::vector<uint8_t>& out;
  uint32_t acc = 0;
  int n = 0;
  void Put(uint32_t bits, int count) {  // LSB first
    acc |= bits << n;
    n += count;
    while (n >= 8) out.push_back(uint8_t(acc)), acc >>= 8, n -= 8;
  }
  void PutRev(uint32_t code, int len) {  // Huffman codes go MSB first
    uint32_t r = 0;
    for (int i = 0; i < len; ++i) r |= ((code >> i) & 1u) << (len - 1 - i);
    Put(r, len);
  }
  void Flush() {
    if (n > 0) out.push_back(uint8_t(acc)), acc = 0, n = 0;
  }
};
inline void Lit(BitOut& b, int sym) {  // fixed literal / length code
  if (sym < 144) b.PutRev(0x30 + sym, 8);
  else if (sym < 256) b.PutRev(0x190 + (sym - 144), 9);
  else if (sym < 280) b.PutRev(sym - 256, 7);
  else b.PutRev(0xC0 + (sym - 280), 8);
}
inline void Match(BitOut& b, int len, int dist) {
  static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  int li = 28;
  while (lbase[li] > len) --li;
  Lit(b, 257 + li);
  if (lext[li]) b.Put(uint32_t(len - lbase[li]), lext[li]);
  int di = 29;
  while (dbase[di] > dist) --di;
  b.PutRev(uint32_t(di), 5);
  if (dext[di]) b.Put(uint32_t(dist - dbase[di]), dext[di]);
}
}  // namespace detail

inline std::vector<uint8_t> ZlibCompress(const std::vector<uint8_t>& in) {
  using namespace detail;
  std::vector<uint8_t> out = {0x78, 0x9C};
  BitOut b{out};
  b.Put(1, 1);  // BFINAL: one block
  b.Put(1, 2);  // BTYPE 01: fixed Huffman
  constexpr int kWin = 32768, kHashBits = 15, kMaxChain = 64;
  std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(in.size(), -1);
  auto hash = [&](size_t i) { return (uint32_t(in[i]) << 10 ^ uint32_t(in[i + 1]) << 5 ^ in[i + 2]) & ((1u << kHashBits) - 1); };
  auto insert = [&](size_t i) {
    if (i + 2 >= in.size()) return;
    const uint32_t h = hash(i);
    prev[i] = head[h], head[h] = int32_t(i);
  };
  size_t i = 0;
  while (i < in.size()) {
    int best = 0, best_d = 0;
    if (i + 2 < in.size()) {
      const uint32_t h = hash(i);
      int32_t c = head[h];
      for (int chain = 0; c >= 0 && chain < kMaxChain && int(i) - c <= kWin; ++chain, c = prev[size_t(c)]) {
        int l = 0;
        const int lim = int(std::min<size_t>(258, in.size() - i));
        while (l < lim && in[size_t(c) + size_t(l)] == in[i + size_t(l)]) ++l;
        if (l > best) best = l, best_d = int(i) - c;
        if (l == 258) break;
      }
    }
    if (best >= 3) {
      Match(b, best, best_d);
      for (int k = 0; k < best; ++k) insert(i + size_t(k));
      i += size_t(best);
    } else {
      Lit(b, in[i]);
      insert(i);
      ++i;
    }
  }
  Lit(b, 256);  // end of block
  b.Flush();
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t c : in) s1 = (s1 + c) % 65521, s2 = (s2 + s1) % 65521;
  const uint32_t ad = s2 << 16 | s1;
  for (int k = 3; k >= 0; --k) out.push_back(uint8_t(ad >> (8 * k)));
  return out;
}

}  // namespace importer::deflate
