// wwe2k14 - S105 IMP: imported superstars added after a save was made reach that save (docs\research\s105\IMP_B.md, IMP_ADV.md).
//
// Hook: sub_82818798 = the game's own "new DLC since the save" pass (called from the save load, sub_827F3500, and two paths never
// seen in our runs). Before it runs, pack 6's bit is cleared in the save's saved DLC mask (block B 0x839129E0, src\superstars_mask.h)
// so the game sees the superstars pack as new and copies the DLC rows of characters imported since into INACTIVE rows; the pass
// writes the bit back itself in the same call. Active rows (edited, or already imported) are never touched by that pass.
// Game logic, not a renderer hook: it runs in the stream-free and the stream build and on Android. Needs the pack offered
// (superstars::g_active, set by src\dlc.h PatchGate) and its id (dlc::g_gate_id); otherwise nothing changes.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>

#include "dlc.h"
#include "superstars.h"
#include "superstars_mask.h"

REX_EXTERN(__imp__sub_82818798);
REX_HOOK_RAW(sub_82818798) {
  namespace sm = wwe2k14::superstars_mask;
  const int id = wwe2k14::dlc::g_gate_id;
  // S105 IMP review L5: only when the DLC manager exists (the game's own pass, sub_82818798, reads the word at 0x8392476C first and
  // does nothing when it is 0: clearing the bit then would hide the pack for no reason). Big-endian, like every guest word.
  if (wwe2k14::superstars::g_active && sm::ValidGateId(id) && sm::LoadBE(base + 0x8392476C) != 0) {
    uint32_t was = 0, now = 0;
    const bool cleared = sm::ClearInGuestWord(base + sm::kSavedMaskAddr, id, &was, &now);
    {  // (N1: every call is logged: the pass runs at each save load, and the step check reads this line)
      REXLOG_INFO("[wwe2k14][dlc] pack {} re-offered to the save (saved mask {:08X} -> {:08X}{}): the game's own pass copies the "
                  "characters imported since into inactive rows", id, was, now, cleared ? "" : ", bit was already clear");
    }
  }
  __imp__sub_82818798(ctx, base);
}
