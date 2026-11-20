// S103 CMB (docs\research\s103\CMP.md CMP / CMP2 / adversary; docs\systems\attire_build.md): composite attires on the HOST, once per load.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game composes a "composite" attire (30 Years legends, Heenan; Jericho a1 a2, Hogan a2 a4 a5, CM Punk a1, Elizabeth a1-a3, Macho Man a1 a2 ...) on the
// GPU at the character load: the atlas starts as the PACH's base (entry 12, DXT1) and each mask entry 13 + k (k = 0..3, "Mask1".."Mask4") runs the
// yComposition channel-split pass: per texel the FIRST NON-ZERO of the mask's R, G, B, A channels picks g_CompR..A = colour 4k + c of the 16 RGB byte
// triples, RGB = colour x channel value (0..top, top 15 for A4R4G4B4), alpha untouched (sub_83418580 / sub_83419CC8; CMP2 section 3, adversary 3 / 5).
// The new renderer's OFF build never executes those draws, so it composes the same thing from the same inputs here: the base + masks from the player's
// own PACH, the 48 colour bytes the game's CPU code chose (read at the model link, model_hooks.h ModelLinkRead).
//
//   * Compose(): pure; plain DDS blobs + 48 bytes in, RGBA8 + mips out. k = entry - 13 (NEVER the n-th present entry: the game binds Mask1..4 by fixed
//     index; attire_build.py's enumerate() shifted k when an entry was missing). Channel masks come from each mask's DDS header (Heenan's: R 0x0F00, G
//     0x00F0, B 0x000F, A 0xF000). A colour of 255 paints value x white (S105 A5dW [C]: sub_83418580 +150..+1E0 sends u8 / 255 as g_CompR..A; shader
//     FB40689827987460 writes (mask value x colour, 1) on the first non-zero channel, blend SrcAlpha / InvSrcAlpha: no colour test anywhere). `White::kKeep` is a
//     test-only alternative (Options::white, the check tool; no player switch) that is NOT the game's rule.
//   * ReadComposite(): the hook's pure half (template over the guest word reader, so the host test drives it with a byte map): desc = [reader+96],
//     mode = u32 [desc+44], set = s32 [desc+48], chosen = [desc+92], loaded = [desc+28]; mode 0 or 2 = the base as is (nothing composed); else
//     P = [reader+100], block = [P+16], 0 <= set <= 15, 48 bytes at block + 132 set + 1320 (index 4k + c, RGB). Every refusal is counted.
//   * Tag: the link's result is an entry-string tag "/c:<96 hex>" (or "/r" = refused: the loader fails it, a hole) stored inside the body's BodyLink (SubmitModelLink) and appended to
//     "model_key:<loaded>" (the model cache key + GameFilesSource::Load's input); sscanf("model_key:%d") still parses a suffixed entry. No second table.
//   Colour freshness [U]: the block is read at the link, the game re-reads it at each build step frames later; a later setter would make this copy stale
//   (the remedy is a hook at sub_83418580 entry: composite+140 = desc, needs codegen). NOT built.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace attire_compose {

constexpr int kColourBytes = 48;      // 16 RGB triples: 4 mask steps x R, G, B, A
constexpr uint32_t kBaseEntry = 12;   // the PACH's base texture ("color")
constexpr uint32_t kFirstMaskEntry = 13;
constexpr int kMasks = 4;             // entries 13..16 = k 0..3

enum class White { kPaint /* the game (S105 A5dW) */, kKeep /* test only */ };

// Planted faults for the offline proof (tools\assets\attire_compose_check.*); never set in the game.
enum class Fault {
  kNone, kSwapRB, kRotl4, kEnumerate, kWhiteKeepDefault, kComposeMode0, kComposeMode2,
};

struct Options {
  White white = White::kPaint;
  Fault fault = Fault::kNone;
  bool bc1 = true;   // BC1 output (see Image); false = RGBA8
  // TEST ONLY (0 = the mask's own DDS header): channel bit masks for R, G, B, A. The captured replay (out\captures\s100_heenan_build) draws the file's masks
  // with the nibbles rotated (R 0xF000, G 0x000F, B 0x00F0, A 0x0F00); the proof composes with them to compare with it. Never set in the game.
  uint32_t test_masks[4] = {0, 0, 0, 0};
};

