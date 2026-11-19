// Bits 4 and 7 from MEMORY at the latch (Session 103 lane A6; docs\renderer\INPUTS_A6.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes.
//
// Under OURS the game skips its scene walk, so the install calls and the model-entry hook never run and two inputs vanish: bit 4 (WHICH light object each drawn model follows: the learned table
// light_index_core.h) and bit 7 / E8 (each wrestler's light STATE: R+0..895, the 896 bytes the install copies into the render-camera record). Both are DATA THE GAME'S NODES HOLD BEFORE THE WALK:
//   * wrestlers (and the referee): the draw nodes sub_8259FAE8 (0x8259FBE8) and sub_825A3908 (0x825A3A68) install `sub_82620488(char + 3936, mode 0)`; r31 of the second is char + 1200 (every field it
//     reads is char + off - 1200: +224 -> -976, +232 -> -968, +48 -> -1152; the install `addi r3,r31,2736` = char + 3936), so both install the same object. The model each draw follows is the skeleton [char + 48]
//     (`lwz r3,48(r30)` 0x8259FCE4). The characters are the 9 words at 0x836F38D0. sub_82620488 (0x82620488-0x8262053C): gate (word 0x836F53D0 non-null with [+152] != 0) or the mode's override word
//     (mode 0: 0x83622668) < 0 -> the node's own EMBEDDED object goes through sub_8261FE90 (its block = object + 48 = char + 3984, 896 bytes); else sub_826203E8(-1, 0) and the manager's rule resolves the object.
//   * the ring: sub_825D0340 installs sub_826203E8([X + 9588], 1) (0x825D03E8) before the posts / turnbuckles / covers (sub_825CCFE0 / sub_825CE250 / sub_825CE668), sub_825CF2F8 installs
//     sub_826203E8([X + 9584], 1) (0x825CF52C, ropes); X is the ring colour callback's r3 - 24, already noted every frame by lane C3 (ring_recompute_contrib.h NoteRing). The canvas and the skirts install
//     sub_826203E8([Y + 1232], 1) (sub_82608CB0 0x82608D30) and sub_826203E8([Y + 1236], 1) (sub_82609290 0x8260931C), Y = r3 - 1168 of sub_826096E0, whose own caller chain (sub_82609670 -> sub_82609060, field
//     +1248) is the object the ring contributor already treats as X [U: Y == X is settled by this lane's census, not assumed]. The models are G + 20 / 24 (ropes), G + 36 / 40 / 44 / 48 / 52 (covers, turnbuckle,
//     posts), G + 56 (canvas), G + 332 / 336 / 340 / 344 (skirts), G = the word at 0x836F4C74 (ring.md section 1).
// What this does: at the latch it builds the PREDICTED install per model (raw index, mode, source object, the 896-byte block), compares it with the walk-learned table on walked frames (the census
// `light_index latch==walk N, differ M`) and the block with the walk's LightState of the same model (`light_state latch==walk`), and, only behind kLatchEntries (off), feeds the prediction to the table
// so an OURS frame has its entries. Pure: reads through a ReadFn, never writes the guest.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "inputs_core.h"
#include "light_index_core.h"
#include "light_latch_board.h"
#include "../renderer_scene/light_install.h"

