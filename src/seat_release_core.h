// S110 SEATREL (docs/research/s110/MODELKEEP.md "Sites the build will touch" 1): the pure part of what a seat bind releases. Std only: tools/seats/
// seat_release_unit.cpp compiles this exact header with a reader callback over synthetic memory; src/seat_release.h is the game side.
//
// The game's slot manager [0x837635A4] keeps 12 model descriptors (192 bytes each at [mgr + 8] + 192 i; +52 = loaded, +28 = model key) and keeps a loaded
// one while the next request for its slot has the same key (a 360 habit: it saves a reload). A seat rewrites keys to TO's 397xx, so the key behind a slot
// stays while the file behind it changes: the bind must release what the game holds for TO, the game's own way (sub_8251ECA0 + sub_8258BC40).
// The select screen's picture pool [[0x839294B8] + 104] is a second keep, keyed by id: the bind asks it again (seat_bind_core.h ReaskStores), the pool's constants are here.
// `read(addr, value)` returns false for an unreadable word; value is the guest word already in host order.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "menu_home_core.h"  // menu_home::IsFrontEndSeq: the front-end sequence ids (one list, the game's own SmartMenu gate's)

namespace wwe2k14::seat_release {

// Review S110 SEATREL F1: a seat (and its release) runs only in a front-end SmartMenu sequence (75 = the main menu and its setup / select screens,
// 80 / 81; menu_home_core.h). The match (load, entrances, the match, pause, replay, the victory scene) is sequence 9 (docs/systems/menu_ui.md): a release
// there frees the model of a wrestler the game is drawing. The creation suite (76 Create a Superstar, 77 Threads, ...) builds models of its own: refused too.
// Empty = allowed; else the reason.
inline std::string SequenceRefusal(uint32_t seq) {
  if (menu_home::IsFrontEndSeq(seq)) return std::string();
  return "the game is in sequence " + std::to_string(seq) + (seq == 9 ? " (in game: a match is loaded)" : " (not a front-end menu)") +
         "; a seat binds only in the main menu or its setup screens (sequences 75 / 80 / 81)";
}

constexpr uint32_t kSlotMgrPtr = 0x837635A4;   // the slot manager; [mgr + 8] = the 12 descriptors, 192 bytes each
constexpr uint32_t kScenePtr = 0x8376303C;     // the scene; [scene + 92 + 4 i] = character slot i's node
constexpr uint32_t kUiPtr = 0x839294B8;        // the UI system object; [ui + 104] = the participant panels' picture pool
constexpr int kDescriptors = 12, kCharacterSlots = 9;
constexpr uint32_t kDescStride = 192, kDescLoaded = 52, kDescKey = 28;
constexpr uint32_t kPoolOff = 104, kPoolCount = 20, kPoolArray = 24, kPoolStride = 56, kPoolState = 52, kPoolMax = 64;

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

// The descriptors i (0..11) that are loaded and hold a key of character TO (key / 100 == TO: every attire and kind digit).
template <class Read>
inline std::vector<int> SlotsToRelease(Read&& read, int to) {
  std::vector<int> out;
  uint32_t mgr = 0, arr = 0;
  if (!read(kSlotMgrPtr, mgr) || !Plausible(mgr) || !read(mgr + 8, arr) || !Plausible(arr)) return out;
  for (int i = 0; i < kDescriptors; ++i) {
    const uint32_t d = arr + kDescStride * uint32_t(i);
    uint32_t loaded = 0, key = 0;
    if (!read(d + kDescLoaded, loaded) || loaded == 0) continue;
    if (!read(d + kDescKey, key)) continue;
    if (key / 100 == uint32_t(to)) out.push_back(i);
  }
  return out;
}

}  // namespace wwe2k14::seat_release
