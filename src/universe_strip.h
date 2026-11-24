#pragma once
// S110 UNISTRIP (docs\research\s110\UNISTRIP.md): the game side. After the Universe roster check (sub_82B75228) has run for handed-over seats, the game
// itself rebuilds the unplayed AUTO matches of the current card at auto ordinals 0..6 (the step that ran the check queues the card builder in mode 1; it
// waits for its async generator). A match inserted by a special builder (flag bit 3: the title-challenge match) and an auto match at an auto ordinal >= 7
// are never rebuilt, and the game's own "Delete" for a flagged one is a stub: the leaver's id would stay booked. For those matches we do what the game does
// to a random-fill slot: set bit 0x40 of the leaver's participant byte +8 and call the game's single-slot replacer sub_82B90820(match index), which picks a
// random available wrestler, writes his id and clears the +7 flag. When it finds nobody it writes nothing: we then put the +8 byte back as it was.
// DEPARTS FROM THE GAME ON PURPOSE: the game leaves such a booked wrestler flagged; here it is replaced (DECISIONS 2026-11-24 10:58: a leaver's Universe
// bookings are stripped, as close to the game's own logic as possible).
// The builder's own jobs see the leaver's seat row-inactive for the rest of that Universe day (universe_rebuild.h, S110 UNIAVAIL).
// Known gap, named: a match of types 20..23 (Royal Rumble / battle royal) or of the bare-id kind (sub_82B79F00) holds bare u16 ids and the game has
// no function that replaces one id: the seat stays booked there (one log line per such match).
// Called once per roster check that answered a seat gone, strictly AFTER g_roster_check is back to 0 (so the replacer's own sub_82B62720 asks get the
// game's answers: inside the scope KeepHeldSeat would offer a hidden held seat). Logs the matches naming a gone seat BEFORE the strip (the proof's line).
// Every guest read / write is bounds-checked; a refused read strips nothing more and logs once.
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <set>
#include <vector>

#include "guest_call.h"
#include "seats.h"
#include "universe_strip_core.h"

REX_EXTERN(sub_82B731E0);  // the current card's match (r3 = the Universe mode object, r4 = index 0..9), 0 = none
REX_EXTERN(sub_82B78CD0);  // match flag test (r3 = match, r4 = bit): bit 3 = inserted by a special builder
REX_EXTERN(sub_822BA578);  // match type predicate (r3 = type byte): != 0 for types 20..23
REX_EXTERN(sub_82B79F00);  // != 0 for the bare-id match kind (flag bits 10..8 == 100b)
REX_EXTERN(sub_82B58988);  // today's show index (r3 = the Universe mode object)
REX_EXTERN(sub_82B6E2F8);  // the show entry (r3 = the mode object, r4 = the show index): entry + 4 holds the current card's pointer (sub_82B731E0 +48..+54)
REX_EXTERN(sub_82B7B210);  // the auto ordinal of match r4 (r3 = the address of the card pointer): bit-3-clear matches counted in index order, -1 for bit 3
REX_EXTERN(sub_82B90820);  // the single-slot replacer (r3 = match index): refills each slot whose byte +8 has bit 0x40

