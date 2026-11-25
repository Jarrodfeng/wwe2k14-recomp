// S110 EXITMAP: host unit for src/mode_table.h (each row against the dispatcher's cases read from the listings, the name lookup, the launch
// value parse, the hand-off rule) and src/launch_gate.h (the SmartMenu's enter gates on a fake flow table; EXITMAP review). Prints
// "mode_table_test: ok N checks" and returns 0, else the failures and 1.
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "launch_gate.h"
#include "mode_table.h"

using namespace wwe2k14::mode_launch;

static int g_n = 0, g_fail = 0;
static void Expect(bool c, const std::string& what) {
  ++g_n;
  if (!c) {
    ++g_fail;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// A fake guest memory holding one flow table (src/menu_goto_core.h ReadTableWith's layout): holder -> R, T = R + 4 (count, record array,
// no index), R + 88 the menu count. Records: {id, parent, menu, item, label, flags (+40), code (+44), lock (+56)}.
struct FakeRec {
  uint32_t id, parent, menu, item, label, flags, code, lock;
};
struct Fake {
  std::map<uint32_t, uint32_t> w;
  uint32_t holder = 0x1000;
  uint32_t operator()(uint32_t a) const {
    const auto it = w.find(a);
    return it == w.end() ? 0u : it->second;
  }
  explicit Fake(const std::vector<FakeRec>& recs) {
    const uint32_t r = 0x2000, arr = 0x3000;
    w[holder + 20] = r;
    w[r + 4] = uint32_t(recs.size());
    w[r + 8] = arr;
    w[r + 88] = 16;
    for (size_t i = 0; i < recs.size(); ++i) {
      const uint32_t a = arr + uint32_t(i) * 76;
      const FakeRec& x = recs[i];
      w[a + 0] = x.id, w[a + 4] = x.parent, w[a + 8] = x.menu, w[a + 12] = x.item << 16, w[a + 24] = x.label;
      w[a + 40] = x.flags, w[a + 44] = x.code, w[a + 52] = 0, w[a + 56] = x.lock;
    }
  }
};

// The verdict for `screen` over a fake table: "" = the launch may send the exit.
// `unresolved`: the record ids the game's resolver sub_8283F7E8 returns -1 for (src/launch_core.h Gates asks it; here it is given).
static std::string Verdict(const std::vector<FakeRec>& recs, uint32_t screen, const GateEnv& e, std::string* detail = nullptr,
                           const std::set<uint32_t>& unresolved = {}) {
  Fake f(recs);
  std::vector<GateRec> g;
  if (!ReadGateTable(f, f.holder, g)) return "table not read";
  for (GateRec& r : g) r.resolves = unresolved.count(r.id) == 0;
  std::string d;
  const std::string why = GateWhy(g, screen, e, d);
  if (detail) *detail = d;
  return why;
}

static void GateCases() {
  const uint32_t N = 0xFFFFFFFFu;
  // title row (menu 0) > CREATIONS (menu 1 item 3) > SUPERSTAR (menu 3 item 0) > NEW (menu 14 item 0, screen 1502: +40 bit 0x10, code 1502)
  const FakeRec title{10, N, 0, 0, 100, 0, 0, 0}, creations{11, 10, 1, 3, 101, 0, 0, 0}, superstar{12, 11, 3, 0, 102, 0, 0, 0},
      leaf{13, 12, 14, 0, 103, 0x10, 1502, 0};
  const std::vector<FakeRec> plain = {title, creations, superstar, leaf};
  GateEnv ok;
  std::string d;
  Expect(Verdict(plain, 1502, ok, &d).empty(), "no gate bit: the press goes through");
  Expect(d.find("records 11 < 12 < 13") != std::string::npos, "the path is CREATIONS < SUPERSTAR < NEW (menu 0 not tested): " + d);
  GateEnv nostore = ok;
  nostore.storage = 0;
  Expect(Verdict(plain, 1502, nostore).empty(), "no storage and no 0x80 record: the game lets it through, so do we");
  auto with = [&](size_t i, uint32_t flags, uint32_t lock) {
    std::vector<FakeRec> v = plain;
    v[i].flags |= flags;
    v[i].lock = lock;
    return v;
  };
  // planted: CREATIONS needs storage
  const std::string s = Verdict(with(1, 0x80, 0), 1502, nostore);
  Expect(s.find("record 11") != std::string::npos && s.find("50245") != std::string::npos, "0x80 + no storage refuses at record 11: " + s);
  Expect(Verdict(with(1, 0x80, 0), 1502, ok).empty(), "0x80 with storage passes");
  GateEnv noprof = ok;
  noprof.profile = false;
  Expect(!Verdict(with(3, 0x80, 0), 1502, noprof).empty(), "0x80 with no profile object refuses");
  // the online bit
  GateEnv off = ok;
  off.online = false;
  Expect(Verdict(with(2, 0x100, 0), 1502, off).find("50373") != std::string::npos, "0x100 and sub_8293EFF0 == 0 refuses");
  Expect(Verdict(with(2, 0x100, 0), 1502, ok).empty(), "0x100 and sub_8293EFF0 != 0: the pad test holds by construction");
  {
    Fake f(with(2, 0x100, 0));
    std::vector<GateRec> g;
    ReadGateTable(f, f.holder, g);
    Expect(PathsNeedOnline(g, 1502), "a 0x100 record on the path asks sub_8293EFF0");
    Fake f2(plain);
    ReadGateTable(f2, f2.holder, g);
    Expect(!PathsNeedOnline(g, 1502), "no 0x100 record: sub_8293EFF0 is not called");
  }
  // the pad bit: by construction, unless the profile object is missing
  Expect(Verdict(with(3, 0x40, 0), 1502, ok).empty(), "0x40 holds by construction (the launch is the profile's pad)");
  Expect(!Verdict(with(3, 0x40, 0), 1502, noprof).empty(), "0x40 with no profile object refuses");
  // the lock (+56 == 1)
  GateEnv locked = ok;
  locked.lock_on = 1, locked.unlocked = 0;
  Expect(Verdict(with(3, 0, 1), 1502, locked).find("50172") != std::string::npos, "+56 == 1, lock on, unlock byte 0 refuses");
  Expect(Verdict(with(3, 0, 1), 1502, ok).empty(), "+56 == 1 with the lock off passes");
  Expect(Verdict(plain, 1502, locked).empty(), "lock on but no +56 record passes");
  // sub_8283CEB0
  GateEnv busy = ok;
  busy.sm_busy = 1;
  Expect(Verdict(plain, 1502, busy).find("51140") != std::string::npos, "SM+11496 == 1 refuses screens 1500..1540");
  // no message box object: neither gate tests anything
  GateEnv nobox = nostore;
  nobox.box = false, nobox.sm_busy = 1;
  Expect(Verdict(with(1, 0x80, 0), 1502, nobox).empty(), "no message box object: no gate");
  // a record with label -1 is not tested
  {
    std::vector<FakeRec> v = with(1, 0x80, 0);
    v[1].label = N;
    Expect(Verdict(v, 1502, nostore).empty(), "label -1: sub_8283CCF0 tests nothing");
  }
  // a menu-0 record is not tested again (entered before the main menu)
  Expect(Verdict(with(0, 0x80, 0), 1502, nostore).empty(), "menu 0's record is not re-tested");
  // no record opens the screen; a broken chain
  Expect(Verdict(plain, 1504, ok).find("no record") != std::string::npos, "an unknown screen refuses");
  {
    std::vector<FakeRec> v = plain;
    v[3].parent = 99;
    Expect(Verdict(v, 1502, ok).find("breaks") != std::string::npos, "a parent not in the table refuses");
    v[3].parent = 13;  // a loop on itself
    Expect(Verdict(v, 1502, ok).find("breaks") != std::string::npos, "a parent loop refuses");
  }
  // two paths to one screen: one passing path is enough (a player can take it); both refusing refuses
  {
    std::vector<FakeRec> v = with(1, 0x80, 0);
    v.push_back({14, 10, 1, 5, 104, 0, 0, 0});        // another main-menu row, no gate
    v.push_back({15, 14, 20, 0, 105, 0x10, 1502, 0});  // its leaf opens the same screen
    Expect(Verdict(v, 1502, nostore).empty(), "one of two paths passes: the game lets a player in");
    v[4].flags |= 0x80;
    Expect(!Verdict(v, 1502, nostore).empty(), "both paths refuse: refused");
  }
  // a record the (menu, item) lookup never returns (an earlier twin of its (menu, item)) is not a path
  {
    std::vector<FakeRec> v = plain;
    v.insert(v.begin() + 1, FakeRec{20, 10, 14, 0, 120, 0, 0, 0});  // menu 14 item 0 before the leaf: the leaf is never seen
    Expect(Verdict(v, 1502, ok).find("no record") != std::string::npos, "an unseen leaf is no path");
  }
  // S110 CASEDIT review: the resolver sub_8283F7E8 (sub_82841648 +4C) is asked for every pressed record; -1 enters nothing
  {
    std::string r = Verdict(plain, 1502, ok, nullptr, {12});
    Expect(r.find("record 12") != std::string::npos && r.find("sub_8283F7E8") != std::string::npos, "SUPERSTAR's press resolves to -1: refused: " + r);
    Expect(r.find("label 102") != std::string::npos, "the refusal names the record's label: " + r);
    r = Verdict(plain, 1502, ok, nullptr, {13});
    Expect(r.find("record 13") != std::string::npos, "the leaf resolves to -1 (e.g. EDIT with no created superstar): refused: " + r);
    GateEnv nobox2 = ok;
    nobox2.box = false;
    Expect(!Verdict(plain, 1502, nobox2, nullptr, {11}).empty(), "the resolver does not need the message box object: still refused");
    Expect(Verdict(plain, 1502, ok, nullptr, {10}).empty(), "a menu-0 record is not pressed again: its resolver answer is not used");
    Expect(Verdict(plain, 1502, ok, nullptr, {99}).empty(), "a record off the path does not matter");
    std::vector<FakeRec> v = plain;
    v.push_back({14, 10, 1, 5, 104, 0, 0, 0});
    v.push_back({15, 14, 20, 0, 105, 0x10, 1502, 0});
    Expect(Verdict(v, 1502, ok, nullptr, {12}).empty(), "one path's record resolves to -1, the other path passes: a player can go in");
    Expect(!Verdict(v, 1502, ok, nullptr, {12, 15}).empty(), "both paths resolve to -1: refused");
    Fake f(v);
    std::vector<GateRec> g;
    ReadGateTable(f, f.holder, g);
    const std::vector<size_t> pr = PathRecords(g, 1502);
    std::set<uint32_t> ids;
    for (size_t k : pr) ids.insert(g[k].id);
    Expect(pr.size() == 5 && ids == std::set<uint32_t>{11, 12, 13, 14, 15}, "PathRecords: every pressed record of both paths once, never menu 0");
  }
  // code 1497: sub_8283A900 needs SM+364 (the record's own menu when pressed) <= 100, signed
  {
    std::vector<FakeRec> v = plain;
    v[2].code = 1497, v[2].menu = 101;  // SUPERSTAR on menu 101 with code 1497
    v[3].menu = 14;
    Expect(Verdict(v, 1502, ok).find("1497") != std::string::npos, "code 1497 on menu 101: refused (sub_8283A900)");
    v[2].menu = 100;
    Expect(Verdict(v, 1502, ok).empty(), "code 1497 on menu 100: passes");
    v[2].menu = 0xFFFFFFF0u;  // a negative menu word is <= 100 signed
    Expect(Verdict(v, 1502, ok).empty(), "code 1497 with a negative menu word: passes (signed compare)");
  }
}

int main() {
  // exit -> the sequence its dispatcher case builds (sub_82833440; the r4 of sequence_base_ctor in each constructor), docs\research\s110\EXITMAP.md.
  const std::map<uint32_t, uint32_t> built = {{2, 31},  {4, 31},  {8, 76},  {9, 76},  {10, 76}, {11, 76}, {12, 25}, {13, 26}, {14, 26},
                                              {15, 26}, {16, 26}, {19, 78}, {20, 77}, {21, 27}, {22, 20}, {25, 5},  {26, 28}, {27, 28}, {30, 29}};
  // exits that need parameters, are online, are not modes, or are unread: never a row
  // (10, EDIT a superstar, is a row since S110 CASEDIT: the editor picks the superstar on its own page, docs\research\s110\CASEDIT.md)
  const std::set<uint32_t> not_rows = {1, 3, 4, 5, 6, 7, 9, 11, 13, 17, 18, 21, 23, 24, 27, 28, 29, 30};

  std::set<std::string> names;
  for (const Mode& m : kModes) {
    const std::string n(m.name);
    Expect(!n.empty(), "a row has a name");
    Expect(names.insert(n).second, "row name unique: " + n);
    for (char c : n) Expect((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_', "row name is lower case: " + n);
    Expect(m.exit >= 1 && m.exit <= 30, "exit in the switch's range: " + n);
    const auto it = built.find(m.exit);
    Expect(it != built.end(), "exit is a case that builds a sequence: " + n);
    if (it != built.end()) Expect(it->second == m.seq, "sequence matches the dispatcher's case: " + n);
    Expect(not_rows.count(m.exit) == 0, "exit is not a parameter / online / non-mode exit: " + n);
    Expect(!m.path.empty(), "row has a menu path: " + n);
    Expect(Find(m.name) == &m, "Find returns the row: " + n);
    Expect(FromLaunch(std::string("@mode:") + n) == &m, "@mode: names the row: " + n);
    Expect(IsModeLaunch(std::string("@mode:") + n), "@mode: is handed off: " + n);
    // the leaf screen the row's menu path presses, and the exit the game sends for it (sub_82867C48: codes 1500..1504 at +518..+53C, the
    // jump table 0x828681C8 for 1506..1531 (listing .270.cpp:11286-11341), and the mode handlers sub_82866680 / sub_8286A0A8 /
    // sub_82867650 / sub_8286A5A8 / sub_8286A418 by their [ctl+4] tests)
    // (1505 -> 10: screen-controller mode 13 sub_8286A288 +84, S110 CASEDIT)
    const std::map<uint32_t, uint32_t> screen_exit = {{1500, 2},  {1502, 8},  {1504, 20}, {1505, 10}, {1511, 14}, {1512, 15}, {1513, 16},
                                                      {1522, 19}, {1523, 12}, {1524, 22}, {1525, 25}, {1526, 26}};
    const auto se = screen_exit.find(m.screen);
    Expect(se != screen_exit.end() && se->second == m.exit, "the row's screen is the one whose handler sends its exit: " + n);
  }
  Expect(names.size() == 12, "12 rows");

  // the checks the menu path makes (EXITMAP.md section 2)
  const std::map<std::string, Check> checks = {{"cas", Check::kStorageSuperstarSlot},
                                               {"threads", Check::kNone},
                                               {"entrance", Check::kNone},
                                               {"moveset", Check::kNone},
                                               {"finisher_grapple", Check::kItemSlot},
                                               {"finisher_toprope", Check::kItemSlot},
                                               {"finisher_corner", Check::kItemSlot},
                                               {"arena", Check::kStorageArenaSlot},
                                               {"logo", Check::kStorage},
                                               {"wm30", Check::kNone},
                                               {"shop", Check::kNone},
                                               {"cas_edit", Check::kStorageCreatedSuperstar}};
  for (const auto& kv : checks) {
    const Mode* m = Find(kv.first);
    Expect(m != nullptr, "row present: " + kv.first);
    if (m) Expect(m->check == kv.second, "row's check: " + kv.first);
  }
  const Mode* cas = FromLaunch("@cas");
  Expect(cas && cas->exit == 8 && cas->seq == 76, "@cas is the cas row (exit 8, sequence 76)");
  Expect(FromLaunch("@mode:cas") == cas, "@mode:cas is @cas");
  const Mode* f = FromLaunch("@mode:finisher_toprope");
  Expect(f && f->exit == 15 && f->seq == 26, "finisher_toprope = exit 15, sequence 26");

  // not ours / unknown
  Expect(FromLaunch("@quick") == nullptr, "@quick is not a mode");
  Expect(!IsModeLaunch("@quick"), "@quick is not handed off");
  Expect(!IsModeLaunch("lesnar_bryan"), "a scenario name is not handed off");
  Expect(!IsModeLaunch(""), "empty is not handed off");
  Expect(!IsModeLaunch("@mode"), "@mode without the colon is not ours");
  Expect(IsModeLaunch("@cas"), "@cas is handed off");
  Expect(IsModeLaunch("@mode:"), "@mode: with no name is ours (refused with the list)");
  Expect(IsModeLaunch("@mode:nope") && FromLaunch("@mode:nope") == nullptr, "an unknown name is ours but has no row");
  Expect(FromLaunch("@mode:") == nullptr, "no name, no row");
  Expect(FromLaunch("@mode:CAS") == nullptr, "names are case-sensitive");
  Expect(FromLaunch("@cas ") == nullptr, "no trimming");
  Expect(std::string(CheckName(Check::kStorageSuperstarSlot)) == "storage + free superstar slot", "check names");
  Expect(std::string(CheckName(Check::kNone)) == "none", "check name none");
  const Mode* ed = FromLaunch("@mode:cas_edit");
  Expect(ed && ed->exit == 10 && ed->seq == 76 && ed->screen == 1505, "cas_edit = exit 10 (sub_833979B8 param 2), sequence 76, screen 1505");
  Expect(std::string(CheckName(Check::kStorageCreatedSuperstar)) == "storage + a created superstar", "check name created superstar");

  GateCases();

  if (g_fail) {
    std::printf("mode_table_test: %d of %d checks failed\n", g_fail, g_n);
    return 1;
  }
  std::printf("mode_table_test: ok %d checks\n", g_n);
  return 0;
}
