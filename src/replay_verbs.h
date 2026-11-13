// wwe2k14 - live verbs as recorded E events (S101 lane 111, go-anywhere L-3 small part; docs/research/s101/GOANY_MOMENTS.md 2.4).
//
// A goto session started in replay mode is replayable only if every live verb that writes guest state is in the recording. The
// `cvar` steps of the live channel's batches (src/input_script.h ApplyCvar) and `!wm30` (src/live_control.h) push their text here; the
// replay's per-frame step (src/replay.h Replay::OnFrame) drains the queue and writes each as `E F <text>` at the frame it takes effect.
// `!scene` is recorded where the owner tick SERVES it (RecordServed below), not at the drain. On playback the same text comes back as an
// event and the owners re-apply it in the same VdSwap:
//   E F cvar NAME VALUE     src/director.h OnReplayEvent (already there; skips a "<path>" value)
//   E F wm30 N [CODE]|off   src/wm30_jump.h OnReplayEvent
//   E F scene ARGS          src/scene_director.h OnReplayEvent (FIFO; the owner tick of frame F serves it)
// While a recording plays the live channel refuses the state-writing verbs (the recording has them); the `off` forms stay allowed.
// Nothing is queued unless a recording is being written. No SDK: standard library only.
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::replay::verbs {

// Set by src/replay.h: true while a recording plays / is being written (null = never, e.g. in the unit test).
inline bool (*g_is_playing)() = nullptr;
inline bool (*g_is_recording)() = nullptr;
inline bool Playing() { return g_is_playing && g_is_playing(); }
inline bool Recording() { return g_is_recording && g_is_recording(); }
// Set by src/replay.h: writes `E frame text` (Replay::RecordEvent). Null in the unit test until it sets its own.
inline bool (*g_record_event)(unsigned long long frame, const std::string& text) = nullptr;

inline std::mutex g_mu;
inline std::vector<std::string> g_queue;

inline std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Event text of each verb (the exact text after "E F ").
inline std::string CvarText(const std::string& name, const std::string& value) { return "cvar " + name + " " + value; }
inline std::string Wm30Text(const std::string& args) { return "wm30 " + Trim(args); }
inline std::string SceneText(const std::string& args) { return "scene " + Trim(args); }

// Settings of the host, not of the game (paths, windows, logs, screenshots...): a recorded `cvar` of one must not be re-applied on playback
// over the player's explicit flags (a recorded `vsync true` would end a --vsync=false fast-forward). Kept in step with HOST_ONLY in
// tools/determinism/det_cross.py (harvest_selftest.py compares the two on a name list).
inline bool HostOnly(const std::string& n) {
  static const char* kPrefixes[] = {"user_data_root", "cache_root", "game_data_root", "log_", "wwe2k14_det_hash_dir", "wwe2k14_replay_dir",
                                    "wwe2k14_det_hash_raw_", "det_serial_trace", "wwe2k14_input_script", "wwe2k14_timeline",
                                    "wwe2k14_settings_file", "gpu_anatomy_path", "window_", "fullscreen", "vsync", "wwe2k14_check_",
                                    "wwe2k14_live", "present_", "wwe2k14_screenshot", "wwe2k14_clip", "wwe2k14_bookmark",
                                    "det_serial_vblank_timeout_ms", "det_serial_stall_ms", "det_frames_timeout_ms", "det_frames_settle_us"};
  static const char* kExact[] = {"wwe2k14_gpu", "wwe2k14_replay_record", "wwe2k14_replay_play", "wwe2k14_det_poke", "wwe2k14_replay_guard",
                                 "wwe2k14_android_lib_dir", "wwe2k14_android_tmp_dir", "wwe2k14_android_driver_dir", "wwe2k14_scenario_dir",
                                 "wwe2k14_native_frame", "audio_mute", "wwe2k14_background_full_speed"};
  for (const char* p : kPrefixes)
    if (n.rfind(p, 0) == 0) return true;
  for (const char* p : kExact)
    if (n == p) return true;
  if (n.rfind("vulkan_", 0) == 0 && n.rfind("vulkan_readback_", 0) != 0) return true;
  if (n.rfind("gpu_", 0) == 0 && n.rfind("gpu_native_detached", 0) != 0 && n.rfind("gpu_extents_", 0) != 0 &&
      n.rfind("gpu_det_skip_host_draws", 0) != 0)
    return true;
  return false;
}

inline bool LooksLikePath(const std::string& v) {
  return v.find(":\\") != std::string::npos || v.find(":/") != std::string::npos || (!v.empty() && (v[0] == '/' || v[0] == '\\'));
}

// Any thread. Queued text is written by the next Replay::OnFrame (the guest thread, one per VdSwap). Only while a recording is being written
// (outside replay mode nothing drains the queue).
inline void Queue(const std::string& text) {
  if (!Recording()) return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_queue.push_back(text);
}

// A `cvar` step: not host-only settings, not wwe2k14_runin_now (src/runin.h records its own run-in event; queuing the trigger too fires it twice
// at the hand-over frame), a path-like value as <path> (a shipped recording names no host folders; the replay handler skips "<path>").
inline void QueueCvar(const std::string& name, const std::string& value) {
  if (HostOnly(name) || name == "wwe2k14_runin_now") return;
  Queue(CvarText(name, LooksLikePath(value) ? std::string("<path>") : value));
}

// Guest thread: everything queued since the last frame, in order.
inline std::vector<std::string> Drain() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<std::string> out;
  out.swap(g_queue);
  return out;
}

// ---- !scene: recorded at the serve site (src/scene_director.h Tick), replayed through a FIFO ----
// The owner tick that serves a request runs after VdSwap(N) (N = the guest frame counter then); recording `E N` there and filing the request in
// VdSwap(N) on playback makes the same tick N serve it: offset 0. (A drain-time record would be N or N+1, racing the pipe thread.)
inline std::mutex g_scene_mu;
inline std::deque<std::string> g_scene_replay;

// Tick (guest thread), when it served an accepted live-channel request: `E tick_frame scene ARGS`.
inline void RecordServed(unsigned long long tick_frame, const std::string& scene_args) {
  if (g_record_event && Recording()) g_record_event(tick_frame, SceneText(scene_args));
}
// Playback handler (guest thread, in VdSwap): file one request; Tick takes ONE per tick (two in a frame are served on consecutive ticks).
inline void SceneReplayPush(const std::string& args) {
  std::lock_guard<std::mutex> lock(g_scene_mu);
  g_scene_replay.push_back(args);
}
inline bool SceneReplayTake(std::string& args) {
  std::lock_guard<std::mutex> lock(g_scene_mu);
  if (g_scene_replay.empty()) return false;
  args = g_scene_replay.front();
  g_scene_replay.pop_front();
  return true;
}

// `verb args` -> true with the verb and the rest (playback side).
inline bool Split(const std::string& text, std::string& verb, std::string& rest) {
  const size_t sp = text.find(' ');
  verb = text.substr(0, sp);
  rest = sp == std::string::npos ? std::string() : Trim(text.substr(sp + 1));
  return !verb.empty();
}

}  // namespace wwe2k14::replay::verbs
