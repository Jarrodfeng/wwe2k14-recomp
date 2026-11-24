// S110 GOTO: the pure part of the menu page goto (no game includes; tools\goto\menu_goto_test.cpp compiles this exact header,
// src\input_script.h includes it for the `page N` line). Source of every address / offset: docs\research\s110\REACH.md and the
// listings named beside each constant. Our own words, no game text.
//
// BY ID: the game's own page-change mailbox (SM+11716..+11728) is armed with a label no record holds (kByIdLabel); the game's
// list update sub_82842C08 then reaches its one call of sub_828426D0 (lr 0x82842CD4) behind ALL of its own tests (+20 focus,
// +40 busy, +8C / +98 code or label, +A4 armed), and our hook on sub_828426D0 (src\menu_goto.h) serves exactly that call with
// the game's own goto sub_828424F0(SM, record id) (the call sub_828426D0 itself makes for a hit), returning 1. The game then
// goes on as for any hit (+D0..+E8): SM+364 != 0 -> sub_82841648(SM, 1) enters the page WITH its gates, back once if asked,
// the four words cleared. Without our hook the label matches nothing: the game finds no record and clears the mailbox.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wwe2k14::menu_goto {

// Mailbox, SmartMenu-relative (sub_82842C08 this = SM+20 reads +11696..+11708; setters sub_8283A4A0 / A498 / A490).
constexpr uint32_t kMbCode = 11716;   // code word (no setter; ctor / reset write 0)
constexpr uint32_t kMbArmed = 11720;  // armed; sub_8283A4A0 stores 1
constexpr uint32_t kMbBack = 11724;   // back once after; sub_8283A498
constexpr uint32_t kMbLabel = 11728;  // label; sub_8283A490
constexpr uint32_t kLabelNone = 0x1869F;  // "no label" (ctor default; sub_82842C08 +98 / +BC)
constexpr uint32_t kByIdLabel = 0xFFFFFFFEu;  // ours: no record's label (Plan refuses when one has it); never 0x1869F
constexpr uint32_t kSmPad = 796;              // SM+796: the pad that pressed this entry (stored at 0x828434C4; gate sub_8283CCF0)
constexpr uint32_t kGamePtr = 0x8376AAF0;     // [[this]+0] = the active profile's pad (read by the same gate; PROFILE.md)
constexpr uint32_t kGotoLr = 0x82842CD4;     // the return address of sub_82842C08's call of sub_828426D0 (listing .182.cpp:10614)

// Flow table, R = sub_8293EAE8(holder, 0) ([holder+20] else [holder+16]), T = R + 4. T+0 record count, T+4 POINTER to the
// 76-byte record array (sub_83424808 +0 `lwz r7,4(r3)` then [T+4] + i*76; sub_834247B8 the same; src\gfx_menu.h:666), T+8 the
// inline default record a failed lookup returns, T+84 index entry count (= [R+88]), T+88 POINTER to the 12-byte index
// {first record u32, record count u16 at +4 (lhz)} by menu id.
constexpr uint32_t kTabRecArray = 4, kTabIdxCount = 84, kTabIdxArray = 88, kIdxStride = 12;
constexpr uint32_t kRecStride = 76;
constexpr uint32_t kRecId = 0;      // sub_834247B8 compares word 0 (the goto's own find by id)
constexpr uint32_t kRecParent = 4;  // sub_828424F0 0x828425EC: the walk goes on with [rec+4] (-1 ends it)
constexpr uint32_t kRecMenu = 8;    // sub_83424808: find by word at +8 ...
constexpr uint32_t kRecItem = 12;   // ... and the halfword AT +12 (`lhz r11,12(r11)`, .270.cpp:30850): the HIGH half of the big-endian word
constexpr uint32_t kRecLabel = 24;  // sub_828426D0 0x82842760 (`lwz r10,24(r3)`)
constexpr uint32_t kRecFlags = 40;  // bit 4 (value 16): the screen code is +44 PLUS +52 (.142.cpp:10716-10719 `lwz r10,52; add r11,r10,r11`)
constexpr uint32_t kRecCode = 44;
constexpr uint32_t kRecCode2 = 52;

// Refused outright (REACH: "No parent chain: menu 5 (1532 / 1533) has no opener; [U] what #1 does").
constexpr uint32_t kRefusedA = 1532, kRefusedB = 1533;

struct Rec {
  uint32_t id = 0xFFFFFFFFu, parent = 0xFFFFFFFFu, menu = 0, item = 0, label = kLabelNone, code = 0;  // code as the game compares it
  bool seen = true;  // the game's (menu, item) lookup sub_83424808 returns this record (MarkSeen)
};

