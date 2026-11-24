// The crowd snapshot read at the latch, with no walk (S103 lane D3b; docs\renderer\INPUTS_D3.md section 3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// PURE: no game, SDK or renderer includes. `Mem` supplies u32(addr), f32(addr), words(addr, n, dst) (guest memory in the game: inputs_glue.h; forged memory in tools\scene\crowd_snapshot_unit.cpp)
// and a `fail` flag set when a read was refused (the glue's fault-checked reader).
//
// WHY: on an OURS frame the crowd entry sub_82519508 is skipped, so the op-21 recorder (the members' worlds) and the palette hook never run. Every value they saw is a function of manager state the UPDATE
// side wrote, readable while the game threads are parked:
//   * the entry's own gates (listing sub_82519508): [this+556] == 0 -> return (0x82519548-50); [manager+560] == 0 -> return (0x82519564-6C): the frame draws NO crowd. The stands / cards pass
//     sub_825185B0 runs only if one of [manager+476/480/484] != 0 (0x82519760-88);
//   * the stands members + worlds: the manager's lists (crowd_mgr_reader.h = lane P8b's proven reader, 6,043 + 27,381 recorded worlds to 1 ULP; sub_8250CA78 = scale * Ry(angle), row 3 = x y z);
//   * the far cards: sub_82512F70 reads the SAME table entry as the block draw (listing 0x82512F7C-0x82512FF8) and the SAME world function; it differs in the skip test: the card loop skips a member whose flag
//     word has bit 0 or bit 2 set (0xA0000000; 0x82513010-0x82513014), the block draw skips on 0xC0000000, and the card loop does not test the group word. sub_825185B0 runs the card loop for type i only
//     when [entry + 0x1E00C + 4 i] != 0 (0x82518778-98; review F1) [C listing, review];
//   * the palettes: the manager's blocks at [manager+608], single buffered; the walk publishes palettes_[[manager+544]] (the READ ring) while the hook on sub_82509CB8 fills palettes_[[manager+548]] (the WRITE
//     ring) as the update side runs; this reader reports both rings so the contributor installs the read ring's set and compares the +608 read with the write ring's (review F2).
// The classifier sub_825133F0 (flag bits, pending request [manager+252]) is served on OURS frames by CrowdKeepRegistration (scene_skip_nodes.h, review F6): a pending request here is a decline.
// * the seated floor people (sub_8276EAA0, review round 2; docs\renderer\CROWD_FAMILY_8276ED60.md section 2.1): the crowd node holds 52 words at node + 32 (the loop 0x82519734-0x8251975C); an object whose vtable
//   word is 0x8202B8C8 is a floor-seat group: [obj+20] members, [obj+24] the person type, [obj+64] the 176-byte elements; element i: [e+152] the 64-byte matrix (rows 0..2 are the world's rotation rows), [e+156] the
//   16-byte position (x, y - f31, z; the w word's top bit is the SCREEN TEST's result, which sub_8250E7E0 writes inside the skipped entry: not read; the reader runs that test itself, ScreenHides below, so
//   the set is the game's, not a superset), [e+160] the flags halfword pointer (the seat is skipped when flags & 0x8000 if [manager+248] != 0 else flags & 0x1000, 0x8276EC60-0x8276EC80). f31 = 0.0 (0x8218B8CC), or the
//   float at 0x82192140 (10.0) when the word 0x836F3940 == 4 (0x8276EB08-0x8276EB1C). The world is the 4x4 (matrix rows 0..2, position) transposed into c76-c78 (0x8276ECF4-0x8276ED34). [C listing]
//   The single people of the 52 (vtable 0x8202B8DC: sub_8276E7B0) are ReadSingles' (S103 C1); any other class is counted `unread` (other) unless its element's flags hide it by the same rule. A node LOD word != 0 draws group 20 lod + type (S103 C1).
// * the signs and foam hands (S103 D3c; docs\renderer\CROWD_PROPS_LATCH.md): sub_82518950 (signs, op 21 returns 0x82518D4C / 0x82518E54) and sub_82518EB8 (foam hands, 0x825192BC / 0x825194A0) walk the SAME
//   objects, `this = node - 32`. [this+596] == 0: pass 1 over the 32 objects at this + 144 + 4 j; else pass 2 over the 96 free props at this + 608. Pass 1: elt = [obj+64]; flags halfword at [elt+160] with 0x4000
//   set and 0x1000 clear; t = (int8)byte [elt+23]; E = [M + 4 (t + 10)] (M the manager); x = [E+1184] (sub_8250CC08 -> sub_82506FC8; -1 when absent): signs when x is 14006 / 14007, foam hands when 14127 / 14128
//   (sub_82694200 / sub_82693198 db); [E + 4 (206 + ([E+832] == 0))] == 1; world = B x M0 (sub_82296630, row vector) with M0 = the 64 bytes at [elt+152] and B = [[E+48]+192] + 64 idx,
//   idx = (int16) halfword [[this + 4 (t + 16)] + 34]; then transposed into c76-c78. Pass 2: entry j is live when byte [this+608+6240+j] == 1 (signs) / 2 (foam hands); its world is the 64 bytes at this + 608 + 64 j
//   as stored, transposed. S103 D5: the game's gates are REPLICATED (resolved-texture gate sub_82294F20, the distance test when [0x836F3940] == 1, the signs' whole-function gate, the foam hands' resource gates,
//   see the constants above ReadProps) and a prop a gate would not draw is KEPT with `hidden` set (the record row carries kCpfHidden), so the shown set is the game's and the mirror compares it EQUAL
//   (`props(shown)`). [C listings; the mirror census is the live proof; the camera slot of the distance test is [U]]
// NOT covered (so a snapshot made here is `latch_partial` in crowd_snapshot_core.h): the single people (sub_8276E7B0: docs\renderer\CROWD_PROPS_GATES.md section 4 says what is missing), and the signs' own pictures.
// Cost (review F8): each type's arrays are read with one words() call each, the 64 KB sine table once, no per-frame heap vectors.
#pragma once
#include <atomic>
#include <cmath>
#include <utility>
#include "crowd_flip_count.h"   // S110 CROWDFLIPS
#include <cstdint>
#include <cstring>
#include <vector>

#include "crowd_contrib.h"
#include "handed_camera.h"   // S110 CUTCROWD: the props' eye takes the scene camera's source (PickCamera)
#include "../renderer_scene/rows_pictures.h"   // S106 G4-2
#include "guest_texture.h"   // S103 D6
#include "sign_file_keys.h"   // S103 CSF
#include "crowd_mgr_reader.h"
#include "skin_data.h"   // S103 WC R1: the shared +220 data object rule

namespace rdr_scene::live::crowd::latch {

inline constexpr uint32_t kPaletteBasePtr = 608, kPaletteStride = 1408, kWriteRingOffset = 548, kReadRingOffset = 544, kRemapOffset = 0x20000u - 30452u;
inline constexpr uint32_t kCardSkipMask = 0xA0000000u;   // 0x82513010 / 0x82513014: bit 0 and bit 2 of the flag word (PowerPC numbering)
inline constexpr uint32_t kCardGateOff = 0x1E00Cu;       // [entry + 0x1E00C + 4 i] != 0: the card loop runs for type i (0x82518778-98)
inline constexpr uint32_t kFloorVtable = 0x8202B8C8u, kFloorSingleVtable = 0x8202B8DCu, kNodeObjects = 32, kNodeObjectCount = 52, kNodeLodWord = 560, kFloorGroupsPerLod = 20;
inline constexpr uint32_t kPhaseWord = 0x836F3940u, kF31Zero = 0x8218B8CCu, kF31Phase4 = 0x82192140u, kManagerFloorFlagMode = 248;
inline constexpr uint32_t kNodeGateOff = 556, kMgrDrawGateOff = 560, kPassGateOff0 = 476, kPassGateOff1 = 480, kPassGateOff2 = 484;

// The reader's decline codes = Contributor::LatchStats::declined indices (0 = read).
enum class Why : uint32_t { None = 0, NoManager = 1, BadRing = 2, ClassifyPending = 3, BadPointer = 4, TooMany = 5, Unreadable = 6, NoPalette = 7 };


// ---- S103 D8: a refused read is the fault of ONE part, not of the whole snapshot ----
// Run 27 (wwe2k14_7277, mode 1): `crowd_latch declined 8196 [unreadable 8196]` of 10596 reads. The reader chased the pointers of the node's 52 objects (any class), the props' 32 objects and the holders with one STICKY `fail`: a single
// refused read anywhere (a non-element class whose +64 is not a pointer, a null / stale pointer in a prop or a holder) declined the whole frame, stands included, although the stands (the lists) read fine (303 of 303 equal in the
// first window). Now: the stands, cards and palettes are HARD (a refused read there declines, as before, naming the stage); the floor, props, holders and pictures are SOFT, each object on its own. A refused object is a HOLE:
// its outputs are dropped, `unread` counts it (blocks the stamp: nothing is guessed), the census names the part and the first refused address. [C under unit; the live cause of run 27 is [U]: the case has no guest memory.]
enum SoftPart : uint32_t { kSoftFloor = 0, kSoftProps = 1, kSoftSingles = 2, kSoftPictures = 3, kSoftParts = 4 };
enum HardStage : uint32_t { kStageNone = 0, kStageNode = 1, kStageHeader = 2, kStageSin = 3, kStageLists = 4, kStagePalettes = 5, kStageTail = 6, kStages = 7 };
template <class M>
inline auto FailAddr(const M& m, int) -> decltype(uint32_t(m.fail_addr)) { return m.fail_addr; }
template <class M>
inline uint32_t FailAddr(const M&, long) { return 0; }
// Runs body with a clear fail flag; true = nothing was refused. The caller's flag is restored (a refusal inside is consumed here).
template <class M, class F>
inline bool Scoped(const M& mem, F&& body, uint32_t* addr = nullptr);
#ifdef RDR_CROWD_SNAP_PLANTS
inline int g_latch_plant = 0;   // test only: 1 the member's yaw sign is wrong (sin negated); 2 the card loop uses the block draw's skip mask; 3 palette slot LOD 1 / 2 not remapped (read raw); 4 the card gate ignored; 18 (S103 D8) a refused read in an optional part declines the whole snapshot; 19 the table ring is read at the latch, not taken from the entry
#endif
template <class M, class F>
inline bool Scoped(const M& mem, F&& body, uint32_t* addr) {
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 18) return body(), true;   // planted: the refusal is not consumed (the old sticky flag)
#endif
  const bool prior = mem.fail;
  mem.fail = false;
  body();
  const bool bad = mem.fail;
  if (bad && addr) *addr = FailAddr(mem, 0);
  mem.fail = prior;
  return !bad;
}
inline void NoteSoft(LatchSnapshot& out, uint32_t part, uint32_t addr) {
  if (out.soft[part]++ == 0) out.soft_addr[part] = addr;
}
// S103 C1: every unread object is counted through here, with its reason (crowd_contrib.h UnreadWhy): unread_by sums to unread.
inline void MarkUnread(LatchSnapshot& out, uint32_t why) {
  ++out.unread;
  ++out.unread_by[why < kUrCount ? why : kUrOther];
}
// What a unit may have counted, saved before it runs (S103 C1: the reasons and the singles' counters roll back with `unread`).
struct UnreadMark {
  uint32_t unread = 0, by[kUrCount] = {}, screen_hidden = 0, lod_holders = 0;
  explicit UnreadMark(const LatchSnapshot& s) : unread(s.unread), screen_hidden(s.screen_hidden), lod_holders(s.lod_holders) { std::memcpy(by, s.unread_by, sizeof by); }
};
// Drops what a failed unit pushed and counts it as one unread hole.
inline void RollbackSoft(LatchSnapshot& out, size_t members, size_t props, size_t singles, size_t pictures, const UnreadMark& u) {
  out.members.erase(out.members.begin() + std::ptrdiff_t(members), out.members.end());
  out.props.erase(out.props.begin() + std::ptrdiff_t(props), out.props.end());
  out.singles.erase(out.singles.begin() + std::ptrdiff_t(singles), out.singles.end());
  out.pictures.erase(out.pictures.begin() + std::ptrdiff_t(pictures), out.pictures.end());
  out.unread = u.unread;
  out.screen_hidden = u.screen_hidden, out.lod_holders = u.lod_holders;
  std::memcpy(out.unread_by, u.by, sizeof u.by);
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 24) return MarkUnread(out, kUrOther);   // planted: a hole is filed under `other` (the census could not tell the run-32 faults from the rest)
#endif
  MarkUnread(out, kUrHole);
}
template <class M, class F>
inline void SoftEach(const M& mem, LatchSnapshot& out, uint32_t part, uint32_t n, F&& one) {
  for (uint32_t j = 0; j < n; ++j) {
    const size_t mb = out.members.size(), pb = out.props.size(), sb = out.singles.size(), qb = out.pictures.size();
    const UnreadMark ub(out);
    uint32_t fa = 0;
    if (Scoped(mem, [&] { one(j); }, &fa)) continue;
    RollbackSoft(out, mb, pb, sb, qb, ub);
    NoteSoft(out, part, fa);
  }
}


