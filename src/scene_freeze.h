// wwe2k14 - true scene freeze (Session 36), shared by photo mode (src\photo_mode.h), CineCam's "freeze scene"
// (src\cinecam.h) and scripts (`cvar wwe2k14_freeze true|false`). Session 33's freeze ran the game's step at 1000 Hz:
// about 17x slower, but time kept moving (Jarrod: Bryan walked from the top of the ramp to the ring while he keyed a
// shot). The game's own pause cannot be used: it shows a blurred copy of the scene (run 2516), and Start does nothing
// in an entrance (run 035). This stops the scene itself while the renderer, the free camera, depth of field and CineCam
// previews keep working, and picks up from exactly where it stopped. Found live with the probe plugin (coverage-build
// runs 034-037, Lesnar vs Bryan with entrances); four parts:
//
// 1. The game's tasks. The per-frame owner update sub_82C302A0 (CGameMainThread) runs 35 task groups (owner +32: 16
//    bytes each, tasks from +0 to +8; a task runs its vtable slot 1 when its word +8 is set). Before each group it asks
//    the sequence manager's slot 4 whether that group runs; in game that is sub_824F5C38 (manager vtable 0x8201B7A8,
//    r4 = group index, result 0 = skip). While frozen it answers 0 for the groups in kGateMask: the groups the game's
//    own pause stops (scenario coverage: their update functions ran in the in_ring window, not in pause_menu) except
//    group 5 (the scene manager: sub_823C8B20, whose hand-off sub_823C79A8 is where the free camera's view goes to the
//    renderer). Gated: 2, 3, 6, 7, 8, 10, 11, 13 (characters, sub_825A35A0), 17 (referee judge_update sub_825B5D70 and
//    23 more), 18, 19, 20, 21, 22 (crowd reactions sub_823B1880), 23, 24, 25, 26 (skeletons sub_825047B0), 27, 28, 29,
//    31 (AI sub_8241C5B8), 34 (the game's camera). Never gated: group 14 holds sub_82C3B908 (frame sync): gating every
//    group but 5 and 14 still hung the game (runs 034 / 035: no new guest frame), so 0, 1, 4, 12, 14, 33 keep running
//    (the game's pause keeps them too). In those, single tasks are switched off for the call (word +8 cleared, put back
//    after) by their update function (kSkipFns): the entrance sequence runner sub_8231C868 and the entrance / match
//    tasks of groups 0, 1, 14 and 33, e.g. sub_82675600 (counts down a block of bytes every update).
// 2. Time-step-driven things outside the task runner (the crowd's animation, effects: other lockstep threads,
//    docs\systems\frame_pacing.md): the timing block's step fields (0x8392DC38: +0x1C s per update, +0x24 ms, +0x3C
//    60 / rate) say 1e-6 s per update while frozen (5 minutes frozen = 18 ms of their time). With only part 1 the crowd
//    kept moving (run 037 t5b); with this too it stood still (t6). The frame-rate word, the rate and the whole-ms
//    field stay as they were (a rate of 0 would divide by zero), and the game's exact values are put back on release.
// 3. Videos (trons, LED walls, ribbon boards): Bink plays by its own millisecond timer sub_8359FBD0 (reads the game's
//    tick sub_82262158; its 19 callers are all in Bink's library). While frozen it returns the value it had when the
//    freeze began; after it, the time spent frozen is taken off, so videos neither skip nor rush to catch up.
// 4. Sound: the SDK's AudioSystem::Pause / Resume (stops the guest audio callbacks and the XMA decoder, so Wwise, and
//    with it the entrance music, holds its exact position), from a host thread of its own (Pause waits for the audio
//    worker). --wwe2k14_freeze_audio=false leaves the sound running.
// 5. 2D overlays (an entrance's nameplate, run 2577: it sparks in ~330 frames after the entrance starts, shows the name
//    from +402 to ~+594, gone by +630): they run in the groups that keep running, animated by the layout step
//    sub_82BBEC18 (pose at time +144, time += speed +148). While frozen that step does nothing (g_ui_anim_hold,
//    load_turbo.h), so the overlay holds its pose and carries on from it (run 2578: held mid-burst and mid-collapse).
// Parts 3 and 4 only for photo mode and the script setting (Jarrod, 2026-10-19 ~19:10: "for cinecam in entrances I
// actually don't want the audio or the trons to freeze, only for photo mode"): CineCam's "freeze scene" leaves the
// videos and the sound running. Whenever photo mode (or the script) asks too, they are held as well, and let go again
// when only CineCam is left.
//
// Auto freeze at the start of every entrance (Jarrod, 2026-10-19 ~19:15: "start frozen for perfectly repeatable camera
// movements"; --wwe2k14_cinecam_auto_freeze, also in CineCam's panels): when a wrestler's motion becomes the entrance
// act 25000, part 0 ([[[0x836F38D0 + 4 * slot] + 28]] == 0x61A80000; the load turbo's entrance signal), the scene
// freezes from the next game frame. Run 2572: Lesnar's act at frame 3224 (his tron opened at 3206), Bryan's at 6256
// (tron 6238); in both the arena had just come up from the black loading screen (shots at 3228 / 6258). The auto
// freeze holds the videos and sound too, so a release starts the scene, the tron and the music together every time.
// CineCam's "Release + take" starts the open shot's take on the first frame that runs again.
//
// The freeze takes effect at the start of a game frame (the owner update) and only in game (sequence 9). Logged:
// "[wwe2k14][freeze] ..." on and off. --wwe2k14_freeze_true=false brings back Session 33's 1000 Hz step instead.
// Known limits: frame-counted things outside the gated tasks still count (the guest frame, CineCam's take clock);
// the step fields are put back as they were at the freeze, so a step_hz change made while frozen shows after the next
// step change.
#pragma once

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame_stats.h"
#include "guest_read.h"  // GuestWordReadableFast (the auto freeze's slot reads)
#include "step_rate.h"  // LoadU32 / StoreU32, the timing block, g_in_game

