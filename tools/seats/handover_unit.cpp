// S110 SEAT2 (lane H-A): host unit for src\slot_handover_core.h. No game files: every payload byte is a made-up pattern; the unit writes only under the
// folder it is given (argv[1], default out\handover_unit).
//   handover_unit.exe [work dir]     every check must PASS (exit 0).   Built and run (with its planted twins) by tools\seats\handover_selftest.py.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <sstream>
#include <string>
#include <thread>

#include "slot_handover_core.h"
#include "universe_strip_core.h"
#include "universe_rebuild_core.h"

namespace fs = std::filesystem;
namespace sc = wwe2k14::seat_card;
namespace st = wwe2k14::seats;
namespace sh = wwe2k14::slot_handover;
namespace sm = wwe2k14::superstars_migrate;

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("FAIL  %s\n", what.c_str());
  }
}
static bool Has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }
static std::string Slurp(const fs::path& p) {
  std::string s;
  sc::detail::ReadAll(p, s);
  return s;
}
static void Spit(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(s.data(), std::streamsize(s.size()));
}

// ---- made-up patterns ----
template <size_t N>
static void Pat(std::array<uint8_t, N>& a, int seed) {
  for (size_t i = 0; i < N; ++i) a[i] = uint8_t((i * 7 + size_t(seed) * 13 + 5) & 255);
}
static void Put16(uint8_t* p, uint32_t v) { st::PutBe16(p, uint16_t(v)); }
static uint32_t Get16(const uint8_t* p) { return st::Be16(p); }

static const char* A = "AAAA0001";
static const char* B = "BBBB0002";
static const char* C = "CCCC0003";
static const char* D = "DDDD0004";

static st::Roster MakeRoster(const std::vector<std::pair<int, const char*>>& res, const std::map<int, std::string>& held = {}, bool loaded = true, bool old_pack = false) {
  st::Roster r;
  r.loaded = loaded, r.old_pack = old_pack;
  for (const auto& x : res) {
    st::Resident s;
    s.seat = x.first, s.key = x.second;
    r.res.push_back(s);
  }
  r.held = held;
  return r;
}
static sh::Entry MakeE(uint64_t stamp, const std::map<int, std::string>& owners, const std::vector<int>& pending = {}) {
  sh::Entry e;
  e.stamp = stamp, e.frame = "1A2B3C4D", e.owners = owners, e.pending = pending;
  return e;
}
// (review) null-safe: a seat the outcome does not hold answers seat -1, so a planted fault fails a check (a printed FAIL), never a crash.
static const sh::SeatAction* Act(const sh::Outcome& o, int seat) {
  static const sh::SeatAction kNone{-1, "", "", false, "(the outcome holds no action for this seat)"};
  const sh::SeatAction* a = o.Find(seat);
  return a ? a : &kNone;
}
static std::string Seats(const std::vector<sh::SeatKey>& v) {
  std::string s;
  for (const auto& x : v) s += std::to_string(x.seat) + ":" + x.key + " ";
  return s;
}

// ================================================================ the plan: every row of the table
static void TestPlan() {
  const sh::Entry e = MakeE(1, {{397, A}, {398, B}, {399, C}, {400, D}});
  // occupied by the same man: nothing
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({{397, A}, {398, B}, {399, C}, {400, D}}));
    Check(!p.adopt && !p.carry && p.leaves.empty() && p.arrivals.empty(), "same men in their seats: nothing to hand over");
  }
  // occupied by another man: leave K, arrive J
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({{397, B}, {398, B}, {399, C}, {400, D}}));
    Check(p.leaves == std::vector<sh::SeatKey>{{397, A}} && p.arrivals == std::vector<sh::SeatKey>{{397, B}}, "seat 397 A -> B: leave A, arrive B (got " + Seats(p.leaves) + "/" + Seats(p.arrivals) + ")");
  }
  // owner none, occupied by J: arrive (even over an active row)
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({{397, A}, {398, B}, {399, C}, {400, D}, {401, B}}));
    Check(p.leaves.empty() && p.arrivals == std::vector<sh::SeatKey>{{401, B}}, "owner none, occupied: arrive only (got " + Seats(p.arrivals) + ")");
  }
  // owner K, empty seat (no card names it): leave K
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({{397, A}, {398, B}, {399, C}}));
    Check(p.leaves == std::vector<sh::SeatKey>{{400, D}} && p.arrivals.empty(), "owner K, empty seat: leave K (got " + Seats(p.leaves) + ")");
  }
  // owner none, empty: nothing; seat with no entry line and no card
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({{397, A}, {398, B}, {399, C}, {400, D}}));
    Check(p.leaves.empty() && p.arrivals.empty(), "owner none, empty seat: nothing");
  }
  // HELD: never handed over, whoever owns it, whatever the held card is (owned by K / by nobody)
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({{397, A}, {399, C}, {400, D}}, {{398, std::string(B) + ": stale inst"}}));
    Check(p.leaves.empty() && p.arrivals.empty(), "held seat owned by B held by B: nothing");
    const sh::Plan q = sh::MakePlan(&e, MakeRoster({{397, A}, {399, C}, {400, D}}, {{398, std::string(D) + ": missing pac"}}));
    Check(q.leaves.empty() && q.arrivals.empty(), "held seat whose card is another man's: never handed over (got " + Seats(q.leaves) + ")");
    const sh::Plan z = sh::MakePlan(&e, MakeRoster({{397, A}, {398, B}, {399, C}, {400, D}}, {{405, std::string(D) + ": held"}}));
    Check(z.leaves.empty() && z.arrivals.empty(), "held seat with no owner line: nothing");
  }
  // no entry -> adopt
  {
    const sh::Plan p = sh::MakePlan(nullptr, MakeRoster({{397, A}}));
    Check(p.adopt && !p.carry && p.leaves.empty() && p.arrivals.empty(), "no entry: adopt, no hand-over");
  }
  // imports off / roster not loaded, an old pack -> carry (even with an entry that would otherwise leave everyone)
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({}, {}, false, false));
    Check(p.carry && !p.adopt && p.leaves.empty() && p.arrivals.empty(), "imports off: carry, nobody leaves");
    const sh::Plan q = sh::MakePlan(&e, MakeRoster({}, {{397, "old pack: x"}}, true, true));
    Check(q.carry && q.leaves.empty() && q.arrivals.empty(), "old pack: carry, nobody leaves");
    const sh::Plan z = sh::MakePlan(nullptr, MakeRoster({}, {}, false, false));
    Check(z.carry && !z.adopt, "imports off with no entry: carry, not adopt (the empty roster must not be adopted)");
  }
  // a man moved from t to s in one load; two leaves and one arrival
  {
    const sh::Plan p = sh::MakePlan(&e, MakeRoster({{397, B}, {398, A}, {399, C}}));
    Check(p.leaves == std::vector<sh::SeatKey>{{397, A}, {398, B}, {400, D}} && p.arrivals == std::vector<sh::SeatKey>{{397, B}, {398, A}}, "A and B swap, D leaves (got " + Seats(p.leaves) + "/" + Seats(p.arrivals) + ")");
    const sh::Plan q = sh::MakePlan(&e, MakeRoster({{397, A}, {398, A}}));  // A moved to 398 too (cannot be: library gives a key one seat; the plan still orders by seat)
    Check(!q.leaves.empty(), "ascending seat order kept");
    const sh::Plan m = sh::MakePlan(&e, MakeRoster({{398, A}, {397, C}, {399, C}, {400, D}}));
    Check(m.leaves == std::vector<sh::SeatKey>{{397, A}, {398, B}} && m.arrivals == std::vector<sh::SeatKey>{{397, C}, {398, A}}, "A moves 397 -> 398, C arrives at 397 (got " + Seats(m.leaves) + "/" + Seats(m.arrivals) + ")");
  }
  // idempotent: the same entry and roster give the same plan; the loaded entry is not touched
  {
    const sh::Entry copy = e;
    const st::Roster r = MakeRoster({{397, B}, {398, B}, {399, C}});
    const sh::Plan p1 = sh::MakePlan(&e, r), p2 = sh::MakePlan(&e, r);
    Check(p1.leaves == p2.leaves && p1.arrivals == p2.arrivals && e.owners == copy.owners && e.stamp == copy.stamp && e.pending == copy.pending, "planning twice: the same plan, the entry untouched");
  }
}

