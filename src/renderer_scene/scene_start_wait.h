// S110 ENTRWAIT2 (docs\research\s110\ENTRWAIT2.md; the refuted first form: ENTRWAIT.md): in normal play the game's scene start waits for OUR scene behind its own loading card. The card
// stays up and the game's own clock for the entrance (or the match) starts only when our loads are done, so its own fade from black runs over a drawn scene.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes (the unit tools\renderer\host_core_unit.cpp drives it; src\scene_start_wait_hooks.h applies it).
//
// The start points; at each the game's own "not yet, call me again" is answered and nothing else of the game runs in that call [C listings]:
//   * a PART's first update: every part class (base 0x8200A4BC ctor sub_8232E828, story 0x8200A410 sub_8232D1A0, Season 0x8200A3C0 sub_8232CFD8; picked by sub_8231C538) has slot 1 =
//     sub_8232E1E8, which calls slot 4 while [part + 124] == 0. Slot 4 (base / Season sub_8232E728; story sub_8232D250 = the card drop, then sub_8232E728) makes the bank's entrance
//     controller (slot 3 sub_8232E8A0 -> [part + 16]), starts it, starts the 8-frame fade from black (sub_8232E630) and sets [part + 124] = 1.
//   * the CARD's drop in an exhibition / Season start: the card item (vtable 0x8200A550, ctor sub_82331DD8, made by sub_8231CAF0 only while the card is up; inert in story) runs its stage
//     sub_82331BC0 each frame: it drops the card (sub_827E62A8) and moves on (+24 = sub_82331940), or returns with +24 unchanged (the game's own wait). The part (task group 1) and the card
//     item (group 33) are separate tasks of sub_82C302A0 with no order between them: the part starts on its first tick whatever the card does, so both are held.
//   * a match start with no entrance: sub_8273D0C0 step 3 (delay +8 == 0, step +12 == 3) with the scene word [0x836F3940] != 4 starts the match FADE's 30-frame fade; its 0 return is the
//     game's own "not yet" (sub_825C0718 +AC..+B4 polls it again next frame). With the word 4 step 3 only holds the FADE black for the entrance: no start there.
// ONE decision, latched per start: ARMED at each match load (sub_8273D0C0 step 0) and at each card item (sub_82331DD8). A point holds only while armed AND the loading card is up: the
// requester G = [0x83924744] is not idle (G+8 a posted request, sub_827E5740 +44; G+4 its state and G+84 the screen while it is shown or closing: sub_827E67C0 takes the request at +88..+94
// and CLEARS G+8 while the card is on screen, then frees G+84 / G+4 at +240..+2A8; the CASREADY rule, src\cas_goto_core.h). The first pass, or a start point reached with the card
// down, spends the arm: the other point passes in the same
// frame, and no later start (the next wrestler's part, a mid-match entrant, a run-in) ever waits; nothing is ever held over a picture the player sees.
// The decision: the host handled a frame after this start's first poll and, at that frame, had asked for nothing new (a build) and had no non-bulk load queued, running or waiting to be
// adopted, and the arena the game opened was handed to the loader (LiveSceneDraw::PublishStartWait; a frame that builds nothing publishes the loader's state only: the stale host opens by
// state). Every load ends (adopted or failed): no clock, no cap, no frame count. A stalled host opens it by STATE too: no drawer, nothing ever handled, a lost device.
// Off under det / replay and in scripted runs unless --wwe2k14_scene_start_wait=1.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene::startwait {

#ifdef WWE2K14_HOST_PLANTS
inline int g_plant = 0;   // test only: 1 no wait; 2 a build from before the ask counts; 3 a stalled host keeps the wait; 4 scripts wait; 5 step 3 starts with the entrance word; 6 a pass keeps the arm;
                          // 7 only the story part class is a start (the refuted rule); 8 a start with the card down still holds; 9 a frame that builds nothing publishes nothing (the stale host);
                          // 10 the card counts as up only while a request is posted (G+8 == 1: the landed rule, false while the card is on screen);
                          // 11 a body asked after the first poll does not re-stamp the decision; 12 the bodies are asked with the card down; 13 a new start keeps the last start's bodies (start_bodies.h)
