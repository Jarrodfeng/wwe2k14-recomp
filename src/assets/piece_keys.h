// S103 W2B / ENT (docs\agents\s103\ENT.md): which pac entries of a wrestler attire are models the game builds as SUBM pieces. The game's own key list is the descriptor builder sub_8271EF80
// (0x8271EFA4-0x8271F18C): 32, 26, 27, 28, 29, 33, 1, 20000-20002, 21000-21002, 21010, 21011, 21020, 21030, 30000, 30001 and 10000-10999, plus the body (0); a key is built whenever the attire PACH holds it.
// The old filter here (0, 26, 27, 10000-39999) never loaded 28 / 29 (Foley's Mr. Socko, Savage's glasses, Hogan's neck pieces, Del Rio p3 / Cena Retro with kami_b nodes). Key 1 is a body entry of a PROP
// pac (prop slots 1..9), so it is taken only as the game's piece when the pac is not a prop pac. Pure.
#pragma once
#include <cstdint>

namespace assets {

inline constexpr bool WrestlerEntryIsModel(uint32_t id, bool prop_slots) {
  if (id == 0) return true;
  if (prop_slots) return id >= 1 && id <= 9 ? true : (id == 26 || id == 27 || (id >= 10000 && id < 40000));   // a prop pac keeps its slot ids; the old range for the rest
  return (id >= 26 && id <= 29) || id == 32 || id == 33 || id == 1 || (id >= 20000 && id <= 20002) || (id >= 21000 && id <= 21002) || id == 21010 || id == 21011 || id == 21020 || id == 21030 ||
         id == 30000 || id == 30001 || (id >= 10000 && id <= 10999);
}
// The filter before W2B / ENT (kept for the unit's planted twin).
inline constexpr bool WrestlerEntryIsModelOld(uint32_t id, bool prop_slots) { return id == 0 || id == 26 || id == 27 || (id >= 10000 && id < 40000) || (prop_slots && id >= 1 && id <= 9); }

}  // namespace assets
