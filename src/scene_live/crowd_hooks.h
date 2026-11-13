// The crowd contributor's game side (Session 101, lane 27; docs\systems\crowd.md): reads the crowd's members and palettes from guest memory
// at the engine's own submit points and hands them to rdr_scene::live::crowd::The() (crowd_contrib.h). READ ONLY: nothing the game computes
// or draws changes. Every read is behind rdr_scene::live::Enabled() (one relaxed load) and every address below is from a listing
// (tools\analysis\list_function.py), noted beside it.
//
//  - Members: scene_crowd.h's op-21 recorder hook (sub_82BB27D8) calls OnOp21 first; the block draw sub_82518340 (r4 = the block / person type i,
//    loop var r18 = the LOD group g, r16 = i + 20 g, member record pointer r30, 16-byte records) and the card loop sub_82512F70 (r3 = i at its
//    entry, record pointer r29) reach it with r4 = 76, r6 = 3 and r5 = the 12 constants.
//  - Palettes: a hook on sub_82509CB8 (the manager's copy of the 20 person types' skeleton matrices into this[608] blocks, stride 1408, group
//    slot = lod * 20 + type; its only caller is sub_8250BAF0, inside the manager's critical section), read right after it returns: the blocks
//    cannot tear because the writer is the function we just returned from. The alternative (reading at a block's first member on the draw
//    thread) would race with the manager's thread; this point does not.
//  - Draw bracket: sub_82519508 (the crowd draw entry; no direct callers, a vtable word). The members seen between its entry and exit are the
//    snapshot (a tiled frame runs it once per tile: the last complete run is published).
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include "crowd_contrib.h"
#include "crowd_floor.h"  // S101 lane 82: the seated floor people's op-21 decode (pure; unit-tested by tools/scene/crowd_make --contract)
#include "crowd_skip_core.h"  // S101 lane 50: t_draws (the P2b decider), reset in the sub_82519508 hook below
#include "bake_skip_hook.h"  // lane 155: the card bake's invisible cut (the pass hook sub_827FD698; the observers are called below and in scene_crowd.h)
#include "card_thin_hook.h"  // lane 175: the far cards thin record (observers called below and in scene_crowd.h)
#include "member_thin_hook.h"  // lane 272: the stands / floor members thin record (observers called below and in scene_crowd.h)

REX_EXTERN(__imp__sub_82509CB8);
REX_EXTERN(__imp__sub_82519508);

namespace wwe2k14::crowd_live {

// Call-site return addresses (the same constants as scene_crowd.h: 0x82518558 after bl 0x82BB27D8 at 0x82518554; 0x825130B0 after 0x825130AC).
inline constexpr uint32_t kBlockOp21Return = 0x82518558;
inline constexpr uint32_t kCardOp21Return = 0x825130B0;
// The member arrays: listing 0x82518360-0x8251848C (block draw) and 0x82512F7C-0x82512FF8 (card loop): the table base 0x835E4CD8 (lis -31906 /
// addi 19672), a double-buffer index at 0x83762F84 (lis -31882 / lwz 12164) times 122972 (0x1E05C, ori 57436 on 0x10000), the array pointer
// at +0 of that entry, plus 12288 * i (rlwinm (3 i) << 12), 16-byte records, 768 per type.
inline constexpr uint32_t kMemberTable = 0x835E4CD8;
inline constexpr uint32_t kRingIndex = 0x83762F84;
inline constexpr uint32_t kRingStride = 0x1E05C;
inline constexpr uint32_t kTypeStride = 12288;
inline constexpr uint32_t kMembersPerType = 768;  // 12288 / 16; C2-1 says 768 per body type
// The palette storage: sub_82509CB8 loads the block pointer from this+608 (lwz r11,608(r26)), 1408 bytes (22 matrices) per slot.
inline constexpr uint32_t kPaletteBasePtr = 608;
inline constexpr uint32_t kPaletteStride = 1408;

inline thread_local uint32_t t_card_block = 0xFFFFFFFFu;  // the card loop's i (its r3 at entry; r3 is reused by the time of the op-21 call)

// Set by RegisterCrowd(): the hooks do nothing until the contributor is registered (a writer that has sinks but no crowd must not pay for it).
inline std::atomic<bool> g_registered{false};
inline bool Active() { return g_registered.load(std::memory_order_relaxed) && rdr_scene::live::Enabled(); }

// The other op-21 recorders of the crowd draw (listing: bl 0x82BB27D8 at 0x82518D48 / 0x82518E50 in sub_82518950, 0x825192B8 / 0x8251949C in
// sub_82518EB8; the return address is the call + 4): the 32 sign / foam-hand skeletons and the 96 entries at obj+608 (S101 lane 78 corrected "likely the seated
// floor people": those are sub_8276EAA0, kFloorOp21Return in crowd_floor.h; these are the sign / foam-hand props [L]). Not modelled: counted unknown.
inline constexpr uint32_t kOtherOp21Return[4] = {0x82518D4C, 0x82518E54, 0x825192BC, 0x825194A0};
// S101 lane 82: sub_8276EAA0 (the seated floor people) was NOT one of these (lane 78 corrected the guess above: those four are the sign / foam-hand props); its
// op 21 is rdr_scene::live::crowd::kFloorOp21Return (crowd_floor.h). Observed only while g_floor_observe is on (--wwe2k14_rdr_crowd_floor with the crowd promotion).
inline std::atomic<bool> g_floor_observe{false};
inline rdr_scene::live::crowd::FloorStats g_floor_stats;  // the observer's own counters (the live check reads them from the log line below)
inline constexpr bool IsCrowdOp21(uint32_t lr) {
  return lr == kBlockOp21Return || lr == kCardOp21Return || lr == kOtherOp21Return[0] || lr == kOtherOp21Return[1] ||
         lr == kOtherOp21Return[2] || lr == kOtherOp21Return[3] || lr == rdr_scene::live::crowd::kFloorOp21Return;
}

inline uint32_t Be32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, address), 4);
  return std::byteswap(v);
}
inline float BeF(uint8_t* base, uint32_t address) { return std::bit_cast<float>(Be32(base, address)); }

