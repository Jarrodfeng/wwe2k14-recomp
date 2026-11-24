// S110 HOMESTEP: host unit for src/menu_home_core.h (the rules of the `home` route line) on fake state. No game data.
// Prints "ok N checks" and returns 0, else the failures and 1.
#include <cstdio>
#include <string>

#include "menu_home_core.h"

using namespace wwe2k14::menu_home;

static int g_n = 0, g_bad = 0;
static bool Check(bool c, const char* what) {
  ++g_n;
  if (!c) std::printf("FAIL: %s\n", what), ++g_bad;
  return c;
}
static bool Has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }

// A settled front end on `menu` (all ReadyWhy inputs good), focus ready.
static View Settled(uint64_t frame, uint32_t menu) {
  View v;
  v.frame = frame;
  v.sm = 0x1000, v.seq = 75, v.table = true, v.holder_state = 2, v.lock56 = 0, v.list_state = 2, v.armed = false, v.menu = menu;
  v.ready_readable = true, v.menu_ready = 1, v.page_readable = true, v.menu_page = 0x82036000;
  return v;
}
// A screen outside the front end (a Threads-like page), taking input.
static View Outside(uint64_t frame, uint32_t page = 0x821B94BC) {
  View v;
  v.frame = frame;
  v.seq = 77;
  v.ready_readable = true, v.menu_ready = 1, v.page_readable = true, v.menu_page = page;
  return v;
}
static View WithBox(View v, uint32_t state, uint32_t id, uint32_t kind, uint32_t cursor = 1) {
  v.box_obj = 0x40000000, v.box_state = state, v.box_id = id, v.box_kind = kind, v.box_cursor = cursor;
  return v;
}

