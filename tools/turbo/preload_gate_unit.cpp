// S111 PRELOADGATE: the host unit of src/load_turbo_preload_gate.h (built and run by tools/turbo/preload_gate_selftest.py).
// A small model of LoadTurbo's "preload" window: per frame the exit step's answer (the hook, during the guest update), then
// the frame end (OnFrame: a sequence change ends the window and the latch; the wall cap ends the window). Each scenario
// is one 30 Years menu exit. The MENULOAD keying (open only while D = [[0x839295F0]+24] is busy, close when D is idle) is
// run on the same scenarios as a twin and must miss the waits that are not D's: the loader's pending count (a) and the UI
// system teardown (c).
#include <cstdint>
#include <cstdio>
#include <vector>

#include "load_turbo_preload_gate.h"

using wwe2k14::preload_gate::Act;
using wwe2k14::preload_gate::Gate;

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

struct Step {
  int answer;      // the exit step's r3 this frame; -1 = not called (controller not finished, or another sequence)
  bool d_busy;     // D+16 != 0 (only the old keying reads it)
  uint32_t seq;    // the sequence id at the frame end
  bool cap;        // the wall cap is reached at this frame end
  bool other;      // another window (not "preload") is open this frame
};

struct Result {
  int waits = 0;          // frames the step answered 0
  int waits_open = 0;     // ... with the preload window open during that call
  int opens = 0;
  bool open_at_end = false;
  bool open_after_done = false;  // the window was still open after an answer of 1
};

// The new keying (load_turbo.h OnSequenceExitAnswer / OnFrame with preload_gate::Gate).
Result RunNew(const std::vector<Step>& steps) {
  Gate g;
  Result r;
  bool open = false;
  uint32_t last_seq = steps.empty() ? 0 : steps[0].seq, window_seq = 0;
  for (const Step& s : steps) {
    if (s.answer >= 0) {
      const Act a = g.OnAnswer(uint32_t(s.answer), open, open || s.other);
      if (a == Act::kOpen) {
        open = true;
        window_seq = last_seq;
        ++r.opens;
      } else if (a == Act::kClose) {
        open = false;
      }
      if (s.answer == 0) {
        ++r.waits;
        if (open) ++r.waits_open;
      } else if (open) {
        r.open_after_done = true;
      }
    }
    if (s.seq != last_seq) g.OnSequenceChanged();
    if (open && s.seq != window_seq) open = false;
    last_seq = s.seq;
    if (open && s.cap) open = false;
  }
  r.open_at_end = open;
  return r;
}

// The MENULOAD keying: OnSequenceExitWait opened only while D was busy; OnFrame closed at D idle or a sequence change.
Result RunOld(const std::vector<Step>& steps) {
  Result r;
  bool open = false;
  uint32_t last_seq = steps.empty() ? 0 : steps[0].seq, window_seq = 0;
  for (const Step& s : steps) {
    if (s.answer == 0 && !open && !s.other && s.d_busy) {
      open = true;
      window_seq = last_seq;
      ++r.opens;
    }
    if (s.answer == 0) {
      ++r.waits;
      if (open) ++r.waits_open;
    } else if (s.answer > 0 && open) {
      r.open_after_done = true;
    }
    if (open && (!s.d_busy || s.seq != window_seq)) open = false;
    last_seq = s.seq;
    if (open && s.cap) open = false;
  }
  r.open_at_end = open;
  return r;
}

// The pick: (b) D busy, then (a) the loader's pending count, then (c) the UI teardown, then the answer 1, then the
// overlay sequence (4) and the match sequence (9).
std::vector<Step> Pick() {
  std::vector<Step> v;
  v.push_back({-1, true, 31, false, false});
  for (int i = 0; i < 3; ++i) v.push_back({0, true, 31, false, false});
  for (int i = 0; i < 2; ++i) v.push_back({0, false, 31, false, false});
  for (int i = 0; i < 4; ++i) v.push_back({0, false, 31, false, false});
  v.push_back({1, false, 31, false, false});
  v.push_back({-1, false, 4, false, false});
  v.push_back({-1, false, 9, false, false});
  return v;
}

