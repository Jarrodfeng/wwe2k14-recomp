// S110 MODEPICK: host unit for src/mode_pick_core.h (the rows that take a pick, the picker's test of an id as sub_829E5E10 makes it, the
// arrival test, the route state names). Prints "mode_pick_test: ok N checks" and returns 0, else the failures and 1.
#include <cstdio>
#include <string>

#include "mode_pick_core.h"

using namespace wwe2k14::mode_pick;

static int g_n = 0, g_fail = 0;
static void Expect(bool c, const std::string& what) {
  ++g_n;
  if (!c) {
    ++g_fail;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

static Facts Stock(int64_t id) {
  Facts f;
  f.id = id;
  f.has_row = true;
  f.row_active = true;
  f.available = true;
  return f;
}

int main() {
  Expect(RowTakesPick("moveset") && RowPick("moveset") == PickRow::kMoveset, "the moveset row takes a pick (the preselect word)");
  Expect(RowTakesPick("entrance") && RowPick("entrance") == PickRow::kEntrance, "S110 ENTPICK: the entrance row takes one (the controller's answers)");
  Expect(!RowTakesPick("cas") && !RowTakesPick("logo") && !RowTakesPick("") && RowPick("threads") == PickRow::kNone, "no other row takes one");

  std::string why;
  Expect(Tab(Stock(324), why) == "SUPERSTAR", "a stock wrestler (row byte +17 == 0) is on the SUPERSTAR tab");
  Facts dlc = Stock(397);
  dlc.dlc_byte = 1;
  Expect(Tab(dlc, why) == "DOWNLOADABLE CONTENT", "a row with byte +17 set (an imported seat such as 397) is on the DOWNLOADABLE CONTENT tab");
  Facts norow = Stock(450);
  norow.has_row = false;
  Expect(Tab(norow, why) == "SUPERSTAR", "a null roster row is still tested for tab 0 (sub_829E5E10 +70 skips to +90)");
  Facts off = Stock(324);
  off.row_active = false;
  why.clear();
  Expect(Tab(off, why).empty() && why.find("sub_822C1CF0") != std::string::npos, "an inactive row is refused, naming the test");
  Facts locked = Stock(324);
  locked.available = false;
  why.clear();
  Expect(Tab(locked, why).empty() && why.find("sub_822BCFB8") != std::string::npos, "an unavailable wrestler is refused, naming the test");
  Facts locked_dlc = dlc;
  locked_dlc.available = false;
  why.clear();
  Expect(Tab(locked_dlc, why).empty(), "the DLC tab makes the same availability test");

  Facts caw;
  caw.id = 0;
  caw.created_used = true;
  caw.row_active = true;
  caw.available = true;
  Expect(Tab(caw, why) == "CUSTOM SUPERSTAR", "a used created-superstar slot, active and available, is on the CUSTOM SUPERSTAR tab");
  // review (S110 MODEPICK): sub_829E5E10 +1D8..+204 gives every listed id a flag word = active && available, which the select screen
  // receives with the id (sub_829E6448 +B8..+C4); tabs 0 / 1 only list ids with both, tab 2 lists a used slot whatever they say.
  Facts caw_off = caw;
  caw_off.row_active = false;
  why.clear();
  Expect(Tab(caw_off, why).empty() && why.find("sub_822C1CF0") != std::string::npos && why.find("flag") != std::string::npos,
         "a used created-superstar slot whose row is not active (the picker's flag word 0) is refused");
  Facts caw_locked = caw;
  caw_locked.available = false;
  why.clear();
  Expect(Tab(caw_locked, why).empty() && why.find("sub_822BCFB8") != std::string::npos && why.find("flag") != std::string::npos,
         "a used created-superstar slot that is not available (the picker's flag word 0) is refused");
  caw.created_used = false;
  why.clear();
  Expect(Tab(caw, why).empty() && why.find("sub_827DA860") != std::string::npos, "an empty created-superstar slot is refused");
  Facts caw_row = Stock(5);
  caw_row.created_used = false;
  why.clear();
  Expect(Tab(caw_row, why).empty(), "ids 0..99 are listed by the slot test alone (row / availability do not list them)");

  for (int64_t bad : {int64_t(-1), int64_t(500), int64_t(512), int64_t(100000)}) {
    Facts b = Stock(bad);
    why.clear();
    Expect(Tab(b, why).empty() && why.find("0..499") != std::string::npos, "id " + std::to_string(bad) + " is not a roster id");
  }

  Seen s;
  s.seq = 20;
  s.work = 0x40001018;
  s.page = 0x40100000;
  s.page_vtable = kCategoriesVtable;
  s.work_id = 397;
  Expect(Arrived(s, 397), "the categories page on a work struct holding 397 in sequence 20 is the arrival");
  Expect(!Arrived(s, 324), "another wrestler's work struct is not");
  Seen top = s;
  top.page_vtable = 0x82056198;
  Expect(!Arrived(top, 397), "the top page (tabs / picker) is not the arrival");
  Seen seq = s;
  seq.seq = 75;
  Expect(!Arrived(seq, 397), "outside sequence 20 it is not");
  Seen none = s;
  none.work = 0;
  Expect(!Arrived(none, 397), "no work struct is not");

  // review (S110 MODEPICK): a pick nobody consumed is put back to the game's idle value once, at the warn frame, only while it still holds ours
  Expect(kGIdle == 512, "the idle value is the boot init's 512 (sub_827FCF58 +10)");
  Expect(RestoreDue(1800, 1800, 397, 397), "no editor on 397 at the warn frame and [G+16] still 397: put 512 back");
  Expect(!RestoreDue(1800, 1800, 512, 397), "state 5 already consumed it (512): nothing to put back");
  Expect(!RestoreDue(1800, 1800, 324, 397), "another writer's id (Universe): never touched");
  Expect(!RestoreDue(1799, 1800, 397, 397) && !RestoreDue(1801, 1800, 397, 397), "only at the warn frame, once");

  // S110 ENTPICK: the entrance picker's lists (sub_8226B2B8), the take (sub_8226B730 state 1, sub-step 0) and the arrival
  std::string ewhy;
  Expect(EntranceListOf(Stock(324), ewhy).list == 0, "entrance: a stock wrestler is on list 0");
  Expect(EntranceListOf(norow, ewhy).list == 0, "entrance: a null roster row is still list 0 (the byte test is skipped)");
  Expect(EntranceListOf(dlc, ewhy).list == 2, "entrance: a row with byte +17 set (an imported seat such as 397) is on list 2");
  Facts ecaw;
  ecaw.id = 3;
  ecaw.row_active = true;
  ecaw.available = true;
  Expect(EntranceListOf(ecaw, ewhy).list == 1, "entrance: an active, available created superstar is on list 1");
  Facts ecaw_off = ecaw;
  ecaw_off.row_active = false;
  ewhy.clear();
  Expect(EntranceListOf(ecaw_off, ewhy).list < 0 && ewhy.find("sub_822C1CF0") != std::string::npos, "entrance: an inactive created slot is not listed");
  Facts elocked = dlc;
  elocked.available = false;
  ewhy.clear();
  Expect(EntranceListOf(elocked, ewhy).list < 0 && ewhy.find("sub_822BCFB8") != std::string::npos, "entrance: an unavailable import is not listed");
  Facts eoff = Stock(324);
  eoff.row_active = false;
  ewhy.clear();
  Expect(EntranceListOf(eoff, ewhy).list < 0 && ewhy.find("sub_822C1CF0") != std::string::npos, "entrance: an inactive row is not listed");
  for (int64_t bad : {int64_t(-1), int64_t(500), int64_t(512)}) {
    ewhy.clear();
    Expect(EntranceListOf(Stock(bad), ewhy).list < 0 && ewhy.find("0..499") != std::string::npos, "entrance: id " + std::to_string(bad) + " is not a roster id");
  }
  Expect(CommitDue(true, 1, 0), "taken at state 1 before its first page is built");
  Expect(!CommitDue(true, 1, 1), "not once the first page is up (sub-step 1)");
  Expect(!CommitDue(true, 0, 0) && !CommitDue(true, 2, 0) && !CommitDue(true, 5, 0), "not in the other states");
  Expect(!CommitDue(false, 1, 0), "not without a pending pick (a player's own visit)");
  EntranceSeen es;
  es.taken = true;
  es.session = 0x40002000;
  es.session_char = 397;
  es.session_superstar = 1;
  es.preview = 0x40003000;
  Expect(EntranceArrived(es, 397), "taken, the session on 397 as a superstar's entrance, the preview object up: arrived");
  Expect(!EntranceArrived(es, 324), "another wrestler: not");
  EntranceSeen es_nopreview = es;
  es_nopreview.preview = 0;
  Expect(!EntranceArrived(es_nopreview, 397), "no preview object (the editor still loading): not");
  EntranceSeen es_untaken = es;
  es_untaken.taken = false;
  Expect(!EntranceArrived(es_untaken, 397), "a pick we did not deliver (a stale session): not");
  EntranceSeen es_created = es;
  es_created.session_superstar = 0;
  Expect(!EntranceArrived(es_created, 397), "the created-entrance path (+44 = 0): not");

  Expect(FindStateName("mode_pick") && FindStateName("mode_pick")->field == kFieldStatus, "state mode_pick");
  Expect(FindStateName("mode_pick_id") && FindStateName("mode_pick_id")->field == kFieldId, "state mode_pick_id");
  Expect(!FindStateName("cas_goto"), "the CAS go-to's names are not ours");

  if (g_fail) {
    std::printf("mode_pick_test: %d of %d checks failed\n", g_fail, g_n);
    return 1;
  }
  std::printf("mode_pick_test: ok %d checks\n", g_n);
  return 0;
}
