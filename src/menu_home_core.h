// S110 HOMESTEP: the rules of the route command `home` (pure, std only; the guest reads and the presses are src/menu_home.h).
// Plan: docs/research/s110/HOME.md (no direct switch exists: each sequence runs its own teardown, so the way home is the game's own B,
// one page at a time), docs/research/s110/MSGBOX.md (the message box), docs/research/s110/BACK.md (the settled front end).
// Mechanism: every frame the caller hands in a View (what the guest holds now); Decide returns one action. Nothing is guessed:
// an unknown YES / NO box, a list box, an unknown kind or box state, an unreadable focus signal that never clears, or a bound
// run out ends the line with a FAIL that names the sequence, the page word and the box.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "menu_goto_core.h"  // ReadyWhy, kSmListState ...: the same settled-front-end test the `page` line waits on

namespace wwe2k14::menu_home {

// ---- The box (docs/research/s110/MSGBOX.md, claims 1 / 4 / 8) ----
constexpr uint32_t kBoxPtr = 0x839294BC;  // MSGBOX: the one global message box manager
constexpr uint32_t kBoxState = 16;        // MSGBOX [C]: 0 never posted, 1 opening, 2 waiting for the answer, 3 answered, 4 closing after B, 5 idle
constexpr uint32_t kBoxCursor = 64;       // MSGBOX claim 5 [C]: YES / NO column (kinds 2 / 3): 0 YES, 1 NO (claim 7: A on open = NO)
constexpr uint32_t kBoxText = 1792;       // MSGBOX claim 4 [C]: the text id
constexpr uint32_t kBoxKind = 1800;       // MSGBOX claim 4 [C]: the event kind 0..7
constexpr uint32_t kBoxChars = 361;       // MSGBOX claim 11 [C]: the expanded text starts at box+361 (best-effort, for the log only)
constexpr uint32_t kBoxOpening = 1, kBoxWaiting = 2, kBoxAnswered = 3, kBoxClosing = 4;

// Text ids of YES / NO boxes that ASK whether to leave / discard; the answer for these is YES. One id per line, one comment per id.
// A YES / NO box whose id is not here FAILS the line (never guessed).
constexpr uint32_t kLeaveYes[] = {
    51121,  // "... want to keep these settings?" on B from a changed option / MATCH RULES page (run 5906 screenshot, tools/atlas/nav.py):
            // NO stays on the page, YES leaves it. Whether YES keeps or drops the change is not settled (nav.py says drops): a route must not rely on it.
};

constexpr bool IsLeaveYes(uint32_t id) {
  for (uint32_t v : kLeaveYes)
    if (v == id) return true;
  return false;
}

// ---- Bounds (the values and why) ----
// A press is judged by its reaction: the screen's page word, the sequence, the SmartMenu, the menu word or a box state changing.
// 180 frames (3 s at 60 fps): a page change and its fade take well under a second; a B that starts a leave (Threads, BACK claim 2)
// changes the sequence only after its own count-down. A press with no reaction in the window is counted; two in a row end the line.
constexpr uint64_t kReactFrames = 180;
constexpr uint32_t kNoReactB = 2;    // B presses in a row with no reaction: FAIL (a screen that ignores B, BACK "Not left by one B")
constexpr uint32_t kNoReactBox = 3;  // box presses (A / LEFT) in a row with no reaction: FAIL (the first may land before the box takes input)
// A wait with no change at all (screen not taking input, a fade, a rebuild, an opening or closing box, a no-input box): the same
// 600 frames (10 s) BACK gives a leave + new SmartMenu + list state 0 -> 1 -> 2 (menu_goto_core.h kReturnFrames).
constexpr uint64_t kStallFrames = wwe2k14::menu_goto::kReturnFrames;
// The whole line: 16 B presses (HOME.md "Not left by one B": a deep screen is a handful of pages) and 3600 frames (60 s: 16 leaves of a
// few seconds each is the longest honest walk; more means the walk is not ending).
constexpr uint32_t kMaxPresses = 16;
constexpr uint64_t kOverallFrames = 3600;

enum class Act { kWait, kPressB, kPressA, kPressLeft, kDone, kFail };

// What the guest holds this frame (filled by src/menu_home.h; a unit fills it by hand).
struct View {
  uint64_t frame = 0;
  // the settled-front-end test (menu_goto_core.h ReadyWhy inputs)
  uint32_t sm = 0, seq = 0;
  bool table = false;
  uint32_t holder_state = 0, lock56 = 0, list_state = 0;
  bool armed = false;
  uint32_t menu = 0;  // SM+364
  // the focused screen (src/menu_focus.h built-in states: the same source `wait_state menu_ready` reads)
  bool ready_readable = false;
  uint32_t menu_ready = 0;
  bool page_readable = false;
  uint32_t menu_page = 0;
  // the box (MSGBOX): box_obj = [0x839294BC]; the fields below are valid only while box_state is 1..4 (claim 4 note)
  uint32_t box_obj = 0, box_state = 0, box_id = 0, box_kind = 0, box_cursor = 0;
  std::string box_text;  // best effort, for messages only ("" = unreadable)
};

struct Decision {
  Act act = Act::kWait;
  std::string log;  // the one action line (without the log prefix); empty for a wait
  std::string why;  // for kWait: what it waits on; for kFail: the message
};

inline std::string Hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

// The settled front end (BACK.md): ReadyWhy empty. The caller supplies the same inputs menu_goto::Tick does.
inline std::string SettledWhy(const View& v) {
  return wwe2k14::menu_goto::ReadyWhy(v.sm, v.seq, v.table, v.holder_state, v.lock56, v.list_state, v.armed, v.menu);
}
// A front-end sequence with or without its SmartMenu: with none yet (the way back is 77 -> 4 -> 75 and the 75 update then builds a new SM,
// BACK claim 4) ReadyWhy says "no SmartMenu" and the line waits for it instead of pressing B into the rebuild.
// The ids are the game's own SmartMenu gate's (sub_8283D370: seq 75 / 80 / 81, BACK.md claim 11). S110 SEATREL: the seat prototype's "no match loaded" test uses the same list.
inline bool IsFrontEndSeq(uint32_t seq) { return seq == 75 || seq == 80 || seq == 81; }
inline bool InFrontEnd(const View& v) { return IsFrontEndSeq(v.seq); }
inline bool BoxActive(const View& v) { return v.box_obj && v.box_state >= kBoxOpening && v.box_state <= kBoxClosing; }
inline bool Main(const View& v) { return SettledWhy(v).empty() && v.seq == 75 && v.menu == 1; }  // HOME "Sites" 1: seq 75 and menu 1

// Where the line is: for every FAIL.
inline std::string Where(const View& v) {
  std::string s = "sequence " + std::to_string(v.seq) + ", focused page " + (v.page_readable ? Hex(v.menu_page) : std::string("unreadable")) +
                  ", menu word " + std::to_string(v.menu) + ", list state " + std::to_string(v.list_state);
  if (v.box_obj && v.box_state != 0 && v.box_state != 5) {
    s += "; message box state " + std::to_string(v.box_state) + " id " + std::to_string(v.box_id) + " kind " + std::to_string(v.box_kind);
    if (!v.box_text.empty()) s += " text '" + v.box_text + "'";
  }
  return s;
}

class Home {
 public:
  void Begin(uint64_t frame) { *this = Home(); start_ = last_change_ = frame; begun_ = true; }
  uint32_t presses() const { return presses_; }

