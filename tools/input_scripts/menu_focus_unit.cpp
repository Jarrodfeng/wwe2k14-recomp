// Session 108 lane MENU: unit for src/menu_focus.h (the built-in route states menu_*: which recorded input mixin is the
// focused one, and the value of each state). Pure: planted guest words in a map, no game, no SDK.
//   menu_focus_unit                 run every case (exit 0 = all pass)
//   menu_focus_unit --plant NAME    NAME = pick_first | no_owner | stale_frame | tie_silent: the function under test is
//                                   swapped for the old / wrong rule; exit 0 only when the checks SEE it (a case fails)
// Production order driven: Tracker::Note (the hook's record, in guest-frame order) -> Choose -> Read (what the
// GuestState host reader calls). src/game_state.h itself needs the SDK headers, so ReadGuestState / StateMatches are
// covered by tools\compile\game_syntax.py only; the state name table is checked here.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../../src/menu_focus.h"

using namespace wwe2k14::menu_focus;

static int g_fail = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++g_fail;                                        \
      std::printf("FAIL line %d: ", __LINE__);         \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static std::map<uint32_t, uint32_t> g_mem;
static uint32_t rd(uint32_t a) {
  auto it = g_mem.find(a);
  return it == g_mem.end() ? 0u : it->second;
}
static void put(uint32_t a, uint32_t v) { g_mem[a] = v; }

static std::string g_plant;

// The planted wrong rules (what the plan's twins stand for), reading the same recorded mixins.
static bool ReadUnderTest(const Tracker& t, Field f, uint64_t now, uint32_t* out) {
  if (g_plant.empty()) return t.Read(f, now, rd, out);
  const MixinRec* pick = nullptr;
  uint32_t n = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    const MixinRec& r = t.at(i);
    if (g_plant != "stale_frame" && r.frame + 1 < now) continue;  // stale_frame: no liveness check
    if (g_plant == "tie_silent" || g_plant == "no_owner") {
      // the right pick (production Choose), except where the plant changes the outcome
    }
    ++n;
    if (!pick) pick = &r;
    if (g_plant == "stale_frame" && r.frame >= pick->frame) pick = &r;
  }
  if (g_plant == "pick_first") {
    if (f == kCount) return *out = n, true;
    if (f == kReady) return *out = pick ? 1u : 0u, true;
    if (!pick) return false;
    if (f == kPage) return *out = rd(pick->m), true;
    if (f == kRow) return *out = rd(pick->m + 20), true;
    if (f == kRows) return *out = rd(pick->m + 24), true;
    if (f == kCol) return *out = rd(pick->m + 36), true;
    return *out = rd(pick->m + 40), true;
  }
  if (g_plant == "stale_frame") {
    if (f == kReady) return *out = pick ? 1u : 0u, true;
    if (!pick) return false;
    if (f == kRow) return *out = rd(pick->m + 20), true;
    if (f == kPage) return *out = rd(pick->m), true;
    return t.Read(f, now, rd, out);
  }
  const Pick p = t.Choose(now);
  if (g_plant == "tie_silent") {
    if (f == kReady) return *out = (p.rec || p.tied) ? 1u : 0u, true;
    const MixinRec* r = p.rec;
    if (!r && p.tied) {  // silently resolved: the first live one
      for (size_t i = 0; i < t.size(); ++i)
        if (t.at(i).frame + 1 >= now) {
          r = &t.at(i);
          break;
        }
    }
    if (!r) return t.Read(f, now, rd, out);
    if (f == kRow) return *out = rd(r->m + 20), true;
    return t.Read(f, now, rd, out);
  }
  if (g_plant == "no_owner") {
    if (!p.rec) return t.Read(f, now, rd, out);
    if (f == kRow) return *out = rd(p.rec->m + 20), true;  // the SmartMenu's mixin row word stays 0
    if (f == kRows) return *out = rd(p.rec->m + 24), true;
    return t.Read(f, now, rd, out);
  }
  return t.Read(f, now, rd, out);
}