REXCVAR_DEFINE_BOOL(wwe2k14_freeze, false, "wwe2k14",
                    "Session 36: freeze the scene now (scripts, the live channel). Photo mode and CineCam's 'freeze "
                    "scene' ask for the same freeze themselves. See src/scene_freeze.h");
REXCVAR_DEFINE_BOOL(wwe2k14_freeze_true, true, "wwe2k14",
                    "Session 36: photo mode / CineCam freeze = a true freeze (nothing moves, carries on from the same "
                    "moment); false = Session 33's step at 1000 Hz (about 17x slower, time keeps moving)");
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_auto_freeze, false, "wwe2k14",
                    "Session 36: freeze the scene at the start of every entrance (the wrestler's first entrance frame; "
                    "videos and sound held too), for repeatable CineCam shots. Also a checkbox in CineCam's panels "
                    "(F8, also outside a match)");
REXCVAR_DEFINE_BOOL(wwe2k14_freeze_audio, true, "wwe2k14",
                    "Session 36: pause the game's sound with a true freeze (and resume it on release)");

REX_EXTERN(__imp__sub_824F5C38);
REX_EXTERN(__imp__sub_8359FBD0);

namespace wwe2k14::freeze {

enum Owner : uint32_t { kScript = 1, kPhoto = 2, kCineCam = 4, kAuto = 8 };
constexpr uint32_t kMediaOwners = kPhoto | kScript | kAuto;  // these hold the videos and the sound too

constexpr uint64_t kGateMask = 0x4BFFE2DCCull;  // groups 2 3 6 7 8 10 11 13 17-29 31 34 (see the header)
constexpr uint32_t kSkipFns[] = {
    0x8231C868,  // group 0: entrance sequence runner (scene word writer, match_state.md)
    0x8286B790,  // group 0 (in match only)
    0x8334CD10,  // group 0 (in match only)
    0x826FCDF0,  // group 1 (in match only)
    0x82675600,  // group 1: counts down a block of signed bytes every update
    0x8232E1E8,  // group 1 (entrances / story only)
    0x8260BF38,  // group 14 (in match only)
    0x8231F078,  // group 14 (entrances / story only)
    0x8232F2B0,  // group 33 (entrances / story only)
};
constexpr float kFrozenStep = 1e-6f;  // seconds per update while frozen

inline std::mutex g_mutex;           // requests and the slow fallback
inline uint32_t g_requests = 0;      // Owner bits
inline std::atomic<uint32_t> g_requests_now{0};  // a copy for lock-free readers
inline bool g_slow_on = false;       // the 1000 Hz fallback is applied
inline std::string g_slow_hz, g_slow_vblank;
inline std::atomic<bool> g_wanted{false};  // a true freeze is asked for (read by the owner update)
inline std::atomic<bool> g_frozen{false};  // applied (set / cleared at the start of the owner update)
inline std::atomic<bool> g_media_wanted{false};  // photo mode or the script asks: videos and sound held too
inline std::atomic<bool> g_media{false};         // applied: Bink's timer held, sound paused
inline uint64_t g_frozen_at = 0;
inline uint32_t g_saved_1c = 0, g_saved_24 = 0, g_saved_3c = 0;
inline std::vector<std::pair<uint32_t, uint32_t>> g_task_restore;  // task, its word +8 (update thread only)

inline bool Active() { return g_frozen.load(std::memory_order_relaxed); }

// Called with g_mutex held whenever the requests change.
inline void ApplyLocked() {
  const bool any = g_requests != 0;
  const bool slow = any && !REXCVAR_GET(wwe2k14_freeze_true);
  if (slow != g_slow_on) {
    g_slow_on = slow;
    if (slow) {
      // Session 33's freeze: the step at 1000 Hz (step_rate.h); high refresh off (the vblank would follow 1000 Hz).
      g_slow_hz = rex::cvar::GetFlagByName("wwe2k14_step_hz");
      g_slow_vblank = rex::cvar::GetFlagByName("wwe2k14_step_vblank");
      rex::cvar::SetFlagByName("wwe2k14_step_vblank", "false");
      rex::cvar::SetFlagByName("wwe2k14_step_hz", "1000");
    } else {
      rex::cvar::SetFlagByName("wwe2k14_step_hz", g_slow_hz.empty() ? "0" : g_slow_hz);
      rex::cvar::SetFlagByName("wwe2k14_step_vblank", g_slow_vblank.empty() ? "false" : g_slow_vblank);
    }
    REXLOG_INFO("[wwe2k14][freeze] slow freeze (step 1000 Hz) {}", slow ? "on" : "off");
  }
  g_wanted.store(any && !slow, std::memory_order_relaxed);
  g_media_wanted.store((g_requests & kMediaOwners) != 0, std::memory_order_relaxed);
}

// Photo mode / CineCam / the script setting ask for the freeze (or let it go); frozen while anyone asks.
inline void Request(Owner who, bool on) {
  std::lock_guard lock(g_mutex);
  const uint32_t before = g_requests;
  g_requests = on ? (g_requests | who) : (g_requests & ~uint32_t(who));
  if (g_requests == before) return;
  g_requests_now.store(g_requests, std::memory_order_relaxed);
  REXLOG_INFO("[wwe2k14][freeze] {} {} the freeze (requests {:X} -> {:X})",
              who == kPhoto ? "photo mode" : who == kCineCam ? "CineCam" : who == kAuto ? "the auto freeze" : "script",
              on ? "asks for" : "lets go of",
              before, g_requests);
  ApplyLocked();
}

// ---- 4. sound (a host thread: AudioSystem::Pause waits for the audio worker to stop) ----
inline std::mutex g_audio_mutex;
inline std::condition_variable g_audio_cv;
inline bool g_audio_want = false, g_audio_started = false;

inline void AudioWorker() {
  bool paused = false;
  for (;;) {
    bool want;
    {
      std::unique_lock lock(g_audio_mutex);
      g_audio_cv.wait(lock, [&] { return g_audio_want != paused; });
      want = g_audio_want;
    }
    auto* audio = static_cast<rex::audio::AudioSystem*>(rex::Runtime::instance()->audio_system());
    if (audio) want ? audio->Pause() : audio->Resume();
    paused = want;
    REXLOG_INFO("[wwe2k14][freeze] sound {}{}", want ? "paused" : "resumed", audio ? "" : " (no audio system)");
  }
}

inline void SetAudio(bool pause) {
  std::lock_guard lock(g_audio_mutex);
  if (!g_audio_started) {
    if (!pause) return;
    g_audio_started = true;
    std::thread(AudioWorker).detach();
  }
  g_audio_want = pause;
  g_audio_cv.notify_all();
}

// ---- the auto freeze (update thread) ----
constexpr uint32_t kSlotTable = 0x836F38D0;  // 9 character pointers (match_state.md)
constexpr uint32_t kEntranceAct0 = 0x61A80000;  // act 25000 (entrance), part 0
inline uint32_t g_slot_act[9] = {};
inline std::atomic<int> g_auto_slot{-1};         // the slot whose entrance start froze the scene last
inline std::atomic<uint64_t> g_auto_frame{0};

inline bool AutoFrozen() { return (g_requests_now.load(std::memory_order_relaxed) & kAuto) != 0; }
inline void ReleaseAuto() { Request(kAuto, false); }

inline void AutoTick(uint8_t* base, bool in_game, uint64_t frame) {
  if (!in_game || !REXCVAR_GET(wwe2k14_cinecam_auto_freeze)) {
    std::memset(g_slot_act, 0, sizeof(g_slot_act));
    return;
  }
  auto rd = [base](uint32_t a, uint32_t* out) {
    if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
    *out = step_rate::LoadU32(base, a);
    return true;
  };
  for (int i = 0; i < 9; ++i) {
    uint32_t ch = 0, motion = 0, act = 0;
    if (!rd(kSlotTable + 4 * i, &ch) || !rd(ch + 28, &motion) || !rd(motion, &act)) act = 0;
    if (act == kEntranceAct0 && g_slot_act[i] != kEntranceAct0) {
      g_auto_slot.store(i), g_auto_frame.store(frame);
      REXLOG_INFO("[wwe2k14][freeze] auto freeze: slot {} entrance starts at frame {}", i, frame);
      Request(kAuto, true);
    }
    g_slot_act[i] = act;
  }
}

// ---- 1 + 2: the owner update (called from scenario.h's sub_82C302A0 override, update thread) ----
inline void RunnerEnter(PPCContext& ctx, uint8_t* base) {
  using step_rate::LoadU32;
  using step_rate::StoreF32;
  using step_rate::StoreU32;
  constexpr uint32_t kBlock = step_rate::kBlock;
  // The script setting is one more requester.
  static bool script = false;
  if (const bool s = REXCVAR_GET(wwe2k14_freeze); s != script) {
    script = s;
    Request(kScript, s);
  }
  const bool in_game = step_rate::g_in_game.load(std::memory_order_relaxed);
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  AutoTick(base, in_game, frame);
  if (!in_game && AutoFrozen()) ReleaseAuto();  // the match is over: nothing to hold
  const bool want = g_wanted.load(std::memory_order_relaxed) && in_game;
  if (want != Active()) {
    if (want) {
      g_saved_1c = LoadU32(base, kBlock + 0x1C), g_saved_24 = LoadU32(base, kBlock + 0x24);
      g_saved_3c = LoadU32(base, kBlock + 0x3C);
      g_frozen_at = frame;
    } else {
      StoreU32(base, kBlock + 0x1C, g_saved_1c);
      StoreU32(base, kBlock + 0x24, g_saved_24);
      StoreU32(base, kBlock + 0x3C, g_saved_3c);
    }
    g_frozen.store(want, std::memory_order_relaxed);
    g_ui_anim_hold().store(want, std::memory_order_relaxed);  // 2D overlays (the nameplate) hold too
    REXLOG_INFO("[wwe2k14][freeze] scene {} at frame {}{}", want ? "frozen" : "running again", frame,
                want ? "" : fmt::format(" ({} frames frozen)", frame - g_frozen_at));
  }
  // Parts 3 and 4: videos and sound, only while photo mode or the script asks.
  if (const bool media = want && g_media_wanted.load(std::memory_order_relaxed); media != g_media.load()) {
    g_media.store(media, std::memory_order_relaxed);
    if (REXCVAR_GET(wwe2k14_freeze_audio) || !media) SetAudio(media);
    REXLOG_INFO("[wwe2k14][freeze] videos and sound {} at frame {}", media ? "held" : "running", frame);
  }
  if (!want) return;
  StoreF32(base, kBlock + 0x1C, kFrozenStep);
  StoreF32(base, kBlock + 0x24, kFrozenStep * 1000.0f);
  StoreF32(base, kBlock + 0x3C, kFrozenStep * 60.0f);
  // Single tasks of the groups that keep running.
  g_task_restore.clear();
  const uint32_t owner = ctx.r3.u32;
  const uint32_t g0 = LoadU32(base, owner + 32), g1 = LoadU32(base, owner + 40);
  if (g1 < g0 || g1 - g0 > 64 * 16) return;
  for (uint32_t g = g0; g < g1; g += 16) {
    const uint32_t b = LoadU32(base, g), e = LoadU32(base, g + 8);
    if (e < b || e - b > 4096) continue;
    for (uint32_t p = b; p < e; p += 4) {
      const uint32_t t = LoadU32(base, p);
      if (t < 0x40000000u || t >= 0xE0000000u) continue;  // -1 = empty slot
      const uint32_t vt = LoadU32(base, t);
      if (vt < 0x82000000u || vt >= 0x84000000u) continue;
      const uint32_t fn = LoadU32(base, vt + 4);
      bool skip = false;
      for (uint32_t s : kSkipFns) skip |= fn == s;
      if (!skip) continue;
      const uint32_t on = LoadU32(base, t + 8);
      if (!on) continue;
      g_task_restore.push_back({t, on});
      StoreU32(base, t + 8, 0);
    }
  }
}

inline void RunnerLeave(uint8_t* base) {
  for (auto& [t, on] : g_task_restore) step_rate::StoreU32(base, t + 8, on);
  g_task_restore.clear();
}

// ---- 3. Bink's timer ----
inline std::mutex g_bink_mutex;
inline bool g_bink_held = false;
inline uint32_t g_bink_hold = 0, g_bink_offset = 0;

}  // namespace wwe2k14::freeze

// The in-game sequence manager's "does group r4 run" (vtable 0x8201B7A8 slot 4).
REX_HOOK_RAW(sub_824F5C38) {
  const uint32_t group = ctx.r4.u32;
  __imp__sub_824F5C38(ctx, base);
  if (wwe2k14::freeze::Active() && group < 64 && ((wwe2k14::freeze::kGateMask >> group) & 1)) ctx.r3.u64 = 0;
}

// Bink's millisecond timer: held while the videos are held (photo mode / script freeze), that time taken off after.
REX_HOOK_RAW(sub_8359FBD0) {
  using namespace wwe2k14::freeze;
  __imp__sub_8359FBD0(ctx, base);
  const uint32_t real = ctx.r3.u32;
  std::lock_guard lock(g_bink_mutex);
  if (g_media.load(std::memory_order_relaxed)) {
    if (!g_bink_held) g_bink_held = true, g_bink_hold = real - g_bink_offset;
    ctx.r3.u64 = g_bink_hold;
    return;
  }
  if (g_bink_held) {
    g_bink_held = false;
    g_bink_offset = real - g_bink_hold;
  }
  ctx.r3.u64 = uint32_t(real - g_bink_offset);
}
