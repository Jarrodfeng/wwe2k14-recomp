// S110 SEATREL (docs/research/s110/MODELKEEP.md "Sites the build will touch" 1): the seat bind releases the models the game still holds for the seat, the game's
// own way, so the next match reads the file again (the new man's, through the re-pointed node) instead of keeping match 1's model. Called from the bind
// (src/seat_bind.h). The pure choice (which descriptors) is src/seat_release_core.h; the picture pool is re-asked, not refused (seat_bind_core.h ReaskStores).
//
// What the game does when a slot's key changes (sub_827E51E8 +1DC..+228, read again for this build): for a character slot i < 9 with a scene node
// [[0x8376303C] + 92 + 4 i] it calls sub_8251ECA0(scene, i) (node release), then always sub_8258BC40(mgr, i) (the dirty flag [mgr + 84 + 4 i] = 1 when the
// descriptor is loaded). The per-frame pass (sub_8258C4E8) then frees the descriptor. Safe where the bind runs: the game thread, no match loaded.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>

#include "guest_call.h"  // scene_director::Caller
#include "scenario.h"    // scenario::U32
#include "seat_release_core.h"

REX_EXTERN(sub_8251ECA0);  // scene node release (r3 = the scene, r4 = the character slot)
REX_EXTERN(sub_8258BC40);  // slot manager: mark the slot's descriptor for release (r3 = the manager, r4 = the slot)

namespace wwe2k14::seat_release {

inline bool ReadGuestWord(uint32_t a, uint32_t& v) {
  if (!Plausible(a)) return false;
  v = scenario::U32(a);
  return true;
}

// For every loaded descriptor of character TO: the game's two release calls. Returns how many slots were released; logs one line per slot, and one line when
// nothing was held.
inline int ReleaseSeatModels(PPCContext& ctx, uint8_t* base, int to) {
  // Never while a match is loaded (review F1): Bind refuses first; this guard keeps any later caller from releasing a model the game is drawing.
  if (const std::string no = SequenceRefusal(scenario::SequenceId()); !no.empty()) {
    REXLOG_ERROR("[wwe2k14][seats] release {}: NOT released: {}", to, no);
    return 0;
  }
  const std::vector<int> slots = SlotsToRelease(ReadGuestWord, to);
  if (slots.empty()) {
    REXLOG_INFO("[wwe2k14][seats] release {}: the slot manager holds no loaded model of this character; nothing to release", to);
    return 0;
  }
  uint32_t scene = 0, mgr = 0;
  ReadGuestWord(kScenePtr, scene);
  ReadGuestWord(kSlotMgrPtr, mgr);
  scene_director::Caller call(ctx, base);
  for (const int i : slots) {
    uint32_t key = 0, node = 0;
    uint32_t arr = 0;
    if (ReadGuestWord(mgr + 8, arr)) ReadGuestWord(arr + kDescStride * uint32_t(i) + kDescKey, key);
    const bool has_node = i < kCharacterSlots && Plausible(scene) && ReadGuestWord(scene + 92 + 4 * uint32_t(i), node) && node != 0;
    if (has_node) call(sub_8251ECA0, {scene, uint64_t(i)});
    call(sub_8258BC40, {mgr, uint64_t(i)});
    REXLOG_INFO("[wwe2k14][seats] release {}: slot {} (model key {}): scene node {}, descriptor marked for the game's per-frame release", to, i, key,
                has_node ? "released (sub_8251ECA0)" : "none");
  }
  return int(slots.size());
}

}  // namespace wwe2k14::seat_release