// The game's quarter sine table (sub_82C32228 / sub_82C322A0 index it; 16384 floats at mgr::kSinTable), read ONCE per process. It is NOT image data: sub_82C32720 fills it at boot, T[i] = sinf(i * f31) (0x82C32748-0x82C32768),
// so it is constant after the boot and a sanity check is enough before caching it (review item 7). Reset only by tests that forge another table.
inline std::vector<float>& SinCache() {
  static std::vector<float> t;
  return t;
}
template <class M>
inline bool EnsureSinTable(const M& mem) {
  std::vector<float>& t = SinCache();
  if (t.size() == mgr::kSinTableEntries) return true;
  std::vector<uint32_t> w(mgr::kSinTableEntries);
  mem.words(mgr::kSinTable, mgr::kSinTableEntries, w.data());
  if (mem.fail) return false;
  float first, last;
  std::memcpy(&first, &w[0], 4);
  std::memcpy(&last, &w[mgr::kSinTableEntries - 1], 4);
  if (!(first == 0.f) || !(last > 0.9999f && last <= 1.0f)) return false;   // not filled yet (the boot's sub_82C32720 has not run): never cached
  t.resize(mgr::kSinTableEntries);
  std::memcpy(t.data(), w.data(), size_t(mgr::kSinTableEntries) * 4);
  return true;
}
// sub_82C32228: a = angle & 0xFFFF; [0, 16384): T[a]; [16384, 32768): T[32767 - a]; [32768, 49152): -T[a - 32768]; else -T[65535 - a]. The cosine is the same lookup at (a + 16384) & 0xFFFF.
inline float CachedSin(uint32_t a) {
  a &= 0xFFFFu;
  const uint32_t idx = a < 16384u ? a : a < 32768u ? 32767u - a : a < 49152u ? a - 32768u : 65535u - a;
  const float v = SinCache()[idx];
  return a < 32768u ? v : -v;
}
struct CachedSinCos {
  void operator()(uint32_t angle, float& cs, float& sn) const {
    cs = CachedSin((angle + 16384u) & 0xFFFFu);
    sn = CachedSin(angle);
#ifdef RDR_CROWD_SNAP_PLANTS
    if (g_latch_plant == 1) sn = -sn;   // planted: a wrong yaw sign
#endif
  }
};

// crowd_hooks.h OnPalettes, ported to the pure reader: LOD 0 = the type's node count entries (the larger of the last non-zero row + 1 and the last remap source index + 1 [U, as there]), LOD 1 / 2 = slots up to the last
// remap entry that is not -1, skipped slots identity. Fixed buffers, no heap.
template <class M>
inline Why ReadPalettes(const M& mem, uint32_t manager, PaletteSet& out) {
  const uint32_t pal = mem.u32(manager + kPaletteBasePtr);
  const uint32_t ring = mem.u32(manager + kWriteRingOffset);
  if (!pal || ring > 1) return Why::NoPalette;
  uint32_t w[kPaletteNodes * 16];
  float f[kPaletteNodes * 16];
  for (uint32_t type = 0; type < kTypes; ++type) {
    uint32_t remap[2][kPaletteNodes];
    uint32_t src_bound = 0;
    for (uint32_t lod = 1; lod < kLods; ++lod) {
      mem.words(manager + kRemapOffset + 1760 * (lod - 1) + 88 * type, kPaletteNodes, remap[lod - 1]);
      for (uint32_t j = 0; j < kPaletteNodes; ++j)
        if (remap[lod - 1][j] < kPaletteNodes && remap[lod - 1][j] + 1 > src_bound) src_bound = remap[lod - 1][j] + 1;
    }
    mem.words(pal + type * kPaletteStride, kPaletteNodes * 16, w);
    uint32_t nonzero = 0;
    for (uint32_t k = 0; k < kPaletteNodes * 16; ++k) {
      float x;
      std::memcpy(&x, &w[k], 4);
      if (x != 0.f) nonzero = k / 16 + 1;
    }
    const uint32_t nodes0 = nonzero > src_bound ? nonzero : src_bound;
    if (nodes0) {
      auto& dst = out[0 * kTypes + type];
      dst.resize(size_t(nodes0) * 16);
      std::memcpy(dst.data(), w, dst.size() * 4);
    }
    for (uint32_t lod = 1; lod < kLods; ++lod) {
      mem.words(pal + (lod * kTypes + type) * kPaletteStride, kPaletteNodes * 16, w);
      uint32_t last = 0;
      for (uint32_t j = 0; j < kPaletteNodes; ++j) {
        bool written = remap[lod - 1][j] != 0xFFFFFFFFu;
#ifdef RDR_CROWD_SNAP_PLANTS
        if (g_latch_plant == 3) written = true;   // planted: the remap is ignored
#endif
        for (uint32_t k = 0; k < 16; ++k) {
          if (written) std::memcpy(&f[j * 16 + k], &w[j * 16 + k], 4);
          else f[j * 16 + k] = (k % 5 == 0 ? 1.f : 0.f);
        }
        if (written) last = j + 1;
      }
      if (last) out[lod * kTypes + type].assign(f, f + size_t(last) * 16);
    }
  }
  return Why::None;
}

inline constexpr uint32_t kPropObjects = 144, kPropObjectCount = 32, kPropPassWord = 596, kPropFreeBase = 608, kPropFreeCount = 96, kPropValidOff = 6240, kPropStyleOff = 6144;
inline constexpr uint32_t kEntityField1184 = 1184, kEntityWord832 = 832, kEntityBones = 48, kBoneTableOff = 192;
// S103 D5: the game's own gates of the two loops (sub_82518950 signs, sub_82518EB8 foam hands), from their listings; docs\renderer\CROWD_PROPS_GATES.md has each with its address.
//   signs, whole function (0x8251898C-0x825189E0): [[M + 0x19710]] (the mesh's index count), the ring entry [[M + 4 (ring + 26049)]] (0 when [M+544] == -1) and [[M + 0x1970C]] must all be non-zero, else the function returns.
//   foam hands, per object / entry (0x82519048-0x825190B0, 0x82519320-0x825193B4): type b (byte elt+19 / the style byte), record M + 28 b: r26 = [[rec + 0x198E0]], r30 = [[rec + 0x198DC]], r5 = sub_82512EF0 =
//   [[M + 4 (7 b + ring + 26165)]] (0 when [M+544] == -1); any of the three zero: pass 1 LEAVES THE FUNCTION (0x825194F8), pass 2 skips the entry.
//   texture key: signs word0 = u32([[0x83767C5C] + 8] + 8 b) (b = byte elt+18 / the style byte); hands word0 = u32([[M + 4 (b + 26277)] + 4]); sub_82294F20 resolves it in the handle table P = [0x8392DD9C]:
//   begin = [P+8], end = [P+16], count = (end - begin) >> 3, idx = word0 & 0xFFFF, found when idx < count (unsigned) and [begin + 8 idx] == word0; the loop continues when the result [begin + 8 idx + 4] is 0.
//   distance (pass 1 only): when [0x836F3940] == 1, d2 = (eye.x - w.x)^2 + (eye.z - w.z)^2 (sub_82C32358, single precision, fmadds), the prop is dropped when d2 < f31 = float at 0x8201C26C (fcmpu / blt 0x82518CD0).
//   eye = the 16 bytes at 0x8206F2D0 unless [0x83929A68] != 0; then the scene camera's eye from the composer's copy [[0x836F3AA8] + 1488] (-(t . R) of the view, as ReadSceneCamera). The game reads +592 of the camera
//   object sub_82BA7748 returns for the draw thread; that this equals the composer's eye is [L] (slot 0 of 0x83929A40 is the HUD camera at the latch: inputs_core.h, run 7259 [C]).
inline constexpr uint32_t kSignMeshCountPtr = 0x19710u, kSignMeshVertPtr = 0x1970Cu, kSignRingBase = 26049u, kHandRecStride = 28, kHandCountPtr = 0x198E0u, kHandVertPtr = 0x198DCu, kHandResBase = 26165u, kHandKeyBase = 26277u;
inline constexpr uint32_t kTexMgrGlobal = 0x83767C5Cu, kHandleTableGlobal = 0x8392DD9Cu, kPropF31Addr = 0x8201C26Cu, kPropEyeDefault = 0x8206F2D0u, kSceneCamMgr = 0x836F3AA8u, kSceneCamView = 1488, kCamEnableWord = 0x83929A68u;
template <class M>
inline uint32_t U8(const M& mem, uint32_t a WWE_FEED_LOC_PARAM) {
  return (mem.u32(a & ~3u WWE_FEED_LOC_ARG) >> ((3u - (a & 3u)) * 8u)) & 0xFFu;
}
// A halfword at a 2-aligned guest address (big-endian memory read as words).
template <class M>
inline uint32_t U16(const M& mem, uint32_t a WWE_FEED_LOC_PARAM) {
  const uint32_t w = mem.u32(a & ~3u WWE_FEED_LOC_ARG);
  return (a & 2u) ? (w & 0xFFFFu) : (w >> 16);
}
// sub_82294F20: does the texture key resolve to a non-zero object? (listing 0x82294F20-0x82294F8C)
template <class M>
inline bool PropTextureResolves(const M& mem, uint32_t key0) {
  const uint32_t P = mem.u32(kHandleTableGlobal);
  if (!P) return false;
  const uint32_t begin = mem.u32(P + 8), end = mem.u32(P + 16);
  const uint32_t count = uint32_t(int32_t(end - begin) >> 3);
  const uint32_t idx = key0 & 0xFFFFu;
  if (idx >= count) return false;
  if (mem.u32(begin + 8u * idx) != key0) return false;
  return mem.u32(begin + 8u * idx + 4u) != 0;
}
// The signs' word0 for style byte b, 0 + `ok` false when the manager object is missing.
template <class M>
inline uint32_t SignKey0(const M& mem, uint32_t b, bool& ok) {
  const uint32_t mgr = mem.u32(kTexMgrGlobal);
  const uint32_t tab = mgr ? mem.u32(mgr + 8) : 0;
  ok = tab != 0;
  return ok ? mem.u32(tab + 8u * b) : 0u;
}
// S103 CSF (review 1): the FILE key of the picture the slot's DRAW handle names. sub_825E59F0 writes {handle, ptr} into the destination slot [[mgr + 8] + 8 slot] and the source slot [[mgr + 12] + 8 slot] and the key at
// [[mgr + 16] + 4 slot]; the phase-4 slot re-map (sub_825E5DF0) rewrites the destination from the source and never touches the keys, so the key at the same index can be stale. The key that belongs to the draw's handle H is
// the one at the source slot k holding the same H: keys[k]. No such slot (or a zero handle) = 0, a guest read. (plant g_sign_file_plant 2: the stale rule keys[b].)
template <class M>
inline uint32_t SignFileKey(const M& mem, uint32_t b) {
  const uint32_t mgr = mem.u32(kTexMgrGlobal);
  if (!mgr || b >= 16u) return 0;
  const uint32_t keys = mem.u32(mgr + 16);
  if (!keys) return 0;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_sign_file_plant == 2) return mem.u32(keys + 4u * b);
