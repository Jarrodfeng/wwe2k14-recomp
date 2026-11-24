// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene {


// S103 SWF (lane E's range, ids 51-53; docs\systems\liquid_map.md; docs\research\s103\SWB.md): the wrestlers' sweat / blood "liquid map" inputs, read at the entry of the map's draw node (sub_8279ACD8: r3 = the CCLM component, the
// map object). Three optional sections, written together or not at all:
//  51 kScLiquidMaps     = LiquidMapRow[]    one per map (character) that had a live item or a pending clear request this hook frame, the LAST call of the frame wins
//  52 kScLiquidItems    = LiquidItemRow[]   the live items of those maps (at most 16 each: the pool's cap), a map's items are the rows [item_first, item_first + item_count)
//  53 kScLiquidPictures = LiquidPictureRow[] the items' source textures (sweat 64x64 DXT1; blood 64x128 .. 256x256 DXT1, S105 A12: its own 32 KB row, not the crowd's 4 KB one), one row per distinct key; LiquidItemRow::picture = the row's `key`, 0 = none
// A reader that does not know them skips them (older readers) and a record without them draws every skin dry (never a failure). A bad layout / a non-finite number drops the three together (liquid_errors).
inline constexpr uint32_t kScLiquidMaps = 51, kScLiquidItems = 52, kScLiquidPictures = 53;
inline constexpr uint32_t kLiquidLayout = 1;
enum LiquidMapFlags : uint32_t {
  kLmEnabled = 1,     // map+2572 != 0 (the node's own enable word)
  kLmClear = 2,       // map+136 != 0: the clear request (create, blood level 0, the Swet reset). The hook CONSUMES it (writes 0) when the node is skipped, so a row's bit is an EDGE
  kLmAlt = 4,         // map+200 != 0: the game's REBUILD (sub_82727F30 sets it with +136; the node then draws sub_8279A328 INSTEAD of the normal paste). The row then carries the rebuild rows (kLiRebuild / kLiRegion) after its live items;
                      // the glue consumes the request (S108 BLD2: docs\research\s108\BLD_plan.md section 10)
  kLmDim = 8,         // sub_8259F0E0(character): character+5076 != 0 || character+5072 != 0 (created wrestlers); fixed at build
  kLmResources = 16,  // sub_82726170 [C]: the map's resources exist (the node draws only then); the map is also 1x1x1 while they are missing
  kLmRebuild = 32,    // S108 BLD3: the row carries a rebuild PACKAGE (the kLiRebuild / kLiRegion rows) of generation reserved[1]. Set on the kLmAlt row and RE-SENT on the map's later rows until the host acknowledges that
                      // generation (LiquidRebuildAck below): the record of the rebuild's frame is often never drawn (run 18: the post-match scene change is 2D / missed / mailbox-dropped frames), so a one-frame row is lost.
                      // The host applies a generation once (clear, the rebuild's steps, then the row's own live items unless the row is the kLmAlt row itself).
};
struct LiquidMapRow {   // 64 bytes
  uint32_t layout = kLiquidLayout;
  uint32_t frame = 0;          // the hook's guest frame (low 32 bits) of the call that wrote the row
  uint32_t map = 0;            // the map object (guest address): the persistent image's key
  uint32_t object = 0;         // [[map+48]+48]: the character's skeleton object = the record's engine object of its body (the key of every wrestler row); 0 = not read
  uint32_t flags = 0;          // LiquidMapFlags
  uint16_t width = 0, height = 0;   // map+124 / +128 (1024 x 512; 1 x 1 while the resources are missing)
  uint32_t levels = 0;         // map+132 (3)
  uint32_t item_first = 0, item_count = 0;   // into kScLiquidItems (S108 BLD2: on a kLmAlt row the live items, then the kLiRebuild / kLiRegion rows)
  uint32_t live_count = 0;     // map+88: the live list's count (item_count counts the ones with a drawable flag set)
  uint32_t reserved[6] = {};   // [0] the clear generation (liquid_hook.h); [1] S108 BLD3: the rebuild package's generation (with kLmRebuild)
};
static_assert(sizeof(LiquidMapRow) == 64, "LiquidMapRow is a flat POD table");