// D was idle before the pick (the cursor rested on the row): only (a) and (c) wait.
std::vector<Step> PickDIdle() {
  std::vector<Step> v;
  for (int i = 0; i < 5; ++i) v.push_back({0, false, 31, false, false});
  v.push_back({1, false, 31, false, false});
  v.push_back({-1, false, 4, false, false});
  return v;
}

// The cap ends the window inside a long wait: no second window for the same wait; the next visit opens again.
std::vector<Step> CapThenNextVisit() {
  std::vector<Step> v;
  for (int i = 0; i < 3; ++i) v.push_back({0, true, 31, i == 2, false});
  for (int i = 0; i < 6; ++i) v.push_back({0, true, 31, false, false});
  v.push_back({1, false, 31, false, false});
  v.push_back({-1, false, 1, false, false});   // back to the menus
  v.push_back({-1, false, 31, false, false});  // the 30 Years menu again
  for (int i = 0; i < 2; ++i) v.push_back({0, false, 31, false, false});
  v.push_back({1, false, 31, false, false});
  return v;
}

// The sequence changes while the step still waits (no answer 1 seen): the window ends there, the latch too.
std::vector<Step> SequenceChangeMidWait() {
  std::vector<Step> v;
  for (int i = 0; i < 3; ++i) v.push_back({0, false, 31, false, false});
  v.push_back({-1, false, 1, false, false});
  v.push_back({-1, false, 31, false, false});
  v.push_back({0, false, 31, false, false});
  v.push_back({1, false, 31, false, false});
  return v;
}

// Another window is open: the preload window does not take over.
std::vector<Step> OtherWindowOpen() {
  std::vector<Step> v;
  for (int i = 0; i < 3; ++i) v.push_back({0, false, 31, false, true});
  v.push_back({1, false, 31, false, true});
  return v;
}

}  // namespace

int main() {
  {
    const Result r = RunNew(Pick());
    Check(r.waits == 9 && r.waits_open == 9, "pick: the window is open on every frame the exit step waits (b, a and c)");
    Check(r.opens == 1, "pick: one window");
    Check(!r.open_after_done && !r.open_at_end, "pick: the window ends at the answer 1");
    const Result o = RunOld(Pick());
    Check(o.waits_open < o.waits, "pick: the MENULOAD keying (D busy) misses the loader and teardown waits");
  }
  {
    const Result r = RunNew(PickDIdle());
    Check(r.waits == 5 && r.waits_open == 5 && r.opens == 1 && !r.open_at_end, "D idle: the window covers the a / c waits");
    const Result o = RunOld(PickDIdle());
    Check(o.opens == 0 && o.waits_open == 0, "D idle: the MENULOAD keying opens no window");
  }
  {
    const Result r = RunNew(CapThenNextVisit());
    // first wait: open for 3 frames (the cap ends it at the 3rd frame end), not reopened for the other 6; next visit: 2
    Check(r.opens == 2, "cap: no second window in the same wait, a new one on the next visit");
    Check(r.waits_open == 3 + 2, "cap: the capped wait keeps its 3 frames, the next visit its 2");
    Check(!r.open_at_end && !r.open_after_done, "cap: nothing left open");
  }
  {
    const Result r = RunNew(SequenceChangeMidWait());
    Check(r.opens == 2 && r.waits_open == 4 && !r.open_at_end, "sequence change: ends the window and the latch");
  }
  {
    const Result r = RunNew(OtherWindowOpen());
    Check(r.opens == 0 && r.waits_open == 0, "another window open: no preload window");
  }
  {
    Gate g;
    Check(g.OnAnswer(1, false, false) == Act::kNone, "answer 1 with no window: nothing");
    // kClose stops whatever window LoadTurbo has open: another window ("ingame" / "match" / "menu") is never closed here
    Check(g.OnAnswer(1, false, true) == Act::kNone, "answer 1 with another window open: that window is not closed");
    Check(g.OnAnswer(0, false, false) == Act::kOpen, "answer 0, no window: open");
    Check(g.OnAnswer(0, true, true) == Act::kNone, "answer 0, window open: nothing");
    Check(g.OnAnswer(2, true, true) == Act::kClose, "any non-zero answer closes");
    Check(g.OnAnswer(0, false, false) == Act::kOpen, "the answer 1 ends the latch: the next wait opens");
  }
  std::printf("preload_gate_unit: %s\n", g_fail ? "FAILED" : "OK");
  return g_fail ? 1 : 0;
}
