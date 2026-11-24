// The weapon manager's slots as a live contributor to the scene record (Session 103, lane PW-1; contract src\renderer_scene\weapon_rows.h; reader weapon_mgr_reader.h).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game or SDK includes: the glue (weapon_glue.h) gives it the guest word reader and registers it.
//
// What it records, per frame: one WeaponSlot row per live slot (section kScWeaponSlots, 59) into SceneTables::weapon_slots. The table is pulled at the frame's LATCH (the game threads parked,
// the same point as the other readers) and handed to Emit; nothing is cached across frames (a reload frees the models and reuses the addresses). No reader / no manager / an unreadable word
// = no rows: a record with no section 59 means "no weapons recorded". The census line says what the last frame held: `[scene][weapons] slots N (ids ...)`.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../renderer_scene/live_scene.h"
#include "../renderer_scene/weapon_rows.h"   // S106 G4-2
#include "tail_latch.h"   // LatchMark
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
    mark_.Set(fi.guest_frame);   // S102 U1 (tail_latch.h LatchMark): the table was pulled at THIS frame's latch
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    if (!mark_.Use(fi.guest_frame, Name())) Take();   // no latch for this frame (a plain EndFrame): take now; a deferred emit doing so is counted as a violation
    if (!mem_.fn) return;
    if (why_ == Why::Unreadable) ++c.unknown;   // a half-read table is never written: a counted hole
    t.weapon_slots.insert(t.weapon_slots.end(), rows_.begin(), rows_.end());
  }
  void Census(std::string& out) override {
    out += "[scene][weapons] slots " + std::to_string(rows_.size()) + " (ids";
    for (const auto& r : rows_) out += " " + std::to_string(r.id);
    out += "), frames " + std::to_string(frames_) + ", no_manager " + std::to_string(no_mgr_) + ", unreadable " + std::to_string(unreadable_) + ", bad_vtable " + std::to_string(bad_vtable_) +
           ", refused " + std::to_string(refused_);
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
  std::vector<rdr_scene::weapons::WeaponSlot> rows_;
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
