// wwe2k14 - S110 ENTRBODIES (docs\research\s110\ENTRBODIES.md): while the game's scene start is held behind its loading card (src\scene_start_wait_hooks.h), the bodies of the game's characters are
// named the way the record will name them and handed to our loader (src\renderer_scene\start_bodies.h), so the start's first frame finds them resident.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Read only (game thread): the slot table 0x836F38D0 (9 character pointers; sub_8273D0C0 step 1 reads the same table, +C8..+D4), each character's chosen key (slot_key.h SlotKeyOf: the descriptor
// [[0x837635A4] + 8] + 192 x [ch + 3760], +92, while +52 says loaded), the slot's build-step colours (attire_build.h SlotDescForChar, only when the latch has the key) and the slot descriptor
// (slotkey::SlotDescriptor): the inputs wrestler_hook.h OnSkelExit hands SubmitSlotKey, through the record's one naming rule (WrestlerContributor::SlotKeyLink). A key not ready yet (its
// descriptor not loaded) is skipped and read again at the next poll.
#pragma once
#include <cstdint>
#include <vector>

#include "renderer_scene/start_bodies.h"
#include "scene_live/wrestler_hook.h"   // wrestlers::ReadWordNoNote, slotkey::*, attire_build::*, WrestlerContributor, scene_frame::CurrentMode, wwe2k14_rdr_slot_key_desc

namespace wwe2k14::wrestlers {
// S110 ENTRBODIES: the slot-key rule switch, read where wrestler_hook.h declares it (its cvar is declared in this namespace)
inline bool SlotKeyDescRule() { return REXCVAR_GET(wwe2k14_rdr_slot_key_desc); }
}  // namespace wwe2k14::wrestlers

namespace wwe2k14::scene_start_bodies {

// Game thread: name every occupied slot's body now and add it to this start's set. true = the set grew (scene_start_wait.h NoteAsked re-stamps the decision).
inline bool Publish() {
  if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return false;   // the record names no body then (wrestler_hook.h :335-337)
  const auto rd = [](uint32_t a, uint32_t& v) { return wrestlers::ReadWordNoNote(a, v); };
  const bool desc_rule = wrestlers::SlotKeyDescRule();
  std::vector<rdr_scene::startbodies::Body> bodies;
  for (int s = 0; s < 9; ++s) {
    uint32_t ch = 0;
    if (!rd(slotkey::kSlotTable + 4u * uint32_t(s), ch) || !ch) continue;
    const uint32_t key = slotkey::SlotKeyOf(rd, s, ch, desc_rule);
    if (!key) continue;
    uint32_t slot_desc = 0;
    if (attire_build::Latch::Instance().HasChosen(key)) {   // wrestler_hook.h :388-392
      attire_build::SlotWhy why = attire_build::SlotWhy::kNoDesc;
      slot_desc = attire_build::SlotDescForChar(rd, ch, key, attire_build::Latch::Instance(), why);
    }
    const rdr_scene::live::WrestlerContributor::Link l = rdr_scene::live::WrestlerContributor::SlotKeyLink(key, slot_desc, slotkey::SlotDescriptor(rd, ch));
    if (l.known) bodies.push_back(rdr_scene::startbodies::Body{l.pac, l.entry, l.variant});
  }
  return !bodies.empty() && rdr_scene::startbodies::TheBoard().Add(bodies);
}

}  // namespace wwe2k14::scene_start_bodies
