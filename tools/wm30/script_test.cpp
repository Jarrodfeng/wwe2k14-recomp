// S110 FIRELINE: host unit for src/wm30_script_core.h (the `wm30_fire` / `wm30_step` input-script lines). Built and driven by
// tools/wm30/script_selftest.py (clang++ -std=c++17). No game, no SDK. Prints one "FAIL ..." line per broken expectation; exit 1 when any.
#include <iostream>
#include <string>
#include <vector>

#include "wm30_script_core.h"

using namespace wwe2k14::wm30_script;

static int g_fails = 0;
static void Expect(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fails;
    std::cout << "FAIL " << what << "\n";
  }
}

// A fake owner tick: records the request text, answers after `delay` polls with `answer` (empty = never).
static std::string g_seen, g_answer, g_submit_error;
static int g_delay = 0, g_polls = 0;
static bool g_withdrawn_was_waiting = true;
static std::string FakeSubmit(const std::string& args) {
  g_seen = args;
  g_polls = 0;
  return g_submit_error;
}
static bool FakePoll(std::string& answer) {
  if (g_answer.empty() || ++g_polls <= g_delay) return false;
  answer = g_answer;
  return true;
}
static bool FakeWithdraw() { return g_withdrawn_was_waiting; }

static void Plug() {
  g_fire_port = {&FakeSubmit, &FakePoll, &FakeWithdraw};
  g_step_port = {&FakeSubmit, &FakePoll, &FakeWithdraw};
  g_seen.clear(), g_answer.clear(), g_submit_error.clear();
  g_delay = 0, g_polls = 0, g_withdrawn_was_waiting = true;
}

int main() {
  // 1. The load-time rule is the live directive's own parser.
  const char* fire_good[] = {"1090", "1090 force", "1780 actor=1 mode=1", "1710 mode=-1", "65535", "  7  force "};
  const char* fire_bad[] = {"", "abc", "0", "65536", "-1", "5 actor=2", "5 forced", "5 mode=10", "force", "1090 force force", "10x"};
  const char* step_good[] = {"3 force", "0 force", "12", "5 steps=1,5 entry=none", "999"};
  const char* step_bad[] = {"", "x", "1000", "3 forse", "3 steps=", "-3", "3 force force"};
  for (const char* t : fire_good) Expect(CheckLine(Verb::kFire, t).empty(), std::string("wm30_fire '") + t + "' must load");
  for (const char* t : fire_bad) Expect(!CheckLine(Verb::kFire, t).empty(), std::string("wm30_fire '") + t + "' must be refused at load");
  for (const char* t : step_good) Expect(CheckLine(Verb::kStep, t).empty(), std::string("wm30_step '") + t + "' must load");
  for (const char* t : step_bad) Expect(!CheckLine(Verb::kStep, t).empty(), std::string("wm30_step '") + t + "' must be refused at load");
  Expect(CheckLine(Verb::kFire, "abc").rfind("error", 0) != 0, "the load refusal drops the live directive's 'error: ' prefix");

  // 2. The request carries every word of the line (force included) to the owner tick's port, under the live directive's verb word.
  Plug();
  Expect(Request(Verb::kFire, "1090 actor=1 force").empty(), "a queued fire returns no failure");
  Expect(g_seen == "fire 1090 actor=1 force", "the fire request text is the live directive's: got '" + g_seen + "'");
  Plug();
  Expect(Request(Verb::kStep, "7 force").empty() && g_seen == "step 7 force", "the step request text is the live directive's: got '" + g_seen + "'");

  // 3. The verdict is the line's outcome.
  Plug();
  Request(Verb::kFire, "1090");
  g_answer = "fired: bank 1090 (flag 0) queued by the game's own call 1003";
  g_delay = 2;
  std::string why;
  Expect(Poll(Verb::kFire, 0, why) == 0 && Poll(Verb::kFire, 1, why) == 0, "no answer yet: still waiting");
  Expect(Poll(Verb::kFire, 2, why) == 1 && why == g_answer, "a fired answer succeeds the line");

  const char* refusals[] = {"refused: not in a 30 Years match", "busy: still loading", "refused: a scene is already running", "failed: the game's function returned without setting G+468",
                            "error: the game thread did not serve the request"};
  for (const char* r : refusals) {
    Plug();
    Request(Verb::kFire, "1090");
    g_answer = r;
    Expect(Poll(Verb::kFire, 0, why) == -1, std::string("a refused fire fails the line: ") + r);
    Expect(why == r, std::string("the failure text is the verdict's own: got '") + why + "'");
  }
  Plug();
  Request(Verb::kStep, "7 force");
  g_answer = "stepped: the match script's step is now 7";
  Expect(Poll(Verb::kStep, 0, why) == 1, "a stepped answer succeeds the line");
  g_answer = "refused: step 7 is guarded";
  Expect(Poll(Verb::kStep, 0, why) == -1 && why == g_answer, "a refused step fails the line with its text");
  g_answer = "fired: bank 1";  // a fire answer is not a step answer
  Expect(Poll(Verb::kStep, 0, why) == -1, "a step line is not satisfied by a fire answer");

  // 4. Never served: bounded, and the failure says which half did not happen.
  Plug();
  Request(Verb::kFire, "1090");
  Expect(Poll(Verb::kFire, kScriptFrames - 1, why) == 0, "an unanswered request waits inside the bound");
  Expect(Poll(Verb::kFire, kScriptFrames, why) == -1 && why.find("never took") != std::string::npos, "an untaken request fails at the bound: got '" + why + "'");
  g_withdrawn_was_waiting = false;
  Expect(Poll(Verb::kFire, kScriptFrames, why) == -1 && why.find("no answer came") != std::string::npos, "a taken request with no answer fails at the bound: got '" + why + "'");

  // 5. The request half's own refusals and a port that is not built in.
  Plug();
  g_submit_error = "error: a fire request is already waiting for the game";
  Expect(Request(Verb::kFire, "1090") == "a fire request is already waiting for the game", "a busy port fails the line with its text");
  g_fire_port = Port{};
  g_step_port = Port{};
  Expect(Request(Verb::kFire, "1090").find("not built in") != std::string::npos, "a missing fire port fails the line");
  Expect(Request(Verb::kStep, "7 force").find("not built in") != std::string::npos, "a missing step port fails the line");

  std::cout << (g_fails ? "script_test: FAILED\n" : "script_test: all passed\n");
  return g_fails ? 1 : 0;
}
