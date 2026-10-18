// wwe2k14 - native XCTD / LZX block decoder (Session 12). No SDK headers: tools/lzx/lzx_test.cpp compiles it as is.
//
// Decodes one XCTD block the way the game's chunk loop (sub_82CA4088) and LZX decoder (sub_82CA4358 / sub_82CA49C0
// and its helpers) do, from a freshly reset LZX state (sub_82CA4310: R0 = R1 = R2 = 1, tree lengths 0, first block
// header expects the E8 bit). The rules below come from the listings (Session 12, docs/systems/file_loading.md):
//   - The block is a run of chunks: big-endian u16 length, then that many bytes. Each chunk is one frame of
//     min(32768, total - decoded) output bytes and starts a new bit stream (sub_82CA4C90 at every frame), except
//     while an uncompressed block is running (then the raw bytes just continue at the new chunk's start).
//     A first length of 0 = stored block: `total` bytes follow the 2-byte zero.
//   - Bit stream: 16-bit little-endian words, most significant bit first (sub_82CA4C30 / sub_82CA4C98). The game's
//     reader may load words up to 4 bytes past a chunk's end (its end pointer is chunk + length + 4).
//   - Block header: on the very first header one E8 bit (+ 32 bits if set), then 3-bit type (1 verbatim, 2 aligned,
//     3 uncompressed; anything else is an error) and a 24-bit size read as three 8-bit fields. Aligned: 8 x 3-bit
//     aligned-tree lengths first. Verbatim / aligned: main tree (pretree + 256 lengths, pretree + 8 x slots lengths)
//     then length tree (pretree + 249), each length = (previous block's length - code + 17) mod 17; 17 = 4 + 4 bits
//     zeros, 18 = 20 + 5 bits zeros, 19 = 4 + 1 bit copies of (previous length at the run's first position - next
//     code) mod 17; runs that would pass the end are cut at the end (sub_82CA5F80). Uncompressed: the byte pointer
//     is the game's reader pointer - 2 (sub_82CA4F90: 1-16 bits of padding), 12 bytes R0 / R1 / R2 (little-endian),
//     then raw bytes; no odd-length padding byte; the next header restarts the bit stream at the byte pointer.
//   - A block's symbols must end exactly where a frame or the block ends (the game's inner decoders return the
//     overrun and sub_82CA49C0 treats non-zero as an error).
// Anything the game would treat differently, or that a valid stream never does (E8 translation on, a match before
// the block start, an undefined Huffman code, input running out), returns an error, and the caller lets the game's
// own decoder do the call.

#pragma once

#include <cstdint>
#include <cstring>

namespace wwe2k14::native_lzx {

enum Error : int {
  kOk = 0,
  kBadWindow = 1,
  kStoredShort = 2,       // stored block shorter than `total`
  kChunkMissing = 3,      // input ended (or a zero length) before `total` bytes were decoded
  kChunkOverrun = 4,      // a chunk's length runs past the input
  kE8 = 5,                // E8 translation flag set (never seen in coverage; left to the game)
  kBlockType = 6,         // block type 0 or 4-7
  kTree = 7,              // oversubscribed or over-long code lengths, or the game's reader would hit its end flag
  kSymbol = 8,            // a code that is not in the tree (or an empty tree)
  kOffset = 9,            // match before the start of the block
  kOverrun = 10,          // symbol crosses the frame / block end
  kUncompressedShort = 11,
  kHeaderInput = 12,      // uncompressed header or restart past the input
};

struct Result {
  int error = kOk;
  uint32_t produced = 0;  // bytes decoded (>= the limit when error == kOk; whole frames, at most total)
  uint32_t frames = 0;
  uint32_t blocks = 0;    // LZX block headers read
};

constexpr int kMaxSlots = 50;  // 2 MB window
constexpr int kMaxMain = 256 + 8 * kMaxSlots;
constexpr int kLenSyms = 249;

inline const uint32_t* PositionBase() {
  static const auto table = [] {
    struct T {
      uint32_t v[kMaxSlots + 1];
    } t{};
    uint32_t b = 0;
    for (int i = 0; i <= kMaxSlots; ++i) {
      t.v[i] = b;
      const int extra = i < 4 ? 0 : ((i - 2) / 2 > 17 ? 17 : (i - 2) / 2);
      b += 1u << extra;
    }
    return t;
  }();
  return table.v;
}
inline int ExtraBits(int slot) { return slot < 4 ? 0 : ((slot - 2) / 2 > 17 ? 17 : (slot - 2) / 2); }

// Number of position slots for a window, the way sub_82CA4710 counts them (slot 4 on, add 1 << extra until the base
// reaches the window size). 0 for an unsupported window.
inline int SlotsForWindow(uint32_t window) {
  if (window < (1u << 15) || window > (1u << 21) || (window & (window - 1)) != 0) return 0;
  uint32_t base = 4;
  int slot = 4;
  while (base < window) {
    base += 1u << ExtraBits(slot);
    ++slot;
  }
  return slot;
}

// Bit reader: 16-bit little-endian words, MSB first, over [base, lim) (lim = the game's end pointer, chunk end + 4;
// words at or past it read as zero). The buffer must stay readable 2 bytes past lim.
struct Bits {
  const uint8_t* base = nullptr;
  const uint8_t* p = nullptr;
  const uint8_t* lim = nullptr;
  uint64_t buf = 0;
  int cnt = 0;

