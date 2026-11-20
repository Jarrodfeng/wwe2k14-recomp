// S105 A9 (docs\research\s105\A9.md): what the player is told when the graphics driver drops the game's GPU device (VK_ERROR_DEVICE_LOST).
// Pure: no SDK, no game, no Windows includes (the unit test tools\renderer\device_lost_message_test.cpp and frame_stats.h include it).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace wwe2k14::device_lost_message {

// The wording Jarrod approved (S105 pre-approval 4). The phone has no "Report a problem" button: its text names the launcher.
inline constexpr const char* kTextPc =
    "The graphics driver stopped. Please restart the game. If this keeps happening, update your graphics driver, then send a report (launcher: Report a problem).";
inline constexpr const char* kTextPhone =
    "The graphics driver stopped. Please restart the game. If this keeps happening, send a report from the launcher.";

// The black-box line the phone's launcher reads (src\launcher_mobile\phone_check.h LastRun looks for exactly this prefix).
inline constexpr const char* kBlackboxPrefix = "GPU LOST: ";

// The switch wwe2k14_rdr_device_lost_message (default ON in the table, in reader.h and in start_settings.cpp) is the only thing that decides;
// the loss itself is told once (the caller keeps the "already seen" flag).
inline bool ShouldShow(bool switch_on) { return switch_on; }

// The log line (the report's reader and the plant's check read it). Always written, whether the box is shown or not.
inline std::string LogLine(bool shown, bool phone) {
  std::string line = "[wwe2k14][frames] message for the player: ";
  line += phone ? kTextPhone : kTextPc;
  if (!shown) line += " (not shown: wwe2k14_rdr_device_lost_message is off)";
  return line;
}

inline std::string BlackboxLine(uint64_t guest_frame) {
  char text[160];
  std::snprintf(text, sizeof(text), "%sthe graphics driver stopped (Vulkan device lost) at guest frame %llu", kBlackboxPrefix, (unsigned long long)guest_frame);
  return text;
}

}  // namespace wwe2k14::device_lost_message
