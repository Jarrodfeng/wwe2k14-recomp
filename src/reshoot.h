// wwe2k14 - multi-angle re-shoot (Session 16, item 2): one performance, many cameras. Each pass replays a recorded
// match (src/replay.h) with the CineCam free camera playing one shot path (src/cinecam.h "Paths") and captures that
// shot as a frame-locked clip (src/clip_capture.h: one video frame per guest frame, so a fast-forwarded pass still
// gives real-speed 60 fps footage). The tool tools\replay\reshoot.ps1 runs one pass per shot and writes the sidecars.
//
// --wwe2k14_reshoot=NAME          replay out\replays\NAME.rec (sets --wwe2k14_replay_play=NAME) ...
// --wwe2k14_reshoot_shot=FILE     ... with this shot path (sets --wwe2k14_cinecam=true and
//                                 --wwe2k14_cinecam_path_play=FILE; relative = out\harness\cinecam\paths\), anchored
//                                 with --wwe2k14_cinecam_path_anchor (clock = match start, frame:N = guest frame N,
//                                 step5:N = an entrance), and capture it: --wwe2k14_clip_frame_locked=true, clip dir
//                                 out\reshoots\NAME (each only if left at default). The clip runs from the first frame
//                                 after the anchor to one frame after the shot's last pose (NNN_clip_LABEL.mp4 + .json
//                                 with the first / last guest frame), then the game quits.
// --wwe2k14_reshoot_label=LABEL   clip name (default: the shot file's stem).
// --wwe2k14_reshoot_fps=N         clip playback rate (default 60 = real speed: one guest frame per video frame).
// Logged: "[wwe2k14][reshoot] ..." (start, clip start / stop with frames, quit). A replay that ends before the anchor
// fires logs "[reshoot] FAILED" and quits.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include "cinecam.h"
#include "clip_capture.h"
#include "input_script.h"  // g_request_quit
#include "replay.h"

REXCVAR_DEFINE_STRING(wwe2k14_reshoot, "", "wwe2k14",
                      "Re-shoot (Session 16): replay out/replays/NAME.rec with --wwe2k14_reshoot_shot as the camera and "
                      "capture it as a frame-locked clip into out/reshoots/NAME, then quit. See src/reshoot.h");
REXCVAR_DEFINE_STRING(wwe2k14_reshoot_shot, "", "wwe2k14",
                      "Re-shoot: the CineCam shot path to play (relative = out/harness/cinecam/paths/)");
REXCVAR_DEFINE_STRING(wwe2k14_reshoot_label, "", "wwe2k14", "Re-shoot: clip name (default: the shot file's stem)");
REXCVAR_DEFINE_INT32(wwe2k14_reshoot_fps, 60, "wwe2k14", "Re-shoot: clip playback rate (60 = real speed)");

namespace wwe2k14::reshoot {

struct State {
  bool on = false;
  int stage = 0;  // 0 waiting for the anchor, 1 clip running, 2 waiting for the clip to finish, 3 quit asked
  std::string name, shot, label;
  uint64_t clip_start = 0, clip_stop = 0, done_frame = 0;
};
inline State& S() {
  static State s;
  return s;
}

inline void SetIfDefault(const char* name, const std::string& value) {
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
}

// From OnPreSetup, BEFORE the replay's PreSetup (it reads --wwe2k14_replay_play).
inline void PreSetup() {
  State& s = S();
  s.name = REXCVAR_GET(wwe2k14_reshoot);
  if (s.name.empty()) return;
  s.on = true;
  // Session 17: "director:NAME" re-shoots a director script (out/director/NAME.txt) instead of a recording.
  bool director = false;
  if (s.name.rfind("director:", 0) == 0) s.name = s.name.substr(9), director = true;
  s.shot = REXCVAR_GET(wwe2k14_reshoot_shot);
  s.label = REXCVAR_GET(wwe2k14_reshoot_label);
  if (s.label.empty()) s.label = std::filesystem::path(s.shot).stem().string();
  if (director) SetIfDefault("wwe2k14_director", s.name);
  else SetIfDefault("wwe2k14_replay_play", s.name);
  SetIfDefault("wwe2k14_cinecam", "true");
  if (!s.shot.empty()) SetIfDefault("wwe2k14_cinecam_path_play", s.shot);
  SetIfDefault("wwe2k14_clip_frame_locked", "true");
  SetIfDefault("wwe2k14_clip_dir", (replay::ReplayDir().parent_path() / "reshoots" / s.name).string());
}

inline void OnFrame(uint64_t frame) {
  State& s = S();
  if (!s.on || s.stage >= 3) return;
  const auto& p = cinecam::P();
  switch (s.stage) {
    case 0:
      // Session 35: + 1 more: the picture of guest frame anchor + 2 still shows the game's camera (the first frame
      // of every earlier re-shoot clip did: runs 1397 / 2562), so the clip starts with the shot's first pose.
      if (p.anchor >= 0 && int64_t(frame) > p.anchor + 1) {
        const int fps = std::clamp(int(REXCVAR_GET(wwe2k14_reshoot_fps)), 1, 240);
        if (clip::g_manager.Start(s.label, double(fps), 1.0, frame)) {
          s.clip_start = frame;
          s.stage = 1;
          REXLOG_INFO("[wwe2k14][reshoot] {} / {}: clip {} from frame {} (anchor {} at frame {})", s.name, s.shot,
                      s.label, frame, p.anchor_spec, p.anchor);
        } else {
          REXLOG_ERROR("[wwe2k14][reshoot] FAILED: clip_start {} refused", s.label);
          s.stage = 2;
        }
      } else if (replay::g_replay.EndFrame() && frame > replay::g_replay.EndFrame()) {
        REXLOG_ERROR("[wwe2k14][reshoot] FAILED: the replay ended (frame {}) before the anchor {} fired", frame,
                     p.anchor_spec);
        s.stage = 2;
      }
      break;
    case 1:
      // Session 35: a keyframed shot (NAME.shot) ends when it has played to its length.
      if (cinecam::ShotPathActive() ? cinecam::ShotPathEnded() : (p.play_ended || p.play.empty())) {
        clip::g_manager.Stop(frame, "reshoot: shot ended");
        s.clip_stop = frame;
        s.stage = 2;
        REXLOG_INFO("[wwe2k14][reshoot] {}: shot ended, clip {} stopped at frame {} ({} frames)", s.name, s.label,
                    frame, frame - s.clip_start);
      }
      break;
    case 2:
      if (g_clips_in_flight.load() == 0) {
        s.stage = 3;
        REXLOG_INFO("[wwe2k14][reshoot] {}: clip written; quitting (frame {})", s.name, frame);
        if (g_request_quit) g_request_quit();
      }
      break;
  }
}

// From OnPostSetup (after the replay's Init).
inline void Install() {
  if (!S().on) return;
  REXLOG_INFO("[wwe2k14][reshoot] re-shoot of {} with shot {} (label {}), anchor {}, clips to {}", S().name, S().shot,
              S().label, REXCVAR_GET(wwe2k14_cinecam_path_anchor), REXCVAR_GET(wwe2k14_clip_dir));
  if (S().shot.empty()) REXLOG_ERROR("[wwe2k14][reshoot] FAILED: --wwe2k14_reshoot_shot is empty");
  replay::g_frame_hook = &OnFrame;
}

}  // namespace wwe2k14::reshoot
