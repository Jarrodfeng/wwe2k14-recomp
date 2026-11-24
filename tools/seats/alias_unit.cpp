// S110 LIBSIZE lane 1: host unit for src\seat_alias_core.h (the pure half of "a seat's names point at the game's own entries") and the ALIA block / file_form line
// of src\seat_card.h. No game files: every row is made up; the unit writes only under the folder it is given (argv[1], default out\alias_unit).
//   alias_unit.exe [work dir]     every check must PASS (exit 0).   Built and run (with its planted twins) by tools\seats\seats_selftest.py.
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "seat_alias_core.h"

namespace fs = std::filesystem;
namespace al = wwe2k14::seat_alias;
namespace sc = wwe2k14::seat_card;
namespace sb = wwe2k14::seat_bind;

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("FAIL  %s\n", what.c_str());
  }
}

// ---- made-up node rows (the game's memory form: tag, flags, 0, rows; ONE terminator word at the end of a node)
struct R { std::string name; uint32_t sector, size; };
static void Dir(std::vector<uint8_t>& v, const char* tag, int kind, const std::vector<R>& rows, uint32_t extra_flags = 0) {
  const size_t rs = kind ? 16 : 12;
  uint8_t w[4];
  v.insert(v.end(), tag, tag + 4);
  sb::PutBe32(w, uint32_t(rows.size() * rs / 4) << 20 | (kind ? 0x1000u : 0u) | extra_flags), v.insert(v.end(), w, w + 4);
  sb::PutBe32(w, 0), v.insert(v.end(), w, w + 4);
  for (const R& r : rows) {
    std::string n = r.name;
    n.resize(rs - 8, ' ');
    v.insert(v.end(), n.begin(), n.end());
    sb::PutBe32(w, r.sector), v.insert(v.end(), w, w + 4);
    sb::PutBe32(w, r.size), v.insert(v.end(), w, w + 4);
  }
}
static std::vector<uint8_t> Term(std::vector<uint8_t> v) { return v.insert(v.end(), 4, 0xFF), v; }

struct Parsed { std::string tag; int kind; std::vector<R> rows; };
// A result node read back: false when it does not scan clean or does not end in the terminator word.
static bool Parse(const std::vector<uint8_t>& rows, std::vector<Parsed>& out) {
  out.clear();
  size_t body = 0;
  std::string why;
  if (!sb::ScanDirs(rows.data(), rows.size(), body, why) || body + 4 != rows.size() || sb::Be32(&rows[body]) != 0xFFFFFFFFu) return false;
  for (size_t at = 0; at < body;) {
    const uint32_t flags = sb::Be32(&rows[at + 4]), words = flags >> 20, kind = (flags >> 12) & 15;
    const size_t rs = kind ? 16 : 12;
    Parsed p;
    p.tag.assign(reinterpret_cast<const char*>(&rows[at]), 4), p.kind = int(kind);
    for (size_t i = 0; i < words * 4u / rs; ++i) {
      const uint8_t* r = &rows[at + 12 + i * rs];
      p.rows.push_back({std::string(reinterpret_cast<const char*>(r), rs - 8), sb::Be32(r + rs - 8), sb::Be32(r + rs - 4)});
    }
    out.push_back(p);
    at += 12 + size_t(words) * 4;
  }
  return true;
}
// every row of every path as "path|tag|name|sector|size" (names trimmed of their padding)
static std::set<std::string> AllRows(const std::map<std::string, std::vector<uint8_t>>& m, bool& clean) {
  std::set<std::string> s;
  for (const auto& [path, rows] : m) {
    std::vector<Parsed> ps;
    if (!Parse(rows, ps)) clean = false;
    for (const Parsed& p : ps)
      for (const R& r : p.rows) {
        std::string n = r.name;
        while (!n.empty() && n.back() == ' ') n.pop_back();
        s.insert(path + "|" + p.tag + "|" + n + "|" + std::to_string(r.sector) + "|" + std::to_string(r.size));
      }
  }
  return s;
}
static int DirCount(const std::map<std::string, std::vector<uint8_t>>& m, const std::string& path, const std::string& tag) {
  const auto it = m.find(path);
  std::vector<Parsed> ps;
  if (it == m.end() || !Parse(it->second, ps)) return -1;
  int n = 0;
  for (const Parsed& p : ps) n += p.tag == tag;
  return n;
}