// ================================================================ NextOwners / pending / the export decision
static void TestOwners() {
  const sh::Entry e = MakeE(1, {{397, A}, {398, B}, {400, D}}, {500 - 1});
  const st::Roster r = MakeRoster({{397, B}, {398, B}, {401, C}}, {{402, std::string(D) + ": stale"}});
  const sh::Plan p = sh::MakePlan(&e, r);  // 397: A->B; 400: D leaves; 401: C arrives
  const sh::Outcome ok = sh::Settle(p, {});
  Check(ok.seats.size() == 3 && ok.seats[0].seat == 397 && ok.seats[0].proceed, "outcome: three seats, ascending, all proceed");
  const auto n = sh::NextOwners(&e, r, p, ok);
  Check(n && *n == std::map<int, std::string>{{397, B}, {398, B}, {401, C}}, "next owners after a clean hand-over");
  Check(sh::NextPending(&e, ok) == std::vector<int>{397, 400, 499}, "pending: the loaded pending + every purged seat (leaves), not an arrival-only seat");
  // the loaded entry is NEVER rewritten
  Check(e.owners == std::map<int, std::string>{{397, A}, {398, B}, {400, D}} && e.pending == std::vector<int>{499}, "the loaded entry is unchanged after the hand-over");
  // export failed at 397: the seat is kept as it was
  const sh::Outcome bad = sh::Settle(p, {397});
  Check(Act(bad, 397)->seat == 397 && !Act(bad, 397)->proceed && Has(Act(bad, 397)->why, "export of AAAA0001 failed"), "export failed: seat 397 does not proceed");
  Check(Act(bad, 400)->proceed && Act(bad, 401)->proceed, "export failed at 397: the other seats still proceed");
  const auto nb = sh::NextOwners(&e, r, p, bad);
  Check(nb && *nb == std::map<int, std::string>{{397, A}, {398, B}, {401, C}}, "export failed: owner(397) stays A (got " + std::to_string(nb ? nb->size() : 0) + " entries)");
  Check(sh::NextPending(&e, bad) == std::vector<int>{400, 499}, "export failed: a seat that was not purged is not pending");
  // a man moved t -> s whose export failed does not arrive at s with a stale file
  const st::Roster mv = MakeRoster({{398, A}, {397, C}});
  const sh::Plan pm = sh::MakePlan(&e, mv);  // 397: A leaves, C arrives; 398: B leaves, A arrives; 400: D leaves
  const sh::Outcome om = sh::Settle(pm, {397});
  Check(Act(om, 397)->seat == 397 && !Act(om, 397)->proceed, "moved man: his own seat is kept");
  Check(Act(om, 398)->seat == 398 && !Act(om, 398)->proceed && Has(Act(om, 398)->why, "failed elsewhere"), "moved man: his arrival at 398 is held back too (his export failed)");
  Check(Act(om, 400)->proceed, "moved man: an unrelated leave proceeds");
  // adopt: occupants, and the holder of a held seat; carry: the loaded map
  const sh::Plan ad = sh::MakePlan(nullptr, r);
  const auto na = sh::NextOwners(nullptr, r, ad, sh::Settle(ad, {}));
  Check(na && *na == std::map<int, std::string>{{397, B}, {398, B}, {401, C}, {402, D}}, "adopt: occupants + the held seat's card key");
  const sh::Plan ca = sh::MakePlan(&e, MakeRoster({}, {}, false));
  const auto nc = sh::NextOwners(&e, MakeRoster({}, {}, false), ca, sh::Settle(ca, {}));
  Check(nc && *nc == e.owners, "carry: the loaded owners as they are (never an empty entry with imports off)");
  Check(sh::NextPending(&e, sh::Settle(ca, {})) == std::vector<int>{499}, "carry: the loaded pending is carried on");
  // (review) carry with NO loaded entry (imports off / an old pack, on an image the record does not know): NOTHING is recorded. An empty owner map
  // recorded for the image it writes would make the next boot with imports on ARRIVE every resident from his card over his saved rows (owner none,
  // occupied -> arrive), with no export: the player's edits of every imported superstar gone.
  const sh::Plan c0 = sh::MakePlan(nullptr, MakeRoster({}, {}, false));
  Check(!sh::NextOwners(nullptr, MakeRoster({}, {}, false), c0, sh::Settle(c0, {})).has_value(), "imports off, image not in the record: no entry is recorded");
  const st::Roster op = MakeRoster({}, {{397, "old pack: x"}}, true, true);
  const sh::Plan c1 = sh::MakePlan(nullptr, op);
  Check(!sh::NextOwners(nullptr, op, c1, sh::Settle(c1, {})).has_value(), "old pack, image not in the record: no entry is recorded");
  // a held seat keeps its owner
  const sh::Plan hp = sh::MakePlan(&e, MakeRoster({{397, A}, {400, D}}, {{398, std::string(C) + ": x"}}));
  const auto nh = sh::NextOwners(&e, MakeRoster({{397, A}, {400, D}}, {{398, std::string(C) + ": x"}}), hp, sh::Settle(hp, {}));
  Check(nh && nh->count(398) && nh->at(398) == B, "held seat: its owner line is unchanged");
}