struct Input {
  const uint8_t* base = nullptr;                 // a DDS file (or a blob holding one in its first 0x100 bytes: FindDds), DXT1
  size_t base_n = 0;
  const uint8_t* mask[kMasks] = {};              // mask[k] = entry 13 + k, null when the PACH has no such entry
  size_t mask_n[kMasks] = {};
  uint8_t colours[kColourBytes] = {};
};

// The output. bc1 (Options::bc1, the default): `data` = BC1 (DXT1) blocks, level 0 first then each mip (w >> m, h >> m), 8 bytes per 4x4 block, opaque 4-colour mode:
// 1.4 MB for Heenan's 2048x1024 + 4 mips (the game itself packs its atlas to DXT1, sub_82CFD8F0), upload as BC1_RGBA_UNORM (never SRGB). Else RGBA8 (10.7 MB for
// Heenan: the proof's and a no-BC device's form), upload R8G8B8A8_UNORM (never SRGB).
struct Image {
  uint32_t w = 0, h = 0, mips = 0;
  bool bc1 = false;
  uint32_t alpha0 = 0;                           // texels of the base with alpha 0 (BC1 3-colour mode): the census says it per load; the file census found 0 in the 14 built attires
  std::vector<uint8_t> data;                     // see above; rows top to bottom
  size_t LevelOffset(uint32_t m) const {
    size_t o = 0;
    for (uint32_t i = 0; i < m; ++i) {
      const size_t lw = std::max(w >> i, 1u), lh = std::max(h >> i, 1u);
      o += bc1 ? ((lw + 3) / 4) * ((lh + 3) / 4) * 8 : lw * lh * 4;
    }
    return o;
  }
};

// Decodes one BC1 level to RGBA8 (the proof reads the encoder back with it).
inline bool DecodeBc1Level(const uint8_t* data, size_t n, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba);

// The DDS file inside a (possibly wrapped) blob: the first "DDS " within the first 0x100 bytes. Null when there is none.
inline const uint8_t* FindDds(const uint8_t* p, size_t n, size_t* len);

// Composes one attire. False + why when an input is not understood (base not DXT1, mask not 16-bit / wrong size).
inline bool Compose(const Input& in, const Options& opt, Image& out, std::string& why);

// FNV-1a 64 of the 48 colour bytes (the census / the cache tag).
// (inline: model_hooks.h reaches it through LinkTag, and tools that include model_hooks.h do not link attire_compose.cpp)
inline uint64_t HashColours(const uint8_t* colours) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < kColourBytes; ++i) h = (h ^ colours[i]) * 1099511628211ull;
  return h;
}

// The entry-string tag: "/c:<96 hex>" (the colours alone decide the picture), and its parse (false when `entry` carries no tag).
inline std::string EntryTag(const uint8_t* colours);
inline bool IsRefusalTag(const std::string& entry);
inline bool ParseEntryTag(const std::string& entry, uint8_t* colours);
// The refusal tag (stream-free rule: a composite attire whose inputs the hook could not read is a HOLE, never the plain base): appended instead of a colour tag; the loader
// fails the load, so the record names nothing and the game's own geometry draws it (DLW's named-failure route). `chosen` is not in any tag: Compose reads only the 48 colours.
inline const char* RefusalTag() { return "/r"; }

// ---- the link read (pure; the hook's half) ----------------------------------------------------------------------------------------------------------

enum class Read {
  kComposite,   // a composite attire: out is filled
  kBase,        // mode 0 or 2: the base as is (right in OFF, not an error)
  kUnreadable,  // a guest read failed
  kNoDesc,      // [reader+96] == 0
  kBadP,        // [reader+100] < 0x10000
  kBadBlock,    // [P+16] < 0x10000
  kBadSet,      // set < 0 or > 15
};
constexpr int kReadKinds = 7;
inline const char* ReadName(Read r) {
  switch (r) {
    case Read::kComposite: return "composed";
    case Read::kBase: return "base (mode 0 / 2)";
    case Read::kUnreadable: return "refused: guest read failed";
    case Read::kNoDesc: return "refused: no descriptor";
    case Read::kBadP: return "refused: load descriptor P not a pointer";
    case Read::kBadBlock: return "refused: colour block not a pointer";
    case Read::kBadSet: return "refused: colour set out of 0..15";
  }
  return "?";
}