#define START_WAIT_PLANT(n) (::rdr_scene::startwait::g_plant == (n))
#else
#define START_WAIT_PLANT(n) false
#endif

inline constexpr uint32_t kStoryPartVtable = 0x8200A410u;    // sub_8232D1A0 (read only by plant 7, the refuted rule)
inline constexpr uint32_t kPartStartedOff = 124u;             // sub_8232E1E8 +10 reads it; sub_8232E728 +74 sets it
inline constexpr uint32_t kLoadDelayOff = 8u, kLoadStepOff = 12u, kLoadStepFirst = 0u, kLoadStepStart = 3u;   // sub_8273D0C0 +C / +4C; step 0 = +74..+B0, step 3 = +1A4..+294
inline constexpr uint32_t kSceneWord = 0x836F3940u, kSceneWordEntrances = 4u;            // sub_8273D0C0 +25C
inline constexpr uint32_t kLoadCard = 0x83924744u;   // the loading-screen requester G (update sub_827E67C0)
inline constexpr uint32_t kCardStateOff = 4u, kCardPendingOff = 8u, kCardScreenOff = 84u;   // G+4 state (1..3 shown / closing), G+8 a posted request (cleared when taken), G+84 the screen
inline constexpr uint32_t kLoadStepBodies = 2u;   // S110 ENTRBODIES: sub_8273D0C0 step 1 (+B4..+140) hands each slot's character (0x836F38D0 + 4 k) to sub_82512A58; from step 2 their bodies are asked

inline std::atomic<bool> g_host_on{false};       // the stream-free scene host's drawer exists (LiveSceneDraw ctor / dtor)
inline std::atomic<uint64_t> g_host_frame{0};    // the guest frame of the newest frame the host handled and published (0 = none yet)
inline std::atomic<bool> g_host_idle{false};     // at that frame: nothing new asked, the game's arena handed over, no non-bulk load queued / running / waiting to be adopted
inline std::atomic<bool> g_armed{false};         // a start behind the loading card is to come (a match load's step 0, a card item)
inline std::atomic<uint64_t> g_asked_at{0};      // the guest frame of the pending start's first poll (0 = none pending)
inline std::atomic<uint64_t> g_bodies_at{0};     // S110 ENTRBODIES: the guest frame this start's body set last grew (start_bodies.h; 0 = none)

// The switch: mode -1 = auto (on in normal play: off under det / replay and in a scripted run), 0 = off, 1 = on (a check run turns it on).
inline bool WaitOn(int mode, bool det, bool script) { return mode == 1 || (mode < 0 && !det && (!script || START_WAIT_PLANT(4))); }

// The start points (the hooks read the words; see the header).
inline bool PartStartPoint(uint32_t vtable, uint32_t started) { return started == 0 && (!START_WAIT_PLANT(7) || vtable == kStoryPartVtable); }
inline bool MatchStartPoint(uint32_t delay, uint32_t step, uint32_t scene_word) { return delay == 0 && step == kLoadStepStart && (scene_word != kSceneWordEntrances || START_WAIT_PLANT(5)); }
inline bool LoadFirstStep(uint32_t delay, uint32_t step) { return delay == 0 && step == kLoadStepFirst; }
inline bool CardUp(uint32_t card, uint32_t state, uint32_t pending, uint32_t screen) {
  if (START_WAIT_PLANT(10)) return card != 0 && pending == 1;
  return card != 0 && (state != 0 || pending != 0 || screen != 0);
}