inline uint32_t ResolveCode(uint32_t flags, uint32_t code44, uint32_t code52) { return (flags & 16u) ? code44 + code52 : code44; }

// sub_83424808(T, menu, item): with an index and 0 <= menu < index count it scans only [first, first + count) of that menu's
// entry (an empty range = not found, no fallback); else the whole array. Either way the FIRST record in array order with that
// (menu, item) is the one returned, so a later twin is never seen. recs in array order; ranges[m] = {first, count} (empty = no index).
inline void MarkSeen(std::vector<Rec>& recs, const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  for (size_t i = 0; i < recs.size(); ++i) {
    Rec& r = recs[i];
    size_t lo = 0, hi = recs.size();
    if (r.menu < ranges.size()) lo = ranges[r.menu].first, hi = size_t(ranges[r.menu].first) + ranges[r.menu].second;
    bool seen = i >= lo && i < hi;
    for (size_t j = lo; seen && j < i; ++j)
      if (recs[j].menu == r.menu && recs[j].item == r.item) seen = false;
    r.seen = seen;
  }
}

// The table read through `rd(guest address) -> u32` (0 for an unreadable address). False when the holder or the table is not there.
template <class Rd>
inline bool ReadTableWith(Rd rd, uint32_t holder, std::vector<Rec>& recs, uint32_t& menu_count) {
  if (!holder) return false;
  const uint32_t w20 = rd(holder + 20);
  const uint32_t r = w20 ? w20 : rd(holder + 16);
  if (!r) return false;
  const uint32_t t = r + 4, n = rd(t), arr = rd(t + kTabRecArray), idx_n = rd(t + kTabIdxCount), idx = rd(t + kTabIdxArray);
  menu_count = rd(r + 88);  // sub_828426D0 0x82842708 `lwz r25,88(r3)` (== T+84)
  if (!arr || n == 0 || n > 4096 || menu_count == 0 || menu_count > 4096) return false;
  recs.assign(n, Rec{});
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t a = arr + i * kRecStride;
    Rec& x = recs[i];
    x.id = rd(a + kRecId);
    x.parent = rd(a + kRecParent);
    x.menu = rd(a + kRecMenu);
    x.item = rd(a + kRecItem) >> 16;
    x.label = rd(a + kRecLabel);
    x.code = ResolveCode(rd(a + kRecFlags), rd(a + kRecCode), rd(a + kRecCode2));
  }
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  if (idx && idx_n <= 4096)
    for (uint32_t m = 0; m < idx_n; ++m) ranges.emplace_back(rd(idx + m * kIdxStride), rd(idx + m * kIdxStride + 4) >> 16);
  MarkSeen(recs, ranges);
  return true;
}

// The mailbox words a by-id request stores (the record id itself goes to the hook, not to the mailbox).
struct Words {
  uint32_t code = 0, label = kByIdLabel;
};

inline int64_t FindRec(const std::vector<Rec>& recs, uint32_t id) {
  for (size_t i = 0; i < recs.size(); ++i)
    if (recs[i].id == id) return int64_t(i);
  return -1;
}

// Empty return = the game's goto can be asked for record `id` (w set); else the refusal reason (logged loudly by the caller).
// sub_828424F0 walks the parent chain and leaves the cursor on each row through the (menu, item) lookup, and the list update
// enters the target only when SM+364 != 0 after the walk (sub_82842C08 +D4; else cursor 0, nothing entered).
inline std::string Plan(const std::vector<Rec>& recs, uint32_t id, Words& w) {
  const std::string rec = "record " + std::to_string(id);
  if (id == kRefusedA || id == kRefusedB)
    return rec + " is menu 5's, which has no opener: what the game's goto does with it is unconfirmed [U], refused";
  const int64_t want = FindRec(recs, id);
  if (want < 0) return rec + " is not in the flow table (" + std::to_string(recs.size()) + " records)";
  for (const Rec& x : recs)
    if (x.label == kByIdLabel) return "record " + std::to_string(x.id) + " holds our by-id label 0xFFFFFFFE: the mailbox cannot be armed safely";
  const Rec& r = recs[size_t(want)];
  if (r.parent == 0xFFFFFFFFu) return rec + " is a top-level row (no parent): the game's goto walks to it but enters nothing (sub_82842C08 +D4)";
  for (int64_t k = want, steps = 0; k >= 0; ++steps) {  // the target and every parent must be what the (menu, item) lookup returns
    const Rec& x = recs[size_t(k)];
    if (!x.seen)
      return rec + ": record " + std::to_string(x.id) + " (menu " + std::to_string(x.menu) + " item " + std::to_string(x.item) +
             ") is never returned by the game's (menu, item) lookup sub_83424808 (an earlier record there, or outside its menu's index range)";
    if (x.parent == 0xFFFFFFFFu) break;
    k = FindRec(recs, x.parent);
    if (k < 0 || steps > int64_t(recs.size()))
      return rec + "'s parent chain breaks at id " + std::to_string(x.parent) + " (not in the table, or a loop)";
  }
  w = Words{};
  return "";
}