struct Link {
  uint32_t loaded = 0, chosen = 0, mode = 0;
  int32_t set = 0;
  uint8_t colours[kColourBytes] = {};
  uint64_t hash = 0;
};

// `rw(addr, uint32_t&) -> bool` reads one big-endian guest word (the game's ReadWord). `fault` only for the offline proof.
template <class ReadWordFn>
inline Read ReadComposite(ReadWordFn rw, uint32_t reader, Link& out, Fault fault = Fault::kNone) {
  uint32_t desc = 0;
  if (!rw(reader + 96, desc)) return Read::kUnreadable;
  if (desc == 0) return Read::kNoDesc;
  uint32_t mode = 0, setw = 0, chosen = 0, loaded = 0;
  if (!rw(desc + 44, mode) || !rw(desc + 48, setw) || !rw(desc + 92, chosen) || !rw(desc + 28, loaded)) return Read::kUnreadable;
  const bool base_mode = (mode == 0 && fault != Fault::kComposeMode0) || (mode == 2 && fault != Fault::kComposeMode2);
  if (base_mode) {   // S103 ARM: the fields stay readable on a base read (the census prints mode 0 and mode 2 apart, never folded)
    out.loaded = loaded, out.chosen = chosen, out.mode = mode, out.set = int32_t(setw);
    return Read::kBase;
  }
  uint32_t P = 0, block = 0;
  if (!rw(reader + 100, P)) return Read::kUnreadable;
  if (P < 0x10000u) return Read::kBadP;
  if (!rw(P + 16, block)) return Read::kUnreadable;
  if (block < 0x10000u) return Read::kBadBlock;
  const int32_t set = int32_t(setw);
  if (set < 0 || set > 15) return Read::kBadSet;
  const uint64_t at = uint64_t(block) + 132u * uint64_t(set) + 1320u;
  if (at + kColourBytes > 0xFFFFFFFFull) return Read::kBadBlock;
  for (int i = 0; i < kColourBytes / 4; ++i) {
    uint32_t w = 0;
    if (!rw(uint32_t(at) + 4u * uint32_t(i), w)) return Read::kUnreadable;
    for (int b = 0; b < 4; ++b) out.colours[4 * i + b] = uint8_t(w >> (24 - 8 * b));   // big-endian word -> bytes in memory order
  }
  out.loaded = loaded, out.chosen = chosen, out.mode = mode, out.set = set, out.hash = HashColours(out.colours);
  return Read::kComposite;
}

// ---- counters and the link's census ---------------------------------------------------------------------------------------------------------------

struct Counters {
  std::atomic<uint64_t> n[kReadKinds] = {};
  std::atomic<uint64_t> base_mode0{0}, base_mode2{0};   // S103 ARM: a base read (kBase) by its mode, never folded
  std::atomic<uint64_t> diag{0}, step{0};               // S103 ARM: diagnosis lines said (DiagLine / BuildStepLine caps)
};
inline Counters& Count() {
  static Counters c;
  return c;
}
inline std::string CountersLine() {
  std::string s = "[attire_compose] links:";
  for (int i = 0; i < kReadKinds; ++i) {
    s += ' ';
    s += ReadName(Read(i));
    s += ' ';
    s += std::to_string(Count().n[i].load(std::memory_order_relaxed));
    s += ';';
  }
  s += " of the base reads: mode 0 " + std::to_string(Count().base_mode0.load(std::memory_order_relaxed)) + ", mode 2 " + std::to_string(Count().base_mode2.load(std::memory_order_relaxed)) + ';';
  return s;
}