// ================================================================ the side record
static void TestRecord(const fs::path& work) {
  sh::Record rec;
  for (uint64_t i = 1; i <= 11; ++i) Check(sh::Prepend(rec, MakeE(i, {{397, A}, {398, B}}, i == 11 ? std::vector<int>{398} : std::vector<int>{})), "prepend " + std::to_string(i));
  Check(rec.entries.size() == sh::kKeepEntries && rec.entries.front().stamp == 11 && rec.entries.back().stamp == 11 - sh::kKeepEntries + 1, "newest first, only the newest kept");
  // an entry is never rewritten
  sh::Entry again = MakeE(11, {{397, D}});
  Check(!sh::Prepend(rec, again) && rec.entries.front().owners.at(397) == A && rec.entries.size() == sh::kKeepEntries, "the same stamp again: the old entry stays exactly as it was");
  // round trip, byte exact text
  const std::string text = sh::RecordText(rec);
  sh::Record back;
  std::string err;
  Check(sh::ParseRecord(text, back, err) && back.notes.empty(), "record parses back: " + err);
  Check(sh::RecordText(back) == text, "record text round trip is byte exact");
  Check(text.rfind("seats 1\nentry 000000000000000B frame 1A2B3C4D\nseat 397 AAAA0001\nseat 398 BBBB0002\npending 398\nend\nentry 000000000000000A", 0) == 0, "the text form: seats 1 / entry / seat / pending / end, newest first");
  // unknown words kept: after a key, a line in an entry, a line outside
  const std::string unk =
      "seats 1\r\nfuture_top yes 1\r\nentry 00000000000000AA frame -\r\nseat 397 AAAA0001 guest resident\r\nflag x y\r\nseat 398 BBBB0002\r\nend\r\n";
  sh::Record u;
  Check(sh::ParseRecord(unk, u, err) && u.entries.size() == 1 && u.entries[0].frame == "-" && u.entries[0].owner_words.at(397) == "guest resident" &&
            u.entries[0].extra.size() == 1 && u.extra.size() == 1,
        "unknown words / lines are kept (CRLF accepted): " + err);
  Check(sh::RecordText(u) == "seats 1\nfuture_top yes 1\nentry 00000000000000AA frame -\nseat 397 AAAA0001 guest resident\nseat 398 BBBB0002\nflag x y\nend\n", "unknown words written back unchanged");
  if (u.entries.size() != 1) {  // (review) null-safe: a planted parser fault fails here, it does not index an empty vector
    Check(false, "MakeEntry cases skipped: the unknown-words record did not parse to one entry");
  } else {
    const sh::Entry carried = sh::MakeEntry(0xBB, "-", u.entries[0].owners, {}, &u.entries[0]);
    Check(carried.owner_words.count(397) && carried.owner_words.at(397) == "guest resident" && carried.extra.size() == 1,
          "MakeEntry from the loaded entry keeps the words of an unchanged seat and its unknown lines");
    const sh::Entry swapped = sh::MakeEntry(0xBB, "-", {{397, B}, {398, B}}, {}, &u.entries[0]);
    Check(swapped.owner_words.empty(), "a seat that changed hands does not inherit the old man's words");
  }
  // refusals
  sh::Record x;
  Check(!sh::ParseRecord("seats 2\nentry 00000000000000AA frame -\nend\n", x, err) && Has(err, "version 2"), "another version refuses the whole file");
  Check(!sh::ParseRecord("entry 00000000000000AA frame -\nend\n", x, err), "no seats line refuses");
  Check(sh::ParseRecord("", x, err) && x.entries.empty(), "an empty text is an empty record");
  const std::string dmg =
      "seats 1\nentry 00000000000000C1 frame -\nseat 397 AAAA0001\nend\n"
      "entry 00000000000000C2 frame -\nseat 99 BBBB0002\nseat 398 BBBB0002\nend\n"
      "entry 00000000000000C3 frame -\nseat 397 AAAA0001\nseat 397 BBBB0002\nend\n"
      "entry 00000000000000C4 frame -\nseat 398 notakey\nend\n"
      "entry 00000000000000C5 frame -\npending 398 x\nend\n"
      "entry zzzz frame -\nseat 397 AAAA0001\nend\n"
      "entry 00000000000000C1 frame -\nseat 397 BBBB0002\nend\n"
      "entry 00000000000000C6 frame -\nseat 398 BBBB0002\nend\n"
      "entry 00000000000000C7 frame -\nseat 398 BBBB0002\n";
  sh::Record d;
  Check(sh::ParseRecord(dmg, d, err), "a damaged file still parses: " + err);
  Check(d.entries.size() == 2 && d.entries[0].stamp == 0xC1 && d.entries[1].stamp == 0xC6, "each damaged entry is refused alone; the others stand (got " + std::to_string(d.entries.size()) + ")");
  Check(d.entries.size() == 2 && d.entries[0].owners.at(397) == A, "a stamp twice: the first stands");
  Check(d.notes.size() >= 7, "every refusal is noted (" + std::to_string(d.notes.size()) + ")");
  // write / read through the file; a missing file is an empty record
  const fs::path rp = sh::RecordPath(work / "root");
  Check(!rp.empty() && rp.filename() == "seats.txt" && rp.parent_path().filename() == "superstars", "record path");
  sh::Record r0;
  Check(sh::ReadRecord(rp, r0, err) && r0.entries.empty(), "a missing record file is an empty record");
  Check(sh::WriteRecord(rp, rec, &err), "record writes: " + err);
  sh::Record r1;
  Check(sh::ReadRecord(rp, r1, err) && sh::RecordText(r1) == text && Slurp(rp) == text, "record file round trip");
  Check(!fs::exists(fs::path(rp) += ".tmp"), "no temp file left");
  sh::Record bad = rec;
  bad.entries[0].owners[50] = A;
  Check(!sh::WriteRecord(rp, bad, &err) && Slurp(rp) == text, "a record the reader would refuse is not written (the old file stays)");
  Spit(rp, "garbage\n");
  Check(!sh::ReadRecord(rp, r1, err), "a file with no version line is refused");
  // wide path
  const fs::path wide = work / fs::path(u8"wäde é中") / "seats.txt";
  Check(sh::WriteRecord(wide, rec, &err) && sh::ReadRecord(wide, r1, err) && r1.entries.size() == rec.entries.size(), "a user data root outside the ANSI code page");
}