#endif
  const uint32_t dst = mem.u32(mgr + 8), src = mem.u32(mgr + 12);
  if (!dst || !src) return 0;
  const uint32_t h = mem.u32(dst + 8u * b);
  if (!h) return 0;
  for (uint32_t k = 0; k < 16u; ++k)
    if (mem.u32(src + 8u * k) == h) return mem.u32(keys + 4u * k);
  return 0;
}
template <class M>
inline uint32_t HandKey0(const M& mem, uint32_t manager, uint32_t b, bool& ok) {
  const uint32_t o = mem.u32(manager + 4u * (b + kHandKeyBase));
  const uint32_t kp = o ? mem.u32(o + 4) : 0;
  ok = kp != 0;
  return ok ? mem.u32(kp) : 0u;
}
// sign function, whole-function gate
template <class M>
inline bool SignFunctionOpen(const M& mem, uint32_t manager) {
  const uint32_t p20 = mem.u32(manager + kSignMeshCountPtr);
  const uint32_t r20 = p20 ? mem.u32(p20) : 0;
  const uint32_t ring = mem.u32(manager + kReadRingOffset);
  uint32_t r5 = 0;
  if (ring != 0xFFFFFFFFu) {
    const uint32_t e = mem.u32(manager + 4u * (ring + kSignRingBase));
    r5 = e ? mem.u32(e) : 0;
  }
  const uint32_t p31 = mem.u32(manager + kSignMeshVertPtr);
  const uint32_t r31 = p31 ? mem.u32(p31) : 0;
  return r20 && r5 && r31;
}
// foam hand resources of type b (sub_82518EB8 r26 / r5 / r30)
template <class M>
inline bool HandResourcesOk(const M& mem, uint32_t manager, uint32_t b) {
  const uint32_t rec = manager + kHandRecStride * b;
  const uint32_t pc = mem.u32(rec + kHandCountPtr);
  const uint32_t r26 = pc ? mem.u32(pc) : 0;
  const uint32_t ring = mem.u32(manager + kReadRingOffset);
  uint32_t r5 = 0;
  if (ring != 0xFFFFFFFFu) {
    const uint32_t t = mem.u32(manager + 4u * (7u * b + ring + kHandResBase));
    r5 = t ? mem.u32(t) : 0;
  }
  const uint32_t pv = mem.u32(rec + kHandVertPtr);
  const uint32_t r30 = pv ? mem.u32(pv) : 0;
  return r26 && r5 && r30;
}
struct PropDist {
  bool on = false;
  float eye_x = 0.f, eye_z = 0.f, f31 = 0.f;
};
template <class M>
inline PropDist MakePropDist(const M& mem) {
  PropDist d;
  d.on = mem.u32(kPhaseWord) == 1u;
  if (!d.on) return d;
  d.f31 = mem.f32(kPropF31Addr);
  d.eye_x = mem.f32(kPropEyeDefault), d.eye_z = mem.f32(kPropEyeDefault + 8);
  // S103 D5 review 2: slot 0 of 0x83929A40 is the HUD camera at the latch (inputs_core.h ReadSceneCamera, run 7259 [C]); the eye is the scene camera's, eye = -(t . R) of its world -> view (row vectors).
  // S110 CUTCROWD: the SAME source as ReadSceneCamera (handed_camera.h PickCamera): the block the game handed to its renderer, [[0x83929A68] + 256] + 240, else the composer's copy [0x836F3AA8] + 1488. On a gated
  // cut +1488 is already the next shot while the game's draw-thread camera is still the handed one. [L] that sub_82BA7748's camera +592 is this eye; the mirror census (props(shown) diff) settles it.
  if (mem.u32(kCamEnableWord)) {
    const uint32_t mgr = mem.u32(kSceneCamMgr);
    if (mgr) {
      float v[16];
      const scene_inputs::handed::Pick pick = scene_inputs::handed::PickCamera([&mem](uint32_t a, uint32_t& w) { w = mem.u32(a); return true; }, mgr);
      if (pick.src == scene_inputs::handed::kSrcBlock) std::memcpy(v, pick.view, sizeof v);
      else for (int i = 0; i < 16; ++i) v[i] = mem.f32(mgr + kSceneCamView + 4u * uint32_t(i));
      d.eye_x = -(v[12] * v[0] + v[13] * v[1] + v[14] * v[2]);
      d.eye_z = -(v[12] * v[8] + v[13] * v[9] + v[14] * v[10]);
    }
  }
  return d;
}
// the drop test: fcmpu f1, f31 / blt (an unordered compare does not drop)
inline bool PropTooNear(const PropDist& d, float wx, float wz) {
  const float dx = d.eye_x - wx, dz = d.eye_z - wz;
  const float sq = dx * dx;
  const float f1 = std::fmaf(dz, dz, sq);
  return f1 < d.f31;
}
// One prop row from a 4x4 world (host order words, 4 rows): c[r*4+k] = W[k][r]
struct PropBoneInfo {   // S103 SG: census only (the frozen-bone count)
  uint32_t sig = 0;
  float holder_t[3] = {};
};
inline void PushProp(LatchSnapshot& out, uint32_t site, uint32_t index, const uint32_t w[16], uint8_t hidden, uint32_t variant, uint32_t picture, uint32_t model_key = 0, const PropBoneInfo* bi = nullptr, uint32_t file_key = 0) {
  PropObs o;
  o.file_key = file_key;
  o.site = site, o.index = index, o.hidden = hidden, o.variant = uint8_t(variant), o.picture = picture, o.model_key = model_key;
  if (bi) o.bone_sig = bi->sig, o.holder_t[0] = bi->holder_t[0], o.holder_t[1] = bi->holder_t[1], o.holder_t[2] = bi->holder_t[2];
  float row[4][4];
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) std::memcpy(&row[r][k], &w[r * 4 + k], 4);
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) o.c[r * 4 + k] = row[k][r];
  out.props.push_back(o);
}
// The pass-1 object's gates shared by both loops (flags 0x4000 set / 0x1000 clear): the element, or 0.
template <class M>
inline uint32_t PropElement(const M& mem, uint32_t obj) {
  if (!obj) return 0;
  const uint32_t elt = mem.u32(obj + 64);
  if (!elt) return 0;
  const uint32_t fp = mem.u32(elt + 160);
  if (!fp) return 0;
  const uint32_t flags = U16(mem, fp);
  if (!(flags & 0x4000u) || (flags & 0x1000u)) return 0;
  return elt;
}
// W = B x M0 (sub_82296630, row vector), B the type's bone matrix; false when the bone table is missing
template <class M>
inline bool PropBoneWorld(const M& mem, uint32_t self, uint32_t manager, uint32_t elt, uint32_t t, uint32_t E, uint32_t w[16], PropBoneInfo* bi = nullptr) {
  const uint32_t holder = mem.u32(self + 4u * uint32_t(t + 16));
  if (!holder) return false;
  const int32_t idx = int16_t(U16(mem, holder + 34));
  const uint32_t bones = mem.u32(E + kEntityBones);
  const uint32_t table = bones ? mem.u32(bones + kBoneTableOff) : 0;
  if (!table) return false;
  (void)manager;
  uint32_t m0[16], b[16];
  mem.words(mem.u32(elt + 152), 16, m0);
  mem.words(table + 64u * uint32_t(idx), 16, b);
  if (bi) {   // S103 SG census: FNV-1a over the bone's 16 words; the holder's translation = M0 row 3
    uint32_t h = 2166136261u;
    for (int q = 0; q < 16; ++q) h = (h ^ b[q]) * 16777619u;
    bi->sig = h;
    for (int k = 0; k < 3; ++k) std::memcpy(&bi->holder_t[k], &m0[12 + k], 4);
  }
  float M0[4][4], B[4][4], W[4][4];
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) std::memcpy(&M0[r][k], &m0[r * 4 + k], 4), std::memcpy(&B[r][k], &b[r * 4 + k], 4);
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) {
      float acc = 0.f;
      for (int q = 0; q < 4; ++q) {
#ifdef RDR_CROWD_SNAP_PLANTS
        acc += (g_latch_plant == 6 ? M0[i][q] * B[q][k] : B[i][q] * M0[q][k]);   // planted: M0 x B instead of B x M0
#else
        acc += B[i][q] * M0[q][k];
#endif
      }
      W[i][k] = acc;
    }
  std::memcpy(w, W, 64);
  return true;
}
template <class M>
inline bool PropEntityOk(const M& mem, uint32_t E) {   // [E + 4 (206 + ([E+832] == 0))] == 1
  return mem.u32(E + 4u * (206u + (mem.u32(E + kEntityWord832) == 0 ? 1u : 0u))) == 1u;
}
// ---- S103 SG: the foam-hand types, as CStageAudienceManager::static_SetUpHandSignBoard (sub_8250B1E8, tag string 0x8201BCF8) makes them at arena load (out\s103\decomp\Q2_sub_8250B1E8.lst; claims_Q2 / V2) ----
// For each of the 6 wrestler slots s (r25 0..5, 0x8250B314-0x8250B4D8):
//   id   = sub_822C5100(G, s)  = s < 43 ? [G + 3580 s + 780] : 500                  G = [0x836F2D88] (lis -31889 / 11656)
//   ten  = sub_822C5128(G, s)  = s < 43 ? ([G + 3580 s + 724 + 16] % 100) / 10 : 0  (sub_822FF368 = word +16; sub_822C0D70 = the tens digit, unsigned)
//   data = sub_822CDD48(R, s)  = [R] + 3376 s, R = [0x836F2D98] (11672); [R] == 0 -> skip the slot (0x8250B34C tests the sum: [R] + 3376 s == 0)
//   idx  = sub_82506E30(id, ten) (HandAttireIdx below); -1 -> skip
//   h    = halfword [data + 2 (idx + 98)] (lhzx, unsigned); h == 0 -> skip (cmpwi / ble on a value 0..65535)
//   h > 19 -> h = 0 by the branchless test 0x8250B37C-0x8250B390 (V2), so the mesh key is 40999, which NO arena pac holds [C: tools\crowd\crowd_prop_keys.py, 48 of 48 audience sets] -> sub_82C351E8 gives
//          -1 -> skip (0x8250B3AC). So a counted type has 1 <= h <= 19: mesh 40999 + h (41000-41018, all present [C]), picture 42000 + h - 1 (all present [C]).
//   Then type b (= the count so far) gets it; the count is stored at [M + 0x198D0] (0x8250B4F4). The [C] presence of every 41000-41018 in all 48 sets stands in for sub_82C351E8 / sub_82C354D8 [L: the
//   loader's object chain sub_82C354D8 != 0 is not replicated; the count compare against [M + 0x198D0] catches any type it dropped].
inline constexpr uint32_t kHandSlotsGlobal = 0x836F2D88u, kHandDataGlobal = 0x836F2D98u, kHandTypeCountOff = 0x198D0u, kHandKeyMeshBase = 40999u, kHandKeyPictureBase = 42000u;
inline constexpr uint32_t kHandSlots = 6, kHandSlotRecStride = 3580, kHandSlotIdOff = 780, kHandSlotAttireOff = 724 + 16, kHandDataStride = 3376, kHandDataHalfBase = 98, kHandMaxH = 19;
// sub_82506E30 (79 instructions, listed): r10 = attire digit + 1; returns -1 / 0 / 1 (the index into the per-wrestler halfword table).
inline int32_t HandAttireIdx(int32_t id, int32_t ten) {
  const int32_t v = ten + 1;
  switch (id) {
    case 123: case 254: case 324: case 334: case 348: return v == 1 ? 0 : -1;
    case 101: return v <= 3 ? 0 : -1;
    case 109: return v == 7 ? 1 : 0;
    case 139: return (v == 9 || v == 10) ? 0 : (v == 1 || v == 2) ? 1 : -1;
    case 177: case 197: return v <= 4 ? 0 : -1;
    case 335: case 346: case 347: return v <= 2 ? 0 : -1;
    case 344: return (v == 3 || v == 4) ? 0 : -1;
    case 365: return v == 2 ? 1 : 0;
    default: return 0;
  }
}
inline constexpr uint32_t kHandPicWordsOff = 0x19A94u;   // the six picture-object words the loader stores (the generation key with the manager pointer)
inline void ResetHandMap(LatchSnapshot& out) {
  out.hand_types = 0, out.hand_types_game = 0, out.hand_map_ok = false;
  for (auto& v : out.hand_h) v = 0;
  for (auto& v : out.hand_idx) v = 0;
}
// The loader's inputs replayed (see the comment above): fills out.hand_h / out.hand_types. Reads through `mem` (a refusal sets mem.fail; the caller scopes it).
template <class M>
inline void ComputeHandMap(const M& mem, LatchSnapshot& out) {
  const uint32_t G = mem.u32(kHandSlotsGlobal), R = mem.u32(kHandDataGlobal);
  const uint32_t data0 = R ? mem.u32(R) : 0u;
  for (uint32_t s = 0; s < kHandSlots; ++s) {
    const int32_t id = G ? int32_t(mem.u32(G + kHandSlotRecStride * s + kHandSlotIdOff)) : 500;
    const int32_t ten = G ? int32_t((mem.u32(G + kHandSlotRecStride * s + kHandSlotAttireOff) % 100u) / 10u) : 0;
    const uint32_t data = data0 + kHandDataStride * s;
    if (!data0 || !data) continue;
#ifdef RDR_CROWD_SNAP_PLANTS
    const int32_t idx = g_latch_plant == 29 ? 0 : HandAttireIdx(id, ten);   // planted: the attire table is ignored
#else
    const int32_t idx = HandAttireIdx(id, ten);
#endif
    if (idx < 0) continue;
    const uint32_t h = U16(mem, data + 2u * uint32_t(idx + int32_t(kHandDataHalfBase)));
    if (h == 0 || h > kHandMaxH) continue;   // (h > 19 -> key 40999, absent from every set: skipped, see above)
    if (out.hand_types < kHandSlots) out.hand_h[out.hand_types] = uint8_t(h);
    ++out.hand_types;
  }
}
// The map, FROZEN per manager generation (review 3: the game's loader runs once; the live G / R words move after it): computed once for the key (manager pointer + the six picture words at [M + 0x19A94]) and then only
// compared with the loader's own count [M + 0x198D0] at every read. Only a clean read whose count matches and is > 0 is stored. A refused read leaves NO map (hand_map_ok false, types 0: a refusal giving 0 == 0 is not ok)
// and does not roll back the other props (Scoped, review 2). hand_idx[b] = the loader's strip index count of type b, a census column (review 3).
template <class M>
inline void HandTypeMap(const M& mem, uint32_t manager, LatchSnapshot& out) {
  ResetHandMap(out);
  if (!manager) return;
  const bool clean = Scoped(mem, [&] {
    out.hand_types_game = mem.u32(manager + kHandTypeCountOff);
    uint32_t pics[kHandSlots];
    for (uint32_t i = 0; i < kHandSlots; ++i) pics[i] = mem.u32(manager + kHandPicWordsOff + 4u * i);
    for (uint32_t b = 0; b < kHandSlots && b < out.hand_types_game; ++b) {
      const uint32_t pc = mem.u32(manager + kHandRecStride * b + kHandCountPtr);
      out.hand_idx[b] = pc ? mem.u32(pc) : 0u;
    }
    HandMapCache* cache = out.hand_cache;
    bool hit = false;
    if (cache) {
      std::lock_guard<std::mutex> lock(cache->mu);
      if (cache->valid && cache->manager == manager && std::memcmp(cache->pics, pics, sizeof pics) == 0) {
        out.hand_types = cache->types;
        for (uint32_t i = 0; i < kHandSlots; ++i) out.hand_h[i] = cache->h[i];
        hit = true;
      }
    }
    if (!hit) ComputeHandMap(mem, out);
    if (!mem.fail) {
      out.hand_map_ok = out.hand_types == out.hand_types_game && out.hand_types <= kHandSlots;
      if (!hit && cache && out.hand_map_ok && out.hand_types > 0) {
        std::lock_guard<std::mutex> lock(cache->mu);
        cache->valid = true, cache->manager = manager, cache->types = out.hand_types;
        std::memcpy(cache->pics, pics, sizeof pics);
        for (uint32_t i = 0; i < kHandSlots; ++i) cache->h[i] = out.hand_h[i];
      }
    }
  });
  if (!clean) ResetHandMap(out);
}
// The mesh key of hand type b (0 = not known: no map, or b past the types).
inline uint32_t HandMeshKey(const LatchSnapshot& s, uint32_t b) {
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 28) return 0;   // planted: the hand map is not used (every hand drew the default 41016: the pre-SG rule)
#endif
  return (s.hand_map_ok && b < s.hand_types) ? kHandKeyMeshBase + s.hand_h[b] : 0u;
}