// S103 ARM (HNN "Open: mode 0/2 at the link vs the game composing", the exact missing evidence): at the model link, for a remapped / suspect attire (desc+28 != desc+92) or Heenan's 24912, the words
// the colour decision rests on: desc+28 / +44 / +48 / +92 and P = [reader+100]: P+40 (mode), P+44 (s8 colour set), P+60 / P+64 (the alternative keys, 0xC800 = none). Read only; capped.
// A second read of P+40 / P+64 at the first build step: BuildStepLine (the hook on sub_83418580's entry, r3 = the composite: P = [r3+100], desc = [r3+140]).
template <class ReadWordFn>
inline bool DiagLine(ReadWordFn rw, uint32_t reader, std::string& line) {
  line.clear();
  uint32_t desc = 0, d28 = 0, d44 = 0, d48 = 0, d92 = 0, P = 0;
  if (!rw(reader + 96, desc) || desc == 0 || !rw(desc + 28, d28) || !rw(desc + 44, d44) || !rw(desc + 48, d48) || !rw(desc + 92, d92)) return false;
  if (!(d28 != d92 || d92 == 24912u)) return false;
  if (Count().diag.fetch_add(1, std::memory_order_relaxed) >= 48) return false;
  uint32_t p40 = 0, p44 = 0, p60 = 0, p64 = 0;
  bool have = rw(reader + 100, P) && P >= 0x10000u && rw(P + 40, p40) && rw(P + 44, p44) && rw(P + 60, p60) && rw(P + 64, p64);
  char b[320];
  if (have) std::snprintf(b, sizeof b, "[attire_compose] diag chosen %u: desc+28 %u +44 %u +48 %d +92 %u; P+40 %u P+44 %d P+60 0x%X P+64 0x%X", d92, d28, d44, int32_t(d48), d92, p40, int(int8_t(p44 >> 24)), p60, p64);
  else std::snprintf(b, sizeof b, "[attire_compose] diag chosen %u: desc+28 %u +44 %u +48 %d +92 %u; P unreadable (reader+100 = 0x%X)", d92, d28, d44, int32_t(d48), d92, P);
  line = b;
  return true;
}

template <class ReadWordFn>
inline bool BuildStepLine(ReadWordFn rw, uint32_t composite, uint32_t step, std::string& line) {
  line.clear();
  if (step != 0) return false;   // the first build step only
  uint32_t P = 0, desc = 0, p40 = 0, p44 = 0, p60 = 0, p64 = 0, d92 = 0, d28 = 0;
  if (!rw(composite + 100, P) || P < 0x10000u || !rw(composite + 140, desc) || desc < 0x10000u) return false;
  if (Count().step.load(std::memory_order_relaxed) >= 32) return false;
  if (!rw(P + 40, p40) || !rw(P + 44, p44) || !rw(P + 60, p60) || !rw(P + 64, p64) || !rw(desc + 92, d92) || !rw(desc + 28, d28)) return false;
  Count().step.fetch_add(1, std::memory_order_relaxed);   // review F5: counted against the cap only after a successful read
  char b[256];
  std::snprintf(b, sizeof b, "[attire_compose] build step 0: desc+92 %u desc+28 %u; P+40 %u P+44 %d P+60 0x%X P+64 0x%X", d92, d28, p40, int(int8_t(p44 >> 24)), p60, p64);
  line = b;
  return true;
}

