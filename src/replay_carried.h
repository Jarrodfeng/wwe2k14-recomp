// wwe2k14 - which settings a replay recording carries, and the header's `flag` line (S101 lane 111, go-anywhere L-2).
// Moved out of src/replay.h unchanged (CarriedSetting) so a unit test can compile the shipped rule without the SDK
// (tools/goto/replay_carried_test.cpp); replay.h includes this and writes / reads its header with HeaderLine / ParseFlagLine.
// No SDK, no game: standard library only.
#pragma once

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wwe2k14::replay {

// Settings a recording carries (applied on playback if left at default): what shapes the match and its frames.
inline bool CarriedSetting(const std::string& n) {
  static const char* kPrefixes[] = {"wwe2k14_launch", "wwe2k14_runin", "wwe2k14_entrance_60fps", "wwe2k14_crowd_hype",
                                    "wwe2k14_preset_", "wwe2k14_force_no_msaa", "wwe2k14_fast_loader",
                                    "wwe2k14_load_turbo", "wwe2k14_skip_boot_screens", "wwe2k14_native_",
                                    "wwe2k14_fast_guest_reads", "wwe2k14_det_inline_motion",
                                    "wwe2k14_det_boot_alloc", "det_frames", "det_serial", "wwe2k14_s6_defaults",
                                    "wwe2k14_s7_defaults", "wwe2k14_quick_",  // Session 27: @quick's type / pace
                                    "wwe2k14_fix_secondary_motion",  // Session 27.5: can change root motion
                                    "wwe2k14_replay_time_follows_step",  // Session 27.5: guest time per frame
                                    "wwe2k14_step_",  // Session 77: the update step (the phone's FRAME RATE 30)
                                    // Session 77 (cross-device): which code runs and what the game is told
                                    "wwe2k14_nf_", "occlusion_query_enable", "wwe2k14_phys_alloc_fast_fail",
                                    "wwe2k14_dynamic_res", "user_language",
                                    // Session 77 (P7): the crowd pass (it skips the game's own recorder calls) and
                                    // the tester check's constant clocks / file answers (include/rex/det.h)
                                    "wwe2k14_scene_crowd", "det_time_constant", "det_fs_constant",
                                    // Lane 176: an explicit off keeps the 16:9 crowd tests (guest memory differs with it on)
                                    "wwe2k14_crowd_fill_wide"};
  // S101 lane 111 (go-anywhere L-2): the place cvars, EXACT names (a prefix would pull in other wwe2k14_match_*
  // options that are not the place): a harvest recording must rebuild the same match, the 30 Years jump and the
  // forced victory scene when it is played back.
  static const char* kPlaceCvars[] = {"wwe2k14_match", "wwe2k14_match_go", "wwe2k14_wm30", "wwe2k14_wm30_auto",
                                       "wwe2k14_scene_victory"};
  for (const char* p : kPlaceCvars)
    if (n == p) return true;
  // Session 27.5: the log option, and det_frames_time_hz, which step_rate.h sets every frame (not a recording's).
  if (n == "wwe2k14_fix_secondary_motion_log" || n == "det_frames_time_hz") return false;
  // Session 77: the scheduler's trace folder and wall-clock tunables are the host's, not the game's (a PC path carried
  // into a recording conflicted with the phone's own folder).
  if (n == "det_serial_trace" || n == "det_serial_stall_ms" || n == "det_serial_vblank_timeout_ms" ||
      n == "det_frames_timeout_ms" || n == "det_frames_settle_us")
    return false;
  for (const char* p : kPrefixes) {
    if (n.rfind(p, 0) == 0) {
      // Not the diagnosis options of those families.
      if (n.find("_verify") != std::string::npos || n.find("_probe") != std::string::npos ||
          n.find("_corpus") != std::string::npos || n == "wwe2k14_launch_poke" || n == "wwe2k14_runin_now" ||
          n == "wwe2k14_quick_compare")
        return false;
      return true;
    }
  }
  return false;
}

// Lane 176 (round 3): the crowd's screen tests depend on the display (gpu_wide_factor / gpu_tall_factor, set from --wwe2k14_aspect), which a recording does not
// carry, so a recording always writes the RESOLVED switch (off, or k=..,t=.. explicit: crowd_wide_core.h RecordValue), and a recording with no such line
// (every one made before lane 176) plays back as off: it ran the game's 16:9 test.
inline constexpr const char* kCrowdFillWide = "wwe2k14_crowd_fill_wide";
inline void AddOldRecordingDefaults(std::vector<std::pair<std::string, std::string>>& flags) {
  for (const auto& kv : flags)
    if (kv.first == kCrowdFillWide) return;
  flags.emplace_back(kCrowdFillWide, "off");
}

// The header line of one setting: "flag NAME VALUE\n" when carried, else the "# modified:" comment line.
inline std::string HeaderLine(const std::string& name, const std::string& value, bool host_path) {
  if (CarriedSetting(name)) return "flag " + name + " " + value + "\n";
  return "# modified: " + name + " = " + (host_path ? std::string("<path>") : value) + "\n";
}

// Reads one header line back: true and the pair when it is a `flag NAME VALUE` line (the value is everything after
// the name and its spaces, as the loader read it before). Any other line (an old recording has no place flags) is false.
inline bool ParseFlagLine(const std::string& line, std::pair<std::string, std::string>& out) {
  if (line.rfind("flag ", 0) != 0) return false;
  size_t i = 5;
  while (i < line.size() && line[i] == ' ') ++i;
  size_t j = line.find(' ', i);
  if (j == std::string::npos) j = line.size();
  out.first = line.substr(i, j - i);
  const std::string v = line.substr(j);
  const size_t k = v.find_first_not_of(' ');
  out.second = k == std::string::npos ? std::string() : v.substr(k);
  return !out.first.empty();
}

}  // namespace wwe2k14::replay