// The signs (sites 0 / 1) and foam hands (sites 2 / 3), WITH the game's gates: a prop a gate would not draw is kept as a row with `hidden` set (CROWD_PROPS_ROWS.md: never dropped, never drawn).
// Needs the crowd node. See the header comment and the constants above.
template <class M>
inline void ReadProps(const M& mem, uint32_t node, uint32_t manager, LatchSnapshot& out) {
  const uint32_t self = node - 32;
  const bool pass1 = mem.u32(self + kPropPassWord) == 0;
  const bool sign_open = manager ? SignFunctionOpen(mem, manager) : false;
#ifdef RDR_CROWD_SNAP_PLANTS
  const bool dist_planted_off = g_latch_plant == 11, tex_planted_off = g_latch_plant == 12, exit_planted_off = g_latch_plant == 13, global_planted_off = g_latch_plant == 14;
#else
  constexpr bool dist_planted_off = false, tex_planted_off = false, exit_planted_off = false, global_planted_off = false;
#endif
  PropDist dist = MakePropDist(mem);
  if (dist_planted_off) dist.on = false;
  HandTypeMap(mem, manager, out);   // S103 SG: the hand types b -> h (mesh 40999 + h)
  if (pass1) {   // pass 1: the objects on seated people
    uint32_t objs[kPropObjectCount];
    mem.words(self + kPropObjects, kPropObjectCount, objs);
    // ---- signs (sub_82518950) ----
    SoftEach(mem, out, kSoftProps, kPropObjectCount, [&](uint32_t j) {
      const uint32_t elt = PropElement(mem, objs[j]);
      if (!elt) return;
      const int32_t t = int8_t(U8(mem, elt + 23));
      if (t < -10 || t > 100) return;
      const uint32_t E = manager ? mem.u32(manager + 4u * uint32_t(t + 10)) : 0;
      if (!E) return;
      const uint32_t x = mem.u32(E + kEntityField1184);
      if (x != 14006u && x != 14007u) return;   // sub_82694200(x) && !sub_82693198(x)
      if (!PropEntityOk(mem, E)) return;
      uint32_t w[16];
      PropBoneInfo bi;
      if (!PropBoneWorld(mem, self, manager, elt, uint32_t(t), E, w, &bi)) return;
      const uint32_t b = U8(mem, elt + 18);
      bool kok = false;
      const uint32_t key0 = SignKey0(mem, b, kok);
      uint8_t hidden = kPhNone;
      float wx, wz;
      std::memcpy(&wx, &w[12], 4), std::memcpy(&wz, &w[14], 4);
      if (!sign_open && !global_planted_off) hidden = kPhSignGlobal;
      else if (!tex_planted_off && !(kok && PropTextureResolves(mem, key0))) hidden = kPhTexture;
      else if (dist.on && PropTooNear(dist, wx, wz)) hidden = kPhDistance;
      PushProp(out, 0, j, w, hidden, b, key0, 0, &bi, SignFileKey(mem, b));
    });
    // ---- foam hands (sub_82518EB8) ----
    bool exited = false;
    SoftEach(mem, out, kSoftProps, kPropObjectCount, [&](uint32_t j) {
      const uint32_t elt = PropElement(mem, objs[j]);
      if (!elt) return;
      const uint32_t b = U8(mem, elt + 19);
      uint8_t hidden = kPhNone;
      if (!exited && !HandResourcesOk(mem, manager, b)) exited = true, hidden = kPhResource;   // 0x825190A0-0x825190B0: the game leaves the function here
      else if (exited) hidden = kPhExited;
      if (exit_planted_off) hidden = kPhNone, exited = false;
      const int32_t t = int8_t(U8(mem, elt + 23));
      if (t < -10 || t > 100) return;
      const uint32_t E = manager ? mem.u32(manager + 4u * uint32_t(t + 10)) : 0;
      if (!E) return;
      const uint32_t x = mem.u32(E + kEntityField1184);
      if (x != 14127u && x != 14128u) return;   // sub_82693198(x)
      if (!PropEntityOk(mem, E)) return;
      uint32_t w[16];
      PropBoneInfo bi;
      if (!PropBoneWorld(mem, self, manager, elt, uint32_t(t), E, w, &bi)) return;
      bool kok = false;
      const uint32_t key0 = HandKey0(mem, manager, b, kok);
      float wx, wz;
      std::memcpy(&wx, &w[12], 4), std::memcpy(&wz, &w[14], 4);
      if (hidden == kPhNone) {
        if (!tex_planted_off && !(kok && PropTextureResolves(mem, key0))) hidden = kPhTexture;
        else if (dist.on && PropTooNear(dist, wx, wz)) hidden = kPhDistance;
      }
      PushProp(out, 2, j, w, hidden, b, key0, HandMeshKey(out, b), &bi);
    });
  } else {   // pass 2: the free props
    const uint32_t base = self + kPropFreeBase;
    SoftEach(mem, out, kSoftProps, kPropFreeCount, [&](uint32_t j) {   // signs: valid == 1
      if (U8(mem, base + kPropValidOff + j) != 1u) return;
      uint32_t w[16];
      mem.words(base + 64u * j, 16, w);
      const uint32_t b = U8(mem, base + kPropStyleOff + j);
      bool kok = false;
      const uint32_t key0 = SignKey0(mem, b, kok);
      uint8_t hidden = kPhNone;
      if (!sign_open && !global_planted_off) hidden = kPhSignGlobal;
      else if (!tex_planted_off && !(kok && PropTextureResolves(mem, key0))) hidden = kPhTexture;
      PushProp(out, 1, j, w, hidden, b, key0, 0, nullptr, SignFileKey(mem, b));
    });
    SoftEach(mem, out, kSoftProps, kPropFreeCount, [&](uint32_t j) {   // foam hands: valid == 2
      if (U8(mem, base + kPropValidOff + j) != 2u) return;
      uint32_t w[16];
      mem.words(base + 64u * j, 16, w);
      const uint32_t b = U8(mem, base + kPropStyleOff + j);
      uint8_t hidden = kPhNone;
      bool kok = false;
      uint32_t key0 = 0;
      if (!HandResourcesOk(mem, manager, b)) hidden = kPhResource;
      else {
        key0 = HandKey0(mem, manager, b, kok);
        if (!tex_planted_off && !(kok && PropTextureResolves(mem, key0))) hidden = kPhTexture;
      }
      PushProp(out, 3, j, w, hidden, b, key0, HandMeshKey(out, b));
    });
  }
}