// ================================================================ the state file
static sh::State MakeState(int seat, int seed, bool cnt) {
  sh::State s;
  s.seat = uint16_t(seat), s.bank = 2301, s.frame_sig = 0x1A2B3C4Du;
  Pat(s.row, seed), Pat(s.pro, seed + 1), Pat(s.e40, seed + 2), Pat(s.e64, seed + 3), Pat(s.m8, seed + 4);
  s.has_cnt = cnt;
  if (cnt) Pat(s.cnt, seed + 5);
  return s;
}
static std::string Framed(const sh::State& s, const std::vector<std::pair<std::string, std::string>>& blocks, bool header = true) {
  std::string b(sh::kStateMagic, 8);
  if (header) {
    sc::detail::Put16(b, s.seat);
    sc::detail::Put16(b, s.bank);
    sc::detail::Put32(b, s.frame_sig);
  }
  for (const auto& kv : blocks) sc::detail::Block(b, kv.first.c_str(), kv.second.data(), kv.second.size());
  return b;
}
static std::string Of(const uint8_t* p, size_t n) { return std::string(reinterpret_cast<const char*>(p), n); }

static void TestState(const fs::path& work) {
  std::string err;
  for (bool cnt : {true, false}) {
    const sh::State s = MakeState(397, 3, cnt);
    std::string bytes;
    Check(sh::EncodeState(s, bytes, err), "encode");
    Check(bytes.size() == (cnt ? 1657u : 1644u), "state file size " + std::to_string(bytes.size()));
    Check(bytes.compare(0, 8, "SEATST01") == 0 && uint8_t(bytes[8]) == 0x01 && uint8_t(bytes[9]) == 0x8D && uint8_t(bytes[10]) == 0x08 && uint8_t(bytes[11]) == 0xFD &&
              uint8_t(bytes[12]) == 0x1A && uint8_t(bytes[15]) == 0x4D && bytes.compare(16, 4, "ROW ") == 0 && uint8_t(bytes[22]) == 0 && uint8_t(bytes[23]) == 204,
          "framing: magic, u16 seat 397, u16 bank 2301, u32 sig, ROW block of 204 (big-endian)");
    sh::State back;
    Check(sh::DecodeState(bytes, back, err) && back == s, std::string("decode round trip (cnt ") + (cnt ? "yes" : "no") + "): " + err);
    const fs::path p = sh::StatePath(work / "root", A);
    Check(sh::WriteState(p, s, err), "state writes: " + err);
    sh::State f;
    Check(sh::ReadState(p, f, err) && f == s && Slurp(p) == bytes, "state file round trip, byte exact");
    Check(!fs::exists(fs::path(p) += ".tmp"), "no temp state file left");
  }
  Check(sh::StatePath(work, "nokey").empty() && sh::StatePath(work, "../EVIL0001").empty(), "a key that is not 8 hex digits has no path");
  Check(sh::StatePath(work / "root", A).filename() == "AAAA0001.bin" && sh::StatePath(work / "root", A).parent_path().filename() == "state", "state path");
  const sh::State s = MakeState(397, 3, true);
  std::string good;
  sh::EncodeState(s, good, err);
  sh::State x;
  // refusals, each with its own words
  std::string bad = good;
  bad.replace(0, 8, "SEATROW1");
  Check(!sh::DecodeState(bad, x, err) && Has(err, "bad magic"), "bad magic refused: " + err);
  bad = good;
  bad.replace(0, 8, "SEATST02");
  Check(!sh::DecodeState(bad, x, err) && Has(err, "another format version"), "another version refused: " + err);
  Check(!sh::DecodeState(good.substr(0, 12), x, err) && Has(err, "truncated in the header"), "truncated header refused: " + err);
  Check(!sh::DecodeState(good.substr(0, 4), x, err) && Has(err, "bad magic"), "a 4-byte file refused: " + err);
  Check(!sh::DecodeState(good.substr(0, good.size() - 3), x, err) && Has(err, "truncated in block CNT"), "a truncated block refused: " + err);
  Check(!sh::DecodeState(good.substr(0, good.size() - 9), x, err) && Has(err, "truncated in a block header"), "a truncated block header refused: " + err);
  Check(!sh::DecodeState(good.substr(0, 16 + 8 + 204 + 3), x, err) && Has(err, "truncated"), "a block header cut short refused: " + err);
  std::string r203(203, 'x');
  Check(!sh::DecodeState(Framed(s, {{"ROW ", r203}}), x, err) && Has(err, "must be 204"), "a block of the wrong size (short) refused: " + err);
  std::string r205(205, 'x');
  Check(!sh::DecodeState(Framed(s, {{"ROW ", r205}}), x, err) && Has(err, "must be 204"), "a block of the wrong size (long) refused: " + err);
  Check(!sh::DecodeState(good + Framed(s, {{"M8  ", Of(s.m8.data(), 8)}}).substr(16), x, err) && Has(err, "twice"), "a block twice refused: " + err);
  Check(!sh::DecodeState(good + Framed(s, {{"XYZ ", ""}}).substr(16), x, err) && Has(err, "unknown tag"), "an unknown tag refused: " + err);
  Check(!sh::DecodeState(Framed(s, {{"ROW ", Of(s.row.data(), 204)}, {"E40 ", Of(s.e40.data(), 40)}, {"E64 ", Of(s.e64.data(), 64)}, {"M8  ", Of(s.m8.data(), 8)}}), x, err) &&
            Has(err, "no PRO block"),
        "a missing block refused: " + err);
  sh::State low = s;
  low.seat = 99;
  Check(!sh::EncodeState(low, bad, err) && Has(err, "100..499"), "a seat outside 100..499 is not written: " + err);
  bad = good;
  bad[8] = 0, bad[9] = 99;
  Check(!sh::DecodeState(bad, x, err) && Has(err, "100..499"), "a seat outside 100..499 is not read: " + err);
  Check(!sh::DecodeState(good + std::string(5000, 'z'), x, err) && Has(err, "more than"), "an oversized file refused: " + err);
  Check(sh::DecodeState(good, x, err) && x.has_cnt, "the good file still reads");
  // the export failure: a state file that cannot be written says so (and Settle then keeps the seat)
  const fs::path blocker = work / "blocker";
  Spit(blocker, "a plain file where a folder is wanted");
  const fs::path unwritable = blocker / "superstars" / "state" / "AAAA0001.bin";
  Check(!sh::WriteState(unwritable, s, err) && !err.empty(), "an export that cannot be written reports failure: " + err);
  sh::State w;
  Check(!sh::ReadState(work / "nothing.bin", w, err) && Has(err, "cannot read"), "a missing state file reports: " + err);
}