static bool Get(const Tracker& t, Field f, uint64_t now, uint32_t* out) { return ReadUnderTest(t, f, now, out); }
static bool Is(const Tracker& t, Field f, uint64_t now, uint32_t want) {
  uint32_t v = 0;
  return Get(t, f, now, &v) && v == want;
}
static bool Unreadable(const Tracker& t, Field f, uint64_t now) {
  uint32_t v = 0;
  return !Get(t, f, now, &v);
}

static void plant_mixin(uint32_t m, uint32_t vt, int row, int rows, int col, int cols) {
  put(m, vt), put(m + 8, 1), put(m + 20, uint32_t(row)), put(m + 24, uint32_t(rows)), put(m + 36, uint32_t(col)),
      put(m + 40, uint32_t(cols));
}

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "--plant") g_plant = argv[2];

  // 1. A Threads colour page: the mixin sits at page+28, vtable 0x821BAA68, row 2 of 4.
  {
    Tracker t;
    const uint32_t page = 0x40000000u, m = page + 28;
    plant_mixin(m, 0x821BAA68u, 2, 4, 0, 0);
    t.Note(m, 10, rd);
    CHECK(Is(t, kReady, 10, 1), "colour page: menu_ready 1");
    CHECK(Is(t, kPage, 10, 0x821BAA68u), "colour page: menu_page");
    CHECK(Is(t, kRow, 10, 2) && Is(t, kRows, 10, 4), "colour page: row 2 of 4");
    CHECK(Is(t, kCount, 10, 1), "colour page: menu_count 1");
    put(m + 20, 3);  // a press moves the row; the value is read live, no new dispatch needed
    CHECK(Is(t, kRow, 10, 3), "colour page: live row after the move");
  }

  // 2. A container and two sub-pages dispatch in the same frames: only the roster's words change.
  {
    Tracker t;
    const uint32_t box = 0x41000000u, attire = 0x41001000u, roster = 0x41002000u;
    plant_mixin(box, 0x821BAE28u, 0, 0, 0, 0);
    plant_mixin(attire, 0x821BDBA8u, 0, 2, 0, 4);
    // the roster HAS generic counts: sub_833D6FC8(page) writes page+52 = M+24 = 3 rows and page+68 = M+40 = n columns
    // (called from sub_833D8050 and the roster's post-move slot 24 sub_833D7170); its M+908 column page is extra
    plant_mixin(roster, 0x821BD670u, 0, 3, 0, 6);
    for (uint64_t f = 20; f <= 22; ++f) {
      t.Note(box, f, rd), t.Note(attire, f, rd), t.Note(roster, f, rd);
    }
    // nothing moved yet: two sub-pages with a cursor, both first seen in frame 20: a tie, reported, not resolved
    CHECK(Unreadable(t, kPage, 22), "container + sub-pages (idle): two fresh cursors tie: menu_page unreadable");
    CHECK(Is(t, kReady, 22, 1), "container + sub-pages (idle): mixins passed the input gates: menu_ready 1");
    put(roster + 20, 1);  // the roster moves at frame 23
    for (uint64_t f = 23; f <= 25; ++f) {
      t.Note(box, f, rd), t.Note(attire, f, rd), t.Note(roster, f, rd);
    }
    CHECK(Is(t, kPage, 25, 0x821BD670u), "container + sub-pages: the roster, the one whose words changed, is focused");
    CHECK(Is(t, kRow, 25, 1), "roster row 1");
    CHECK(Is(t, kCount, 25, 3), "three mixins dispatched in the newest frame (menu_count 3: the rule picked, not the count)");
  }

  // 3. The SmartMenu: the mixin at SM+48 keeps its row words 0, the cursor is SM+352.
  {
    Tracker t;
    const uint32_t sm = 0x42000000u, m = sm + 48;
    plant_mixin(m, kSmartMenuVtable, 0, 0, 0, 0);
    put(sm + 352, 3);
    t.Note(m, 30, rd);
    CHECK(Is(t, kRow, 30, 3), "SmartMenu: menu_row is [SM+352]");
    CHECK(Is(t, kCol, 30, 0) && Is(t, kCols, 30, 1), "SmartMenu: column 0 of 1");
    CHECK(Unreadable(t, kRows, 30), "SmartMenu: row count unreadable");
    put(sm + 352, 5);
    CHECK(Is(t, kRow, 30, 5), "SmartMenu: live cursor");
  }

  // 4. A UI list widget: its mixin is standalone at [list+924]; the list keeps [list+88] / [list+612].
  {
    Tracker t;
    const uint32_t list = 0x43000000u, m = 0x43100000u;
    put(list, kListVtable), put(list + 88, 5), put(list + 612, 9), put(list + 924, m);
    plant_mixin(m, 0x82070000u, 0, 0, 0, 0);
    t.NoteList(list, rd(list + 924), 40);
    t.Note(m, 40, rd);
    CHECK(Is(t, kRow, 40, 5) && Is(t, kRows, 40, 9), "UI list: item 5 of 9 from the list");
    CHECK(Is(t, kCols, 40, 1), "UI list: one column");
  }

  // 5. A mixin that stopped dispatching is not focused: live = this or the previous guest frame.
  {
    Tracker t;
    const uint32_t m = 0x44000000u;
    plant_mixin(m, 0x821BA588u, 1, 3, 0, 0);
    t.Note(m, 50, rd);
    CHECK(Is(t, kReady, 51, 1), "previous frame still live");
    CHECK(Is(t, kReady, 52, 0), "two frames old: menu_ready 0");
    CHECK(Unreadable(t, kRow, 52), "two frames old: menu_row unreadable");
    CHECK(Is(t, kCount, 52, 0), "two frames old: menu_count 0");
  }

  // 6. Two cursors that never moved, both first seen in the same frame: a tie is reported, never resolved.
  {
    Tracker t;
    const uint32_t a = 0x45000000u, b = 0x45001000u;
    plant_mixin(a, 0x821BA588u, 0, 3, 0, 0);
    plant_mixin(b, 0x821BAC90u, 0, 5, 0, 0);
    t.Note(a, 60, rd), t.Note(b, 60, rd);
    CHECK(Is(t, kCount, 60, 2), "tie: menu_count 2");
    CHECK(Is(t, kReady, 60, 1), "tie: menu_ready 1 (both passed the input gates: a press now is taken)");
    CHECK(Unreadable(t, kRow, 60), "tie: menu_row unreadable");
    CHECK(t.Choose(60).tied == 2, "tie: counted as 2 tied");
    put(b + 20, 1);  // b moves: now it is the focused one
    t.Note(a, 61, rd), t.Note(b, 61, rd);
    CHECK(Is(t, kPage, 61, 0x821BAC90u), "tie broken by the cursor that moved");
  }

  // 7. A page replaced at the same address (new vtable) restarts its history; a page skipped for long does too.
  {
    Tracker t;
    const uint32_t m = 0x46000000u;
    plant_mixin(m, 0x821BA588u, 0, 3, 0, 0);
    t.Note(m, 70, rd);
    put(m + 20, 2);
    t.Note(m, 71, rd);
    CHECK(t.at(0).change_frame == 71 && t.at(0).first_frame == 70, "history: change stamped at the move");
    plant_mixin(m, 0x821BAC90u, 0, 3, 0, 0);
    t.Note(m, 72, rd);
    CHECK(t.at(0).first_frame == 72 && t.at(0).change_frame == 72, "history: a new vtable at the same address starts over");
    t.Note(m, 72 + kHistGap + 1, rd);
    CHECK(t.at(0).first_frame == 72 + kHistGap + 1, "history: a long gap starts over");
  }

  // 8. The built-in name table.
  {
    size_t n = 0;
    for (const HostName& h : kHostNames) {
      ++n;
      CHECK(h.field != kNone && h.name && std::strncmp(h.name, "menu_", 5) == 0, "name %s", h.name ? h.name : "?");
      for (const HostName& o : kHostNames)
        CHECK(&o == &h || (std::strcmp(o.name, h.name) != 0 && o.field != h.field), "duplicate %s", h.name);
    }
    CHECK(n == 7, "seven built-in states, got %zu", n);
  }

  std::printf("menu_focus_unit: %d failed%s%s\n", g_fail, g_plant.empty() ? "" : " (plant ", g_plant.empty() ? "" : (g_plant + ")").c_str());
  if (!g_plant.empty()) return g_fail ? 0 : 1;  // a planted twin: exit 0 only when the checks SEE the plant
  return g_fail ? 1 : 0;
}