// S103 D6: the pictures of the SHOWN props: the texture object behind each distinct key, read through guest_texture.h (handle -> wrapper -> header -> fetch dwords -> pixel memory). Needs mem.phys(addr, n, dst).
// A key that cannot be read stays in the list with its why (the prop is then a hole, never a grey guess).
template <class M>
inline void ReadPictures(const M& mem, LatchSnapshot& out) {
  for (const PropObs& p : out.props) {
    if (p.hidden || !p.picture) continue;
    uint32_t fkey = 0, fset = 0;
    const bool cand = FileCandidate(p.site, p.file_key, p.model_key, fkey, fset);
    if (cand && SignFileKeys::Verified(fkey, fset)) continue;   // S103 CSF (H-CS1): a key with a FILE picture (a sign: audience.pac; a foam hand: the arena set's 42000 + h - 1), checked once against the guest's, is never read back: no read, no row
    bool seen = false;
    for (const PictureObs& q : out.pictures) seen = seen || q.key == p.picture;
    if (seen) continue;
    PictureObs o;
    o.key = p.picture;
    o.site = uint8_t(p.site);   // S103 CSF: the first prop that named this key
    guest_tex::Resolved r;
    guest_tex::Shape s;
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return mem.phys(a, n, d); };
    const guest_tex::Why w = guest_tex::ReadHandleTexture(mem, p.picture, phys, r, s, o.bytes, kCrowdPictureBytes);
    o.why = uint8_t(w);
    std::memcpy(o.fetch, r.fetch, sizeof o.fetch);   // S103 CSF census: what the registry gave and the shape decoded from it, kept for a refused read too
    o.shape_w = s.width, o.shape_h = s.height, o.shape_fmt = s.game_format, o.pitch_blocks = s.pitch_blocks, o.tiled = s.tiled ? 1 : 0, o.packed = uint8_t((r.fetch[5] >> 11) & 1u);
    if (w == guest_tex::Why::Ok && o.bytes.size() > kCrowdPictureBytes) o.why = uint8_t(guest_tex::Why::Size);
    if (cand && o.why == uint8_t(guest_tex::Why::Ok)) SignFileKeys::Check(fkey, fset, o.bytes.data(), o.bytes.size());   // S103 CSF review 2: the one-time check, the first time this key would be file-served: the guest's blocks against the file's; a mismatch keeps the key on guest reads
    if (o.why == uint8_t(guest_tex::Why::Ok)) o.width = s.width, o.height = s.height, o.format = s.game_format;
    else o.bytes.clear();
    out.pictures.push_back(std::move(o));
  }
}

// ---- S103 D5: the single people = the sign / foam-hand holders (sub_8276E7B0, vtable 0x8202B8DC; docs\renderer\CROWD_PROPS_GATES.md 4 and 7) ----
// Per object of the node's 52-word list (listing 0x8276E7B0-0x8276EA9C): entity x = [[M+16] + 4 [obj+28]] (sub_82507018: ptr non-null, 0 <= index < 60; null = the game returns); element e = [obj+64], flags halfword f at [e+160]:
// hidden when f & (0x8000 if [M+248] != 0 else 0x1000). World written to x+32: f & 0x2000 or f & 0x4000: the 64 bytes at [e+152] as stored; else byte e+25 >= 128: sub_82C334A8 from x+80 / e+80 (NOT replicated:
// `world_kind` 3, the object is unread); else the 64 bytes at M + 64 (2 ([obj+24] - 20 + 722) + (M[0] == 0)). Palette: node count n = vtable+8 of the entity (sub_82BD3AB8: [x+300] when [x+296] != 0, else forward to [x+220], else -1),
// block M + 2304 + 1408 (2 ([obj+24] - 20) + (M[0] == 0)) (the double buffer the update side fills: sub_8250A6A0 for the type skeletons' +180 arrays, sub_8250B7F0 for the 32 slot objects at M+340), n matrices; then bone (int16)[obj+32]
// = bone x RotY(-[e+116]) (sub_82C32DE0: m00 = cos, m02 = -sin, m20 = sin, m22 = cos, m11 = m33 = 1; sub_82296630 dst = r5 x r4). S103 C1: the screen test sub_8276E2C0 IS replicated now (SingleScreen, the draw thread's camera noted at the entry); before C1 it was not (its camera is the thread's: the same
// question as the props' eye), so the reader holds a SUPERSET of the drawn set. [C listing; no walk observes the singles, so no mirror: the unit is the proof; the block holds the +180 arrays (locals) [C writers], which
// the draw's material pass turns into skin matrices: whether the stands' skin palette form applies is [U]]
// S103 C1 (R15): the +220 forward. sub_82BD3AB8 (count) / sub_82BD3AF8 (table) load [x+220] and bctr to ITS vtable slot +8 / +12 (0x82BD3AE0-0x82BD3AF0). Only a CSkinModel (vtable 0x8206AF90, whose slots +8 / +12 ARE
// sub_82BD3AB8 / sub_82BD3AF8: image words 0x8206AF98 / 0x8206AF9C [C]) is followed by the same rule; any other class's getter is not replicated: `forward` is set and NOTHING is read through the foreign object
// (run 32: following it as a CSkinModel read [Y+220] = 0x79646566, ASCII 'ydef', then +296 / +220 of that: the 0x7964668E / 0x79646642 access-violation pairs, 38070 soft holes). The depth cap 4 is ours (a cycle guard).
// S103 WC R1: the constants, SkinGetters and the data-object leaf are hoisted into skin_data.h (shared with ring_rows_core.h NodeCount); the names stay reachable here.
using skin::kSkinModelVtable;
using skin::kSkinForwardOff;
using skin::kSkinNodeTableOff;
using skin::kSkinNodeCountOff;
// S103 SN: the data object. MakeModel (sub_824EB400) stores sub_82BD41C8's 16-byte object at [CSkinModel+220] (sub_82BDB460); sub_82C275D8 builds it with vtable 0x8206C7A0, whose slots are
// +8 sub_82C27620 (d = [y+12]; d ? [d+24] : 0 = the node count) and +12 sub_82C27640 (d ? [d+32] : 0 = the node table, stride 80 as [x+296]'s) [C listings, docs/research/s103/SN_singles.md A1]. Run 34: every shown holder had
// [x+296] == 0 and this object at [x+220], so all 52396 were `forward`. Any other class stays `forward` (its first vtable word is kept for the census: LatchSnapshot::forward_vt).
using skin::kSkinDataVtable;
using skin::kSkinDataBlockOff;
using skin::kSkinDataCountOff;
using skin::kSkinDataTableOff;
using skin::SkinGetters;
// sub_82BD3AB8 / sub_82BD3AF8 along +220 (a CSkinModel is followed; the data object ends the chain; anything else = forward). Every guest read goes through mem (range-checked: a refusal sets mem.fail).
template <class M>
inline SkinGetters ResolveSkin(const M& mem, uint32_t x) {
  SkinGetters g;
  for (int depth = 0; depth < 4 && x; ++depth) {
    if (const uint32_t t = mem.u32(x + kSkinNodeTableOff)) return g.count = int32_t(mem.u32(x + kSkinNodeCountOff)), g.table = t, g;
    const uint32_t y = mem.u32(x + kSkinForwardOff);
    if (!y) break;
    const uint32_t vt = mem.u32(y);
    bool skin = vt == kSkinModelVtable;
#ifdef RDR_CROWD_SNAP_PLANTS
    if (g_latch_plant == 20) skin = true;   // planted: any +220 object is read as a CSkinModel (the run-32 fault)
#endif
    if (skin) {
      x = y;
      continue;
    }
    if (vt == kSkinDataVtable) {
      bool skip_block = false;
#ifdef RDR_CROWD_SNAP_PLANTS
      skip_block = g_latch_plant == 30;   // planted: the +12 indirection skipped (count read at [y+24])
#endif
      skin::ReadDataGetters([&mem](uint32_t a, uint32_t& v) { return v = mem.u32(a), true; }, y, g, skip_block);   // the shared leaf (skin_data.h): mem.u32 range-checks and sets mem.fail itself
      return g;
    }
    g.forward = true, g.forward_vt = vt;
    return g;
  }
  return g;
}
// S103 C1 review fix 5: the entity x itself must be a CSkinModel (its vtable word 0x8206AF90) before its +296 / +300 are read; kShForward / unread `forward` otherwise (a getter of another class, not replicated).
template <class M>
inline bool EntityIsSkin(const M& mem, uint32_t x) {
  if (!x) return false;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 26) return true;   // planted: the entity's class is not checked
