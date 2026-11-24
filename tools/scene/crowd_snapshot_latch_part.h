#include "../../src/renderer_scene/rows_crowd.h"
#include "../../src/renderer_scene/rows_pictures.h"
// Part of crowd_snapshot_unit.cpp (S103 lane D3b): the latch reader on forged manager memory. Included by the unit after its helpers (Check, Fresh, Record, g_stamps ...).
// Forged guest memory in the listing's layout (crowd_mgr_reader.h constants) and a HAND ORACLE of the member world (m = scale * Ry(angle), row 3 = x y z, transposed into c: CROWD docs), written from the
// documented formula, never from the reader's own code.
#pragma once
#include <unordered_map>
#include <unordered_set>

struct Forge {
  std::unordered_map<uint32_t, uint32_t> w;
  mutable bool fail = false;
  mutable uint32_t fail_addr = 0;   // S103 D8: the first refused address while `fail` was clear (as the glue's CrowdGuestMem)
  void U(uint32_t a, uint32_t v) { w[a] = v; }
  void F(uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); w[a] = v; }
  std::unordered_set<uint32_t> bad;   // S103 D6: addresses whose read fails (sets fail, reads 0)
  uint32_t u32(uint32_t a) const { if (bad.count(a)) { if (!fail) fail_addr = a; return fail = true, 0u; } auto it = w.find(a); return it == w.end() ? 0u : it->second; }
  float f32(uint32_t a) const { const uint32_t v = u32(a); float f; std::memcpy(&f, &v, 4); return f; }
  void words(uint32_t a, uint32_t n, uint32_t* dst) const { for (uint32_t i = 0; i < n; ++i) dst[i] = u32(a + 4 * i); }
  std::unordered_map<uint32_t, uint8_t> pmem;   // S103 D6: forged PIXEL memory (physical addresses), byte by byte
  bool phys(uint32_t a, uint32_t n, uint8_t* dst) const { if (!pmem.count(a)) return false; for (uint32_t i = 0; i < n; ++i) { auto it = pmem.find(a + i); dst[i] = it == pmem.end() ? 0 : it->second; } return true; }   // (a region is mapped when its first byte is; the tiled padding reads zero)
};
namespace fm {
constexpr uint32_t N = 0x95000000u, M = 0x90000000u, R = 0x91000000u, C = 0x92000000u, S = 0x93000000u, P = 0x94000000u, E = 0x835E4CD8u;
}
static void ForgeMember(Forge& f, uint32_t type, uint32_t k, uint32_t flags, uint32_t group, float x, float y, float z, uint32_t angle16, float scale) {
  f.U(fm::E + 0xF00Cu + 3072u * type + 4 * k, flags);
  f.U(fm::E + 12u + 3072u * type + 4 * k, group);
  f.F(fm::S + 3072u * type + 4 * k, scale);
  const uint32_t rec = fm::R + 12288u * type + 16u * k;
  f.F(rec, x), f.F(rec + 4, y), f.F(rec + 8, z);
  f.U(rec + 12, angle16 << 15);
}
// The game's u16 sine (listing sub_82C32228, written here by hand from the four branches) over the forged quarter table T[i] = sin(i * 2 pi / 65536). [U] whether the game's real table is this one (its bytes are in no
// capture); the reader follows the LISTING's indexing, so it reproduces the game's own function whatever the table holds: the mirror census (eq/near/diff) is the live proof.
static float GameTable(uint32_t i) { return float(std::sin(double(i) * 6.283185307179586 / 65536.0)); }
static float GameSin(uint32_t a) {
  a &= 0xFFFFu;
  if (a < 16384u) return GameTable(a);
  if (a < 32768u) return GameTable(32767u - a);
  if (a < 49152u) return -GameTable(a - 32768u);
  return -GameTable(65535u - a);
}
static crowd::MemberObs Oracle(uint32_t type, uint32_t lod, uint32_t index, bool card, float x, float y, float z, uint32_t angle16, float scale) {
  const float cs = GameSin(angle16 + 16384u) * scale, sn = GameSin(angle16) * scale;
  float m[16] = {};
  m[0] = cs, m[2] = -sn, m[5] = scale, m[8] = sn, m[10] = cs, m[12] = x, m[13] = y, m[14] = z, m[15] = 1.f;
  crowd::MemberObs o;
  o.block = type, o.lod = card ? crowd::kCardLayer : lod, o.index = index, o.card = card;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) o.c[r * 4 + k] = m[k * 4 + r];
  return o;
}
static void ForgeManager(Forge& f) {
  namespace mg = crowd::mgr;
  f.U(mg::kMgrGlobal, fm::M);
  f.U(mg::kRingIndex, 0);
  f.U(fm::M + mg::kRequestOff, 0);
  f.U(fm::E + 0, fm::R), f.U(fm::E + 4, fm::C), f.U(fm::E + 8, fm::S);
  for (uint32_t i = 0; i < 16384; ++i) f.F(mg::kSinTable + 4 * i, GameTable(i));
  f.U(fm::C + 0, 3), f.U(fm::C + 4, 1), f.U(fm::C + 8, 2);   // type 0: 3 members, type 1: 1, type 2: 2
  f.U(fm::M + crowd::latch::kNodeGateOff, 0);   // (the node object is forged apart: fm::N)
  f.U(fm::M + crowd::latch::kMgrDrawGateOff, 1), f.U(fm::M + crowd::latch::kPassGateOff0, 1);   // the entry's gates open: [manager+560] != 0, one of [476/480/484] != 0
  for (uint32_t i = 0; i < 3; ++i) f.U(fm::E + crowd::latch::kCardGateOff + 4 * i, 1);          // the card loop runs for types 0..2 (review F1)
  f.U(fm::N + crowd::latch::kNodeGateOff, 1);
  ForgeMember(f, 0, 0, 0u, 0, 1.f, 2.f, 3.f, 8192, 1.f);              // a near stand in group 0, also a card (flags clear: both rules take it)
  ForgeMember(f, 0, 1, 0u, 1, 4.f, 5.f, 6.f, 4096, 0.9f);             // group 1
  ForgeMember(f, 0, 2, 0x40000000u, 0, 7.f, 8.f, 9.f, 16384, 1.1f);  // bit 1 set: the block draw skips it, the card loop draws it
  ForgeMember(f, 1, 0, 0x20000000u, 2, -1.f, 0.f, 2.f, 24576, 0.95f); // bit 2 set: the block draw draws it (group 2), the card loop skips it
  for (uint32_t row = 0; row < 2; ++row)
    for (uint32_t k = 0; k < 16; ++k) f.F(fm::P + 3 * 1408 + 64 * row + 4 * k, 60.f + float(row) + 0.01f * float(k));   // type 3 LOD 0 (the floor seats' type): 2 rows
  ForgeMember(f, 2, 0, 0u, 0, 11.f, 12.f, 13.f, 57344, 1.f);                    // angle 57344 (the fourth quadrant)
  ForgeMember(f, 2, 1, 0u, 1, 14.f, 15.f, 16.f, 0x10000u | 40960u, 1.05f);       // angle 40960 in a word with the sign bit set: ((w >>a 15) & ~0x1FF) & 0xFFFF
  f.U(fm::M + crowd::latch::kPaletteBasePtr, fm::P), f.U(fm::M + crowd::latch::kWriteRingOffset, 0);
  for (uint32_t lod = 1; lod < 3; ++lod)
    for (uint32_t t = 0; t < 20; ++t)
      for (uint32_t j = 0; j < 22; ++j) f.U(fm::M + crowd::latch::kRemapOffset + 1760 * (lod - 1) + 88 * t + 4 * j, 0xFFFFFFFFu);
  for (uint32_t j = 0; j < 3; ++j) f.U(fm::M + crowd::latch::kRemapOffset + 4 * j, j);   // type 0 LOD 1: slots 0..2 <- 0..2
  for (uint32_t j = 0; j < 2; ++j) f.U(fm::M + crowd::latch::kRemapOffset + 88 * 2 + 4 * j, j);   // type 2 LOD 1: slots 0..1 <- 0..1
  for (uint32_t row = 0; row < 2; ++row)
    for (uint32_t k = 0; k < 16; ++k) {
      f.F(fm::P + 2 * 1408 + 64 * row + 4 * k, 40.f + float(row) + 0.01f * float(k));               // type 2 LOD 0: 2 rows
      f.F(fm::P + (1 * 20 + 2) * 1408 + 64 * row + 4 * k, 50.f + float(row) + 0.01f * float(k));    // type 2 LOD 1: 2 rows
    }
  for (uint32_t j = 0; j < 2; ++j) f.U(fm::M + crowd::latch::kRemapOffset + 1760 * 1 + 88 * 1 + 4 * j, j);   // type 1 LOD 2: slots 0..1 <- 0..1
  for (uint32_t row = 0; row < 2; ++row)
    for (uint32_t k = 0; k < 16; ++k) {
      f.F(fm::P + 1408 + 64 * row + 4 * k, 20.f + float(row) + 0.01f * float(k));                    // type 1 LOD 0: 2 rows
      f.F(fm::P + (2 * 20 + 1) * 1408 + 64 * row + 4 * k, 30.f + float(row) + 0.01f * float(k));     // type 1 LOD 2: 2 rows
    }
  for (uint32_t row = 0; row < 5; ++row)
    for (uint32_t k = 0; k < 16; ++k) f.F(fm::P + 64 * row + 4 * k, 1.f + float(row) + 0.01f * float(k));                          // type 0 LOD 0: 5 rows
  for (uint32_t row = 0; row < 3; ++row)
    for (uint32_t k = 0; k < 16; ++k) f.F(fm::P + 20 * 1408 + 64 * row + 4 * k, 10.f + float(row) + 0.01f * float(k));             // type 0 LOD 1: 3 rows
}
// ---- the seated floor people (docs\renderer\CROWD_FAMILY_8276ED60.md 2.1): the node's 52-word object list, a kind-0 group of 4 seats of type 3, and two single-person objects
static void H16(Forge& f, uint32_t addr, uint32_t v) {   // a halfword in big-endian memory read as words
  uint32_t w = f.u32(addr & ~3u);
  if (addr & 2u) w = (w & 0xFFFF0000u) | (v & 0xFFFFu);
  else w = (w & 0x0000FFFFu) | (v << 16);
  f.U(addr & ~3u, w);
}
constexpr uint32_t kFloorObj = 0x96000000u, kFloorElems = 0x96100000u, kFloorMat = 0x97000000u, kFloorPos = 0x97100000u, kFloorFlags = 0x97200000u;
// ---- the signs and foam hands (docs\renderer\CROWD_PROPS_LATCH.md): this = node - 32
static void H8(Forge& f, uint32_t addr, uint32_t v) {   // a byte in big-endian memory read as words
  uint32_t w = f.u32(addr & ~3u);
  const uint32_t sh = (3u - (addr & 3u)) * 8u;
  f.U(addr & ~3u, (w & ~(0xFFu << sh)) | ((v & 0xFFu) << sh));
}
constexpr uint32_t kPropThis = fm::N - 32;
static void ForgeEntity(Forge& f, uint32_t type, uint32_t E, uint32_t x1184, uint32_t w832, uint32_t tab, uint32_t holder_idx) {
  f.U(fm::M + 4 * (type + 10), E);
  f.U(E + 1184, x1184), f.U(E + 832, w832);
  f.U(E + 4 * (w832 == 0 ? 207 : 206), 1);
  f.U(E + 48, E + 0x1000), f.U(E + 0x1000 + 192, tab);
  const uint32_t holder = 0x9D000000u + 0x100u * type;
  f.U(kPropThis + 4 * (type + 16), holder);
  H16(f, holder + 34, holder_idx);
}
// The game's gates, forged OPEN with the listings' literals (docs\renderer\CROWD_PROPS_GATES.md): M = fm::M, ring [M+544] = 0.
constexpr uint32_t kTexMgrObj = 0x9E100000u, kTexTab = 0x9E110000u, kHandleTab = 0x9E120000u, kHandleBegin = 0x9E130000u, kHandKeyObj = 0x9E140000u, kHandKeyPtr = 0x9E150000u;
constexpr uint32_t kSignCount = 0x9E160000u, kSignRing = 0x9E161000u, kSignVert = 0x9E162000u, kHandCount = 0x9E170000u, kHandVert = 0x9E171000u, kHandRes = 0x9E172000u;
constexpr uint32_t kSignKey = 0x00030007u, kHandKey = 0x00040009u;   // texture keys: handle-table index 7 / 9
static void ForgeHandType(Forge& f, uint32_t b, uint32_t count_ptr, uint32_t vert_ptr, uint32_t res_ptr) {   // sub_82518EB8 0x82519048-0x825190B0: rec = M + 28 b
  f.U(fm::M + 28 * b + 0x198E0, count_ptr), f.U(count_ptr, 77);
  f.U(fm::M + 28 * b + 0x198DC, vert_ptr), f.U(vert_ptr, 1);
  f.U(fm::M + 4 * (7 * b + 0 + 26165), res_ptr), f.U(res_ptr, 1);   // sub_82512EF0, ring 0
}
// S103 D6: a holder with a COMPLETE pose chain: entity (3 nodes, bone table with parents -1 / 0 / 1, inverse binds at +224), the locals in the update-side block, bone 2 turned by RotY(-0.5). Oracle by hand in double:
// chain[k] = local[k] x chain[parent] (parent -1: local), skin[k] = inv[k] x chain[k]; the root is the row's world, NOT in the palette (listing sub_82BD30E8 / sub_82BD35B0, guest_texture-free).
constexpr uint32_t kSkObj = 0x9E700000u, kSkElt = 0x9E701000u, kSkFp = 0x9E702000u, kSkMat = 0x9E703000u, kSkArr = 0x9E704000u, kSkEnt = 0x9E705000u, kSkTab = 0x9E706000u, kSkInv = 0x9E707000u;
static double SkLocal(int k, int i, int j) { return double(10 * k) + 0.25 * double(i * 4 + j) + ((i * 4 + j) % 5 == 0 ? 1.0 : 0.0); }
static double SkInv(int k, int i, int j) { return 0.5 * double(k + 1) + 0.0625 * double(i * 4 + j) - (i == j ? 0.25 : 0.0); }
static void ForgeSkinned(Forge& sf, uint32_t slot_in_node = 5) {
  sf.U(fm::N + 32 + 4 * slot_in_node, kSkObj);
  sf.U(kSkObj, 0x8202B8DCu), sf.U(kSkObj + 24, 21), sf.U(kSkObj + 28, 7), sf.U(kSkObj + 64, kSkElt);
  sf.U(kSkObj + 32, 2u << 16);                 // the bone index halfword (high half of the word at +32): 2
  sf.U(kSkElt + 160, kSkFp), sf.U(kSkElt + 152, kSkMat), sf.F(kSkElt + 116, 0.5f);
  sf.U(kSkFp, 0x4000u << 16);                  // flags halfword 0x4000 (big-endian: the high half of the word)
  for (uint32_t q = 0; q < 16; ++q) sf.F(kSkMat + 4 * q, 100.f + float(q));
  sf.U(fm::M + 16, kSkArr), sf.U(kSkArr + 4 * 7, kSkEnt), sf.U(kSkEnt, crowd::latch::kSkinModelVtable), sf.U(kSkEnt + 296, kSkTab), sf.U(kSkEnt + 300, 3), sf.U(kSkEnt + 224, kSkInv);
  sf.U(kSkTab + 48 + 80 * 0, 0xFFFFFFFFu), sf.U(kSkTab + 48 + 80 * 1, 0), sf.U(kSkTab + 48 + 80 * 2, 1);
  sf.U(fm::M + 0, 0);                          // M[0] == 0 -> parity z = 1
  const uint32_t blk = fm::M + 2304 + 1408 * (2 * 1 + 1);
  for (uint32_t n = 0; n < 3; ++n)
    for (uint32_t q = 0; q < 16; ++q) {
      sf.F(blk + 64 * n + 4 * q, float(SkLocal(int(n), int(q / 4), int(q % 4))));
      sf.F(kSkInv + 64 * n + 4 * q, float(SkInv(int(n), int(q / 4), int(q % 4))));
    }
}
static void SkinOracle(double skin[3][16]) {
  double L[3][4][4], C[3][4][4];
  for (int k = 0; k < 3; ++k)
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) L[k][i][j] = double(float(SkLocal(k, i, j)));
  const double cs = std::cos(-0.5), sn = std::sin(-0.5);
  const double R[4][4] = {{cs, 0, -sn, 0}, {0, 1, 0, 0}, {sn, 0, cs, 0}, {0, 0, 0, 1}};
  double T[4][4];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      T[i][j] = 0;
      for (int q = 0; q < 4; ++q) T[i][j] += L[2][i][q] * R[q][j];
    }
  std::memcpy(L[2], T, sizeof T);                // bone 2 x RotY
  const int parent[3] = {-1, 0, 1};
  for (int k = 0; k < 3; ++k)
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        if (parent[k] < 0) { C[k][i][j] = L[k][i][j]; continue; }
        double a = 0;
        for (int q = 0; q < 4; ++q) a += L[k][i][q] * C[parent[k]][q][j];
        C[k][i][j] = a;
      }
  for (int k = 0; k < 3; ++k)
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        double a = 0;
        for (int q = 0; q < 4; ++q) a += double(float(SkInv(k, i, q))) * C[k][q][j];
        skin[k][i * 4 + j] = a;
      }
}
// S103 D6: a forged picture behind a wrapper: wrapper +52 kind 0, +80 header, header +28 the six fetch dwords (tiled bit, pitch / 32, DXT1 = 18, endian 8-in-16, base, size, 2D), the blocks TILED with the
// reference formula (Xenia's GetTiledOffset2D, written here apart from guest_texture.h) and 16-bit swapped in physical memory. `lin` = the linear little-endian blocks the reader must give back.
static uint32_t RefTiled(int32_t x, int32_t y, uint32_t width, uint32_t log2_bpp) {
  width = (width + 31u) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(width >> 5)) << (log2_bpp + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpp;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return uint32_t(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F));
}
static void ForgePicture(Forge& f, uint32_t wrapper, uint32_t header, uint32_t base, uint32_t w, uint32_t h, uint8_t seed, std::vector<uint8_t>* lin = nullptr, bool linear = false) {   // S103 CSF: `linear` = an untiled texture, rows padded to 32 blocks (the foam hand's real layout)
  f.U(wrapper, 0x8206ADB8u), f.U(wrapper + 52, 0), f.U(wrapper + 80, header);
  const uint32_t pitch_div32 = ((w + 31u) & ~31u) / 32u;   // (texels; the sign pictures are 64 wide)
  f.U(header + 28, (linear ? 0u : (1u << 31)) | (pitch_div32 << 22)), f.U(header + 32, 18u | (1u << 6) | (wwe2k14::guest_view::ToEViewCpu(base) & 0xFFFFF000u)), f.U(header + 36, (w - 1) | ((h - 1) << 13));
  f.U(header + 40, 0), f.U(header + 44, 0), f.U(header + 48, 1u << 9);
  const uint32_t bx = w / 4, by = h / 4;
  if (lin) lin->assign(size_t(bx) * by * 8, 0);
  for (uint32_t y = 0; y < by; ++y)
    for (uint32_t x = 0; x < bx; ++x) {
      uint8_t blk[8];
      for (int i = 0; i < 8; ++i) blk[i] = uint8_t(seed + 7 * x + 13 * y + 31 * i);
      if (lin) std::memcpy(lin->data() + (size_t(y) * bx + x) * 8, blk, 8);
      const uint32_t at = base + (linear ? (y * ((bx + 31u) & ~31u) + x) * 8u : RefTiled(int32_t(x), int32_t(y), ((bx + 31u) & ~31u), 3));
      for (int i = 0; i < 8; i += 2) f.pmem[at + i] = blk[i + 1], f.pmem[at + i + 1] = blk[i];   // stored 8-in-16 swapped
    }
}
static std::vector<uint8_t> g_sign_lin, g_hand_lin;   // S103 D6: what the reader must give back for the sign / the foam hand picture
// S103 SG: the six wrestler slots static_SetUpHandSignBoard (sub_8250B1E8) walks, forged with the LISTINGS' literals (not the reader's constants): G = [0x836F2D88] records of 3580 bytes (id at +780,
// attire word at +724+16), R = [0x836F2D98] -> data of 3376 bytes per slot, h = halfword [data + 2 (idx + 98)], idx = sub_82506E30(id, tens digit). Expected types (the hand oracle, by hand):
//   s0 id 343, attire 20 -> idx 0, h 3   -> b0 (mesh 41002)
//   s1 id 139, attire 10 -> v 2 -> idx 1, h at +2*99 = 12 (the idx 0 halfword holds 5: a reader ignoring the table takes it) -> b1 (41011)
//   s2 id 343, h 25 (> 19: key 40999, absent from every arena set) -> skipped
//   s3 id 324, attire 0 -> v 1 -> idx 0, h 7 -> b2 (41006)
//   s4 id 77,  h 17 -> b3 (41016: the type the forged foam hand uses, elt+19 = 3)
//   s5 id 324, attire 10 -> v 2 -> idx -1 -> skipped (its h 9 is never read)
// The loader's own count [M + 0x198D0] = 4.
constexpr uint32_t kHandG = 0x9E400000u, kHandR = 0x9E480000u, kHandData = 0x9E500000u;
static void ForgeHandSlots(Forge& f, uint32_t game_count = 4) {
  f.U(0x836F2D88u, kHandG), f.U(0x836F2D98u, kHandR), f.U(kHandR, kHandData);
  const uint32_t id[6] = {343, 139, 343, 324, 77, 324}, att[6] = {20, 10, 0, 0, 0, 10}, h0[6] = {3, 5, 25, 7, 17, 9}, h1[6] = {0, 12, 0, 0, 0, 0};
  for (uint32_t s = 0; s < 6; ++s) {
    f.U(kHandG + 3580 * s + 780, id[s]), f.U(kHandG + 3580 * s + 724 + 16, att[s]);
    H16(f, kHandData + 3376 * s + 2 * 98, h0[s]), H16(f, kHandData + 3376 * s + 2 * 99, h1[s]);
  }
  f.U(fm::M + 0x198D0, game_count);
}
static void ForgePropGates(Forge& f) {
  f.U(fm::M + 0x19710, kSignCount), f.U(kSignCount, 77);            // sub_82518950 0x8251898C: the mesh's index count
  f.U(fm::M + 4 * (0 + 26049), kSignRing), f.U(kSignRing, 1);        // 0x825189A4-0x825189B0: the read ring's entry (ring 0)
  f.U(fm::M + 0x1970C, kSignVert), f.U(kSignVert, 1);                // 0x825189C4-0x825189CC
  ForgeHandType(f, 3, kHandCount, kHandVert, kHandRes);
  f.U(0x83767C5Cu, kTexMgrObj), f.U(kTexMgrObj + 8, kTexTab);        // 0x82518988, 0x82518BC8: [[r17 + 8] + 8 b]
  f.U(kTexTab + 8 * 2, kSignKey);
  f.U(0x8392DD9Cu, kHandleTab), f.U(kHandleTab + 8, kHandleBegin), f.U(kHandleTab + 16, kHandleBegin + 8 * 16);   // sub_82294F20: begin [P+8], end [P+16], 16 entries
  f.U(kHandleBegin + 8 * 7, kSignKey), f.U(kHandleBegin + 8 * 7 + 4, 0x00AA0001u);
  f.U(kHandleBegin + 8 * 9, kHandKey), f.U(kHandleBegin + 8 * 9 + 4, 0x00BB0002u);
  ForgePicture(f, 0x00AA0001u, 0x9E300000u, 0x00100000u, 64, 64, 11, &g_sign_lin), ForgePicture(f, 0x00BB0002u, 0x9E301000u, 0x00200000u, 64, 128, 53, &g_hand_lin, true);   // S103 D6
  f.U(fm::M + 4 * (3 + 26277), kHandKeyObj), f.U(kHandKeyObj + 4, kHandKeyPtr), f.U(kHandKeyPtr, kHandKey);   // 0x825191EC-0x82519144: [[M + 4 (b + 26277)] + 4]
  ForgeHandSlots(f);
}
static void ForgeProps(Forge& f) {
  ForgeEntity(f, 5, 0x9A000000u, 14006, 0, 0x9A200000u, 2);   // signs: x 14006, [E+832] == 0 -> the word at E + 4 * 207
  ForgeEntity(f, 6, 0x9B000000u, 14127, 7, 0x9B200000u, 1);   // foam hands: x 14127, [E+832] != 0 -> the word at E + 4 * 206
  const uint32_t types[3] = {5, 6, 5}, flags[3] = {0x4000u, 0x4000u, 0u};   // the third object has no 0x4000: skipped
  for (uint32_t j = 0; j < 3; ++j) {
    const uint32_t obj = 0x9C000000u + 0x10000u * j, elt = obj + 0x1000u, fp = obj + 0x2000u, mat = obj + 0x3000u;
    f.U(kPropThis + 144 + 4 * j, obj), f.U(obj + 64, elt), f.U(elt + 160, fp), f.U(elt + 152, mat);
    H8(f, elt + 23, types[j]);
    H8(f, elt + 18, 2), H8(f, elt + 19, 3);   // the sign's style byte (elt+18, 0x82518BC0) and the foam hand's type byte (elt+19, 0x82519048)
    H16(f, fp, flags[j]);
    for (uint32_t r = 0; r < 4; ++r)
      for (uint32_t k = 0; k < 4; ++k) f.F(mat + 16 * r + 4 * k, 1.f + float(j) + 0.1f * float(r) + 0.01f * float(k) + (r == k ? 1.f : 0.f));
  }
  for (uint32_t i = 0; i < 4; ++i)
    for (uint32_t k = 0; k < 4; ++k) {
      f.F(0x9A200000u + 64 * 2 + 16 * i + 4 * k, 0.5f * float(i) + 0.25f * float(k) + (i == k ? 2.f : 0.f));   // sign bone (idx 2)
      f.F(0x9B200000u + 64 * 1 + 16 * i + 4 * k, 0.75f * float(i) - 0.125f * float(k) + (i == k ? 3.f : 0.f));  // foam bone (idx 1)
    }
  f.U(kPropThis + 596, 0);
  ForgePropGates(f);
}
static crowd::PropObs PropOracle(uint32_t site, uint32_t j, bool foam) {   // W = B x M0 by hand (row vector), c[r*4+k] = W[k][r]
  float M0[4][4], B[4][4], W[4][4];
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) {
      M0[r][k] = 1.f + float(j) + 0.1f * float(r) + 0.01f * float(k) + (r == k ? 1.f : 0.f);
      B[r][k] = foam ? 0.75f * float(r) - 0.125f * float(k) + (r == k ? 3.f : 0.f) : 0.5f * float(r) + 0.25f * float(k) + (r == k ? 2.f : 0.f);
    }
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) {
      W[i][k] = 0.f;
      for (int q = 0; q < 4; ++q) W[i][k] += B[i][q] * M0[q][k];
    }
  crowd::PropObs o;
  o.site = site, o.index = j;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) o.c[r * 4 + k] = W[k][r];
  return o;
}
static bool PropNear(const crowd::PropObs& a, const crowd::PropObs& b) {
  if (a.site != b.site) return false;
  for (int k = 0; k < 12; ++k)
    if (!(std::fabs(a.c[k] - b.c[k]) <= 1e-5f)) return false;
  return true;
}
// The camera block of the screen test (sub_8250E7E0): view = identity, projection with m22 = m23 = 1 so the clip w is the seat's z
static void ForgeCamera(Forge& f) {
  const uint32_t obj = 0x9F000000u, C = 0x9F001000u;
  f.U(crowd::latch::kCamTable, obj), f.U(obj + 256, C);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      f.F(C + 240 + 16 * i + 4 * j, i == j ? 1.f : 0.f);
      f.F(C + 176 + 16 * i + 4 * j, (i == j && i < 2) ? 1e-4f : (i == j && i == 2) ? 1.f : 0.f);
    }
  f.F(C + 176 + 16 * 2 + 4 * 3, 1.f);
  f.F(crowd::latch::kScreenK, 120.f), f.F(crowd::latch::kScreenNear, 80.f), f.F(crowd::latch::kScreenOne, 1.f), f.F(crowd::latch::kScreenLo, -0.9f), f.F(crowd::latch::kScreenHi, 0.9f);
}
// S103 C1: the camera of the singles' screen test sub_8276E2C0 (the object sub_82BA7748 returns on the draw thread): view = identity, projection diag(1e-4, 1e-4, 1, 1) so every fixture holder near the
// origin is inside the box (|px|, |py| ~ 1e-2, radius 120 * 1e-4), w = 1. The constants are the image's (K 120, y offset -120, one 1, near 0, lo -1). [0x83929A68] made non-zero when the fixture has none
// (a dummy object whose +256 is 0: the floor test then hides nothing, as without it).
constexpr uint32_t kSingleCam = 0x9F400000u, kSingleCamDummy = 0x9F500000u;
static void SetScreenScale(float sx, float sy) {   // S105 A2: the crowd switch's 1 / k, 1 / t as start_settings.cpp sets them
  uint32_t bx, by;
  std::memcpy(&bx, &sx, 4), std::memcpy(&by, &sy, 4);
  crowd::latch::g_screen_sx_bits.store(bx), crowd::latch::g_screen_sy_bits.store(by);
}
static void ForgeSingleCamera(Forge& f) {
  if (!f.u32(crowd::latch::kSingleCamGate)) f.U(crowd::latch::kSingleCamGate, kSingleCamDummy);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      f.F(kSingleCam + 240 + 16 * i + 4 * j, i == j ? 1.f : 0.f);
      f.F(kSingleCam + 176 + 16 * i + 4 * j, i == j ? (i < 2 ? 1e-4f : 1.f) : 0.f);
    }
  f.F(crowd::latch::kSingleScreenK, 120.f), f.F(crowd::latch::kSingleScreenYOff, -120.f), f.F(crowd::latch::kSingleScreenOne, 1.f), f.F(crowd::latch::kSingleScreenNear, 0.f), f.F(crowd::latch::kSingleScreenLo, -1.f);
}
static void ForgeFloor(Forge& f, float f31_phase4) {
  f.U(fm::N + crowd::latch::kNodeLodWord, 0);
  f.U(fm::N + crowd::latch::kNodeObjects + 0, kFloorObj);
  f.U(kFloorObj, crowd::latch::kFloorVtable), f.U(kFloorObj + 20, 6), f.U(kFloorObj + 24, 3), f.U(kFloorObj + 64, kFloorElems);
  // sub_82512E80's group conditions for type 3, LOD 0: read ring 0, [M + 628 + 28 * 3] > 0, the stream pointer [M + 4 (153 + 7 * 3 + 0)] and its first word non-null
  f.U(fm::M + 628 + 28 * 3, 1), f.U(fm::M + 4 * (153 + 21), 0x9E000000u), f.U(0x9E000000u, 0x9E000100u);
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t e = kFloorElems + 176 * i;
    f.U(e + 152, kFloorMat + 64 * i), f.U(e + 156, kFloorPos + 16 * i), f.U(e + 160, kFloorFlags + 2 * i);
    for (uint32_t r = 0; r < 4; ++r)
      for (uint32_t k = 0; k < 4; ++k) f.F(kFloorMat + 64 * i + 16 * r + 4 * k, float(100 * i + 10 * r + k));
    f.F(kFloorPos + 16 * i, float(2 * i + 1)), f.F(kFloorPos + 16 * i + 4, float(3 * i + 2)), f.F(kFloorPos + 16 * i + 8, float(100 + 4 * i));
    H8(f, e + 23, 3);   // the element's type byte
    if (i == 4) f.F(kFloorPos + 16 * i, 5.0e6f), f.F(kFloorPos + 16 * i + 8, 100.f);   // seat 4: far off to the side, a tiny radius: the screen BOX hides it
    if (i == 5) f.F(kFloorPos + 16 * i + 8, 50.f);                                                                            // seat 5: clip w 50 < 80: the NEAR cut hides it
    f.U(kFloorPos + 16 * i + 12, i == 2 ? 0x80808080u : 0x00808080u);   // seat 2: the screen test's top bit (not honoured: the reader holds it)
    H16(f, kFloorFlags + 2 * i, i == 1 ? 0x1000u : i == 3 ? 0x8000u : 0u);   // seat 1 hidden by 0x1000; seat 3 by 0x8000 only when [manager+248] != 0
  }
  // two single-person objects (vtable 0x8202B8DC): slot 1's element is hidden by 0x1000, slot 2's is not (-> one unread)
  for (uint32_t s = 1; s <= 2; ++s) {
    const uint32_t o = 0x98000000u + 0x1000u * s, el = o + 0x100u, fl = 0x99000000u + 4 * s;
    f.U(fm::N + crowd::latch::kNodeObjects + 4 * s, o);
    f.U(o, crowd::latch::kFloorSingleVtable), f.U(o + 64, el), f.U(el + 160, fl);
    H16(f, fl, s == 1 ? 0x1000u : 0u);
    if (s == 2) f.U(o + 28, 1), f.U(fm::M + 16, 0x9A000000u), f.U(0x9A000000u + 4, 0x9A001000u), f.U(0x9A001000u, crowd::latch::kSkinModelVtable), f.U(0x9A001000u + 296, 1), f.U(0x9A001000u + 300, 2);   // S103 D6: a READABLE entity (2 nodes) with no inverse bind / bone table: not modelled, stays unread
  }
  f.U(crowd::latch::kF31Zero, 0), f.F(crowd::latch::kF31Phase4, f31_phase4);
  f.U(crowd::latch::kPhaseWord, 0);
}
// the oracle by hand: c[r*4+k] = row_k[r] for k < 3 from the matrix, k = 3 from the position (y - f31)
static crowd::MemberObs FloorOracle(uint32_t i, float f31) {
  crowd::MemberObs o;
  o.block = 3, o.lod = 0, o.index = i, o.floor = true;
  const float pos[3] = {float(2 * i + 1), float(3 * i + 2) - f31, float(100 + 4 * i)};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) o.c[r * 4 + k] = float(100 * i + 10 * k + r);
    o.c[r * 4 + 3] = pos[r];
  }
  return o;
}
static std::vector<crowd::MemberObs> OracleStands() {
  return {Oracle(0, 0, 0, false, 1, 2, 3, 8192, 1.f), Oracle(0, 1, 1, false, 4, 5, 6, 4096, 0.9f), Oracle(1, 2, 0, false, -1, 0, 2, 24576, 0.95f),
          Oracle(2, 0, 0, false, 11, 12, 13, 57344, 1.f), Oracle(2, 1, 1, false, 14, 15, 16, 40960, 1.05f)};
}
static std::vector<crowd::MemberObs> OracleCards() {
  return {Oracle(0, 0, 0, true, 1, 2, 3, 8192, 1.f), Oracle(0, 0, 1, true, 4, 5, 6, 4096, 0.9f), Oracle(0, 0, 2, true, 7, 8, 9, 16384, 1.1f),
          Oracle(2, 0, 0, true, 11, 12, 13, 57344, 1.f), Oracle(2, 0, 1, true, 14, 15, 16, 40960, 1.05f)};
}
static bool WorldsNear(const crowd::MemberObs& a, const crowd::MemberObs& b) {
  if (a.block != b.block || a.lod != b.lod || a.index != b.index || a.card != b.card) return false;
  for (int k = 0; k < 12; ++k)
    if (!(std::fabs(a.c[k] - b.c[k]) <= 1e-6f)) return false;
  return true;
}
static Forge g_forge;
static uint32_t ForgeRead(crowd::LatchSnapshot& out) { return uint32_t(crowd::latch::ReadSnapshot(g_forge, out)); }
// A walked crowd draw with the oracle's members and the two palettes the forged manager holds.
// The hook's palette copies (sub_82509CB8's hook) into ring `ring`, the six slots the forged manager holds; `shift` makes another set (review F2).
static void FeedPalettes(crowd::Contributor& c, uint32_t ring, float shift = 0.f) {
  float v[22 * 16];
  auto put = [&](uint32_t type, uint32_t lod, uint32_t rows, float base) {
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t k = 0; k < 16; ++k) v[row * 16 + k] = base + float(row) + 0.01f * float(k) + shift;
    c.SetPalette(ring, type, lod, v, rows);
  };
  put(0, 0, 5, 1.f), put(0, 1, 3, 10.f), put(1, 0, 2, 20.f), put(1, 2, 2, 30.f), put(2, 0, 2, 40.f), put(2, 1, 2, 50.f), put(3, 0, 2, 60.f);
}
static void SetWalk(crowd::Contributor& c, const std::vector<crowd::MemberObs>& ms, const std::vector<crowd::PropObs>& props = {}) {
  float a[22 * 16], b[22 * 16];
  for (uint32_t row = 0; row < 5; ++row)
    for (uint32_t k = 0; k < 16; ++k) a[row * 16 + k] = 1.f + float(row) + 0.01f * float(k);
  for (uint32_t row = 0; row < 3; ++row)
    for (uint32_t k = 0; k < 16; ++k) b[row * 16 + k] = 10.f + float(row) + 0.01f * float(k);
  c.SetPalette(0, 0, 0, a, 5), c.SetPalette(0, 0, 1, b, 3);
  float d[22 * 16], e[22 * 16];
  for (uint32_t row = 0; row < 2; ++row)
    for (uint32_t k = 0; k < 16; ++k) d[row * 16 + k] = 20.f + float(row) + 0.01f * float(k), e[row * 16 + k] = 30.f + float(row) + 0.01f * float(k);
  c.SetPalette(0, 1, 0, d, 2), c.SetPalette(0, 1, 2, e, 2);
  float g[22 * 16], h[22 * 16];
  for (uint32_t row = 0; row < 2; ++row)
    for (uint32_t k = 0; k < 16; ++k) g[row * 16 + k] = 40.f + float(row) + 0.01f * float(k), h[row * 16 + k] = 50.f + float(row) + 0.01f * float(k);
  c.SetPalette(0, 2, 0, g, 2), c.SetPalette(0, 2, 1, h, 2);
  float q[22 * 16];
  for (uint32_t row = 0; row < 2; ++row)
    for (uint32_t k = 0; k < 16; ++k) q[row * 16 + k] = 60.f + float(row) + 0.01f * float(k);
  c.SetPalette(0, 3, 0, q, 2);
  c.BeginDraw(0);
  for (const auto& m : ms) c.NoteMember(m);
  for (const auto& pr : props) c.NoteProp(pr.site, pr.c);
  c.EndDraw();
}