// The hook's whole body, after the contributor's own link read (model_hooks.h ModelLinkRead): reads, counts, and returns the entry tag
// ("/a:<chosen>/c:<hex>", empty = the plain base) that the contributor stores INSIDE the body's link (WrestlerContributor::BodyLink via SubmitModelLink), so every
// path that drops the link (node-count change, another character's slot key, LRU, a relink at the address) drops the colours with it: there is no second table.
// `line` gets the census text (empty for a base read past the first kBaseLogs). Never writes guest memory.
template <class ReadWordFn>
inline std::string LinkTag(ReadWordFn rw, uint32_t reader, std::string& line, Fault fault = Fault::kNone, Read* verdict = nullptr, Link* link_out = nullptr) {
  Link l;
  const Read r = ReadComposite(rw, reader, l, fault);
  if (verdict) *verdict = r;   // S103 ARM2: ModelLinkRead forgets the build-step capture of a chosen key whose load did not build (a base read: mode 0 / 2)
  if (link_out) *link_out = l;
  const uint64_t seen = Count().n[int(r)].fetch_add(1, std::memory_order_relaxed);
  line.clear();
  constexpr uint64_t kBaseLogs = 24;
  if (!(r == Read::kBase && seen >= kBaseLogs)) {
    char b[256];
    if (r == Read::kComposite) std::snprintf(b, sizeof b, "[attire_compose] key %u chosen %u set %d: composed (mode %u, colours %016llX)", l.loaded, l.chosen, l.set, l.mode, static_cast<unsigned long long>(l.hash));
    else if (r == Read::kBase) std::snprintf(b, sizeof b, "[attire_compose] link: base (mode %u): key %u chosen %u", l.mode, l.loaded, l.chosen);   // S103 ARM: mode 0 and mode 2 apart, never folded
    else std::snprintf(b, sizeof b, "[attire_compose] link: %s", ReadName(r));
    line = b;
  }
  if (r == Read::kBase) (l.mode == 2 ? Count().base_mode2 : Count().base_mode0).fetch_add(1, std::memory_order_relaxed);
  if (r == Read::kBase || r == Read::kComposite) {   // S103 ARM: the open question's evidence (several lines in one string, split by the hook)
    std::string d;
    if (DiagLine(rw, reader, d)) line += (line.empty() ? "" : "\n") + d;
  }
  if (r == Read::kComposite) return EntryTag(l.colours);
  if (r == Read::kBase) return std::string();
  return RefusalTag();   // unreadable / no descriptor / bad P / bad block / bad set: the loader fails it, the attire is a hole
}


// ---- the implementation (header-only: model_hooks.h and live_assets.cpp are compiled by many tool builds that list sources by hand) ----

namespace detail {

struct DdsView {
  uint32_t w = 0, h = 0, mips = 1, fourcc = 0, bits = 0, cm[4] = {};
  const uint8_t* data = nullptr;
  size_t n = 0;
};

inline uint32_t Rd32(const uint8_t* p, size_t o) {
  uint32_t v;
  std::memcpy(&v, p + o, 4);
  return v;
}

inline bool View(const uint8_t* blob, size_t n, DdsView& v, std::string& why) {
  size_t len = 0;
  const uint8_t* d = blob ? FindDds(blob, n, &len) : nullptr;
  if (!d || len < 128) return why = "no DDS header", false;
  v.h = Rd32(d, 12), v.w = Rd32(d, 16);
  v.mips = (Rd32(d, 8) & 0x20000u) ? std::max(Rd32(d, 28), 1u) : 1u;
  v.fourcc = Rd32(d, 84), v.bits = Rd32(d, 88);
  for (int i = 0; i < 4; ++i) v.cm[i] = Rd32(d, 92 + 4 * size_t(i));
  v.data = d + 128, v.n = len - 128;
  if (v.w == 0 || v.h == 0 || v.w > 16384 || v.h > 16384) return why = "bad DDS size", false;
  return true;
}

inline uint8_t Div255(uint32_t v) { return uint8_t(v > 255 ? 255 : v); }

// RGBA8 of a DXT1 level 0 (BC1: the 3-colour mode's index 3 is transparent black).
inline bool DecodeBc1(const DdsView& v, std::vector<uint8_t>& rgba, std::string& why) {
  const uint32_t bw = (v.w + 3) / 4, bh = (v.h + 3) / 4;
  if (v.n < size_t(bw) * bh * 8) return why = "base shorter than its first level", false;
  rgba.assign(size_t(v.w) * v.h * 4, 0);
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* b = v.data + (size_t(by) * bw + bx) * 8;
      const uint32_t c0 = b[0] | (uint32_t(b[1]) << 8), c1 = b[2] | (uint32_t(b[3]) << 8);
      uint32_t idx = b[4] | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
      int p[4][4];
      auto expand = [](uint32_t c, int* o) {
        o[0] = int(((c >> 11) & 31) * 255 + 15) / 31;
        o[1] = int(((c >> 5) & 63) * 255 + 31) / 63;
        o[2] = int((c & 31) * 255 + 15) / 31;
        o[3] = 255;
      };
      expand(c0, p[0]);
      expand(c1, p[1]);
      if (c0 > c1) {
        for (int c = 0; c < 3; ++c) p[2][c] = (2 * p[0][c] + p[1][c] + 1) / 3, p[3][c] = (p[0][c] + 2 * p[1][c] + 1) / 3;
        p[2][3] = p[3][3] = 255;
      } else {
        for (int c = 0; c < 3; ++c) p[2][c] = (p[0][c] + p[1][c] + 1) / 2, p[3][c] = 0;
        p[2][3] = 255, p[3][3] = 0;
      }
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t px = bx * 4 + x, py = by * 4 + y;
          if (px >= v.w || py >= v.h) continue;
          const int s = int((idx >> (2 * (4 * y + x))) & 3);
          uint8_t* o = &rgba[(size_t(py) * v.w + px) * 4];
          for (int c = 0; c < 4; ++c) o[c] = uint8_t(p[s][c]);
        }
    }
  return true;
}

