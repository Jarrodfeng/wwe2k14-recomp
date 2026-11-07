// wwe2k14 - Session 95: the diagnostic run's plan, shared by the PC launcher (src/launcher) and the phone launcher
// (src/launcher_mobile). Pure standard C++ (no game or SDK headers): the route text, the arms, and the flag edits.
//
// One button, hands-free, about 3 minutes, one report zip. Arm 0 plays the player's own settings through a fixed route
// (boot, menu, a Quick Match with entrances, the match, the pause menu) and freezes at each scene for in-run A/Bs
// (same frame exactly). Relaunch arms (settings latched at start) run the entrance scene only, with one setting
// flipped. The game side is src/diag.h (--wwe2k14_diag=<run folder>); the zip is read by tools/native/diag_read.py.
//
// Corrections to the first brief (design notes, Session 95): the match is a Quick Match (--wwe2k14_launch=@quick with
// participants and arena, as tools/xdet/scenario_check.flags), never a scenario capture; the main arm is "as played"
// (a replay serialises the GPU thread and can hide the upload races we hunt), the replay is one late arm.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace wwe2k14::diag_plan {

// ---------------------------------------------------------------- flag editing (a repeated flag drops later ones:
// an arm REPLACES a flag, it never appends a second copy)
inline std::string FlagNameOf(const std::string& arg) {
  if (arg.rfind("--", 0) != 0) return {};
  const size_t eq = arg.find('=');
  return arg.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
}
inline std::string FlagValueOf(const std::vector<std::string>& args, const std::string& name, bool* found = nullptr) {
  for (const auto& a : args) {
    if (FlagNameOf(a) != name) continue;
    if (found) *found = true;
    const size_t eq = a.find('=');
    return eq == std::string::npos ? std::string() : a.substr(eq + 1);
  }
  if (found) *found = false;
  return {};
}
inline void RemoveFlag(std::vector<std::string>& args, const std::string& name) {
  args.erase(std::remove_if(args.begin(), args.end(), [&](const std::string& a) { return FlagNameOf(a) == name; }),
             args.end());
}
inline void SetFlag(std::vector<std::string>& args, const std::string& name, const std::string& value) {
  RemoveFlag(args, name);
  args.push_back("--" + name + "=" + value);
}
// "a --b=c "d e"" -> tokens (double quotes group; quotes removed inside a --name="value" too).
inline std::vector<std::string> SplitArgs(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, any = false;
  for (char c : line) {
    if (c == '"') quoted = !quoted, any = true;
    else if ((c == ' ' || c == '\t') && !quoted) {
      if (any || !cur.empty()) out.push_back(cur);
      cur.clear(), any = false;
    } else cur += c;
  }
  if (any || !cur.empty()) out.push_back(cur);
  return out;
}

// ---------------------------------------------------------------- arms
struct Arm {
  std::string id;      // "0", "split", "vtx", "sysdrv", "render", "vinput", "dense", "sparse", "mediump"
  std::string title;   // for the progress line and the report
  bool full = false;   // the whole route (arm 0); else the entrance scene only
  int est_s = 40;      // estimated seconds (the governor starts no arm that would end past the budget)
};