#endif
  return mem.u32(x) == kSkinModelVtable;
}
template <class M>
inline int32_t EntityNodeCount(const M& mem, uint32_t x, bool* forward = nullptr, uint32_t* forward_vt = nullptr) {   // sub_82BD3AB8 (see ResolveSkin)
  if (!EntityIsSkin(mem, x)) {   // S103 C1 review fix 5: the draw calls the ENTITY's own vtable +8 (0x8276E9CC): only a CSkinModel's is sub_82BD3AB8; nothing read through another class
    if (forward) *forward = true;
    if (forward_vt && x) *forward_vt = mem.u32(x);
    return -1;
  }
  const SkinGetters g = ResolveSkin(mem, x);
  if (forward) *forward = g.forward;
  if (forward_vt) *forward_vt = g.forward_vt;
  return g.count;
}
// S103 D6: the pose the game makes from the copied locals (the draw's vtable +44 = sub_82BD4F80 -> sub_82BD3038 [C listing]: vcall +92 = sub_82BD30E8 (the world update), vcall +96 = sub_82BD2010 -> +100 = sub_82BD35B0 (the skin)).
// sub_82BD30E8 [C 0x82BD31EC-0x82BD3398]: per node k, parent p = the word at T + 48 + 80 k (T = the bone table: [x+296] when non-zero, else the same getter on [x+220], sub_82BD3AF8); p == -1: world[k] = local[k] x root
// (root = [x+96], made by sub_82BD3F48 = [x+32] when there is no parent object at [x+16]); else world[k] = local[k] x world[p] (rows: out row i = sum_j local[i][j] world[p] row j, the vmaddfp chain); the early exits: [x+312] != 0,
// no [x+220] and no [x+296]. sub_82BD35B0 [C]: skin[k] = inv[k] x world[k] ([x+224] = the inverse bind; sub_82296630 (dst, a = world, b = inv): dst = b x a, as SkinFromWorld). The root stays OUT of the palette here: the row's world
// is it (v x inv x chain x root, the same product). Everything read here is memory the update side wrote; a thing that cannot be read (no bone table, a parent that is not an earlier node, a parent object, no inverse bind) = not modelled.
template <class M>
inline uint32_t BoneTable(const M& mem, uint32_t x) {   // sub_82BD3AF8 along +220 (ResolveSkin), only for a CSkinModel
  if (!EntityIsSkin(mem, x)) return 0;   // (review fix 5)
  return ResolveSkin(mem, x).table;
}
template <class M>
inline bool ReadSingleSkin(const M& mem, uint32_t x, SingleObs& s) {
  if (mem.u32(x + 312) != 0 || mem.u32(x + 16) != 0) return false;   // the pose is skipped (stale) / a parent object (vcall +28 of it): not modelled
  const uint32_t inv_p = mem.u32(x + 224), table = BoneTable(mem, x);
  if (!inv_p || !table || s.nodes == 0 || s.nodes > kPaletteNodes) return false;
  uint32_t iw[kPaletteNodes * 16];
  mem.words(inv_p, s.nodes * 16u, iw);
  std::vector<float> chain(size_t(s.nodes) * 16);
  s.skin.assign(size_t(s.nodes) * 16, 0.f);
  for (uint32_t k = 0; k < s.nodes; ++k) {
    const int32_t p = int32_t(mem.u32(table + 48u + 80u * k));
    if (p != -1 && (p < 0 || uint32_t(p) >= k)) return false;   // the game would read a world it has not made yet this pass
    const float* L = s.palette.data() + size_t(k) * 16;
    float* C = chain.data() + size_t(k) * 16;
    if (p == -1) std::memcpy(C, L, 64);
    else {
      const float* P = chain.data() + size_t(p) * 16;
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
          float acc = 0.f;
          for (int q = 0; q < 4; ++q) acc += L[i * 4 + q] * P[q * 4 + j];
          C[i * 4 + j] = acc;
#ifdef RDR_CROWD_SNAP_PLANTS
          if (g_latch_plant == 17) { acc = 0.f; for (int q = 0; q < 4; ++q) acc += P[i * 4 + q] * L[q * 4 + j]; C[i * 4 + j] = acc; }   // planted: parent x local
#endif
        }
    }
    float inv[16];
    std::memcpy(inv, iw + size_t(k) * 16, 64);
    float* S = s.skin.data() + size_t(k) * 16;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        float acc = 0.f;
        for (int q = 0; q < 4; ++q) acc += inv[i * 4 + q] * C[q * 4 + j];
        S[i * 4 + j] = acc;
      }
  }
  for (float f : s.skin)
    if (!std::isfinite(f)) return false;
  return true;
}
// S103 C1 (R15): the singles' own screen test, sub_8276E2C0 (listing 0x8276E2C0-0x8276E424, 90 insns), called at 0x8276E9A0 with f1 = [x+80], f2 = [x+84] + c(0x8201BDDC), f3 = [x+88], f4 = c(0x8201BDD8) after the
// world was written to x+32 (so (f1, f3) = the world's row 3 x / z, f2 its y + the offset). Returns 0 -> the holder is NOT drawn (0x8276E9A8). NOT the floor seats' camera: sub_82BA7748 ignores r3 and returns the word at
// 0x83929A40 + 4 * the calling thread's slot (listing 0x82BA7748-0x82BA7774), while the floor test reads [[0x83929A68] + 256]. Form:
//   [0x83929A68] == 0 or the camera C == 0: return 0 (hidden) (0x8276E2EC-0x8276E304);
//   Pj = the 4x4 at C + 176; f30 = Pj[0][0] K, f29 = Pj[1][1] K (0x8276E354-0x8276E360, K = f4); Pc = V x Pj with V = C + 240 (sub_822505B8, as the floor's); v = (f1, f2, f3, one) x Pc (sub_82C32A88);
//   hidden when w < near (blt 0x8276E3AC); inv = one / w; px = out0 inv, r = inv f30, py = out1 inv, ry = inv f29; hidden when r + px < lo, px - r > one, py - ry < lo (blt / bgt: a NaN is not taken, so it passes each); DRAWN unless ry + py > one: `ble` at 0x8276E404 is "branch if not greater" (bf gt), TAKEN on an unordered compare, so a NaN anywhere DRAWS (S103 C1 review fix 1).
// Constants (image words [C]): K = 120.0 (0x8201BDD8: the floor's K), the y offset -120.0 (0x8201BDDC), one = 1.0 (0x8218B8C0, also the `hi` bound: f31 reloaded at 0x8276E390), near = 0.0 (0x8218B8CC), lo = -1.0 (0x82004734).
inline constexpr uint32_t kSingleCamGate = 0x83929A68u, kSingleScreenK = 0x8201BDD8u, kSingleScreenYOff = 0x8201BDDCu, kSingleScreenOne = 0x8218B8C0u, kSingleScreenNear = 0x8218B8CCu, kSingleScreenLo = 0x82004734u;
// S105 A2 (ultrawide / tall screens): our two ports of the game's 16:9 screen tests (the floor seats' sub_8250E7E0 and the singles' sub_8276E2C0) read the game's own 16:9 projection; on a wider / taller
// frame the seats and sign holders past the 16:9 box would pop at the sides. Columns 0 / 1 of the copied projection are scaled by 1 / k and 1 / t, as the S101 floor hook does (crowd_wide_core.h ApplyFloor),
// BEFORE the radius scales (m00 K, m11 K) are taken and before the view multiply. Set once at start (start_settings.cpp) from the crowd switch's Scale; 1.0f = the game's test (default, 16:9, `off`).
inline std::atomic<uint32_t> g_screen_sx_bits{0x3F800000u}, g_screen_sy_bits{0x3F800000u};
// (review crowd F2) The singles' test (sub_8276E2C0) has no wide hook in the STREAM build: a walked frame draws the game's own holders, culled at 16:9, so there the singles' port keeps the game's 16:9 test
// (g_single_screen_scaled false, set by start_settings.cpp under WWE2K14_STREAM()) and CompareScreenLocked compares like with like. The floor seats' test has the S101 hook in both builds: it is always scaled.
inline std::atomic<bool> g_single_screen_scaled{true};
inline void ScaleScreenProjection(float pj[4][4], bool single = false) {
  if (single && !g_single_screen_scaled.load(std::memory_order_relaxed)) return;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 31) return;   // planted twin screen_scale_ignored: the screen tests keep the game's 16:9 projection
#endif
  const uint32_t bx = g_screen_sx_bits.load(std::memory_order_relaxed), by = g_screen_sy_bits.load(std::memory_order_relaxed);
  float sx, sy;
  std::memcpy(&sx, &bx, 4), std::memcpy(&sy, &by, 4);
  if (sx == 1.0f && sy == 1.0f) return;
  for (int r = 0; r < 4; ++r) pj[r][0] *= sx, pj[r][1] *= sy;
}
struct SingleScreen {
  enum State : uint8_t { kUnknown = 0, kHideAll = 1, kOn = 2 } state = kUnknown;
  float pc[4][4] = {}, f30 = 0.f, f29 = 0.f, yoff = 0.f, one = 1.f, near_w = 0.f, lo = 0.f;
};
template <class M>
inline void InitSingleScreen(const M& mem, uint32_t cam, SingleScreen& st) {
  if (cam == kScreenCamUnknown) return;   // the draw thread's camera was not noted: the test cannot run (kUnknown)
  if (!mem.u32(kSingleCamGate) || !cam) return void(st.state = SingleScreen::kHideAll);
  uint32_t pj_w[16], v_w[16];
  mem.words(cam + 176, 16, pj_w);
  mem.words(cam + 240, 16, v_w);
  float pj[4][4], v[4][4];
  std::memcpy(pj, pj_w, sizeof pj);
  ScaleScreenProjection(pj, true);   // S105 A2 (the singles' test)
  std::memcpy(v, v_w, sizeof v);
  const float k = mem.f32(kSingleScreenK);
  st.f30 = pj[0][0] * k, st.f29 = pj[1][1] * k;
  st.yoff = mem.f32(kSingleScreenYOff), st.one = mem.f32(kSingleScreenOne), st.near_w = mem.f32(kSingleScreenNear), st.lo = mem.f32(kSingleScreenLo);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float acc = 0.f;
      for (int q = 0; q < 4; ++q) acc += v[i][q] * pj[q][j];
      st.pc[i][j] = acc;
    }
  st.state = SingleScreen::kOn;
}
// true = drawn (sub_8276E2C0 returns 1). Only for state kOn / kHideAll.
inline bool SingleScreenShows(const SingleScreen& st, float x, float y, float z) {
  if (st.state != SingleScreen::kOn) return false;
  const float vec[4] = {x, y + st.yoff, z, st.one};
  float o[4];
  for (int j = 0; j < 4; ++j) {
    float acc = 0.f;
    for (int i = 0; i < 4; ++i) acc += vec[i] * st.pc[i][j];
    o[j] = acc;
  }
  const float w = o[3];
  if (w < st.near_w) return false;
  const float inv = st.one / w;
  const float px = inv * o[0], r = inv * st.f30, py = inv * o[1], ry = inv * st.f29;
  if (r + px < st.lo) return false;
  if (px - r > st.one) return false;
  if (py - ry < st.lo) return false;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 25) return ry + py <= st.one;   // planted: the pre-review form (a NaN hides)
#endif
  return !(ry + py > st.one);
}