// the decision the hook takes: export every leave, collect the failures, Settle
static void TestExportDecision(const fs::path& work) {
  const sh::Entry e = MakeE(1, {{397, A}, {398, B}});
  const st::Roster r = MakeRoster({{397, C}, {398, D}});
  const sh::Plan p = sh::MakePlan(&e, r);
  const fs::path blocker = work / "blocker2";
  Spit(blocker, "file");
  std::set<int> failed;
  std::string err;
  for (const sh::SeatKey& l : p.leaves) {
    const fs::path path = l.seat == 397 ? sh::StatePath(blocker, l.key) : sh::StatePath(work / "ok", l.key);  // 397's folder cannot be made
    if (!sh::WriteState(path, MakeState(l.seat, l.seat, true), err)) failed.insert(l.seat);
  }
  const sh::Outcome o = sh::Settle(p, failed);
  Check(failed == std::set<int>{397}, "only the unwritable export failed");
  Check(Act(o, 397)->seat == 397 && !Act(o, 397)->proceed && Act(o, 398)->proceed, "the seat whose export failed is NOT purged, the other is");
  Check(Act(o, 397)->seat == 397 && !Act(o, 397)->proceed && Act(o, 397)->arrive == C, "the arrival there waits too (the seat is kept as it was)");
  const auto n = sh::NextOwners(&e, r, p, o);
  Check(n && n->count(397) && n->count(398) && n->at(397) == A && n->at(398) == D, "the record keeps A at 397 (his rows are still there) and records D at 398");
  Check(sh::NextPending(&e, o) == std::vector<int>{398}, "only the purged seat is pending");
}

// ================================================================ re-target
static void TestRetarget() {
  const int t = 397, s = 398, bank_t = 2301;
  std::array<uint8_t, 204> srow{}, crow{};
  Pat(srow, 11), Pat(crow, 77);
  Put16(&srow[0], t), Put16(&srow[4], t), Put16(&srow[200], t);
  Put16(&crow[0], s), Put16(&crow[4], s), Put16(&crow[200], s);
  srow[16] = 0, crow[16] = 1;
  Put16(&srow[120], t * 100 + 5), Put16(&srow[150], t * 100 + 99), Put16(&srow[198], t * 100 + 0);  // keys of t (in 116..199)
  Put16(&srow[130], 12345), Put16(&srow[140], s * 100 + 7);                                      // not t's: left
  Put16(&srow[70], t * 100 + 5);                                                                  // a value that merely equals a key outside the range: left
  Put16(&srow[116], t * 100 + 100);                                                               // one past the range of t: left
  std::array<uint8_t, 204> out{};
  const int keys = sh::RetargetRow(srow.data(), crow.data(), t, s, out.data());
  Check(Get16(&out[0]) == s && Get16(&out[4]) == s && Get16(&out[200]) == s, "row: ids +0 / +4 / +200 are the seat");
  Check(std::memcmp(&out[2], &crow[2], 2) == 0, "row: the rank is the card's");
  Check(std::memcmp(&out[6], &crow[6], 6) == 0, "row: the three text ids are the card's");
  Check(out[17] == crow[17], "row: the pack code is the card's");
  // (review) +12 is a fourth text id, 79000 + id or 99999 (docs\formats\chetc_roster_record.md): seat-derived like +6 / +8 / +10, so the card's
  Check(std::memcmp(&out[12], &crow[12], 4) == 0 && std::memcmp(&srow[12], &crow[12], 4) != 0, "row: the text id at +12 is the card's");
  Check(out[16] == 1, "row: active");
  Check(keys == 3 && Get16(&out[120]) == s * 100 + 5 && Get16(&out[150]) == s * 100 + 99 && Get16(&out[198]) == s * 100, "row: t's keys become the seat's (3)");
  Check(Get16(&out[130]) == 12345 && Get16(&out[140]) == s * 100 + 7 && Get16(&out[70]) == t * 100 + 5 && Get16(&out[116]) == t * 100 + 100, "row: other words are left");
  Check(std::memcmp(&out[52], &srow[52], 8) == 0, "row: the move-set editor's +52..+59 are the player's, kept");
  bool rest = true;
  for (size_t i = 0; i < 204; ++i)
    if ((i < 16 || i == 16 || i == 17 || i == 200 || i == 201 || i == 120 || i == 121 || i == 150 || i == 151 || i == 198 || i == 199) == false && out[i] != srow[i]) rest = false;
  Check(rest, "row: every other byte is the state's");
  // s == t is the identity (the card's seat-derived bytes equal the state's)
  std::array<uint8_t, 204> same = srow, o2{};
  same[16] = 1;
  const int k2 = sh::RetargetRow(same.data(), same.data(), t, t, o2.data());
  Check(k2 == 3 && o2 == same, "row: s == t is the identity");

  // profile
  std::array<uint8_t, 1272> sp{}, cp{}, po{};
  Pat(sp, 21), Pat(cp, 99);
  // a made-up pattern could hit a bank-class word; clear every word the rule reads, then set the ones the case wants
  for (size_t off : sm::kBankWords) Put16(&sp[off], 0), Put16(&cp[off], 0);
  for (int k = 0; k < 5; ++k) Put16(&sp[sm::kPartBase + sm::kPartStride * size_t(k)], 0), Put16(&cp[sm::kPartBase + sm::kPartStride * size_t(k)], 0);
  for (size_t off : sm::kMovieWords) Put16(&sp[off], 0), Put16(&cp[off], 0);
  Put16(&sp[576], bank_t), Put16(&sp[592], t), Put16(&sp[578], t), Put16(&sp[580], bank_t), Put16(&sp[594], 4000), Put16(&sp[596], 0);
  Put16(&cp[576], 2455), Put16(&cp[592], 2455), Put16(&cp[578], 2455), Put16(&cp[580], 2455), Put16(&cp[594], 2455), Put16(&cp[596], 2455);
  Put16(&sp[618 + 34], 10000 + 1000 + bank_t), Put16(&sp[618 + 68], 10000 + 2000 + t), Put16(&sp[618], 4321);
  Put16(&sp[574], t), Put16(&cp[574], 5000), Put16(&sp[590], 6000), Put16(&cp[590], 5001);
  Put16(&sp[444], 4242), Put16(&sp[700], t), Put16(&sp[702], bank_t);  // the player's choices (a move id, other words that merely equal t / bank)
  Put16(&sp[1256], t), Put16(&sp[1258], t), Put16(&sp[1264], 7), Put16(&sp[1266], t);
  std::string what;
  const int n = sh::RetargetProfile(sp.data(), cp.data(), t, bank_t, s, po.data(), &what);
  Check(Get16(&po[576]) == 2455 && Get16(&po[592]) == 2455 && Get16(&po[578]) == 2455 && Get16(&po[580]) == 2455, "profile: bank / champion words that named t or his bank become the card's (" + what + ")");
  Check(Get16(&po[594]) == 4000 && Get16(&po[596]) == 0, "profile: a bank-class word that is neither is left");
  Check(Get16(&po[618 + 34]) == 10000 + 1000 + 2455 && Get16(&po[618 + 68]) == 10000 + 2000 + 2455 && Get16(&po[618]) == 4321, "profile: part banks of t / his bank move; another is left");
  Check(Get16(&po[574]) == 5000 && Get16(&po[590]) == 6000, "profile: the movie word that was t moves, a valid choice stays");
  Check(Get16(&po[444]) == 4242 && Get16(&po[700]) == t && Get16(&po[702]) == bank_t, "profile: the player's move-set slot and other words are left");
  Check(Get16(&po[1256]) == s && Get16(&po[1264]) == 7 && Get16(&po[1258]) == t && Get16(&po[1266]) == t, "profile: the alt-key entries' own id becomes the seat; their other words stay");
  Check(n == 8,"profile: words changed counted (" + std::to_string(n) + ")");
  bool prest = true;
  for (size_t i = 0; i < 1272; ++i) {
    const bool touched = (i >= 574 && i < 600) || (i >= 618 && i < 618 + 5 * 34 && ((i - 618) % 34) < 2) || i == 1256 || i == 1257;
    if (!touched && po[i] != sp[i]) prest = false;
  }
  Check(prest, "profile: every other byte is the state's (the player's bytes survive)");
  std::array<uint8_t, 1272> p2{};
  sh::RetargetProfile(sp.data(), sp.data(), t, bank_t, t, p2.data());
  Check(Get16(&p2[1256]) == t, "profile: s == t leaves the alt-key id");
}