// The arms that apply to these base flags (the player's own), most useful first. Reasons for the ones left out go in
// `skipped` ("id: why"), so the report names them.
// `phone`: a flag that is absent means the SDK's default `auto`, which turns the storage windows on when the GPU's
// maxStorageBufferRange is under 512 MiB (most phones): the split arm is planned unless the settings say off.
inline std::vector<Arm> PlanArms(const std::vector<std::string>& base, std::vector<std::string>* skipped, bool phone = false) {
  std::vector<Arm> arms;
  arms.push_back({"0", "as played", true, 110});
  auto skip = [&](const std::string& why) { if (skipped) skipped->push_back(why); };
  bool f = false;
  const std::string window = FlagValueOf(base, "vulkan_shared_memory_window", &f);
  if ((f && (window == "on" || window == "auto" || window == "true")) || (phone && (!f || window.empty())))
    arms.push_back({"split", "storage: old split instead of windows", false, 40});
  else skip("split: the storage windows were not set on by the settings");
  // The vertex truth at the entrance, in an arm of its own: its debug pipelines come last in an arm (a new pipeline can crash a fragile driver).
  arms.push_back({"vtx", "the vertex truth at the entrance", false, 50});
  if (FlagValueOf(base, "wwe2k14_android_driver", &f), f) {
    arms.push_back({"sysdrv", "the phone's own driver", false, 40});
  } else skip("sysdrv: no custom driver is set (so no dense: the sparse-binding arm is for the custom driver)");
  const std::string owned = FlagValueOf(base, "wwe2k14_d3d_owned", &f);
  if (f) arms.push_back({"render", owned == "false" ? "TRUE NATIVE on" : "NATIVE instead of TRUE NATIVE", false, 40});
  else skip("render: the renderer setting is the game's default (not flipped without a known value)");
  // Session 95 (Lane D, out\s95\android_structural.md): the vertex-input arm. It flips gpu_native_vertex_input to 0 (the storage-buffer
  // vertex read), so it is planned only when the settings carry that flag with a value other than 0, or when TRUE NATIVE is on and
  // no value is given: there it is on by default on a PC and a phone (src\d3d_owned.h), which does not appear in the base flags.
  // est_s is higher than 40: the arm's first frames pay a cold prewarm.
  {
    const std::string vi = FlagValueOf(base, "gpu_native_vertex_input", &f);
    bool vi_on = f && vi != "0";
    if (!f) {
      bool owned_given = false;
      vi_on = FlagValueOf(base, "wwe2k14_d3d_owned", &owned_given) == "true";
    }
    if (vi_on) arms.push_back({"vinput", "vertex input off (the storage-buffer vertex read)", false, 70});
    else skip("vinput: gpu_native_vertex_input is not on in the settings");
  }
  // Session 95 (diag4): with a custom driver (any imported one, Turnip being the case in mind) the shared memory may be sparse-bound or
  // one plain buffer. Since Lane D the Turnip quirk rule (turnip_no_sparse) makes arm 0 a plain buffer, so "dense" (sparse off) would
  // repeat arm 0 there: the "sparse" arm sets vulkan_sparse_shared_memory=true (an explicit value beats the rule) and "dense" sets false.
  // Which one is the real change is read from arm 0's [sparse] shared memory line in the report. Both need a restart; no frozen in-run A/B.
  // Planned AFTER render so a short or ordinary run keeps render; they run only when the time budget has room (a LONG RUN), review diag4 F5.
  if (FlagValueOf(base, "wwe2k14_android_driver", &f), f) {
    const std::string sp = FlagValueOf(base, "vulkan_sparse_shared_memory", &f);
    if (!f || sp != "false")
      arms.push_back({"dense", "shared memory as one plain buffer (sparse binding off)", false, 40});
    else skip("dense: sparse binding is already off in the settings");
    if (!f || sp != "true")
      arms.push_back({"sparse", "shared memory sparse-bound (sparse binding forced on)", false, 40});
    else skip("sparse: sparse binding is already forced on in the settings");
  }
  if (FlagValueOf(base, "wwe2k14_shader_mediump", &f) == "true")
    arms.push_back({"mediump", "half-precision colour maths off", false, 40});
  else skip("mediump: not on in the settings");
  return arms;
}

