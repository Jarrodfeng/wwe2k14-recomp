// S106 0a: should a failure of the main input script quit the game? One pure function (no SDK includes) so a host
// unit can test it (tools/script_fail/).
//   route_says_quit: the route's own `on_fail` (quit_on_fail_)
//   force:           --wwe2k14_input_fail_quit (run_list sets it: a failed route frees its slot at once)
//   live_batch:      a live batch is running (they keep their own rule: never forced)
//   diag_hold:       the phone's diagnostic hold (never quits)
//   live_session:    --wwe2k14_live is set (live.py, the launcher's Test harness): the game stays up whatever the route's boot
//                    script does, so the force switch is ignored (the route's own on_fail still counts)
#pragma once

namespace wwe2k14 {

inline bool QuitOnScriptFail(bool route_says_quit, bool force, bool live_batch, bool diag_hold, bool live_session = false) {
  if (diag_hold) return false;
  if (live_batch || live_session) return route_says_quit;
  return route_says_quit || force;
}

// The same decision from one named bundle (the two fail functions in input_script.h build it; reads clearer than four bools).
struct ScriptFailState {
  bool route_says_quit, force, live_batch, diag_hold, live_session = false;
};
inline bool QuitOnScriptFail(const ScriptFailState& s) {
  return QuitOnScriptFail(s.route_says_quit, s.force, s.live_batch, s.diag_hold, s.live_session);
}

}  // namespace wwe2k14