// ================================================================ the stamp
static void TestFnv() {
  const char* a = "a";
  const char* fb = "foobar";
  Check(sh::Fnv64(nullptr, 0) == 0xCBF29CE484222325ull, "Fnv64 of nothing is the offset basis");
  Check(sh::Fnv64(reinterpret_cast<const uint8_t*>(a), 1) == 0xAF63DC4C8601EC8Cull, "Fnv64('a')");
  Check(sh::Fnv64(reinterpret_cast<const uint8_t*>(fb), 6) == 0x85944171F73967E8ull, "Fnv64('foobar')");
  Check(sh::Hex16(0xAF63DC4C8601EC8Cull) == "AF63DC4C8601EC8C" && sh::Hex16(1) == "0000000000000001", "Hex16");
  uint64_t v = 0;
  Check(sh::ParseHex16("AF63DC4C8601EC8C", v) && v == 0xAF63DC4C8601EC8Cull && !sh::ParseHex16("af63dc4c8601ec8c", v) && !sh::ParseHex16("AF63DC4C", v), "ParseHex16: 16 upper-case digits only");
  Check(sh::kKeepEntries == 8, "kKeepEntries is the documented housekeeping number");

  // (SEAT2-HB) the image stamp: A, B and the footer count (contiguous, payload +28 .. +1790648); the 28-byte header (sizes, versions, byte sum) does not
  std::vector<uint8_t> img(sh::kImgEnd);
  for (size_t i = 0; i < img.size(); ++i) img[i] = uint8_t((i * 31 + 7) & 255);
  const uint64_t s0 = sh::ImageStamp(img.data());
  auto changed = [&](size_t at) {
    std::vector<uint8_t> c = img;
    c[at] ^= 0x5A;
    return sh::ImageStamp(c.data());
  };
  Check(s0 == sh::ImageStamp(img.data()) && s0 != 0, "the image stamp is stable");
  Check(sh::kImgA + sh::kImgASize == sh::kImgB && sh::kImgB + sh::kImgBSize == sh::kImgFoot && sh::kImgFoot + sh::kImgFootSize == sh::kImgEnd && sh::kImgEnd == 1790648,
        "the segments are the game's own (header 28, A 0x1224AC, B 0x92DC0, footer 48, back to back)");
  Check(changed(sh::kImgA) != s0 && changed(sh::kImgB - 1) != s0, "a byte of block A changes the stamp");
  Check(changed(sh::kImgB) != s0 && changed(sh::kImgB + sh::kImgBSize - 1) != s0, "a byte of block B changes the stamp");
  Check(changed(sh::kImgFoot) != s0 && changed(sh::kImgEnd - 1) != s0, "a byte of the footer changes the stamp");
  Check(changed(0) == s0 && changed(27) == s0, "the header is not in the stamp");

  // (SEAT2-HB) the trim never drops the entry of the image this process loaded
  sh::Record rec;
  for (uint64_t i = 1; i <= 8; ++i) sh::Prepend(rec, MakeE(i, {{397, A}}));
  Check(sh::Prepend(rec, MakeE(9, {{397, B}}), uint64_t(1)) && rec.entries.size() == 8 && rec.Find(1) && !rec.Find(2) && rec.entries.front().stamp == 9,
        "a protected oldest entry stays, the next oldest goes");
  Check(sh::Prepend(rec, MakeE(10, {{397, B}}), uint64_t(1)) && rec.Find(1) && !rec.Find(3) && rec.entries.size() == 8, "and again: the protected one still stays");
  Check(sh::Prepend(rec, MakeE(11, {{397, B}})) && !rec.Find(1) && rec.entries.size() == 8, "no protection: the oldest goes as before");
}

