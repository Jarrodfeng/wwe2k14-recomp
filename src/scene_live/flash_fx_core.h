// The crowd camera flashes' live writer, the pure rules (S103 lane C3; docs\renderer\STAGE_LIVE_WRITERS.md section 4). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only; NO game, SDK or renderer-adapter includes: the contributor (flash_fx_contrib.h), the game glue (stage_fx_glue.h) and the offline unit (tools\scene\flash_fx_unit.cpp) run THESE functions.
// Marks: [C] read in a listing / the run-10 trace (cited), [L] a reading, [U] unknown (a run names it).
//
// Where the game keeps its flashes [C: generated listings + run 10's trace `wwe2k14_078.log`, 120 of 120 draws from 0x823B2BC8]:
//  * The FLASH MANAGER M is the word at 0x836F3A7C (stored by sub_823B23F8 at 0x823B2440 after the constructor sub_823B1490). M + 182816 + 4 i, i = 0..23, are 24 flash ITEM pointers (constructor loop
//    0x823B1550-0x823B1594: `stwu r3,4(r29)` from M + 182812, count 24; the pooled cursor of sub_823B1050 is M + 182912, array index = cursor, `0x823B106C-0x823B1078`).
//  * An item's vtable is 0x8200F1AC (constructor sub_823B1358 stores it at 0x823B1378-0x823B1384). Its slots: 0 sub_823B1420 (dtor), 1 sub_823B0F90 (per-frame UPDATE), 2 sub_823B27F0 (DRAW one flash),
//    3 sub_823B0F50 (SPAWN). Item fields, from those four functions [C]:
//       +4  done flag (1 = finished)           SPAWN: 0.   UPDATE sets 1 when counter == life + 1 (life != 0);  DRAW sets 1 (0x823B2C44-0x823B2C54) when counter == life (after drawing it)
//       +16 position x, +20 y, +24 z, +28 w     SPAWN: the 16 bytes at r4; DRAW writes +28 := 1.0 in the visibility test (0x823B258C)
//       +32 counter                              SPAWN: 0.   UPDATE: counter + 1 each frame
//       +36 life (frames)                        SPAWN: r6
//       +40 scale (float)                        SPAWN: f1.  UPDATE: x 0.3 whenever +52 is set (0x823B0FDC-0x823B1000) and then clears +52; the DRAW sets +52 again after EVERY drawn frame (0x823B2C40), so the scale decays 0.3 per drawn frame (0.5 -> 0.15 -> 0.045 ...; crowd_flash.md measured the first two)
//       +44 visible flag                         SPAWN: 0.   The visibility test sub_823B2470 (run from DRAW while +48 != 0) clears +44 / +48 and sets +44 = 1 only if the point is on screen, outside the 36 wrestler boxes
//                                                (0x823B26B8-0x823B27D4) and the screen rectangle test passes (0x823B2650-0x823B2688)
//       +48 pending flag                         SPAWN: 1 (the next DRAW runs the visibility test and clears it)
//       +52 "drawn" flag, +56 colour index       SPAWN: 0, 0; DRAW sets +52 := 1 after it drew (0x823B2C40, before the done mark); UPDATE clears it (scale x 0.3) and sets +56 := rand mod 4 (0x823B1024-0x823B1038)
//  * The UPDATE returns at once when done (0x823B0FA8-0x823B0FAC), and so does the DRAW: a finished item keeps done = 1, counter == life, visible = 1, scale, +52 FOREVER (until the pool respawns it).
//    So "done and counter == life" cannot mean "drawn this frame". The ROW RULE (review F1 / F2): a row only when +52 != 0 (the draw ran and drew it; the test frame returns before drawing, 0x823B281C-0x823B2830,
//    and leaves +52 = 0), pending == 0, visible == 1, and (not done, or this is the FIRST latch that sees this item pointer done: the frame whose draw set the mark). The first-seen-done memory is per slot,
//    kept in FlashState between Takes (a respawned item has done = 0 again and resets it).
//  * The DRAW sub_823B27F0 [C]: returns if +4 != 0; runs sub_823B2470 if +48 != 0; returns if state==2/life!=0/+44==0 (marks done); with r19 = [[0x83767DE4] + 12] == 0 it draws nothing; else builds the
//    sprite world = (camera-facing basis from the inverse view) x scale (+40) with the TRANSLATION ROW (x, y - 40.0, z) [the double at 0x8200F2F0 = 40.0; 0x823B2A8C fsub f4 = f5(y) - f0], stores
//    it at r19 + 32 .. + 95 and, when +44 != 0, calls vtable slot 42 (+168) of r19 = sub_82C0F528 (the flash pass). The pass reads the translation row r19 + 80 as the flash centre [L: `addi r5,r21,80`
//    0x82C0F584 feeds the FlashPoint / c214 path; the stream's own c214 would confirm it].
//  * Colour: R block k = [item + 56] (0..3) of the manager's four 896-byte light blocks at M + 179232 + 896 k (constructor 0x823B14F4-0x823B1510, draw 0x823B2908-0x823B291C); the five colours are at
//    block + 96 (rgbw, PER_DRAW_LIGHTS.md "R + 96 = five colours"). When the word 0x836F3A8C is non-zero the draw takes light set 500 instead (0x823B28F8-0x823B2928) [U which path the entrance runs].
//  * Alpha: c185.x = 0.5 (crowd_flash.md, measured in the stream) [C value, U source].
//
// A frame's rows: for each item that is the item class (vtable read), finite, with a pending flag of 0 (the visibility test has run), a visible flag of 1, the drawn flag +52 set and either not done or
// done for the FIRST latch (the draw that marked it ran in that frame): one kFxFlash row. Every other item is a counted reason, never a guess (the contract: a missing value = no row +
// a counted reason). [U] the update's order against the draw inside a frame: the rule above keeps an item the walk drew; run proof = rows per frame against the stream's flash draws (the mirror census).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../renderer_scene/rows_stage_fx.h"
#include "../feed_seen.h"   // S110 FEEDREC