// The hook's whole decision (src\menu_goto.h REX_HOOK_RAW(sub_828426D0)): serve this call with our record only when a request of
// ours is armed AND it is the list update's own call (lr) on the SmartMenu with exactly our mailbox words (code 0, our label).
// Any other call (the re-entry method sub_82842868, the Universe handler's label, a player's play) runs the game's function unchanged.
inline bool ServesOurCall(bool armed, uint32_t lr, uint32_t r3, uint32_t sm, uint32_t r4, uint32_t r5) {
  return armed && lr == kGotoLr && sm != 0 && r3 == sm && r4 == 0 && r5 == kByIdLabel;
}

// One listing line's tail: `enterable yes` / `enterable no(REASON)`.
inline std::string Enterable(const std::vector<Rec>& recs, uint32_t id) {
  Words w;
  const std::string why = Plan(recs, id, w);
  return why.empty() ? "enterable yes" : "enterable no(" + why + ")";
}

// `rec=N [back]` -> id / back. Empty = ok.
inline std::string ParseArgs(const std::string& args, uint32_t& id, bool& back) {
  id = 0xFFFFFFFFu;
  back = false;
  size_t p = 0;
  while (p < args.size()) {
    while (p < args.size() && (args[p] == ' ' || args[p] == '\t')) ++p;
    size_t q = p;
    while (q < args.size() && args[q] != ' ' && args[q] != '\t') ++q;
    const std::string w = args.substr(p, q - p);
    p = q;
    if (w.empty()) continue;
    if (w == "back") { back = true; continue; }
    if (w.rfind("rec=", 0) == 0 && w.size() > 4 && w.size() <= 10) {
      uint64_t v = 0;
      for (size_t i = 4; i < w.size(); ++i) {
        if (w[i] < '0' || w[i] > '9') return "error: rec=N takes digits only: '" + w + "'";
        v = v * 10 + uint64_t(w[i] - '0');
      }
      if (v >= 0xFFFFFFFFull) return "error: rec=" + std::to_string(v) + " is out of range";
      id = uint32_t(v);
      continue;
    }
    return "error: !page takes rec=N [back], not '" + w + "'";
  }
  if (id == 0xFFFFFFFFu) return "error: !page needs rec=N";
  return "";
}

// The input script's `page N [back]` (the text after "page"). Empty = ok.
inline std::string ParseScriptLine(const std::string& rest, uint32_t& id, bool& back) {
  const size_t p = rest.find_first_not_of(" \t");
  if (p == std::string::npos) return "page needs a record id: page N [back]";
  const std::string bad = ParseArgs("rec=" + rest.substr(p), id, back);
  return bad.empty() ? "" : "page N [back]: " + bad;
}