// ================================================================ (review of H-B) pending after a check, fresh O6, the status clause
static void TestReviewHB() {
  // a seat leaves the pending list only when the game's loop asked about it in that check
  Check(sh::PendingAfterCheck({397, 400, 450}, {400}) == std::set<int>{397, 450}, "pending: only the answered seat leaves the list");
  Check(sh::PendingAfterCheck({397}, {}) == std::set<int>{397}, "pending: a check that never reached the seat keeps it");
  Check(sh::PendingAfterCheck({397}, {397, 398}).empty(), "pending: an answered seat is done (an answered seat that was not pending changes nothing)");
  // O6 of a never-edited character: the counters 0, the byte 1 (sub_827F2DC8 +11C, sub_827D8A38: 400 bytes of 1 at B + 0x92C30)
  const std::array<uint8_t, 5> f = sh::FreshCnt();
  Check(f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0 && f[4] == 1, "fresh O6: u16 0, u16 0, u8 1 (the game's new-save values)");
  // the status clause on line 1 of game_status.txt
  const std::string cl = std::string(", 2 ") + sh::kUnseenClause;
  Check(sh::WithUnseenClause("in 12", 2) == "in 12" + cl, "status: the clause is appended to 'in N'");
  Check(sh::WithUnseenClause("in 12" + cl, 0) == "in 12", "status: n = 0 removes it");
  Check(sh::WithUnseenClause("in 12" + cl, 3) == "in 12, 3 " + std::string(sh::kUnseenClause), "status: a second load replaces it, never twice");
  const std::string rb = "in 12, 2 imported superstar(s) need a rebuild: open the importer";
  Check(sh::WithUnseenClause(rb, 1) == rb + ", 1 " + sh::kUnseenClause && sh::WithUnseenClause(rb + ", 1 " + sh::kUnseenClause, 0) == rb,
        "status: an earlier clause stays first and is kept when it is removed");
  Check(sh::WithUnseenClause("out the pack is not built yet", 2) == "out the pack is not built yet", "status: an 'out' line is left as it is");
}

// ================================================================ (S110 UNISTRIP) which slot of a card match is stripped
static void TestUniverseStrip() {
  namespace us = wwe2k14::universe_strip;
  const std::set<int> gone{397};
  Check(us::StripSlot(0, true, false, 2, 4, 397, gone), "strip: a gone id in a bit-3 ordinary match is stripped");
  Check(!us::StripSlot(2, true, false, 2, 4, 397, gone), "strip: a played match (status 2) is kept");
  Check(!us::StripSlot(0, false, false, 2, 4, 397, gone), "strip: a match the game's builder rebuilds is left to the game");
  Check(!us::StripSlot(0, true, true, 2, 4, 397, gone), "strip: a bare-id kind (rumble) is kept");
  Check(!us::StripSlot(0, true, false, 6, 8, 397, gone), "strip: slot 6 and beyond is refused");
  Check(us::StripSlot(0, true, false, 5, 6, 397, gone), "strip: slot 5 is the last one");
  Check(!us::StripSlot(0, true, false, 4, 4, 397, gone), "strip: a slot at or past the match's count is refused");
  Check(!us::StripSlot(0, true, false, 2, 4, 398, gone), "strip: an id that is not gone is kept");
  Check(!us::StripSlot(0, true, false, 2, 4, 397, {}), "strip: nothing gone, nothing stripped");
  Check(us::IdOffset(0) == 16 && us::IdOffset(5) == 76 && us::FlagByte(0) == 24 && us::FlagByte(5) == 84, "strip: the id offset is 16 + 12k and the flag byte 24 + 12k");
  // (review C3) the game's builder walks auto ordinals 0..6 only: a special match (ordinal -1) and an auto match at ordinal 7.. are left by it
  Check(us::LeftByBuilder(true, -1), "strip: a special match (bit 3, ordinal -1) is left by the builder");
  Check(!us::LeftByBuilder(false, 0) && !us::LeftByBuilder(false, 6), "strip: auto ordinals 0..6 are rebuilt by the builder");
  Check(us::LeftByBuilder(false, 7) && us::LeftByBuilder(false, 9), "strip: auto ordinals 7..9 are left by the builder");
  Check(!us::LeftByBuilder(false, -1), "strip: an auto match the game could not place (-1) is left to the game");
  // (review C1) the replacer found nobody: the byte +8 goes back as it was; replaced: the game's byte stays
  Check(us::FlagAfterReplace(0x05, 0x45, 397, 397) == 0x05, "strip: not replaced (same id) -> byte +8 put back, our 0x40 gone");
  Check(us::FlagAfterReplace(0x05, 0x05, 397, 120) == 0x05 && us::FlagAfterReplace(0x05, 0x01, 397, 120) == 0x01, "strip: replaced -> the game's byte +8 kept");
  // (review) the participant ids of the bare kinds, read for the 'card before strip' line
  Check(us::BareIdOffset(0) == 16 && us::BareIdOffset(39) == 94, "strip: a bare-id kind holds its ids at 16 + 2k");
  Check(us::SlotCap(true, false) == 40 && us::SlotCap(false, true) == 16 && us::SlotCap(false, false) == 6, "strip: 40 ids for types 20..23, 16 for the bare kind, 6 records ordinary");
  // (S110 UNIDIAG) the four states of the summary line and the per-slot verdicts
  Check(us::Summarise(0, 0, 0) == us::CardState::NoCard, "card: no match reached = no card");
  Check(us::Summarise(10, 0, 0) == us::CardState::Empty, "card: matches reached, none booked = an empty card");
  Check(us::Summarise(10, 3, 0) == us::CardState::NoneNamed, "card: booked, none names a gone seat");
  Check(us::Summarise(10, 3, 1) == us::CardState::Named, "card: a gone seat is named");
  Check(std::string(us::SlotVerdictText(2, true, false, false)) == "played (kept)", "verdict: a played match");
  Check(std::string(us::SlotVerdictText(0, true, true, false)) == "bare-id kind (kept, no game replacer)", "verdict: the bare-id kind");
  Check(std::string(us::SlotVerdictText(0, false, false, false)) == "left to the game's builder", "verdict: left to the builder");
  Check(std::string(us::SlotVerdictText(0, true, false, true)) == "strip", "verdict: strip");
}