// S103 C1: every object of the node's list with the singles' vtable is accounted HERE (ReadFloor leaves them): drawn and read (a row), not drawn (no entity, flags, the screen test), or unread with a reason
// (forward, no_nodes, world3, bind, screen = no camera to test with). A refused read is a hole (SoftEach). Holders still unread are REAL: drawn by the game and not modelled; they keep blocking the stamp.
template <class M>
inline void ReadSingles(const M& mem, uint32_t node, uint32_t manager, LatchSnapshot& out) {
  uint32_t objs[kNodeObjectCount];
  mem.words(node + kNodeObjects, kNodeObjectCount, objs);
  const uint32_t mask = mem.u32(manager + kManagerFloorFlagMode) != 0 ? 0x8000u : 0x1000u;
  uint32_t z = mem.u32(manager) == 0 ? 1u : 0u;   // cntlzw(M[0]) >> 5
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 16) z ^= 1u;   // planted: the buffer parity inverted
#endif
  SingleScreen screen;
  InitSingleScreen(mem, out.screen_cam, screen);
  SoftEach(mem, out, kSoftSingles, kNodeObjectCount, [&](uint32_t j) {   // S103 D8
    const uint32_t obj = objs[j];
    if (!obj || mem.u32(obj) != kFloorSingleVtable) return;
    SingleObs s;
    s.object_slot = j, s.object = obj, s.slot = mem.u32(obj + 24), s.index = mem.u32(obj + 28);
    const int32_t idx = int32_t(s.index);
    const uint32_t arr = mem.u32(manager + 16);
    const uint32_t x = (arr && idx >= 0 && idx < 60) ? mem.u32(arr + 4u * uint32_t(idx)) : 0;
    const uint32_t elt = mem.u32(obj + 64);
    const uint32_t fp = elt ? mem.u32(elt + 160) : 0;
    const uint32_t flags = fp ? U16(mem, fp) : 0;
    s.entity = x;
    // S103 C1 (R15, item 3): the entity array is 60 = 3 x 20: index -> (type = idx % 20, lod = idx / 20) [L from the layout]; the entity is fetched like any other (sub_82507018), the palette block by [obj+24] - 20 [C].
    s.type = s.index % 20u, s.lod = s.index / 20u;
#ifdef RDR_CROWD_SNAP_PLANTS
    if (g_latch_plant == 22) s.type = s.index, s.lod = 0;   // planted: the index is taken as the type (the pre-C1 rule)
#endif
    if (!x) s.hidden = kShNoEntity;   // the game returns (0x8276E7E4): not drawn
    else if (!elt || !fp) s.hidden = kShFlags, MarkUnread(out, kUrOther);   // no flags halfword: the game would fault reading it (lhz at 0x8276E804); not modelled
    else if (flags & mask) s.hidden = kShFlags;   // 0x8276E818: not drawn
    if (s.hidden) {
      out.singles.push_back(std::move(s));
      return;
    }
    if (s.index >= 20u) ++out.lod_holders;
    uint32_t w[16] = {};
    const uint32_t N20 = s.slot - 20u;
    if ((flags & 0x2000u) || (flags & 0x4000u)) mem.words(mem.u32(elt + 152), 16, w), s.world_kind = 1;
    else if (U8(mem, elt + 25) >= 128u) s.world_kind = 3;   // sub_82C334A8: not replicated
    else mem.words(manager + 64u * (2u * (N20 + 722u) + z), 16, w), s.world_kind = 2;
    if (s.world_kind == 3) {   // the screen test reads the world the game made: not known, so neither is the verdict
      MarkUnread(out, kUrWorld3);
      out.singles.push_back(std::move(s));
      return;
    }
    {   // the screen test (0x8276E980-0x8276E9A8) on the world just written: row 3 = x+80 / x+84 / x+88
      float wx, wy, wz;
      std::memcpy(&wx, &w[12], 4), std::memcpy(&wy, &w[13], 4), std::memcpy(&wz, &w[14], 4);
      bool shown = SingleScreenShows(screen, wx, wy, wz);
#ifdef RDR_CROWD_SNAP_PLANTS
      if (g_latch_plant == 21) shown = true;   // planted: the screen test is not run (every holder counts as drawn: the pre-C1 superset)
#endif
      if (screen.state == SingleScreen::kUnknown && !shown) {   // no camera: the game's verdict is unknown; read on (the census sees the holder) but it stays unread
        s.screen_unknown = true;   // (screen_verdict stays 0: not run)
      } else if (!shown) {
        s.hidden = kShScreen, s.screen_verdict = 2;
        ++out.screen_hidden;
        out.singles.push_back(std::move(s));
        return;
      }
      if (shown && screen.state != SingleScreen::kUnknown) s.screen_verdict = 1;   // (review fix 2: compared with the game's own verdict on walked frames)
    }
    bool forward = false;
    uint32_t fvt = 0;
    const int32_t n = EntityNodeCount(mem, x, &forward, &fvt);
    if (n <= 0 || n > int32_t(kPaletteNodes)) {
      if (forward && !out.forward_vt) out.forward_vt = fvt;
      s.hidden = forward ? kShForward : kShNoNodes;
      MarkUnread(out, forward ? kUrForward : kUrNoNodes);
      out.singles.push_back(std::move(s));
      return;
    }
    s.nodes = uint32_t(n);
    {
      float row[4][4];
      for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) std::memcpy(&row[r][k], &w[r * 4 + k], 4);
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) s.c[r * 4 + k] = row[k][r];
    }
    uint32_t pw[kPaletteNodes * 16];
    mem.words(manager + 2304u + 1408u * (2u * N20 + z), s.nodes * 16u, pw);
    s.palette.resize(size_t(s.nodes) * 16);
    std::memcpy(s.palette.data(), pw, s.palette.size() * 4);
    const int32_t bone = int16_t(U16(mem, obj + 32));
    s.bone = uint32_t(bone);
    if (bone >= 0 && uint32_t(bone) < s.nodes) {
      const float ang = -mem.f32(elt + 116);
      s.yaw = ang;
      const float cs = std::cos(ang), sn = std::sin(ang);
      const float R[16] = {cs, 0, -sn, 0, 0, 1, 0, 0, sn, 0, cs, 0, 0, 0, 0, 1};
      float* B = s.palette.data() + size_t(bone) * 16;   // (S103 D6: the skin below is made after this block)
      float out16[16];
      for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) {
          float acc = 0.f;
#ifdef RDR_CROWD_SNAP_PLANTS
          if (g_latch_plant == 15) { for (int q = 0; q < 4; ++q) acc += R[i * 4 + q] * B[q * 4 + k]; continue; }   // planted: rot x bone
#endif
          for (int q = 0; q < 4; ++q) acc += B[i * 4 + q] * R[q * 4 + k];   // dst = bone x rot (sub_82296630 r5 x r4)
          out16[i * 4 + k] = acc;
        }
      std::memcpy(B, out16, sizeof out16);
    }
    if (ReadSingleSkin(mem, x, s)) s.bind_ok = true;   // S103 D6
    if (!s.bind_ok) MarkUnread(out, kUrBind);
    else if (s.screen_unknown) MarkUnread(out, kUrScreen);   // read, but whether the game draws it is not known (no camera): not a row, unread
    out.singles.push_back(std::move(s));
  });
}

// The floor seats' screen test, sub_8250E7E0 (listing 0x8250E7E0-0x8250E9B4; docs crowd.md 124-150), run inside the entry that an OURS frame skips: the reader applies it itself.
// camera block C = [[0x83929A68] + 256] (0: the function returns, nothing is hidden); Pj = the 4x4 at C + 176, V = C + 240; Pc = V x Pj (sub_822505B8: r3 = r4 x r3, row vector); f29 = Pj[0][0] * K and
// f28 = Pj[1][1] * K with K = 120.0 (read BEFORE the multiply, 0x8250E858-64); per seat: v = (x, y - K, z, 1.0) x Pc (sub_82C32A88, out[j] = sum_i v[i] Pc[i][j]); hidden when w < 80.0 (0x8250E918);
// then inv = 1 / w, px = out0 inv, py = out1 inv, rr = [[e+152]+20] * inv; hidden when px + rr f29 < lo, px - rr f29 > hi, py - rr f28 < lo or py + rr f28 > hi (lo -0.9, hi 0.9: 0x8201BFB8 / 0x8201BF00).
inline constexpr uint32_t kCamTable = 0x83929A68u, kScreenK = 0x8201BDD8u, kScreenNear = 0x82003F6Cu, kScreenOne = 0x8218B8C0u, kScreenLo = 0x8201BFB8u, kScreenHi = 0x8201BF00u;
struct ScreenTest {
  bool on = false;
  float pc[4][4] = {}, k = 0.f, near_w = 0.f, one = 1.f, lo = 0.f, hi = 0.f, f29 = 0.f, f28 = 0.f;
};
template <class M>
inline void InitScreenTest(const M& mem, ScreenTest& st) {
  const uint32_t cam_obj = mem.u32(kCamTable);
  const uint32_t C = cam_obj ? mem.u32(cam_obj + 256) : 0;
  if (!C) return;   // 0x8250E800-0x8250E808: no camera block, the function returns and hides nothing
  uint32_t pj_w[16], v_w[16];
  mem.words(C + 176, 16, pj_w);
  mem.words(C + 240, 16, v_w);
  float pj[4][4], v[4][4];
  std::memcpy(pj, pj_w, sizeof pj);
  ScaleScreenProjection(pj);   // S105 A2 (the floor seats' test: always scaled)
  std::memcpy(v, v_w, sizeof v);
  st.k = mem.f32(kScreenK), st.near_w = mem.f32(kScreenNear), st.one = mem.f32(kScreenOne), st.lo = mem.f32(kScreenLo), st.hi = mem.f32(kScreenHi);
  st.f29 = pj[0][0] * st.k;
  st.f28 = pj[1][1] * st.k;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float acc = 0.f;
      for (int q = 0; q < 4; ++q) acc += v[i][q] * pj[q][j];
      st.pc[i][j] = acc;
    }
  st.on = true;
}
// true = the screen test hides the seat (position x y z, member matrix element [1][1] = m11)
inline bool ScreenHides(const ScreenTest& st, float x, float y, float z, float m11) {
  if (!st.on) return false;
  const float vec[4] = {x, y - st.k, z, st.one};
  float out[4];
  for (int j = 0; j < 4; ++j) {
    float acc = 0.f;
    for (int i = 0; i < 4; ++i) acc += vec[i] * st.pc[i][j];
    out[j] = acc;
  }
  const float w = out[3];
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 7) {   // planted: the near cut is ignored
    if (!(w > 0.f)) return true;
  } else
#endif
  if (w < st.near_w) return true;
  const float inv = st.one / w;
  const float px = out[0] * inv, py = out[1] * inv;
  const float f9 = m11 * inv, f13 = f9 * st.f29, f11 = f9 * st.f28;
  return (px + f13 < st.lo) || (px - f13 > st.hi) || (py - f11 < st.lo) || (py + f11 > st.hi);
}