  void Init(const uint8_t* start, const uint8_t* limit) {
    base = p = start;
    lim = limit;
    buf = 0;
    cnt = 0;
    Refill();
  }
  inline void Refill() {
    while (cnt <= 48) {
      uint64_t w = 0;
      if (p < lim) w = uint64_t(p[0]) | (uint64_t(p[1]) << 8);
      p += 2;
      buf |= w << (48 - cnt);
      cnt += 16;
    }
  }
  inline uint32_t Peek(int n) const { return uint32_t(buf >> (64 - n)); }
  inline void Skip(int n) {
    buf <<= n;
    cnt -= n;
  }
  inline uint32_t Read(int n) {  // n <= 32, needs cnt >= n
    if (n == 0) return 0;
    const uint32_t v = Peek(n);
    Skip(n);
    return v;
  }
  inline void Need(int n) {
    if (cnt < n) Refill();
  }
  uint64_t BitPos() const { return uint64_t(p - base) * 8 - uint64_t(cnt); }
  // The game's reader pointer after the same reads (sub_82CA4C98 keeps 17-32 bits loaded): two words past the
  // current one.
  const uint8_t* GamePtr() const { return base + 2 * (BitPos() / 16 + 2); }
  // The game's sticky end flag (byte at +11959): set once its reader wanted a word at or past lim.
  bool GameEof() const { return GamePtr() - 2 >= lim; }
};

// Canonical Huffman table, MSB-first codes up to 16 bits: a kFast-bit direct table, longer codes by length.
template <int kSyms, int kFast>
struct Huff {
  uint16_t fast[1 << kFast];  // (symbol << 5) | length; 0 = longer code or undefined
  uint16_t first[18], count[18], offs[18];
  uint16_t sorted[kSyms];
  int max_len = 0;

  // lens[i] in 0..16. false = oversubscribed or a length above 16. Empty and incomplete codes are allowed (a
  // missing code decodes as kSymbol).
  bool Build(const uint8_t* lens, int n) {
    std::memset(count, 0, sizeof(count));
    for (int i = 0; i < n; ++i) {
      if (lens[i] > 16) return false;
      ++count[lens[i]];
    }
    count[0] = 0;
    int left = 1;
    max_len = 0;
    for (int l = 1; l <= 16; ++l) {
      left <<= 1;
      left -= count[l];
      if (left < 0) return false;
      if (count[l]) max_len = l;
    }
    uint16_t code = 0, idx = 0;
    for (int l = 1; l <= 16; ++l) {
      first[l] = code;
      offs[l] = idx;
      idx = uint16_t(idx + count[l]);
      code = uint16_t((code + count[l]) << 1);
    }
    std::memset(fast, 0, sizeof(fast));
    uint16_t next[18];
    std::memcpy(next, first, sizeof(next));
    uint16_t fill[18];
    std::memcpy(fill, offs, sizeof(fill));
    for (int s = 0; s < n; ++s) {
      const int l = lens[s];
      if (!l) continue;
      const uint32_t c = next[l]++;
      sorted[fill[l]++] = uint16_t(s);
      if (l <= kFast) {
        const uint32_t start = c << (kFast - l), span = 1u << (kFast - l);
        const uint16_t e = uint16_t((s << 5) | l);
        for (uint32_t j = 0; j < span; ++j) fast[start + j] = e;
      }
    }
    return true;
  }

