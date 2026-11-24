// wwe2k14 - the input-script lines `wm30_fire BANK [actor=0|1] [mode=N] [force]` and `wm30_step K [force] [steps=A,B,..] [entry=none]`
// (S110 FIRELINE). The pure part, no game or SDK includes (tools\wm30\script_test.cpp compiles this exact header; src\input_script.h
// includes it, the same recipe as src\menu_goto_core.h).
//
// They do what `!wm30 fire ...` / `!wm30 step ...` do on the live channel, through the SAME code: the argument rule is ParseFireArgs /
// ParseStepArgs (src\wm30_fire_args.h / src\wm30_step_args.h; a line is checked with them when the script LOADS) and the guards are the
// owner tick's FireVerdict / StepVerdict (src\wm30_fire.h / src\wm30_step.h: Tick -> Fire / Step). The live command blocks its pipe thread
// up to 3 s for the owner tick's answer; the script runs on the frame hook and must not block, so the game side offers the same request
// split in two (submit = the live command's request half, poll = its wait half) and the line polls once per frame.
// The line succeeds when the verb's answer starts "fired" / "stepped" and FAILS the script (on_fail quit) with the verdict's own text
// otherwise (busy:, refused:, failed:, or the port not built in / never served within kScriptFrames).
#pragma once

#include <cstdint>
#include <string>

#include "wm30_fire_args.h"
#include "wm30_step_args.h"

namespace wwe2k14::wm30_script {

constexpr uint64_t kScriptFrames = 600;  // the owner tick serves a request within a frame or two; the live command waits 3 s

// One verb's port, filled at start-up by src\wm30_fire.h / src\wm30_step.h (null = that header is not built in).
//   submit(args): the live command's text ("fire 1090 force"); "" = queued, else the refusal text (a request already waiting).
//   poll(answer): true once the owner tick has answered (answer = its verdict text); false while it has not.
//   withdraw(): take back a request the tick never took; true = it was still waiting.
struct Port {
  std::string (*submit)(const std::string& args) = nullptr;
  bool (*poll)(std::string& answer) = nullptr;
  bool (*withdraw)() = nullptr;
};
inline Port g_fire_port, g_step_port;

enum class Verb { kFire, kStep };

inline const char* VerbWord(Verb v) { return v == Verb::kFire ? "fire" : "step"; }

// The load-time check of a script line: "" = fine, else the reason (the live directive's own error text, "error: " dropped).
inline std::string CheckLine(Verb v, const std::string& rest) {
  std::string e;
  if (v == Verb::kFire) {
    wm30_fire::FireArgs a;
    e = wm30_fire::ParseFireArgs(std::string("fire ") + rest, a);
  } else {
    wm30_step::StepArgs a;
    e = wm30_step::ParseStepArgs(std::string("step ") + rest, a);
  }
  if (e.rfind("error: ", 0) == 0) e = e.substr(7);
  return e;
}

// The verb's answer as a line outcome: "" = it fired / stepped; else the verdict's own text (the line fails with it).
inline std::string Judge(Verb v, const std::string& answer) {
  const std::string ok = v == Verb::kFire ? "fired" : "stepped";
  if (answer.rfind(ok, 0) == 0) return "";
  return answer.empty() ? "no answer text" : answer;
}

// The request half, once per line. "" = queued; else the failure text.
inline std::string Request(Verb v, const std::string& rest) {
  const Port& p = v == Verb::kFire ? g_fire_port : g_step_port;
  if (!p.submit || !p.poll) return std::string("the ") + VerbWord(v) + " verb is not built in";
  std::string e = p.submit(std::string(VerbWord(v)) + " " + rest);
  if (e.rfind("error: ", 0) == 0) e = e.substr(7);
  return e;
}

// The wait half, once per frame after Request. 0 = waiting (frames_waited < kScriptFrames); 1 = the verb fired / stepped (why = its answer);
// -1 = the line fails (why = the verdict's refusal text, or the not-served text).
inline int Poll(Verb v, uint64_t frames_waited, std::string& why) {
  const Port& p = v == Verb::kFire ? g_fire_port : g_step_port;
  std::string answer;
  if (p.poll && p.poll(answer)) {
    const std::string bad = Judge(v, answer);
    why = bad.empty() ? answer : bad;
    return bad.empty() ? 1 : -1;
  }
  if (frames_waited < kScriptFrames) return 0;
  if (p.withdraw && p.withdraw()) why = "the game thread never took the request (is the owner tick running frames?)";
  else why = "the request was taken but no answer came";
  return -1;
}

}  // namespace wwe2k14::wm30_script
