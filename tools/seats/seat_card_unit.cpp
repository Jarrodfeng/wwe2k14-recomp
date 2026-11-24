// S110 SEAT-A0: host unit for src\seat_card.h (card.txt + rows.bin). No game files: every payload byte is a made-up pattern.
//   seat_card_unit.exe     every check must PASS (exit 0).   Built and run (with its planted twins) by tools\seats\seat_card_selftest.py.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "seat_card.h"

namespace sc = wwe2k14::seat_card;
namespace fs = std::filesystem;

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
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static void Spit(const fs::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(s.data(), std::streamsize(s.size()));
}

// the unit's own raw-file builder (big-endian, independent of the header's writer)
static void P32(std::string& s, uint32_t v) { for (int i = 24; i >= 0; i -= 8) s += char(v >> i); }
static std::string Blk(const char* tag, size_t len, int seed) {
  std::string s(tag, 4);
  P32(s, uint32_t(len));
  for (size_t i = 0; i < len; ++i) s += char((i * 7 + size_t(seed)) & 255);
  return s;
}
static std::string Head(int seat = 397) {
  std::string s = "SEATROW1";
  s += char(seat >> 8);
  s += char(seat);
  return s;
}
static std::string Names() { return Blk("NAME", 5, 1) + Blk("NAME", 3, 2) + Blk("NAME", 5, 3); }
static std::string Core() { return Blk("DAT ", 208, 1) + Blk("PRO ", 1276, 2) + Blk("DSBD", 80, 3) + Blk("BIN5", 16, 4); }
static std::string GoodRaw() { return Head() + Core() + Names(); }

static void Fill(sc::SeatRows& w) {
  for (size_t i = 0; i < w.dat.size(); ++i) w.dat[i] = uint8_t(i * 3 + 1);
  for (size_t i = 0; i < w.pro.size(); ++i) w.pro[i] = uint8_t(i * 5 + 2);
  for (size_t i = 0; i < w.dsbd.size(); ++i) w.dsbd[i] = uint8_t(i * 7 + 3);
  w.names[0] = "A", w.names[1] = "B", w.names[2] = "C";
}
static sc::Card Full() {
  sc::Card c;
  c.key = "0A1B2C3D";
  c.name = "Test Name With  Spaces";
  c.short_name = "TNWS";
  c.base = 123;
  c.entrance = 77;
  c.copy_ratings = false;
  c.source = "sources/some dir/x y.pac";
  c.source_sig = 0xDEADBEEF;
  c.render[0] = c.render[2] = true;
  c.seat = 410;
  c.inst = {true, 410, 12, 55, 60, 0x01020304u, 321};
  c.tron_from = 130;
  c.tron_slot = 850;
  c.extra = {{"future_key", "some value 1 2"}, {"lonely", ""}};
  return c;
}
static bool SameCard(const sc::Card& a, const sc::Card& b) {
  return a.version == b.version && a.key == b.key && a.name == b.name && a.short_name == b.short_name && a.base == b.base && a.entrance == b.entrance &&
         a.copy_ratings == b.copy_ratings && a.source == b.source && a.source_sig == b.source_sig && a.render[0] == b.render[0] && a.render[1] == b.render[1] &&
         a.render[2] == b.render[2] && a.seat == b.seat && a.inst == b.inst && a.tron_from == b.tron_from && a.tron_slot == b.tron_slot && a.extra == b.extra;
}

