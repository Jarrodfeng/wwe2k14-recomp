// S102 lane T16 (docs\renderer\CROWD_PROMOTION.md, "T16"): the stands member set read from the crowd manager's own lists. PURE: no game, SDK or renderer includes; a Mem type with
// u32(addr) / f32(addr) (guest memory in the game, forged memory in tools\crowd\crowd_from_manager_test.cpp).
//
// FROM P8b: ReadMemberSet, MemberConstants, AngleOf, Digest and the Why codes are the reader of lane P8b (branch worktree-agent-a0d57e760ce5cbb40, src\renderer_game\crowd_inputs.h, proven there on forged
// memory and against 33,424 recorded stands worlds), copied here without P8's board / stamps / node constants, in this namespace, so this file does not clash when P8b merges (then this
// header can include P8b's reader instead; the function is the same). The addresses are from the listings of sub_82518340 (0x82518340-0x825185AC), sub_825185B0 and sub_825133F0, noted beside each.
// ONE change from P8b: the sine / cosine of the member's angle (sub_82C33208 -> sub_82C322A0 / sub_82C32228) are read from the game's own float table in guest memory (TableSinCos below, listing
// 0x82C32228-0x82C32298 and 0x82C322A0-0x82C3231C), not libm, so the worlds are the walk's bits when the table is read right [U: the table itself is not in a capture; the unit test proves the port's
// indexing against a forged table, and the libm variant stays as the offline reference].
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rdr_scene::live::crowd::mgr {

inline constexpr uint32_t kMgrGlobal = 0x836F4DC0u;    // lis -31889 / lwz 19904 (0x82518364 / 0x82518380)
inline constexpr uint32_t kRingIndex = 0x83762F84u;    // lis -31882 / lwz 12164 (0x82518460): the double buffer index 0 / 1
inline constexpr uint32_t kTable = 0x835E4CD8u;        // lis -31906 / addi 19672 (0x825183C8 / 0x825183EC)
inline constexpr uint32_t kRingStride = 0x1E05Cu;      // ori 57436 on 0x10000 (0x825183E4); mullw 0x8251846C
inline constexpr uint32_t kFlagsOff = 0xF00Cu;         // addis +1 / addi -4084 (0x82518478-0x82518484)
inline constexpr uint32_t kGroupOff = 12u;             // table + 12 (0x82518498-0x825184A0)
inline constexpr uint32_t kScalePtrOff = 8u;           // table + 8: pointer, + 3072 i (0x82518494-0x8251849C)
inline constexpr uint32_t kCountPtrOff = 4u;           // table + 4: pointer, count of type i at +4 i (0x82518470, 0x8251847C)
inline constexpr uint32_t kRecPtrOff = 0u;             // table + 0: pointer, + 12288 i, 16-byte records (0x82518474, 0x82518488)
inline constexpr uint32_t kRecStride = 16u, kTypeStride = 12288u, kFlagTypeStride = 3072u;
inline constexpr uint32_t kRequestOff = 252u;          // manager + 252: the classifier's request word (0x825185F4)
inline constexpr uint32_t kTypes = 20u, kGroups = 3u;
inline constexpr uint32_t kMaxPerType = 768u;
inline constexpr uint32_t kSinTable = 0x83930058u;     // lis -31853 / addi 88 (0x82C322A4-0x82C322AC and 0x82C3222C-0x82C32234): 16384 floats, the quarter-wave table both functions index
inline constexpr uint32_t kSinTableEntries = 16384u;

struct Member {
  uint8_t type = 0, group = 0;
  uint16_t index = 0;
  float c[12] = {};   // VS c76-c78 as the walk's op 21 records them (the world, column r of the row-vector 4x4 in c[4r..4r+3])
};

enum class Why : uint8_t { None = 0, NoManager, BadRing, ClassifyPending, BadPointer, TooMany, Count };

// sub_8250CA78 + sub_82C33208 (listings 0x8250CA78-0x8250CB0C, 0x82C33208-0x82C33280): the member's matrix. angle = ((w >>a 15) & ~0x1FF) & 0xFFFF (0x825184D8, 0x825184DC, 0x82C33214);
// m = scale * Ry(angle) with row 3 = the record's x, y, z; [0] = c*s, [2] = -sn*s, [5] = s, [8] = sn*s, [10] = c*s (the rotation stores 0x82C3325C-0x82C33268, the scale 0x8250CAAC-0x8250CAD8).
inline uint32_t AngleOf(uint32_t w) { return uint32_t((int32_t(w) >> 15) & ~0x1FF) & 0xFFFFu; }
template <class SinCos>
inline void MemberConstants(const uint32_t rec[4], float scale, SinCos sc, float c[12]) {
  float cs, sn;
  sc(AngleOf(rec[3]), cs, sn);
  float m[16] = {};
  m[0] = cs * scale; m[2] = -sn * scale; m[5] = scale; m[8] = sn * scale; m[10] = cs * scale;
  std::memcpy(&m[12], &rec[0], 12);
  m[15] = 1.f;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) c[r * 4 + k] = m[k * 4 + r];
}
inline void LibmSinCos(uint32_t angle, float& cs, float& sn) {   // the offline reference
  const double t = double(angle) * 6.283185307179586 / 65536.0;
  cs = float(std::cos(t));
  sn = float(std::sin(t));
}
// The game's table sine (listing 0x82C32228): a = angle & 0xFFFF; [0, 16384): T[a]; [16384, 32768): T[32767 - a]; [32768, 49152): -T[a - 32768]; [49152, 65536): -T[65535 - a].
// The cosine (0x82C322A0) is the same lookup at (a + 16384) & 0xFFFF.
template <class M>
inline float TableSin(const M& mem, uint32_t a) {
  a &= 0xFFFFu;
  const uint32_t idx = a < 16384u ? a : a < 32768u ? 32767u - a : a < 49152u ? a - 32768u : 65535u - a;
  const float v = mem.f32(kSinTable + 4u * idx);
  return a < 32768u ? v : -v;
}
template <class M>
struct TableSinCos {
  const M& mem;
  void operator()(uint32_t angle, float& cs, float& sn) const {
    cs = TableSin(mem, (angle + 16384u) & 0xFFFFu);
    sn = TableSin(mem, angle);
  }
};

