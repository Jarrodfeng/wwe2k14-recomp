// S101 lane 111: the shipped live-verb queue and scene serve-site record (src/replay_verbs.h) driven the way src/replay.h and src/scene_director.h
// drive them. Built and run by tools/goto/harvest_selftest.py. No game, no SDK. Output lines (compared exactly):
//   "E <frame> <text>"          the queue drained at a VdSwap (Replay::OnFrame's RecordEvent loop)
//   "host <name> <0|1>"         HostOnly(name)
//   "split ..." / "playing .."  playback side helpers
//   "live served F ARGS"        a scene request served by the owner tick of guest frame F (Tick -> RecordServed)
//   "rec F TEXT"                the E line RecordServed wrote
//   "replay served F ARGS"      the same file played back: handler files in VdSwap(E), Tick takes ONE per tick
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "replay_verbs.h"

namespace V = wwe2k14::replay::verbs;

static std::vector<std::pair<unsigned long long, std::string>> g_written;  // what RecordEvent would write
static bool Sink(unsigned long long f, const std::string& t) {
  g_written.emplace_back(f, t);
  return true;
}

static void Step(unsigned long long frame) {
  for (const std::string& t : V::Drain()) std::cout << "E " << frame << " " << t << "\n";  // = Replay::OnFrame's RecordEvent loop
}

int main() {
  // F6: not recording -> nothing is queued.
  V::Queue("wm30 3");
  V::QueueCvar("wwe2k14_crowd_hype", "80");
  std::cout << "idle drained " << V::Drain().size() << "\n";

  // recording on
  V::g_is_recording = [] { return true; };
  V::g_record_event = &Sink;
  V::QueueCvar("wwe2k14_crowd_hype", "80");
  V::QueueCvar("wwe2k14_match_go", "7");
  V::Queue(V::Wm30Text("  3 X01 "));
  V::QueueCvar("vsync", "true");                      // F3: host-only, skipped
  V::QueueCvar("wwe2k14_runin_now", "1");             // F4: runin records its own event, skipped
  V::QueueCvar("wwe2k14_screenshot_dir", "C:\\x\\y");  // F3: host-only
  V::QueueCvar("wwe2k14_replay_dir", "D:/a");          // F3: host-only
  V::QueueCvar("some_game_path", "C:\\Users\\me\\f");  // a game cvar with a path value -> <path>
  Step(100);
  Step(101);
  V::Queue(V::Wm30Text("off"));
  Step(102);
  for (const char* n : {"vsync", "window_width", "wwe2k14_screenshot_dir", "log_level", "gpu_extents_x", "gpu_native_detached", "gpu_other", "vulkan_readback_fast",
                        "vulkan_x", "wwe2k14_crowd_hype", "wwe2k14_gpu", "wwe2k14_gpu_x", "audio_mute", "wwe2k14_match_go", "present_x"})
    std::cout << "host " << n << " " << V::HostOnly(n) << "\n";
  for (const char* t : {"cvar wwe2k14_crowd_hype 80", "wm30 3 X01", "scene victory 6343", "wm30 off", "log"}) {
    std::string verb, rest;
    const bool ok = V::Split(t, verb, rest);
    std::cout << "split " << ok << " [" << verb << "] [" << rest << "]\n";
  }
  std::cout << "playing " << V::Playing() << "\n";
  V::g_is_playing = [] { return true; };
  std::cout << "playing " << V::Playing() << "\n";

  // ---- F1: LIVE. The owner tick of frame f serves a request that arrived since the previous tick; the engine order per frame is VdSwap(f), then Tick(f).
  g_written.clear();
  struct Req { unsigned long long tick; const char* args; };
  for (const Req& r : {Req{7, "victory 6343"}, Req{9, "menu 5"}}) {
    std::cout << "live served " << r.tick << " " << r.args << "\n";
    V::RecordServed(r.tick, r.args);  // what Tick does after serving
  }
  const auto file = g_written;
  for (auto& w : file) std::cout << "rec " << w.first << " " << w.second << "\n";

  // ---- F1: REPLAY of that file, then F2: two events in ONE frame (20), written by hand.
  std::vector<std::pair<unsigned long long, std::string>> events = file;
  events.emplace_back(20, "scene mid 1,2");
  events.emplace_back(20, "scene victory 9");
  for (unsigned long long frame = 1; frame <= 24; ++frame) {
    for (auto& e : events) {  // VdSwap(frame): the recording's events are handed to the handlers
      if (e.first != frame) continue;
      std::string verb, rest;
      if (V::Split(e.second, verb, rest) && verb == "scene") V::SceneReplayPush(rest);
    }
    std::string args;  // Tick(frame): ONE per tick
    if (V::SceneReplayTake(args)) std::cout << "replay served " << frame << " " << args << "\n";
  }
  return 0;
}