int main() {
  const fs::path dir = fs::temp_directory_path() / ("seat_card_unit_" + std::to_string(std::rand()) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const std::string cp = (dir / "card.txt").string(), rp = (dir / "rows.bin").string();
  std::string err;

  // ---- card round trips
  {
    sc::Card c = Full(), r;
    Check(sc::WriteCard(cp, c, &err), "full card writes: " + err);
    Check(sc::ReadCard(cp, r, err), "full card reads: " + err);
    Check(SameCard(c, r), "full card round trip");
    const std::string t = Slurp(cp);
    Check(Has(t, "\ntron from 130 slot 850\n"), "the tron line is written");
    Check(Has(t, "movie 321"), "the inst line carries the movie word");
    Check(Has(t, "\nrenders A C\n") && Has(t, "\nratings 75\n") && Has(t, "\nseat 410\n"), "renders / ratings / seat lines");
    Check(t.compare(0, 7, "card 1\n") == 0, "the card line is first");
  }
  {
    sc::Card c, r;
    c.key = "00000001";
    Check(sc::WriteCard(cp, c, &err), "minimal card writes");
    Check(sc::ReadCard(cp, r, err), "minimal card reads: " + err);
    Check(SameCard(c, r) && r.seat == -1 && !r.inst.valid && r.entrance == -1 && r.copy_ratings, "minimal card round trip");
    const std::string t = Slurp(cp);
    Check(!Has(t, "tron") && !Has(t, "inst") && !Has(t, "source") && Has(t, "seat none") && Has(t, "renders -"), "minimal card has no tron / inst / source line");
  }
  {  // the tron line: absent when both 0 (or negative), present with only one
    sc::Card c = Full();
    c.tron_from = 0; c.tron_slot = 0; c.inst.movie = 0;
    sc::WriteCard(cp, c, &err);
    std::string t = Slurp(cp);
    Check(!Has(t, "tron") && !Has(t, "movie"), "tron line and movie word omitted when 0");
    c.tron_from = -1; c.tron_slot = -5;
    sc::WriteCard(cp, c, &err);
    Check(!Has(Slurp(cp), "tron"), "tron <= 0 writes no line");
    c.tron_from = 0; c.tron_slot = 900;
    sc::WriteCard(cp, c, &err);
    sc::Card r;
    Check(Has(Slurp(cp), "tron from 0 slot 900") && sc::ReadCard(cp, r, err) && r.tron_from == 0 && r.tron_slot == 900, "slot-only tron round trips");
    c.tron_from = 140; c.tron_slot = 0;
    sc::WriteCard(cp, c, &err);
    Check(sc::ReadCard(cp, r, err) && r.tron_from == 140 && r.tron_slot == 0, "from-only tron round trips");
  }
  {  // unknown key kept and written back unchanged; CRLF read
    Spit(cp, "card 1\r\nkey ABCDEF01\r\nnewer_thing a b  c\r\nname Zed\r\nanother\r\n");
    sc::Card r;
    Check(sc::ReadCard(cp, r, err), "card with unknown keys reads: " + err);
    Check(r.name == "Zed" && r.extra.size() == 2 && r.extra[0].first == "newer_thing" && r.extra[0].second == "a b  c" && r.extra[1].first == "another", "unknown keys kept in order");
    Check(sc::WriteCard(cp, r, &err), "rewrite");
    sc::Card r2;
    Check(sc::ReadCard(cp, r2, err) && r2.extra == r.extra, "unknown keys survive a rewrite");
    Check(Has(Slurp(cp), "newer_thing a b  c\n") && Has(Slurp(cp), "\nanother\n"), "unknown lines written back unchanged");
  }
  // ---- card refusals
  {
    sc::Card r;
    Spit(cp, "card 2\nkey ABCDEF01\n");
    Check(!sc::ReadCard(cp, r, err) && Has(err, "version 2") && Has(err, "version 1"), "version 2 refused, naming both versions: " + err);
    Spit(cp, "card 0\nkey ABCDEF01\n");
    Check(!sc::ReadCard(cp, r, err) && Has(err, "version 0"), "version 0 refused");
    Spit(cp, "key ABCDEF01\ncard 1\n");
    Check(!sc::ReadCard(cp, r, err), "card line not first refused");
    Spit(cp, "card 1\nkey ABCDEF0\n");
    Check(!sc::ReadCard(cp, r, err) && Has(err, "8 upper-case hex"), "7-digit key refused: " + err);
    Spit(cp, "card 1\nkey abcdef01\n");
    Check(!sc::ReadCard(cp, r, err), "lower-case key refused");
    Spit(cp, "card 1\nkey ABCDEG01\n");
    Check(!sc::ReadCard(cp, r, err), "non-hex key refused");
    Spit(cp, "card 1\nkey ABCDEF012\n");
    Check(!sc::ReadCard(cp, r, err), "9-digit key refused");
    Spit(cp, "card 1\nname x\n");
    Check(!sc::ReadCard(cp, r, err) && Has(err, "no key"), "a card with no key refused");
    Spit(cp, "card 1\nkey ABCDEF01\nname a\nname b\n");
    Check(!sc::ReadCard(cp, r, err) && Has(err, "twice"), "a repeated known key refused");
    Spit(cp, "card 1\nkey ABCDEF01\ntron from 3\n");
    Check(!sc::ReadCard(cp, r, err), "a malformed tron line refused");
    Check(!sc::ReadCard((dir / "nope.txt").string(), r, err), "a missing file refused");
    sc::Card c = Full();
    c.key = "abc";
    Check(!sc::WriteCard(cp, c, &err), "WriteCard refuses a bad key");
    c = Full();
    c.version = 2;
    Check(!sc::WriteCard(cp, c, &err), "WriteCard refuses version 2");
    c = Full();
    c.name = "two\nlines";
    Check(!sc::WriteCard(cp, c, &err), "WriteCard refuses a name with a line break");
  }
  // ---- rows round trip, byte-exact
  {
    sc::SeatRows w, r;
    w.seat = 433;
    for (size_t i = 0; i < w.dat.size(); ++i) w.dat[i] = uint8_t(i * 3 + 1);
    for (size_t i = 0; i < w.pro.size(); ++i) w.pro[i] = uint8_t(i * 5 + 2);
    for (size_t i = 0; i < w.dsbd.size(); ++i) w.dsbd[i] = uint8_t(i * 11 + 3);
    for (size_t i = 0; i < w.bin5.size(); ++i) w.bin5[i] = uint8_t(0xF0 + i);
    for (int k = 0; k < 3; ++k) {
      sc::SeatRows::Binp b;
      b.k = uint8_t(k);
      for (size_t i = 0; i < b.row.size(); ++i) b.row[i] = uint8_t(k * 40 + i);
      w.binp.push_back(b);
    }
    for (int k = 0; k < 4; ++k) {
      sc::SeatRows::Lsd l;
      l.list = uint8_t(k == 0 ? 2 : k == 1 ? 3 : k == 2 ? 17 : 16);
      for (size_t i = 0; i < l.row.size(); ++i) l.row[i] = uint8_t(0x80 + k * 9 + i);
      w.lsd.push_back(l);
    }
    w.has_t102 = true;
    w.t102_src_bank = 0x00010203;
    w.names[0] = "N\xC3\xA9me";  // UTF-8 passes through
    w.names[1] = "SHRT";
    w.names[2] = "";
    Check(sc::WriteRows(rp, w, err), "rows write: " + err);
    Check(sc::ReadRows(rp, r, err), "rows read: " + err);
    Check(r.seat == 433 && r.dat == w.dat && r.pro == w.pro && r.dsbd == w.dsbd && r.bin5 == w.bin5 && r.has_t102 && r.t102_src_bank == w.t102_src_bank &&
              r.names[0] == w.names[0] && r.names[1] == w.names[1] && r.names[2] == w.names[2],
          "rows round trip: blocks equal");
    bool same = r.binp.size() == 3 && r.lsd.size() == 4;
    for (size_t i = 0; same && i < 3; ++i) same = r.binp[i].k == w.binp[i].k && r.binp[i].row == w.binp[i].row;
    for (size_t i = 0; same && i < 4; ++i) same = r.lsd[i].list == w.lsd[i].list && r.lsd[i].row == w.lsd[i].row;
    Check(same, "rows round trip: BINP / LSD equal");
    const std::string a = Slurp(rp);
    Check(sc::WriteRows(rp, r, err) && Slurp(rp) == a, "rows file is byte-identical after read + write");
    Check(a.compare(0, 8, "SEATROW1") == 0 && uint8_t(a[8]) == 433 >> 8 && uint8_t(a[9]) == (433 & 255), "framing: magic + big-endian seat");
    Check(a.compare(10, 4, "DAT ") == 0 && uint8_t(a[14]) == 0 && uint8_t(a[17]) == 208, "framing: tag + big-endian length");
    Check(uint8_t(a[18]) == w.dat[0] && uint8_t(a[19]) == w.dat[1], "DAT payload copied through (byte 0 = pattern)");
    w.has_t102 = false;
    sc::WriteRows(rp, w, err);
    Check(sc::ReadRows(rp, r, err) && !r.has_t102, "no T102 block round trips");
  }
  // ---- rows refusals
  {
    sc::SeatRows r;
    Spit(rp, GoodRaw());
    Check(sc::ReadRows(rp, r, err), "the hand-built good file reads: " + err);
    std::string g = GoodRaw();
    Spit(rp, g.substr(0, g.size() - 3));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "truncated"), "truncated at the end refused: " + err);
    std::string h = Head() + Blk("DAT ", 208, 1) + Blk("PRO ", 1276, 2);
    Spit(rp, h.substr(0, h.size() - 100));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "truncated"), "truncated inside a fixed block refused: " + err);
    Spit(rp, g.substr(0, 5));
    Check(!sc::ReadRows(rp, r, err), "a file shorter than the magic refused");
    Spit(rp, g.substr(0, 10) + "DAT ");
    Check(!sc::ReadRows(rp, r, err) && Has(err, "truncated"), "truncated block header refused");
    std::string bad = g;
    bad[7] = '2';
    Spit(rp, bad);
    Check(!sc::ReadRows(rp, r, err) && Has(err, "magic"), "bad magic refused: " + err);
    Spit(rp, Head() + Blk("DAT ", 209, 1) + Blk("PRO ", 1276, 2) + Blk("DSBD", 80, 3) + Blk("BIN5", 16, 4) + Names());
    Check(!sc::ReadRows(rp, r, err) && Has(err, "DAT") && Has(err, "208"), "DAT of 209 bytes refused: " + err);
    Spit(rp, Head() + Blk("DAT ", 207, 1) + Blk("PRO ", 1276, 2) + Blk("DSBD", 80, 3) + Blk("BIN5", 16, 4) + Names());
    Check(!sc::ReadRows(rp, r, err), "DAT of 207 bytes refused");
    Spit(rp, Head() + Blk("DAT ", 208, 1) + Blk("PRO ", 1276, 2) + Blk("DSBD", 81, 3) + Blk("BIN5", 16, 4) + Names());
    Check(!sc::ReadRows(rp, r, err) && Has(err, "DSBD"), "DSBD of 81 bytes refused");
    Spit(rp, GoodRaw() + Blk("BINP", 20, 1));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "BINP"), "BINP of 20 bytes (no k) refused");
    Spit(rp, GoodRaw() + Blk("LSD ", 8, 1));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "LSD"), "LSD of 8 bytes (no list) refused");
    Spit(rp, GoodRaw() + Blk("T102", 2, 1));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "T102"), "T102 of 2 bytes refused");
    Spit(rp, GoodRaw() + Blk("XYZW", 4, 1));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "unknown tag") && Has(err, "XYZW"), "unknown tag refused: " + err);
    // a pattern block's first payload byte is its seed: BINP seed = leaf k, LSD seed = list
    std::string dup = Head() + Core() + Names() + Blk("BINP", 21, 2) + Blk("BINP", 21, 3) + Blk("BINP", 21, 2);
    Spit(rp, dup);
    Check(!sc::ReadRows(rp, r, err) && Has(err, "BINP leaf 2 twice"), "a second BINP with the same leaf refused: " + err);
    std::string ok = Head() + Core() + Names();
    for (int i = 0; i < sc::kBinpLeaves; ++i) ok += Blk("BINP", 21, i);
    Spit(rp, ok);
    Check(sc::ReadRows(rp, r, err) && r.binp.size() == size_t(sc::kBinpLeaves), "one BINP per leaf 0..4 accepted: " + err);
    dup = Head() + Core() + Names() + Blk("LSD ", 9, 17) + Blk("LSD ", 9, 2) + Blk("LSD ", 9, 17);
    Spit(rp, dup);
    Check(!sc::ReadRows(rp, r, err) && Has(err, "LSD list 17 twice"), "a second LSD with the same list refused: " + err);
    ok = Head() + Core() + Names();
    for (int i = 0; i < 256; ++i) ok += Blk("LSD ", 9, i);
    Spit(rp, ok);
    Check(sc::ReadRows(rp, r, err) && r.lsd.size() == 256, "256 distinct LSD lists accepted (no list number restricted): " + err);
    Spit(rp, Head() + Blk("BINP", 21, 0) + Blk("DAT ", 208, 1) + Blk("PRO ", 1276, 2) + Blk("DSBD", 80, 3) + Names());
    Check(!sc::ReadRows(rp, r, err) && Has(err, "BIN5"), "a missing BIN5 refused");
    Spit(rp, Head() + Core() + Blk("NAME", 5, 1) + Blk("NAME", 3, 2));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "NAME"), "two NAME blocks refused");
    Spit(rp, GoodRaw() + Blk("NAME", 1, 1));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "NAME"), "four NAME blocks refused");
    Spit(rp, Head() + Core() + Core() + Names());
    Check(!sc::ReadRows(rp, r, err) && Has(err, "twice"), "a repeated DAT refused");
    Spit(rp, Head() + Core() + Blk("NAME", sc::kMaxNameBytes + 1, 1) + Blk("NAME", 1, 1) + Blk("NAME", 1, 1));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "NAME"), "an over-long NAME refused");
    Spit(rp, Head() + Blk("BINP", 21, 5) + Core() + Names());  // k = 5 (first payload byte = seed 5)
    Check(!sc::ReadRows(rp, r, err) && Has(err, "BINP leaf"), "BINP leaf 5 refused");
    Check(!sc::ReadRows((dir / "nope.bin").string(), r, err), "a missing rows file refused");
    sc::SeatRows w;
    sc::SeatRows::Binp bp;
    bp.k = 1;
    w.binp = {bp, bp};
    Check(!sc::WriteRows(rp, w, err) && Has(err, "BINP leaf 1 twice"), "WriteRows refuses a repeated BINP leaf: " + err);
    w.binp.clear();
    sc::SeatRows::Lsd ls;
    ls.list = 9;
    w.lsd = {ls, ls};
    Check(!sc::WriteRows(rp, w, err) && Has(err, "LSD list 9 twice"), "WriteRows refuses a repeated LSD list: " + err);
    w.lsd.clear();
    w.names[1] = std::string(size_t(sc::kMaxNameBytes) + 1, 'x');
    Check(!sc::WriteRows(rp, w, err), "WriteRows refuses an over-long name");
  }
  // ---- the relocation list (RELO, S110 SEAT3-R)
  {
    auto entry = [](const char* tag, int sub, int off, int kind, int arg) {
      std::string e(tag, 4);
      e += char(sub), e += char(off >> 8), e += char(off), e += char(kind), e += char(arg);
      return e;
    };
    auto relo_blk = [&](const std::vector<std::string>& v) {
      std::string p;
      P32(p, uint32_t(v.size()));
      for (const std::string& e : v) p += e;
      std::string s("RELO", 4);
      P32(s, uint32_t(p.size()));
      return s + p;
    };
    sc::SeatRows w, r;
    w.seat = 433;
    Fill(w);
    sc::SeatRows::Binp bp;
    bp.k = 3;
    w.binp = {bp};
    sc::SeatRows::Lsd ls;
    ls.list = 17;
    w.lsd = {ls};
    auto add = [&](const char* tag, int sub, int off, sc::RelKind k, int arg) {
      sc::Reloc e;
      e.tag = tag, e.sub = uint8_t(sub), e.off = uint16_t(off), e.kind = k, e.arg = uint8_t(arg);
      w.relo.push_back(e);
    };
    w.has_relo = true;
    add("DAT ", 0, 4, sc::RelKind::Seat16, 0), add("DAT ", 0, 120, sc::RelKind::Key16, 0), add("DAT ", 0, 6, sc::RelKind::Text16, 2), add("DAT ", 0, 21, sc::RelKind::Pack8, 0);
    add("DAT ", 0, 6 + 4, sc::RelKind::Rank16, 0), add("PRO ", 0, 580, sc::RelKind::Bank16, 0), add("PRO ", 0, 640, sc::RelKind::PartBank16, 4), add("DSBD", 0, 2, sc::RelKind::Key16, 0);
    add("BIN5", 0, 14, sc::RelKind::PartBank16, 0), add("BINP", 3, 4, sc::RelKind::Text32, 0), add("BINP", 3, 16, sc::RelKind::Unlock16, 0), add("LSD ", 17, 6, sc::RelKind::Unlock16, 0);
    Check(sc::WriteRows(rp, w, err), "rows with a RELO list write: " + err);
    Check(sc::ReadRows(rp, r, err), "rows with a RELO list read: " + err);
    bool same = r.has_relo && r.relo.size() == w.relo.size();
    for (size_t i = 0; same && i < w.relo.size(); ++i)
      same = r.relo[i].tag == w.relo[i].tag && r.relo[i].sub == w.relo[i].sub && r.relo[i].off == w.relo[i].off && r.relo[i].kind == w.relo[i].kind && r.relo[i].arg == w.relo[i].arg;
    Check(same, "RELO round trip: every entry (tag, sub, offset, kind, arg) equal, in order");
    const std::string a = Slurp(rp);
    Check(sc::WriteRows(rp, r, err) && Slurp(rp) == a, "RELO: the file is byte-identical after read + write");
    const size_t at = a.find("RELO");
    Check(at != std::string::npos && uint8_t(a[at + 7]) == 4 + 9 * 12 && a[at + 8] == 0 && a[at + 11] == 12 && a.compare(at + 12, 4, "DAT ") == 0 && uint8_t(a[at + 16]) == 0 &&
              uint8_t(a[at + 17]) == 0 && uint8_t(a[at + 18]) == 4 && uint8_t(a[at + 19]) == uint8_t(sc::RelKind::Seat16),
          "RELO framing: tag, big-endian length 4 + 9 x 12, count 12, then the entries (tag, sub, big-endian offset, kind, arg)");
    // rows with no RELO block still read (an old importer's), and say so
    Spit(rp, GoodRaw());
    Check(sc::ReadRows(rp, r, err) && !r.has_relo && r.relo.empty(), "rows without a RELO block read: no list (not re-targetable)");
    // hand-built files
    auto raw = [&](const std::string& relo) { return Head() + Core() + Names() + Blk("BINP", 21, 3) + Blk("LSD ", 9, 17) + relo; };
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 1, 0), entry("BINP", 3, 4, 4, 0), entry("LSD ", 17, 2, 7, 0)})));
    Check(sc::ReadRows(rp, r, err) && r.has_relo && r.relo.size() == 3, "a hand-built RELO block reads: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 1, 0)}) + relo_blk({entry("DAT ", 0, 6, 1, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "RELO twice"), "a second RELO block refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 0, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "kind"), "kind 0 refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 10, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "kind"), "kind 10 refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 3, 3)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "arg"), "a text id family 3 refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 6, 10)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "arg"), "a part number 10 refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 1, 1)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "arg"), "an arg on a kind that has none refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 207, 1, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "past the block"), "a 16-bit word at the last byte of DAT refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 206, 1, 0)})));
    Check(sc::ReadRows(rp, r, err), "a 16-bit word at the last two bytes of DAT accepted: " + err);
    Spit(rp, raw(relo_blk({entry("BINP", 3, 17, 4, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "past the block"), "a 32-bit word at BINP +17 of 20 refused: " + err);
    Spit(rp, raw(relo_blk({entry("BINP", 2, 4, 1, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "no such block"), "an entry for a BINP leaf the rows do not hold refused: " + err);
    Spit(rp, raw(relo_blk({entry("LSD ", 2, 0, 1, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "no such block"), "an entry for an LSD list the rows do not hold refused: " + err);
    Spit(rp, raw(relo_blk({entry("NAME", 0, 0, 1, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "no such block"), "an entry for a tag that is no word block refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 1, 4, 1, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "no such block"), "a sub on DAT refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 1, 0), entry("DAT ", 0, 5, 1, 0)})));
    Check(!sc::ReadRows(rp, r, err) && Has(err, "overlaps"), "two entries that share a byte refused: " + err);
    Spit(rp, raw(relo_blk({entry("DAT ", 0, 4, 1, 0), entry("PRO ", 0, 4, 1, 0)})));
    Check(sc::ReadRows(rp, r, err), "the same offset in two blocks is two words: " + err);
    {  // a count that disagrees with the length
      std::string p;
      P32(p, 2);
      p += entry("DAT ", 0, 4, 1, 0);
      std::string s("RELO", 4);
      P32(s, uint32_t(p.size()));
      Spit(rp, raw(s + p));
      Check(!sc::ReadRows(rp, r, err) && Has(err, "RELO"), "a count of 2 with one entry's bytes refused: " + err);
      p.clear(), P32(p, uint32_t(sc::kMaxRelocs) + 1);
      s = "RELO", P32(s, uint32_t(p.size()));
      Spit(rp, raw(s + p));
      Check(!sc::ReadRows(rp, r, err) && Has(err, "RELO"), "a count past the cap refused: " + err);
    }
    // the write side refuses what the read side would
    sc::SeatRows bad = w;
    bad.relo[0].kind = sc::RelKind(0);
    Check(!sc::WriteRows(rp, bad, err) && Has(err, "kind"), "WriteRows refuses an unknown kind: " + err);
    bad = w, bad.relo.push_back(w.relo[0]);
    Check(!sc::WriteRows(rp, bad, err) && Has(err, "overlaps"), "WriteRows refuses an entry twice: " + err);
    bad = w, bad.binp.clear();
    Check(!sc::WriteRows(rp, bad, err) && Has(err, "no such block"), "WriteRows refuses an entry for a block the rows do not hold: " + err);
    bad = w, bad.has_relo = false;
    Check(!sc::WriteRows(rp, bad, err) && Has(err, "without a RELO block"), "WriteRows refuses entries with no RELO block: " + err);
    bad = w, bad.relo.clear();
    Check(sc::WriteRows(rp, bad, err) && sc::ReadRows(rp, r, err) && r.has_relo && r.relo.empty(), "an empty RELO list is a list (has_relo, no entries): " + err);
  }
  // ---- a failed write leaves the old file
  // ---- a failed write leaves the old file
  {
    sc::SeatRows w, r;
    w.seat = 401;
    w.dat[0] = 9;
    Check(sc::WriteRows(rp, w, err), "old rows written");
    const std::string before = Slurp(rp);
    fs::create_directories(rp + ".tmp");  // the temp path is blocked: the write must fail
    w.dat[0] = 8;
    Check(!sc::WriteRows(rp, w, err), "rows write into a blocked temp path fails");
    Check(Slurp(rp) == before && sc::ReadRows(rp, r, err) && r.dat[0] == 9, "the old rows.bin is untouched after a failed write");
    fs::remove_all(rp + ".tmp");
    sc::Card c = Full(), r2;
    Check(sc::WriteCard(cp, c, &err), "old card written");
    const std::string cb = Slurp(cp);
    fs::create_directories(cp + ".tmp");
    c.name = "Changed";
    Check(!sc::WriteCard(cp, c, &err), "card write into a blocked temp path fails");
    Check(Slurp(cp) == cb && sc::ReadCard(cp, r2, err) && r2.name == Full().name, "the old card.txt is untouched after a failed write");
    fs::remove_all(cp + ".tmp");
    Check(sc::WriteCard(cp, c, &err) && !fs::exists(cp + ".tmp"), "a good write leaves no temp file");
  }

  // ---- a folder whose name is outside the ANSI code page (the four entries take std::filesystem::path; std::string callers still compile)
  {
    const fs::path wd = dir / fs::path(L"名前テスト");
    fs::create_directories(wd);
    sc::Card c = Full(), r;
    Check(sc::WriteCard(wd / "card.txt", c, &err) && fs::exists(wd / "card.txt"), "wide path: card writes: " + err);
    Check(sc::ReadCard(wd / "card.txt", r, err) && SameCard(c, r), "wide path: card reads back equal: " + err);
    sc::SeatRows w, rr;
    w.seat = 412;
    w.dat[3] = 77;
    Check(sc::WriteRows(wd / "rows.bin", w, err) && !fs::exists(wd / "rows.bin.tmp"), "wide path: rows write (no temp left): " + err);
    Check(sc::ReadRows(wd / "rows.bin", rr, err) && rr.seat == 412 && rr.dat[3] == 77, "wide path: rows read back equal: " + err);
    Check(!sc::ReadRows(wd / "nope.bin", rr, err) && Has(err, "nope.bin"), "wide path: a missing file is refused and named: " + err);
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
  if (g_fail) {
    std::printf("seat_card_unit: %d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("seat_card_unit: PASS (%d checks)\n", g_checks);
  return 0;
}