// The member set the block draw would draw RIGHT NOW (no classification pending), in the walk's order: type i, group g (outer to inner), member k.
// A member is drawn when its flag word has no bit in the top two (rlwinm 0,0,1 == 0, 0x825184B0) and its group word equals g (0x825184BC-0x825184C4).
template <class M, class SinCos>
inline Why ReadMemberSet(const M& mem, SinCos sc, std::vector<Member>& out, uint32_t* ring_out = nullptr) {
  out.clear();
  const uint32_t mgr = mem.u32(kMgrGlobal);
  if (!mgr) return Why::NoManager;
  const uint32_t ring = mem.u32(kRingIndex);
  if (ring > 1) return Why::BadRing;
  if (mem.u32(mgr + kRequestOff) != 0) return Why::ClassifyPending;   // the walk would classify first (0x825185F4-0x82518608): the lists below would be stale
  const uint32_t e = kTable + ring * kRingStride;
  const uint32_t recs = mem.u32(e + kRecPtrOff), counts = mem.u32(e + kCountPtrOff), scales = mem.u32(e + kScalePtrOff);
  if (!recs || !counts || !scales) return Why::BadPointer;
  for (uint32_t i = 0; i < kTypes; ++i) {
    const int32_t n = int32_t(mem.u32(counts + 4 * i));
    if (n <= 0) continue;
    if (n > int32_t(kMaxPerType)) return Why::TooMany;
    // S102 T16: one pass over the type's flag and group words (P8b's loop reads them once per group: three times); the members come out in the same order (group outer, member inner).
    uint16_t bucket[kGroups][kMaxPerType];
    uint32_t cnt[kGroups] = {};
    for (uint32_t k = 0; k < uint32_t(n); ++k) {
      if (mem.u32(e + kFlagsOff + kFlagTypeStride * i + 4 * k) & 0xC0000000u) continue;
      const uint32_t g = mem.u32(e + kGroupOff + kFlagTypeStride * i + 4 * k);
      if (g < kGroups) bucket[g][cnt[g]++] = uint16_t(k);
    }
    for (uint32_t g = 0; g < kGroups; ++g)
      for (uint32_t j = 0; j < cnt[g]; ++j) {
        const uint32_t k = bucket[g][j];
        uint32_t rec[4];
        for (int q = 0; q < 4; ++q) rec[q] = mem.u32(recs + kTypeStride * i + kRecStride * k + 4 * q);
        Member m;
        m.type = uint8_t(i); m.group = uint8_t(g); m.index = uint16_t(k);
        MemberConstants(rec, mem.f32(scales + kFlagTypeStride * i + 4 * k), sc, m.c);
        out.push_back(m);
      }
  }
  if (ring_out) *ring_out = ring;
  return Why::None;
}

