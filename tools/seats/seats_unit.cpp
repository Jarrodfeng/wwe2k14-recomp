// S110 SEAT-B: host unit for src\seats_core.h (the pure half of "the game seats imported characters from their library cards"). No game files: every
// payload byte is a made-up pattern; the unit writes only under the folder it is given (argv[1], default out\seats_unit).
//   seats_unit.exe [work dir]     every check must PASS (exit 0).   Built and run (with its planted twins) by tools\seats\seats_selftest.py.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../../src/importer/pack_fit.h"  // the old pack's superstars.txt parser (ParseBuilt), the one the game uses for an unmigrated pack
#include "../../src/importer/pack_io.h"  // the reference: importer::io::WriteArc / WritePac (the node rows must equal the arc group the pack builder writes)
#include "seats_core.h"

namespace fs = std::filesystem;
namespace sc = wwe2k14::seat_card;
namespace st = wwe2k14::seats;
namespace sb = wwe2k14::seat_bind;
namespace io = importer::io;

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("FAIL  %s\n", what.c_str());
  }
}
static bool Has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }
static std::string Joined(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += x + "\n";
  return s;
}
static void SpitBytes(const fs::path& p, const std::vector<uint8_t>& b) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static void SpitText(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(s.data(), std::streamsize(s.size()));
}

// ---- a made-up pac with every kind of directory a library pac holds ----
static io::Entry E(const std::string& name, size_t n, int seed) {
  io::Entry e;
  e.name = name;
  for (size_t i = 0; i < n; ++i) e.blob.push_back(uint8_t((i * 13 + size_t(seed)) & 255));
  return e;
}
static io::Pac MadePac(int seat) {
  io::Pac p;
  const std::string s3 = std::to_string(seat);
  io::Dir emd;
  emd.tag = "EMD ";
  emd.kind = 1;
  emd.ents.push_back(E("000" + s3 + "01", 700, 1));
  emd.ents.push_back(E("000" + s3 + "02", 3000, 2));
  io::Entry al = E("000" + s3 + "03", 0, 0);
  al.alias = 0;  // an attire alias: two rows, one sector run
  emd.ents.push_back(al);
  p.dirs.push_back(emd);
  io::Dir evp;  // two entrance tables in the importer's identity-BPE form: the entrance bank (header 9997) and the victory scene (6000 + seat), one actor each
  evp.tag = "EVP ";
  evp.kind = 1;
  for (const int bk : {sc::kRefBank, 6000 + sc::kRefSeat}) {
    io::Bytes ev(0x44 + 32 + 40, 0x11);
    std::memcpy(ev.data(), "0FOP", 4);
    io::WB16(&ev[0x20], uint16_t(bk));
    io::WB32(&ev[0x40], 1);
    io::WB32(&ev[0x44 + 12], uint32_t(seat) * 100u + (bk == sc::kRefBank ? 3u : 1u));
    ev[0x44 + 16] = 1;
    io::Entry e;
    e.name = io::Pad(std::to_string(bk), 8);
    e.blob = io::BpeIdentity(ev);
    evp.ents.push_back(e);
  }
  p.dirs.push_back(evp);
  for (const char* t : {"SSFA", "SSFB", "SSFC", "SDFA", "SDFB", "SDFC"}) {
    io::Dir d;
    d.tag = t;
    d.kind = 0;
    d.ents.push_back(E("0" + s3, 400 + size_t(t[3]), int(t[3])));
    p.dirs.push_back(d);
  }
  return p;
}
static std::vector<uint8_t> PacFile(io::Pac& p) { return io::WritePac(p); }  // lays the sectors out in p itself

static std::vector<uint8_t> Head(const std::vector<uint8_t>& f) { return std::vector<uint8_t>(f.begin(), f.begin() + std::min<size_t>(f.size(), st::kPacHeader)); }

// the reference arc rows of `p` as the pack builder writes them: WriteArc's group (marker, rows) + the terminator, minus the marker
static std::vector<uint8_t> RefRows(const io::Pac& p) {
  const io::Bytes arc = io::WriteArc({&p});  // FF000000, rows, FFFFFFFF
  return std::vector<uint8_t>(arc.begin() + 4, arc.end());
}

