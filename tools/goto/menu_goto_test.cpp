// S110 GOTO: host unit for src/menu_goto_core.h (the table read, the by-id request rules and refusals, the hook's decision, the
// argument parses, the request port). Synthetic tables only (no game data). Prints "ok N checks" and returns 0, else the failures and 1.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "menu_goto_core.h"

using namespace wwe2k14::menu_goto;

static int g_n = 0;
static bool Check(bool c, const char* what) {
  ++g_n;
  if (!c) std::printf("FAIL: %s\n", what);
  return c;
}

static Rec R(uint32_t id, uint32_t menu, uint32_t item, uint32_t label, uint32_t code, uint32_t parent = 1) {
  Rec r;
  r.id = id, r.parent = parent, r.menu = menu, r.item = item, r.label = label, r.code = code;
  return r;
}

// A synthetic guest memory: word address -> value (big-endian words as the game holds them; 0 elsewhere).
struct Mem {
  std::map<uint32_t, uint32_t> w;
  uint32_t operator()(uint32_t a) const { auto it = w.find(a); return it == w.end() ? 0 : it->second; }
};

// The game's side of one served mailbox, from the listings: sub_82842C08 +8C..+C8 calls sub_828426D0(SM, code, label) at lr kGotoLr;
// our hook answers it when ServesOurCall says so (the record id handed to sub_828424F0); else sub_828426D0's own scan runs: menus
// 0..count-1, items 0..19, the record the (menu, item) lookup returns, first whose code == code (code > 0) or label == label.
// Returns the record id the game's goto is given, or -1 (nothing found: the mailbox is cleared, cursor 0).
static int64_t GameServes(const std::vector<Rec>& recs, uint32_t menu_count, uint32_t sm, const Words& w, bool armed, uint32_t hook_id) {
  if (w.code == 0 && w.label == kLabelNone) return -1;  // +8C / +98: nothing asked
  if (ServesOurCall(armed, kGotoLr, sm, sm, w.code, w.label)) return hook_id;
  for (uint32_t m = 0; m < menu_count; ++m)
    for (uint32_t i = 0; i < 20; ++i)
      for (const Rec& r : recs)
        if (r.seen && r.menu == m && r.item == i && r.id != 0xFFFFFFFFu) {
          if ((w.code != 0 && r.code == w.code) || (w.label != kLabelNone && r.label == w.label)) return r.id;
          break;  // the lookup returns one record per (menu, item)
        }
  return -1;
}

static bool LayoutChecks() {
  // holder H: [H+20] = 0, [H+16] = R; T = R+4: count 3, [T+4] = the record ARRAY at A (not inline), [T+84] 2 index entries, [T+88] index at X.
  const uint32_t H = 0x40000000, Rr = 0x40001000, T = Rr + 4, A = 0x40002000, X = 0x40003000;
  Mem m;
  m.w[H + 16] = Rr;
  m.w[T] = 3, m.w[T + 4] = A, m.w[T + 84] = 2, m.w[T + 88] = X, m.w[Rr + 88] = 2;
  m.w[T + 8] = 0xFFFFFFFFu;  // the inline default record (what an inline reader would take for record 0's id: A)
  auto rec = [&](uint32_t i, uint32_t id, uint32_t parent, uint32_t menu, uint32_t w12, uint32_t label, uint32_t flags, uint32_t c44, uint32_t c52) {
    const uint32_t a = A + i * 76;
    m.w[a] = id, m.w[a + 4] = parent, m.w[a + 8] = menu, m.w[a + 12] = w12, m.w[a + 24] = label, m.w[a + 40] = flags, m.w[a + 44] = c44, m.w[a + 52] = c52;
  };
  rec(0, 100, 0xFFFFFFFFu, 0, 0x00000007u, 600, 0, 1, 0);  // menu 0 item 0: low half 7 is NOT the item (lhz +12 = high half 0)
  rec(1, 101, 100, 1, 0x00020000u, 601, 16, 2000, 5);       // menu 1 item 2; screen code 2000 + type 5 = 2005
  rec(2, 102, 100, 1, 0x00020000u, 602, 16, 1300, 0);       // a second record at menu 1 item 2: the game's lookup never returns it
  m.w[X] = 0, m.w[X + 4] = 1u << 16;                          // menu 0: records [0, 1)
  m.w[X + 12] = 1, m.w[X + 16] = 2u << 16;                    // menu 1: records [1, 3)
  std::vector<Rec> recs;
  uint32_t mc = 0;
  bool ok = Check(ReadTableWith(m, H, recs, mc) && recs.size() == 3 && mc == 2, "the table is read through the [T+4] record pointer");
  if (!ok) return false;
  ok &= Check(recs[0].id == 100 && recs[1].id == 101, "record ids come from the array, not from T+4 inline");
  ok &= Check(recs[0].item == 0 && recs[1].item == 2, "item = the halfword at +12 (the high half of the big-endian word)");
  ok &= Check(recs[1].code == 2005 && recs[0].code == 1, "flags bit 4: the code is +44 plus +52");
  ok &= Check(recs[1].parent == 100, "parent = word +4");
  ok &= Check(recs[1].seen && !recs[2].seen, "a later record at the same (menu, item) is not seen by the game's lookup");
  Words w;
  ok &= Check(Plan(recs, 101, w).empty() && w.code == 0 && w.label == kByIdLabel, "a match setup row is asked for by id");
  ok &= Check(Plan(recs, 102, w).find("never returned") != std::string::npos, "the unseen twin is refused (the cursor walk would land on the other)");
  ok &= Check(Plan(recs, 100, w).find("top-level") != std::string::npos, "a top-level row is refused (the mailbox enters nothing)");
  m.w[X + 16] = 1u << 16;  // menu 1's range now [1, 2): record 2 lies outside it
  ok &= Check(ReadTableWith(m, H, recs, mc) && !recs[2].seen && recs[1].seen, "a record outside its menu's index range is not seen");
  return ok;
}