// S101 lane 82: guest memory for crowd_floor.h's pure decode.
struct FloorMem {
  uint8_t* base;
  uint32_t u32(uint32_t a) const { return Be32(base, a); }
  uint8_t u8(uint32_t a) const { return *rex::memory::GuestPtr(base, a); }
  float f32(uint32_t a) const { return BeF(base, a); }
};

// The member's index in its type's array, or kUnknown when the record pointer does not fit the expected array (counted by the contributor).
inline uint32_t MemberIndex(uint8_t* base, uint32_t record, uint32_t type) {
  const uint32_t ring = Be32(base, kRingIndex);
  if (ring > 1) return rdr_scene::live::crowd::kUnknown;  // (a double buffer: 0 / 1)
  const uint32_t array_base = Be32(base, kMemberTable + ring * kRingStride);
  const uint32_t first = array_base + kTypeStride * type;
  if (!array_base || record < first || (record - first) % 16 || (record - first) / 16 >= kMembersPerType) return rdr_scene::live::crowd::kUnknown;
  return (record - first) / 16;
}

// Called by scene_crowd.h's op-21 hook before it does anything else. Observes only.
inline void OnOp21(PPCContext& ctx, uint8_t* base) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (!IsCrowdOp21(lr)) return;
  if (lr == rdr_scene::live::crowd::kFloorOp21Return) {  // (before the r4 / r6 test: a wrong r4 / r6 is FloorReject::Regs, counted live)  // S101 lane 82: a seated floor person (sub_8276EAA0): decoded by crowd_floor.h, recorded as a member (or counted rejected)
    if (!g_floor_observe.load(std::memory_order_relaxed)) return;
    rdr_scene::live::crowd::MemberObs o;
    FloorMem mem{base};
    const rdr_scene::live::crowd::FloorReject why = rdr_scene::live::crowd::DecodeFloorMember(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r26.u32, ctx.r29.u32, ctx.r31.u32, mem, o);
    ++g_floor_stats.seen;
    ++g_floor_stats.rejected[uint32_t(why)];
    if (wwe2k14::member_thin::RawMode() > 0) {   // lane 272: the floor member's world as recorded (the tally of the thin record) and its class (the palette test of the op 15 that follows)
      uint32_t w[12];
      for (uint32_t k = 0; k < 12; ++k) w[k] = Be32(base, ctx.r5.u32 + 4 * k);
      wwe2k14::member_thin::OnMemberOp21(wwe2k14::member_thin::kFloor, w, why == rdr_scene::live::crowd::FloorReject::None, o.block, o.lod);
    }
    if (why == rdr_scene::live::crowd::FloorReject::None) {
      ++g_floor_stats.taken;
      rdr_scene::live::crowd::The().NoteMember(o);
    } else {
      rdr_scene::live::crowd::The().NoteUnmodelled();  // seen but not understood: counted unknown, never recorded
    }
    return;
  }
  if (ctx.r4.u32 != 76 || ctx.r6.u32 != 3) return;
  if (lr != kBlockOp21Return && lr != kCardOp21Return) {  // the populations not modelled: seen, counted unknown, not recorded
    rdr_scene::live::crowd::The().NoteUnmodelled();
    return;
  }
  rdr_scene::live::crowd::MemberObs o;
  if (lr == kBlockOp21Return) {
    const uint32_t group = ctx.r18.u32, r16 = ctx.r16.u32;  // r16 = i + 20 * group (0x825183D0, += 20 at 0x82518594)
    if (group < rdr_scene::live::crowd::kLods && r16 >= 20 * group && r16 - 20 * group < rdr_scene::live::crowd::kTypes) {
      o.block = r16 - 20 * group;
      o.lod = group;
      o.index = MemberIndex(base, ctx.r30.u32, o.block);
    }
  } else {
    o.card = true;
    o.lod = rdr_scene::live::crowd::kCardLayer;
    if (t_card_block < rdr_scene::live::crowd::kTypes) {
      o.block = t_card_block;
      o.index = MemberIndex(base, ctx.r29.u32, o.block);
    }
  }
  const uint32_t m = ctx.r5.u32;
  if (lr == kBlockOp21Return && wwe2k14::member_thin::RawMode() > 0) {   // lane 272: the stands member's world as recorded and its class
    uint32_t w[12];
    for (uint32_t k = 0; k < 12; ++k) w[k] = Be32(base, m + 4 * k);
    wwe2k14::member_thin::OnMemberOp21(wwe2k14::member_thin::kStands, w, o.block < rdr_scene::live::crowd::kTypes, o.block, o.lod);
  }
  if (o.card && wwe2k14::card_thin::RawMode() > 0) {   // lane 175 (review B1): the card's world as recorded, summed into this frame's tally (the delivery check of the thin record)
    uint32_t w[12];
    for (uint32_t k = 0; k < 12; ++k) w[k] = Be32(base, m + 4 * k);
    wwe2k14::card_thin::g_counts.NoteWorld(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), w);
  }
  for (uint32_t k = 0; k < 12; ++k) o.c[k] = BeF(base, m + 4 * k);
  rdr_scene::live::crowd::The().NoteMember(o);
}