namespace rdr_scene::live::lightlatch {

// OFF. When true the latch's predictions are fed into the learned table (Table::OnLatchEntry) so the entries exist without a walk. Released only after a mirror run shows `light_index ... differ 0` for every class
// it feeds and `light_state ... differ 0` (docs\renderer\INPUTS_A6.md section 6). The census and the comparison run with it off.
inline constexpr bool kLatchEntries = false;

inline constexpr uint32_t kCharSlotTable = 0x836F38D0, kCharSlots = 9;   // sub_82812E90 (circle_hook.h: the 9 character slots; listing 0x82812E90-0x82813224)
inline constexpr uint32_t kCharSkeletonOff = 48;                 // sub_8259FAE8 0x8259FCE4 `lwz r3,48(r30)`: the skeleton the draw poses and drives (r30 = the character)
inline constexpr uint32_t kCharEmbeddedOff = 3936;               // sub_8259FAE8 0x8259FBE4 `addi r3,r30,3936` then bl sub_82620488 (0x8259FBE8); sub_825A3908 0x825A3A64 `addi r3,r31,2736` = char + 1200 + 2736
inline constexpr uint32_t kBlockOffset = 48;                     // sub_8261FE90 / sub_82BCC018: the install copies object + 48, 896 bytes (PER_DRAW_LIGHTS.md 1.1)
inline constexpr uint32_t kRingGlobal = 0x836F4C74;              // G, sub_825D0340 0x825D0384-0x825D0388 (lis -31889 / lwz 19572); ring.md 1
inline constexpr uint32_t kRingRopeIndexOff = 9584;              // sub_825CF2F8 0x825CF52C `lwz r3,9584(r31)` before bl sub_826203E8 (0x825CF534), mode 1
inline constexpr uint32_t kRingFixtureIndexOff = 9588;           // sub_825D0340 0x825D03E8 `lwz r3,9588(r31)` before bl sub_826203E8 (0x825D03EC), mode 1
inline constexpr uint32_t kRingCanvasIndexOff = 1232;            // sub_82608CB0 0x82608D30 `lwz r3,1232(r28)` before bl sub_826203E8 (0x82608D38), mode 1 [object = r3 of sub_82608CB0: Y, [U] Y == X]
inline constexpr uint32_t kRingSkirtIndexOff = 1236;             // sub_82609290 0x8260931C `lwz r3,1236(r30)` before bl sub_826203E8 (0x82609324), mode 1

// The manager row's gate rule (sub_82620488 0x82620498-0x826204A8 / sub_826203E8 0x82620410-0x8262043C), read from the record's manager row.
inline bool WrestlerUsesEmbedded(const rdr_scene::LightObjectRow& m, const LatchFaults* ft) {
  if (ft && ft->ignore_gate) return false;
  const bool gated = m.words[rdr_scene::kLmGateObject] != 0 && m.words[rdr_scene::kLmGateFlag] != 0;
  const uint32_t ov = m.words[rdr_scene::kLmOverrideMode0];
  return gated || ov == 0xFFFFFFFFu || int32_t(ov) < 0;
}

// Predictions for the characters of the slot table. `rows` = the record's light object rows (manager row first). One Pred per character with a skeleton.
inline void PredictWrestlers(scene_inputs::ReadFn read, const std::vector<rdr_scene::LightObjectRow>& rows, std::vector<Pred>& out, Stats& st, const LatchFaults* ft = nullptr) {
  // LightManagerRow requires kLoBlockRead (every selection word read). A manager row whose words were NOT all read leaves w0 = 0xFFFFFFFF, which would take the embedded path silently: no prediction, counted unresolved.
  const rdr_scene::LightObjectRow* m = rdr_scene::LightManagerRow(rows);
  if (!m && ft && ft->ignore_block_flag)   // test-only fault: the flag is not required
    for (const rdr_scene::LightObjectRow& r : rows)
      if (!m && r.kind == rdr_scene::kLoManager) m = &r;
  if (!m) {
    bool unread = false;
    for (const rdr_scene::LightObjectRow& r : rows) unread = unread || r.kind == rdr_scene::kLoManager;
    if (unread) ++st.unresolved[kWrestler];
    else ++st.no_manager;
    return;
  }
  for (uint32_t s = 0; s < kCharSlots; ++s) {
    uint32_t ch = 0, skel = 0;
    if (!scene_inputs::ReadWord(read, kCharSlotTable + 4 * s, ch)) {
      ++st.unreadable[kWrestler];
      return;
    }
    if (ch < 0x10000) continue;   // empty slot
    if (!scene_inputs::ReadWord(read, ch + (ft && ft->skeleton_52 ? 52u : kCharSkeletonOff), skel) || skel < 0x10000) {
      ++st.unreadable[kWrestler];
      continue;
    }
    Pred p;
    p.cls = kWrestler, p.model = skel;
    if (WrestlerUsesEmbedded(*m, ft)) {
      p.source = ch + kCharEmbeddedOff + kBlockOffset, p.source_exact = true;
      p.index = rdr_scene::live::lightidx::kNoIndex, p.mode = rdr_scene::live::lightidx::kNoIndex;
      if (!scene_inputs::ReadLightBlockAt(read, p.source, p.block)) {
        ++st.unreadable[kWrestler];
        continue;
      }
      p.have_block = true;
    } else {
      p.index = 0xFFFFFFFFu, p.mode = 0;   // sub_82620488 passes li r3,-1 and mode 0; sub_826203E8 replaces the index by the mode's override word
      const rdr_scene::LightObjectRow* row = nullptr;
      const char* why = "";
      if (!rdr_scene::ResolveInstallRow(rows, p.index, p.mode, row, why) || !row) {
        ++st.unresolved[kWrestler];
        continue;
      }
      p.source = row->object + kBlockOffset, p.source_exact = false;
      std::memcpy(p.block, row->words, sizeof p.block);
      p.have_block = true;
    }
    ++st.preds[kWrestler];
    out.push_back(p);
  }
}

// The ring's installs. `x` = the ring object (ring_recompute_contrib.h Ring()), 0 = not noted / stale (counted). `rows` as above. mode 1 for every ring install.
inline void PredictRing(scene_inputs::ReadFn read, const std::vector<rdr_scene::LightObjectRow>& rows, uint32_t x, std::vector<Pred>& out, Stats& st, const LatchFaults* ft = nullptr) {
  if (x < 0x10000) {
    ++st.ring_unnoted;
    return;
  }
  uint32_t g = 0;
  if (!scene_inputs::ReadWord(read, kRingGlobal, g) || g < 0x10000) {
    ++st.unreadable[kFixture];
    return;
  }
  struct One { uint32_t cls, model_off, index_off; };
  const bool sw = ft && ft->swap_ring_words;
  static const One kModels[] = {
      {kRope, 20, kRingRopeIndexOff},       {kRope, 24, kRingRopeIndexOff},
      {kFixture, 36, kRingFixtureIndexOff}, {kFixture, 40, kRingFixtureIndexOff}, {kFixture, 44, kRingFixtureIndexOff}, {kFixture, 48, kRingFixtureIndexOff}, {kFixture, 52, kRingFixtureIndexOff},
      {kCanvas, 56, kRingCanvasIndexOff},
      {kSkirt, 332, kRingSkirtIndexOff},    {kSkirt, 336, kRingSkirtIndexOff},    {kSkirt, 340, kRingSkirtIndexOff},    {kSkirt, 344, kRingSkirtIndexOff},
  };
  for (const One& o : kModels) {
    uint32_t model = 0, index = 0;
    uint32_t ioff = o.index_off;
    if (sw && o.cls == kRope) ioff = kRingFixtureIndexOff;
    else if (sw && o.cls == kFixture) ioff = kRingRopeIndexOff;
    if (!scene_inputs::ReadWord(read, g + o.model_off, model)) {
      ++st.unreadable[o.cls];
      continue;
    }
    if (model < 0x10000) continue;   // a model the arena's ring does not have (a pac without it: ring.md 1)
    if (!scene_inputs::ReadWord(read, x + ioff, index)) {
      ++st.unreadable[o.cls];
      continue;
    }
    if (int32_t(index) < 0) {   // sub_826203E8 with a negative index installs nothing (0x82620474): no entry exists for it, by the game's own rule
      ++st.no_install[o.cls];
      continue;
    }
    Pred p;
    p.cls = o.cls, p.model = model, p.index = index, p.mode = 1;
    const rdr_scene::LightObjectRow* row = nullptr;
    const char* why = "";
    if (!rdr_scene::ResolveInstallRow(rows, index, 1, row, why) || !row) {
      ++st.unresolved[o.cls];
      continue;
    }
    p.source = row->object + kBlockOffset, p.source_exact = false;
    std::memcpy(p.block, row->words, sizeof p.block);
    p.have_block = true;
    ++st.preds[o.cls];
    out.push_back(p);
  }
}

}  // namespace rdr_scene::live::lightlatch