// The seated floor people. out.unread += objects of the class the reader does not model. Needs the crowd node (out.node != 0).
template <class M>
inline void ReadFloor(const M& mem, uint32_t node, uint32_t manager, LatchSnapshot& out) {
  const uint32_t lod = mem.u32(node + kNodeLodWord);   // [r4 + 592] with r4 = node - 32 (0x8276EAC4)
  const bool flag_mode = mem.u32(manager + kManagerFloorFlagMode) != 0;
  const uint32_t hide_mask = flag_mode ? 0x8000u : 0x1000u;
  const float f31 = mem.f32(mem.u32(kPhaseWord) == 4 ? kF31Phase4 : kF31Zero);
  ScreenTest screen;
  InitScreenTest(mem, screen);
  // sub_82512E80(manager, g = type, 0, 0) (0x82512E80-0x82512EE0): the group's stream must exist or the floor draw returns (0x8276EB34): the read ring in {0, 1}, [M + 628 + 28 g] > 0, the stream pointer
  // [M + 4 (153 + 7 g + ring)] and its first word non-null (review item 6)
  const uint32_t read_ring = mem.u32(manager + kReadRingOffset);
  auto group_ok = [&](uint32_t type) {
    if (read_ring > 1) return false;
    if (int32_t(mem.u32(manager + 628u + 28u * type)) <= 0) return false;
    const uint32_t stream = mem.u32(manager + 4u * (153u + 7u * type + read_ring));
    return stream != 0 && mem.u32(stream) != 0;
  };
  uint32_t objs[kNodeObjectCount];
  mem.words(node + kNodeObjects, kNodeObjectCount, objs);
  SoftEach(mem, out, kSoftFloor, kNodeObjectCount, [&](uint32_t j) {   // S103 D8: one object at a time; a refused read makes THAT object a hole (unread), not a declined snapshot
    const uint32_t obj = objs[j];
    if (!obj) return;
    const uint32_t vt = mem.u32(obj);
    if (vt == kFloorSingleVtable) return;   // S103 C1: sub_8276E7B0's single people are ReadSingles' to account (drawn / hidden / unread with a reason)
    if (vt != kFloorVtable) {   // any other class (R15: the only factory sub_826D0688 makes the two above [C]; [L] that nothing else stores into the list): hidden by the flags halfword of its one element, else unread
      const uint32_t elt = mem.u32(obj + 64);
      const uint32_t fp = elt ? mem.u32(elt + 160) : 0;
      if (!(fp && (U16(mem, fp) & hide_mask))) MarkUnread(out, kUrOther);
      return;
    }
    const uint32_t count = mem.u32(obj + 20), type = mem.u32(obj + 24);
    if (count == 0) return;
    if (type >= kTypes || count > 4096) {
      MarkUnread(out, kUrOther);
      return;
    }
    // S103 C1 (R15, item 4): sub_8276EAA0 draws the group g = 20 lod + type (r31 = 4 (lod + 4 lod) + type, 0x8276EAC4-0x8276EB10), lod = [r4 + 592] = the node's word; g indexes sub_82512E80 (0x8276EB2C) and the
    // 28-byte group records (mulli r31, 28 at 0x8276EC18: [M + 620 + 28 g], [M + 624 + 28 g]) [C]. g past the 3 x 20 groups is not modelled.
    uint32_t g = kFloorGroupsPerLod * lod + type;
#ifdef RDR_CROWD_SNAP_PLANTS
    if (g_latch_plant == 23) g = type;   // planted: the group ignores the LOD (the LOD 0 group's stream)
#endif
    if (lod >= kLods) {
      MarkUnread(out, kUrFloorLod);
      return;
    }
    if (!group_ok(g)) return;   // the game's floor draw returns for this group: no seat is drawn (not unread)
    const uint32_t elems = mem.u32(obj + 64);
    if (!elems) {
      MarkUnread(out, kUrOther);
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t e = elems + 176u * i;
      const uint32_t mp = mem.u32(e + 152), pp = mem.u32(e + 156), fp = mem.u32(e + 160);
      if (!mp || !pp || !fp) continue;
      // S103 C1 review fix 3: NO type-byte test. The game's per-seat loop (0x8276EC44-0x8276ED74) reads only [e+152], [e+156] (+ its word +12), [e+160]; the group's type is [obj+24] and the stream / palette
      // are group g's. A seat whose byte e+23 disagrees is still drawn as the group's type, so the reader draws it too (the walk's decode, crowd_floor.h, still rejects it: a mirror `extra`, never a silent skip).
#ifdef RDR_CROWD_SNAP_PLANTS
      if (g_latch_plant == 27 && U8(mem, e + 23) != type) continue;   // planted: the old silent skip
#endif
      if (U16(mem, fp) & hide_mask) continue;
      uint32_t m[16], p[4];
      mem.words(mp, 16, m);
      mem.words(pp, 4, p);
      {
        float px_, py_, pz_, m11_;
        std::memcpy(&px_, &p[0], 4), std::memcpy(&py_, &p[1], 4), std::memcpy(&pz_, &p[2], 4), std::memcpy(&m11_, &m[5], 4);
        if (ScreenHides(screen, px_, py_, pz_, m11_)) continue;   // sub_8250E7E0's verdict (item 2)
      }
      MemberObs o;
      o.block = type, o.lod = lod, o.index = i, o.floor = true;   // (S103 C1: the group's LOD; the body and palette slot follow it)
      float row[4][4];
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) std::memcpy(&row[r][k], &m[r * 4 + k], 4);
      for (int k = 0; k < 4; ++k) std::memcpy(&row[3][k], &p[k], 4);
      row[3][1] -= f31;   // 0x8276ECE8-0x8276ECF0
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) {
          o.c[r * 4 + k] = row[k][r];
#ifdef RDR_CROWD_SNAP_PLANTS
          if (g_latch_plant == 5 && k < 3) o.c[r * 4 + k] = row[r][k];   // planted: the matrix is not transposed
#endif
        }
      out.members.push_back(o);
    }
  });
}

// The whole snapshot: stands members, cards, palettes. `out` is cleared first; out.node (0 = unknown) is the crowd node object r3 of sub_82519508 (the gate [node+556]).
// Returns None with EMPTY members when the entry's own gates say the frame draws no crowd.
template <class M>
inline Why ReadSnapshot(const M& mem, LatchSnapshot& out) {
  const uint32_t node = out.node, entry_ring = out.entry_ring;
  out.Clear();
  out.node = node, out.entry_ring = entry_ring;   // (scope, served, want_palettes are inputs Clear leaves alone)
  if (node && mem.u32(node + kNodeGateOff) == 0) return mem.fail ? (out.fail_stage = kStageNode, out.fail_addr = FailAddr(mem, 0), Why::Unreadable) : Why::None;   // 0x82519548-50
  const uint32_t manager = mem.u32(mgr::kMgrGlobal);
  if (!manager) return Why::NoManager;
  if (mem.u32(manager + kMgrDrawGateOff) == 0) return mem.fail ? (out.fail_stage = kStageHeader, out.fail_addr = FailAddr(mem, 0), Why::Unreadable) : Why::None;       // 0x82519564-6C
  const uint32_t live_ring = mem.u32(mgr::kRingIndex);
  uint32_t ring = entry_ring <= 1 ? entry_ring : live_ring;   // S103 D8
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_latch_plant == 19) ring = live_ring;   // planted: the ring is read at the latch
#endif
  // ^ the table ring the ENTRY saw (the game reads it inside the entry), when the hook noted it
  out.ring_moved = entry_ring <= 1 && live_ring != entry_ring;
  if (ring > 1) return Why::BadRing;
  out.read_ring = mem.u32(manager + kReadRingOffset);
  out.write_ring = mem.u32(manager + kWriteRingOffset);
  if (out.read_ring > 1) return Why::NoPalette;
  if (out.want_palettes && out.write_ring > 1) return Why::NoPalette;   // (a write ring < 0 is a state the game tests, 0x82509D20; an OURS install does not need it: item 5)
  const bool pass = mem.u32(manager + kPassGateOff0) != 0 || mem.u32(manager + kPassGateOff1) != 0 || mem.u32(manager + kPassGateOff2) != 0;   // 0x82519760-88: else no stands, no cards
  wwe2k14::crowd_flips::Latch fl_latch;   // S110 CROWDFLIPS
  if (pass && out.scope != 2) {   // S103 D9: scope 2 (the entry half) reads no lists
    // the walk classifies first (0x825185F4-0x82518608): a pending request means the lists are stale. On an OURS frame CrowdKeepRegistration has done that serve step (out.served): not a decline (item 4).
    if (!out.served && mem.u32(manager + mgr::kRequestOff) != 0) return Why::ClassifyPending;
    if (!EnsureSinTable(mem)) return out.fail_stage = kStageSin, out.fail_addr = FailAddr(mem, 0), Why::Unreadable;
    const uint32_t e = mgr::kTable + ring * mgr::kRingStride;
    const uint32_t recs = mem.u32(e + mgr::kRecPtrOff), counts_ptr = mem.u32(e + mgr::kCountPtrOff), scales = mem.u32(e + mgr::kScalePtrOff);
    if (!recs || !counts_ptr || !scales) return Why::BadPointer;
    uint32_t counts[mgr::kTypes], gate[mgr::kTypes];
    mem.words(counts_ptr, mgr::kTypes, counts);
    mem.words(e + kCardGateOff, mgr::kTypes, gate);
    static thread_local uint32_t fl[mgr::kMaxPerType], gr[mgr::kMaxPerType], sc[mgr::kMaxPerType], rc[mgr::kMaxPerType * 4];
    const CachedSinCos sincos;
    fl_latch.Open();   // S110 CROWDFLIPS: census only (crowd_flip_count.h); nothing below reads it
    // pass 1: the stands (type i, group g, member k), pass 2: the cards (type i, member k): the walk's order
    for (int pass_no = 0; pass_no < 2; ++pass_no) {
      for (uint32_t i = 0; i < mgr::kTypes; ++i) {
        const int32_t n = int32_t(counts[i]);
        if (n <= 0) continue;
        if (n > int32_t(mgr::kMaxPerType)) return Why::TooMany;
        bool gate_ok = true;
        if (pass_no == 1) {
          gate_ok = gate[i] != 0;
#ifdef RDR_CROWD_SNAP_PLANTS
          if (g_latch_plant == 4) gate_ok = true;   // planted: the card gate is ignored
#endif
        }
        if (!gate_ok) continue;
        mem.words(e + mgr::kFlagsOff + mgr::kFlagTypeStride * i, uint32_t(n), fl);
        if (pass_no == 0) mem.words(e + mgr::kGroupOff + mgr::kFlagTypeStride * i, uint32_t(n), gr);
        if (pass_no == 0)
          for (uint32_t k = 0; k < uint32_t(n); ++k)   // S110 CROWDFLIPS: the DRAWN class per member: culled / group 0-2 / card
            fl_latch.Set(i, k, (fl[k] & 0x80000000u) ? uint8_t(0) : (fl[k] & 0x40000000u) ? wwe2k14::crowd_flips::kCardClass : uint8_t(1 + (gr[k] > 2 ? 2 : gr[k])));
        mem.words(scales + mgr::kFlagTypeStride * i, uint32_t(n), sc);
        mem.words(recs + mgr::kTypeStride * i, uint32_t(n) * 4, rc);
        auto emit = [&](uint32_t k, uint32_t group, bool card) {
          MemberObs o;
          o.block = i, o.lod = card ? kCardLayer : group, o.index = k, o.card = card;
          float scale;
          std::memcpy(&scale, &sc[k], 4);
          mgr::MemberConstants(&rc[k * 4], scale, sincos, o.c);
          out.members.push_back(o);
        };
        if (pass_no == 0) {
          for (uint32_t g = 0; g < mgr::kGroups; ++g)
            for (uint32_t k = 0; k < uint32_t(n); ++k)
              if (!(fl[k] & 0xC0000000u) && gr[k] == g) emit(k, g, false);
        } else {
          uint32_t mask = kCardSkipMask;
#ifdef RDR_CROWD_SNAP_PLANTS
          if (g_latch_plant == 2) mask = 0xC0000000u;   // planted: the block draw's mask
#endif
          for (uint32_t k = 0; k < uint32_t(n); ++k)
            if (!(fl[k] & mask)) emit(k, 0, true);
        }
      }
    }
  }
  if (mem.fail) return out.fail_stage = kStageLists, out.fail_addr = FailAddr(mem, 0), Why::Unreadable;   // S103 D8: the stands / cards are HARD
  fl_latch.Finish();   // S110 CROWDFLIPS: the lists were read whole (no-op when they were not read)
  if (node && out.scope != 1) {   // S103 D9: scope 1 (the latch half) leaves the in-place parts to the entry's read   // S103 D3c   // before the stands pass in the walk (0x82519734 < 0x82519788); order is immaterial to the record (compared sorted)
    // S103 D8: each optional part on its own flag, on top of the per-object scopes inside (a refused read in a part's setup drops that part only)
    const auto part = [&](uint32_t which, auto&& fn) {
      const size_t mb = out.members.size(), pb = out.props.size(), sb = out.singles.size(), qb = out.pictures.size();
      const UnreadMark ub(out);
      uint32_t fa = 0;
      if (Scoped(mem, fn, &fa)) return;
      RollbackSoft(out, mb, pb, sb, qb, ub);
      if (which == kSoftProps) ResetHandMap(out);   // the props are gone with their keys: no hand map is claimed either (review 7)
      NoteSoft(out, which, fa);
    };
    part(kSoftFloor, [&] { ReadFloor(mem, node, manager, out); });
    part(kSoftProps, [&] { ReadProps(mem, node, manager, out); });
    part(kSoftPictures, [&] { ReadPictures(mem, out); });
    part(kSoftSingles, [&] { ReadSingles(mem, node, manager, out); });
  }
  if (out.want_palettes && out.scope != 2)
    if (const Why p = ReadPalettes(mem, manager, out.palettes); p != Why::None) return out.fail_stage = kStagePalettes, out.fail_addr = FailAddr(mem, 0), p;
  if (mem.fail) return out.fail_stage = kStageTail, out.fail_addr = FailAddr(mem, 0), Why::Unreadable;
  return Why::None;
}

}  // namespace rdr_scene::live::crowd::latch