// The manager's words (the block draw reads them: lis -31889 / lwz 19904 = the global [0x836F4DC0], then 544(r19) at 0x82518400; sub_82509CB8
// reads 548(r26) at 0x82509D14 and 248(r26) at 0x82509D24).
inline constexpr uint32_t kManagerGlobal = 0x836F4DC0;
inline constexpr uint32_t kReadRingOffset = 544;   // the ring the draw reads the skinned vertex streams from (0 / 1)
inline constexpr uint32_t kWriteRingOffset = 548;  // the ring the skin loops write (and the palette copy's gate: >= 0)
// The LOD 1 / 2 remap lists: this + 0x20000 - 30452 (addis 2 / addi -30452 at 0x82509CC4 / 0x82509CD8), 88 bytes (22 signed words, -1 = skip) per
// type, 1760 per LOD (addi r7,r7,1760 at 0x82509DB8).
inline constexpr uint32_t kRemapOffset = 0x20000u - 30452u;

// After sub_82509CB8 returned (inside the manager's critical section). Records ONLY what the game wrote: LOD 0 = the type's node count entries
// (the count is a virtual call's result in the game: not readable here, so it is derived as the larger of the last LOD-remap source index + 1 and the
// last non-zero row + 1 [U]), LOD 1 / 2 = slots up to the last remap entry that is not -1 (skipped slots are identity, never written by the game).
inline void OnPalettes(uint8_t* base, uint32_t manager) {
  using namespace rdr_scene::live::crowd;
  const uint32_t pal = Be32(base, manager + kPaletteBasePtr);
  const uint32_t ring = Be32(base, manager + kWriteRingOffset);
  if (!pal || ring > 1) return;  // (the game copies only when [548] >= 0; the rings are 0 / 1)
  float f[kPaletteNodes * 16];
  for (uint32_t type = 0; type < kTypes; ++type) {
    uint32_t remap[2][kPaletteNodes];
    uint32_t src_bound = 0;  // the LOD 0 node count's lower bound from what the remaps read
    for (uint32_t lod = 1; lod < kLods; ++lod)
      for (uint32_t j = 0; j < kPaletteNodes; ++j) {
        const uint32_t r = Be32(base, manager + kRemapOffset + 1760 * (lod - 1) + 88 * type + 4 * j);
        remap[lod - 1][j] = r;
        if (r < kPaletteNodes && r + 1 > src_bound) src_bound = r + 1;
      }
    // LOD 0
    const uint32_t at0 = pal + type * kPaletteStride;
    uint32_t nonzero = 0;
    for (uint32_t k = 0; k < kPaletteNodes * 16; ++k) {
      f[k] = BeF(base, at0 + 4 * k);
      if (f[k] != 0.f) nonzero = k / 16 + 1;
    }
    const uint32_t nodes0 = nonzero > src_bound ? nonzero : src_bound;
    if (nodes0) The().SetPalette(ring, type, 0, f, nodes0), wwe2k14::member_thin::OnPalette(ring, type, 0);
    // LOD 1 / 2
    for (uint32_t lod = 1; lod < kLods; ++lod) {
      const uint32_t at = pal + (lod * kTypes + type) * kPaletteStride;
      uint32_t last = 0;
      for (uint32_t j = 0; j < kPaletteNodes; ++j) {
        const bool written = remap[lod - 1][j] != 0xFFFFFFFFu;
        for (uint32_t k = 0; k < 16; ++k) f[j * 16 + k] = written ? BeF(base, at + 64 * j + 4 * k) : (k % 5 == 0 ? 1.f : 0.f);
        if (written) last = j + 1;
      }
      if (last) The().SetPalette(ring, type, lod, f, last), wwe2k14::member_thin::OnPalette(ring, type, lod);
    }
  }
}

