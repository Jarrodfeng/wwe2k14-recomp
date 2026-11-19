// S103 lane WMB: drives the menu-free 30 Years start's order of steps (src/wm30_boot_guard.h) with a recording stand-in for the game and prints one
// line per scenario: "<name>|<trace>|<facts>". tools/wm30/boot_selftest.py compares each line with the expected one (compiled against the real header
// and against planted twins that must each differ). No game, no SDK.
//
// The stand-in answers like the game would: Ready() turns true `ready_after` frames after S was created (or from the start when S already existed),
// Lookup() answers `lookup_ok` WITHOUT regard to Ready() (the pessimistic case: a header that skips the ready wait would then act too early), Idle()
// follows a scripted frame pattern. Every call that matters is traced with "@ready" / "@pending" (was the game's ready test true at that moment).
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "wm30_boot_guard.h"

using namespace wwe2k14::wm30_boot;

struct Script {
  const char* name;
  bool has_s = false;
  int ready_after = 5;       // frames after CreateS (or after the start when has_s)
  bool lookup_ok = true;
  int idle_from = 0;         // idle for frames in [idle_from, idle_until)
  int idle_until = 1 << 30;
  int idle_gap = -1;         // one non-idle frame at this index (-1 none)
  bool idle_lost_after_select = false;
  int prereq_from = 0;       // Prerequisites() true from this frame
  int calls_per_frame = 1;   // Step called this many times with the same frame number
  int frames = 1500;
  int ms_per_frame = 100;    // the wall clock: NowMs() = frame * this (20 s ready timeout = 200 frames at 100 ms)
  bool selobj_ok = true;     // [S+4] exists
  int s_replaced_at = -1;    // the current S is another object from this frame
  int seq_left_at = -1;      // the sequence is not the main menu from this frame
  int exit_action_from = -1; // 0x83929434 holds exit_action_value from this frame
  int exit_action_value = 0;
};

struct Mock {
  const Script& s;
  uint64_t frame = 0;
  std::vector<std::string> trace;
  std::vector<int> at;       // frame of each trace entry
  bool created_s = false, selected = false;
  int64_t created_at = -1;
  int exits = 0;
  explicit Mock(const Script& sc) : s(sc) {}

  uint64_t Frame() { return frame; }
  bool Idle() {
    if (s.idle_lost_after_select && selected) return false;
    if (int(frame) == s.idle_gap) return false;
    return int(frame) >= s.idle_from && int(frame) < s.idle_until;
  }
  uint64_t NowMs() { return uint64_t(frame) * uint64_t(s.ms_per_frame); }
  bool MainMenuSeq() { return !(s.seq_left_at >= 0 && int(frame) >= s.seq_left_at); }
  uint32_t ExitAction() { return (s.exit_action_from >= 0 && int(frame) >= s.exit_action_from) ? uint32_t(s.exit_action_value) : 0; }
  uint32_t SAddr() {
    if (!HasS()) return 0;
    if (s.s_replaced_at >= 0 && int(frame) >= s.s_replaced_at) return 0x9000;
    return created_s ? 0x1000 : 0x2000;
  }
  bool SelectionObject() { return s.selobj_ok; }
  bool Prerequisites() { return int(frame) >= s.prereq_from; }
  bool HasS() { return s.has_s || created_s; }
  bool ReadyNow() {
    if (!HasS()) return false;
    const int64_t base = s.has_s ? 0 : created_at;
    return int64_t(frame) >= base + s.ready_after;
  }
  bool Ready() { return ReadyNow(); }
  void Rec(const std::string& what) {
    trace.push_back(what);
    at.push_back(int(frame));
  }
  void PreCreate(uint32_t heap) { Rec("pre(" + std::to_string(heap) + ")"); }
  void CreateS(uint32_t heap) {
    created_s = true;
    created_at = int64_t(frame);
    Rec("create(" + std::to_string(heap) + ")");
  }
  bool Lookup(int& era, int& code) {
    if (!s.lookup_ok) return false;
    era = 2;
    code = 10201;
    return true;
  }
  void DecodeKinds() { Rec("decode"); }
  void ResetMatchDescription() { Rec("reset"); }
  void Select(int, int) {
    selected = true;
    Rec(std::string("select") + (ReadyNow() ? "@ready" : "@pending"));
  }
  void LogRoute() {}
  void Exit(int action) {
    ++exits;
    Rec("exit(" + std::to_string(action) + ")" + (ReadyNow() ? "@ready" : "@pending"));
  }
  void Destroy() { Rec(std::string("destroy") + (ReadyNow() ? "@ready" : "@pending")); }
  void Finished() { Rec("finished"); }
  void Note(const char*, int) {}
};

