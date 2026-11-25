// wwe2k14 - S110 ENTRWAIT2: the game's scene start waits for OUR scene behind its own loading card, in normal play (src\renderer_scene\scene_start_wait.h: why, the start points, the decision).
// Hooks: sub_8232E1E8 (every part's per-frame update; its first call starts the entrance's controller and fade), sub_82331BC0 (the card item's stage: drops the loading card in an exhibition /
// Season start), sub_8273D0C0 (the match load task: step 0 arms; step 3 with no entrance to come starts the match FADE's fade) and sub_82331DD8 (the card item's ctor: arms).
// While the decision says "not yet", the game's own "not yet" is answered and nothing of the game runs in that call: the part stays unstarted / the card item stays on its stage / the step
// stays 3, and the game calls again next frame.
// Switch --wwe2k14_scene_start_wait (src\cvars\cvars_hand.cpp): -1 (default) = on in normal play, off under det / replay and in scripted runs (--wwe2k14_input_script); 1 = on; 0 = off.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/det.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "frame_stats.h"   // wwe2k14::g_guest_frame
#include "load_probe.h"    // load_probe::SafeRead32
#include "renderer/breadcrumbs.h"   // rdr::DeviceLost
#include "renderer_scene/scene_start_wait.h"
#include "scene_start_bodies.h"   // S110 ENTRBODIES: the game's characters' bodies, asked while the start is held

REXCVAR_DECLARE(int32_t, wwe2k14_scene_start_wait);
REXCVAR_DECLARE(std::string, wwe2k14_input_script);

namespace wwe2k14::scene_start_wait {
inline std::chrono::steady_clock::time_point g_hold_since{};   // game thread: the wall time of this start's first "not yet" (the log line only; it never decides)
inline uint64_t g_holds = 0;                                    // game thread: this start's "not yet" answers (both points)

inline bool WaitOnNow() {
  return rdr_scene::startwait::WaitOn(REXCVAR_GET(wwe2k14_scene_start_wait), rex::det::Enabled(), !REXCVAR_GET(wwe2k14_input_script).empty());
}
// The game's loading card is up: the requester G = [0x83924744] has a request posted (G+8), a state (G+4) or a screen (G+84) (scene_start_wait.h CardUp).
inline bool CardUpNow() {
  namespace sw = rdr_scene::startwait;
  uint32_t card = 0, state = 0, pending = 0, screen = 0;
  return load_probe::SafeRead32(sw::kLoadCard, &card) && card && load_probe::SafeRead32(card + sw::kCardStateOff, &state) &&
         load_probe::SafeRead32(card + sw::kCardPendingOff, &pending) && load_probe::SafeRead32(card + sw::kCardScreenOff, &screen) && sw::CardUp(card, state, pending, screen);
}
// S110 ENTRBODIES (src\scene_start_bodies.h): while a start is armed and the loading card is up, the bodies of the game's characters go to our loader now (the record names a body only once the
// game poses it, after the start). A body new to this start re-stamps the decision (scene_start_wait.h NoteAsked): only a host frame after the ask counts.
inline void AskBodies(uint64_t now, bool card) {
  namespace sw = rdr_scene::startwait;
  if (!sw::AskBodiesNow(sw::g_armed.load(std::memory_order_acquire), card)) return;
  if (scene_start_bodies::Publish()) sw::NoteAsked(now);
}
// One poll of a start point (game thread). true = answer the game's own "not yet". The line is logged once per start: at its pass, or when it was reached with no card up.
inline bool HoldNow(const char* point) {
  namespace sw = rdr_scene::startwait;
  if (!sw::g_armed.load(std::memory_order_acquire)) return false;
  const uint64_t now = g_guest_frame.load(std::memory_order_relaxed);
  const bool card = CardUpNow();
  AskBodies(now, card);   // S110 ENTRBODIES: before the decision reads the stamps
  if (sw::Poll(now, sw::g_host_on.load(std::memory_order_relaxed), rdr::DeviceLost(), card)) {
    if (g_holds++ == 0) g_hold_since = std::chrono::steady_clock::now();
    return true;
  }
  const long long ms = g_holds ? (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_hold_since).count() : 0;
  REXLOG_INFO("[scene][startwait] {} start at guest frame {}: {}; waited {} polls, {} ms; {} bodies of the game's characters asked", point, now,
              card ? "the loading card stayed up for our scene" : "no loading card up: not held", g_holds, ms, rdr_scene::startbodies::TheBoard().Size());
  g_holds = 0;
  return false;
}
}  // namespace wwe2k14::scene_start_wait