namespace wwe2k14::universe_strip {

constexpr uint32_t kModePtr = 0x83929894;  // [[kModePtr] + kModeOff] = the Universe mode object (the roster check's r3)
constexpr uint32_t kModeOff = 36028;

inline bool g_refused_logged = false;  // one line per process for a refused read

inline bool Refuse(const char* what) {
  if (!g_refused_logged) {
    g_refused_logged = true;
    REXLOG_WARN("[wwe2k14][handover] universe: strip refused ({}: guest memory not readable); nothing stripped", what);
  }
  return false;
}
inline bool Rd8(uint8_t* base, uint32_t a, uint8_t& v) {
  if (!seats::Readable(a, 1)) return false;
  v = *seats::P(base, a);
  return true;
}
inline bool Rd16(uint8_t* base, uint32_t a, uint16_t& v) {
  if (!seats::Readable(a, 2)) return false;
  const uint8_t* p = seats::P(base, a);
  v = uint16_t((p[0] << 8) | p[1]);
  return true;
}

struct Marked {
  int i = 0;
  bool set[kMaxSlots] = {};
  uint8_t flag[kMaxSlots] = {};  // byte +8 before the strip
  uint16_t id[kMaxSlots] = {};
};

// `umode` = the roster check's r3; `gone` = the seats the check answered gone (called after the original returned, BEFORE the set is cleared).
inline void StripSpecialBookings(PPCContext& ctx, uint8_t* base, uint32_t umode, const std::set<int>& gone) {
  if (gone.empty()) return;
  if (umode == 0 || umode != seats::U32(base, seats::U32(base, kModePtr) + kModeOff)) {
    Refuse("the Universe mode object is not the roster check's");
    return;
  }
  scene_director::Caller call(ctx, base);
  const uint32_t entry = call(sub_82B6E2F8, {uint64_t(umode), uint64_t(call(sub_82B58988, {uint64_t(umode)}))});
  const uint32_t holder = entry + 4;
  if (entry == 0 || !seats::Readable(holder, 4)) {
    Refuse("the current card's pointer");
    return;
  }
  std::vector<Marked> marked;
  int named = 0, reached = 0, booked = 0;
  for (int i = 0; i < kMatches; ++i) {
    const uint32_t m = call(sub_82B731E0, {uint64_t(umode), uint64_t(i)});
    if (m == 0) continue;
    ++reached;
    uint8_t type = 0, count = 0, status = 0;
    if (!Rd8(base, m, type) || !Rd8(base, m + 2, count) || !Rd8(base, m + 3, status)) {
      Refuse("a card match header");
      break;
    }
    if (count > 0) ++booked;
    const bool special = call(sub_82B78CD0, {uint64_t(m), 3}) != 0;
    const bool rumble = call(sub_822BA578, {uint64_t(type)}) != 0;
    const bool bare_kind = !rumble && call(sub_82B79F00, {uint64_t(m)}) != 0;
    const bool bare = rumble || bare_kind;
    const int ordinal = int(int32_t(call(sub_82B7B210, {uint64_t(holder), uint64_t(i)})));
    const bool left = LeftByBuilder(special, ordinal);
    Marked mk;
    mk.i = i;
    bool any = false, names_gone = false;
    for (int k = 0; k < SlotCap(rumble, bare_kind) && k < count; ++k) {
      uint16_t id = 0;
      if (!Rd16(base, m + (bare ? BareIdOffset(k) : IdOffset(k)), id)) {
        Refuse("a participant id");
        break;
      }
      if (!gone.count(int(id))) continue;
      names_gone = true;
      ++named;
      const bool strip = StripSlot(status, left, bare, k, count, id, gone);
      REXLOG_INFO("[wwe2k14][handover] universe: card before strip: match {} type {} status {} special {} ordinal {} slot {} id {}: {}", i, int(type),
                  int(status), int(special), ordinal, k, id, SlotVerdictText(status, left, bare, strip));
      uint8_t flag = 0;
      if (bare || !Rd8(base, m + FlagByte(k), flag)) continue;
      if (!strip) continue;
      mk.set[k] = true, mk.flag[k] = flag, mk.id[k] = id;
      *seats::P(base, m + FlagByte(k)) = uint8_t(flag | kReplaceBit);  // bounds: Rd8 above read the same byte
      any = true;
    }
    if (names_gone && bare && left && status != kStatusResult)
      REXLOG_INFO("[wwe2k14][handover] universe: match {} holds bare ids (rumble / battle royal kind): a handed-over seat stays booked there (the game has no replacer)", i);
    if (any) marked.push_back(mk);
  }
  switch (Summarise(reached, booked, named)) {
    case CardState::NoCard:
      REXLOG_INFO("[wwe2k14][handover] universe: card before strip: no card (today's show is off or there is none): the game books it when a show is on");
      break;
    case CardState::Empty:
      REXLOG_INFO("[wwe2k14][handover] universe: card before strip: the current card is empty (a Universe not entered yet): the game books it after this check, with the seats' current occupants");
      break;
    case CardState::NoneNamed:
      REXLOG_INFO("[wwe2k14][handover] universe: card before strip: {} of {} match(es) booked, none names a seat answered gone", booked, kMatches);
      break;
    case CardState::Named: break;
  }
  for (const Marked& mk : marked) {
    const uint32_t m = call(sub_82B731E0, {uint64_t(umode), uint64_t(mk.i)});
    if (m == 0) {
      Refuse("a marked card match");
      continue;
    }
    call(sub_82B90820, {uint64_t(mk.i)});
    for (int k = 0; k < kMaxSlots; ++k) {
      if (!mk.set[k]) continue;
      uint16_t now = 0;
      uint8_t flag_now = 0;
      if (!Rd16(base, m + IdOffset(k), now) || !Rd8(base, m + FlagByte(k), flag_now)) continue;
      *seats::P(base, m + FlagByte(k)) = FlagAfterReplace(mk.flag[k], flag_now, mk.id[k], now);  // bounds: Rd8 above read the same byte
      if (now != mk.id[k])
        REXLOG_INFO("[wwe2k14][handover] universe: seat {} stripped from match {} slot {} -> id {}", mk.id[k], mk.i, k, now);
      else
        REXLOG_INFO("[wwe2k14][handover] universe: seat {} not replaced in match {} slot {} (the game found no available wrestler); it stays booked, byte +8 put back",
                    mk.id[k], mk.i, k);
    }
  }
}

}  // namespace wwe2k14::universe_strip