// The literal P8b loop (group outer, a read of the flag and group words per group): the reference the tests compare ReadMemberSet with. Not used by the game.
template <class M, class SinCos>
inline Why ReadMemberSetReference(const M& mem, SinCos sc, std::vector<Member>& out) {
  out.clear();
  const uint32_t mgr = mem.u32(kMgrGlobal);
  if (!mgr) return Why::NoManager;
  const uint32_t ring = mem.u32(kRingIndex);
  if (ring > 1) return Why::BadRing;
  if (mem.u32(mgr + kRequestOff) != 0) return Why::ClassifyPending;
  const uint32_t e = kTable + ring * kRingStride;
  const uint32_t recs = mem.u32(e + kRecPtrOff), counts = mem.u32(e + kCountPtrOff), scales = mem.u32(e + kScalePtrOff);
  if (!recs || !counts || !scales) return Why::BadPointer;
  for (uint32_t i = 0; i < kTypes; ++i) {
    const int32_t n = int32_t(mem.u32(counts + 4 * i));
    if (n <= 0) continue;
    if (n > int32_t(kMaxPerType)) return Why::TooMany;
    for (uint32_t g = 0; g < kGroups; ++g)
      for (uint32_t k = 0; k < uint32_t(n); ++k) {
        if (mem.u32(e + kFlagsOff + kFlagTypeStride * i + 4 * k) & 0xC0000000u) continue;
        if (mem.u32(e + kGroupOff + kFlagTypeStride * i + 4 * k) != g) continue;
        uint32_t rec[4];
        for (int j = 0; j < 4; ++j) rec[j] = mem.u32(recs + kTypeStride * i + kRecStride * k + 4 * j);
        Member m;
        m.type = uint8_t(i); m.group = uint8_t(g); m.index = uint16_t(k);
        MemberConstants(rec, mem.f32(scales + kFlagTypeStride * i + 4 * k), sc, m.c);
        out.push_back(m);
      }
  }
  return Why::None;
}

inline uint64_t Digest(const std::vector<Member>& v) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  for (const Member& m : v) mix(&m, sizeof(m));
  return h;
}

// A reader's member set as the contributor keeps it: the members plus where each type starts (the set is type-major).
struct Set {
  std::vector<Member> members;
  uint32_t first[kTypes + 1] = {};   // members[first[i] .. first[i + 1]) are type i's, in (group, member) order
  uint64_t stamp = ~0ull;            // the guest frame the set was read for
  void Index() {
    uint32_t at = 0;
    for (uint32_t i = 0; i <= kTypes; ++i) {
      while (at < members.size() && members[at].type < i) ++at;
      first[i] = at;
    }
    first[kTypes] = uint32_t(members.size());
  }
  uint32_t Count(uint32_t type) const { return type < kTypes ? first[type + 1] - first[type] : 0; }
};

}  // namespace rdr_scene::live::crowd::mgr
