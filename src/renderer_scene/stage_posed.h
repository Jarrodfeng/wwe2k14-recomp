// S110 BARRICADE (docs\research\s110\BARRICADE.md): the stage slots the game draws POSED, one pure header shared by the writer (scene_live\stage_fx_contrib.h) and the applier
// (rf_c_core.cpp / rf_arena_live.cpp). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game, SDK or renderer includes (the unit tools\scene\stage_posed_unit.cpp includes it).
//
// The game's rule [C, the note]: a stage slot is drawn with the palette its model holds, [model + 200], one 4x4 (64 bytes) per node; the node count is the model's own (sub_82BD3AB8: [model + 300] when
// [model + 296] != 0). The corner barricade rigs (slots 390-397; 81-88 / 91-94 in a created arena) have 5 nodes and the game writes their pose into that palette every frame (the STBA task, the break
// motions); every other barricade slot has one node. The rule here is therefore NOT a list of slots: ANY slot 0..398 whose model has more than one node is read, from the game's values alone.
//  * Slot k's model = the word at stage + 6164 + 4 k (0 = none). n = the game's own node count of that model (ringrows::NodeCount, sub_82BD3AB8: [+296] != 0 -> [+300], else the [+220] forward; unreadable = refused).
//    n of 0 or 1 = not posed (no row: the slot stays in the static arena / its rigid moving model). More than kPosedMaxNodes = refused (absurd).
//  * One kFxSlotPalette row per node: slot, movie = the node index, flags = the node count n (so a reader takes all of a slot's nodes or none), pad = the model object, world = the 12 floats
//    RowsFromMatrix cuts from the node's matrix (VS c76..c78, the packer's cut), seq = the frame.
//  * Fail closed: a slot whose model / palette is unreadable or holds a non-finite float gives no rows this frame (the other slots still do); the applier keeps what the record carries.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "../scene_live/arena_world.h"   // RowsFromMatrix (pure)
#include "../scene_live/ring_rows_core.h"   // NodeCount (pure)
#include "rows_stage_fx.h"
#include "stage_slot_vis.h"   // VisFromRows / VisIsForArena (the marker row: stage generation and arena)