// S108 BLD3: the host's acknowledgement of a rebuild package (map, generation), read by the hook's tracker to stop re-sending it. One writer (the host's sweat pass), any reader; a slot per map (linear, kMaxMaps-sized).
class LiquidRebuildAck {
 public:
  static constexpr int kSlots = 32;
  void Ack(uint32_t map, uint32_t gen) {   // the host, after it applied the package
    int free_at = -1;
    for (int i = 0; i < kSlots; ++i) {
      const uint64_t v = slot_[i].load(std::memory_order_relaxed);
      if (uint32_t(v >> 32) == map) { slot_[i].store((uint64_t(map) << 32) | gen, std::memory_order_release); return; }
      if (v == 0 && free_at < 0) free_at = i;
    }
    const int at = free_at >= 0 ? free_at : int(next_++ % kSlots);   // (more maps than slots: a reused slot only makes its old map's package be re-sent until it is acked again)
    slot_[at].store((uint64_t(map) << 32) | gen, std::memory_order_release);
  }
  bool Acked(uint32_t map, uint32_t gen) const {
    for (int i = 0; i < kSlots; ++i) {
      const uint64_t v = slot_[i].load(std::memory_order_acquire);
      if (uint32_t(v >> 32) == map) return uint32_t(v) == gen;
    }
    return false;
  }
  void Reset() { for (auto& s : slot_) s.store(0, std::memory_order_relaxed); }   // (tests)

 private:
  std::atomic<uint64_t> slot_[kSlots] = {};
  uint32_t next_ = 0;
};
inline LiquidRebuildAck& RebuildAck() {
  static LiquidRebuildAck a;
  return a;
}
enum LiquidItemFlags : uint32_t {
  kLiHasSource = 1,   // the source texture was read
  // S108 BLD2 (BLD_plan.md section 10): the rows of the game's REBUILD draw sub_8279A328, on a kLmAlt row only, after the live items (item_count counts them):
  kLiRebuild = 2,     // loop 1: an item of the SWEAT sub-pool [map+72, +72+[map+64]) with bits 27 and 30: its own disc, sweat shader, colour write mask B only
  kLiRegion = 4,      // loop 2: a dirty region entry ([map+76+4k] +12 != 0): type = the list k (0 blood -> mask R, 1 sweat -> mask G), state = the entry's +16 flag (list 0: c203.y = flag ? 0 : 1),
                      // world_param = the region's corners (x0, y0, x1, y1) in map UV (its record [[map+160+4k]+8] +20j, +4..+16), picture = the entry's +8 texture; drawn as a quad with T_Paste p1 / p4 (plain + tint / tint_dim)
};
// live: the pool's cap. S108 BLD3 review: rebuild / regions are NOT caps of the game's work: the game pages its rebuild (64 region draws, 63 sweat discs per call, +200 kept until the regions are done: liquid_hook.h
// DecodeRebuild) and the hook records its end state whole; these two only bound a torn guest count (the sweat sub-pool walk; the two region lists, 1024 each). Run 18/19 counts: 256+ dirty regions and 64+ sweat discs a rebuild.
inline constexpr uint32_t kLiquidLiveMax = 16, kLiquidRebuildMax = 4096, kLiquidRegionMax = 2048;
inline constexpr uint32_t kLiquidRowsPerMapMax = kLiquidLiveMax + kLiquidRebuildMax + kLiquidRegionMax;   // record_checks.h: a map row's item range
// item+60 bits the node's draw loop skips [C SWB 1d: sub_82725E28 bit30, sub_82725E38 bit29, sub_82725E58 bit26]: paused + not drawn, not drawn. (bit31 = alive: a dying item is still drawn once.)
inline constexpr uint32_t kLiStateNotDrawn = 0x40000000u | 0x20000000u | 0x04000000u;
struct LiquidItemRow {   // 72 bytes
  uint32_t layout = kLiquidLayout;
  uint32_t flags = 0;          // LiquidItemFlags
  uint32_t type = 0;           // item+20: 0 blood, 1 sweat ("tear"); >= 2 is recorded and not drawn (the game inherits its constants from the previous item)
  uint32_t state = 0;          // item+60: bit31 alive, bit30 / bit29 paused + not drawn, bit26 not drawn
  uint32_t pass = 0;           // item+144: 0 = T_Normal p0 (sweat), 1 = T_Normal p1 (paste)
  uint32_t picture = 0;        // CrowdPictureRow::key of the source texture (item+64 -> wrapper -> header); 0 = none read
  float local_area[4] = {};    // item+80..92 (VS c205)
  float world_param[4] = {};   // item+112..124 (VS c206): the disc's two corners in the map's UV
  float rect[4] = {};          // item+128..140: the dirty rect in map texels (x0, y0, x1, y1), 32-snapped
};
static_assert(sizeof(LiquidItemRow) == 72, "LiquidItemRow is a flat POD table");

}  // namespace rdr_scene