static bool ByIdChecks() {
  // Record 1 is the root row. Menu 1 holds a label twin of menu 2 (like PLAY / MATCH RULES): label 500 at 10 (menu 1) and 20 (menu 2),
  // and record 21 shares screen code 1300 with record 11: neither 20 nor 21 is the first match of its label / code.
  const std::vector<Rec> t = {R(10, 1, 0, 500, 0), R(11, 1, 1, 501, 1300), R(20, 2, 0, 500, 0), R(21, 2, 1, 502, 1300),
                              R(1, 0, 0, 400, 0, 0xFFFFFFFFu)};
  const uint32_t sm = 0x50000000;
  bool ok = true;
  for (uint32_t id : {20u, 21u, 10u, 11u}) {
    Words w;
    const bool planned = Plan(t, id, w).empty();
    ok &= Check(planned, "every record with a parent chain is enterable by id");
    ok &= Check(planned && GameServes(t, 61, sm, w, true, id) == int64_t(id), "the game's goto is handed exactly the asked record (not the first match)");
    ok &= Check(planned && GameServes(t, 61, sm, w, false, id) == -1, "with the hook not armed, our mailbox words open nothing (no record holds the by-id label)");
  }
  ok &= Check(!ServesOurCall(true, kGotoLr + 4, sm, sm, 0, kByIdLabel), "another call site (lr) is never served");
  ok &= Check(!ServesOurCall(true, kGotoLr, sm, sm, 0, 41763), "the game's own label request (Universe 41763) runs unchanged");
  ok &= Check(!ServesOurCall(true, kGotoLr, sm + 20, sm, 0, kByIdLabel), "a call on another object runs unchanged");
  ok &= Check(!ServesOurCall(false, kGotoLr, sm, sm, 0, kByIdLabel), "no armed request: the hook is inert");
  ok &= Check(!ServesOurCall(true, kGotoLr, 0, 0, 0, kByIdLabel), "no SmartMenu: the hook is inert");
  std::vector<Rec> clash = t;
  clash.push_back(R(30, 3, 0, kByIdLabel, 0));
  Words w;
  ok &= Check(Plan(clash, 20, w).find("by-id label") != std::string::npos, "a table holding our label is refused (the words could open that record)");
  std::vector<Rec> chain = t;
  chain.push_back(R(50, 4, 0, 950, 0, 777));
  ok &= Check(Plan(chain, 50, w).find("parent chain breaks") != std::string::npos, "a parent id missing from the table is refused");
  std::vector<Rec> twin = t;
  twin.push_back(R(60, 5, 3, 960, 0, 61));
  twin.push_back(R(61, 1, 0, 961, 0, 1));  // a second record at menu 1 item 0: record 60's parent is never the one the walk enters
  MarkSeen(twin, {});
  ok &= Check(Plan(twin, 60, w).find("record 61") != std::string::npos, "a parent the (menu, item) lookup never returns is refused");
  ok &= Check(Plan(t, 999, w).find("not in the flow table") != std::string::npos, "an id not in the table is refused");
  std::vector<Rec> five = t;
  five.push_back(R(1532, 5, 0, 700, 1400));
  ok &= Check(Plan(five, 1532, w).find("[U]") != std::string::npos && Plan(five, 1533, w).find("[U]") != std::string::npos, "menu 5 records 1532 / 1533 are refused");
  return ok;
}

static bool PortChecks() {
  uint64_t a = 0, b = 0;
  bool ok = Check(Request(20, true, a).empty() && a != 0, "a request is taken");
  ok &= Check(!Request(21, false, b).empty(), "a second request while one waits is refused (busy)");
  uint32_t id = 0;
  bool back = false;
  uint64_t t = 0;
  ok &= Check(Take(id, back, t) && id == 20 && back && t == a, "the game thread takes it with its ticket");
  std::string text;
  ok &= Check(Poll(a, text) == 0, "no outcome before the game finishes it");
  Finish(a, -1, "REFUSED: x");
  ok &= Check(Poll(a, text) == -1 && text == "REFUSED: x", "a refusal reaches the script line");
  ok &= Check(Request(22, false, b).empty() && b != a && Poll(b, text) == 0, "a new request does not see the old outcome");
  ok &= Check(Withdraw() && !Withdraw(), "a waiting request can be withdrawn once");
  return ok;
}