inline uint16_t Pack565(int r, int g, int b) { return uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }

// One level of RGBA8 -> BC1, opaque 4-colour mode (c0 > c1): per block the endpoints are the bounding box of its texels' RGB, pulled in by 1/16 of the span
// (the usual cheap fit), indices = nearest of the 4 palette colours. A flat block gets c0 == c1 (index 0). Alpha is dropped (the composite is opaque
// where the build paints; BC1's own alpha-0 texels in the base are the census's question, S103 CMB B iii).
inline void EncodeBc1Level(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out) {
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      int px[16][3], lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);   // (a level under 4 wide / high repeats its edge)
          const uint8_t* s = rgba + (size_t(sy) * w + sx) * 4;
          for (int c = 0; c < 3; ++c) {
            px[4 * y + x][c] = s[c];
            lo[c] = std::min(lo[c], int(s[c])), hi[c] = std::max(hi[c], int(s[c]));
          }
        }
      for (int c = 0; c < 3; ++c) {
        const int inset = (hi[c] - lo[c]) / 16;
        lo[c] += inset, hi[c] -= inset;
      }
      uint16_t c0 = Pack565(hi[0], hi[1], hi[2]), c1 = Pack565(lo[0], lo[1], lo[2]);
      uint32_t idx = 0;
      uint8_t* o = out + (size_t(by) * bw + bx) * 8;
      if (c0 != c1) {
        if (c0 < c1) std::swap(c0, c1);
        int pal[4][3];
        auto exp = [](uint16_t c, int* p) { p[0] = ((c >> 11) & 31) * 255 / 31, p[1] = ((c >> 5) & 63) * 255 / 63, p[2] = (c & 31) * 255 / 31; };
        exp(c0, pal[0]);
        exp(c1, pal[1]);
        for (int c = 0; c < 3; ++c) pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3, pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
        for (int i = 0; i < 16; ++i) {
          int best = 0, bd = 1 << 30;
          for (int p = 0; p < 4; ++p) {
            const int d = (px[i][0] - pal[p][0]) * (px[i][0] - pal[p][0]) + (px[i][1] - pal[p][1]) * (px[i][1] - pal[p][1]) + (px[i][2] - pal[p][2]) * (px[i][2] - pal[p][2]);
            if (d < bd) bd = d, best = p;
          }
          idx |= uint32_t(best) << (2 * i);
        }
      }
      o[0] = uint8_t(c0), o[1] = uint8_t(c0 >> 8), o[2] = uint8_t(c1), o[3] = uint8_t(c1 >> 8);
      for (int i = 0; i < 4; ++i) o[4 + i] = uint8_t(idx >> (8 * i));
    }
}

}  // namespace detail

inline const uint8_t* FindDds(const uint8_t* p, size_t n, size_t* len) {
  const size_t lim = std::min<size_t>(n, 0x100 + 4);
  for (size_t i = 0; i + 4 <= lim; ++i)
    if (std::memcmp(p + i, "DDS ", 4) == 0) {
      if (len) *len = n - i;
      return p + i;
    }
  return nullptr;
}