// The flags for one arm: the player's own with the arm's one change.
inline std::vector<std::string> ArmArgs(const Arm& arm, std::vector<std::string> args) {
  if (arm.id == "split") {
    SetFlag(args, "vulkan_shared_memory_window", "off");
    // only the storage limit goes (review F4): a faked phone on a PC keeps its other fake limits
    bool f = false;
    const std::string lim = FlagValueOf(args, "vulkan_emulate_limits", &f);
    if (f) {
      std::string kept;
      size_t at = 0;
      while (at <= lim.size()) {
        size_t comma = lim.find(',', at);
        if (comma == std::string::npos) comma = lim.size();
        const std::string item = lim.substr(at, comma - at);
        if (!item.empty() && item.rfind("maxStorageBufferRange", 0) != 0) kept += (kept.empty() ? "" : ",") + item;
        at = comma + 1;
      }
      if (kept.empty()) RemoveFlag(args, "vulkan_emulate_limits");
      else SetFlag(args, "vulkan_emulate_limits", kept);
    }
  } else if (arm.id == "sysdrv") {
    RemoveFlag(args, "wwe2k14_android_driver");
    RemoveFlag(args, "wwe2k14_android_driver_dir");
    RemoveFlag(args, "wwe2k14_android_driver_trial");
  } else if (arm.id == "dense") {
    SetFlag(args, "vulkan_sparse_shared_memory", "false");
  } else if (arm.id == "sparse") {
    SetFlag(args, "vulkan_sparse_shared_memory", "true");
  } else if (arm.id == "render") {
    bool f = false;
    const std::string v = FlagValueOf(args, "wwe2k14_d3d_owned", &f);
    SetFlag(args, "wwe2k14_d3d_owned", v == "false" ? "true" : "false");
  } else if (arm.id == "vinput") {
    SetFlag(args, "gpu_native_vertex_input", "0");
    // (Session 95 review: with the input level at 0 skin once stays on and its listed wrestler draws still read attribute rows, so the arm would not isolate vertex input.)
    SetFlag(args, "gpu_native_skin_once", "false");
  } else if (arm.id == "mediump") {
    SetFlag(args, "wwe2k14_shader_mediump", "false");
  } else if (arm.id == "0") {
    // Session 95 (P0, out\s95\android_structural.md): the phones' own numbers for the vertex-input design, log only. gpu_native_vertex_census is a frame
    // count (int; it counts down from each frame end): 20000 outlasts the whole route, so every shader's vfetch line prints; no end summary is wanted here.
    SetFlag(args, "gpu_native_vinput_census", "true");
    SetFlag(args, "gpu_native_vertex_census", "20000");
  }
  return args;
}

// The flags every arm gets on top (the run folder, the route, the Quick Match, the reports). `run` and `route` are
// paths as the game sees them; `arm_s` the arm's deadline. Paths with spaces are quoted by the caller.
inline std::vector<std::string> DiagArgs(const std::string& run, const std::string& arm_id, const std::string& route,
                                         int arm_s, const std::string& device_json, bool hold = false) {
  return {"--wwe2k14_diag=" + run,
          "--gpu_diag_layer=" + run + "/arm" + arm_id,  // the per-pass sheets and the vertex truth (SDK: native/diag_layer.h)
          "--wwe2k14_diag_arm=" + arm_id,
          "--wwe2k14_diag_arm_s=" + std::to_string(arm_s),
          "--wwe2k14_input_script=" + route,
          "--vulkan_device_report_path=" + device_json,
          "--vulkan_gpu_timestamps=true",
          // (the timeline CSV is started by the game itself for 300 frames at the entrance and the match: src/diag.h)
          "--user_language=1",              // the route waits on English text (review F3); the player's language is not used
          "--wwe2k14_mouse_menus=false",    // as the determinism check's pins
          "--wwe2k14_fixed_seed=12345",     // the same random numbers on every arm (review F7)
          std::string("--wwe2k14_diag_hold=") + (hold ? "true" : "false"),
          "--wwe2k14_launch=@quick",
          "--wwe2k14_quick_type=0",
          "--wwe2k14_launch_participants=324:0,343",  // Bryan (controller 1) vs 343, stock wrestlers
          "--wwe2k14_launch_arena=3",
          "--wwe2k14_launch_idle_frames=240",
          "--wwe2k14_launch_skip_entrances=false"};
}

// ---------------------------------------------------------------- did the route finish (both launchers, the arm's stages.log)
// stages.log is one JSON object per line ({"stage":"boot",...}); the route ends on "done". The last stage tells where a failed
// route stopped (run 6629: every arm ended in 3 s with exit 0x0 at "boot" and a tidy zip that looked fine).
inline std::string LastStage(const std::string& stages_log) {
  std::string last;
  size_t at = 0;
  while ((at = stages_log.find("\"stage\":\"", at)) != std::string::npos) {
    at += 9;
    const size_t end = stages_log.find('"', at);
    if (end == std::string::npos) break;
    last = stages_log.substr(at, end - at);
    at = end;
  }
  return last;
}
// "" = the route reached done; else a plain sentence for arm_summary.txt and the result screen.
inline std::string RouteFailure(const std::string& stages_log, long seconds) {
  const std::string st = LastStage(stages_log);
  if (st == "done") return {};
  return st.empty() ? "ROUTE FAILED: the game never reported a stage (after " + std::to_string(seconds) + " s)"
                    : "ROUTE FAILED at stage '" + st + "' (after " + std::to_string(seconds) + " s)";
}