static bool ReadyChecks() {
  // args: sm, seq, table, holder_state, lock56, list_state, armed, menu (SM+364; 0 = still on the root page)
  bool ok = Check(ReadyWhy(0x1000, 75, true, 2, 0, 2, false, 1).empty(), "ready: SM, seq 75, state 2, on a page");
  ok &= Check(ReadyWhy(0x1000, 80, true, 2, 0, 2, false, 1).empty() && ReadyWhy(0x1000, 81, true, 2, 0, 2, false, 3).empty(), "ready on seq 80 / 81");
  ok &= Check(!ReadyWhy(0, 75, true, 2, 0, 2, false, 1).empty(), "not ready: no SmartMenu");
  ok &= Check(!ReadyWhy(0x1000, 77, true, 2, 0, 2, false, 1).empty(), "not ready: sequence 77 (Threads)");
  ok &= Check(!ReadyWhy(0x1000, 75, false, 2, 0, 2, false, 1).empty(), "not ready: no flow table");
  ok &= Check(!ReadyWhy(0x1000, 75, true, 1, 0, 2, false, 1).empty(), "not ready: holder not loaded");
  ok &= Check(!ReadyWhy(0x1000, 75, true, 2, 1, 2, false, 1).empty(), "not ready: focus gate closed");
  ok &= Check(!ReadyWhy(0x1000, 75, true, 2, 0, 0, false, 1).empty() && !ReadyWhy(0x1000, 75, true, 2, 0, 1, false, 1).empty(), "not ready: list state 0 / 1");
  ok &= Check(!ReadyWhy(0x1000, 75, true, 2, 0, 2, true, 1).empty(), "not ready: mailbox armed");
  // S110 run 11: a fresh SmartMenu reaches list state 2 for one frame while still on the root page (menu 0); its first list
  // update then enters the start page and drops back to 1. A request armed there was served from the root and went nowhere.
  ok &= Check(!ReadyWhy(0x1000, 75, true, 2, 0, 2, false, 0).empty(), "not ready: list state 2 but still on the root (menu 0)");
  ok &= Check(ReadyWhy(0x1000, 75, true, 2, 0, 2, false, 0).find("root") != std::string::npos, "the root reason says root");
  // B press: SM goes 0 -> a NEW SmartMenu at another address; each frame reads it again and the state climbs 0 -> 1 -> 2.
  ok &= Check(!ReadyWhy(0, 4, false, 0, 0, 0, false, 0).empty() && !ReadyWhy(0x2000, 75, true, 2, 0, 0, false, 0).empty() &&
                  !ReadyWhy(0x3000, 75, true, 2, 0, 1, false, 1).empty() && ReadyWhy(0x3000, 75, true, 2, 0, 2, false, 1).empty(),
              "a changed SM between frames is judged on its own words");
  ok &= Check(ReadyWhy(0x1000, 77, true, 2, 0, 2, false, 1).find("77") != std::string::npos, "the sequence reason names the sequence");
  ok &= Check(kScriptFrames >= kReturnFrames + kWatchFrames, "the script bound outlasts the wait and the watch");
  return ok;
}

int main() {
  bool ok = ReadyChecks();
  ok &= LayoutChecks();
  ok &= ByIdChecks();
  ok &= PortChecks();
  uint32_t id = 0;
  bool back = true;
  ok &= Check(ParseArgs(" rec=1300 ", id, back).empty() && id == 1300 && !back, "rec=N parses");
  ok &= Check(ParseArgs("rec=7 back", id, back).empty() && id == 7 && back, "rec=N back parses");
  ok &= Check(!ParseArgs("", id, back).empty(), "an empty request is an error");
  ok &= Check(!ParseArgs("rec=", id, back).empty(), "rec= with no digits is an error");
  ok &= Check(!ParseArgs("rec=12x", id, back).empty(), "letters after the digits are an error");
  ok &= Check(!ParseArgs("rec=5 now", id, back).empty(), "an unknown word is an error");
  ok &= Check(!ParseArgs("rec=99999999999", id, back).empty(), "an over-long number is an error");
  ok &= Check(ParseScriptLine(" 7168", id, back).empty() && id == 7168 && !back, "the script line `page 7168` parses");
  ok &= Check(ParseScriptLine("40 back", id, back).empty() && id == 40 && back, "the script line `page 40 back` parses");
  ok &= Check(!ParseScriptLine("", id, back).empty() && !ParseScriptLine("x", id, back).empty(), "a page line with no id / no digits is an error");
  if (!ok) return 1;
  std::printf("ok %d checks\n", g_n);
  return 0;
}