static sc::Alias A(const char* tag, uint8_t kind, std::string game, std::string ours) {
  sc::Alias a;
  a.tag = tag, a.kind = kind;
  game.resize(sc::AliasNameLen(kind), ' '), ours.resize(sc::AliasNameLen(kind), ' ');
  a.game = game, a.ours = ours;
  return a;
}

int main(int argc, char** argv) {
  const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::path("out") / "alias_unit";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);

  // ---- the game's node list as the game side reads it (head first): ours first (they are head-inserted), then the game's own files
  std::vector<al::NodeView> game;
  {
    std::vector<uint8_t> o, e, a;
    Dir(a, "EVZ ", 1, {{"100", 66, 66}});
    game.push_back({"ENTRANCE_DLC04.PAC", Term(a), true});  // an alias node of ours: it carries the GAME's path, the flag tells it
    Dir(o, "EVZ ", 1, {{"100", 99, 98}});
    Dir(o, "GMEV", 0, {{"0100", 97, 96}, {"0777", 95, 94}});
    game.push_back({"SSLIB:\\0000AAA1\\PAC\\CH\\CH497.PAC", Term(o)});
    Dir(e, "EVP ", 1, {{"100", 93, 92}});
    game.push_back({"SSEAT:\\S398.EV", Term(e)});
    std::vector<uint8_t> n;
    Dir(n, "EVZ ", 1, {{"100", 77, 77}}, 0x10000u);  // a hex-name directory never answers (and is stepped over)
    Dir(n, "EVZ ", 1, {{"100", 55, 55}}, 0x20000u);  // a directory of level 1 never answers
    Dir(n, "EVZ ", 1, {{"100", 10, 20}, {"10100", 11, 21}, {"6100", 12, 22}});
    Dir(n, "EVP ", 1, {{"100", 5, 5}});
    game.push_back({"ENTRANCE_DLC04.PAC", Term(n)});
    std::vector<uint8_t> n2;
    Dir(n2, "EVZ ", 1, {{"100", 50, 60}});  // a second file with the same name: the first one wins
    game.push_back({"ENTRANCE2_DLC05.PAC", Term(n2)});
    std::vector<uint8_t> gm;
    Dir(gm, "GMEV", 0, {{"0100", 1, 2}, {"0101", 7, 8}});
    game.push_back({"GM_DLC05.PAC", Term(gm)});
    std::vector<uint8_t> pc;
    Dir(pc, "PCE6", 0, {{"0100", 3, 4}});
    game.push_back({"PRTCLS_DLC05.PAC", Term(pc)});
    std::vector<uint8_t> mv;
    Dir(mv, "MOVN", 0, {{"M600", 30, 31}});
    game.push_back({"EVTMOV_DLC04.PAC", Term(mv)});
    std::vector<uint8_t> mh;
    Dir(mh, "SSFA", 0, {{"0100", 40, 41}});
    game.push_back({"MENUHD_DLC.PAC", Term(mh)});
  }
  // ---- (S110 ALIASMISS) a game node's rows are read to the first 0xFF-topped word, never by node +52 (negative for the disc's makers, positive for the DLC's)
  {
    const auto reader = [](const std::vector<uint8_t>& b) {
      return [&b](size_t off, uint32_t& w) {
        if (off + 4 > b.size()) return false;
        w = sb::Be32(&b[off]);
        return true;
      };
    };
    const auto marker = [](std::vector<uint8_t>& b, uint32_t m) {
      uint8_t w[4];
      sb::PutBe32(w, m), b.insert(b.end(), w, w + 4);
    };
    // the disc's arc: FF000000, group 0 (EVZ), FFFF0001, group 1 (GMEV), FFFFFFFF; node 0's rows start right after the first marker
    std::vector<uint8_t> arc;
    marker(arc, 0xFF000000u);
    const size_t g0 = arc.size();
    Dir(arc, "EVZ ", 1, {{"6100", 12, 22}});
    const size_t m1 = arc.size();
    marker(arc, 0xFFFF0001u);
    Dir(arc, "GMEV", 0, {{"0777", 3, 4}});
    marker(arc, 0xFFFFFFFFu);
    const std::vector<uint8_t> rows0(arc.begin() + long(g0), arc.end());
    size_t span = 0;
    Check(al::GameRowsSpan(reader(rows0), 1u << 20, span) && span == (m1 - g0) + 4, "GameRowsSpan: a disc arc's first group ends exactly through the next group's marker (span " + std::to_string(span) + ", want " + std::to_string((m1 - g0) + 4) + ")");
    std::vector<al::NodeView> one(1);
    one[0].path = "PAC.PAC", one[0].rows.assign(rows0.begin(), rows0.begin() + long(span));
    std::string p;
    uint32_t sec = 0, sz = 0;
    Check(al::FindFirst(one, "EVZ ", "6100    ", p, sec, sz) && sec == 12 && sz == 22, "GameRowsSpan: the first group's own EVZ name is found through its span");
    Check(!al::FindFirst(one, "GMEV", "0777", p, sec, sz), "GameRowsSpan: the next group's GMEV name is NOT found in the first node");
    // the last group of the arc ends at FFFFFFFF
    const std::vector<uint8_t> rows1(arc.begin() + long(m1 + 4), arc.end());
    size_t span1 = 0;
    Check(al::GameRowsSpan(reader(rows1), 1u << 20, span1) && span1 == rows1.size(), "GameRowsSpan: the last group ends through the FFFFFFFF word");
    // a DLC-shaped node: one group, FFFFFFFF
    std::vector<uint8_t> dlc;
    Dir(dlc, "EVZ ", 1, {{"6100", 12, 22}});
    marker(dlc, 0xFFFFFFFFu);
    size_t span2 = 0;
    Check(al::GameRowsSpan(reader(dlc), 1u << 20, span2) && span2 == dlc.size(), "GameRowsSpan: a DLC-shaped node (one group, FFFFFFFF) spans all its bytes");
    std::vector<al::NodeView> two(1);
    two[0].path = "DLC.PAC", two[0].rows.assign(dlc.begin(), dlc.begin() + long(span2));
    Check(al::FindFirst(two, "EVZ ", "6100    ", p, sec, sz) && sec == 12, "GameRowsSpan: the DLC-shaped node's name is found");
    // no 0xFF word: refused (the rows run out, and a max that is too small)
    std::vector<uint8_t> open;
    Dir(open, "EVZ ", 1, {{"6100", 12, 22}});
    size_t span3 = 0;
    Check(!al::GameRowsSpan(reader(open), 1u << 20, span3), "GameRowsSpan: rows with no 0xFF word are refused");
    Check(!al::GameRowsSpan(reader(rows0), 8, span3), "GameRowsSpan: a 0xFF word past max is refused");
  }

  const std::vector<sc::Alias> wants = {
      A("EVZ ", 1, "100", "9997"),           A("EVZ ", 1, "10100", std::to_string(sb::PartBank(0, sc::kRefBank))),
      A("EVZ ", 1, "6100", std::to_string(6000 + sc::kRefSeat)), A("GMEV", 0, "0100", "9997"),
      A("PCE6", 0, "0100", "9997"),          A("MOVN", 0, "M600", "M" + std::to_string(500 + sc::kRefSeat)),
      A("SSFA", 0, "0100", "0" + std::to_string(sc::kRefSeat)),
      A("EVZ ", 1, "555", "9997"),           // the game has no such name
  };
  const std::map<int, int> bank = {{398, 12}, {400, 14}, {498, 20}};
  const std::string kEnt = "ENTRANCE_DLC04.PAC", kGm = "GM_DLC05.PAC", kPc = "PRTCLS_DLC05.PAC", kMv = "EVTMOV_DLC04.PAC", kMh = "MENUHD_DLC.PAC";

  // ---- FindFirst: the first node, ours skipped, a hex-name directory never matches
  {
    std::string path;
    uint32_t sec = 0, size = 0;
    Check(al::FindFirst(game, "EVZ ", "100     ", path, sec, size) && path == kEnt && sec == 10 && size == 20, "FindFirst: the first game node that holds the name answers (ours, the hex directory and a later file do not)");
    Check(al::FindFirst(game, "GMEV", "0100", path, sec, size) && path == kGm && sec == 1, "FindFirst: our own node's GMEV row is skipped");
    Check(!al::FindFirst(game, "GMEV", "0777", path, sec, size), "FindFirst: a name only our node holds is not found");
    Check(!al::FindFirst(game, "EVZ ", "100", path, sec, size) && !al::FindFirst(game, "EVZ ", "5555", path, sec, size) && !al::FindFirst(game, "ABCD", "0100", path, sec, size),
          "FindFirst: a name of the wrong length, a missing name and an unknown tag are not found");
    Check(al::IsOurPath("SSLIB:\\X") && al::IsOurPath("sseat:\\S1.EV") && !al::IsOurPath("ENTRANCE_DLC04.PAC"), "IsOurPath: SSLIB: / SSEAT:, any case; a game path is not ours");
  }

  // ---- 1. grouping per path
  std::map<std::string, std::vector<uint8_t>> res1;
  std::vector<std::string> missing;
  std::string err;
  {
    const bool ok = al::AliasNodes({{398, wants}, {400, wants}}, bank, game, res1, err, &missing);
    bool clean = true;
    const std::set<std::string> rows = AllRows(res1, clean);
    Check(ok && err.empty() && clean, "alias nodes: made, every node scans clean and ends in the terminator word: " + err);
    Check(res1.size() == 5 && res1.count(kEnt) && res1.count(kGm) && res1.count(kPc) && res1.count(kMv) && res1.count(kMh), "grouping: one node per game FILE (5 paths), the game's own path as its path (" + std::to_string(res1.size()) + ")");
    Check(DirCount(res1, kEnt, "EVZ ") == 1 && DirCount(res1, kGm, "GMEV") == 1 && DirCount(res1, kMh, "SSFA") == 1, "grouping: ONE directory per tag in a node, however many seats");
    std::vector<Parsed> ps;
    Check(Parse(res1[kEnt], ps) && ps.size() == 1 && ps[0].tag == "EVZ " && ps[0].kind == 1 && ps[0].rows.size() == 6 && Parse(res1[kGm], ps) && ps.size() == 1 && ps[0].kind == 0 && ps[0].rows.size() == 2,
          "grouping: the entrance node holds the 3 + 3 EVZ rows of the two seats and nothing else; the gm node 2 GMEV rows");
    // the seat names: canonical -> seat, with the game's own sector and size
    const std::set<std::string> want398 = {kEnt + "|EVZ |12|10|20",   kEnt + "|EVZ |10012|11|21", kEnt + "|EVZ |6398|12|22", kGm + "|GMEV|0012|1|2",
                                           kPc + "|PCE6|0012|3|4",    kMv + "|MOVN|M898|30|31",    kMh + "|SSFA|0398|40|41"};
    bool all = true;
    for (const std::string& r : want398) all = all && rows.count(r);
    Check(all && rows.size() == 14, "names: seat 398 / bank 12 gets EVZ 12 / 10012 / 6398, GMEV + PCE6 0012, plate M898, render 0398, each with the game's sector and size (" + std::to_string(rows.size()) + " rows)");
    Check(rows.count(kEnt + "|EVZ |14|10|20") && rows.count(kEnt + "|EVZ |10014|11|21") && rows.count(kEnt + "|EVZ |6400|12|22") && rows.count(kMv + "|MOVN|M900|30|31"), "names: seat 400 / bank 14 gets its own");
    Check(!rows.count(kEnt + "|EVZ |100|50|60") && !rows.count(kEnt + "|EVZ |100|77|77") && !rows.count(kEnt + "|EVZ |12|66|66") && !rows.count(kEnt + "|EVZ |12|55|55"),
          "first match: the later file's row, the hex directory's, a level-1 directory's and an alias node of ours (game path, ours = true) never answer");
    bool ours = false;
    for (const std::string& r : rows) ours = ours || r.find("|99|") != std::string::npos || r.find("|97|") != std::string::npos || r.find("|93|") != std::string::npos;
    Check(!ours && res1.count("SSLIB:\\0000AAA1\\PAC\\CH\\CH497.PAC") == 0 && res1.count("SSEAT:\\S398.EV") == 0, "our own nodes (SSLIB: / SSEAT:) are skipped: no row is read from them and none is written for them");
    Check(missing.size() == 2 && missing[0] == "seat 398 EVZ  555     " && missing[1] == "seat 400 EVZ  555     ", "a name the game does not have gives no row, and is named (got " + std::to_string(missing.size()) + ")");
    Check(!rows.count(kEnt + "|EVZ |555|0|0"), "a name missing from the game: no row");
  }

  // ---- 2. a bind changes one seat's rows only (seat 400 takes another character: only GMEV 0101 of the gm file)
  {
    bool c1 = true, c2 = true, c3 = true;
    std::map<std::string, std::vector<uint8_t>> res2, res3, res4;
    const std::vector<sc::Alias> other = {A("GMEV", 0, "0101", "9997")};
    const bool ok = al::AliasNodes({{398, wants}, {400, other}}, bank, game, res2, err);
    const std::set<std::string> a = AllRows(res1, c1), b = AllRows(res2, c2);
    std::set<std::string> gone, added, kept;
    for (const std::string& r : a) (b.count(r) ? kept : gone).insert(r);
    for (const std::string& r : b)
      if (!a.count(r)) added.insert(r);
    bool only400 = true;
    for (const std::string& r : gone) {
      const std::string n = r.substr(r.find('|', r.find('|') + 1) + 1);  // name|sector|size
      only400 = only400 && (n.rfind("14|", 0) == 0 || n.rfind("10014|", 0) == 0 || n.rfind("6400|", 0) == 0 || n.rfind("0014|", 0) == 0 || n.rfind("M900|", 0) == 0 || n.rfind("0400|", 0) == 0);
    }
    Check(ok && c1 && c2 && only400 && gone.size() == 7 && kept.size() == 7, "bind: seat 400 changing character removes ONLY his 7 rows (seat 398's 7 stay identical): gone " + std::to_string(gone.size()) + ", kept " + std::to_string(kept.size()));
    Check(added.size() == 1 && *added.begin() == kGm + "|GMEV|0014|7|8", "bind: seat 400's new character gets his one row, from the entry he names (GMEV 0101 -> 0014)");
    // ---- 3. a vacate removes them
    const bool ok3 = al::AliasNodes({{398, wants}}, bank, game, res3, err);
    const std::set<std::string> v = AllRows(res3, c3);
    bool has400 = false;
    for (const std::string& r : v) has400 = has400 || r.find("|14|") != std::string::npos || r.find("|10014|") != std::string::npos || r.find("|0014|") != std::string::npos || r.find("|M900|") != std::string::npos;
    Check(ok3 && c3 && !has400 && v.size() == 7 && v == std::set<std::string>(kept.begin(), kept.end()), "vacate: with seat 400 gone from the wants, none of his rows is made and seat 398's are unchanged");
    Check(al::AliasNodes({}, bank, game, res4, err) && res4.empty() && al::AliasNodes({{398, {}}}, bank, game, res4, err) && res4.empty(), "vacate: nobody wants anything: no node at all");
  }

  // ---- a seat with no plate of its own (498 / 499): the disc's M998 / M999 answer, no MOVN row
  {
    std::map<std::string, std::vector<uint8_t>> r;
    bool c = true;
    const bool ok = al::AliasNodes({{498, wants}}, bank, game, r, err);
    const std::set<std::string> rows = AllRows(r, c);
    Check(ok && c && !r.count(kMv) && rows.count(kGm + "|GMEV|0020|1|2") && rows.count(kEnt + "|EVZ |20|10|20"), "plate: seat 498 gets no MOVN row (and no MOVN node), everything else as any seat");
  }
  // ---- refusals
  {
    std::map<std::string, std::vector<uint8_t>> r;
    Check(!al::AliasNodes({{399, wants}}, bank, game, r, err) && !err.empty() && r.empty(), "refused: a seat with wants and no bank: " + err);
    sc::Alias bad = A("GMEV", 0, "0100", "9997");
    bad.ours = "99";
    Check(!al::AliasNodes({{398, {bad}}}, bank, game, r, err) && !err.empty() && r.empty(), "refused: an alias whose name is not its kind's length: " + err);
    Check(!al::AliasNodes({{50, wants}}, {{50, 12}}, game, r, err) && r.empty(), "refused: a seat outside 100..499");
    // (S110 LIBSIZE-1 review) `ours` must be a name of the reference identity: a name the re-naming leaves as it is would be registered AS ITSELF (the game's own
    // name, or one another seat makes too), which the "kept once" rule below would then hide. An EVP / EMD want is never an alias (the content carries the seat).
    Check(!al::AliasNodes({{398, {A("EVZ ", 1, "100", "100")}}}, bank, game, r, err) && !err.empty() && r.empty(), "refused: an alias whose own name is not in the reference form (EVZ 100): " + err);
    Check(!al::AliasNodes({{398, {A("GMEV", 0, "0100", "0100")}}}, bank, game, r, err) && !err.empty() && r.empty(), "refused: an alias whose own name is not in the reference form (GMEV 0100): " + err);
    Check(!al::AliasNodes({{398, {A("EVP ", 1, "100", "9997")}}}, bank, game, r, err) && !err.empty() && r.empty(), "refused: an EVP alias (its content carries the seat): " + err);
    // two seats with one bank would make the same EVZ / GMEV / PCE6 names: the second seat's would silently point at the first seat's entries
    Check(!al::AliasNodes({{398, wants}, {400, wants}}, {{398, 12}, {400, 12}}, game, r, err) && !err.empty() && r.empty(), "refused: two seats with wants and one bank: " + err);
  }

  // ---- the rows.bin ALIA block and the card's file_form line (seat_card.h)
  {
    sc::SeatRows rows;
    rows.seat = sc::kRefSeat;
    rows.dat.fill(1), rows.pro.fill(2), rows.dsbd.fill(3), rows.bin5.fill(4);
    rows.has_relo = true;
    rows.names[0] = "A", rows.names[1] = "B", rows.names[2] = "C";
    rows.alias = wants;
    std::string e2;
    Check(sc::WriteRows(work / "rows.bin", rows, e2), "rows: the ALIA block writes: " + e2);
    sc::SeatRows back;
    Check(sc::ReadRows(work / "rows.bin", back, e2) && back.alias.size() == wants.size() && back.alias[0].tag == "EVZ " && back.alias[0].kind == 1 && back.alias[0].game == wants[0].game &&
              back.alias[0].ours == wants[0].ours && back.alias[3].tag == "GMEV" && back.alias[3].kind == 0 && back.alias[3].game == "0100" && back.alias[3].ours == "9997" && back.has_relo,
          "rows: the ALIA block reads back, every tag / kind / name (kind 0 names are 4 bytes, kind 1 names 8)");
    sc::SeatRows none = rows;
    none.alias.clear();
    Check(sc::WriteRows(work / "rows0.bin", none, e2) && sc::ReadRows(work / "rows0.bin", back, e2) && back.alias.empty(), "rows: a character with no alias rows writes no ALIA block and reads back with none");
    sc::SeatRows odd = rows;
    odd.alias[1].ours = "9";
    Check(!sc::WriteRows(work / "rows1.bin", odd, e2) && e2.find("alias") != std::string::npos, "rows: an alias whose name is not its kind's length is refused at write: " + e2);
    // a cut ALIA block (the length word says one more entry than the bytes hold): refused at read
    std::string f;
    {
      FILE* fp = std::fopen((work / "rows.bin").string().c_str(), "rb");
      char buf[65536];
      size_t n;
      while (fp && (n = std::fread(buf, 1, sizeof buf, fp)) > 0) f.append(buf, n);
      if (fp) std::fclose(fp);
    }
    const size_t at = f.find("ALIA");
    Check(at != std::string::npos, "rows: the file holds an ALIA block");
    if (at != std::string::npos) {
      f[at + 7] = char(f[at + 7] - 21);  // block length - 21: the count no longer matches
      FILE* fp = std::fopen((work / "rows2.bin").string().c_str(), "wb");
      std::fwrite(f.data(), 1, f.size(), fp);
      std::fclose(fp);
      Check(!sc::ReadRows(work / "rows2.bin", back, e2) && !e2.empty(), "rows: an ALIA block whose length does not fit its count is refused at read: " + e2);
    }
    sc::Card c;
    c.key = "0000AAA1", c.name = "N", c.short_name = "S";
    Check(sc::FileForm(c) == 0, "card: no file_form line = form 0 (the old per-seat file)");
    sc::SetFileForm(c, sc::kFileForm);
    sc::SetFileForm(c, sc::kFileForm);
    c.inst = {true, sc::kRefSeat, sc::kRefBank, 50, 60, 0xABCDu, 0};
    std::string e3;
    sc::Card c2;
    Check(sc::WriteCard(work / "card.txt", c, &e3) && sc::ReadCard(work / "card.txt", c2, e3) && sc::FileForm(c2) == 2 && c2.extra.size() == 1 && c2.inst.seat == sc::kRefSeat && c2.inst.bank == sc::kRefBank,
          "card: `file_form 2` and the reference inst survive a write / read (once)");
  }

  std::printf("alias_unit: %d checks, %d failed\n", g_checks, g_fail);
  std::printf("%s\n", g_fail ? "alias_unit FAILED" : "alias_unit OK");
  return g_fail ? 1 : 0;
}
