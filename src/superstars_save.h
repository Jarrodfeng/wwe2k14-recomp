// wwe2k14 - S105 IMP / S110 SEAT-B: imported superstars reach the game's tables and a save made before them (docs\research\s110\SEAT1.md section 4).
//
// Hook: sub_82818798 = the game's own "new DLC since the save" pass (called from the save load, sub_827F3500, and from the DLC pass end,
// sub_8281DAF0 +5C). After it runs, src\seats.h AfterRowPass seats the library's residents: their pac-list nodes (once), then for each one whose live
// roster row is not active (the game's own rule for a row it keeps) his row and profile. The pass itself never touches a seat (the frame's chEtc rows
// are blank), so the old "clear the pack's bit in the saved DLC mask" trick (src\superstars_mask.h) is gone: the residents no longer ride the DLC
// manager's copy. Game logic, not a renderer hook: it runs in the stream-free and the stream build and on Android. Needs the pack offered
// (superstars::g_active, set by src\dlc.h PatchGate); otherwise nothing changes.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>

#include "dlc.h"
#include "seats.h"
#include "slot_handover.h"
#include "superstars.h"
#include "superstars_migrate.h"

REX_EXTERN(__imp__sub_82818798);
REX_HOOK_RAW(sub_82818798) {
  __imp__sub_82818798(ctx, base);
  wwe2k14::slot_handover::BeforeSeating(ctx, base);  // S110 SEAT2: inside a save load only, a seat that changed hands is exported + purged + re-seated
  wwe2k14::seats::AfterRowPass(ctx, base);
  // S110 BANKID: MIGRATION BAND-AID for saves made before an import's entrance bank got its own number (src\superstars_migrate.h; removable
  // once old packs are gone). Only for the pack the game was offered; does nothing for a save that is already current. Its "pack row" is the
  // resident's profile from his card (converted by the game's own functions, src\seats.h PackProfile).
  if (wwe2k14::superstars::g_active && !wwe2k14::seats::g_roster.res.empty()) {
    std::vector<int> slots;
    std::map<int, std::vector<int>> was;
    for (const auto& r : wwe2k14::seats::g_roster.res) {
      slots.push_back(r.seat);
      if (!r.was.empty()) was[r.seat] = r.was;
    }
    wwe2k14::superstars_migrate::MigrateEntranceWords(base, [](int seat) { return wwe2k14::seats::PackProfile(seat); }, slots,
                                                      [](const std::string& l) { REXLOG_INFO("{}", l); }, was);  // (S110 TRONSLOT: + the movie words equal to the slot)
  }
}