// ---- a seat's rows.bin: every payload a made-up pattern; the file-form words that matter set by hand ----
static sc::SeatRows Rows(int seat, int src_bank, bool t102 = true) {
  sc::SeatRows r;
  r.seat = seat;
  for (size_t i = 0; i < r.dat.size(); ++i) r.dat[i] = uint8_t(i + 1);
  r.dat[4 + 16] = 1;  // the row's active byte (+16, after the 4-byte file header)
  for (size_t i = 0; i < r.pro.size(); ++i) r.pro[i] = uint8_t(i * 3 + 7);
  for (size_t i = 0; i < r.dsbd.size(); ++i) r.dsbd[i] = uint8_t(100 + i);
  for (size_t i = 0; i < r.bin5.size(); ++i) r.bin5[i] = uint8_t(0xA0 + i);
  r.bin5[0] = uint8_t(seat & 255), r.bin5[1] = uint8_t(seat >> 8);  // the template row's id, little-endian (file form)
  sc::SeatRows::Binp b;
  b.k = 2;
  for (size_t i = 0; i < b.row.size(); ++i) b.row[i] = uint8_t(0x50 + i);
  r.binp.push_back(b);
  sc::SeatRows::Lsd l2, l17;
  l2.list = 2;
  l17.list = 17;
  const uint8_t a[8] = {uint8_t(seat & 255), uint8_t(seat >> 8), 0x4C, 0x04, 0x01, 0x02, 0x03, 0x04};  // value = seat, unlock 1100 + seat (LE), text u32
  std::memcpy(l2.row.data(), a, 8);
  std::memcpy(l17.row.data(), a, 8);
  l17.row[4] = 0x60;
  r.lsd.push_back(l2);
  r.lsd.push_back(l17);
  r.has_t102 = t102;
  r.t102_src_bank = uint32_t(src_bank);
  r.names[0] = "NAME " + std::to_string(seat);
  r.names[1] = "SHORT " + std::to_string(seat);
  r.names[2] = "LONG NAME " + std::to_string(seat);
  return r;
}
// (S110 LIBSIZE) a card in the one form: the reference inst (seat 497 / bank 9997) and the `file_form 2` line, whatever seat he sits in
static sc::Card CardOf(const std::string& key, int seat, uint32_t sig, int src_bank = 50, int music = 60) {
  sc::Card c;
  c.key = key;
  c.name = "Name " + std::to_string(seat);
  c.short_name = "N" + std::to_string(seat);
  c.seat = seat;
  c.inst = {true, sc::kRefSeat, sc::kRefBank, src_bank, music, sig, 0};
  sc::SetFileForm(c, sc::kFileForm);
  return c;
}
// the canonical rows.bin (made for seat 497) with a relocation list: the seat word at DAT +100 and the rank word at DAT +102
static sc::SeatRows Canon(int src_bank) {
  sc::SeatRows r = Rows(sc::kRefSeat, src_bank);
  r.dat[100] = uint8_t(sc::kRefSeat & 255), r.dat[101] = uint8_t(sc::kRefSeat >> 8), r.dat[102] = 77, r.dat[103] = 0;
  r.has_relo = true;
  r.relo.push_back({"DAT ", 0, 100, sc::RelKind::Seat16, 0});
  r.relo.push_back({"DAT ", 0, 102, sc::RelKind::Rank16, 0});
  return r;
}
static bool HasBytes(const std::vector<uint8_t>& v, const char* s) { return std::search(v.begin(), v.end(), s, s + std::strlen(s)) != v.end(); }

static const char* kFrameText =
    "# a frame\npack 6\nbuilt_on 4 5\ntop 5 8A1C33F0\nframe 0A0B0C0D\n"
    "seat 397 bank 11 rank 0\nseat 398 bank 12 rank 1 was 3 4\nseat 400 bank 14 rank 2\nseat 401 bank 15 rank 3\nseat 402 bank 16 rank 4\nseat 403 bank 17 rank 5\n";