  // Needs br.cnt >= 16. Returns the symbol or -1.
  inline int Decode(Bits& br) const {
    const uint16_t e = fast[br.Peek(kFast)];
    if (e) {
      br.Skip(e & 31);
      return e >> 5;
    }
    const uint32_t v = br.Peek(16);
    for (int l = kFast + 1; l <= max_len; ++l) {
      const uint32_t c = v >> (16 - l);
      const uint32_t i = c - first[l];
      if (c >= first[l] && i < count[l]) {
        br.Skip(l);
        return sorted[offs[l] + i];
      }
    }
    return -1;
  }
};

struct State {
  uint8_t main_len[kMaxMain];
  uint8_t len_len[kLenSyms];
  uint8_t prev[kMaxMain];
  Huff<kMaxMain, 11> main;
  Huff<kLenSyms, 10> len;
  Huff<8, 7> aligned;
  Huff<20, 8> pre;
};

// sub_82CA5F80: pretree + `n` lengths into lens[0..n) from the previous lengths (copied first).
inline bool ReadLens(Bits& br, State& st, uint8_t* lens, int n, bool& eof) {
  std::memcpy(st.prev, lens, size_t(n));
  uint8_t pl[20];
  for (int i = 0; i < 20; ++i) {
    br.Need(4);
    pl[i] = uint8_t(br.Read(4));
  }
  if (eof || br.GameEof()) {
    eof = true;
    return false;
  }
  if (!st.pre.Build(pl, 20)) return false;
  for (int x = 0; x < n;) {
    br.Need(32);
    int z = st.pre.Decode(br);
    if (z < 0) return false;
    if (z == 17 || z == 18) {
      int run = z == 17 ? int(br.Read(4)) + 4 : int(br.Read(5)) + 20;
      if (x + run >= n) run = n - x;
      std::memset(lens + x, 0, size_t(run));
      x += run;
    } else if (z == 19) {
      int run = int(br.Read(1)) + 4;
      if (x + run >= n) run = n - x;
      br.Need(16);
      const int z2 = st.pre.Decode(br);
      if (z2 < 0) return false;
      int v = int(st.prev[x]) - z2 + 17;
      if (v >= 17) v -= 17;
      std::memset(lens + x, uint8_t(v), size_t(run));
      x += run;
    } else {
      int v = int(st.prev[x]) - z + 17;
      if (v >= 17) v -= 17;
      lens[x++] = uint8_t(v);
    }
  }
  if (br.GameEof()) {
    eof = true;
    return false;
  }
  return true;
}

// Decodes a block of `total` bytes into out[0..), frame by frame, until at least `limit` bytes are out (limit >=
// total or 0 = the whole block; a stored block is always copied whole). Like the game's chunk loop, which stops once
// the wanted bytes are out, frames past the limit are not looked at. `in` must stay readable 8 bytes past in_len
// (zeros are fine); `out` needs total + 8 bytes. `st` is scratch (about 10 KB; reuse it across calls).
inline Result DecodeBlock(const uint8_t* in, uint32_t in_len, uint32_t total, uint32_t window, uint8_t* out,
                          State& st, uint32_t limit = 0) {
  Result r;
  if (limit == 0 || limit > total) limit = total;
  const int slots = SlotsForWindow(window);
  if (!slots) {
    r.error = kBadWindow;
    return r;
  }
  if (in_len >= 2 && in[0] == 0 && in[1] == 0) {
    if (uint64_t(in_len) < 2ull + total) {
      r.error = kStoredShort;
      return r;
    }
    std::memcpy(out, in + 2, total);
    r.produced = total;
    return r;
  }
  const int main_syms = 256 + 8 * slots;
  const uint32_t* pbase = PositionBase();
  std::memset(st.main_len, 0, size_t(main_syms));
  std::memset(st.len_len, 0, sizeof(st.len_len));
  uint32_t R0 = 1, R1 = 1, R2 = 1;
  bool first_header = true, need_header = true, eof = false;
  int type = 0;
  uint32_t block_left = 0;
  const uint8_t* rp = nullptr;  // raw pointer while an uncompressed block runs
  const uint8_t* chunk_lim = nullptr;
  Bits br;

  uint32_t pos = 0, ip = 0;
  while (pos < limit) {
    if (ip + 2 > in_len) {
      r.error = kChunkMissing;
      break;
    }
    const uint32_t clen = (uint32_t(in[ip]) << 8) | in[ip + 1];
    if (clen == 0) {
      r.error = kChunkMissing;
      break;
    }
    ip += 2;
    if (clen > in_len - ip) {
      r.error = kChunkOverrun;
      break;
    }
    const uint8_t* chunk = in + ip;
    chunk_lim = chunk + clen + 4;
    ip += clen;
    ++r.frames;
    if (type == 3) {
      rp = chunk;
    } else {
      br.Init(chunk, chunk_lim);
    }
    uint32_t frame_left = total - pos < 32768u ? total - pos : 32768u;

    while (frame_left > 0) {
      if (need_header) {
        if (first_header) {
          first_header = false;
          br.Need(1);
          if (br.Read(1)) {
            r.error = kE8;
            return r;
          }
        }
        if (type == 3) {
          type = 0;
          if (rp + 4 > chunk_lim) {
            r.error = kHeaderInput;
            return r;
          }
          br.Init(rp, chunk_lim);
        }
        br.Need(27);
        type = int(br.Read(3));
        block_left = br.Read(8) << 16;
        block_left |= br.Read(8) << 8;
        block_left |= br.Read(8);
        ++r.blocks;
        if (type == 2) {
          uint8_t al[8];
          for (int i = 0; i < 8; ++i) {
            br.Need(3);
            al[i] = uint8_t(br.Read(3));
          }
          if (!st.aligned.Build(al, 8)) {
            r.error = kTree;
            return r;
          }
        }
        if (type == 1 || type == 2) {
          if (!ReadLens(br, st, st.main_len, 256, eof) ||
              !ReadLens(br, st, st.main_len + 256, 8 * slots, eof) || !st.main.Build(st.main_len, main_syms) ||
              !ReadLens(br, st, st.len_len, kLenSyms, eof) || !st.len.Build(st.len_len, kLenSyms)) {
            r.error = kTree;
            return r;
          }
        } else if (type == 3) {
          const uint8_t* q = br.GamePtr() - 2;
          if (q + 4 >= chunk_lim || q + 12 > in + in_len) {
            r.error = kHeaderInput;
            return r;
          }
          R0 = uint32_t(q[0]) | (uint32_t(q[1]) << 8) | (uint32_t(q[2]) << 16) | (uint32_t(q[3]) << 24);
          R1 = uint32_t(q[4]) | (uint32_t(q[5]) << 8) | (uint32_t(q[6]) << 16) | (uint32_t(q[7]) << 24);
          R2 = uint32_t(q[8]) | (uint32_t(q[9]) << 8) | (uint32_t(q[10]) << 16) | (uint32_t(q[11]) << 24);
          rp = q + 12;
        } else {
          r.error = kBlockType;
          return r;
        }
        need_header = false;
      }

      while (block_left > 0 && frame_left > 0) {
        const uint32_t n = block_left < frame_left ? block_left : frame_left;
        if (type == 3) {
          if (rp + n > chunk_lim || rp + n > in + in_len) {
            r.error = kUncompressedShort;
            return r;
          }
          std::memcpy(out + pos, rp, n);
          rp += n;
          pos += n;
        } else {
          uint8_t* o = out + pos;
          uint8_t* const oend = o + n;
          const bool aligned = type == 2;
          while (o < oend) {
            br.Need(48);
            const int sym = st.main.Decode(br);
            if (sym < 256) {
              if (sym < 0) {
                r.error = kSymbol;
                return r;
              }
              *o++ = uint8_t(sym);
              continue;
            }
            const int m = sym - 256;
            uint32_t mlen = uint32_t(m & 7) + 2;
            if ((m & 7) == 7) {
              const int ls = st.len.Decode(br);
              if (ls < 0) {
                r.error = kSymbol;
                return r;
              }
              mlen += uint32_t(ls);
            }
            const int slot = m >> 3;
            uint32_t off;
            if (slot == 0) {
              off = R0;
            } else if (slot == 1) {
              off = R1;
              R1 = R0;
              R0 = off;
            } else if (slot == 2) {
              off = R2;
              R2 = R0;
              R0 = off;
            } else {
              const int extra = ExtraBits(slot);
              br.Need(32);
              if (aligned && extra >= 3) {
                uint32_t v = br.Read(extra - 3) << 3;
                br.Need(16);
                const int a = st.aligned.Decode(br);
                if (a < 0) {
                  r.error = kSymbol;
                  return r;
                }
                v += uint32_t(a);
                off = pbase[slot] + v - 2;
              } else {
                off = pbase[slot] + br.Read(extra) - 2;
              }
              R2 = R1;
              R1 = R0;
              R0 = off;
            }
            if (mlen > uint32_t(oend - o)) {
              r.error = kOverrun;
              return r;
            }
            if (off > uint32_t(o - out) || off == 0) {
              r.error = kOffset;
              return r;
            }
            const uint8_t* s = o - off;
            if (off >= 8) {
              // 8-byte steps; may write up to 7 bytes past the match (the caller's buffer has slack).
              uint8_t* d = o;
              uint8_t* const de = o + mlen;
              while (d < de) {
                uint64_t w;
                std::memcpy(&w, s, 8);
                std::memcpy(d, &w, 8);
                d += 8;
                s += 8;
              }
            } else {
              for (uint32_t i = 0; i < mlen; ++i) o[i] = s[i];
            }
            o += mlen;
          }
          pos += n;
        }
        block_left -= n;
        frame_left -= n;
      }
      if (block_left == 0) need_header = true;
    }
  }
  r.produced = pos;
  if (!r.error && pos < limit) r.error = kChunkMissing;
  return r;
}

}  // namespace wwe2k14::native_lzx