static void Run(const Script& sc) {
  Mock g(sc);
  Machine m;
  for (int f = 0; f < sc.frames; ++f) {
    g.frame = uint64_t(f);
    for (int k = 0; k < sc.calls_per_frame; ++k) Step(m, g);
  }
  std::string t;
  for (size_t i = 0; i < g.trace.size(); ++i) t += (i ? " " : "") + g.trace[i];
  // facts: the frame of the first and last call, and the number of exits
  std::string facts = "first=" + std::to_string(g.at.empty() ? -1 : g.at.front()) + " last=" + std::to_string(g.at.empty() ? -1 : g.at.back()) +
                      " exits=" + std::to_string(g.exits);
  std::cout << sc.name << "|" << t << "|" << facts << "\n";
}

int main() {
  { Script s{"happy"}; Run(s); }
  { Script s{"existing_s"}; s.has_s = true; s.ready_after = 0; Run(s); }
  { Script s{"lookup_miss_ready"}; s.lookup_ok = false; Run(s); }
  { Script s{"never_ready"}; s.ready_after = 100000; Run(s); }
  { Script s{"idle_lost_before_ready"}; s.ready_after = 50; s.idle_until = 60 + 3; Run(s); }
  { Script s{"idle_lost_never_ready"}; s.ready_after = 100000; s.idle_until = 60 + 3; Run(s); }
  { Script s{"idle_lost_after_select"}; s.idle_lost_after_select = true; Run(s); }
  { Script s{"idle_gap_at_59"}; s.idle_gap = 59; Run(s); }
  { Script s{"prereq_late"}; s.prereq_from = 100; Run(s); }
  { Script s{"same_frame_thrice"}; s.calls_per_frame = 3; Run(s); }
  { Script s{"existing_s_idle_lost"}; s.has_s = true; s.ready_after = 0; s.idle_until = 60 + 2; s.lookup_ok = false; Run(s); }
  { Script s{"lookup_miss_idle_lost"}; s.lookup_ok = false; s.idle_until = 60 + 10; Run(s); }
  // S replaced / the sequence left / sequence 31 adopted S while an abort waits for the reads: never destroyed here
  { Script s{"abort_s_replaced"}; s.ready_after = 50; s.idle_until = 60 + 3; s.s_replaced_at = 70; Run(s); }
  { Script s{"abort_seq_left"}; s.ready_after = 50; s.idle_until = 60 + 3; s.seq_left_at = 70; Run(s); }
  { Script s{"abort_adopted_action2"}; s.ready_after = 50; s.idle_until = 60 + 3; s.exit_action_from = 63; s.exit_action_value = 2; Run(s); }
  { Script s{"abort_adopted_action4_later"}; s.ready_after = 50; s.idle_until = 60 + 3; s.exit_action_from = 80; s.exit_action_value = 4; Run(s); }
  // an existing S whose match is not in the index: the timeout leaves it alone
  { Script s{"existing_s_lookup_miss"}; s.has_s = true; s.ready_after = 0; s.lookup_ok = false; Run(s); }
  // the progress object B = [S+4] is missing: abort (S destroyed, nothing written)
  { Script s{"no_selection_object"}; s.selobj_ok = false; Run(s); }
  // the timeouts are wall time, not frames: a slow clock times out in few frames, a fast clock (uncapped) in many
  { Script s{"slow_clock_timeout"}; s.ready_after = 100000; s.ms_per_frame = 1000; Run(s); }
  { Script s{"fast_clock_waits_long"}; s.ready_after = 1500; s.ms_per_frame = 10; s.frames = 3000; Run(s); }
  { Script s{"never_idle"}; s.idle_from = 1 << 30; Run(s); }
  return 0;
}
