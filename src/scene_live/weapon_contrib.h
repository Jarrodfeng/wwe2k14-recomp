// The weapon manager's slots as a live contributor to the scene record (Session 103, lane PW-1; contract src\renderer_scene\weapon_rows.h; reader weapon_mgr_reader.h).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game or SDK includes: the glue (weapon_glue.h) gives it the guest word reader and registers it.
//
// What it records, per frame: one WeaponSlot row per live slot (section kScWeaponSlots, 59) into SceneTables::weapon_slots. The table is pulled at the frame's LATCH (the game threads parked,
// the same point as the other readers) and handed to Emit; nothing is cached across frames (a reload frees the models and reuses the addresses). No reader / no manager / an unreadable word
// = no rows: a record with no section 59 means "no weapons recorded". The census line says what the last frame held: `[scene][weapons] slots N (ids ...)`.
// S111 CUTREACH2 review (docs\research\s111\CUTREACH2.md "Review"): the table is model-class (the slot-13 dispatcher draws it in the walk, whose draws land in the next stream), so the record's DRAW side is the
// table of the latch before the record's (latch_period.h, the camera's rule; the latch's own read at the boundary), as plain rows. The newest table's rows the draw side does not hold follow as load HINTS
// (weapon_rows.h kSlotFlagHint): the prefetch asks for them at once, the gate never counts them. The census keeps the newest table.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../renderer_scene/live_scene.h"
#include "../renderer_scene/weapon_rows.h"   // S106 G4-2
#include "tail_latch.h"   // LatchMark
#include "latch_period.h"   // S111 CUTREACH2 review: OfPalette (the draw side takes latch r - 1's table)
#include "weapon_mgr_reader.h"

namespace rdr_scene::live::weapons {

// A read-only view over the guest through one function: `bool (*)(uint32_t addr, uint32_t& host_order_word)`.
using ReadWordFn = bool (*)(uint32_t addr, uint32_t& v);
struct FnMem {
  ReadWordFn fn = nullptr;
  bool read32(uint32_t a, uint32_t& v) const { return fn && fn(a, v); }
};

class WeaponContributor final : public Contributor {
 public:
  const char* Name() const override { return "weapons"; }
  void SetReader(ReadWordFn f) { mem_.fn = f; }

  bool Latch(const FrameInfo& fi) override {
    Take();
    drawn_ = period_.Pick(fi.guest_frame, rows_);   // S111 CUTREACH2 review: the draw side = latch r - 1's table (the walk the record's calls come from)
    mark_.Set(fi.guest_frame);   // S102 U1 (tail_latch.h LatchMark): the table was pulled at THIS frame's latch
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    if (!mark_.Use(fi.guest_frame, Name())) Take(), drawn_ = rows_;   // no latch for this frame (a plain EndFrame): take now (the boundary: its own read); a deferred emit doing so is counted as a violation
    if (!mem_.fn) return;
    if (why_ == Why::Unreadable) ++c.unknown;   // a half-read table is never written: a counted hole
    t.weapon_slots.insert(t.weapon_slots.end(), drawn_.begin(), drawn_.end());
    for (const rdr_scene::weapons::WeaponSlot& r : rows_) {   // S111 CUTREACH2 review: the newest table's rows the draw side does not hold: load hints (the prefetch starts their loads now)
      bool held = false;
      for (const rdr_scene::weapons::WeaponSlot& d : drawn_) held = held || std::memcmp(&d, &r, sizeof r) == 0;
      if (held) continue;
      rdr_scene::weapons::WeaponSlot h = r;
      h.flags |= rdr_scene::weapons::kSlotFlagHint;
      t.weapon_slots.push_back(h);
    }
  }
  void Census(std::string& out) override {
    out += "[scene][weapons] slots " + std::to_string(rows_.size()) + " (ids";
    for (const auto& r : rows_) out += " " + std::to_string(r.id);
    out += "), frames " + std::to_string(frames_) + ", no_manager " + std::to_string(no_mgr_) + ", unreadable " + std::to_string(unreadable_) + ", bad_vtable " + std::to_string(bad_vtable_) +
           ", refused " + std::to_string(refused_) + ", " + period_.CensusLine("draw_period");   // S111 CUTREACH2 review: records whose draw side took the table of the latch before theirs
  }
  const std::vector<rdr_scene::weapons::WeaponSlot>& Rows() const { return rows_; }   // tests

 private:
  void Take() {
    Stats s;
    why_ = ReadSlots(mem_, rows_, &s);
    if (why_ == Why::None) { auto rd = [this](uint32_t a, uint32_t& v) { return mem_.read32(a, v); }; ApplyBeltIds(rd, belt_keys_, rows_); }   // S103 LDR (review 3): the belt's section-59 row carries its sub-key too (Prefetch loads belt:<sub> ahead, DropStored frees it)
    ++frames_;
    if (why_ == Why::NoManager) ++no_mgr_;
    if (why_ == Why::Unreadable) ++unreadable_;
    bad_vtable_ += s.bad_vtable;
    if (why_ == Why::Refused) ++refused_;
  }
  FnMem mem_;
  BeltKeyCache belt_keys_;
  std::vector<rdr_scene::weapons::WeaponSlot> rows_;    // the newest table (the census, the load hints)
  std::vector<rdr_scene::weapons::WeaponSlot> drawn_;   // S111 CUTREACH2 review: the draw side (latch r - 1's table; the own read at the boundary)
  scene_inputs::period::OfPalette<std::vector<rdr_scene::weapons::WeaponSlot>> period_;   // latch thread only
  Why why_ = Why::None;
  LatchMark mark_;
  uint64_t frames_ = 0, no_mgr_ = 0, unreadable_ = 0, bad_vtable_ = 0, refused_ = 0;
};

inline WeaponContributor& Weapons() {
  static WeaponContributor w;
  return w;
}
inline void RegisterWeapons(ReadWordFn f) {   // once, at start; idempotent
  static bool done = false;
  if (done) return;
  done = true;
  Weapons().SetReader(f);
  Register(&Weapons());
}

}  // namespace rdr_scene::live::weapons
