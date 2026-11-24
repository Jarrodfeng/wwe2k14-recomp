// S110 LIBSIZE lane 3: host unit for src\seat_alias_plan.h (which nodes are ours, which alias nodes to write and which went empty, the EV file's stamp).
// No game files: every row is made up; the unit writes only under the folder it is given (argv[1], default out\alias_plan_unit).
//   alias_plan_unit.exe [work dir]     every check must PASS (exit 0).   Built and run (with its planted twins) by tools\seats\seats_selftest.py.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "seat_alias_plan.h"

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

struct R { std::string name; uint32_t sector, size; };
static void Dir(std::vector<uint8_t>& v, const char* tag, int kind, const std::vector<R>& rows) {
  const size_t rs = kind ? 16 : 12;
  uint8_t w[4];
  v.insert(v.end(), tag, tag + 4);
  sb::PutBe32(w, uint32_t(rows.size() * rs / 4) << 20 | (kind ? 0x1000u : 0u)), v.insert(v.end(), w, w + 4);
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
using Rows = std::map<std::string, std::vector<uint8_t>>;

static sc::Alias A(const char* tag, uint8_t kind, std::string game, std::string ours) {
  sc::Alias a;
  a.tag = tag, a.kind = kind;
  game.resize(sc::AliasNameLen(kind), ' '), ours.resize(sc::AliasNameLen(kind), ' ');
  a.game = game, a.ours = ours;
  return a;
}

static std::vector<uint8_t> Slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// an EV file with two blobs (made-up bytes: BuildEvFile does not look inside)
static sb::EvFile MakeEv(uint8_t fill, size_t n2 = 700) {
  std::vector<sb::EvIn> in;
  in.push_back({"100", std::vector<uint8_t>(300, fill)});
  in.push_back({"6100", std::vector<uint8_t>(n2, uint8_t(fill + 1))});
  sb::EvFile f;
  std::string err;
  if (!sb::BuildEvFile(in, f, err)) std::printf("FAIL  BuildEvFile: %s\n", err.c_str()), ++g_fail;
  return f;
}

int main(int argc, char** argv) {
  const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::path("out") / "alias_plan_unit";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);

  // ---- 1. which nodes are ours: by ADDRESS (an alias node carries the game's own path)
  {
    std::vector<uint8_t> a, g;
    Dir(a, "EVZ ", 1, {{"9997", 66, 66}});  // our alias node's row (a name the game's own file does not hold)
    Dir(g, "EVZ ", 1, {{"9997", 10, 20}});  // a game file that does hold it
    std::vector<al::NodeView> game = {{"ENTRANCE_DLC04.PAC", Term(a), false}, {"ENTRANCE_DLC04.PAC", Term(g), false}, {"EVTMOV.PAC", Term(g), false}};
    const std::vector<uint32_t> addrs = {0x1000, 0x2000, 0x3000};
    std::string p;
    uint32_t sec = 0, size = 0;
    Check(al::FindFirst(game, "EVZ ", "9997    ", p, sec, size) && sec == 66, "before marking, the lookup answers from our own node (the path alone cannot tell it)");
    Check(al::MarkOurs(game, addrs, {0x1000}) == 1 && game[0].ours && !game[1].ours && !game[2].ours, "MarkOurs flags the node at the address and no other");
    Check(al::FindFirst(game, "EVZ ", "9997    ", p, sec, size) && sec == 10 && size == 20 && p == "ENTRANCE_DLC04.PAC", "after marking, the lookup skips our node and resolves in the game's own");
    std::vector<al::NodeView> g2 = game;
    for (auto& n : g2) n.ours = false;
    Check(al::MarkOurs(g2, {0x1000, 0x2000}, {0x1000, 0x2000, 0x3000}) == 0 && !g2[0].ours && !g2[1].ours && !g2[2].ours, "MarkOurs with addresses that do not match the nodes marks nothing");
    Check(al::MarkOurs(g2, addrs, {}) == 0 && !g2[0].ours, "MarkOurs with no address of ours marks nothing");
    Check(al::MarkOurs(g2, {0, 0x2000, 0x3000}, {0}) == 0 && !g2[0].ours, "a node whose address is 0 (unknown) is never ours");
    Check(al::MarkOurs(g2, addrs, {0x1000, 0x3000}) == 2 && g2[0].ours && !g2[1].ours && g2[2].ours, "two addresses of ours: two nodes flagged");
  }

  // ---- 2. the nodes to write
  {
    const Rows empty;
    Rows w1;
    w1["ENTRANCE_DLC04.PAC"] = {1, 2, 3, 4};
    w1["GM_DLC05.PAC"] = {5, 6, 7, 8};
    std::vector<std::string> gone;
    Rows put = al::PutPlan(w1, empty, &gone);
    Check(put == w1 && gone.empty(), "first plan: every wanted node is written, nothing is emptied");
    put = al::PutPlan(w1, w1, &gone);
    Check(put.empty() && gone.empty(), "the same rows again: nothing is written (a write needs the worker idle)");
    Rows w2 = w1;
    w2["GM_DLC05.PAC"] = {9, 9, 9, 9};
    put = al::PutPlan(w2, w1, &gone);
    Check(put.size() == 1 && put.count("GM_DLC05.PAC") && put["GM_DLC05.PAC"] == w2["GM_DLC05.PAC"] && gone.empty(), "a changed node is written, an unchanged one is not");
    Rows w3;
    w3["GM_DLC05.PAC"] = w1["GM_DLC05.PAC"];  // a vacate: the entrance file has no rows any more
    put = al::PutPlan(w3, w1, &gone);
    Check(put.size() == 1 && put.count("ENTRANCE_DLC04.PAC") && put["ENTRANCE_DLC04.PAC"] == al::EmptyNodeRows() && gone == std::vector<std::string>{"ENTRANCE_DLC04.PAC"},
          "a path that has gone empty is re-pointed to the empty node and named");
    Rows last = w1;
    last["ENTRANCE_DLC04.PAC"] = al::EmptyNodeRows();
    put = al::PutPlan(w3, last, &gone);
    Check(put.empty() && gone.empty(), "a node that is empty already is not written again");
    put = al::PutPlan(w1, last, &gone);
    Check(put.size() == 1 && put["ENTRANCE_DLC04.PAC"] == w1["ENTRANCE_DLC04.PAC"], "a path that has rows again after being emptied is written with them");
    put = al::PutPlan(empty, w1, &gone);
    Check(put.size() == 2 && gone.size() == 2, "nobody seated: every node made is emptied");
    // the empty node scans clean, answers nothing, and is one the game's walk reads as an ordinary node
    size_t body = 0;
    std::string why;
    const std::vector<uint8_t>& e = al::EmptyNodeRows();
    Check(sb::ScanDirs(e.data(), e.size(), body, why) && body + 4 == e.size() && sb::Be32(&e[body]) == 0xFFFFFFFFu, "the empty node scans clean and ends in the terminator word");
    std::vector<al::NodeView> g = {{"X.PAC", e, false}};
    std::string p;
    uint32_t s = 0, z = 0;
    Check(!al::FindFirst(g, "SSEM", "0100", p, s, z) && !al::FindFirst(g, "EVZ ", "100     ", p, s, z), "the empty node holds no row for any name");
    Check(e.size() >= 8, "the empty node is big enough for the node writer (8 bytes at least)");
  }

  // ---- 3. the cycle through AliasNodes: a bind, a re-bind to another character, a vacate
  {
    std::vector<uint8_t> en, gm, ev2;
    Dir(en, "EVZ ", 1, {{"100", 10, 20}, {"200", 11, 21}});
    Dir(gm, "GMEV", 0, {{"0100", 1, 2}});
    std::vector<al::NodeView> game = {{"ENTRANCE_DLC04.PAC", Term(en), false}, {"GM_DLC05.PAC", Term(gm), false}};
    const std::vector<sc::Alias> w100 = {A("EVZ ", 1, "100", "9997"), A("GMEV", 0, "0100", "9997")};
    const std::vector<sc::Alias> w200 = {A("EVZ ", 1, "200", "9997")};
    Rows last, wanted;
    std::string err;
    std::vector<std::string> gone;
    Check(al::AliasNodes({{398, w100}}, {{398, 11}}, game, wanted, err) && wanted.size() == 2, "bind 1: the character's wants make a node in each game file");
    Rows put = al::PutPlan(wanted, last, &gone);
    last = wanted;  // (the writer's bookkeeping: what each node holds now)
    for (const auto& kv : put) last[kv.first] = kv.second;
    Check(put.size() == 2, "bind 1: both nodes are written");
    Check(al::AliasNodes({{398, w200}}, {{398, 11}}, game, wanted, err) && wanted.size() == 1 && wanted.count("ENTRANCE_DLC04.PAC"), "re-bind: the new character needs the entrance file only");
    put = al::PutPlan(wanted, last, &gone);
    Check(put.size() == 2 && put["GM_DLC05.PAC"] == al::EmptyNodeRows() && gone == std::vector<std::string>{"GM_DLC05.PAC"} && put["ENTRANCE_DLC04.PAC"] == wanted["ENTRANCE_DLC04.PAC"],
          "re-bind: the entrance node gets the new rows, the gm node (no rows now) is emptied");
    for (const auto& kv : put) last[kv.first] = kv.second;
    Check(al::AliasNodes({}, {}, game, wanted, err) && wanted.empty(), "vacate: nobody wants anything");
    put = al::PutPlan(wanted, last, &gone);
    Check(put.size() == 1 && put.count("ENTRANCE_DLC04.PAC") && put["ENTRANCE_DLC04.PAC"] == al::EmptyNodeRows(), "vacate: the entrance node is emptied (the gm node was already)");
    for (const auto& kv : put) last[kv.first] = kv.second;
    put = al::PutPlan(wanted, last, &gone);
    Check(put.empty(), "after the vacate nothing is left to write");
  }

  // ---- 4. the EV file's stamp
  {
    const fs::path f = work / "S398.ev";
    sb::EvFile ev = MakeEv(0x40);
    const std::vector<uint8_t> blobs_before(ev.bytes.begin() + sb::kEvHeader, ev.bytes.end());
    bool wrote = false;
    std::string err;
    Check(al::WriteEvIfChanged(f, ev, "0000AAA1", 398, 11, wrote, err) && wrote, "first write: the file is written");
    std::vector<uint8_t> disk = Slurp(f);
    Check(disk == ev.bytes, "the file on disk is the stamped bytes");
    Check(sb::Be32(&disk[0]) == al::kStampMagic && sb::Be32(&disk[4]) == al::kStampVersion && (sb::Be32(&disk[8]) | sb::Be32(&disk[12])) != 0, "the stamp is in the first 16 bytes of the header");
    bool zero = true;
    for (size_t i = al::kStampBytes; i < sb::kEvHeader; ++i) zero = zero && disk[i] == 0;
    Check(zero && std::vector<uint8_t>(disk.begin() + sb::kEvHeader, disk.end()) == blobs_before, "the rest of the header is zero and the blobs are untouched by the stamp");
    sb::EvFile ev2 = MakeEv(0x40);
    Check(al::WriteEvIfChanged(f, ev2, "0000AAA1", 398, 11, wrote, err) && !wrote, "the same file again: not written (same stamp)");
    {  // the file's date is not touched either: write a marker into the unread part of the file and see it survive
      std::fstream m(f, std::ios::in | std::ios::out | std::ios::binary);
      m.seekp(100), m.put('Z');
    }
    sb::EvFile ev3 = MakeEv(0x40);
    Check(al::WriteEvIfChanged(f, ev3, "0000AAA1", 398, 11, wrote, err) && !wrote && Slurp(f)[100] == 'Z', "a file with the same stamp is left alone (not rewritten)");
    ev3 = MakeEv(0x40);
    Check(al::WriteEvIfChanged(f, ev3, "0000AAA1", 399, 11, wrote, err) && wrote, "another seat: written");
    ev3 = MakeEv(0x40);
    Check(al::WriteEvIfChanged(f, ev3, "0000AAA1", 399, 12, wrote, err) && wrote, "another bank: written");
    ev3 = MakeEv(0x40);
    Check(al::WriteEvIfChanged(f, ev3, "0000AAA2", 399, 12, wrote, err) && wrote, "another character: written");
    ev3 = MakeEv(0x41);
    Check(al::WriteEvIfChanged(f, ev3, "0000AAA2", 399, 12, wrote, err) && wrote, "the same character, seat and bank with other blob bytes: written");
    ev3 = MakeEv(0x41, 3000);
    Check(al::WriteEvIfChanged(f, ev3, "0000AAA2", 399, 12, wrote, err) && wrote && Slurp(f) == ev3.bytes, "a longer file: written whole");
    // a file cut short that keeps the first bytes: the size says it is not the same file
    {
      sb::EvFile e = MakeEv(0x41, 3000);
      al::WriteEvIfChanged(f, e, "0000AAA2", 399, 12, wrote, err);
      fs::resize_file(f, e.bytes.size() - 2048, ec);
      e = MakeEv(0x41, 3000);
      Check(al::WriteEvIfChanged(f, e, "0000AAA2", 399, 12, wrote, err) && wrote && Slurp(f) == e.bytes, "a truncated file with the same first bytes is rewritten");
    }
    // a blob that moves (a name change moves the rows, not the bytes after the header): the rows are in the hash
    sb::EvFile a = MakeEv(0x50), b = MakeEv(0x50);
    b.rows[0].name = "101     ";
    Check(al::EvIdentity("K", 398, 11, a) != al::EvIdentity("K", 398, 11, b), "a renamed entry changes the identity");
    Check(al::EvIdentity("K", 398, 11, a) == al::EvIdentity("K", 398, 11, MakeEv(0x50)), "the identity is the same for the same file");
    // no file at the path, an unwritable folder
    Check(!al::WriteEvIfChanged(work / "nope" / "deeper" / "S1.ev", a, "K", 398, 11, wrote, err) || fs::exists(work / "nope" / "deeper" / "S1.ev"), "a path in a folder that does not exist: made, or refused with a reason");
    sb::EvFile small;
    small.bytes.assign(100, 0);
    Check(!al::WriteEvIfChanged(work / "small.ev", small, "K", 398, 11, wrote, err) && !wrote && !fs::exists(work / "small.ev"), "a file without a header is refused and not written");
  }

  // ---- 5. the retry of a Refresh that could not write every node (review S110 LIBSIZE-3: a refused re-point left the previous guest's rows answering)
  {
    al::RefreshRetry r;
    Check(!r.pending, "nothing owed at the start");
    Check(!r.Done(true) && !r.pending, "a Refresh that wrote everything owes nothing");
    Check(!r.Done(false) && r.pending, "a refused Refresh is owed again");
    Check(!r.Done(true) && !r.pending && r.tries == 0, "the retry that writes everything clears the debt and the count");
    bool gave_up = false;
    int n = 0;
    while (!gave_up && n < 10) gave_up = r.Done(false), ++n;
    Check(gave_up && n == al::RefreshRetry::kTries && !r.pending, "refused kTries times in a row: given up (named once by the caller), not owed any more");
    Check(!r.Done(false) && r.pending && r.tries == 1, "a refusal after giving up starts a new count");
    r.Done(false);
    r.Owe();
    Check(r.pending && r.tries == 0, "an occupant change (a row pass cleared guests) owes a Refresh with a fresh count");
    r.Done(true);
    r.Owe();
    Check(r.pending, "an occupant change owes a Refresh even when the last one wrote everything");
  }

  std::printf("alias_plan_unit: %d check(s), %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