namespace rdr_scene::live::flashfx {

inline constexpr uint32_t kManagerGlobal = 0x836F3A7C;   // word = the flash manager M (0 = none)
inline constexpr uint32_t kPassGlobal = 0x83767DE4;      // word = P; [P + 12] = the flash model-pass object (0 = the game draws no flashes)
inline constexpr uint32_t kLoopGate = 0x8392B948;      // sub_823B38C0 runs its loop only when this word is non-zero (0x823B38D4-0x823B38E4: lwz, beq to the return)
inline constexpr uint32_t kLightSetFlag = 0x836F3A8C;    // non-zero = the draw takes light set 500 for the colour
inline constexpr uint32_t kItemArray = 182816;           // M + 182816 + 4 i
inline constexpr uint32_t kItemCount = 24;
inline constexpr uint32_t kColorBlocks = 179232;         // M + 179232 + 896 k: the four colour blocks
inline constexpr uint32_t kBlockStride = 896;
inline constexpr uint32_t kBlockColors = 96;             // block + 96: the first colour (rgbw)
inline constexpr uint32_t kItemVtable = 0x8200F1AC;
inline constexpr float kYDrop = 40.0f;                   // the double at 0x8200F2F0
inline constexpr float kFlashAlpha = 0.5f;               // c185.x

// StageFxRow::flags of a kFxFlash row: StageFxFlashFlags (scene_record.h).
// The writer's memory between two latches: per item slot, the item pointer and whether the previous latch saw it done (review F1).
struct FlashState {
  uint32_t ptr[kItemCount] = {};
  uint8_t done_seen[kItemCount] = {};
};

// Reasons an item or a frame gives no row (all counted).
struct FlashCensus {
  uint64_t frames = 0, rows = 0, no_manager = 0, no_pass = 0, no_loop = 0, empty_slots = 0, unreadable = 0, not_item = 0, bad_values = 0, unspawned = 0;
  uint64_t pending = 0, invisible = 0, done = 0, last_frame = 0, not_drawn = 0,  light_set_frames = 0, color_unread = 0, held = 0;   // held: rows shown from the hold, not from the game this frame (flash_hold_core.h)
};

using ReadFn = ::wwe2k14::feedseen::ReadFnT;   // S110 FEEDREC: the plain pointer outside coverage (feed_seen.h)
inline uint32_t Swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
inline bool ReadWord(ReadFn read, uint32_t a, uint32_t& v WWE_FEED_LOC_PARAM) {
  uint32_t raw = 0;
  if (!read || !read(a, &raw, 4 WWE_FEED_LOC_ARG)) return false;
  v = Swap32(raw);
  return true;
}
inline float AsFloat(uint32_t w) {
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

// The item as the game stores it (host order after the swap), the 64 bytes at the item.
struct ItemWords {
  uint32_t w[16] = {};
  float F(int off) const { return AsFloat(w[off / 4]); }
};
inline bool ReadItem(ReadFn read, uint32_t a, ItemWords& it) {
  uint32_t raw[16];
  if (!read || !read(a, raw, sizeof raw)) return false;
  for (int i = 0; i < 16; ++i) it.w[i] = Swap32(raw[i]);
  return true;
}

// One frame's flash rows. `rows` is appended to; the return value is the number appended.
inline uint32_t BuildFlashRows(ReadFn read, uint32_t frame, std::vector<StageFxRow>& rows, FlashCensus& cn, FlashState& st) {
  ++cn.frames;
  uint32_t m = 0;
  if (!ReadWord(read, kManagerGlobal, m) || m == 0) {
    ++cn.no_manager;
    return 0;
  }
  uint32_t p = 0, pass_obj = 0;
  if (!ReadWord(read, kPassGlobal, p) || p == 0 || !ReadWord(read, p + 12, pass_obj) || pass_obj == 0) {
    ++cn.no_pass;   // the draw returns before the sprite and the flash pass when r19 is null (0x823B2880): nothing is drawn
    return 0;
  }
  uint32_t loop_gate = 0;
  if (!ReadWord(read, kLoopGate, loop_gate) || loop_gate == 0) {   // the manager's draw loop does not run at all (review F5)
    ++cn.no_loop;
    return 0;
  }
  uint32_t lightset = 0;
  const bool light_set = ReadWord(read, kLightSetFlag, lightset) && lightset != 0;
  if (light_set) ++cn.light_set_frames;
  uint32_t made = 0;
  for (uint32_t i = 0; i < kItemCount; ++i) {
    uint32_t ip = 0;
    if (!ReadWord(read, m + kItemArray + 4 * i, ip)) {
      ++cn.unreadable;
      continue;
    }
    if (ip == 0) {
      ++cn.empty_slots;
      continue;
    }
    ItemWords it;
    if (!ReadItem(read, ip, it)) {
      ++cn.unreadable;
      continue;
    }
    if (it.w[0] != kItemVtable) {   // read, not assumed: a recycled or foreign object is not a flash item
      ++cn.not_item;
      continue;
    }
    const float x = it.F(16), y = it.F(20), z = it.F(24), scale = it.F(40);
    if (std::isfinite(scale) && scale == 0.f && std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {   // the constructor leaves +40 = 0.0 (0x823B13B8, the float at 0x8218B8CC): a pooled item that was never spawned (run 13: ~2.9 a frame)
      ++cn.unspawned;
      continue;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(scale) || !(scale > 0.f)) {
      ++cn.bad_values;
      continue;
    }
    const uint32_t done = it.w[1], counter = it.w[8], life = it.w[9], visible = it.w[11], pending = it.w[12], drawn = it.w[13], color_idx = it.w[14];
    const bool seen_done = st.ptr[i] == ip && st.done_seen[i] != 0;   // the previous latch saw this very item done
    st.ptr[i] = ip;
    st.done_seen[i] = done != 0;
    if (pending != 0) {   // spawned, the visibility test has not run: the game has not drawn it
      ++cn.pending;
      continue;
    }
    if (visible == 0) {
      ++cn.invisible;
      continue;
    }
    if (drawn == 0) {   // the test frame (the draw returns after the visibility test) or the update ran after the draw: not drawn in this walk
      ++cn.not_drawn;
      continue;
    }
    uint32_t flags = kFxfGameScale;
    if (done != 0) {
      if (seen_done) {   // finished at an earlier latch: its fields stay frozen, it is not drawn any more
        ++cn.done;
        continue;
      }
      flags |= kFxfDrawDone;
      ++cn.last_frame;
    }
    StageFxRow r;
    r.kind = kFxFlash;
    r.slot = uint16_t(i);
    r.seq = counter;
    r.flags = flags | (light_set ? kFxfLightSet : 0u);
    r.pos[0] = x, r.pos[1] = y - kYDrop, r.pos[2] = z;
    r.scale = scale;
    r.alpha = kFlashAlpha;
    r.color[0] = r.color[1] = r.color[2] = 1.f;
    r.color[3] = life ? std::min(1.f, float(counter) / float(life)) : 0.f;
    const uint32_t k = std::min<uint32_t>(color_idx, 3u);
    float rgbw[4];
    uint32_t raw[4];
    if (read(m + kColorBlocks + kBlockStride * k + kBlockColors, raw, sizeof raw)) {
      for (int c = 0; c < 4; ++c) rgbw[c] = AsFloat(Swap32(raw[c]));
      if (std::isfinite(rgbw[0]) && std::isfinite(rgbw[1]) && std::isfinite(rgbw[2])) {
        r.color[0] = rgbw[0], r.color[1] = rgbw[1], r.color[2] = rgbw[2];
      } else {
        r.flags |= kFxfColorUnread;
      }
    } else {
      r.flags |= kFxfColorUnread;
    }
    if (r.flags & kFxfColorUnread) ++cn.color_unread;
    (void)frame;
    rows.push_back(r);
    ++cn.rows;
    ++made;
  }
  return made;
}

}  // namespace rdr_scene::live::flashfx