REX_EXTERN(__imp__sub_8232E1E8);
REX_HOOK_RAW(sub_8232E1E8) {
  namespace sw = rdr_scene::startwait;
  using wwe2k14::load_probe::SafeRead32;
  const uint32_t self = ctx.r3.u32;
  uint32_t vt = 0, started = 1;
  if (sw::g_armed.load(std::memory_order_acquire) && SafeRead32(self + sw::kPartStartedOff, &started) && SafeRead32(self, &vt) && sw::PartStartPoint(vt, started) &&
      wwe2k14::scene_start_wait::WaitOnNow() && wwe2k14::scene_start_wait::HoldNow("entrance")) {
    ctx.r3.u64 = 0;   // the part stays unstarted ([+124] == 0): the object manager calls this update again next frame; no controller, no fade yet
    return;
  }
  __imp__sub_8232E1E8(ctx, base);
}

REX_EXTERN(__imp__sub_82331BC0);
REX_HOOK_RAW(sub_82331BC0) {
  if (rdr_scene::startwait::g_armed.load(std::memory_order_acquire) && wwe2k14::scene_start_wait::WaitOnNow() && wwe2k14::scene_start_wait::HoldNow("card")) {
    ctx.r3.u64 = 1;   // the game's own wait answer (sub_827E63D8's 1): +24 stays sub_82331BC0, the card stays up, the item runs this stage again next frame
    return;
  }
  __imp__sub_82331BC0(ctx, base);
}

REX_EXTERN(__imp__sub_82331DD8);
REX_HOOK_RAW(sub_82331DD8) {
  rdr_scene::startwait::Arm();   // a card item: sub_8231CAF0 builds it only while the loading card is up; the start behind it waits for our scene
  rdr_scene::startbodies::TheBoard().Reset();   // S110 ENTRBODIES: this start's own body set (its polls ask them again; a resident body is no new job)
  __imp__sub_82331DD8(ctx, base);
}

REX_EXTERN(__imp__sub_8273D0C0);
REX_HOOK_RAW(sub_8273D0C0) {
  namespace sw = rdr_scene::startwait;
  using wwe2k14::load_probe::SafeRead32;
  const uint32_t self = ctx.r3.u32;
  uint32_t delay = 1, step = 0, word = 0;
  if (SafeRead32(self + sw::kLoadDelayOff, &delay) && delay == 0 && SafeRead32(self + sw::kLoadStepOff, &step)) {
    if (sw::LoadFirstStep(delay, step)) {
      sw::Arm();   // a new match load (a rematch in the same arena too): its first start waits for our scene
      rdr_scene::startbodies::TheBoard().Reset();   // S110 ENTRBODIES: this start asks its own bodies (a body an arena change retired is asked again)
    } else {
      if (step >= sw::kLoadStepBodies && sw::g_armed.load(std::memory_order_acquire) && wwe2k14::scene_start_wait::WaitOnNow())   // S110 ENTRBODIES: step 1 handed every slot's character on; ask their bodies now
        wwe2k14::scene_start_wait::AskBodies(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), wwe2k14::scene_start_wait::CardUpNow());
      if (step == sw::kLoadStepStart && SafeRead32(sw::kSceneWord, &word) && sw::MatchStartPoint(delay, step, word) && wwe2k14::scene_start_wait::WaitOnNow() &&
          wwe2k14::scene_start_wait::HoldNow("match")) {
        ctx.r3.u64 = 0;   // the game's own "not yet": sub_825C0718 polls step 3 again next frame, its loading card stays
        return;
      }
    }
  }
  __imp__sub_8273D0C0(ctx, base);
}
