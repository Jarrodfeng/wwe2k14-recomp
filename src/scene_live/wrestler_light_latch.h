// The wrestlers' light object at the latch: the live side (Session 103, lane A8; docs\renderer\INPUTS_A8.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only.
// The pure part (Store, Apply, the census, the replay) is src\renderer_scene\wrestler_light_latch.h. This file reads the blocks at the latch (read-only guest reads, no hook, no guest write) and applies them at the
// lights contributor's Emit (light_state_contrib.h). Per slot of the character table (0x836F38D0 + 4 * slot): the embedded object's block (char + 3984) on the embedded path (the same gate / override rule as
// light_latch.h WrestlerUsesEmbedded: sub_82620488 0x82620498-0x826204AC), else the manager's object the override word names (ResolveInstallRow). Nothing is read from mgr + 376 / 380: the embedded object is
// persistent data and already holds whatever sub_8259F630 copied into it (INPUTS_A8.md 2).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "inputs_core.h"
#include "caster_stamp_flag.h"   // S103 L1C: bit 7's stamp rule (StampFromLatch)
#include "light_latch.h"
#include "../renderer_scene/light_install.h"
#include "../renderer_scene/wrestler_light_latch.h"

namespace rdr_scene::live::wlatch {

// S103 L1C: ON. When true, an OURS frame (no walked wrestler state) gets a LightState + LightRef per wrestler actor from the latch's block. The census (`wrestler_light latch==walk N, differ M (changed frames only)`) runs
// with it off, on walked (mirror) frames. Flip only on a mirror run with `differ 0` (docs\renderer\INPUTS_A8.md section 6). The run-time copy is what the game reads.
inline constexpr bool kWrestlerLightFromLatch = true;   // S103 L1C: ON for landing 1 (A8: replay differ 0 on runs 27-29, review: no blocker); --wwe2k14_rdr_scene_hold=4 turns it off
inline bool g_on = kWrestlerLightFromLatch;
// scene_frame::Ours, set by wwe2k14_app.h OnPostSetup (null = never synthesize).
inline bool (*g_ours_fn)(uint64_t) = nullptr;

// S103 L1C review F3: an entry is USABLE when the draw side would accept it as a wrestler's light state: the same test as renderer_scene\rf_wrestlers.cpp WrestlerLightState (finite directions and colours, colour sum of
// lights 0-3 > 0, light 4's direction a key direction). Bit 7 counts only usable entries: a raw entry the draw would reject (`Rejected`) leaves the wrestler with the old arena set, not its own light.
inline bool EntryUsable(const rdr_scene::wlight::Entry& e, bool key_off_counts = true, bool key_off_by_direction = false) {
  float f[rdr_scene::kLsLightFloats];
  std::memcpy(f, e.block, sizeof f);
  return rdr_scene::LightStateUsable(f, key_off_counts, key_off_by_direction);   // S103 CE: one shared predicate with the draw (light_install.h)
}
inline bool EntryKeyed(const rdr_scene::wlight::Entry& e) {
  float f[rdr_scene::kLsLightFloats];
  std::memcpy(f, e.block, sizeof f);
  return rdr_scene::HasKey(f);
}

// S103 CE: bit 7's decision, pure (the unit calls it with planted rules): every occupied slot (`occupied` = the entries plus the unread / unresolved ones) has a USABLE entry (a key-off state counts, light_install.h
// LightStateUsable) and at least one usable entry has a lit key direction (`keyed`: the shadow fit needs one). `key_off_counts` = false is the planted twin of the old rule; `count_key_off_as_keyed` the twin of the second.
inline bool LatchStamps(const std::vector<rdr_scene::wlight::Entry>& ents, uint32_t occupied, bool synth_on = g_on, bool key_off_counts = true, bool count_key_off_as_keyed = false) {
  uint32_t usable = 0, keyed = 0;
  for (const rdr_scene::wlight::Entry& e : ents) {
    const bool u = EntryUsable(e, key_off_counts);
    usable += u ? 1u : 0u;
    keyed += (u && (EntryKeyed(e) || count_key_off_as_keyed)) ? 1u : 0u;
  }
  return rdr_scene::live::caster_stamp::StampFromLatch(synth_on, occupied, usable, keyed);
}

class Wl {
 public:
  // At the latch (hook frame `hook`): read each character's block. `rows` = the record's light object rows (manager row first). Unread / unresolved slots are counted, never guessed.
  void OnLatch(scene_inputs::ReadFn read, const std::vector<rdr_scene::LightObjectRow>& rows, uint32_t hook) {
    std::vector<rdr_scene::wlight::Entry> ents;
    const rdr_scene::LightObjectRow* m = rdr_scene::LightManagerRow(rows);
    if (!m) {
      std::lock_guard<std::mutex> lock(mu_);
      ++no_manager_;
      return;
    }
    uint64_t unreadable = 0, unresolved = 0;
    for (uint32_t s = 0; s < rdr_scene::wlight::kSlots; ++s) {
      uint32_t ch = 0;
      if (!scene_inputs::ReadWord(read, lightlatch::kCharSlotTable + 4 * s, ch)) {
        ++unreadable;
        break;
      }
      if (ch < 0x10000) continue;   // empty slot
      rdr_scene::wlight::Entry e;
      e.slot = s, e.character = ch;
      if (lightlatch::WrestlerUsesEmbedded(*m, nullptr)) {
        e.source = ch + lightlatch::kCharEmbeddedOff + lightlatch::kBlockOffset;
        if (!scene_inputs::ReadLightBlockAt(read, e.source, e.block)) {
          ++unreadable;
          continue;
        }
      } else {
        const rdr_scene::LightObjectRow* row = nullptr;
        const char* why = "";
        if (!rdr_scene::ResolveInstallRow(rows, 0xFFFFFFFFu, 0, row, why) || !row) {   // sub_82620488 passes -1 and mode 0; sub_826203E8 replaces the index by the override word
          ++unresolved;
          continue;
        }
        e.source = row->object + lightlatch::kBlockOffset;
        std::memcpy(e.block, row->words, sizeof e.block);
      }
      ents.push_back(e);
    }
    // S103 L1C review B1 / F2 / F3: bit 7 (caster_entries) is stamped HERE, at the latch (the record's stamp snapshot is taken at the end of the latch, before any Emit), for THIS latch's hook: every occupied character
    // slot read and resolved, every one of them has a USABLE entry (EntryUsable), and there is at least one. The key light of the caster maps is this block's dir 4. Held by --wwe2k14_rdr_scene_hold=7 (Capture) or =4 (g_on off).
    // NOT covered (unchecked gap, F3): casters that are not character slots (props, chairs, the referee if he is no slot) cast shadows the game may draw; their light is not read here.
    const uint32_t occupied = uint32_t(ents.size()) + uint32_t(unreadable + unresolved);
    const bool stamp = scene_frame::CurrentMode() != scene_frame::Mode::Off && unreadable == 0 && unresolved == 0 && LatchStamps(ents, occupied);
    store_.Put(hook, std::move(ents));
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++latches_, unreadable_ += unreadable, unresolved_ += unresolved;
      ++(stamp ? caster_stamps_ : caster_unstamped_);
    }
    if (stamp) scene_inputs::Capture(scene_inputs::kInCasterEntries, hook);
  }
  // At the lights contributor's Emit of record `t`: the mirror census always; the synthesis only with the switch. Never throws, never touches a walked state.
  void OnEmit(rdr_scene::SceneTables& t) {
    std::lock_guard<std::mutex> lock(mu_);
    rdr_scene::wlight::Apply(t, store_, rdr_scene::wlight::ShouldSynthesize(g_on, g_ours_fn, t), census_);
    // (bit 7 is NOT stamped here any more: see OnLatch. Emit is after the record's stamp snapshot.)
  }
  std::string Census() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string s = rdr_scene::wlight::CensusText(census_, g_on);
    s.insert(s.size() - 1, ", caster_stamps " + std::to_string((unsigned long long)caster_stamps_) + ", caster_unstamped " + std::to_string((unsigned long long)caster_unstamped_) + ", latches " + std::to_string((unsigned long long)latches_) + ", no_manager " + std::to_string((unsigned long long)no_manager_) + ", unreadable " + std::to_string((unsigned long long)unreadable_) +
                               ", unresolved " + std::to_string((unsigned long long)unresolved_));
    return s;
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    store_.Reset();
    census_ = rdr_scene::wlight::Census{};
    latches_ = no_manager_ = unreadable_ = unresolved_ = 0;
    caster_stamps_ = caster_unstamped_ = 0;
  }

 private:
  mutable std::mutex mu_;
  rdr_scene::wlight::Store store_;
  rdr_scene::wlight::Census census_;
  uint64_t latches_ = 0, no_manager_ = 0, unreadable_ = 0, unresolved_ = 0, caster_stamps_ = 0, caster_unstamped_ = 0;
};
inline Wl& The() {
  static Wl w;
  return w;
}

}  // namespace rdr_scene::live::wlatch