// S110 BACK (docs/research/s110/BACK.md): a request made while the front end is not ready WAITS for the game's own menu. Ready = SM != 0,
// sequence 75 / 80 / 81, the flow table read, holder state 2, focus gate open, SM+36 == 2 (the only state that serves the mailbox),
// and the mailbox not armed. Returns "" when a request can be armed, else the reason. Pure: the caller reads every value each frame.
constexpr uint32_t kSmListState = 36;  // SM+36: list state 0 re-entry, 1 fade, 2 serves requests (BACK.md claim 5)
constexpr uint32_t kListServes = 2;
// Moved here from menu_goto.h (S110 HOMESTEP: src/menu_home.h reads the same words and must not include the hook file).
constexpr uint32_t kHolderPtr = 0x839294DC;  // the flow table holder (word; sub_828426D0 / sub_828424F0 read it)
constexpr uint32_t kSmLockPtr = 436;         // sub_82842C08 +10..+20 (this = SM+20): [[SM+436]+56] must be 0
constexpr uint32_t kSmMenu = 364;            // SM+364 = the menu word (menus.md "Two layers")
constexpr uint32_t kHolderState = 24;        // sub_8293EAC0(holder, 0) = [holder+24]; 2 = loaded (REACH claim 7)
// The bound of the wait: 600 frames (10 s at 60 fps). B -> the screen's own leave (Threads: states 10 / 14 / 12 / 13 / 16) -> sequence 77 -> 4
// -> 75 -> new SmartMenu -> list state 0 -> 1 -> 2 is a few seconds with fades and loads; a leave that never ends (BACK.md "Not left by
// one B") must still fail, not hang the script.
constexpr uint32_t kReturnFrames = 600;
// `menu` = SM+364. S110 BACK "The 2 -> 1 -> 2 bounce" [C]: a fresh SmartMenu reaches list state 2 while still on the root page
// (SM+364 == 0, the game's own "no page yet" test in sub_82842C08 +204..+20C); its first list update enters the start page and
// that entry stores SM+36 = 1 for the fade (sub_8283E550 +EC), then 2 again. The mailbox is handled before that root entry,
// so a request armed at the first 2 is served from the root and goes nowhere (run 11). Settled = state 2 AND a page is open.
inline std::string ReadyWhy(uint32_t sm, uint32_t seq, bool table, uint32_t holder_state, uint32_t lock56, uint32_t list_state, bool armed,
                            uint32_t menu) {
  if (!sm) return "no SmartMenu (the game is inside a screen or on its way back to the menu)";
  if (seq != 75 && seq != 80 && seq != 81) return "no front end is running (sequence " + std::to_string(seq) + ")";
  if (!table) return "the flow table is not readable";
  if (holder_state != 2) return "the flow table holder is not loaded (state " + std::to_string(holder_state) + ")";
  if (lock56 != 0) return "the focus gate is closed";
  if (list_state != kListServes) return "the list is not serving yet (list state " + std::to_string(list_state) + ", needs 2)";
  if (menu == 0) return "the list is still on the root (menu 0): its start page is not entered yet";
  if (armed) return "the mailbox is still armed";
  return "";
}

constexpr uint64_t kScriptFrames = 900;  // a script line's own bound: the game side ends a request within kReturnFrames + kWatchFrames
constexpr uint32_t kWatchFrames = 240;   // the page change is watched this long after arming (menu_goto.h uses this one)
static_assert(kScriptFrames >= kReturnFrames + kWatchFrames, "a script line must outlast the wait and the watch");

// ---- The request port (std only): the live channel and the input script hand a record id to the game thread here. ----
// One request at a time. The game thread answers twice: Answer() when it armed (or refused) the request, Finish() when the
// game served it (1) or it failed (-1: refused, not served, timed out). A script waits for Finish of ITS ticket.
inline std::mutex g_mu;
inline std::atomic<bool> g_pending{false};
inline uint32_t g_pending_id = 0;  // under g_mu
inline bool g_pending_back = false;
inline uint64_t g_ticket = 0, g_pending_ticket = 0;
inline std::string g_answer;
inline bool g_answered = false;
inline uint64_t g_outcome_ticket = 0;
inline int g_outcome = 0;  // for g_outcome_ticket: 1 served, -1 failed
inline std::string g_outcome_text;

// Stores the request. "" or the reason it cannot be taken; ticket = this request's number.
inline std::string Request(uint32_t record_id, bool back, uint64_t& ticket) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_pending.load()) return "error: a page request is already waiting for the game";
  g_pending_id = record_id;
  g_pending_back = back;
  g_pending_ticket = ticket = ++g_ticket;
  g_answered = false;
  g_answer.clear();
  g_pending.store(true);
  return "";
}

// Game thread: takes the waiting request (false when none).
inline bool Take(uint32_t& id, bool& back, uint64_t& ticket) {
  if (!g_pending.exchange(false)) return false;
  std::lock_guard<std::mutex> lock(g_mu);
  id = g_pending_id, back = g_pending_back, ticket = g_pending_ticket;
  return true;
}

// A request taken back before the game thread took it (the live channel's own timeout). True when it was still waiting.
inline bool Withdraw() { return g_pending.exchange(false); }

inline std::condition_variable g_cv;  // SetAnswer wakes the live channel's wait

inline void SetAnswer(const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_answer = text;
    g_answered = true;
  }
  g_cv.notify_all();
}

inline void Finish(uint64_t ticket, int outcome, const std::string& text) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_outcome_ticket = ticket, g_outcome = outcome, g_outcome_text = text;
}

// 0 = not finished yet; 1 served; -1 failed (text = why).
inline int Poll(uint64_t ticket, std::string& text) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_outcome_ticket != ticket) return 0;
  text = g_outcome_text;
  return g_outcome;
}

}  // namespace wwe2k14::menu_goto