// ================================================================ (S110 UNIAVAIL) the builder's hold: keyed by seat and day, read only inside a job's frame
static void TestUniverseRebuild() {
  namespace ur = wwe2k14::universe_rebuild;
  const uint32_t D = 5000;
  ur::Held h;
  // a check that answered 397 gone on day D: a job on day D gets {397}
  ur::NoteCheck(h, D, {397});
  Check(ur::SeatsForJob(h, D) == std::set<int>{397}, "rebuild: a job on the check's day sees seat 397");
  Check(ur::SeatsForJob(h, D) == std::set<int>{397}, "rebuild: a second job of the same day sees it again (the hold covers every job of the day)");
  // inside the frame only 397 is inactive; after it nothing is, on the same thread
  {
    ur::JobFrame f(ur::SeatsForJob(h, D));
    Check(ur::RowInactiveInJob(397), "rebuild: a held seat is row-inactive inside the job's frame");
    Check(!ur::RowInactiveInJob(398), "rebuild: another seat is not (keyed by seat)");
  }
  Check(!ur::RowInactiveInJob(397), "rebuild: after the frame the seat is active again on the same thread");
  // a same-day check merges
  ur::NoteCheck(h, D, {398});
  Check(ur::SeatsForJob(h, D) == std::set<int>({397, 398}), "rebuild: a check on the same day merges its seats");
  ur::NoteCheck(h, D, {});
  Check(ur::SeatsForJob(h, D) == std::set<int>({397, 398}), "rebuild: a check that answered nobody changes nothing");
  // a new-day check replaces
  ur::NoteCheck(h, D + 1, {399});
  Check(ur::SeatsForJob(h, D + 1) == std::set<int>{399}, "rebuild: a check on a new day replaces the hold (no seat of the old day stays)");
  // the first job on another day ends the hold for good
  ur::Held g;
  ur::NoteCheck(g, D, {397});
  Check(ur::SeatsForJob(g, D + 1).empty(), "rebuild: a job on day + 1 sees nothing");
  Check(ur::SeatsForJob(g, D).empty(), "rebuild: a later job on the old day sees nothing (the hold ended for good)");
  // the answer is the job thread's own
  {
    std::promise<void> in_frame, may_leave;
    std::shared_future<void> leave = may_leave.get_future().share();
    bool worker_seen = false;
    std::thread t([&] {
      ur::JobFrame f(std::set<int>{397});
      worker_seen = ur::RowInactiveInJob(397);
      in_frame.set_value();
      leave.wait();
    });
    in_frame.get_future().wait();
    Check(!ur::RowInactiveInJob(397), "rebuild: while a worker's frame holds 397, the main thread's answer is the game's");
    may_leave.set_value();
    t.join();
    Check(worker_seen, "rebuild: the worker inside its frame sees 397 inactive");
  }
  // a load never keeps the last save's hold
  {
    ur::Held before, entry;
    before.day = D, before.seats = {397};
    entry.day = D + 7, entry.seats = {410};
    Check(ur::HeldAfterLoad(before, true, entry).seats == std::set<int>{410}, "rebuild: a load with an entry holds the entry's seats");
    Check(ur::HeldAfterLoad(before, false, entry).seats.empty(), "rebuild: a load with no entry holds nothing (the last save's hold does not leak)");
  }
  // the record line
  {
    sh::Record r;
    sh::Entry e = MakeE(0x1111, {{397, A}, {398, B}});
    e.held.day = D, e.held.seats = {397, 398};
    r.entries.push_back(e);
    r.entries.push_back(MakeE(0x2222, {{397, A}}));
    const std::string text = sh::RecordText(r);
    Check(Has(text, "held 5000 397 398\n"), "rebuild: the entry writes 'held <day> <seat> ...'");
    Check(text.find("held", text.find("entry 0000000000002222")) == std::string::npos, "rebuild: an entry with no hold writes no held line");
    sh::Record back;
    std::string err;
    const bool ok = sh::ParseRecord(text, back, err);
    const sh::Entry* b = ok ? back.Find(0x1111) : nullptr;
    Check(b && b->held.day == D && b->held.seats == std::set<int>({397, 398}), "rebuild: the held line round-trips through RecordText / ParseRecord");
    const sh::Entry* b2 = ok ? back.Find(0x2222) : nullptr;
    Check(b2 && b2->held.seats.empty(), "rebuild: an entry with no held line reads back with none");
    // a seat outside 100..499, a bad day, or no seat refuses the entry
    for (const char* bad : {"held 5000 99", "held 5000 500", "held x 397", "held 5000", "held 5000 397 abc", "held 5000 397\nheld 5000 398"}) {
      const std::string t2 = std::string("seats 1\nentry 0000000000003333 frame -\nseat 397 ") + A + "\n" + bad + "\nend\n";
      sh::Record r2;
      std::string e2;
      const bool ok2 = sh::ParseRecord(t2, r2, e2);
      Check(ok2 && !r2.Find(0x3333), std::string("rebuild: the entry is refused for '") + bad + "'");
    }
  }
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // (review) every FAIL line reaches the selftest even if a later case dies
  const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::path("out") / "handover_unit";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);
  const std::function<void()> tests[] = {TestFnv, TestPlan, TestOwners, [&] { TestRecord(work); }, [&] { TestState(work); }, [&] { TestExportDecision(work); }, TestRetarget, TestReviewHB, TestUniverseStrip, TestUniverseRebuild};
  for (const auto& t : tests) {
    try {
      t();
    } catch (const std::exception& e) {  // a lookup that throws is a failed check, not a crash
      Check(false, std::string("a test threw: ") + e.what());
    }
  }
  std::printf("handover_unit: %d checks, %d failed\n", g_checks, g_fail);
  std::printf(g_fail ? "FAIL handover_unit\n" : "PASS handover_unit\n");
  return g_fail ? 1 : 0;
}