// Registration of the contributor: the ONE startup call (before the writer's sinks can attach). Until it runs, every hook returns at once.
inline void RegisterCrowd() {
  rdr_scene::live::Register(&rdr_scene::live::crowd::The());
  g_registered.store(true, std::memory_order_relaxed);
}

}  // namespace wwe2k14::crowd_live

REX_HOOK_RAW(sub_82509CB8) {
  const uint32_t manager = ctx.r3.u32;
  wwe2k14::crowd_skip::InvalidateMap();  // S101 lane 50: a new manager update (this runs once per sub_8250BAF0 call, before the skin loops): the buffer -> group map is rebuilt once
  __imp__sub_82509CB8(ctx, base);
  if (wwe2k14::crowd_live::Active()) wwe2k14::crowd_live::OnPalettes(base, manager);
}

REX_HOOK_RAW(sub_82519508) {
  wwe2k14::bake_skip::OnCrowdDraw();  // lane 155
  wwe2k14::card_thin::OnCrowdDraw();  // lane 175
  if (wwe2k14::member_thin::RawMode() > 0) {  // lane 272: a new crowd draw run (the stands / floor members); the ring the draw reads is the palette set the eligibility test uses
    const uint32_t mgr = wwe2k14::crowd_live::Be32(base, wwe2k14::crowd_live::kManagerGlobal);
    wwe2k14::member_thin::OnCrowdDraw(mgr ? wwe2k14::crowd_live::Be32(base, mgr + wwe2k14::crowd_live::kReadRingOffset) : 0);
  }
  wwe2k14::crowd_skip::t_draws.BeginRun();  // S101 lane 50: a new run of the crowd draw entry (P2b keeps its first block draw)
  if (!wwe2k14::crowd_live::Active()) {
    __imp__sub_82519508(ctx, base);
    return;
  }
  const uint32_t manager = wwe2k14::crowd_live::Be32(base, wwe2k14::crowd_live::kManagerGlobal);
  rdr_scene::live::crowd::The().BeginDraw(manager ? wwe2k14::crowd_live::Be32(base, manager + wwe2k14::crowd_live::kReadRingOffset) : 0);
  __imp__sub_82519508(ctx, base);
  rdr_scene::live::crowd::The().EndDraw();
  if (wwe2k14::crowd_live::g_floor_observe.load(std::memory_order_relaxed)) {  // S101 lane 82: the floor observer's register reads, the live check's line (every 600th crowd draw)
    static uint32_t n = 0;
    const auto& s = wwe2k14::crowd_live::g_floor_stats;
    if (++n % 600 == 1)
      REXLOG_INFO("[scene][crowd] floor op 21: seen {}, taken {}, rejected: regs {}, type {}, group {}, lod {}, index {}, element {}", s.seen, s.taken, s.rejected[1], s.rejected[2], s.rejected[3], s.rejected[4], s.rejected[5], s.rejected[6]);
  }
}