inline bool Compose(const Input& in, const Options& opt, Image& out, std::string& why) {
  detail::DdsView base;
  if (!detail::View(in.base, in.base_n, base, why)) return why = "base: " + why, false;
  if (base.fourcc != 0x31545844u) return why = "base is not DXT1", false;
  std::vector<uint8_t> px;
  if (!detail::DecodeBc1(base, px, why)) return false;
  const White white = opt.fault == Fault::kWhiteKeepDefault ? White::kKeep : opt.white;

  int present_ord = 0;   // kEnumerate (planted fault): k = the n-th PRESENT mask
  for (int ent = 0; ent < kMasks; ++ent) {
    if (!in.mask[ent]) continue;
    const int k = opt.fault == Fault::kEnumerate ? present_ord : ent;
    ++present_ord;
    detail::DdsView m;
    std::string mwhy;
    if (!detail::View(in.mask[ent], in.mask_n[ent], m, mwhy)) return why = "mask " + std::to_string(13 + ent) + ": " + mwhy, false;
    if (m.w != base.w || m.h != base.h || m.bits != 16 || m.fourcc != 0) return why = "mask " + std::to_string(13 + ent) + " is not a 16-bit " + std::to_string(base.w) + "x" + std::to_string(base.h) + " DDS", false;
    if (m.n < size_t(m.w) * m.h * 2) return why = "mask " + std::to_string(13 + ent) + " shorter than its first level", false;
    // channel masks from the DDS header: R, G, B, A in the order the shader tests them
    uint32_t cm[4] = {m.cm[0], m.cm[1], m.cm[2], m.cm[3]};
    if (opt.test_masks[0] | opt.test_masks[1] | opt.test_masks[2] | opt.test_masks[3])
      for (int c = 0; c < 4; ++c) cm[c] = opt.test_masks[c];
    if (opt.fault == Fault::kSwapRB) std::swap(cm[0], cm[2]);
    uint32_t shift[4], top[4];
    for (int c = 0; c < 4; ++c) {
      shift[c] = cm[c] ? uint32_t(__builtin_ctz(cm[c])) : 0;
      top[c] = cm[c] ? (cm[c] >> shift[c]) : 1;
    }
    bool keep[4];
    for (int c = 0; c < 4; ++c) {
      const uint8_t* col = in.colours + 3 * (4 * k + c);
      keep[c] = white == White::kKeep && col[0] == 255 && col[1] == 255 && col[2] == 255;
    }
    const size_t count = size_t(m.w) * m.h;
    for (size_t i = 0; i < count; ++i) {
      uint32_t v = uint32_t(m.data[2 * i]) | (uint32_t(m.data[2 * i + 1]) << 8);
      if (opt.fault == Fault::kRotl4) v = ((v << 4) | (v >> 12)) & 0xFFFFu;
      for (int c = 0; c < 4; ++c) {
        const uint32_t raw = cm[c] ? ((v & cm[c]) >> shift[c]) : 0;
        if (!raw) continue;
        if (!keep[c] && 3 * (4 * k + c) + 2 < kColourBytes) {   // (k is 0..3 here; the bound only guards a fault-shifted k)
          const uint8_t* col = in.colours + 3 * (4 * k + c);
          uint8_t* o = &px[i * 4];
          for (int ch = 0; ch < 3; ++ch) o[ch] = detail::Div255((uint32_t(col[ch]) * raw * 2 + top[c]) / (2 * top[c]));   // colour x value / top, rounded
        }
        break;   // the channel claims the texel (a kept 255 colour leaves it as it was)
      }
    }
  }

  // RGBA8 with mips: the level count of the base's DDS; each level a 2x2 box of the one above (the game composes each level of its atlas separately; a
  // box filter of the finished level 0 is the same picture to within the filter).
  out.w = base.w, out.h = base.h;
  out.alpha0 = 0;
  for (size_t i = 3; i < px.size(); i += 4) out.alpha0 += px[i] == 0;
  out.mips = std::max(1u, std::min(base.mips, 1u + uint32_t(31 - __builtin_clz(std::max(base.w, base.h)))));
  std::vector<uint8_t> chain;
  size_t total = 0;
  for (uint32_t l = 0; l < out.mips; ++l) total += size_t(std::max(out.w >> l, 1u)) * std::max(out.h >> l, 1u) * 4;
  chain.assign(total, 0);
  std::memcpy(chain.data(), px.data(), px.size());
  size_t src = 0;
  for (uint32_t l = 1; l < out.mips; ++l) {
    const uint32_t sw = std::max(out.w >> (l - 1), 1u), sh = std::max(out.h >> (l - 1), 1u), dw = std::max(out.w >> l, 1u), dh = std::max(out.h >> l, 1u);
    const size_t dst = src + size_t(sw) * sh * 4;
    for (uint32_t y = 0; y < dh; ++y)
      for (uint32_t x = 0; x < dw; ++x)
        {
          // S103 CMB fix 6: alpha-0 texels (BC1 3-colour mode's index 3: transparent black) are left out of the colour average, so a cut-out edge does not darken its mips;
          // alpha is the mean over all four. All four alpha-0 = transparent black.
          uint32_t s[4] = {0, 0, 0, 0}, opaque = 0;
          for (uint32_t dy = 0; dy < 2; ++dy)
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint8_t* p = &chain[src + (size_t(std::min(2 * y + dy, sh - 1)) * sw + std::min(2 * x + dx, sw - 1)) * 4];
              s[3] += p[3];
              if (p[3] == 0) continue;
              ++opaque;
              for (int c = 0; c < 3; ++c) s[c] += p[c];
            }
          uint8_t* o = &chain[dst + (size_t(y) * dw + x) * 4];
          for (int c = 0; c < 3; ++c) o[c] = opaque ? uint8_t((s[c] + opaque / 2) / opaque) : 0;
          o[3] = uint8_t((s[3] + 2) / 4);
        }
    src = dst;
  }
  out.bc1 = opt.bc1;
  if (!opt.bc1) {
    out.data = std::move(chain);
    return true;
  }
  // BC1: each level of the chain encoded on its own; the RGBA chain is freed before the caller uploads.
  size_t bytes = 0;
  for (uint32_t l = 0; l < out.mips; ++l) bytes += size_t((std::max(out.w >> l, 1u) + 3) / 4) * ((std::max(out.h >> l, 1u) + 3) / 4) * 8;
  out.data.assign(bytes, 0);
  size_t in_off = 0, out_off = 0;
  for (uint32_t l = 0; l < out.mips; ++l) {
    const uint32_t lw = std::max(out.w >> l, 1u), lh = std::max(out.h >> l, 1u);
    detail::EncodeBc1Level(chain.data() + in_off, lw, lh, out.data.data() + out_off);
    in_off += size_t(lw) * lh * 4;
    out_off += size_t((lw + 3) / 4) * ((lh + 3) / 4) * 8;
  }
  return true;
}