namespace rdr_scene {

inline constexpr uint32_t kPosedStageModels = 6164;   // stage + 6164 + 4 * slot: the slot's model object
inline constexpr uint32_t kPosedSlots = 399;          // slots 0..398
inline constexpr uint32_t kPosedModelPalette = 200, kPosedModelCountFlag = 296, kPosedModelCount = 300;
inline constexpr uint32_t kPosedMaxNodes = 64;        // the rigs are 5 nodes; anything above this is not a stage slot's rig
inline constexpr size_t kPosedRowCap = 512;         // rows one frame may carry (whole slots only). S110 review: the worst shipped arena is bg36 = 252 rows (slots 0-14 x 15 nodes, 18 x 7, 390-393 x 5; asset index
                                                      // census of every bg / car pac): at 128 its corners were dropped behind the 15-node ground slots. A hit cap is counted (PosedToRows' return, the stage_fx census)

// Offline twins (tools\scene\stage_posed_check.py): 0 = the real rule. 1 = reads node 1 only (the slot's world is one node), 2 = ignores [model + 296], 3 = the wrong matrix stride (48 bytes).
inline int g_posed_plant = 0;

struct PosedSlot {
  uint32_t slot = 0, model = 0;
  std::vector<float> rows;   // node k = rows[12 k .. 12 k + 11] (RowsFromMatrix of palette matrix k)
  uint32_t Nodes() const { return uint32_t(rows.size() / 12); }
};

namespace posed_detail {
inline uint32_t Swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
template <class Rd>
inline bool Word(Rd& rd, uint32_t a, uint32_t& v) {
  uint32_t raw = 0;
  if (!rd(a, &raw, 4)) return false;
  v = Swap32(raw);
  return true;
}
}  // namespace posed_detail

// Reads every posed slot of `stage` through `rd(guest, dst, n)` (false = unreadable). Returns false when the stage's model array cannot be read (out cleared); a slot that cannot be read is left out.
// `bad` (optional) counts the slots refused (unreadable / absurd / non-finite).
template <class Rd>
inline bool ReadPosedSlots(Rd&& rd, uint32_t stage, std::vector<PosedSlot>& out, uint32_t* bad = nullptr) {
  out.clear();
  if (!stage) return false;
  uint32_t models[kPosedSlots];
  if (!rd(stage + kPosedStageModels, models, sizeof models)) return false;
  for (uint32_t s = 0; s < kPosedSlots; ++s) {
    const uint32_t model = posed_detail::Swap32(models[s]);
    if (model < 0x10000) continue;   // no model
    uint32_t pal = 0, n = 0;
    {   // the game's own count (sub_82BD3AB8 as ringrows::NodeCount follows it: [+296] != 0 -> [+300]; else the [+220] forward; no skeleton = -1)
      int32_t cnt = 0;
      uint32_t cflags = 0;
      const auto rw = [&rd](uint32_t a, uint32_t& v) { return posed_detail::Word(rd, a, v); };
      if (g_posed_plant == 2) {   // twin: the count straight from [+300], no forward
        uint32_t c = 0;
        if (!rw(model + kPosedModelCount, c)) { if (bad) ++*bad; continue; }
        cnt = int32_t(c);
      } else if (!live::ringrows::NodeCount(rw, model, cnt, cflags)) {
        if (bad) ++*bad;
        continue;
      }
      if (cnt < 0 || uint32_t(cnt) > kPosedMaxNodes) { if (cnt > 0 && bad) ++*bad; continue; }
      n = uint32_t(cnt);
      if (g_posed_plant == 1 && n > 1) n = 1;
      if (n <= 1 && g_posed_plant != 1) continue;   // one node (or none): not posed
    }
    if (n > kPosedMaxNodes) { if (bad) ++*bad; continue; }
    if (!posed_detail::Word(rd, model + kPosedModelPalette, pal) || pal < 0x10000) { if (bad) ++*bad; continue; }
    const uint32_t stride = g_posed_plant == 3 ? 48u : 64u;
    std::vector<uint32_t> raw(size_t(n) * 16u);
    bool ok = true;
    for (uint32_t k = 0; k < n && ok; ++k) ok = rd(pal + stride * k, &raw[size_t(k) * 16u], 64);
    PosedSlot ps;
    ps.slot = s, ps.model = model;
    ps.rows.resize(size_t(n) * 12u);
    for (uint32_t k = 0; k < n && ok; ++k) {
      float m[16];
      for (int i = 0; i < 16; ++i) {
        const uint32_t w = posed_detail::Swap32(raw[size_t(k) * 16u + size_t(i)]);
        std::memcpy(&m[i], &w, 4);
        if (!std::isfinite(m[i])) ok = false;
      }
      if (ok) live::arenaworld::RowsFromMatrix(m, &ps.rows[size_t(k) * 12u]);
    }
    if (!ok) { if (bad) ++*bad; continue; }
    out.push_back(std::move(ps));
  }
  return true;
}

// Exactly one of kFxSlotWorld / kFxSlotPalette per slot: a slot the table already holds a kFxSlotWorld row for (an m(N) slot) keeps that row, its palette rows are not written.
inline void PosedWithoutWorldRows(std::vector<PosedSlot>& slots, const std::vector<StageFxRow>& rows) {
  for (size_t i = slots.size(); i-- > 0;)
    for (const StageFxRow& r : rows)
      if (r.kind == kFxSlotWorld && r.slot == slots[i].slot) { slots.erase(slots.begin() + long(i)); break; }
}

// One kFxSlotPalette row per node, whole slots only (a slot that would pass `cap` rows is left out). Returns the slots left out by the cap (the caller counts them).
inline size_t PosedToRows(const std::vector<PosedSlot>& slots, uint32_t frame, std::vector<StageFxRow>& out, size_t cap = kPosedRowCap) {
  size_t used = 0, dropped = 0;
  for (const PosedSlot& p : slots) {
    const uint32_t n = p.Nodes();
    if (n == 0) continue;
    if (used + n > cap) { ++dropped; continue; }
    for (uint32_t k = 0; k < n; ++k) {
      StageFxRow r;
      r.kind = kFxSlotPalette;
      r.slot = uint16_t(p.slot);
      r.movie = k;
      r.seq = frame;
      r.flags = n;
      r.pad = p.model;
      std::memcpy(r.world, &p.rows[size_t(k) * 12u], sizeof r.world);
      out.push_back(r);
    }
    used += n;
  }
  return dropped;
}

// The applier's side: the posed slots a record's rows describe, by slot. A slot whose rows are not exactly nodes 0..n-1 once each (n = flags) is left out (a torn set never draws half a rig).
inline void PosedFromRows(const std::vector<StageFxRow>& rows, std::map<int, PosedSlot>& out) {
  out.clear();
  std::map<int, std::vector<const StageFxRow*>> by;
  for (const StageFxRow& r : rows)
    if (r.kind == kFxSlotPalette) by[int(r.slot)].push_back(&r);
  for (const auto& kv : by) {
    const uint32_t n = kv.second[0]->flags;
    if (n < 2 || n > kPosedMaxNodes) continue;
    std::vector<const StageFxRow*> node(n, nullptr);
    bool ok = true;
    for (const StageFxRow* r : kv.second) {
      if (r->flags != n || r->movie >= n || node[r->movie]) { ok = false; break; }
      node[r->movie] = r;
    }
    for (uint32_t k = 0; k < n && ok; ++k) ok = node[k] != nullptr;
    if (!ok) continue;
    PosedSlot ps;
    ps.slot = uint32_t(kv.first), ps.model = node[0]->pad;
    ps.rows.resize(size_t(n) * 12u);
    for (uint32_t k = 0; k < n; ++k) std::memcpy(&ps.rows[size_t(k) * 12u], node[k]->world, sizeof node[k]->world);
    out[kv.first] = std::move(ps);
  }
}

// The applier's keep rule (fail closed, as the show bits and the skip list): a slot is drawn from the LAST palette rows its stage generation carried, so a frame without rows (an unreadable take) never makes
// a rig vanish. A new stage generation (a new match / stage object) drops them. `gen_known` = the frame's marker row named a generation; a frame with none keeps everything.
struct PosedKeep {
  uint32_t gen = 0;
  bool have_gen = false;
  std::map<int, PosedSlot> last;
};
inline void PosedAdopt(PosedKeep& keep, bool gen_known, uint32_t gen, std::map<int, PosedSlot>&& now) {
  if (gen_known && (!keep.have_gen || keep.gen != gen)) keep.last.clear(), keep.gen = gen, keep.have_gen = true;
  for (auto& kv : now) keep.last[kv.first] = std::move(kv.second);
}
// The applier's whole adoption step for one frame (rf_arena_live.cpp), the rule of VisAdoptRows: the frame's rows are adopted only under a marker of the arena being drawn (VisIsForArena). A frame with no marker, or the
// marker of another arena's stage (the editor's, a stage still being freed), changes nothing: it neither clears this arena's rows nor puts its own slots' rows on this arena's models.
inline void PosedAdoptRows(const std::vector<StageFxRow>& rows, int arena, PosedKeep& keep) {
  StageVis mark;
  bool valid = false;
  if (!VisFromRows(rows, mark, valid) || !mark.marker || !VisIsForArena(mark, arena)) return;
  std::map<int, PosedSlot> now;
  PosedFromRows(rows, now);
  PosedAdopt(keep, true, mark.gen, std::move(now));
}

}  // namespace rdr_scene