static void LatchTests(crowd::Contributor& c, int p) {
  // ---- K: the reader on forged manager memory against the hand oracle
  {
    ForgeManager(g_forge);
    crowd::LatchSnapshot snap;
    const crowd::latch::Why why = crowd::latch::ReadSnapshot(g_forge, snap);
    Check(why == crowd::latch::Why::None, "latch.reads", std::to_string(int(why)));
    std::vector<crowd::MemberObs> st, cd;
    for (const auto& m : snap.members) (m.card ? cd : st).push_back(m);
    const auto os = OracleStands(), oc = OracleCards();
    bool ok = st.size() == os.size();
    for (size_t i = 0; ok && i < st.size(); ++i) ok = WorldsNear(st[i], os[i]);
    Check(ok, "latch.world_matches_walk", "stands " + std::to_string(st.size()));
    bool okc = cd.size() == oc.size();
    for (size_t i = 0; okc && i < cd.size(); ++i) okc = WorldsNear(cd[i], oc[i]);
    Check(okc, "latch.cards_rule", "cards " + std::to_string(cd.size()));
    {   // the entry's own gates and the card gate (review F1, F3), on copies of the forged manager
      crowd::LatchSnapshot g;
      Forge nocard = g_forge;
      nocard.U(fm::E + crowd::latch::kCardGateOff + 0, 0);   // type 0's card gate closed
      crowd::latch::ReadSnapshot(nocard, g);
      size_t t0 = 0, t2 = 0, stands = 0;
      for (const auto& m : g.members) { if (m.card) (m.block == 0 ? t0 : t2)++; else stands++; }
      Check(t0 == 0 && t2 == 2 && stands == 5, "latch.card_gate", "t0 " + std::to_string(t0) + " t2 " + std::to_string(t2) + " stands " + std::to_string(stands));
      Forge no560 = g_forge;
      no560.U(fm::M + crowd::latch::kMgrDrawGateOff, 0);
      g.Clear();
      Check(crowd::latch::ReadSnapshot(no560, g) == crowd::latch::Why::None && g.members.empty(), "latch.manager_draw_gate");
      Forge nopass = g_forge;
      nopass.U(fm::M + crowd::latch::kPassGateOff0, 0);
      g.Clear();
      Check(crowd::latch::ReadSnapshot(nopass, g) == crowd::latch::Why::None && g.members.empty() && !g.palettes[0].empty(), "latch.pass_gate_476_484");
      nopass.U(fm::M + crowd::latch::kPassGateOff2, 5);   // any one of the three opens it
      g.Clear();
      crowd::latch::ReadSnapshot(nopass, g);
      Check(!g.members.empty(), "latch.pass_gate_any_of_three");
      Forge nonode = g_forge;
      nonode.U(fm::N + crowd::latch::kNodeGateOff, 0);
      g.Clear();
      g.node = fm::N;
      Check(crowd::latch::ReadSnapshot(nonode, g) == crowd::latch::Why::None && g.members.empty(), "latch.node_gate");
      g.Clear();
      g.node = fm::N;
      crowd::latch::ReadSnapshot(g_forge, g);
      Check(!g.members.empty(), "latch.node_gate_open");
    }
    {   // the seated floor people (kind-0 group at node slot 0), phase word != 4 -> f31 = 0
      Forge fl = g_forge;
      ForgeFloor(fl, 10.f);
      crowd::LatchSnapshot g;
      g.node = fm::N;
      crowd::latch::ReadSnapshot(fl, g);
      std::vector<crowd::MemberObs> fm_;
      for (const auto& m : g.members) if (m.floor) fm_.push_back(m);
      // no camera block: nothing is hidden by the screen test; seat 1 is hidden by 0x1000, seats 2 (the screen-test bit), 3 (0x8000, flag mode off), 4 and 5 are read
      bool okf = fm_.size() == 5 && fm_[0].index == 0 && fm_[1].index == 2 && fm_[2].index == 3 && fm_[3].index == 4 && fm_[4].index == 5;
      const uint32_t want[3] = {0, 2, 3};
      for (size_t q = 0; okf && q < 3; ++q) okf = WorldsNear(fm_[q], FloorOracle(want[q], 0.f));
      Check(okf, "latch.floor_world", "floor " + std::to_string(fm_.size()));
      {   // review item 2: with the camera block the port of sub_8250E7E0 hides seat 5 (clip w < 80) and seat 4 (outside the box); the top bit of the position's w word (seat 2) is still not honoured
        Forge cam = fl;
        ForgeCamera(cam);
        crowd::LatchSnapshot gc;
        gc.node = fm::N;
        crowd::latch::ReadSnapshot(cam, gc);
        std::vector<uint32_t> idx;
        for (const auto& m : gc.members) if (m.floor) idx.push_back(m.index);
        Check(idx.size() == 3 && idx[0] == 0 && idx[1] == 2 && idx[2] == 3, "latch.floor_screen", "n " + std::to_string(idx.size()));
        {   // S105 A2: on a frame widened by k = 1.333 the floor seats' screen test is widened by 1 / k too (g_screen_sx_bits): a seat at clip x 1.0 (past the game's 0.9 box) is shown, a seat at 1.4 still hidden; the scale off (16:9, `off`) = today
          Forge wcam = cam;
          wcam.F(kFloorPos + 16 * 0, 1.0e6f);   // seat 0: x 1e6, w = z = 100, Pj[0][0] 1e-4: px = 1.0 (hi 0.9: hidden by the game's own box)
          auto count_seat0 = [&](float sx) {
            SetScreenScale(sx, 1.f);
            crowd::LatchSnapshot gw;
            gw.node = fm::N;
            crowd::latch::ReadSnapshot(wcam, gw);
            SetScreenScale(1.f, 1.f);
            for (const auto& m : gw.members) if (m.floor && m.index == 0) return true;
            return false;
          };
          const bool off_shown = count_seat0(1.f), wide_shown = count_seat0(0.75f);
          Check(!off_shown && wide_shown, "latch.floor_screen_wide", std::string("scale off: ") + (off_shown ? "shown" : "hidden") + ", 1/k 0.75: " + (wide_shown ? "shown" : "hidden"));
          wcam.F(kFloorPos + 16 * 0, 1.4e6f);   // px = 1.4 x 0.75 = 1.05: still hidden
          Check(!count_seat0(0.75f), "latch.floor_screen_wide_edge");
        }
        // the near cut alone: seat 5 only
        Forge cam2 = cam;
        cam2.F(kFloorPos + 16 * 4 + 0, 1.f);      // seat 4 back inside the box
        cam2.F(kFloorMat + 64 * 4 + 20, 1000.f);  // and a big radius
        gc.Clear();
        gc.node = fm::N;
        crowd::latch::ReadSnapshot(cam2, gc);
        bool has4 = false, has5 = false;
        for (const auto& m : gc.members) if (m.floor) has4 |= m.index == 4, has5 |= m.index == 5;
        Check(has4 && !has5, "latch.floor_near_cut", "has4 " + std::to_string(has4) + " has5 " + std::to_string(has5));
      }
      {   // review items 2, 6 and the walk-away cases: LOD, count cap, null elements, group condition, type byte
        auto floor_count = [&](Forge& fx, uint32_t* unread = nullptr) {
          crowd::LatchSnapshot gx;
          gx.node = fm::N;
          crowd::latch::ReadSnapshot(fx, gx);
          if (unread) *unread = gx.unread;
          size_t n = 0;
          for (const auto& m : gx.members) n += m.floor;
          return n;
        };
        uint32_t un = 0;
        {   // S103 C1 (R15 item 4): a node LOD != 0 draws group g = 20 lod + type (0x8276EAC4-0x8276EB10), its own stream / record (sub_82512E80(M, g))
          const size_t lod0 = floor_count(fl);
          Forge lod = fl;
          lod.U(fm::N + crowd::latch::kNodeLodWord, 1);   // group 23: its record is closed ([M + 628 + 28 * 23] == 0) while the LOD 0 group 3 is open: the game's draw returns, nothing drawn
          const size_t closed = floor_count(lod, &un);
          Check(closed == 0, "latch.floor_lod_group", "seats " + std::to_string(closed) + " (the LOD 0 group's stream was used)");
          lod.U(fm::M + 628 + 28 * 23, 1), lod.U(fm::M + 4 * (153 + 7 * 23), 0x9E010000u), lod.U(0x9E010000u, 0x9E010100u);   // group 23 open
          crowd::LatchSnapshot gx;
          gx.node = fm::N;
          crowd::latch::ReadSnapshot(lod, gx);
          size_t n1 = 0;
          bool all_lod1 = true;
          for (const auto& m : gx.members)
            if (m.floor) ++n1, all_lod1 = all_lod1 && m.lod == 1 && m.block == 3;
          Check(lod0 > 0 && n1 == lod0 && all_lod1 && gx.unread_by[crowd::kUrFloorLod] == 0, "latch.floor_lod_nonzero", "seats " + std::to_string(n1) + "/" + std::to_string(lod0) + " unread " + std::to_string(gx.unread));
          Forge lod3 = fl;
          lod3.U(fm::N + crowd::latch::kNodeLodWord, 3);   // past the 3 x 20 groups: not modelled
          crowd::LatchSnapshot g3;
          g3.node = fm::N;
          crowd::latch::ReadSnapshot(lod3, g3);
          Check(g3.unread_by[crowd::kUrFloorLod] == 1, "latch.floor_lod_past_groups", "floor_lod " + std::to_string(g3.unread_by[crowd::kUrFloorLod]));
        }
        Forge cap = fl;
        cap.U(kFloorObj + 20, 5000);
        Check(floor_count(cap, &un) == 0 && un >= 2, "latch.floor_count_cap");
        Forge nul = fl;
        nul.U(kFloorObj + 64, 0);
        Check(floor_count(nul, &un) == 0 && un >= 2, "latch.floor_null_element");
        Forge grp = fl;
        grp.U(fm::M + 628 + 28 * 3, 0);
        Check(floor_count(grp, &un) == 0 && un == 1, "latch.floor_group_condition");   // the game's floor draw returns: nothing drawn, nothing unread (the single person still counts)
        Forge grp2 = fl;
        grp2.U(0x9E000000u, 0);
        Check(floor_count(grp2) == 0, "latch.floor_stream_null");
        Forge ty = fl;
        H8(ty, kFloorElems + 176 * 0 + 23, 9);
        const size_t with = floor_count(fl), without = floor_count(ty);
        Check(without == with, "latch.floor_type_byte", std::to_string(with) + "/" + std::to_string(without));   // S103 C1 review fix 3: the game reads no type byte: the seat is drawn as the group's type
      }
      Check(g.unread == 1, "latch.floor_other_objects", "unread " + std::to_string(g.unread));
      {   // review items 4, 5, 7 on the reader
        Forge pend = g_forge;
        pend.U(fm::M + crowd::mgr::kRequestOff, 1);
        crowd::LatchSnapshot gp;
        Check(crowd::latch::ReadSnapshot(pend, gp) == crowd::latch::Why::ClassifyPending, "latch.pending_declines");
        gp.Clear();
        gp.served = true;
        Check(crowd::latch::ReadSnapshot(pend, gp) == crowd::latch::Why::None && !gp.members.empty(), "latch.served_not_a_decline");
        Forge nopass = pend;
        nopass.U(fm::M + crowd::latch::kPassGateOff0, 0);
        gp.Clear();
        gp.served = false;
        Check(crowd::latch::ReadSnapshot(nopass, gp) == crowd::latch::Why::None, "latch.pending_only_matters_with_pass");
        Forge wr = g_forge;
        wr.U(fm::M + crowd::latch::kWriteRingOffset, 0xFFFFFFFFu);
        gp.Clear();
        gp.want_palettes = true;
        Check(crowd::latch::ReadSnapshot(wr, gp) == crowd::latch::Why::NoPalette, "latch.write_ring_needed_on_mirror");
        gp.Clear();
        gp.want_palettes = false;
        Check(crowd::latch::ReadSnapshot(wr, gp) == crowd::latch::Why::None && !gp.members.empty(), "latch.write_ring_not_needed_on_ours");
        // the sine table is sanity-checked before it is cached (the boot's sub_82C32720 may not have run)
        std::vector<float> saved_tbl = crowd::latch::SinCache();
        crowd::latch::SinCache().clear();
        Forge badt = g_forge;
        badt.F(crowd::mgr::kSinTable + 4 * 16383, 0.f);
        gp.Clear();
        gp.want_palettes = true;
        const bool declined = crowd::latch::ReadSnapshot(badt, gp) == crowd::latch::Why::Unreadable && crowd::latch::SinCache().empty();
        Check(declined, "latch.sine_sanity");
        crowd::latch::SinCache() = saved_tbl;
      }   // the visible single-person object
      fl.U(crowd::latch::kPhaseWord, 4);
      g.Clear();
      crowd::latch::ReadSnapshot(fl, g);
      bool ok10 = false;
      for (const auto& m : g.members) if (m.floor && m.index == 0) ok10 = WorldsNear(m, FloorOracle(0, 10.f));
      Check(ok10, "latch.floor_f31_phase4");
      fl.U(crowd::latch::kPhaseWord, 0);
      fl.U(fm::M + crowd::latch::kManagerFloorFlagMode, 1);   // flag mode on: 0x8000 hides, 0x1000 does not
      g.Clear();
      crowd::latch::ReadSnapshot(fl, g);
      size_t n = 0; bool has1 = false, has3 = false;
      for (const auto& m : g.members) if (m.floor) { ++n, has1 |= m.index == 1, has3 |= m.index == 3; }
      Check(n == 5 && has1 && !has3, "latch.floor_flag_mode", "n " + std::to_string(n));
      Forge nonode = g_forge;
      ForgeFloor(nonode, 0.f);
      g.Clear();
      g.node = 0;
      crowd::latch::ReadSnapshot(nonode, g);   // node unknown: no floor read
      bool any = false;
      for (const auto& m : g.members) any |= m.floor;
      Check(!any, "latch.floor_needs_node");
    }
    {   // the signs and foam hands (review of D3b: nobody drew them from the record; this is the READER, CROWD_PROPS_LATCH.md)
      Forge pf = g_forge;
      ForgeProps(pf);
      crowd::LatchSnapshot g;
      g.node = fm::N;
      crowd::latch::ReadSnapshot(pf, g);
      bool okw = g.props.size() == 2 && PropNear(g.props[0], PropOracle(0, 0, false)) && PropNear(g.props[1], PropOracle(2, 1, true));
      Check(okw, "latch.prop_world", "props " + std::to_string(g.props.size()));
      Check(g.props.size() == 2, "latch.prop_flags", std::to_string(g.props.size()));   // the object without 0x4000 is skipped
      Forge pf2 = pf;
      pf2.U(0x9A000000u + 1184, 12345);   // another variant: neither a sign nor a foam hand
      g.Clear();
      g.node = fm::N;
      crowd::latch::ReadSnapshot(pf2, g);
      Check(g.props.size() == 1 && g.props[0].site == 2, "latch.prop_variant");
      Forge free2 = pf;   // pass 2: the free props
      free2.U(kPropThis + 596, 1);
      const uint32_t base = kPropThis + 608;
      H8(free2, base + 6240 + 3, 1), H8(free2, base + 6240 + 7, 2), H8(free2, base + 6240 + 8, 3);
      for (uint32_t j : {3u, 7u, 8u})
        for (uint32_t q = 0; q < 16; ++q) free2.F(base + 64 * j + 4 * q, float(100 * j + q));
      g.Clear();
      g.node = fm::N;
      crowd::latch::ReadSnapshot(free2, g);
      bool okf = g.props.size() == 2 && g.props[0].site == 1 && g.props[0].index == 3 && g.props[1].site == 3 && g.props[1].index == 7;
      for (int r = 0; okf && r < 3; ++r)
        for (int k = 0; k < 4; ++k) okf = okf && g.props[0].c[r * 4 + k] == float(300 + k * 4 + r);   // c[r*4+k] = row_k[r]
      Check(okf, "latch.prop_free", "props " + std::to_string(g.props.size()));
      // ---- S103 D5: the game's gates (CROWD_PROPS_GATES.md). Each hides the prop (a row with `hidden`), never drops it.
      auto read_props = [&](const Forge& fg) {
        crowd::LatchSnapshot gs;
        gs.node = fm::N;
        crowd::latch::ReadSnapshot(fg, gs);
        return gs.props;
      };
      {
        const auto open = read_props(pf);
        Check(open.size() == 2 && open[0].hidden == 0 && open[1].hidden == 0 && open[0].variant == 2 && open[1].variant == 3 && open[0].picture == kSignKey && open[1].picture == kHandKey, "latch.prop_gates_open",
              open.size() == 2 ? std::to_string(open[0].hidden) + "/" + std::to_string(open[1].hidden) : "n");
        Forge g1 = pf;
        g1.U(kSignCount, 0);   // the signs' mesh has no index count: sub_82518950 returns (0x825189C0)
        auto r = read_props(g1);
        Check(r.size() == 2 && r[0].site == 0 && r[0].hidden == crowd::kPhSignGlobal && r[1].hidden == 0, "latch.prop_sign_global", r.size() == 2 ? std::to_string(r[0].hidden) + "/" + std::to_string(r[1].hidden) : "n");
        Forge g2 = pf;
        g2.U(fm::M + 4 * (0 + 26049), 0);   // the ring entry is missing (0x825189D4)
        r = read_props(g2);
        Check(r.size() == 2 && r[0].hidden == crowd::kPhSignGlobal, "latch.prop_sign_ring");
        Forge g4 = pf;
        g4.U(kHandleBegin + 8 * 7 + 4, 0);   // the sign's key resolves to 0 (sub_82294F20 result word, 0x82518BE0)
        r = read_props(g4);
        Check(r.size() == 2 && r[0].hidden == crowd::kPhTexture && r[1].hidden == 0, "latch.prop_texture", r.size() == 2 ? std::to_string(r[0].hidden) + "/" + std::to_string(r[1].hidden) : "n");
        Forge g5 = pf;
        g5.U(kHandleBegin + 8 * 9, 0x00040019u);   // the table entry's key differs (0x82294F70: bne -> 0)
        r = read_props(g5);
        Check(r.size() == 2 && r[0].hidden == 0 && r[1].hidden == crowd::kPhTexture, "latch.prop_texture_key_mismatch");
        Forge g6 = pf;
        g6.U(kHandleTab + 16, kHandleBegin + 8 * 9);   // 9 entries: the hand's index 9 is out of range (0x82294F4C-50), the sign's index 7 is in
        r = read_props(g6);
        Check(r.size() == 2 && r[0].hidden == 0 && r[1].hidden == crowd::kPhTexture, "latch.prop_texture_range");
        Forge g7 = pf;
        g7.U(kHandCount, 0);   // the foam hand's mesh has no index count (0x825190A0): the game LEAVES the function at the FIRST object whose type b (byte elt+19) lacks it, before any type test: object 0 (a sign holder, b 3) is that object
        r = read_props(g7);
        Check(r.size() == 2 && r[0].hidden == 0 && r[1].hidden == crowd::kPhExited, "latch.prop_hand_resource", r.size() == 2 ? std::to_string(r[1].hidden) : "n");
        // the exit: object 1 (type b 3) is fine, a second foam hand whose type (b 4) has no resources makes the game leave the function: it and every later hand are hidden, the signs are not
        Forge ex = pf;
        for (uint32_t j : {3u, 4u}) {
          const uint32_t obj = 0x9C000000u + 0x10000u * j, elt = obj + 0x1000u, fp = obj + 0x2000u, mat = obj + 0x3000u;
          ex.U(kPropThis + 144 + 4 * j, obj), ex.U(obj + 64, elt), ex.U(elt + 160, fp), ex.U(elt + 152, mat);
          H8(ex, elt + 23, 6), H8(ex, elt + 18, 2), H8(ex, elt + 19, j == 3 ? 4 : 3);
          H16(ex, fp, 0x4000u);
          for (uint32_t q = 0; q < 16; ++q) ex.F(mat + 4 * q, 1.f + float(q));
        }
        r = read_props(ex);
        Check(r.size() == 4 && r[0].hidden == 0 && r[1].site == 2 && r[1].hidden == 0 && r[2].site == 2 && r[2].index == 3 && r[2].hidden == crowd::kPhResource && r[3].index == 4 && r[3].hidden == crowd::kPhExited, "latch.prop_hand_exit",
              r.size() == 4 ? std::to_string(r[1].hidden) + "/" + std::to_string(r[2].hidden) + "/" + std::to_string(r[3].hidden) : "n " + std::to_string(r.size()));
        // the distance test (0x82518CA4-0x82518CD4): only when [0x836F3940] == 1; d2 < f31 drops; eye = the composer's copy when [0x83929A68] != 0
        const crowd::PropObs s0 = PropOracle(0, 0, false), h1 = PropOracle(2, 1, true);   // translation = c[3], c[11]
        Forge dn = pf;
        dn.F(0x8201C26Cu, 25.f);                                  // f31
        dn.U(0x836F3940u, 1);                                     // the match phase
        dn.U(0x83929A68u, 0x9F100000u), dn.U(0x836F3AA8u, 0x9F200000u);   // the composer's copy [0x836F3AA8] + 1488: identity rotation, translation -eye (eye = -(t . R), ReadSceneCamera)
        dn.U(0x83929A40u, 0x9F300000u), dn.F(0x9F300000u + 592, s0.c[3] + 500.f);   // the HUD camera slot 0 must NOT be the eye (review 2): far away, would show everything
        for (int q = 0; q < 16; ++q) dn.F(0x9F200000u + 1488 + 4 * q, (q % 5 == 0) ? 1.f : 0.f);
        dn.F(0x9F200000u + 1488 + 48, -s0.c[3]), dn.F(0x9F200000u + 1488 + 56, -(s0.c[11] + 6.f));   // the sign is 6 units away (d2 36 is not < 25: shown)
        r = read_props(dn);
        const float hdx = s0.c[3] - h1.c[3], hdz = (s0.c[11] + 6.f) - h1.c[11];
        const bool hand_near = hdx * hdx + hdz * hdz < 25.f;
        Check(r.size() == 2 && r[0].hidden == 0 && (r[1].hidden != 0) == hand_near, "latch.prop_distance_far_shown");
        dn.F(0x9F200000u + 1488 + 48, -s0.c[3]), dn.F(0x9F200000u + 1488 + 56, -(s0.c[11] + 4.9f));   // d2 24.01 < 25: dropped
        r = read_props(dn);
        Check(r.size() == 2 && r[0].hidden == crowd::kPhDistance, "latch.prop_distance", r.size() == 2 ? std::to_string(r[0].hidden) : "n");
        {   // S110 CUTCROWD: the eye comes from the block the game handed to its renderer ([[0x83929A68] + 256] + 240, a block of the manager [0x836F3AA8]), not +1488 (here 4.9 units: would drop)
          Forge dh = dn;
          const uint32_t blk = 0x9F200000u + 48;
          dh.U(0x9F100000u + 256, blk);
          for (int q = 0; q < 16; ++q) dh.F(blk + 240 + 4 * q, (q % 5 == 0) ? 1.f : 0.f);
          dh.F(blk + 240 + 48, -s0.c[3]), dh.F(blk + 240 + 56, -(s0.c[11] + 6.f));   // the handed eye: 6 units away (shown)
          dh.F(0x82004D70u, 0.017453292f), dh.F(0x9F200000u + 1552, 50.f), dh.F(blk + 512, float(double(50.f) * double(0.017453292f)));
          r = read_props(dh);
          Check(r.size() == 2 && r[0].hidden == 0, "latch.prop_distance_handed_eye", r.size() == 2 ? std::to_string(r[0].hidden) : "n");
        }
        Forge dp0 = dn;
        dp0.U(0x836F3940u, 4);   // another phase: no test
        r = read_props(dp0);
        Check(r.size() == 2 && r[0].hidden == 0, "latch.prop_distance_phase");
        Forge dnc = dn;
        dnc.U(0x83929A68u, 0);   // no camera enabled: the default eye (the constants at 0x8206F2D0) is used, not the composer's
        dnc.F(0x8206F2D0u, s0.c[3] + 1.f), dnc.F(0x8206F2D0u + 8, s0.c[11] + 1.f);
        r = read_props(dnc);
        Check(r.size() == 2 && r[0].hidden == crowd::kPhDistance, "latch.prop_distance_default_eye");
        // pass 2: no distance test; the texture gate (style byte) and the hands' resources apply per entry
        Forge fr = free2;
        fr.U(0x836F3940u, 1), fr.F(0x8201C26Cu, 1e9f);   // a huge f31 would drop everything in pass 1
        H8(fr, base + 6144 + 3, 2), H8(fr, base + 6144 + 7, 3), H8(fr, base + 6144 + 8, 3);
        r = read_props(fr);
        Check(r.size() == 2 && r[0].site == 1 && r[0].hidden == 0 && r[1].site == 3 && r[1].hidden == 0 && r[0].variant == 2, "latch.prop_free_gates_open", r.size() == 2 ? std::to_string(r[0].hidden) + "/" + std::to_string(r[1].hidden) : "n");
        Forge fr2 = fr;
        H8(fr2, base + 6144 + 3, 5);   // a style byte with no key: tab + 8 * 5 is zero -> the handle table's entry 0 is empty
        H8(fr2, base + 6144 + 7, 4);   // a hand type with no resources
        r = read_props(fr2);
        Check(r.size() == 2 && r[0].hidden == crowd::kPhTexture && r[1].hidden == crowd::kPhResource, "latch.prop_free_gates", r.size() == 2 ? std::to_string(r[0].hidden) + "/" + std::to_string(r[1].hidden) : "n");
      }
    }

    {   // S103 D5: the single people (sub_8276E7B0): CROWD_PROPS_GATES.md 4 / 7. Literals from the listing: block M + 2304 + 1408 (2 (slot - 20) + (M[0] == 0)), world table M + 64 (2 (slot - 20 + 722) + z).
      constexpr uint32_t kObj = 0x9E500000u, kElt = 0x9E501000u, kFp = 0x9E502000u, kMat = 0x9E503000u, kArr = 0x9E504000u, kEnt = 0x9E505000u, kFwd = 0x9E506000u;
      Forge sf = g_forge;
      sf.U(fm::N + 32 + 4 * 5, kObj);   // slot 5 of the node's 52 words
      sf.U(kObj, 0x8202B8DCu), sf.U(kObj + 24, 21), sf.U(kObj + 28, 7), sf.U(kObj + 64, kElt);
      H16(sf, kObj + 32, 2);                         // the bone index
      sf.U(kElt + 160, kFp), sf.U(kElt + 152, kMat), sf.F(kElt + 116, 0.5f);
      H16(sf, kFp, 0x4000u);
      for (uint32_t q = 0; q < 16; ++q) sf.F(kMat + 4 * q, 100.f + float(q));
      sf.U(fm::M + 16, kArr), sf.U(kArr + 4 * 7, kEnt), sf.U(kEnt, crowd::latch::kSkinModelVtable), sf.U(kEnt + 296, 1), sf.U(kEnt + 300, 3);   // the entity (a CSkinModel), 3 nodes (sub_82BD3AB8)
      sf.U(fm::M + 0, 0);                            // M[0] == 0 -> parity z = 1
      const uint32_t blk = fm::M + 2304 + 1408 * (2 * 1 + 1);
      for (uint32_t n = 0; n < 3; ++n)
        for (uint32_t q = 0; q < 16; ++q) sf.F(blk + 64 * n + 4 * q, float(10 * n) + 0.25f * float(q) + (q % 5 == 0 ? 1.f : 0.f));
      auto rd_full = [&](const Forge& fg, uint32_t cam = kSingleCam) {   // S103 C1: the draw thread's camera noted (a fixture camera that shows every holder near the origin)
        Forge f2 = fg;
        ForgeSingleCamera(f2);
        crowd::LatchSnapshot gs;
        gs.node = fm::N;
        gs.screen_cam = cam;
        crowd::latch::ReadSnapshot(f2, gs);
        return gs;
      };
      auto rd = [&](const Forge& fg) { return rd_full(fg).singles; };
      auto s = rd(sf);
      bool ok = s.size() == 1 && s[0].hidden == 0 && s[0].world_kind == 1 && s[0].nodes == 3 && s[0].slot == 21 && s[0].index == 7 && s[0].palette.size() == 48;
      if (ok) {   // independent hand calc (double): bone 2 x RotY(-0.5) with m00 = cos, m02 = -sin, m20 = sin, m22 = cos
        const double cs = std::cos(-0.5), sn = std::sin(-0.5);
        const double R[4][4] = {{cs, 0, -sn, 0}, {0, 1, 0, 0}, {sn, 0, cs, 0}, {0, 0, 0, 1}};
        for (int i = 0; ok && i < 4; ++i)
          for (int k = 0; ok && k < 4; ++k) {
            double acc = 0;
            for (int q = 0; q < 4; ++q) acc += double(20.f + 0.25f * float(i * 4 + q) + ((i * 4 + q) % 5 == 0 ? 1.f : 0.f)) * R[q][k];
            ok = std::fabs(double(s[0].palette[32 + i * 4 + k]) - acc) < 1e-4;
          }
        for (int k = 0; ok && k < 16; ++k) ok = s[0].palette[k] == 0.25f * float(k) + (k % 5 == 0 ? 1.f : 0.f);   // bone 0 untouched
        for (int r2 = 0; ok && r2 < 3; ++r2)
          for (int k = 0; ok && k < 4; ++k) ok = s[0].c[r2 * 4 + k] == 100.f + float(k * 4 + r2);   // c[r*4+k] = W[k][r]
      }
      Check(ok, "latch.single_palette", s.empty() ? "none" : std::to_string(s[0].palette.size()));
      Forge s1 = sf;
      H16(s1, kFp, 0x1000u);   // [M+248] == 0: 0x1000 hides
      s = rd(s1);
      Check(s.size() == 1 && s[0].hidden == crowd::kShFlags && s[0].palette.empty(), "latch.single_hidden_1000");
      H16(s1, kFp, 0x8000u | 0x4000u);   // 0x8000 does not hide while [M+248] == 0 ...
      s = rd(s1);
      Check(s.size() == 1 && s[0].hidden == 0, "latch.single_8000_open");
      s1.U(fm::M + 248, 1);              // ... and does when [M+248] != 0 (and 0x1000 then does not)
      s = rd(s1);
      Check(s.size() == 1 && s[0].hidden == crowd::kShFlags, "latch.single_hidden_8000");
      H16(s1, kFp, 0x1000u | 0x4000u);
      s = rd(s1);
      Check(s.size() == 1 && s[0].hidden == 0, "latch.single_flag_mode");
      Forge s2 = sf;
      s2.U(kArr + 4 * 7, 0);   // sub_82507018 gives 0: the game returns
      s = rd(s2);
      Check(s.size() == 1 && s[0].hidden == crowd::kShNoEntity, "latch.single_no_entity");
      Forge s3 = sf;
      s3.U(kObj + 28, 60);     // index out of the array (>= 60)
      s = rd(s3);
      Check(s.size() == 1 && s[0].hidden == crowd::kShNoEntity, "latch.single_index_range");
      Forge s4 = sf;
      s4.U(kEnt + 296, 0), s4.U(kEnt + 220, kFwd), s4.U(kFwd, crowd::latch::kSkinModelVtable), s4.U(kFwd + 296, 1), s4.U(kFwd + 300, 2);   // the getter forwards along +220 to a CSkinModel (vtable 0x8206AF90)
      s = rd(s4);
      Check(s.size() == 1 && s[0].nodes == 2 && s[0].palette.size() == 32, "latch.single_node_forward");
      {   // S103 C1 review fix 5: the ENTITY's own class: the draw calls its vtable +8 (0x8276E9CC); a foreign class's +296 / +300 are not read
        Forge se = sf;
        se.U(kEnt, 0x82001234u);
        se.bad.insert(kEnt + 296), se.bad.insert(kEnt + 300);   // reading them would be a hole: the check must come first
        const crowd::LatchSnapshot ge = rd_full(se);
        Check(ge.singles.size() == 1 && ge.singles[0].hidden == crowd::kShForward && ge.unread_by[crowd::kUrForward] == 1 && ge.soft[crowd::latch::kSoftSingles] == 0, "latch.single_entity_class",
              "singles " + std::to_string(ge.singles.size()) + " holes " + std::to_string(ge.soft[crowd::latch::kSoftSingles]));
      }
      {   // S103 C1 (R15 item 1, the run-32 re-find): +220 is an object of ANOTHER class whose own +220 holds text ('ydef' = 0x79646566); the reader must not read through it (the fault pairs at 0x7964668E / 0x79646642)
        constexpr uint32_t kYdef = 0x79646566u;
        Forge sf2 = sf;
        sf2.U(kEnt + 296, 0), sf2.U(kEnt + 220, kFwd), sf2.U(kFwd, 0x82001234u), sf2.U(kFwd + 220, kYdef);   // a foreign vtable word; [kFwd + 296] = 0
        sf2.bad.insert(kYdef + 296), sf2.bad.insert(kYdef + 220);   // the two addresses run 32 faulted at (X + 296 / X + 220 with X = 'ydef')
        const crowd::LatchSnapshot g2 = rd_full(sf2);
        const bool fw = g2.singles.size() == 1 && g2.singles[0].hidden == crowd::kShForward && g2.unread_by[crowd::kUrForward] == 1 && g2.unread == 1 && g2.soft[crowd::latch::kSoftSingles] == 0;
        Check(fw, "latch.single_forward_foreign", "singles " + std::to_string(g2.singles.size()) + " hidden " + (g2.singles.empty() ? std::string("-") : std::to_string(g2.singles[0].hidden)) + " unread " + std::to_string(g2.unread) +
                                                    " holes " + std::to_string(g2.soft[crowd::latch::kSoftSingles]) + "@0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%08X", g2.soft_addr[crowd::latch::kSoftSingles]); return std::string(b); }());
      }
      Forge s5 = sf;
      s5.U(kEnt + 296, 0);     // no table, no forward: -1
      s = rd(s5);
      Check(s.size() == 1 && s[0].hidden == crowd::kShNoNodes, "latch.single_no_nodes");
      Forge s6 = sf;
      H16(s6, kFp, 0u);        // no 0x2000 / 0x4000: the manager's table (byte elt+25 < 128)
      for (uint32_t q = 0; q < 16; ++q) s6.F(fm::M + 64 * (2 * (1 + 722) + 1) + 4 * q, 500.f + float(q));
      s = rd(s6);
      Check(s.size() == 1 && s[0].world_kind == 2 && s[0].c[0] == 500.f && s[0].c[3] == 500.f + 12.f, "latch.single_table_world");
      H8(s6, kElt + 25, 200);
      s = rd(s6);
      Check(s.size() == 1 && s[0].world_kind == 3 && s[0].hidden == 0, "latch.single_world_unsupported");
      Forge s7 = sf;
      s7.U(fm::M + 0, 1);      // M[0] != 0 -> z = 0: the other buffer
      for (uint32_t n = 0; n < 3; ++n)
        for (uint32_t q = 0; q < 16; ++q) s7.F(fm::M + 2304 + 1408 * (2 * 1 + 0) + 64 * n + 4 * q, 900.f + float(n));
      s = rd(s7);
      Check(s.size() == 1 && s[0].palette[0] == 900.f, "latch.single_parity", s.empty() ? "none" : std::to_string(s[0].palette[0]));
      {   // S103 D6: the pose the game makes from the locals (CROWD_PROPS_GATES.md 10): chain by the bone table, skin = inverse bind x chain, against a hand oracle in double
        Forge sk = g_forge;
        ForgeSkinned(sk);
        s = rd(sk);
        double want[3][16];
        SkinOracle(want);
        bool oks = s.size() == 1 && s[0].hidden == 0 && s[0].bind_ok && s[0].type == 7 && s[0].skin.size() == 48;
        double worst = 0;
        for (int k = 0; oks && k < 3; ++k)
          for (int q = 0; q < 16; ++q) worst = std::max(worst, std::fabs(double(s[0].skin[size_t(k) * 16 + size_t(q)]) - want[k][q]) / (1.0 + std::fabs(want[k][q])));
        Check(oks && worst < 2e-5, "latch.single_skin", oks ? "worst " + std::to_string(worst) : "not read");
        Forge sk2 = sk;
        sk2.U(kSkEnt + 224, 0);   // no inverse bind: not modelled (hidden stays 0, bind_ok false)
        s = rd(sk2);
        Check(s.size() == 1 && s[0].hidden == 0 && !s[0].bind_ok, "latch.single_no_bind");
        {   // S103 SN: the data-object form (run 34: 52396 shown holders were `forward`): [x+296] == 0, [x+220] = a 0x8206C7A0 object, d = [y+12], count [d+24], table [d+32] (sub_82C27620 / sub_82C27640)
          constexpr uint32_t kDataObj = 0x9E507000u, kDataBlk = 0x9E508000u;
          Forge sd = sk;
          sd.U(kSkEnt + 296, 0), sd.U(kSkEnt + 300, 0), sd.U(kSkEnt + 220, kDataObj);
          sd.U(kDataObj, crowd::latch::kSkinDataVtable), sd.U(kDataObj + 12, kDataBlk), sd.U(kDataBlk + 24, 3), sd.U(kDataBlk + 32, kSkTab);
          const crowd::LatchSnapshot gd = rd_full(sd);
          bool okd = gd.singles.size() == 1 && gd.singles[0].hidden == 0 && gd.singles[0].nodes == 3 && gd.singles[0].bind_ok && gd.singles[0].skin.size() == 48 && gd.unread == 0 && gd.forward_vt == 0;
          double wd[3][16];
          SkinOracle(wd);
          double worstd = 0;
          for (int k2 = 0; okd && k2 < 3; ++k2)
            for (int q = 0; q < 16; ++q) worstd = std::max(worstd, std::fabs(double(gd.singles[0].skin[size_t(k2) * 16 + size_t(q)]) - wd[k2][q]) / (1.0 + std::fabs(wd[k2][q])));
          Check(okd && worstd < 2e-5, "latch.single_data_object", okd ? "worst " + std::to_string(worstd) : "not read (hidden " + (gd.singles.empty() ? std::string("-") : std::to_string(gd.singles[0].hidden)) + " unread " + std::to_string(gd.unread) + ")");
          Forge sd0 = sd;   // a null block: the game's 0-byte copy (nodes 0 -> no_nodes, not forward)
          sd0.U(kDataObj + 12, 0);
          const crowd::LatchSnapshot g0 = rd_full(sd0);
          Check(g0.singles.size() == 1 && g0.singles[0].hidden == crowd::kShNoNodes && g0.unread_by[crowd::kUrNoNodes] == 1 && g0.soft[crowd::latch::kSoftSingles] == 0, "latch.single_data_object_null");
          Forge sdf = sk;   // a foreign class keeps `forward` and names its vtable word
          sdf.U(kSkEnt + 296, 0), sdf.U(kSkEnt + 220, kDataObj), sdf.U(kDataObj, 0x82001234u);
          const crowd::LatchSnapshot gf = rd_full(sdf);
          Check(gf.singles.size() == 1 && gf.singles[0].hidden == crowd::kShForward && gf.forward_vt == 0x82001234u, "latch.single_forward_vt");
        }
        Forge sk3 = sk;
        sk3.U(kSkTab + 48 + 80 * 1, 2);   // a parent that is not an earlier node
        s = rd(sk3);
        Check(s.size() == 1 && !s[0].bind_ok, "latch.single_bad_parent");
        Forge sk4 = sk;
        sk4.U(kSkObj + 28, 27);   // S103 C1 (R15 item 3): entity index 27 = LOD 1 (27 / 20) of type 7 (27 % 20): drawn by the game like any other, the LOD 1 body
        sk4.U(kSkArr + 4 * 27, kSkEnt);
        const crowd::LatchSnapshot g4 = rd_full(sk4);
        s = g4.singles;
        Check(s.size() == 1 && s[0].hidden == 0 && s[0].bind_ok && s[0].type == 7 && s[0].lod == 1 && g4.unread == 0 && g4.lod_holders == 1, "latch.single_lod_map",
              s.empty() ? "none" : "hidden " + std::to_string(s[0].hidden) + " type " + std::to_string(s[0].type) + " lod " + std::to_string(s[0].lod) + " unread " + std::to_string(g4.unread));
        sk4.U(kSkObj + 28, 47);   // LOD 2 of type 7
        sk4.U(kSkArr + 4 * 47, kSkEnt);
        s = rd(sk4);
        Check(s.size() == 1 && s[0].type == 7 && s[0].lod == 2, "latch.single_lod2_map");
        // the singles' screen test (sub_8276E2C0), each exit on its own with the listing's literals
        const crowd::LatchSnapshot gsh = rd_full(sk);
        Check(gsh.singles.size() == 1 && gsh.singles[0].hidden == 0 && gsh.screen_hidden == 0 && gsh.unread == 0, "latch.single_screen_shows", "unread " + std::to_string(gsh.unread));
        Forge skx = sk;
        skx.F(kSkMat + 48, 5.0e6f);   // world row 3 x far to the side: px = 500 > 1 + r
        const crowd::LatchSnapshot gbx = rd_full(skx);
        Check(gbx.singles.size() == 1 && gbx.singles[0].hidden == crowd::kShScreen && gbx.screen_hidden == 1 && gbx.unread == 0, "latch.single_screen_box", "hidden " + (gbx.singles.empty() ? std::string("-") : std::to_string(gbx.singles[0].hidden)) + " unread " + std::to_string(gbx.unread));
        Forge sky = sk;
        sky.F(kSkMat + 52, 120.f + 9000.f);   // y: py = (y - 120) 1e-4 = 0.9; + ry 0.012 = 0.912 <= 1: shown; one step higher is not
        bool edge_in = rd_full(sky).singles.at(0).hidden == 0;
        sky.F(kSkMat + 52, 120.f + 9950.f);   // py + ry = 0.995 + 0.012 > 1: hidden
        bool edge_out = rd_full(sky).singles.at(0).hidden == crowd::kShScreen;
        Check(edge_in && edge_out, "latch.single_screen_top", std::to_string(edge_in) + "/" + std::to_string(edge_out));
        {   // S105 A2: the singles' screen test widened by 1 / k on a frame widened by k = 1.333 (g_screen_sx_bits): a holder at clip x 1.2 (past the game's box) is shown, the scale off = the game's test (hidden)
          Forge skw = sk;
          skw.F(kSkMat + 48, 1.2e4f);   // px = 1.2, r = 0.012 (w = 1): px - r > one: hidden by the game's own box
          SetScreenScale(1.f, 1.f);
          const bool off_hidden = rd_full(skw).singles.at(0).hidden == crowd::kShScreen;
          SetScreenScale(0.75f, 1.f);
          const bool wide_shown = rd_full(skw).singles.at(0).hidden == 0;
          SetScreenScale(1.f, 1.f);
          Check(off_hidden && wide_shown, "latch.single_screen_wide", std::string("scale off hidden ") + (off_hidden ? "1" : "0") + ", 1/k shown " + (wide_shown ? "1" : "0"));
        }
        {   // S103 C1 review fix 1: a NaN DRAWS (blt / bgt not taken, `ble` at 0x8276E404 = bf gt taken on unordered)
          Forge skq = sk;
          skq.U(kSkMat + 52, 0x7FC00000u);   // y = quiet NaN
          const crowd::LatchSnapshot gq = rd_full(skq);
          Check(gq.singles.size() == 1 && gq.singles[0].hidden == 0 && gq.singles[0].screen_verdict == 1 && gq.screen_hidden == 0, "latch.single_screen_nan_draws",
                gq.singles.empty() ? "none" : "hidden " + std::to_string(gq.singles[0].hidden));
        }
        {   // the near cut (blt at 0x8276E3AC: w < 0.0): Pj[2][3] = -1 makes w = 1 - z, z = 114 -> w < 0
          Forge skn = sk;
          ForgeSingleCamera(skn);
          skn.F(kSingleCam + 176 + 16 * 2 + 4 * 3, -1.f);
          crowd::LatchSnapshot gn;
          gn.node = fm::N, gn.screen_cam = kSingleCam;
          crowd::latch::ReadSnapshot(skn, gn);
          Check(gn.singles.size() == 1 && gn.singles[0].hidden == crowd::kShScreen, "latch.single_screen_near");
        }
        const crowd::LatchSnapshot gz = rd_full(sk, 0);   // sub_82BA7748 gave 0: the test returns 0, nothing drawn (0x8276E304)
        Check(gz.singles.size() == 1 && gz.singles[0].hidden == crowd::kShScreen && gz.unread == 0, "latch.single_screen_no_camera_hides");
        {
          Forge skg = sk;
          ForgeSingleCamera(skg);
          skg.U(crowd::latch::kSingleCamGate, 0);   // [0x83929A68] == 0: returns 0 (0x8276E2F4)
          crowd::LatchSnapshot gg;
          gg.node = fm::N, gg.screen_cam = kSingleCam;
          crowd::latch::ReadSnapshot(skg, gg);
          Check(gg.singles.size() == 1 && gg.singles[0].hidden == crowd::kShScreen, "latch.single_screen_gate_hides");
        }
        {   // the camera was never noted (a latch-only read): the verdict is unknown, the holder is read but stays unread `screen` and is no row
          crowd::LatchSnapshot gu;
          gu.node = fm::N;   // screen_cam left kScreenCamUnknown
          crowd::latch::ReadSnapshot(sk, gu);
          Check(gu.singles.size() == 1 && gu.singles[0].screen_unknown && gu.singles[0].bind_ok && gu.unread == 1 && gu.unread_by[crowd::kUrScreen] == 1, "latch.single_screen_unknown_unread", "unread " + std::to_string(gu.unread));
        }
        {   // per-reason census: a hole (a refused read inside the holder) is `hole`, the reasons sum to unread
          Forge skh = sk;
          skh.bad.insert(kSkInv);   // the inverse bind's first word refused
          const crowd::LatchSnapshot gh = rd_full(skh);
          uint32_t sum = 0;
          for (uint32_t r = 0; r < crowd::kUrCount; ++r) sum += gh.unread_by[r];
          Check(gh.unread == 1 && gh.unread_by[crowd::kUrHole] == 1 && sum == gh.unread && gh.singles.empty(), "latch.unread_by_hole", "unread " + std::to_string(gh.unread) + " hole " + std::to_string(gh.unread_by[crowd::kUrHole]) + " sum " + std::to_string(sum));
          Forge skb = sk;
          skb.U(kSkEnt + 224, 0);   // no inverse bind: `bind`
          const crowd::LatchSnapshot gb = rd_full(skb);
          Check(gb.unread == 1 && gb.unread_by[crowd::kUrBind] == 1, "latch.unread_by_bind");
        }
      }
    }
    bool okp = snap.palettes[0].size() == 5 * 16 && snap.palettes[20].size() == 3 * 16 && snap.palettes[40].empty() && snap.palettes[1].size() == 2 * 16 && snap.palettes[41].size() == 2 * 16 && snap.palettes[21].empty() && snap.palettes[2].size() == 2 * 16 && snap.palettes[22].size() == 2 * 16;
    for (uint32_t row = 0; okp && row < 5; ++row)
      for (uint32_t k = 0; k < 16; ++k) okp = okp && std::fabs(snap.palettes[0][row * 16 + k] - (1.f + float(row) + 0.01f * float(k))) < 1e-6f;
    for (uint32_t row = 0; okp && row < 3; ++row)
      for (uint32_t k = 0; k < 16; ++k) okp = okp && std::fabs(snap.palettes[20][row * 16 + k] - (10.f + float(row) + 0.01f * float(k))) < 1e-6f;
    Check(okp, "latch.palette_remap", "p0 " + std::to_string(snap.palettes[0].size()) + " p20 " + std::to_string(snap.palettes[20].size()) + " p40 " + std::to_string(snap.palettes[40].size()));
    Forge none = g_forge;
    none.U(crowd::mgr::kMgrGlobal, 0);
    crowd::LatchSnapshot s2;
    Check(crowd::latch::ReadSnapshot(none, s2) == crowd::latch::Why::NoManager, "latch.no_manager");
    Forge pend = g_forge;
    pend.U(fm::M + crowd::mgr::kRequestOff, 1);
    Check(crowd::latch::ReadSnapshot(pend, s2) == crowd::latch::Why::ClassifyPending, "latch.classify_pending");
    Forge bad = g_forge;
    bad.fail = true;
    Check(crowd::latch::ReadSnapshot(bad, s2) == crowd::latch::Why::Unreadable, "latch.unreadable");
    {   // ---- S103 D8: run 27 (8196 of 10596 reads declined `unreadable`): a refused read in an OPTIONAL part is a hole of that part, never a declined frame; a refused read in the lists is a decline that names its stage
      Forge fl = g_forge;
      ForgeFloor(fl, 10.f);
      crowd::LatchSnapshot base;
      base.node = fm::N;
      crowd::latch::ReadSnapshot(fl, base);
      const size_t base_members = base.members.size();
      const uint32_t base_unread = base.unread;
      // a node object of ANOTHER class (neither floor group nor holder) whose +64 is not a pointer (a float's bits): the floor loop reads [obj + 64] + 160 and the address is refused
      Forge odd = fl;
      const uint32_t kOddObj = 0x98800000u, kNotPtr = 0x3F800000u;
      odd.U(fm::N + crowd::latch::kNodeObjects + 4 * 3, kOddObj);
      odd.U(kOddObj, 0x8202BEEFu), odd.U(kOddObj + 64, kNotPtr);
      odd.bad.insert(kNotPtr + 160);
      crowd::LatchSnapshot g;
      g.node = fm::N;
      const crowd::latch::Why w = crowd::latch::ReadSnapshot(odd, g);
      Check(w == crowd::latch::Why::None && g.members.size() == base_members && g.unread == base_unread + 1 && g.soft[crowd::latch::kSoftFloor] == 1 && g.soft_addr[crowd::latch::kSoftFloor] == kNotPtr + 160 && !odd.fail,
            "latch.soft_floor_object", "why " + std::to_string(int(w)) + " members " + std::to_string(g.members.size()) + "/" + std::to_string(base_members) + " unread " + std::to_string(g.unread) + " soft " + std::to_string(g.soft[0]));
      // a sign object whose world matrix pointer is refused: THAT prop is a hole, the foam hand and the stands are read
      Forge pf = g_forge;
      ForgeProps(pf);
      crowd::LatchSnapshot gp0, gp1;
      gp0.node = fm::N, gp1.node = fm::N;
      crowd::latch::ReadSnapshot(pf, gp0);
      pf.bad.insert(0x9C000000u + 0x3000u);   // object 0's matrix (elt + 152 -> mat)
      const crowd::latch::Why wp = crowd::latch::ReadSnapshot(pf, gp1);
      Check(wp == crowd::latch::Why::None && gp0.props.size() == 2 && gp1.props.size() == 1 && gp1.props[0].site == 2 && gp1.soft[crowd::latch::kSoftProps] == 1 && gp1.unread == gp0.unread + 1 && gp1.members.size() == gp0.members.size(),
            "latch.soft_prop_object", "why " + std::to_string(int(wp)) + " props " + std::to_string(gp1.props.size()) + " soft " + std::to_string(gp1.soft[1]) + " unread " + std::to_string(gp1.unread) + "/" + std::to_string(gp0.unread));
      // a refused read in the stands' own lists declines, and says where
      Forge lists = g_forge;
      lists.bad.insert(fm::C);   // the counts pointer's table: [fm::C + 0] is type 0's count
      crowd::LatchSnapshot gl;
      const crowd::latch::Why wl = crowd::latch::ReadSnapshot(lists, gl);
      Check(wl == crowd::latch::Why::Unreadable && gl.fail_stage == crowd::latch::kStageLists && gl.fail_addr == fm::C, "latch.hard_stage_named", "why " + std::to_string(int(wl)) + " stage " + std::to_string(gl.fail_stage) + " addr " + std::to_string(gl.fail_addr));
      // the table ring the ENTRY saw is the one read, whatever the word says at the latch (the update side may have flipped it): ring 1's lists are empty here, so reading it would be BadPointer
      Forge flip = g_forge;
      flip.U(crowd::mgr::kRingIndex, 1);
      crowd::LatchSnapshot g0;
      crowd::latch::ReadSnapshot(g_forge, g0);
      crowd::LatchSnapshot gr;
      gr.entry_ring = 0;
      const crowd::latch::Why wr_ = crowd::latch::ReadSnapshot(flip, gr);
      Check(wr_ == crowd::latch::Why::None && gr.ring_moved && gr.members.size() == g0.members.size(),"latch.entry_ring_used", "why " + std::to_string(int(wr_)) + " moved " + std::to_string(gr.ring_moved) + " members " + std::to_string(gr.members.size()));
    }
  }
  // ---- L: the contributor with the latch reader. An OURS frame (the crowd entry skipped) gets a fresh snapshot from the reader; latch_partial unless the policy allows it.
  {
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);   // the hook on sub_82509CB8 keeps filling the rings on OURS frames (update side)
    c.NoteOursSkip();
    live::FrameInfo fi;
    fi.guest_frame = 100;
    SceneTables t;
    live::Counters cnt;
    c.Latch(fi);
    c.Emit(fi, t, cnt);
    Check(c.LatchCensus().installed == 1, "latch.ours_frame_installs");
    Check(t.objects.size() >= 10, "latch.ours_frame_objects", std::to_string(t.objects.size()));   // 5 stands + 5 cards
    Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::LatchPartial)] == 1, "latch.partial_no_stamp", "stamps " + std::to_string(g_stamps));
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);
    crowd::SnapPolicy ok_partial;
    ok_partial.latch_partial_ok = true;
    c.SetSnapPolicy(ok_partial);
    c.NoteOursSkip();
    Record(c, 101, true);
    Check(g_stamps == 1 && g_last_stamp == 100, "latch.partial_ok_stamps");
    Record(c, 102, true);   // no walk and no skip (a menu): no read; the crowd is empty and whole (the previous latch was active: armed)
    Check(c.LatchCensus().installed == 1 && c.LatchCensus().absent == 1 && g_stamps == 2, "latch.absent_is_empty_whole", "stamps " + std::to_string(g_stamps));
    // review F5: the FIRST active latch has no history: "not reached" proves nothing yet
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    Record(c, 100, true);
    Check(g_stamps == 0 && c.LatchCensus().absent_unarmed == 1, "latch.absent_needs_history", "stamps " + std::to_string(g_stamps));
    Record(c, 101, true);
    Check(g_stamps == 1 && c.LatchCensus().absent == 1, "latch.absent_armed_stamps");
    // review F5: under ObserverKnockOut the walk's snapshot is not a witness: no stamp
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    c.SetStampSink({&Active, &Stamp, []() { return true; }});
    Record(c, 100, true);
    Record(c, 101, true);
    Check(g_stamps == 0 && c.LatchCensus().knocked_out == 2 && c.LatchCensus().absent_unarmed == 0, "latch.knock_out_withholds");   // (item 8: not labelled a missing history)
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    g_mode_on = false;
    c.NoteOursSkip();
    Record(c, 103, true);
    Check(c.LatchCensus().installed == 0, "latch.off_costs_nothing");
    // a decline on an OURS frame: no crowd is drawn rather than the previous frame's repeated (review F6)
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);
    c.NoteOursSkip();
    Record(c, 100, true);   // installs 5 + 5 members
    const Forge saved = g_forge;
    g_forge.U(crowd::mgr::kMgrGlobal, 0);
    c.NoteOursSkip();
    SceneTables t2;
    live::Counters cnt2;
    live::FrameInfo fi2;
    fi2.guest_frame = 101;
    c.Latch(fi2);
    c.Emit(fi2, t2, cnt2);
    g_forge = saved;
    Check(c.LatchCensus().installed == 1 && c.LatchCensus().declined[1] == 1 && t2.objects.empty(), "latch.decline_clears", "objects " + std::to_string(t2.objects.size()));
  }
  // ---- N (S103 D7, run 26): the synchronous frame end (wwe2k14_rdr_tail_release off) must latch the crowd too, else the reader never reads (crowd_latch compared 0 for a whole match). Driven through the
  // real live_scene EndFrame, not Record(). Twin end_frame_no_latch = the pre-fix EndFrame (no latch).
  {
    struct NullSink : live::Sink {
      void OnRecord(const live::FrameInfo&, const SceneTables&) override {}
    };
    struct Probe : live::Contributor {   // a second, NON-crowd contributor: EndFrame must latch every contributor, not the crowd alone (the class fix)
      int latches = 0, emits = 0;
      const char* Name() const override { return "probe"; }
      void Emit(const live::FrameInfo&, SceneTables&, live::Counters&) override { ++emits; }
      bool Latch(const live::FrameInfo&) override { ++latches; return true; }
    };
    static NullSink sink;
    static Probe probe;
    static bool registered = false;
    if (!registered) live::Register(&c), live::Register(&probe), live::AddSink(&sink), registered = true;   // (the crowd is contributor 0, the probe 1)
    live::g_tail_plant = p == 9 ? 1 : p == 10 ? 3 : 0;   // twins: 9 = EndFrame latches nobody (the pre-fix path); 10 = only the crowd is latched (the probe, index 1, is skipped)
    probe.latches = probe.emits = 0;
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    auto walk = OracleStands();
    for (const auto& m : OracleCards()) walk.push_back(m);
    SetWalk(c, walk);
    live::EndFrame(100);
    const auto l = c.LatchCensus();
    live::g_tail_plant = 0;
    Check(l.compared == 1, "latch.endframe_latches", "compared " + std::to_string((unsigned long long)l.compared));
    Check(probe.latches == 1 && probe.emits == 1, "latch.endframe_latches_all", "probe latches " + std::to_string(probe.latches) + " emits " + std::to_string(probe.emits));
    Check(l.stands.equal + l.stands.almost == 1 && l.stands.differs == 0, "latch.endframe_compare_equal");
  }
  // ---- M: the mirror compare on a walked frame: the walk's snapshot stands, the reader's is compared with it
  {
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    auto walk = OracleStands();
    for (const auto& m : OracleCards()) walk.push_back(m);
    SetWalk(c, walk);
    Record(c, 100, true);
    const auto l = c.LatchCensus();
    Check(l.compared == 1 && l.installed == 0, "latch.mirror_compared");
    Check(l.stands.equal + l.stands.almost == 1 && l.stands.differs == 0, "latch.mirror_stands_equal", std::to_string((unsigned long long)l.stands.differs));
    Check(l.cards.equal + l.cards.almost == 1 && l.cards.differs == 0, "latch.mirror_cards_equal");
    Check(l.pal_equal == 7 && l.pal_differs == 0, "latch.mirror_palettes_equal", std::to_string((unsigned long long)l.pal_equal) + "/" + std::to_string((unsigned long long)l.pal_differs));
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    walk = OracleStands();
    walk[1].c[3] += 0.5f;   // the walk saw another world
    SetWalk(c, walk);
    Record(c, 100, true);
    Check(c.LatchCensus().stands.differs == 1, "latch.mirror_differs_counted");
  }
  // ---- N (review F2): read ring != write ring. The walk publishes palettes_[read ring]; the +608 read is the newest copy = the write ring's.
  {
    const Forge saved = g_forge;
    g_forge.U(fm::M + crowd::latch::kReadRingOffset, 1);
    g_forge.U(fm::M + crowd::latch::kWriteRingOffset, 0);
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);          // ring 0 = what +608 holds
    FeedPalettes(c, 1, 100.f);   // ring 1 = the set the draw reads: another pose
    c.NoteOursSkip();
    Record(c, 100, true);
    const auto& pub = c.PublishedPalettes();
    Check(c.LatchCensus().installed == 1 && pub[0].size() == 5 * 16 && std::fabs(pub[0][0] - 101.f) < 1e-4f, "latch.ring_installs_read_set", pub[0].empty() ? "empty" : std::to_string(pub[0][0]));
    // mirror frame: the reader's +608 read equals the WRITE ring's set (so the compare is meaningful), not the read ring's
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);
    FeedPalettes(c, 1, 100.f);
    auto walk = OracleStands();
    for (const auto& m : OracleCards()) walk.push_back(m);
    c.BeginDraw(1);   // the walk's draw reads ring 1
    for (const auto& m : walk) c.NoteMember(m);
    c.EndDraw();
    Record(c, 100, true);
    Check(c.LatchCensus().pal_equal == 7 && c.LatchCensus().pal_differs == 0, "latch.ring_compare_write_set", std::to_string((unsigned long long)c.LatchCensus().pal_equal) + "/" + std::to_string((unsigned long long)c.LatchCensus().pal_differs));
    // ---- S103 D9 (run 28: palettes 21 equal / 635619 differ): at the latch the game has flipped the rings (write ring 1 now); the newest set (+608) went into ring 0 = write ^ 1, and the walk's draw read ring 0
    g_forge.U(fm::M + crowd::latch::kReadRingOffset, 0);
    g_forge.U(fm::M + crowd::latch::kWriteRingOffset, 1);
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 1, 100.f);
    FeedPalettes(c, 0);   // the hook wrote ring 0 last: the newest set (what +608 holds)
    for (int frame = 0; frame < 2; ++frame) {   // the first frame fills the history (and proves nothing); the second compares
      c.NoteCrowdNode(fm::N);
      c.BeginDraw(0);
      for (const auto& m : walk) c.NoteMember(m);
      c.EndDraw();
      c.EntryRead();
      Record(c, 100 + frame, true);
    }
    Check(c.LatchCensus().pal_walk_eq == 7 && c.LatchCensus().pal_walk_diff == 0 && c.LatchCensus().pal_walk_other_diff == 7, "latch.palette_history_ring",
          std::to_string((unsigned long long)c.LatchCensus().pal_walk_eq) + "/" + std::to_string((unsigned long long)c.LatchCensus().pal_walk_diff) + " other " + std::to_string((unsigned long long)c.LatchCensus().pal_walk_other_diff));
    // ---- S103 D9 review 2: an entry snapshot is for ONE latch. Mode Off at the latch drops it; the next latch (mode on) must not consume it.
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);
    c.NoteOursSkip(fm::N);
    c.EntryRead();
    g_mode_on = false;
    Record(c, 100, true);
    g_mode_on = true;
    c.NoteOursSkip(fm::N);
    Record(c, 101, true);
    Check(c.LatchCensus().entry_used == 0 && c.LatchCensus().installed == 1, "latch.entry_snapshot_one_latch", "used " + std::to_string((unsigned long long)c.LatchCensus().entry_used) + " installed " + std::to_string((unsigned long long)c.LatchCensus().installed));
    g_forge = saved;
  }
  // ---- S103 C1 review fix 2: the walked-frame cross-check of the singles' screen test: the reader's verdict (entry camera noted) against the game's sub_8276E2C0 return for the same object
  {
    const Forge saved = g_forge;
    ForgeSkinned(g_forge);
    ForgeSingleCamera(g_forge);
    auto walked = [&](bool game_shown, bool note) {
      Fresh(c, p);
      c.SetLatchReader(&ForgeRead);
      c.NoteEntryCamera(kSingleCam);
      c.NoteCrowdNode(fm::N);
      c.BeginDraw(0);
      if (note) c.NoteScreenVerdict(kSkObj, game_shown);
      c.EndDraw();
      Record(c, 100, true);
      return c.LatchCensus();
    };
    const auto e1 = walked(true, true);
    Check(e1.screen_eq == 1 && e1.screen_diff == 0 && e1.screen_reader_only == 0 && e1.screen_game_only == 0, "latch.screen_check_eq", std::to_string(e1.screen_eq) + "/" + std::to_string(e1.screen_diff));
    const auto e2 = walked(false, true);
    Check(e2.screen_eq == 0 && e2.screen_diff == 1, "latch.screen_check_diff", std::to_string(e2.screen_eq) + "/" + std::to_string(e2.screen_diff));
    const auto e3 = walked(true, false);
    Check(e3.screen_reader_only == 1, "latch.screen_check_reader_only");
    std::string cs;
    c.Census(cs);
    Check(cs.find("screen_check eq 0 screen_diff 0 reader_only 1 game_only 0 (trust off)") != std::string::npos, "latch.screen_check_census", cs.substr(cs.find("screen_check") == std::string::npos ? 0 : cs.find("screen_check"), 90));
    g_forge = saved;
  }
  // ---- O: the floor people through the contributor. An OURS frame with the node known installs them; the object the reader could not model blocks the stamp (unmodelled). A mirror frame compares as a subset.
  {
    const Forge saved = g_forge;
    ForgeFloor(g_forge, 10.f);
    ForgeCamera(g_forge);
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);
    crowd::SnapPolicy ok_partial;
    ok_partial.latch_partial_ok = true;
    c.SetSnapPolicy(ok_partial);
    c.NoteOursSkip(fm::N);
    Record(c, 100, true);
    Check(c.LatchCensus().floor_installed == 3 && c.LatchCensus().floor_unread == 1, "latch.floor_installed", std::to_string((unsigned long long)c.LatchCensus().floor_installed));
    Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::Unmodelled)] == 1, "latch.floor_unread_blocks", "stamps " + std::to_string(g_stamps));
    {   // S103 C1: the census names what is left (the visible holder has no inverse bind: `bind`)
      std::string cs;
      c.Census(cs);
      const size_t at = cs.find(" unread_by [");
      Check(at != std::string::npos && cs.find("unread_by [hole 0 forward 0 no_nodes 0 world3 0 bind 1 screen 0 floor_lod 0 other 0] screen_hidden 0 lod_holders 0") != std::string::npos, "latch.census_unread_by",
            at == std::string::npos ? "no unread_by" : cs.substr(at, 120));
    }
    g_forge.U(0x98000000u + 0x1000u * 2 + 0x100u + 160, 0);   // hide the second single-person object too (its flags pointer -> none)
    H16(g_forge, 0x99000000u + 8, 0x1000u);
    g_forge.U(0x98000000u + 0x1000u * 2 + 0x100u + 160, 0x99000000u + 8);
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    FeedPalettes(c, 0);
    c.SetSnapPolicy(ok_partial);
    c.NoteOursSkip(fm::N);
    Record(c, 101, true);
    { std::string cs; c.Census(cs); Check(g_stamps == 1 && c.LatchCensus().floor_unread == 0, "latch.floor_whole_stamps", cs); }
    // a mirror frame: the walk drew seats 0 and 3 (seat 2 was off screen): a subset of the reader's set, worlds equal
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    auto walk = OracleStands();
    for (const auto& m : OracleCards()) walk.push_back(m);
    walk.push_back(FloorOracle(0, 0.f));
    walk.push_back(FloorOracle(3, 0.f));
    SetWalk(c, walk);
    c.NoteCrowdNode(fm::N);
    Record(c, 100, true);
    Check(c.LatchCensus().floor.equal + c.LatchCensus().floor.almost == 1 && c.LatchCensus().floor.differs == 0 && c.LatchCensus().floor_extra == 1, "latch.floor_mirror_subset",
          "diff " + std::to_string((unsigned long long)c.LatchCensus().floor.differs) + " extra " + std::to_string((unsigned long long)c.LatchCensus().floor_extra));
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    walk.back().c[3] += 1.f;
    SetWalk(c, walk);
    c.NoteCrowdNode(fm::N);
    Record(c, 100, true);
    Check(c.LatchCensus().floor.differs == 1, "latch.floor_mirror_differs");
    // review item 3: with the floor observer off the walk holds no seats: nothing is compared, `floor_unobserved` counts it
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    c.BeginDraw(0);
    for (const auto& m : OracleStands()) c.NoteMember(m);
    c.NoteUnobserved();
    c.EndDraw();
    c.NoteCrowdNode(fm::N);
    Record(c, 100, true);
    Check(c.LatchCensus().floor_unobserved == 1 && c.LatchCensus().floor.equal + c.LatchCensus().floor.almost + c.LatchCensus().floor.differs == 0, "latch.floor_unobserved_not_compared");
    g_forge = saved;
  }
  // ---- P (S103 D3c): the mirror compare of the signs / foam hands on a walked frame, and the Capture withheld by kCrowdOursWhole
  {
    const Forge saved = g_forge;
    ForgeProps(g_forge);
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    auto walk = OracleStands();
    for (const auto& m : OracleCards()) walk.push_back(m);
    SetWalk(c, walk, {PropOracle(0, 0, false), PropOracle(2, 1, true)});
    c.NoteCrowdNode(fm::N);
    Record(c, 100, true);
    Check(c.LatchCensus().props.equal + c.LatchCensus().props.almost == 1 && c.LatchCensus().props.differs == 0 && c.PublishedProps().size() == 2, "latch.prop_mirror", "diff " + std::to_string((unsigned long long)c.LatchCensus().props.differs));
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    auto wrong = PropOracle(2, 1, true);
    wrong.c[5] += 1.f;
    SetWalk(c, walk, {PropOracle(0, 0, false), wrong});
    c.NoteCrowdNode(fm::N);
    Record(c, 100, true);
    Check(c.LatchCensus().props.differs == 1, "latch.prop_mirror_differs");
    // ---- S103 D9: the ENTRY read. The update side runs between the entry and the latch and moves the in-place data (the sign's bone): the snapshot of the frame is what the entry saw
    Fresh(c, p);
    c.SetLatchReader(&ForgeRead);
    SetWalk(c, walk, {PropOracle(0, 0, false), PropOracle(2, 1, true)});
    c.NoteCrowdNode(fm::N);
    c.EntryRead();
    g_forge.F(0x9A200000u + 64 * 2 + 4 * 5, 99.f);   // the update ran: the sign bone moved
    Record(c, 100, true);
    Check(c.LatchCensus().entry_used == 1 && c.LatchCensus().props.equal + c.LatchCensus().props.almost == 1 && c.LatchCensus().props.differs == 0, "latch.entry_read_in_place",
          "used " + std::to_string((unsigned long long)c.LatchCensus().entry_used) + " eq " + std::to_string((unsigned long long)c.LatchCensus().props.equal) + " diff " + std::to_string((unsigned long long)c.LatchCensus().props.differs));
    g_forge.F(0x9A200000u + 64 * 2 + 4 * 5, 2.75f);   // (back to the forged value)
    g_forge = saved;

    // ---- S103 D5: mirror compare of the SHOWN set, and the record rows of a latch-built frame
    {
      Forge hid = g_forge;
      ForgeProps(hid);
      hid.U(kHandleBegin + 8 * 7 + 4, 0);   // the reader's sign is gated off (texture): the walk did not draw it either
      g_forge = hid;
      Fresh(c, p);
      c.SetLatchReader(&ForgeRead);
      SetWalk(c, walk, {PropOracle(2, 1, true)});
      c.NoteCrowdNode(fm::N);
      Record(c, 100, true);
      Check(c.LatchCensus().props.equal + c.LatchCensus().props.almost == 1 && c.LatchCensus().props.differs == 0 && c.LatchCensus().prop_extra == 1, "latch.prop_mirror_hidden_ignored",
            "diff " + std::to_string((unsigned long long)c.LatchCensus().props.differs) + " extra " + std::to_string((unsigned long long)c.LatchCensus().prop_extra));
      Fresh(c, p);
      c.SetLatchReader(&ForgeRead);
      SetWalk(c, walk, {PropOracle(0, 0, false), PropOracle(2, 1, true)});   // the walk DREW the sign the reader hid: the reader's gate is wrong
      c.NoteCrowdNode(fm::N);
      Record(c, 100, true);
      Check(c.LatchCensus().props.differs == 1, "latch.prop_mirror_gate_disagrees");
      g_forge = saved;
      ForgeProps(g_forge);
      Fresh(c, p);
      c.SetLatchReader(&ForgeRead);
      SetWalk(c, walk, {PropOracle(2, 1, true)});   // the reader shows a sign the walk did not draw: its gates are looser than the game's
      c.NoteCrowdNode(fm::N);
      Record(c, 100, true);
      Check(c.LatchCensus().props.differs == 1, "latch.prop_mirror_extra_shown_differs");
      g_forge = saved;
    }
    {   // the rows (an OURS frame): through the contributor and the writer API
      Forge fp = saved;
      ForgeProps(fp);
      auto emit_rows = [&](const Forge& fg, SceneTables& tb, live::Counters& cn, bool ours = true) {
        const Forge keep = g_forge;
        g_forge = fg;
        Fresh(c, p);
        c.SetLatchReader(&ForgeRead);
        ForgeSingleCamera(g_forge), c.NoteEntryCamera(kSingleCam);   // S103 C1: the draw thread's camera as the entry hook notes it (the holders' screen test runs)
        FeedPalettes(c, 0);
        crowd::SnapPolicy ok_partial;
        ok_partial.latch_partial_ok = true;
        c.SetSnapPolicy(ok_partial);
        if (ours) c.NoteOursSkip(fm::N);
        else {
          auto w2 = OracleStands();
          for (const auto& m : OracleCards()) w2.push_back(m);
          SetWalk(c, w2, {PropOracle(0, 0, false), PropOracle(2, 1, true)});
          c.NoteCrowdNode(fm::N);
        }
        live::FrameInfo fi;
        fi.guest_frame = 100;
        c.Latch(fi);
        c.Emit(fi, tb, cn);
        g_forge = keep;
      };
      SceneTables tb;
      live::Counters cn;
      emit_rows(fp, tb, cn);
      const crowd::PropObs s0 = PropOracle(0, 0, false), h1 = PropOracle(2, 1, true);
      bool okr = tb.crowd_props.size() == 2;
      if (okr) {
        const CrowdPropRow& a = tb.crowd_props[0];
        const CrowdPropRow& b = tb.crowd_props[1];
        okr = a.kind == 1 && b.kind == 2 && a.flags == 2 && b.flags == 2 && a.variant == 2 && b.variant == 3 && a.picture == kSignKey && b.picture == kHandKey;   // kCpSign 1 / kCpHand 2, kCpfBone 2
        for (int k = 0; okr && k < 12; ++k) okr = std::fabs(a.c[k] - s0.c[k]) < 1e-5f && std::fabs(b.c[k] - h1.c[k]) < 1e-5f;
      }
      Check(okr, "rows.props_appended", "rows " + std::to_string(tb.crowd_props.size()));
      {   // ---- S103 SG2 (run 37: signs never reached the record on walked frames): a WALKED frame whose read was used carries the reader's props, flagged kCpfInPlace, with the pictures of their keys
        SceneTables tw;
        live::Counters cw;
        emit_rows(fp, tw, cw, false);
        bool okw = tw.crowd_props.size() == 2;
        for (const CrowdPropRow& r : tw.crowd_props) okw = okw && (r.flags & kCpfInPlace) && (r.flags & ~uint32_t(kCpfInPlace)) == 2u;   // bone-held (2) + in place (8)
        if (okw) okw = tw.crowd_props[0].picture == kSignKey && tw.crowd_props[1].picture == kHandKey && tw.crowd_props[1].model_key == 41016 && tw.crowd_props[0].c[3] == tb.crowd_props[0].c[3];
        bool pics = tw.crowd_pictures.size() == tb.crowd_pictures.size();   // the same picture rows an OURS frame writes (its keys: kSignKey / kHandKey when the forged textures read)
        Check(okw && pics, "latch.walked_props_rows", "rows " + std::to_string(tw.crowd_props.size()) + " pictures " + std::to_string(tw.crowd_pictures.size()) + " (ours " + std::to_string(tb.crowd_pictures.size()) + ")");
        bool ours_clean = true;   // an OURS frame's rows never carry the flag (the stream does not draw them)
        for (const CrowdPropRow& r : tb.crowd_props) ours_clean = ours_clean && !(r.flags & kCpfInPlace);
        Check(ours_clean, "latch.ours_rows_not_in_place");
        Check(tw.crowd_singles.empty(), "latch.walked_holders_held_back", "singles " + std::to_string(tw.crowd_singles.size()));
      }
      {   // ---- S103 SG: the foam hand's mesh = 40999 + h of its type b (sub_8250B1E8 replicated; the forged slots' oracle is written by hand in ForgeHandSlots)
        crowd::LatchSnapshot gh;
        gh.node = fm::N;
        crowd::latch::ReadSnapshot(fp, gh);
        const bool map = gh.hand_map_ok && gh.hand_types == 4 && gh.hand_types_game == 4 && gh.hand_h[0] == 3 && gh.hand_h[1] == 12 && gh.hand_h[2] == 7 && gh.hand_h[3] == 17;
        Check(map, "latch.hand_map", "types " + std::to_string(gh.hand_types) + " game " + std::to_string(gh.hand_types_game) + " h " + std::to_string(gh.hand_h[0]) + "," + std::to_string(gh.hand_h[1]) + "," +
                                         std::to_string(gh.hand_h[2]) + "," + std::to_string(gh.hand_h[3]) + (gh.hand_map_ok ? "" : " (map not ok)"));
        Check(crowd::latch::HandAttireIdx(139, 0) == 1 && crowd::latch::HandAttireIdx(139, 8) == 0 && crowd::latch::HandAttireIdx(139, 4) == -1 && crowd::latch::HandAttireIdx(109, 6) == 1 &&
              crowd::latch::HandAttireIdx(109, 5) == 0 && crowd::latch::HandAttireIdx(344, 3) == 0 && crowd::latch::HandAttireIdx(344, 4) == -1 && crowd::latch::HandAttireIdx(101, 3) == -1 &&
              crowd::latch::HandAttireIdx(177, 3) == 0 && crowd::latch::HandAttireIdx(365, 1) == 1 && crowd::latch::HandAttireIdx(348, 1) == -1 && crowd::latch::HandAttireIdx(500, 9) == 0,
              "latch.hand_attire_table");   // sub_82506E30 by hand from its listing (v = digit + 1)
        bool okk = tb.crowd_props.size() == 2 && tb.crowd_props[0].model_key == 0 && tb.crowd_props[1].model_key == 41016;
        Check(okk, "rows.hand_mesh_key", "sign key " + std::to_string(tb.crowd_props.empty() ? 0u : tb.crowd_props[0].model_key) + " hand key " + std::to_string(tb.crowd_props.size() > 1 ? tb.crowd_props[1].model_key : 0u));
        Forge fh = fp;   // the hand's type is b = 1 (h 12): mesh 41011, not the 41016 every hand drew before
        {
          const uint32_t obj1 = 0x9C000000u + 0x10000u, elt1 = obj1 + 0x1000u;
          H8(fh, elt1 + 19, 1);
          ForgeHandType(fh, 1, kHandCount + 0x100, kHandVert + 0x100, kHandRes + 0x100);
          fh.U(fm::M + 4 * (1 + 26277), kHandKeyObj);
        }
        SceneTables tbh;
        live::Counters cnh;
        emit_rows(fh, tbh, cnh);
        bool ok1 = false;
        for (const CrowdPropRow& r : tbh.crowd_props) ok1 = ok1 || (r.kind == 2 && r.variant == 1 && r.model_key == 41011);
        Check(ok1, "rows.hand_mesh_key_per_type", "rows " + std::to_string(tbh.crowd_props.size()));
        Forge fbad = fp;   // the loader counted 5 types, the walk finds 4: no hand gets a key; the shown hand is a hole (never 41016)
        fbad.U(fm::M + 0x198D0, 5);
        SceneTables tbb;
        live::Counters cnb;
        emit_rows(fbad, tbb, cnb);
        const auto lb = c.LatchCensus();
        Check(lb.hand_map_bad == 1 && lb.hand_key_unread == 1 && lb.hand_types == 4 && lb.hand_types_game == 5, "latch.hand_map_count_guard",
              "bad " + std::to_string((unsigned long long)lb.hand_map_bad) + " key_unread " + std::to_string((unsigned long long)lb.hand_key_unread));
      }
      {   // ---- S103 SG review 1 / 2 / 3: the hand map through the ENTRY half (scope 2) merged into the latch half (scope 1); a refused read; the frozen map per generation
        const Forge keep = g_forge;
        g_forge = fp;
        Fresh(c, p);
        c.SetLatchReader(&ForgeRead);
        ForgeSingleCamera(g_forge), c.NoteEntryCamera(kSingleCam);
        FeedPalettes(c, 0);
        crowd::SnapPolicy oke;
        oke.latch_partial_ok = true;
        c.SetSnapPolicy(oke);
        c.NoteOursSkip(fm::N);
        c.EntryRead();
        live::FrameInfo fe;
        fe.guest_frame = 100;
        SceneTables te;
        live::Counters ce;
        c.Latch(fe);
        c.Emit(fe, te, ce);
        const auto le = c.LatchCensus();
        Check(le.entry_used == 1 && le.hand_map_ok == 1 && le.hand_map_bad == 0 && le.hand_types == 4 && le.hand_types_game == 4 && le.hand_idx[3] == 77, "latch.hand_map_entry_merge",
              "used " + std::to_string((unsigned long long)le.entry_used) + " ok " + std::to_string((unsigned long long)le.hand_map_ok) + " types " + std::to_string(le.hand_types) + " idx3 " + std::to_string(le.hand_idx[3]));   // (the entry's map reaches the latch's census, review 1)
        // review 2: a refused read of the map's inputs leaves NO map, but the props stay (a refusal giving 0 == 0 is not ok)
        Forge fr = fp;
        fr.U(fm::M + 0x198D0, 0);
        fr.bad.insert(0x9E400000u + 3580 * 3 + 780);   // slot 3's wrestler id cannot be read
        crowd::LatchSnapshot gr;
        gr.node = fm::N;
        crowd::latch::ReadSnapshot(fr, gr);
        Check(!gr.hand_map_ok && gr.hand_types == 0 && gr.props.size() == 2 && gr.soft[1] == 0, "latch.hand_map_refused_keeps_props", "ok " + std::to_string(gr.hand_map_ok) + " props " + std::to_string(gr.props.size()) + " soft " + std::to_string(gr.soft[1]));
        // review 3: frozen per generation. The loader's inputs move AFTER it ran (slot 0's wrestler halfword is rewritten): the cached map still answers; a changed picture word (a new generation) recomputes
        crowd::HandMapCache hc;
        Forge fz = fp;
        crowd::LatchSnapshot z1, z2, z3;
        z1.node = z2.node = z3.node = fm::N;
        z1.hand_cache = z2.hand_cache = z3.hand_cache = &hc;
        crowd::latch::ReadSnapshot(fz, z1);
        H16(fz, kHandData + 2 * 98, 11);   // slot 0's h 3 -> 11 (the live inputs drift)
        crowd::latch::ReadSnapshot(fz, z2);
        fz.U(fm::M + 0x19A94, 0x12345678u);   // a new generation (the picture words differ)
        crowd::latch::ReadSnapshot(fz, z3);
        Check(z1.hand_map_ok && z2.hand_map_ok && z1.hand_h[0] == 3 && z2.hand_h[0] == 3 && z3.hand_map_ok && z3.hand_h[0] == 11, "latch.hand_map_frozen",
              "h0 " + std::to_string(z1.hand_h[0]) + "," + std::to_string(z2.hand_h[0]) + "," + std::to_string(z3.hand_h[0]));
        g_forge = keep;
      }
      {   // ---- S103 SG census: a bone-held prop whose bone bytes stay while the holder moves (the OURS-frame risk), per frame kind
        const Forge keep = g_forge;
        g_forge = fp;
        Fresh(c, p);
        c.SetLatchReader(&ForgeRead);
        crowd::SnapPolicy okp;
        okp.latch_partial_ok = true;
        c.SetSnapPolicy(okp);
        FeedPalettes(c, 0);
        auto w2 = OracleStands();
        for (const auto& m : OracleCards()) w2.push_back(m);
        SetWalk(c, w2, {PropOracle(0, 0, false), PropOracle(2, 1, true)});
        c.NoteCrowdNode(fm::N);
        Record(c, 100, true);
        for (uint32_t j = 0; j < 2; ++j) g_forge.F(0x9C000000u + 0x10000u * j + 0x3000u + 48, 500.f);   // both holders move (M0 row 3 x); the bones stay
        SetWalk(c, w2, {PropOracle(0, 0, false), PropOracle(2, 1, true)});
        c.NoteCrowdNode(fm::N);
        Record(c, 101, true);
        const auto lz = c.LatchCensus();
        Check(lz.bone_seen_walked == 2 && lz.bone_changed_walked == 0 && lz.bone_seen_ours == 0 && lz.bone_changed_ours == 0, "latch.bone_frozen_census",
              "seen " + std::to_string((unsigned long long)lz.bone_seen_walked) + " changed " + std::to_string((unsigned long long)lz.bone_changed_walked));   // the holders moved, the bone bytes did not: counted per frame kind, no holder translation involved
        g_forge = keep;
      }
      {   // S103 D6: the pictures behind the shown props, read through guest_texture.h (registry -> wrapper -> header -> fetch dwords -> tiled, 16-bit swapped blocks) and written as kScCrowdPictures rows
        crowd::LatchSnapshot gp;
        gp.node = fm::N;
        crowd::latch::ReadSnapshot(fp, gp);
        bool okp = gp.pictures.size() == 2;
        if (okp) {
          const crowd::PictureObs& a = gp.pictures[0];
          const crowd::PictureObs& b = gp.pictures[1];
          okp = a.key == kSignKey && a.why == 0 && a.width == 64 && a.height == 64 && a.format == 18 && a.bytes == g_sign_lin && b.key == kHandKey && b.why == 0 && b.width == 64 && b.height == 128 && b.bytes == g_hand_lin;
        }
        Check(okp, "latch.picture_blocks", "pictures " + std::to_string(gp.pictures.size()));
        {   // S103 CSF (H-CS1): a sign / foam hand whose key has a FILE picture is left to the file once the one-time check (the guest's blocks against the file's FNV) has passed; the sign's file key is the one of the SOURCE slot holding the draw's handle (the phase-4 re-map leaves keys[b] stale)
          namespace cs = rdr_scene::live::crowd;
          constexpr uint32_t kKeyTab = 0x9E160000u, kSrcTab = 0x9E170000u;
          struct R { size_t pics = 0, rows = 0; uint32_t key = 0xFFFFFFFFu; };
          const uint64_t fs_sign = cs::Fnv1a(g_sign_lin.data(), g_sign_lin.size()), fs_hand = cs::Fnv1a(g_hand_lin.data(), g_hand_lin.size());
          // the draw slot b = 2 holds kSignKey; the source slot 5 holds it too (the re-map moved it); keys[5] = the true file key, keys[2] = `stale`
          const auto run = [&](uint32_t file_key, uint32_t stale) {
            Forge fs = fp;
            fs.U(kTexMgrObj + 12, kSrcTab), fs.U(kTexMgrObj + 16, kKeyTab);
            fs.U(kSrcTab + 8 * 5, kSignKey), fs.U(kKeyTab + 4 * 5, file_key), fs.U(kKeyTab + 4 * 2, stale);
            crowd::LatchSnapshot w;   // (the warm-up frame: the one-time check happens here)
            w.node = fm::N;
            crowd::latch::ReadSnapshot(fs, w);
            crowd::LatchSnapshot g;
            g.node = fm::N;
            crowd::latch::ReadSnapshot(fs, g);
            R r;
            r.pics = g.pictures.size();
            for (const crowd::PropObs& po : g.props)
              if (po.site == 0) r.key = po.file_key;
            SceneTables ts;
            live::Counters cs2;
            emit_rows(fs, ts, cs2);
            r.rows = ts.crowd_pictures.size();
            if (!ts.crowd_props.empty() && ts.crowd_props[0].file_key != r.key) r.key = 0xFFFFFFFEu;   // (the row must carry what the reader saw)
            return r;
          };
          cs::SignFileKeys::Clear();
          const R a0 = run(60011, 60011);   // nothing published: the guest reads stay (sign + hand = 2), the key rides in the row
          cs::SignFileKeys::Publish(60011, 0, fs_sign);
          const R a1 = run(60011, 60011);   // the sign: checked once, then the file's: only the hand's picture is read
          cs::SignFileKeys::Publish(42016, 1, fs_hand);   // the hand (mesh 41016) has its file picture 42016 too
          const R a2 = run(60011, 60011);   // both from the file: no guest read at all
          const R a3 = run(60021, 60021);   // a sign key the file lacks keeps its guest read
          cs::g_sign_file_pictures.store(false);
          const R a4 = run(60011, 60011);   // the one switch off: the pre-CSF rule (both read)
          cs::g_sign_file_pictures.store(true);
          const R a5 = run(65001, 65001);   // the game's runtime texture range: never the file
          const R a6 = run(60011, 60021);   // the re-map: keys[2] is stale (60021), the source slot of the same handle says 60011: the draw is key 60011, from the file
          const uint64_t ver = cs::SignFileKeys::Verifications().load(), mis0 = cs::SignFileKeys::Mismatches().load();
          cs::SignFileKeys::Clear();
          cs::SignFileKeys::Publish(60011, 0, fs_sign ^ 1u);   // a file whose picture is NOT the guest's: the check fails, the key stays on guest reads
          const R a7 = run(60011, 60011);
          const R a8 = run(60011, 60011);
          const uint64_t mis1 = cs::SignFileKeys::Mismatches().load();
          const bool still_has = cs::SignFileKeys::Has(60011, 0);
          cs::SignFileKeys::Clear();
          Check(a0.pics == 2 && a0.key == 60011 && a0.rows == 2 && a1.pics == 1 && a1.key == 60011 && a1.rows == 1 && a2.pics == 0 && a2.key == 60011 && a2.rows == 0 && a3.pics == 1 && a3.key == 60021 && a3.rows == 1 &&
                a4.pics == 2 && a4.rows == 2 && a5.pics == 1 && a5.key == 65001 && a5.rows == 1,
                "latch.sign_file_no_guest_read", "pics " + std::to_string(a0.pics) + "/" + std::to_string(a1.pics) + "/" + std::to_string(a2.pics) + "/" + std::to_string(a3.pics) + "/" + std::to_string(a4.pics) + "/" + std::to_string(a5.pics) + " rows " + std::to_string(a0.rows) + "/" + std::to_string(a1.rows) + "/" + std::to_string(a2.rows) + "/" + std::to_string(a3.rows) + "/" + std::to_string(a4.rows) + "/" + std::to_string(a5.rows));
          Check(a6.key == 60011 && a6.pics == 0 && a6.rows == 0, "latch.sign_file_key_remap", "key " + std::to_string(a6.key) + " pics " + std::to_string(a6.pics));
          Check(ver >= 2 && a7.pics == 2 && a8.pics == 2 && mis1 > mis0 && !still_has, "latch.sign_file_mismatch_guest", "pics " + std::to_string(a7.pics) + "/" + std::to_string(a8.pics) + " mismatches " + std::to_string(mis1 - mis0));
        }
        bool okr2 = tb.crowd_pictures.size() == 2;
        if (okr2) okr2 = tb.crowd_pictures[0].key == kSignKey && tb.crowd_pictures[0].nbytes == 2048 && std::memcmp(tb.crowd_pictures[0].bytes, g_sign_lin.data(), 2048) == 0 && tb.crowd_pictures[1].nbytes == 4096 && tb.crowd_pictures[1].height == 128;
        Check(okr2, "rows.picture_rows", "rows " + std::to_string(tb.crowd_pictures.size()));
        Forge fw = fp;
        fw.U(0x00AA0001u + 52, 1);   // the sign's wrapper is a cube: not a plain 2D texture
        SceneTables tw;
        live::Counters cw;
        emit_rows(fw, tw, cw);
        Check(tw.crowd_pictures.size() == 1 && cw.unknown > cn.unknown, "rows.picture_unreadable_hole", "rows " + std::to_string(tw.crowd_pictures.size()) + " unk " + std::to_string(cw.unknown) + "/" + std::to_string(cn.unknown));
        Forge fk = fp;
        fk.U(kHandleBegin + 8 * 7, kSignKey ^ 0x01000000u);   // a registry entry whose stored id differs from the handle: the key is not resolved, never the cached pointer
        rdr_scene::live::guest_tex::Resolved rk;
        Check(rdr_scene::live::guest_tex::ResolveHandle(fk, kSignKey, rk, 0x00AA0001u) != rdr_scene::live::guest_tex::Why::Ok, "latch.picture_handle_checked");   // (given the entry's cached pointer, which must not be trusted)
        {   // review 3 / 4 / 5
          namespace gt = rdr_scene::live::guest_tex;
          const uint32_t f64[6] = {(1u << 31) | (2u << 22), 18u | (1u << 6) | 0x00100000u, 63u | (63u << 13), 0, 0, (1u << 9) | (1u << 11)};
          const uint32_t f16[6] = {(1u << 31) | (1u << 22), 18u | (1u << 6) | 0x00100000u, 15u | (15u << 13), 0, 0, (1u << 9) | (1u << 11)};
          gt::Shape sh;
          {   // S108 CAP2: mip 0 of a 16x16 texture with the packed bit sits in the base's packed tail at block (4, 0) (the SDK's GetPackedMipOffset: 16 texels right; tools\scene\car_paint_unit.cpp part T
              // proves the read against the SDK's own layout): it is read THERE, never from the tail's corner (plant 4 picture_packed_ignored = the corner)
            const bool s64 = gt::ShapeOfFetch(f64, sh) == gt::Why::Ok && sh.tail_x == 0 && sh.tail_y == 0;
            gt::Shape s16;
            const bool shaped16 = gt::ShapeOfFetch(f16, s16) == gt::Why::Ok;
            const auto by_offset = [](uint32_t a, uint32_t n, uint8_t* d) { for (uint32_t i = 0; i < n; ++i) d[i] = uint8_t(((a + i) >> 3) * 7u + 1u); return true; };   // each 8-byte block of memory distinct
            std::vector<uint8_t> b16;
            const bool read16 = shaped16 && gt::ReadMip0(s16, by_offset, b16) == gt::Why::Ok && b16.size() == 4 * 4 * 8;
            const uint32_t at = gt::TiledOffset2D(4, 0, 32, 3);   // block (4, 0) of a 32-block pitch, 8-byte blocks
            uint8_t want[8];
            by_offset(s16.base + at, 8, want);
            gt::SwapBlock(want, 8, s16.endian);
            Check(s64 && shaped16 && s16.tail_x == 4 && s16.tail_y == 0 && read16 && std::memcmp(b16.data(), want, 8) == 0, "latch.picture_packed_mips",
                  "a 16x16 packed texture: mip 0 read at its tail offset (4, 0), tail " + std::to_string(s16.tail_x) + "," + std::to_string(s16.tail_y));
          }
          int reads = 0;
          const auto counting = [&](uint32_t, uint32_t, uint8_t*) { ++reads; return true; };
          const uint32_t fbig[6] = {(1u << 31) | (128u << 22), 18u | (1u << 6) | 0x00100000u, 4095u | (4095u << 13), 0, 0, 1u << 9};
          std::vector<uint8_t> bl;
          gt::Shape sb;
          const bool shaped = gt::ShapeOfFetch(fbig, sb) == gt::Why::Ok;
          Check(shaped && gt::ReadMip0(sb, counting, bl) == gt::Why::Size && reads == 0, "latch.picture_size_before_read", "reads " + std::to_string(reads));
          gt::Resolved rr;
          gt::Shape sr;
          std::vector<uint8_t> br;
          Check(gt::ReadHandleTexture(fp, kSignKey, counting, rr, sr, br, 1000) == gt::Why::Size && reads == 0, "latch.picture_cap_before_read", "reads " + std::to_string(reads));
          {   // S103 CSF: a LINEAR 64x128 BC1 hand (payload 4096 B, rows padded to 32 blocks = 8192 B source): the padded extent is never counted against the payload cap
            const uint32_t flin[6] = {2u << 22, 18u | (1u << 6) | 0x00100000u, 63u | (127u << 13), 0, 0, 1u << 9};
            gt::Shape sl;
            std::vector<uint8_t> bll;
            const bool ok_shape = gt::ShapeOfFetch(flin, sl) == gt::Why::Ok && !sl.tiled && sl.pitch_blocks == 32;
            uint32_t asked = 0;
            const auto rows = [&](uint32_t, uint32_t n, uint8_t* d) { asked = n; for (uint32_t i = 0; i < n; ++i) d[i] = uint8_t(i / 256u); return true; };   // every 256-byte padded source row holds its row number
            const bool read_ok = ok_shape && gt::ReadMip0(sl, rows, bll, 4096) == gt::Why::Ok && bll.size() == 4096 && asked == 8192;
            bool depad = read_ok;
            for (uint32_t y = 0; depad && y < 32; ++y)
              for (uint32_t x = 0; x < 128; ++x) depad = depad && bll[y * 128 + x] == y;   // the padding is stepped over: output row y = source row y
            Check(depad, "latch.picture_linear_padded_cap", "a linear 64x128 BC1 picture reads ok under the 4096 payload cap, 8192 B asked, rows de-padded (asked " + std::to_string(asked) + ")");
            // review A: a bad (huge) pitch is refused before any read
            const uint32_t fhuge[6] = {0x1FFu << 22, 18u | (1u << 6) | 0x00100000u, 63u | (127u << 13), 0, 0, 1u << 9};
            gt::Shape sh2;
            int huge_reads = 0;
            const auto cnt = [&](uint32_t, uint32_t, uint8_t*) { ++huge_reads; return true; };
            std::vector<uint8_t> b2;
            Check(gt::ShapeOfFetch(fhuge, sh2) == gt::Why::Ok && gt::ReadMip0(sh2, cnt, b2, 4096) == gt::Why::Size && huge_reads == 0, "latch.picture_huge_pitch_refused", "reads " + std::to_string(huge_reads));
            gt::Shape sh3 = sh2;   // a 4x256 BC1 strip (512 B payload) on a 4096-block pitch: its two tile rows are 1 MB apart (top 1,056,408 B)
            sh3.tiled = true, sh3.width = 4, sh3.height = 256, sh3.pitch_blocks = 4096;
            Check(gt::ReadMip0(sh3, cnt, b2, 4096) == gt::Why::Size && huge_reads == 0, "latch.picture_huge_pitch_tiled_refused", "reads " + std::to_string(huge_reads));
            {   // review 5: wide, short TILED textures under the 4096 payload cap are legitimate (the padded rectangle bounds the extent, not payload + a row of padding)
              bool sweep = true;
              std::string bad;
              const uint32_t wh[][2] = {{512, 4}, {256, 8}, {128, 16}, {1024, 4}, {64, 64}, {64, 128}, {32, 128}, {16, 256}};
              for (const auto& d : wh) {
                gt::Shape sw;
                sw.width = d[0], sw.height = d[1], sw.game_format = 18, sw.bpb = 8, sw.tiled = true, sw.base = 0x100000u;
                sw.pitch_blocks = ((d[0] / 4 + 31u) & ~31u);
                std::vector<uint8_t> bw;
                const bool ok = gt::ReadMip0(sw, [](uint32_t, uint32_t, uint8_t*) { return true; }, bw, 4096) == gt::Why::Ok && bw.size() == size_t(d[0] / 4) * (d[1] / 4) * 8;
                if (!ok) sweep = false, bad += " " + std::to_string(d[0]) + "x" + std::to_string(d[1]);
              }
              Check(sweep, "latch.picture_tiled_wide_short_ok", "refused:" + bad);
            }
          }
          Forge fb = fp;
          fb.bad.insert(0x9E300000u + 28);   // the sign's header cannot be read
          crowd::LatchSnapshot gb;
          gb.node = fm::N;
          const crowd::latch::Why wb = crowd::latch::ReadSnapshot(fb, gb);
          Check(wb == crowd::latch::Why::None && !fb.fail && gb.pictures.size() == 2 && gb.pictures[0].why != 0 && gb.pictures[1].why == 0, "latch.picture_bad_pointer_local", "why " + std::to_string(int(wb)));
          SceneTables tbd;
          live::Counters cbd;
          emit_rows(fb, tbd, cbd);
          Check(c.LatchCensus().pictures_unread >= 1 && tbd.crowd_pictures.size() == 1, "rows.picture_bad_pointer_hole", "unread " + std::to_string(c.LatchCensus().pictures_unread));
        }
        // the holder: a complete pose chain through the contributor -> one CrowdSingleRow (body = the arena's type body, the skin palette in kScBones)
        Forge fs = fp;
        ForgeSkinned(fs);
        SceneTables ts;
        live::Counters cs;
        emit_rows(fs, ts, cs);
        bool oks = ts.crowd_singles.size() == 1 && ts.crowd_singles[0].bones_count == 3 && ts.crowd_singles[0].type == 7 && ts.crowd_singles[0].asset < ts.assets.size();
        if (oks) {
          double want[3][16];
          SkinOracle(want);
          const CrowdSingleRow& g = ts.crowd_singles[0];
          for (uint32_t k = 0; oks && k < 3; ++k)
            for (uint32_t q = 0; q < 16; ++q) oks = oks && std::fabs(double(ts.bones[g.bones_first + k].m[q]) - want[k][q]) < 2e-5 * (1.0 + std::fabs(want[k][q]));
          for (int r = 0; oks && r < 3; ++r)
            for (int q = 0; oks && q < 4; ++q) oks = g.c[r * 4 + q] == 100.f + float(q * 4 + r);   // the world, members' constants form (c[r*4+k] = W[k][r])
        }
        Check(oks, "rows.single_rows", "rows " + std::to_string(ts.crowd_singles.size()));
      }
      // the props alone do not make the frame whole: the sign holders are the second class's single people (slots 20-51 of the node's list, vtable 0x8202B8DC), which are still unread (CROWD_PROPS_GATES.md 4)
      Check(c.LatchCensus().props_installed == 2 && g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::Unmodelled)] == 1, "rows.singles_still_block", "stamps " + std::to_string(g_stamps));
      Forge fh = fp;
      fh.U(kHandleBegin + 8 * 7 + 4, 0);   // the sign's texture gate: kept as a HIDDEN row
      SceneTables tb2;
      live::Counters cn2;
      emit_rows(fh, tb2, cn2);
      Check(tb2.crowd_props.size() == 2 && (tb2.crowd_props[0].flags & 4) != 0 && (tb2.crowd_props[1].flags & 4) == 0, "rows.hidden_flag", tb2.crowd_props.empty() ? "0 rows" : std::to_string(tb2.crowd_props[0].flags));
      Forge fz = fp;
      for (uint32_t q = 0; q < 16; ++q) fz.F(0x9C000000u + 0x3000u + 4 * q, 0.f);   // a SHOWN sign whose world is all zeros: the writer refuses, the frame is not whole
      SceneTables tb3;
      live::Counters cn3;
      emit_rows(fz, tb3, cn3);
      Check(tb3.crowd_props.size() == 1 && g_stamps == 0 && cn3.unknown == cn.unknown + 1, "rows.bad_world_hole", "rows " + std::to_string(tb3.crowd_props.size()) + " stamps " + std::to_string(g_stamps));
      {   // review 4: a gated-off prop with a bad world is no row and no hole
        Forge fzh = fz;
        fzh.U(kHandleBegin + 8 * 7 + 4, 0);
        SceneTables tbh;
        live::Counters cnh;
        emit_rows(fzh, tbh, cnh);
        Check(tbh.crowd_props.size() == 1 && cnh.unknown == cn.unknown, "rows.hidden_bad_world_no_hole", "rows " + std::to_string(tbh.crowd_props.size()) + " unk " + std::to_string(cnh.unknown) + "/" + std::to_string(cn.unknown));
      }
      {   // review 1: an OURS install, then an OURS decline: no stale signs
        emit_rows(fp, tb, cn);
        const Forge keep = g_forge;
        g_forge = fp;
        g_forge.U(crowd::mgr::kMgrGlobal, 0);
        c.NoteOursSkip(fm::N);
        SceneTables tbd;
        live::Counters cnd;
        live::FrameInfo fid;
        fid.guest_frame = 101;
        c.Latch(fid);
        c.Emit(fid, tbd, cnd);
        g_forge = keep;
        Check(tbd.crowd_props.empty() && c.LatchCensus().declined[1] == 1, "rows.decline_clears_props", "rows " + std::to_string(tbd.crowd_props.size()));
      }
      SceneTables tb4;
      live::Counters cn4;
      emit_rows(fp, tb4, cn4, false);
      {   // S103 SG2: the old rule (a walked frame carries no prop row: the stream draws them) is gone; the rows are there, ALL flagged in place, so a live drawer skips them and the mirror draws them
        bool all_flagged = !tb4.crowd_props.empty();
        for (const CrowdPropRow& r : tb4.crowd_props) all_flagged = all_flagged && (r.flags & kCpfInPlace);
        Check(all_flagged, "rows.walked_frame_in_place", "rows " + std::to_string(tb4.crowd_props.size()));
      }
    }
  }
}
