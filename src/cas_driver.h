// S110 CASROW: ONE owner of the Create a Superstar editor's presses. The go-to (src/cas_goto.h), the item pick (src/cas_pick.h) and the
// row verb (src/cas_row.h) each press the editor's pages through the game's own handlers; two of them pressing in one frame would drive
// two pages at once (the CASPICK review: the go-to's B puts the opening part back under a running pick). This word replaces the pairwise
// guard cas_pick had (Reason::kGotoArmed): a request is TAKEN only while nobody else drives (first armed wins), the others refuse with
// their own `busy` reason, and the driver gives the word back when its request ends (applied, arrived, settled, stopped or refused).
// Std only, so tools/goto/cas_row_test.cpp drives it. Game thread only: every Step runs from src/cas_launch.h Tick.
#pragma once

#include <cstdint>

namespace wwe2k14::cas_driver {

enum Driver : uint32_t { kNone = 0, kGoto = 1, kPick = 2, kRow = 3 };

inline uint32_t g_owner = kNone;  // who drives the editor's presses now (game thread only)

inline const char* Name(uint32_t d) {
  switch (d) {
    case kGoto: return "the go-to (wwe2k14_cas_goto)";
    case kPick: return "the item pick (wwe2k14_cas_pick)";
    case kRow: return "the row verb (wwe2k14_cas_row)";
    default: return "nobody";
  }
}
// True when `who` may drive now: nobody does, or it already does (a re-arm). False = another driver holds the editor.
inline bool Claim(uint32_t who) {
  if (g_owner != kNone && g_owner != who) return false;
  g_owner = who;
  return true;
}
// The request of `who` has ended: the word is free again (a no-op when someone else holds it).
inline void Release(uint32_t who) {
  if (g_owner == who) g_owner = kNone;
}

}  // namespace wwe2k14::cas_driver
