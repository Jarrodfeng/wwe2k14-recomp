// S106 0a: host unit for src/script_fail_policy.h (QuitOnScriptFail). Exit 1 on any wrong row.
// entry: wwe2k14::QuitOnScriptFail
#include <cstdio>
#include "../../src/script_fail_policy.h"
using wwe2k14::QuitOnScriptFail;
static int bad = 0;
static void Row(const char* n, bool got, bool want) {
  if (got != want) { std::printf("FAIL %s: got %d want %d\n", n, got, want); bad++; } else std::printf("ok   %s\n", n);
}
int main() {
  // QuitOnScriptFail(route_says_quit, force, live_batch, diag_hold, live_session)
  Row("S105 fault today: on_fail continue, switch off -> no quit", QuitOnScriptFail(false, false, false, false), false);
  Row("the fix: on_fail continue, switch on -> quit", QuitOnScriptFail(false, true, false, false), true);
  Row("default: on_fail quit, switch off -> quit", QuitOnScriptFail(true, false, false, false), true);
  Row("on_fail quit, switch on -> quit", QuitOnScriptFail(true, true, false, false), true);
  Row("planted: switch on + live batch on_fail continue -> no quit", QuitOnScriptFail(false, true, true, false), false);
  Row("planted: switch on + diag hold -> no quit", QuitOnScriptFail(false, true, false, true), false);
  Row("planted: on_fail quit + diag hold -> no quit", QuitOnScriptFail(true, false, false, true), false);
  Row("planted: on_fail quit + switch on + diag hold -> no quit", QuitOnScriptFail(true, true, false, true), false);
  Row("live batch on_fail quit -> quit (unchanged)", QuitOnScriptFail(true, false, true, false), true);
  Row("live batch on_fail quit, switch on -> quit", QuitOnScriptFail(true, true, true, false), true);
  // S106 0aF (review BLOCK): a --wwe2k14_live session (live.py, the launcher's Test harness) ignores the force switch
  Row("live session on_fail continue, switch on -> no quit", QuitOnScriptFail(false, true, false, false, true), false);
  Row("live session on_fail quit, switch on -> quit (route's own rule)", QuitOnScriptFail(true, true, false, false, true), true);
  Row("live session, diag hold, on_fail quit -> no quit", QuitOnScriptFail(true, false, false, true, true), false);
  Row("no live session, switch on -> quit (unchanged)", QuitOnScriptFail(false, true, false, false, false), true);
  // the load arm (a script that fails to load): the route's on_fail is unknown, only the switch asks
  Row("load arm: switch on, no live, no hold -> quit", QuitOnScriptFail(wwe2k14::ScriptFailState{false, true, false, false, false}), true);
  Row("load arm: switch on + diag hold -> hold (no quit)", QuitOnScriptFail(wwe2k14::ScriptFailState{false, true, false, true, false}), false);
  Row("load arm: switch on + live session -> no quit", QuitOnScriptFail(wwe2k14::ScriptFailState{false, true, false, false, true}), false);
  return bad ? 1 : 0;
}