int main() {
  // done only on a settled main menu (seq 75, menu 1, ReadyWhy empty, no box)
  { Home h; h.Begin(0); Check(h.Decide(Settled(10, 1)).act == Act::kDone, "settled main menu: done"); }
  { Home h; h.Begin(0); Decision d = h.Decide(Settled(10, 1)); Check(Has(d.log, "done: main menu after 0 presses, 10 frames"), "done line text"); }
  { View v = Settled(10, 1); v.list_state = 1; Home h; h.Begin(0); Check(h.Decide(v).act == Act::kWait, "main menu while the list is not serving: wait, not done"); }
  { View v = Settled(10, 0); Home h; h.Begin(0); Check(h.Decide(v).act == Act::kWait, "root (menu 0): wait"); }
  { View v = Settled(10, 1); v.seq = 80; Home h; h.Begin(0); Check(h.Decide(v).act == Act::kPressB, "menu 1 in sequence 80 is not the main menu: B (bounded), not done"); }
  { View v = Settled(10, 1); v.armed = true; Home h; h.Begin(0); Check(h.Decide(v).act == Act::kWait, "mailbox armed: not settled, wait"); }
  { View v = WithBox(Settled(10, 1), 2, 1, 0); Home h; h.Begin(0); Check(h.Decide(v).act == Act::kPressA, "a box over the main menu is answered first, not done"); }

  // a submenu: one B (the submenu's own back)
  { Home h; h.Begin(0); Decision d = h.Decide(Settled(10, 1523)); Check(d.act == Act::kPressB && h.presses() == 1, "settled submenu: one B"); }
  // outside the front end: B only when the focused screen takes input
  { Home h; h.Begin(0); Decision d = h.Decide(Outside(10)); Check(d.act == Act::kPressB, "outside the front end, ready: B");
    Check(Has(d.log, "B on page 0x821B94BC (seq 77)"), "B log names the page word and the sequence"); }
  { View v = Outside(10); v.menu_ready = 0; Home h; h.Begin(0); Check(h.Decide(v).act == Act::kWait, "menu_ready 0: no B"); }
  { View v = Outside(10); v.ready_readable = false; Home h; h.Begin(0); Check(h.Decide(v).act == Act::kWait, "menu_ready unreadable: wait"); }
  { View v = Outside(10); v.sm = 0; Home h; h.Begin(0); Check(h.Decide(v).act == Act::kPressB, "no SmartMenu at all: B"); }
  // a front-end sequence whose SmartMenu is not built yet (the way back: 77 -> 4 -> 75, then the 75 update builds a new SM): wait, no B
  { View v = Outside(10); v.seq = 75; v.sm = 0; Home h; h.Begin(0); Decision d = h.Decide(v);
    Check(d.act == Act::kWait && Has(d.why, "no SmartMenu"), "sequence 75 with no SmartMenu yet: wait for it, no B"); }
  { Home h; h.Begin(0); View v = Settled(10, 1523); v.list_state = 1; Check(h.Decide(v).act == Act::kWait, "front end not settled: wait, no B"); }

  // after a press: wait for the reaction, one B per page
  { Home h; h.Begin(0);
    Check(h.Decide(Outside(10)).act == Act::kPressB, "press 1");
    Check(h.Decide(Outside(20)).act == Act::kWait, "no second B while the first has no reaction yet");
    Check(h.Decide(Outside(100, 0x821B9500)).act == Act::kPressB, "the page word changed: the next B at once");
    Check(h.presses() == 2, "two presses counted"); }
  // a B with no reaction twice in a row fails, naming sequence and page
  { Home h; h.Begin(0);
    h.Decide(Outside(10));
    Check(h.Decide(Outside(10 + kReactFrames - 1)).act == Act::kWait, "inside the window: wait");
    Check(h.Decide(Outside(10 + kReactFrames)).act == Act::kPressB, "window ran out with no change: the second B");
    Decision f = h.Decide(Outside(10 + 2 * kReactFrames));
    Check(f.act == Act::kFail && Has(f.why, "no reaction") && Has(f.why, "sequence 77") && Has(f.why, "0x821B94BC"), "two B with no reaction: FAIL naming sequence and page"); }
  // the 16-press bound: pages change every press, never home
  { Home h; h.Begin(0); uint64_t fr = 10; Decision d; int presses = 0;
    for (uint32_t i = 0; i < 40 && d.act != Act::kFail; ++i, fr += 5) { d = h.Decide(Outside(fr, 0x821B9000 + i * 4)); if (d.act == Act::kPressB) ++presses; }
    Check(presses == int(kMaxPresses) && d.act == Act::kFail && Has(d.why, "16 B presses"), "16 B and not home: FAIL"); }
  // the stall bound: not taking input for 600 frames
  { View v = Outside(10); v.menu_ready = 0; Home h; h.Begin(0); h.Decide(v);
    v.frame = 10 + kStallFrames; Check(h.Decide(v).act == Act::kWait, "stall: at the bound still waits");
    v.frame = 11 + kStallFrames; Decision d = h.Decide(v); Check(d.act == Act::kFail && Has(d.why, "no change in 600 frames") && Has(d.why, "menu_ready 0"), "stall past the bound: FAIL with the reason"); }
  // the overall bound
  { Home h; h.Begin(0); Check(h.Decide(Outside(kOverallFrames + 1)).act == Act::kFail, "overall bound: FAIL"); }

  // boxes by kind
  { Home h; h.Begin(0); Decision d = h.Decide(WithBox(Outside(10), 2, 50232, 0)); Check(d.act == Act::kPressA && Has(d.log, "box 50232 kind 0 answered A"), "kind 0: A"); }
  { Home h; h.Begin(0); Check(h.Decide(WithBox(Outside(10), 2, 1, 1)).act == Act::kPressA, "kind 1: A"); }
  { Home h; h.Begin(0); Decision d = h.Decide(WithBox(Outside(10), 2, 51121, 2, 1));
    Check(d.act == Act::kPressLeft, "kind 2 leave id, cursor on NO: LEFT first"); }
  { Home h; h.Begin(0); Decision d = h.Decide(WithBox(Outside(10), 2, 51121, 3, 0));
    Check(d.act == Act::kPressA && Has(d.log, "box 51121 kind 3 answered YES"), "kind 3 leave id, cursor on YES: A = YES"); }
  { Home h; h.Begin(0); Decision d = h.Decide(WithBox(Outside(10), 2, 99999, 2, 1)); View v = WithBox(Outside(10), 2, 99999, 2, 1); v.box_text = "ARE YOU SURE";
    d = Home().Decide(v);
    Check(d.act == Act::kFail && Has(d.why, "99999") && Has(d.why, "ARE YOU SURE") && Has(d.why, "kLeaveYes"), "unknown YES / NO id: FAIL with the id and the text, no guess"); }
  { Home h; h.Begin(0); Decision d = h.Decide(WithBox(Outside(10), 2, 7, 4)); Check(d.act == Act::kFail && Has(d.why, "id 7"), "kind 4 list: FAIL with the id"); }
  { Home h; h.Begin(0); Check(h.Decide(WithBox(Outside(10), 2, 7, 5)).act == Act::kFail, "kind 5 list: FAIL"); }
  { Home h; h.Begin(0); Check(h.Decide(WithBox(Outside(10), 2, 7, 6)).act == Act::kWait, "kind 6: wait"); }
  { Home h; h.Begin(0); Check(h.Decide(WithBox(Outside(10), 2, 7, 7)).act == Act::kWait, "kind 7: wait"); }
  { Home h; h.Begin(0); Check(h.Decide(WithBox(Outside(10), 2, 7, 9)).act == Act::kFail, "unknown kind: FAIL"); }
  { Home h; h.Begin(0); Check(h.Decide(WithBox(Outside(10), 2, 51121, 2, 5)).act == Act::kFail, "YES / NO cursor out of range: FAIL"); }
  // opening / answered / closing wait, and never press B under them
  for (uint32_t s : {1u, 3u, 4u}) {
    Home h; h.Begin(0);
    Check(h.Decide(WithBox(Outside(10), s, 50232, 0)).act == Act::kWait, "box state 1 / 3 / 4: wait");
  }
  { Home h; h.Begin(0); Check(h.Decide(WithBox(Outside(10), 6, 1, 0)).act == Act::kFail, "unknown box state: FAIL"); }
  { Home h; h.Begin(0); View v = Outside(10); v.box_obj = 0x40000000; v.box_state = 5; Check(h.Decide(v).act == Act::kPressB, "box idle (5): ignored, B"); }
  { Home h; h.Begin(0); View v = Outside(10); v.box_obj = 0x40000000; v.box_state = 0; Check(h.Decide(v).act == Act::kPressB, "box never posted (0): ignored, B"); }
  // a box press with no reaction: bounded at kNoReactBox
  { Home h; h.Begin(0); uint64_t fr = 10; Decision d; int a = 0;
    for (int i = 0; i < 10 && d.act != Act::kFail; ++i, fr += kReactFrames + 1) { d = h.Decide(WithBox(Outside(fr), 2, 50232, 0)); if (d.act == Act::kPressA) ++a; }
    Check(a == int(kNoReactBox) && d.act == Act::kFail, "box A with no reaction: 3 presses then FAIL"); }
  // a box wait stalls out too (no-input box that never ends)
  { Home h; h.Begin(0); h.Decide(WithBox(Outside(10), 2, 7, 6)); Decision d = h.Decide(WithBox(Outside(11 + kStallFrames), 2, 7, 6));
    Check(d.act == Act::kFail && Has(d.why, "kind 6"), "no-input box that never ends: FAIL naming the kind"); }
  // the leave table
  Check(IsLeaveYes(51121) && !IsLeaveYes(50232) && !IsLeaveYes(0), "leave table holds 51121 only");

  if (g_bad) return 1;
  std::printf("menu_home ok %d checks\n", g_n);
  return 0;
}