  Decision Decide(const View& v) {
    if (!begun_) Begin(v.frame);
    const std::string snap = Snapshot(v);
    if (snap != snap_) {  // a reaction to the press (or any change): the waits restart
      snap_ = snap;
      last_change_ = v.frame;
      if (pending_) noreact_ = 0;
      pending_ = false;
    }
    if (v.frame - start_ > kOverallFrames)
      return Fail(v, "no way home within " + std::to_string(kOverallFrames) + " frames (" + std::to_string(presses_) + " presses)");

    // 1. a message box answers first: it is up over whatever else is going on.
    if (v.box_obj && v.box_state != 0 && v.box_state != 5) {
      if (v.box_state == kBoxOpening || v.box_state == kBoxAnswered || v.box_state == kBoxClosing) return Wait(v, "message box " + std::string(v.box_state == kBoxOpening ? "opening" : v.box_state == kBoxAnswered ? "answered" : "closing"));
      if (v.box_state != kBoxWaiting) return Fail(v, "message box state " + std::to_string(v.box_state) + " is not one of 0 / 1 / 2 / 3 / 4 / 5 (MSGBOX claim 8)");
      return Box(v);
    }
    // 2. done: the settled main menu (and no box)
    if (Main(v)) {
      Decision d;
      d.act = Act::kDone;
      d.log = "done: main menu after " + std::to_string(presses_) + " presses, " + std::to_string(v.frame - start_) + " frames";
      return d;
    }
    // 3. a front end that is not settled yet (leaving a page, a fade, a rebuilt SmartMenu): wait for the game's own state
    if (InFrontEnd(v)) {
      const std::string why = SettledWhy(v);
      if (!why.empty()) return Wait(v, "front end not settled: " + why);
    }
    // 4. press B, one page at a time, only on a frame the focused screen takes input
    if (pending_) {
      if (v.frame - press_frame_ < kReactFrames) return Wait(v, "waiting for the screen to react to B");
      pending_ = false;  // the window ran out with no change
      if (++noreact_ >= kNoReactB)
        return Fail(v, std::to_string(noreact_) + " B presses in a row with no reaction in " + std::to_string(kReactFrames) + " frames each (the screen ignores B)");
    }
    if (!v.ready_readable) return Wait(v, "the focus signal menu_ready is unreadable");
    if (v.menu_ready != 1) return Wait(v, "the screen is not taking input (menu_ready " + std::to_string(v.menu_ready) + ")");
    if (b_presses_ >= kMaxPresses) return Fail(v, std::to_string(kMaxPresses) + " B presses and not home");
    ++b_presses_;
    ++presses_;
    pending_ = true;
    press_frame_ = v.frame;
    Decision d;
    d.act = Act::kPressB;
    d.log = "B on page " + (v.page_readable ? Hex(v.menu_page) : std::string("(unreadable)")) + " (seq " + std::to_string(v.seq) + ")";
    return d;
  }