inline bool DecodeBc1Level(const uint8_t* data, size_t n, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
  detail::DdsView v;
  v.w = w, v.h = h, v.data = data, v.n = n;
  std::string why;
  return detail::DecodeBc1(v, rgba, why);
}

inline std::string EntryTag(const uint8_t* colours) {
  static const char* hex = "0123456789abcdef";
  std::string s = "/c:";
  for (int i = 0; i < kColourBytes; ++i) s += hex[colours[i] >> 4], s += hex[colours[i] & 15];
  return s;
}

inline bool IsRefusalTag(const std::string& entry) { return entry.size() >= 2 && entry.compare(entry.size() - 2, 2, "/r") == 0; }

inline bool ParseEntryTag(const std::string& entry, uint8_t* colours) {
  const size_t c = entry.find("/c:");
  if (c == std::string::npos || entry.size() < c + 3 + 2 * kColourBytes) return false;
  auto nib = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
  for (int i = 0; i < kColourBytes; ++i) {
    const int hi = nib(entry[c + 3 + 2 * size_t(i)]), lo = nib(entry[c + 4 + 2 * size_t(i)]);
    if (hi < 0 || lo < 0) return false;
    colours[i] = uint8_t(hi * 16 + lo);
  }
  return true;
}


}  // namespace attire_compose