struct Facts {
  bool host_on = false;      // g_host_on
  bool device_lost = false;  // rdr::DeviceLost()
  uint64_t asked_at = 0;     // this start's first poll (non-zero)
  uint64_t bodies_at = 0;    // S110 ENTRBODIES: g_bodies_at (the last body ask of this start; 0 = none)
  uint64_t host_frame = 0;   // g_host_frame
  bool host_idle = false;    // g_host_idle
};
// A host that cannot finish the wait opens it (by state, never by a clock).
inline bool HostStalled(const Facts& f) { return !START_WAIT_PLANT(3) && (!f.host_on || f.host_frame == 0 || f.device_lost); }
// The decision: may the game start its scene now?
inline bool MayStart(const Facts& f) {
  if (START_WAIT_PLANT(1) || HostStalled(f)) return true;
  const bool after = START_WAIT_PLANT(2) || (f.host_frame > f.asked_at && f.host_frame > f.bodies_at);   // S110 ENTRBODIES: and after the last body ask
  return after && f.host_idle;
}
// The host side: is this frame idle? asked_now = a build of this frame asked for something new (false for a frame that built nothing).
inline bool HostIdle(bool asked_now, bool game_arena_pending, uint32_t running, bool undelivered, bool queued) {
  return !asked_now && !game_arena_pending && running == 0 && !undelivered && !queued;
}
// S110 ENTRBODIES (start_bodies.h): the game's characters' bodies are asked only while a start is armed AND the loading card is up (a later start, a mid-match entrant: never).
inline bool AskBodiesNow(bool armed, bool card_up) { return armed && (card_up || START_WAIT_PLANT(12)); }
// Game thread: this start's body set grew at guest frame `now`: only a host frame after it counts (MayStart).
inline void NoteAsked(uint64_t now) {
  if (START_WAIT_PLANT(11) || !g_armed.load(std::memory_order_acquire)) return;
  g_bodies_at.store(now | 1u, std::memory_order_relaxed);
}
// Host thread: a start is held now (armed, and its first poll or a body ask happened): the host asks the arena's crowd classes for it (LiveSceneDraw::StartCrowdIn).
inline bool StartPending() {
  return g_armed.load(std::memory_order_acquire) && (g_asked_at.load(std::memory_order_relaxed) != 0 || g_bodies_at.load(std::memory_order_relaxed) != 0);
}

// Game thread: a new start behind the loading card is to come (a match load's step 0, a card item); a stale ask is forgotten.
inline void Arm() {
  g_asked_at.store(0, std::memory_order_relaxed);
  g_bodies_at.store(0, std::memory_order_relaxed);   // S110 ENTRBODIES
  g_armed.store(true, std::memory_order_release);
}
// Host thread, after each build (LiveSceneDraw::PublishStartWait).
inline void NoteHostBuild(uint64_t guest_frame, bool idle) {
  g_host_idle.store(idle, std::memory_order_relaxed);
  g_host_frame.store(guest_frame, std::memory_order_release);
}
// Host thread, a handled frame that built nothing (no record, stale OURS inputs, no picture / size): the loader's state only.
inline void NoteHostNoBuild(uint64_t guest_frame, bool loader_idle) {
  if (START_WAIT_PLANT(9)) return;
  NoteHostBuild(guest_frame, loader_idle);
}
// This start has happened: forget the ask, spend the arm.
inline void Spend() {
  g_asked_at.store(0, std::memory_order_relaxed);
  g_bodies_at.store(0, std::memory_order_relaxed);   // S110 ENTRBODIES
  if (!START_WAIT_PLANT(6)) g_armed.store(false, std::memory_order_relaxed);
}
// Game thread: one poll of a start point at guest frame `now`. true = answer the game's own "not yet". Not armed: never. The card down: never, and the start is spent. The first poll stamps
// the ask; the pass spends the arm, so the start's other point passes too.
inline bool Poll(uint64_t now, bool host_on, bool device_lost, bool card_up) {
  if (!g_armed.load(std::memory_order_acquire)) return false;
  if (!card_up && !START_WAIT_PLANT(8)) {
    Spend();
    return false;
  }
  uint64_t asked = g_asked_at.load(std::memory_order_relaxed);
  if (asked == 0) asked = now | 1u, g_asked_at.store(asked, std::memory_order_relaxed);
  Facts f;
  f.host_on = host_on, f.device_lost = device_lost, f.asked_at = asked;
  f.bodies_at = g_bodies_at.load(std::memory_order_relaxed);   // S110 ENTRBODIES
  f.host_frame = g_host_frame.load(std::memory_order_acquire);
  f.host_idle = g_host_idle.load(std::memory_order_relaxed);
  if (!MayStart(f)) return true;
  Spend();
  return false;
}

}  // namespace rdr_scene::startwait
