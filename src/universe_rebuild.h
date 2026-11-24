// S110 UNIAVAIL (docs\research\s110\UNIAVAIL.md): the GAME side. After a seat is handed over, the game's own card rebuild (the Universe card builder's
// async job sub_82B9BBF0, queued by the step that ran the roster check and by every later step that day) must not book the seat's NEW occupant into the
// leaver's match. The builder decides who is bookable from the row-active test sub_822C1CF0 (under sub_82B62720, sub_82B638A8 and the plan rows); a
// deleted Created Superstar fails that test. For the seats the roster check answered gone, the test answers 0 INSIDE THE JOB'S OWN FRAME ONLY (the job
// runs on the game's worker thread; the frame is thread-local), for every job of that Universe day (the date word U + 637800); the first job on another
// day ends the hold. The pure half (the hold, the frame, the record line) is src\universe_rebuild_core.h.
// Everything outside a job frame (select lists, match setup, the card screen, the main thread) keeps the game's answers: the new occupant stays visible.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <string>

#include "seats.h"
#include "universe_rebuild_core.h"

REX_EXTERN(__imp__sub_82B9BBF0);  // the card generator's async job (r3 = the generator)
REX_EXTERN(__imp__sub_822C1CF0);  // the row-active test (r3 = id; != 0 when the row's byte +16 == 1 for ids 100..499)

namespace wwe2k14::universe_rebuild {

constexpr uint32_t kDatePtr = 0x8376F3C0;  // [it] + 486564 + 637800 = the Universe date word (sub_827D9020's U + 637800)
constexpr uint32_t kDateOff = 486564 + 637800;
constexpr uint32_t kModeOff = 14424;       // generator + 14424 = the job's mode (set by sub_82B9C190)

inline std::mutex g_mx;
inline Held g_held;  // (g_mx)

// Today's Universe date word; false when a guest read is not possible (nothing is then noted or held).
inline bool Today(uint8_t* base, uint32_t& day) {
  const uint32_t p = seats::U32(base, kDatePtr);
  if (!seats::Plausible(p) || !seats::Readable(p + kDateOff, 4)) return false;
  day = seats::U32(base, p + kDateOff);
  return true;
}

// Called by the roster-check hook after the original returned, with the seats the check answered gone.
inline void NoteCheck(uint8_t* base, const std::set<int>& gone) {
  if (gone.empty()) return;
  uint32_t day = 0;
  if (!Today(base, day)) {
    REXLOG_WARN("[wwe2k14][handover] universe: the date word is not readable; the builder will not be told about {} seat(s)", gone.size());
    return;
  }
  std::lock_guard lock(g_mx);
  NoteCheck(g_held, day, gone);
}
inline Held HeldNow() {
  std::lock_guard lock(g_mx);
  return g_held;
}
inline void RestoreHeld(const Held& h) {
  std::lock_guard lock(g_mx);
  g_held = h;
}
// Every load path: the hold becomes this save's own (or none). Takes only the rebuild mutex (callers may hold slot_handover's g_mx: that order only).
inline void LoadHeld(bool have_entry, const Held& entry_held) {
  std::lock_guard lock(g_mx);
  g_held = HeldAfterLoad(g_held, have_entry, entry_held);
}

}  // namespace wwe2k14::universe_rebuild

// The card builder's async job (worker thread). One log line per job that holds seats; the frame covers the whole original (table build, ranking lists,
// plan rows), so every read the job makes of the row-active test below sees the held seats as inactive.
REX_HOOK_RAW(sub_82B9BBF0) {
  namespace ur = wwe2k14::universe_rebuild;
  const uint32_t gen = ctx.r3.u32;
  std::set<int> seats;
  uint32_t day = 0;
  if (ur::Today(base, day)) {
    std::lock_guard lock(ur::g_mx);
    seats = ur::SeatsForJob(ur::g_held, day);
  }
  if (seats.empty()) {
    __imp__sub_82B9BBF0(ctx, base);
    return;
  }
  std::string list;
  for (int s : seats) list += " " + std::to_string(s);
  REXLOG_INFO("[wwe2k14][handover] universe: builder job (gen {:08X} mode {}, day {}): seats{} answer row-inactive", gen,
              wwe2k14::seats::U32(base, gen + ur::kModeOff), day, list);
  ur::JobFrame frame(std::move(seats));
  __imp__sub_82B9BBF0(ctx, base);
}

// The row-active test, called from very many places on every thread. DEPARTS FROM THE GAME ON PURPOSE (DECISIONS 2026-11-24 10:58: a leaver's Universe
// bookings are stripped, as close to the game's own logic as possible): inside a builder job's frame a held seat is answered inactive (r3 = 0), which is
// how the game sees a deleted Created Superstar, so its own card rebuild books somebody else. Outside a frame the only cost is the thread-local test.
REX_HOOK_RAW(sub_822C1CF0) {
  if (wwe2k14::universe_rebuild::RowInactiveInJob(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_822C1CF0(ctx, base);
}
