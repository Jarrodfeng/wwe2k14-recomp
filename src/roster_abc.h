// S110 DLCRANK (docs\research\s110\DLCRANK.md): the DOWNLOADED SUPERSTARS tab in ONE alphabetical order (the game's DLC wrestlers and the imported
// superstars together). The owner's decision, 2026-11-24 01:00. Additive and toggled: --wwe2k14_roster_abc (default ON), read through AbcOn().
//
// Hook: sub_82924E00, the derive step every select-list build calls right after its rank sort (r4 = the six lists; its 4 call sites are in
// sub_82927390 / sub_82927550 / sub_82927728). Before the original runs, list 4 (r4 + 16016: count u32, ids at +4 + 4 i, flag words at +2004 + 4 i) has its
// DOWNLOADED entries (bit 31 clear, 100 <= id < 500, roster row +17 != 0: the DOWNLOADED tab's own test, sub_8282F748) put in alphabetical order inside the
// positions they already hold; the id word and the flag word of an entry move together. The original then copies list 4 into lists 0 / 1 / 2 and re-sorts
// list 3, so every tab inherits the order. NO roster row is written and nothing outside list 4 (UI memory, never the save): off = the stock order at the
// next list build. The pure half (the permutation) is src\roster_abc_core.h (unit: tools\roster_abc\roster_abc_unit.cpp).
// Logs: one line at boot (on / off), one `[abc]` line when a build's resulting order differs from the previous build's; nothing per frame.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <string>
#include <vector>

#include "guest_call.h"
#include "letter_jump.h"  // NameOf (the one name getter), kTextManagerPtr, sub_827DA788
#include "roster_abc_core.h"
#include "seats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_roster_abc, true, "wwe2k14",
                    "S110 DLCRANK: the DOWNLOADED SUPERSTARS tab of the select screens lists the game's DLC wrestlers and the imported superstars together in one "
                    "alphabetical order (the order of the select lists only; no roster row and nothing in the save is written). Off = the stock order (DLC in pack "
                    "order, then imports) at the next list build. See src/roster_abc.h");

REX_EXTERN(__imp__sub_82924E00);

namespace wwe2k14::roster_abc {

constexpr uint32_t kList4 = 16016;      // r4 + 16016: list 4 (count, then the ids at +4, the flag words at +2004)
constexpr uint32_t kFlagsAt = 2004;     // the flag word of entry i: list + 2004 + 4 i
constexpr uint32_t kRowPack = 17;       // roster row +17: the pack code (!= 0 = a DOWNLOADED row; sub_8282CEE8 / sub_8282F748)
constexpr uint32_t kMaxCount = 500;

inline bool AbcOn() { return REXCVAR_GET(wwe2k14_roster_abc); }

inline std::vector<uint32_t> g_last_order;  // the downloaded ids of the previous build, in the order they were left (the log only)

// Reorders list 4 at `lists` (= r4). true = the words were rewritten.
inline bool Reorder(PPCContext& ctx, uint8_t* base, uint32_t lists) {
  if (!seats::Plausible(lists)) return false;
  const uint32_t L = lists + kList4;
  if (!seats::Readable(L, 4)) return false;
  const uint32_t count = seats::U32(base, L);
  if (count < 1 || count > kMaxCount) return false;
  if (!seats::Readable(L, kFlagsAt + 4 * count)) return false;
  uint32_t mgr = 0;
  if (!seats::Readable(letter_jump::kTextManagerPtr, 4)) return false;
  mgr = seats::U32(base, letter_jump::kTextManagerPtr);
  if (!seats::Plausible(mgr)) return false;

  scene_director::Caller call(ctx, base);
  std::vector<uint32_t> ids(count), flags(count);
  std::vector<AbcItem> items(count);
  uint32_t downloaded = 0;
  for (uint32_t i = 0; i < count; ++i) {
    ids[i] = seats::U32(base, L + 4 + 4 * i);
    flags[i] = seats::U32(base, L + kFlagsAt + 4 * i);
    const uint32_t id = ids[i];
    if ((id & 0x80000000u) || id < letter_jump::kCawIds || id >= kMaxCount) continue;
    const uint32_t row = call(sub_827DA788, {id});
    if (!seats::Plausible(row) || !seats::Readable(row, kRowPack + 1)) continue;
    if (seats::P(base, row)[kRowPack] == 0) continue;
    items[i].downloaded = true;
    items[i].name = letter_jump::NameOf(base, call, id, mgr);
    ++downloaded;
  }
  if (downloaded < 2) return false;

  const std::vector<int> perm = AbcOrder(items);
  std::vector<uint32_t> now;  // the downloaded ids as the list now reads
  for (uint32_t i = 0; i < count; ++i)
    if (items[i].downloaded) now.push_back(ids[perm[i]]);
  const bool identity = IsIdentity(perm);
  if (!identity) {
    for (uint32_t i = 0; i < count; ++i) {  // id AND flag words together
      seats::PutBe32(seats::P(base, L + 4 + 4 * i), ids[perm[i]]);
      seats::PutBe32(seats::P(base, L + kFlagsAt + 4 * i), flags[perm[i]]);
    }
  }
  if (now != g_last_order) {
    uint32_t moved = 0;
    std::string first;
    int shown = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (perm[i] != int(i)) ++moved;
      if (items[i].downloaded && shown < 3) {
        first += (shown ? ", '" : "'") + items[perm[i]].name + "'";
        ++shown;
      }
    }
    REXLOG_INFO("[wwe2k14][abc] list built: {} downloaded rows, {} moved; first {}", downloaded, moved, first);
    g_last_order = now;
  }
  return !identity;
}

inline void Install() {
  REXLOG_INFO("[wwe2k14][abc] downloaded tab alphabetical: {} (--wwe2k14_roster_abc)", AbcOn() ? "on" : "off");
}

}  // namespace wwe2k14::roster_abc

// ---------------------------------------------------------------------------------------------------------------- the hook
REX_HOOK_RAW(sub_82924E00) {
  if (wwe2k14::roster_abc::AbcOn()) wwe2k14::roster_abc::Reorder(ctx, base, ctx.r4.u32);
  __imp__sub_82924E00(ctx, base);
}