 private:
  static std::string Snapshot(const View& v) {  // what a press can change
    return std::to_string(v.seq) + "/" + std::to_string(v.sm) + "/" + std::to_string(v.menu) + "/" + std::to_string(v.list_state) + "/" +
           (v.page_readable ? std::to_string(v.menu_page) : "-") + "/" + std::to_string(v.box_state) + "/" + std::to_string(v.box_id) + "/" +
           std::to_string(v.box_cursor);
  }
  Decision Wait(const View& v, const std::string& why) {
    if (!pending_ && v.frame - last_change_ > kStallFrames)
      return Fail(v, "no change in " + std::to_string(kStallFrames) + " frames while waiting: " + why);
    Decision d;
    d.why = why;
    return d;
  }
  Decision Fail(const View& v, const std::string& why) {
    Decision d;
    d.act = Act::kFail;
    d.why = why + "; " + Where(v) + "; after " + std::to_string(presses_) + " presses, " + std::to_string(v.frame - start_) + " frames";
    return d;
  }
  Decision Box(const View& v) {  // state 2: waiting for an answer
    const std::string id = std::to_string(v.box_id), kind = std::to_string(v.box_kind);
    if (pending_) {
      if (v.frame - press_frame_ < kReactFrames) return Wait(v, "waiting for the box to react");
      pending_ = false;
      if (++noreact_ >= kNoReactBox)
        return Fail(v, std::to_string(noreact_) + " box presses in a row with no reaction in " + std::to_string(kReactFrames) + " frames each");
    }
    Act act = Act::kWait;
    std::string log;
    switch (v.box_kind) {
      case 0:
      case 1:  // text and OK: A (MSGBOX claim 5: no cursor)
        act = Act::kPressA;
        log = "box " + id + " kind " + kind + " answered A";
        break;
      case 2:
      case 3:  // YES / NO, opens on NO (claims 6 / 7). YES only for a "leave / discard" id; any other id is not guessed.
        if (!IsLeaveYes(v.box_id)) return Fail(v, "YES / NO box id " + id + " is not in the leave table (kLeaveYes): no answer is guessed");
        if (v.box_cursor > 1) return Fail(v, "YES / NO box cursor " + std::to_string(v.box_cursor) + " is neither 0 (YES) nor 1 (NO)");
        if (v.box_cursor == 1) {
          act = Act::kPressLeft;
          log = "box " + id + " kind " + kind + ": LEFT to YES";
        } else {
          act = Act::kPressA;
          log = "box " + id + " kind " + kind + " answered YES";
        }
        break;
      case 4:
      case 5:
        return Fail(v, "list box id " + id + " kind " + kind + ": no rule for a list box yet");
      case 6:
      case 7:  // no input at all (claim 5): it ends by itself
        return Wait(v, "no-input box kind " + kind);
      default:
        return Fail(v, "message box kind " + kind + " is not one of 0..7 (MSGBOX claim 4)");
    }
    ++presses_;
    pending_ = true;
    press_frame_ = v.frame;
    Decision d;
    d.act = act;
    d.log = log + (v.box_text.empty() ? "" : " (text '" + v.box_text + "')");
    return d;
  }

  bool begun_ = false;
  uint64_t start_ = 0, last_change_ = 0, press_frame_ = 0;
  uint32_t presses_ = 0, b_presses_ = 0, noreact_ = 0;
  bool pending_ = false;
  std::string snap_;
};

}  // namespace wwe2k14::menu_home