int main(int argc, char** argv) {
  const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::path("out") / "seats_unit";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);

  // ---------------------------------------------------------------- the frame's lines
  {
    const st::Frame f = st::ParseFrame(kFrameText);
    Check(f.pack_id == 6 && f.has_sig && f.sig == 0x0A0B0C0Du, "frame: pack 6 and the signature line");
    Check(f.seats.size() == 6 && f.Find(398) && f.Find(398)->bank == 12 && f.Find(398)->rank == 1, "frame: six seat lines, 398 bank 12 rank 1");
    Check(f.Find(398)->was.size() == 2 && f.Find(398)->was[0] == 3 && f.Find(398)->was[1] == 4, "frame: was 3 4 of seat 398");
    Check(f.notes.empty(), "frame: a clean frame has no notes");
    const st::Frame g = st::ParseFrame("seat 99 bank 5 rank 0\nseat 500 bank 5 rank 0\nseat 401 bank 0 rank 0\nseat 402 rank 0\nseat 403 bank 5 rank -1\n"
                                       "seat 404 bank 5 rank 0 junk 3\nseat 405 bank 5 rank 0\nseat 405 bank 6 rank 1\nseat x bank 5 rank 0\nframe 12\n");
    Check(g.seats.size() == 1 && g.seats[0].seat == 405 && g.seats[0].bank == 5, "frame: only the one usable seat line stands (seat 99 / 500, bank 0, no bank, rank < 0, junk word, a bad seat)");
    Check(g.notes.size() == 9, "frame: every refused line says so (eight seat lines and the short signature) (got " + std::to_string(g.notes.size()) + " notes)");
    Check(!g.has_sig, "frame: a short signature is not taken");
    // (S110 LIBSIZE) a bank names one seat: a second seat line with a bank already taken is left out and noted
    const st::Frame b = st::ParseFrame("seat 400 bank 7 rank 0\nseat 401 bank 7 rank 1\nseat 402 bank 8 rank 2\n");
    Check(b.seats.size() == 2 && b.Find(400) && !b.Find(401) && b.Find(402) && b.notes.size() == 1 && b.notes[0].find("bank 7 twice") != std::string::npos,
          "frame: a bank twice: the second seat is left out, the first stands, one note");
  }

  // ---------------------------------------------------------------- node rows == io::WriteArc's group + the terminator
  {
    io::Pac p = MadePac(397);
    const std::vector<uint8_t> file = PacFile(p);
    Check(!file.empty(), "node: the made-up pac was written");
    io::Pac hd = p;
    std::erase_if(hd.dirs, [](const io::Dir& d) { return d.tag[1] == 'D'; });
    io::Pac sd = p;
    std::erase_if(sd.dirs, [](const io::Dir& d) { return d.tag[0] == 'S' && d.tag[1] == 'S'; });
    for (io::Dir& d : sd.dirs)
      if (d.tag[0] == 'S' && d.tag[1] == 'D') d.tag[1] = 'S';
    std::vector<uint8_t> rows;
    std::string why;
    Check(st::NodeRows(Head(file), file.size(), false, rows, why) && rows == RefRows(hd), "node rows (HD) == WriteArc's group for the pac (no SDF dirs) + the terminator " + why);
    Check(rows.size() >= 4 && rows[rows.size() - 4] == 0xFF && rows.back() == 0xFF, "node rows (HD): end with the 0xFFFFFFFF terminator");
    Check(st::NodeRows(Head(file), file.size(), true, rows, why) && rows == RefRows(sd), "node rows (SD) == WriteArc's group with SDFx renamed SSFx (the HD SSFx gone) " + why);
    {  // the whole group: the arc of ONE pac is marker + the same rows
      const io::Bytes arc = io::WriteArc({&hd});
      Check(arc.size() == RefRows(hd).size() + 4 && arc[0] == 0xFF, "node: WriteArc's group is a marker word and the same rows");
    }
    // refusals: a pac that is not one, a header cut short, an entry outside the file, a directory of an unknown kind
    std::vector<uint8_t> junk = file;
    junk[0] = 'X';
    Check(!st::NodeRows(Head(junk), junk.size(), false, rows, why) && Has(why, "not a plain"), "node: a file that is not an EPAC is refused");
    Check(!st::NodeRows(std::vector<uint8_t>(file.begin(), file.begin() + 100), file.size(), false, rows, why), "node: a header cut short is refused");
    Check(!st::NodeRows(Head(file), 0x4800, false, rows, why) && Has(why, "outside the file"), "node: an entry that lies outside the file is refused");
    std::vector<uint8_t> k2 = file;
    k2[0x800 + 6] = 2;  // the first directory's kind word
    Check(!st::NodeRows(Head(k2), k2.size(), false, rows, why) && Has(why, "row kind"), "node: a directory of an unknown row kind is refused");
    std::vector<uint8_t> big = file;
    big[4] = 0xFF, big[5] = 0xFF;
    Check(!st::NodeRows(Head(big), big.size(), false, rows, why) && Has(why, "too big"), "node: a directory size past the header is refused");
  }

  // ---------------------------------------------------------------- the BIN builders (the game holds the old rows big-endian)
  {
    std::vector<uint8_t> old;  // leaf 5: rows for ids 394, 397 (a placeholder), 398 in memory form
    for (int id : {394, 397, 398}) {
      uint8_t row[16];
      for (int i = 0; i < 16; ++i) row[i] = uint8_t(0x10 + i);
      st::PutBe16(row, uint16_t(id));
      old.insert(old.end(), row, row + 16);
    }
    const sc::SeatRows a = Rows(397, 50), b = Rows(410, 50);
    std::vector<std::pair<int, const uint8_t*>> add = {{397, a.bin5.data()}, {410, b.bin5.data()}};
    const st::Appended r = st::BuildBin5(old.data(), 3, add);
    Check(r.count == 4 && r.bytes.size() == 64, "bin5: three old rows, one replaced and one appended = four");
    Check(r.swap_at.size() == 2 && r.swap_at[0] == 16 && r.swap_at[1] == 48, "bin5: the placeholder row of seat 397 is REPLACED in place (offset 16), seat 410 is appended (offset 48)");
    Check(std::memcmp(&r.bytes[0], old.data(), 16) == 0 && std::memcmp(&r.bytes[32], &old[32], 16) == 0, "bin5: the other old rows are untouched");
    Check(std::memcmp(&r.bytes[16], a.bin5.data(), 16) == 0 && std::memcmp(&r.bytes[48], b.bin5.data(), 16) == 0, "bin5: the new rows are the card's file-form bytes (the game swaps them)");
    // the game's swap of a 16-byte row = eight halfwords: apply it as the guest call would, then the id reads big-endian
    std::vector<uint8_t> done = r.bytes;
    for (size_t at : r.swap_at)
      for (int i = 0; i < 16; i += 2) std::swap(done[at + size_t(i)], done[at + size_t(i + 1)]);
    Check(st::Be16(&done[16]) == 397 && st::Be16(&done[48]) == 410, "bin5: after the game's swap the replaced and the appended row carry their seat big-endian");
    const st::Appended none = st::BuildBin5(old.data(), 3, {});
    Check(none.count == 3 && none.bytes == old && none.swap_at.empty(), "bin5: no resident, no change");

    std::vector<uint8_t> parts(2 * 20, 0x33);
    const std::vector<const uint8_t*> padd = {a.binp[0].row.data(), b.binp[0].row.data()};
    const st::Appended p = st::BuildBinPart(parts.data(), 2, padd);
    Check(p.count == 4 && p.bytes.size() == 80 && p.swap_at.size() == 2 && p.swap_at[0] == 40 && p.swap_at[1] == 60, "binp: two old rows + two appended (offsets 40, 60)");
    Check(std::memcmp(&p.bytes[0], parts.data(), 40) == 0 && std::memcmp(&p.bytes[42], a.binp[0].row.data() + 2, 18) == 0, "binp: the old rows first, then ours in order");
    // +0 = the row's own index in the leaf (PatchEnexBin: io::W16(&copy[0], nk)); the card holds the top's count (the index "if appended first"), so each
    // appended row is numbered where it really lands: file form (little-endian), the game's swap sub_822700A0 then makes it big-endian
    Check(p.bytes[40] == 2 && p.bytes[41] == 0 && p.bytes[60] == 3 && p.bytes[61] == 0, "binp: each appended row's +0 is its real index (2, 3), little-endian before the swap");
    std::vector<uint8_t> big(size_t(300) * 20, 0x11);
    const st::Appended q = st::BuildBinPart(big.data(), 300, {a.binp[0].row.data()});
    Check(q.bytes[6000] == 0x2C && q.bytes[6001] == 0x01, "binp: index 300 is written low byte first (0x2C 0x01)");
  }

  // ---------------------------------------------------------------- the LSD builders (file little-endian, game memory big-endian)
  {
    const sc::SeatRows a = Rows(397, 50);
    const auto m = st::LsdRowToMemory(a.lsd[0].row.data());
    Check(st::Be16(&m[0]) == 397 && st::Be16(&m[2]) == 0x044C && st::Be32(&m[4]) == 0x04030201u, "lsd: a row's value / unlock / text words are swapped to the game's big-endian");
    std::vector<uint8_t> old(3 * 8, 0x77);
    const std::vector<std::array<uint8_t, 8>> add = {m, st::LsdRowToMemory(Rows(410, 50).lsd[0].row.data())};
    const std::vector<uint8_t> out = st::BuildLsd(old.data(), 3, add);
    Check(out.size() == 40 && std::memcmp(out.data(), old.data(), 24) == 0 && std::memcmp(&out[24], m.data(), 8) == 0 && st::Be16(&out[32]) == 410, "lsd: the game's records, then ours in order");
    st::Roster r;
    r.loaded = true;
    for (int s : {410, 397}) {
      st::Resident x;
      x.seat = s;
      x.rows = Rows(s, 50);
      r.res.push_back(x);
    }
    std::sort(r.res.begin(), r.res.end(), [](const st::Resident& p, const st::Resident& q) { return p.seat < q.seat; });
    const auto lists = st::LsdLists(r);
    Check(lists.size() == 2 && lists[0] == 2 && lists[1] == 17, "lsd: the lists the residents add to, ascending, each once");
    const auto rows2 = st::LsdRows(r, 2);
    Check(rows2.size() == 2 && st::Be16(&rows2[0][0]) == 397 && st::Be16(&rows2[1][0]) == 410, "lsd: one row per resident, in seat order");
    Check(st::LsdRows(r, 3).empty(), "lsd: a list no resident has a row in gets none");
    const auto b5 = st::Bin5Rows(r);
    Check(b5.size() == 2 && b5[0].first == 397 && b5[1].first == 410, "bin: the leaf-5 rows in seat order");
    Check(st::BinpRows(r, 2).size() == 2 && st::BinpRows(r, 1).empty(), "bin: the part rows of one leaf");
  }

  // ---------------------------------------------------------------- availability, the 102 key map, the name gate, DSBD, the bounds
  {
    st::Roster r;
    r.loaded = true;
    r.frame = st::ParseFrame(kFrameText);
    st::Resident a;
    a.seat = 397, a.bank = 11, a.rows = Rows(397, 55);
    st::Resident b;
    b.seat = 398, b.bank = 12, b.rows = Rows(398, 56, false);  // no T102 block
    r.res = {a, b};
    // availability: a frame seat with no occupant is hidden; an occupied seat and a seat the frame does not list are the game's
    Check(!st::HideSeat(r, 397) && !st::HideSeat(r, 398), "avail: an occupied seat is left to the game");
    Check(st::HideSeat(r, 400) && st::HideSeat(r, 403), "avail: an EMPTY frame seat answers 0 (the saved row of a removed character stays hidden)");
    Check(!st::HideSeat(r, 399) && !st::HideSeat(r, 100) && !st::HideSeat(r, 385) && !st::HideSeat(r, 500) && !st::HideSeat(r, 99), "avail: a seat the frame does not list is never touched");
    st::Roster never;
    Check(!st::HideSeat(never, 400), "avail: a roster that was never loaded hides nothing");
    Check(st::StateOf(r, 400) == st::SeatState::kEmpty && !st::KeepUniverseState(r, 400), "avail: an empty frame seat (no card names it) is purged by the game's rule");
    Check(!st::KeepUniverseState(never, 400), "avail: a roster that was never loaded keeps nothing");
    // the 102 key map
    Check(st::SrcBank(r, 11) == 55, "t102: a resident's bank is answered as his entrance wrestler's");
    Check(st::SrcBank(r, 0x10000 | 11) == 55, "t102: the key is the low 16 bits (the callers pass `id & 0xFFFF`)");
    Check(st::SrcBank(r, 12) == -1, "t102: a resident with no T102 block keeps his key");
    Check(st::SrcBank(r, 13) == -1 && st::SrcBank(r, 100) == -1, "t102: a bank no resident holds is the game's own");
    // the name gate
    Check(st::NameFor(r, 60397) && *st::NameFor(r, 60397) == "NAME 397", "names: text 60000 + seat = his name");
    Check(st::NameFor(r, 62398) && *st::NameFor(r, 62398) == "SHORT 398", "names: text 62000 + seat = his short name");
    Check(st::NameFor(r, 64397) && *st::NameFor(r, 64397) == "LONG NAME 397", "names: text 64000 + seat = his long name");
    Check(!st::NameFor(r, 60400) && !st::NameFor(r, 60399) && !st::NameFor(r, 59999) && !st::NameFor(r, 60100) && !st::NameFor(r, 60500) && !st::NameFor(r, -60397), "names: a seat nobody holds, a stock id and an id out of range are not ours");
    Check(!st::NameFor(r, 61397) && !st::NameFor(r, 63397), "names: the gaps between the three bases are not ours");
    // DSBD
    std::array<uint8_t, 80> f;
    for (size_t i = 0; i < 80; ++i) f[i] = uint8_t(i);
    const auto m = st::DsbdMemoryRow(f);
    bool ok = true;
    for (size_t i = 0; i < 8; i += 2) ok = ok && m[i] == f[i + 1] && m[i + 1] == f[i];
    for (size_t i = 8; i < 80; ++i) ok = ok && m[i] == f[i];
    Check(ok, "dsbd: bytes +0..+7 are the four attire halfwords swapped, the other 72 bytes raw");
    uint32_t off = 0;
    Check(st::DsbdOffset(397, off) && off == 80u * 397, "dsbd: a seat's row is at 80 x seat");
    Check(st::DsbdOffset(499, off) && off == 39920 && st::DsbdOffset(0, off), "dsbd: the last seat fits the 40000-byte buffer");
    Check(!st::DsbdOffset(500, off) && !st::DsbdOffset(-1, off) && !st::DsbdOffset(100000, off), "dsbd: a seat past the buffer is refused");
    // the live tables
    uint32_t a1 = 0;
    Check(st::RowAddr(0x83000000u, 100, a1) && a1 == 0x83000000u && st::RowAddr(0x83000000u, 499, a1) && a1 == 0x83000000u + 204u * 399u, "rows: seat 100 is the first row, 499 the last");
    Check(!st::RowAddr(0x83000000u, 99, a1) && !st::RowAddr(0x83000000u, 500, a1) && !st::RowAddr(0, 397, a1), "rows: seat 99, seat 500 and an empty array pointer are refused");
    Check(st::ProfileAddr(397, a1) && a1 == 0x8389187Cu + 1272u * 297u && st::ProfileAddr(100, a1) && a1 == 0x8389187Cu, "profile: 1272 x (seat - 100) from 0x8389187C");
    Check(!st::ProfileAddr(99, a1) && !st::ProfileAddr(500, a1), "profile: seat 99 and 500 are refused");
    Check(st::ShouldSeat(0) && !st::ShouldSeat(1) && !st::ShouldSeat(0x80), "seat: only a live row whose active byte is 0 is written; a saved active row is kept");
  }

  // ---------------------------------------------------------------- the library on disk: a good card, and every card the loader must leave out
  // (S110 LIBSIZE) every character is ONE file made for the reference identity (seat 497 / bank 9997); a seated card is re-targeted to his seat at load
  {
    const fs::path root = work / "superstars";
    const fs::path lib = root / "library";
    io::Pac pac = MadePac(sc::kRefSeat);
    const std::vector<uint8_t> pac_file = PacFile(pac);
    const uint32_t kSig = 0x0A0B0C0Du;
    auto put = [&](const std::string& key, const sc::Card& c, const sc::SeatRows& r, bool with_pac, const std::vector<uint8_t>& file) {
      const fs::path d = lib / key;
      fs::create_directories(d);
      std::string err;
      if (!sc::WriteCard((d / "card.txt").string(), c, &err)) std::printf("setup: card %s: %s\n", key.c_str(), err.c_str());
      if (!sc::WriteRows((d / "rows.bin").string(), r, err)) std::printf("setup: rows %s: %s\n", key.c_str(), err.c_str());
      if (with_pac) SpitBytes(d / st::CharRel(key), file);
    };
    // 397: good
    put("0000AAA1", CardOf("0000AAA1", 397, kSig, 55, 60), Canon(55), true, pac_file);
    // 398: made in the old per-seat form (inst for seat 398 / bank 12, no file_form line)
    {
      sc::Card c = CardOf("0000AAA2", 398, kSig);
      c.inst.seat = 398, c.inst.bank = 12, c.extra.clear();
      put("0000AAA2", c, Rows(398, 50), true, pac_file);
    }
    // 400: the pac is missing
    put("0000AAA3", CardOf("0000AAA3", 400, kSig), Canon(50), false, {});
    // 401: made for another frame
    put("0000AAA4", CardOf("0000AAA4", 401, 0x11111111u), Canon(50), true, pac_file);
    // 402: his rows.bin was made for another seat
    put("0000AAA5", CardOf("0000AAA5", 402, kSig), Rows(403, 50), true, pac_file);
    // a second card claims seat 397 (the first by key keeps it)
    put("0000AAA6", CardOf("0000AAA6", 397, kSig), Canon(50), true, pac_file);
    // 403: the pac's entry runs past the file
    {
      std::vector<uint8_t> cut(pac_file.begin(), pac_file.begin() + 0x4800);
      put("0000AAA7", CardOf("0000AAA7", 403, kSig), Canon(50), true, cut);
    }
    // not instantiated yet (no inst line)
    {
      sc::Card c = CardOf("0000AAA8", 397, kSig);
      c.inst = sc::Inst();
      put("0000AAA8", c, Canon(50), true, pac_file);
    }
    // a card with `seat none` and no inst line: unseated, not an error
    {
      sc::Card c = CardOf("0000AAA9", -1, kSig);
      c.seat = -1;
      c.inst = sc::Inst();
      put("0000AAA9", c, Canon(50), false, {});
    }
    // the card's key is not its folder name
    put("0000AAAA", CardOf("0000AAAB", 397, kSig), Canon(50), true, pac_file);
    // a card whose seat the frame does not list
    put("0000AAAC", CardOf("0000AAAC", 450, kSig), Canon(50), true, pac_file);
    // the reference inst but no `file_form 2` line (a card of an importer that wrote the identity but not the form)
    {
      sc::Card c = CardOf("0000AAAD", 400, kSig);
      c.extra.clear();
      put("0000AAAD", c, Canon(50), true, pac_file);
    }
    fs::create_directories(lib / "not-a-key");  // a stray folder
    SpitText(root / "frame" / "superstars.txt", kFrameText);

    st::Roster r;
    st::LoadRoster(root, 6, kFrameText, r);
    const std::string log = Joined(st::RosterLog(r));
    Check(r.loaded && r.res.size() == 1 && r.res[0].seat == 397 && r.res[0].key == "0000AAA1" && r.res[0].bank == 11, "library: exactly the one good card is seated (seat 397, bank 11): " + log);
    if (!r.res.empty()) {
      const st::Resident& x = r.res[0];
      Check(x.node_path == "SSLIB:\\0000AAA1\\PAC\\CH\\CH497.PAC" && x.node_path.size() <= st::kMaxNodePath && st::NodePath("0000AAA1") == x.node_path, "library: the node path is SSLIB:\\<KEY>\\PAC\\CH\\CH497.PAC for every seat");
      Check(!x.hd.empty() && !x.sd.empty() && x.hd != x.sd, "library: both the HD and the SD node rows are built");
      Check(x.rows.names[0] == "NAME 497" && x.card.inst.music == 60 && x.rows.t102_src_bank == 55, "library: the card and the rows came through");
      Check(x.was.empty(), "library: his `was` list is the frame's (none for 397)");
      // a resident's rows are in SEAT form (the one loader re-targets a seated card to his seat)
      Check(x.rows.seat == 397 && x.rows.dat[100] == uint8_t(397 & 255) && x.rows.dat[101] == uint8_t(397 >> 8) && x.rows.dat[102] == 0,
            "library: a resident's rows are in seat form: rows seat 397, the seat word moved, the rank word the frame's (0)");
      Check(HasBytes(x.hd, "000397") && !HasBytes(x.hd, "000497") && !HasBytes(x.hd, "EVP ") && HasBytes(x.hd, "SSFA") && !HasBytes(x.hd, "SDFA") && HasBytes(x.sd, "SSFA") && HasBytes(x.hd, "0397") && !HasBytes(x.hd, "0497"),
            "library: a resident's node rows are in seat form (EMD 000397nn, renders 0397, the EVP rows dropped: they are in his EV file)");
      Check(st::g_roster.res.empty(), "library: the loader fills the roster it is given, not the published one");
      // the EV file of the seat, made from the ONE file (the importer's identity-BPE blobs re-targeted: bank 9997 -> 11, victory 6497 -> 6397, actors 497nn -> 397nn)
      sb::EvFile ev;
      int actors = 0, blobs = 0;
      std::string e2;
      const bool ok = st::SeatEvFile(lib, x.card, 397, 11, ev, e2, &actors, &blobs);
      Check(ok && blobs == 2 && actors == 2 && ev.rows.size() == 2 && ev.rows[0].name.rfind("11", 0) == 0 && ev.rows[1].name.rfind("6397", 0) == 0, "EV file: two blobs, named 11 and 6397, two actors moved: " + e2);
      if (ok && ev.rows.size() == 2) {
        const size_t at = size_t(sb::kEvHeader) + size_t(ev.rows[0].sector) * sb::kEvSector + 16 + 5;  // the first chunk's raw bytes
        Check(at + 0x80 < ev.bytes.size() && sb::Be16(&ev.bytes[at + 0x20]) == 11 && sb::Be32(&ev.bytes[at + 0x44 + 12]) == 39703, "EV file: the header bank is 11 and the actor key 39703 (from 9997 / 49703)");
      }
      Check(!st::SeatEvFile(work / "no_library", x.card, 397, 11, ev, e2) && !e2.empty(), "EV file: a missing character file is refused with a reason: " + e2);
    }
    Check(r.cards == 12 && r.unseated == 1, "library: twelve cards read (the stray folder is no card), one unseated: " + std::to_string(r.cards) + "/" + std::to_string(r.unseated));
    Check(r.left_out.size() == 11, "library: ten cards and one stray folder left out, each with a reason (got " + std::to_string(r.left_out.size()) + "): " + log);
    Check(Has(log, "0000AAA2") && Has(log, "made in the old per-seat form") && Has(log, "rebuild it in the Superstar Importer"), "library: a card in the OLD per-seat form is refused loudly (no second loader)");
    Check(Has(log, "0000AAAD") && Has(log, "file_form 0"), "library: the reference inst without the `file_form 2` line is refused");
    Check(Has(log, "0000AAA3") && Has(log, "pac/ch/ch497.pac is missing"), "library: a missing pac is left out and says so");
    Check(Has(log, "0000AAA4") && Has(log, "stale inst: frame 11111111 is not the frame's 0A0B0C0D"), "library: a card made for another frame is left out and says so");
    Check(Has(log, "0000AAA5") && Has(log, "rows.bin was made for seat 403, not 497"), "library: rows made for another seat are left out and say so");
    Check(Has(log, "0000AAA6") && Has(log, "already held by 0000AAA1"), "library: a second card for a held seat is left out and says so");
    Check(Has(log, "0000AAA7") && Has(log, "outside the file"), "library: a pac whose entry runs past the file is left out and says so");
    Check(Has(log, "0000AAA8") && Has(log, "no inst line"), "library: a card never instantiated is left out and says so");
    Check(Has(log, "0000AAAA") && Has(log, "is not its folder name"), "library: a card whose key is not its folder is left out");
    Check(Has(log, "0000AAAC") && Has(log, "seat 450 is not a seat of the frame"), "library: a card for a seat the frame does not list is left out");
    Check(Has(log, "not-a-key: the folder name is not a key"), "library: a stray folder that is no key is noted, not read as a card");
    Check(!Has(log, "0000AAA9"), "library: a card with `seat none` is not an error");
    // the three seat states (D1): occupied, HELD (a card names the seat but was left out), empty (no card names it)
    Check(st::StateOf(r, 397) == st::SeatState::kOccupied, "states: 397 is occupied (a good card; the cards left out for it do not matter)");
    for (int s : {398, 400, 401, 402, 403})
      Check(st::StateOf(r, uint32_t(s)) == st::SeatState::kHeld, "states: seat " + std::to_string(s) + " is HELD (its card exists but was left out)");
    Check(r.held.count(398) && Has(r.held.at(398), "old per-seat form"), "states: a held seat keeps its card's reason");
    Check(st::StateOf(r, 499) == st::SeatState::kEmpty && st::StateOf(r, 404) == st::SeatState::kEmpty, "states: a seat no card names is empty");
    Check(!r.held.count(-1) && st::StateOf(r, 397) != st::SeatState::kHeld, "states: an unseated card (`seat none`) holds nothing");
    // availability: held and empty frame seats are both not offered
    Check(st::HideSeat(r, 398) && st::HideSeat(r, 401) && !st::HideSeat(r, 397), "states: a held frame seat is not offered, the occupied one is the game's");
    // the Universe purge decision (sub_82B75228's sub_82B62720(id, 1)): a HELD seat's saved state is kept; an empty or occupied one is the game's rule
    Check(st::KeepUniverseState(r, 398) && st::KeepUniverseState(r, 403), "purge: a held seat's Universe state is kept (the check answers 'valid')");
    Check(!st::KeepUniverseState(r, 397), "purge: an occupied seat is left to the game's own answer");
    Check(!st::KeepUniverseState(r, 499) && !st::KeepUniverseState(r, 0) && !st::KeepUniverseState(r, 512), "purge: an empty seat (and a non-seat id) is purged by the game's own rule");
    Check(Has(log, "held"), "states: the log says how many seats are held");

    // a frame with no signature line is NOT usable (the importer always writes it): no resident at all, and the log says why
    st::Roster q;
    std::string nosig = kFrameText;
    nosig.erase(nosig.find("frame 0A0B0C0D\n"), 15);
    st::LoadRoster(root, 6, nosig, q);
    Check(q.loaded && q.res.empty(), "library: a frame with no signature line seats nobody (got " + std::to_string(q.res.size()) + " resident(s))");
    Check(Has(Joined(st::RosterLog(q)), "no `frame XXXXXXXX` signature line"), "library: ... and the log says so: " + Joined(st::RosterLog(q)));
    Check(st::HideSeat(q, 397) && st::HideSeat(q, 401), "library: ... so every seat of that frame is hidden (none occupied)");
    Check(st::StateOf(q, 397) == st::SeatState::kHeld && st::KeepUniverseState(q, 397), "library: ... but the seats its cards name are HELD, not removed (their saved state is kept)");
    {  // FormRefusal on its own: a frame with no signature refuses every card
      st::Frame f = st::ParseFrame(nosig);
      // (a card whose frame word is 0 = the unsigned frame's empty signature: only the has_sig rule refuses it)
      Check(!st::FormRefusal(CardOf("0000AAA1", 397, 0u), Canon(55), f).empty(), "library: FormRefusal refuses a card against a frame with no signature");
    }
    {  // (S110 LIBSIZE-1 review) an ALIA row whose own name is not of the reference identity would be registered AS ITSELF: the card is left out at load, with
       // its reason (else one such card makes AliasNodes refuse the alias rows of every seat)
      const st::Frame f = st::ParseFrame(kFrameText);
      sc::SeatRows good = Canon(55), bad = Canon(55), evp = Canon(55);
      sc::Alias a;
      a.tag = "GMEV", a.kind = 0, a.game = "0100", a.ours = "9997";
      good.alias.push_back(a);
      a.ours = "0100";
      bad.alias.push_back(a);
      a.tag = "EVP ", a.kind = 1, a.game = "100     ", a.ours = "9997    ";
      evp.alias.push_back(a);
      Check(st::FormRefusal(CardOf("0000AAA1", 397, 0x0A0B0C0Du), good, f).empty(), "library: a card whose alias rows are in the reference form is taken");
      const std::string why = st::FormRefusal(CardOf("0000AAA1", 397, 0x0A0B0C0Du), bad, f), why2 = st::FormRefusal(CardOf("0000AAA1", 397, 0x0A0B0C0Du), evp, f);
      Check(Has(why, "reference identity") && Has(why2, "reference identity"), "library: a card with an alias row not in the reference form (GMEV 0100) or an EVP alias is refused: " + why + " / " + why2);
    }

    // a mods folder whose path holds characters outside the ANSI code page (seat_card.h / LoadLibrary take std::filesystem::path)
    {
      const fs::path wide = work / fs::path(L"名前テスト") / "superstars";
      const fs::path wlib = wide / "library" / "0000BBB1";
      fs::create_directories(wlib);
      std::string err;
      Check(sc::WriteCard(wlib / "card.txt", CardOf("0000BBB1", 397, kSig, 55, 60), &err), "wide path: the card writes: " + err);
      Check(sc::WriteRows(wlib / "rows.bin", Canon(55), err), "wide path: the rows write: " + err);
      SpitBytes(wlib / st::CharRel(), pac_file);
      st::Roster w;
      st::LoadRoster(wide, 6, kFrameText, w);
      Check(w.res.size() == 1 && w.res[0].key == "0000BBB1", "wide path: the library under a non-ANSI folder seats its card: " + Joined(st::RosterLog(w)));
    }

    // S110 SEAT3 + LIBSIZE: library characters (`seat none` cards with an inst line) stay in the REFERENCE form (seat 497, bank 9997, no `was`), whatever the
    // frame holds for seat 497 (it is a frame seat here); a stale or foreign one is left out with a reason and holds no seat; one with no inst line is skipped silently
    {
      const fs::path lroot = work / "libchars" / "superstars";
      const std::string frame2 = std::string(kFrameText) + "seat 497 bank 40 rank 9 was 8\n";
      auto card = [&](const std::string& key, int seat, uint32_t sig, bool with_inst, int rows_seat = -1, bool relo = true, bool form = true) {
        const fs::path dir = lroot / "library" / key;
        fs::create_directories(dir);
        sc::Card c = CardOf(key, seat, sig);
        c.seat = seat;
        c.inst.valid = with_inst;
        if (!form) c.extra.clear();
        std::string err;
        sc::WriteCard(dir / "card.txt", c, &err);
        sc::SeatRows rows = rows_seat < 0 ? Canon(50) : Rows(rows_seat, 50);
        rows.has_relo = relo;  // a character is only ever seated / bound through his relocation list: an empty list is a valid one
        if (!relo) rows.relo.clear();
        sc::WriteRows(dir / "rows.bin", rows, err);
        SpitBytes(dir / st::CharRel(key), pac_file);
      };
      card("0000CCC1", -1, kSig, true);                    // fits
      card("0000CCC2", -1, 0x11111111u, true);             // stale frame
      card("0000CCC3", -1, kSig, true, -1, true, false);   // the reference inst but no file_form line
      card("0000CCC5", -1, kSig, false);                   // no inst line
      card("0000CCC6", -1, kSig, true, 403);               // rows.bin made for another seat
      card("0000CCC7", -1, kSig, true, -1, false);         // fits, but his rows.bin has no relocation list (an importer before S110 SEAT3-R): never bindable
      {  // the old per-seat form: inst for seat 398 / bank 12
        sc::Card c = CardOf("0000CCC4", -1, kSig);
        c.seat = -1, c.inst.seat = 398, c.inst.bank = 12, c.extra.clear();
        const fs::path dir = lroot / "library" / "0000CCC4";
        fs::create_directories(dir);
        std::string err;
        sc::WriteCard(dir / "card.txt", c, &err);
        sc::WriteRows(dir / "rows.bin", Rows(398, 50), err);
        SpitBytes(dir / st::CharRel(), pac_file);
      }
      card("0000CCC8", 398, kSig, true);                   // a RESIDENT in the same library: seat 398 (bank 12, rank 1, was 3 4)
      st::Roster L;
      st::LoadRoster(lroot, 6, frame2, L);
      Check(L.lib.size() == 1 && L.lib[0].key == "0000CCC1" && L.lib[0].seat == sc::kRefSeat && L.lib[0].bank == sc::kRefBank && L.lib[0].was.empty() && L.res.size() == 1,
            "library chars: only the card made for this frame is a library character, in the reference form: seat 497, bank 9997, no `was` (the frame's seat 497 is another man's) (got " + std::to_string(L.lib.size()) + ")");
      Check(!L.lib.empty() && !L.lib[0].hd.empty() && !L.lib[0].sd.empty() && L.lib[0].node_path == st::NodePath("0000CCC1") && L.lib[0].rows.seat == sc::kRefSeat &&
                HasBytes(L.lib[0].hd, "000497") && HasBytes(L.lib[0].hd, "0497"),
            "library chars: his node rows (HD and SD) stay in the reference form, his node path is made");
      Check(!L.res.empty() && L.res[0].seat == 398 && L.res[0].bank == 12 && L.res[0].was == std::vector<int>({3, 4}) && L.res[0].rows.seat == 398 && L.res[0].rows.dat[100] == uint8_t(398 & 255) &&
                L.res[0].rows.dat[102] == 1&& HasBytes(L.res[0].hd, "000398"),
            "library chars: a resident of the same library is in HIS seat form: seat 398, the frame's bank 12, `was` 3 4, rank word 1");
      Check(L.unseated == 7 && L.left_out.size() == 5 && L.held.empty(), "library chars: seven unseated cards, five left out with a reason (the fifth has no inst line and is skipped), none holds a seat (got " + std::to_string(L.left_out.size()) + ")");
      Check(Has(Joined(L.left_out), "old per-seat form") && Has(Joined(L.left_out), "stale inst: frame") && Has(Joined(L.left_out), "file_form 0") && Has(Joined(L.left_out), "rows.bin was made for seat 403") &&
                Has(Joined(L.left_out), "0000CCC7 (library): rows.bin has no relocation list"),
            "library chars: the reasons are said: " + Joined(L.left_out));
      Check(st::HideSeat(L, 401) && !st::HideSeat(L, 398) && !st::HideSeat(L, 399), "library chars: a library character seats nobody: an empty frame seat is still hidden, the resident's is not");
    }

    // an UNMIGRATED old pack (pack6 + superstars.txt, no frame): every old star's seat is HELD (hidden, Universe state kept), none occupied.
    // The star lines are read by the one existing parser (src\importer\pack_fit.h ParseBuilt), as the game's superstars.h does.
    {
      std::istringstream old_txt("pack 6\nbuilt_on 4 5\ntop 5 8A1C33F0\nstar 397 music 100 bank 1\nstar 398 music 120 bank 2 was 5\n");
      const wwe2k14::superstars_fit::Built b = wwe2k14::superstars_fit::ParseBuilt(old_txt);
      std::vector<int> stars;
      for (const auto& kv : b.music) stars.push_back(kv.first);
      st::Roster o;
      st::HoldOldPack(stars, o);
      Check(o.loaded && o.res.empty() && o.frame.seats.empty() && o.held.size() == 2 && o.held.count(397) && o.held.count(398),
            "old pack: exactly the seats its star lines name are held (397, 398), none occupied, no frame");
      Check(o.held.count(397) && Has(o.held.at(397), "start the game from the launcher once"), "old pack: the reason says how it is cured");
      Check(st::HideSeat(o, 397) && st::HideSeat(o, 398) && !st::HideSeat(o, 399) && !st::HideSeat(o, 100), "old pack: the held seats are not offered (no frame needed); others are the game's");
      Check(st::KeepUniverseState(o, 397) && st::KeepUniverseState(o, 398) && !st::KeepUniverseState(o, 399), "old pack: the Universe check keeps the old stars' state, nothing else");
      Check(Has(Joined(st::RosterLog(o)), "old pack"), "old pack: the log says so");
    }

    // an empty / missing library: no residents, no throw
    st::Roster e;
    st::LoadRoster(work / "nothing_here", 6, kFrameText, e);
    Check(e.loaded && e.res.empty() && e.left_out.empty(), "library: no library folder = no residents");
    Check(!st::OldPackPresent(root), "old pack: none here");
    fs::create_directories(work / "old" / "pack6");
    Check(st::OldPackPresent(work / "old"), "old pack: a pack6 folder is seen (the launcher migrates it)");
  }

  std::printf("seats_unit: %d checks, %d failed\n", g_checks, g_fail);
  std::printf("%s\n", g_fail ? "seats_unit FAILED" : "seats_unit OK");
  return g_fail ? 1 : 0;
}