// ---------------------------------------------------------------- the route (src/input_script.h; diag_* steps are
// src/diag.h). The title takes ~2 s of real time to take START and runs ~1000 fps (run 6629): retries outlast it. Inline chains only: phones have no states.toml. The boot part is tools/xdet/check_route.txt's (a fresh
// save: the save-data prompt answers NO, the "You have not selected to save" prompt LEFT then A = YES (start without saving), see
// tools/xdet/check_route.txt; nothing is written to the player's save; the run has a save folder of its own. The LEFT hold is counted in frames
// (30 = ~33 ms at the PC's ~900 fps in the title, 0.5-1 s on a phone): not checked on a phone, a miss stops the route at stage 'boot').
inline std::string FrozenArms(const std::string& scene) {
  std::string r;
  r += "diag_scene " + scene + "_aa\n";  // A/A control: nothing changed, the picture must be the same
  auto ab = [&](const char* cvar, const char* value, const char* tag) {
    r += std::string("diag_ab ") + cvar + " " + value + "\nwait_frames 10\ndiag_scene " + scene + "_" + tag +
         "\ndiag_restore\n";
  };
  ab("gpu_native_shader_ablate", "all", "ablate");  // every replaced pixel shader flat grey: geometry vs pixel shader
  ab("gpu_native_vs_swap", "false", "vsoff");       // our vertex shaders -> the translated ones
  ab("gpu_native_shader_swap", "false", "psoff");   // our pixel shaders -> the translated ones
  return r;
}

// Which of the arm's routes (the files the launchers write into the run folder).
inline const char* RouteFile(const Arm& arm) { return arm.full ? "route_full.txt" : arm.id == "vtx" ? "route_vertex.txt" : "route_entrance.txt"; }

// `hold` (the phone): the route never quits the game; it waits at "done" for the launcher's activity to end it (a process
// exit would beat the activity's poll and strand the arm chain: review F2).
// `vertex`: this arm's frozen entrance also runs the vertex truth's debug pipelines (diag_vertex). The match's run last in arm 0.
inline std::string Route(bool full, bool hold = false, bool vertex = false) {
  std::string r =
      "# Session 95 diagnostic run (src/diag_plan.h)\n"
      + std::string(hold ? "on_fail continue\n" : "on_fail quit\n") +
      "log diag: route start (fresh save)\n"
      "diag_stage boot\n"
      "wait_state 0x839400F8>0>48 3 36000\n"
      "wait_text Press START 36000\n"
      "press_until_text START 100 200 30 ATTENTION\n"
      "press_until_text A 100 200 30 There is no\n"
      "press_until_text A 100 200 30 You have not selected\n"
      "wait_frames 30\n"
      "press LEFT 30 60\n"
      "press_until_text A 100 200 30 YEARS OF WRESTLEMANIA\n"
      "diag_stage menu\n";
  if (full) {
    r += "wait_frames 90\n"
         "diag_scene menu\n";
  }
  r += "diag_stage load_entrance\n"
       "wait_file 324.bik2 36000\n"
       "diag_stage entrance\n"
       "wait_frames 200\n"
       "diag_scene entrance_live\n"  // live timing (the last 300 frames) + pictures
       "cvar wwe2k14_freeze true\n"
       "wait_frames 60\n"
       "diag_scene entrance\n";
  if (full) r += FrozenArms("entrance");
  // diag_truth / diag_vertex: the frame again, the draws its pictures' frame picked (src/diag.h)
  r += "wait_frames 10\n"
       "diag_truth entrance\n";
  if (vertex) r += "diag_vertex entrance\n";
  if (full) {
    r += "cvar wwe2k14_freeze false\n"
         "diag_stage load_match\n"
         "press_until_file START M3.PAC 60 240 30\n"
         "diag_stage match\n"
         "wait_frames 300\n"
         "diag_scene match_live\n"
         "cvar wwe2k14_freeze true\n"
         "wait_frames 60\n"
         "diag_scene match\n";
    r += FrozenArms("match");
    r += "wait_frames 10\n"
         "diag_truth match\n"
         "diag_vertex match\n"
         "cvar wwe2k14_freeze false\n"
         "diag_stage pause\n"
         "press START 5 30\n"
         "wait_frames 60\n"
         "diag_scene pause\n";
  }
  r += "diag_stage done\n";
  r += hold ? "wait_frames 1000000\n" : "quit\n";
  return r;
}

}  // namespace wwe2k14::diag_plan
