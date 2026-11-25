// S102 M7: --wwe2k14_ab_flip, the cvar glue of src\ab_flip_math.h (read its header for the spec format and the rules). Default empty = off: AtFrame costs one
// bool test. The spec is read ONCE at the first guest frame (it is a run-level choice; the switches it flips are the per-frame ones).
// Called from frame_stats.h's OnSwap on the swap thread, BEFORE rdr_sw::RefreshFromCvars of the same frame, so the refresh logs "[switch] frame N NAME=VALUE"
// for every flipped switch and every reader's "[switch] seen" line follows as for a route's cvar step. Log lines: [ab_flip] on ... / anchor mark N at frame A /
// [ab_flip] frame F phase NAME no K rel R (R = F - the anchor frame).
#pragma once
#include <rex/cvar.h>
#include <rex/logging.h>

#include "ab_flip_anchor.h"
#include "ab_flip_math.h"

REXCVAR_DECLARE(std::string, wwe2k14_ab_flip);

namespace ab_flip {

inline Runner g_runner;
inline bool SetCvar(const char* name, const char* value) { return rex::cvar::SetFlagByName(name, value); }
inline void LogCvar(const char* line) { REXLOG_INFO("{}", line); }
inline bool IsBoolCvar(const char* name) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  return info && info->type == rex::cvar::FlagType::Boolean;
}
inline bool ReadCvar(const char* name, std::string& text) {
  if (!rex::cvar::GetFlagInfo(name)) return false;
  text = rex::cvar::GetFlagByName(name);
  return true;
}

inline void AtFrame(uint64_t frame) {
  if (!g_runner.started) {
    Env env;
    env.is_bool = &IsBoolCvar;
    env.read = &ReadCvar;
    g_runner.Begin(REXCVAR_GET(wwe2k14_ab_flip), frame, &LogCvar, &env);
  }
  if (g_runner.active) g_runner.AtFrame(frame, g_runner.spec.anchor ? ab_flip_anchor::MarkFrame(g_runner.spec.anchor) : 0, &SetCvar, &LogCvar);
}

}  // namespace ab_flip
