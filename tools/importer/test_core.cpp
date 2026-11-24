// Session 98: the Superstar Importer core's PC check (src\importer). Built by tools\importer\build_test.ps1; runs on this PC
// with the player's own files and writes ONLY under the --mods folder you give it (keep it under out\: game data).
//
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source ZIP_OR_PAC [--slot 397] [--base 100] [--name "Mr. Kennedy"]
//                 [--short KENNEDY] [--entrance-from N] [--selftest-only] [--render]
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source ZIP_OR_PAC --fit-cases [--stfs-dir DIR]
//                 Session 101: the "why imports fail" cases (FitCases below), each in OUT_DIR\fit_<case>
//
// 1. selftest: the writers against the real packs (arcs rebuilt from the pacs' directories, chEtc / DLC_HD re-packed: byte for
//    byte, like build_mod_pack.py --selftest); deflate / BC3 / string-table round trips.
// 2. InspectSources + ExtractSource on the source, then BuildPack, then reads the pack back (the game's parser order, arcs vs
//    pac directories, the name texts, row SLOT, the model names). Exit code = failures.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/dlc_catalog.h"
#include "../../src/importer/importer_core.h"
#include "../../src/importer/pack_fit.h"
#include "../../src/importer/pack_io.h"
#include "../../src/importer/pack_entry.h"
#include "../../src/importer/enex_rows.h"
#include "../../src/importer/bc3.h"
#include "../../src/xctd_file.h"
#include "../../src/importer/asset_ids.h"
#include "../../src/superstars_migrate.h"
#include "../../src/tron_slots_core.h"

namespace fs = std::filesystem;
using importer::io::Bytes;
namespace io = importer::io;

static int g_fail = 0;
static bool Say(bool ok, const std::string& m) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", m.c_str());
  if (!ok) ++g_fail;
  return ok;
}
static Bytes Read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static fs::path Find(const fs::path& root, std::string rel) {
  for (char& c : rel)
    if (c == '\\') c = '/';
  fs::path p = root / rel;
  std::error_code ec;
  if (fs::exists(p, ec)) return p;
  for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
    std::string a = e.path().filename().string(), b = p.filename().string();
    for (char& c : a) c = char(std::tolower((unsigned char)c));
    for (char& c : b) c = char(std::tolower((unsigned char)c));
    if (a == b) return e.path();
  }
  return {};
}

// S110 (lane BUNDLE, docs\research\s110\MASK64.md): the pack's one type-21 entry. No game files needed.
static void BundleUnits() {
  std::printf("bundle entry (pack_entry.h):\n");
  namespace pe = importer::pack_entry;
  std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
  gate[0] = 6u << 16 | 1;
  auto make_real = [](int top) {  // the player's four packs end at entry id 12
    std::vector<io::CatEntry> r;
    io::CatEntry e;
    e.id = uint16_t(top), e.type = 1, e.pack = 5, e.char_id = 393;
    r.push_back(e);
    return r;
  };
  auto cat_of = [&](const pe::PackEntryPlan& p) {
    const Bytes cat = io::WriteCatalog(6, 32, {p.entry}, p.bundles, {{"pac\\x.pac", -1, -1}}, 1);
    io::CatInfo info;
    const std::string why = io::ParseCatalogLikeGame(cat, gate, info);
    if (!why.empty()) std::printf("    parse: %s\n", why.c_str());
    return info;
  };
  std::vector<int> slots;
  for (int i = 0; i < 103; ++i) slots.push_back(397 + i);
  const pe::PackEntryPlan p103 = pe::PlanPackEntry(make_real(12), slots, 6, "Superstar Importer");
  const io::CatInfo i103 = cat_of(p103);
  Say(p103.error.empty() && i103.entries.size() == 1 && i103.entries[0].id <= 63 && i103.entries[0].id == 13 && i103.entries[0].type == 21 &&
          i103.entries[0].char_id == -1 && i103.entries[0].attr1 == -1 && i103.entries[0].bit == 0,
      "103 slots: one type-21 entry, id " + std::to_string(i103.entries.empty() ? -1 : i103.entries[0].id) + ", +16 / +20 = -1, licence bit 0");
  Say(i103.bundles.size() == 2 && i103.bundles[0].rows.size() == 64 && i103.bundles[1].rows.size() == 39, "103 slots: two bundle records, 64 + 39 rows");
  bool all = !i103.entries.empty();
  for (int s : slots) all = all && pe::EntryIdFor(i103, s, 0) == i103.entries[0].id;
  Say(all && pe::EntryIdFor(i103, 396, 0) == -1 && pe::EntryIdFor(i103, 397, 1) == -1, "103 slots: EntryIdFor = the entry's id for every slot (none for a slot outside, none for attire 1)");
  Say(pe::AvailabilityRows(i103) == 1 && i103.entries.size() == 1, "103 slots: the entry adds exactly one availability row (the real packs fill the table's entries + 2 exactly)");
  // S110 review: availability is a different lookup from EntryIdFor (sub_828173E8 reads only the records whose key = the entry's +24)
  bool avail = true;
  for (int s : slots) avail = avail && pe::AvailableLikeGame(i103, s);
  Say(avail && !pe::AvailableLikeGame(i103, 396), "103 slots: every slot is available the game's way (bundle key = the entry's +24), none outside");
  {  // planted twin: the entry's +24 is 1 in the file, the records keep key 0 -> the content-mask id still answers, availability does not
    Bytes cat = io::WriteCatalog(6, 32, {p103.entry}, p103.bundles, {{"pac\\x.pac", -1, -1}}, 1);
    cat[28 + 24] = 1;  // the one entry starts right after the 28-byte header
    io::CatInfo tw;
    const std::string why = io::ParseCatalogLikeGame(cat, gate, tw);
    Say(why.empty() && pe::EntryIdFor(tw, 397, 0) == 13 && !pe::AvailableLikeGame(tw, 397),
        "planted twin (entry +24 = 1, records' key 0): EntryIdFor still answers 13, AvailableLikeGame refuses slot 397");
  }
  const pe::PackEntryPlan none = pe::PlanPackEntry({}, slots, 6, "x");  // no DLC installed
  Say(none.error.empty() && none.entry.id == 13, "no DLC installed: the id is " + std::to_string(none.entry.id) + " (13, not 92)");
  Say(!pe::PlanPackEntry(make_real(63), slots, 6, "x").error.empty() && pe::PlanPackEntry(make_real(62), slots, 6, "x").entry.id == 63,
      "highest real id 63: the build is refused with an error; 62: the id is 63");
  // the planted twin: the OLD form, one type-1 entry per character numbered from 13, for 60 slots: the 52nd is above 63 and the game drops it
  io::CatInfo old;
  for (int k = 0; k < 60; ++k) {
    io::CatEntry e;
    e.id = uint16_t(13 + k), e.type = 1, e.pack = 6, e.char_id = 397 + k;
    old.entries.push_back(e);
  }
  Say(pe::EntryIdFor(old, 397 + 50, 0) == 63 && pe::EntryIdFor(old, 397 + 51, 0) == -1 && pe::EntryIdFor(old, 397 + 59, 0) == -1,
      "planted twin (old form, 60 slots): the 51st answers 63, the 52nd and later answer -1 (the bug)");
  // a bundle record of more than 64 rows is refused by the parser, as the game does not bound-check it
  io::CatBundle big;
  big.pack = 6;
  for (int k = 0; k < 65; ++k) big.rows.push_back({uint16_t(397 + k), 0, 0});
  io::CatInfo bi;
  const Bytes bc = io::WriteCatalog(6, 32, {p103.entry}, {big}, {{"pac\\x.pac", -1, -1}}, 1);
  Say(!io::ParseCatalogLikeGame(bc, gate, bi).empty(), "a bundle record of 65 rows is refused by ParseCatalogLikeGame");
}

static void SelfTest(const std::string& dlc) {
  BundleUnits();
  std::printf("selftest (writers vs the real packs):\n");
  std::set<int> row_packs;  // S110 review: the availability rows of the player's packs (IMP_B: packs 2-5 make 14 rows from 12 entries)
  size_t rows_all = 0, entries_all = 0;
  for (const auto& pk : wwe2k14::dlc::Scan(fs::u8path(dlc))) {
    if (pk.status != wwe2k14::dlc::Status::kReady || pk.package) continue;
    const Bytes cat = Read(pk.root / "info" / "catalog.dlc");
    io::CatInfo info;
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    const std::string why = io::ParseCatalogLikeGame(cat, gate, info);
    Say(why.empty(), "pack " + std::to_string(pk.pack_id) + " catalog parses like sub_8281F150" + (why.empty() ? "" : ": " + why));
    if (why.empty()) {
      row_packs.insert(info.pack), rows_all += importer::pack_entry::AvailabilityRows(info), entries_all += info.entries.size();
      size_t n = 0;
      bool all = true;
      for (const auto& bd : info.bundles)
        for (const auto& r : bd.rows) ++n, all = all && importer::pack_entry::AvailableLikeGame(info, r.ch);
      if (n) Say(all, "pack " + std::to_string(pk.pack_id) + ": its " + std::to_string(n) + " bundled characters are available the game's way (no type-1 entry of their own)");
    }
    if (!why.empty() || info.files.empty()) continue;
    bool names31 = true;  // (the writer keeps names to 31 bytes + NUL; pack 3's first name is 32 bytes with no NUL, a limit we do not need)
    for (const auto& e : info.entries) names31 = names31 && e.name.size() < 32;
    if (!info.bundles.empty() && io::L16(&cat[14]) == 0 && names31) {  // S110: WriteCatalog with the bundle records = the real file, byte for byte
      uint64_t ft = 0;
      for (int k = 7; k >= 0; --k) ft = ft << 8 | cat[20 + size_t(k)];
      const Bytes mine = io::WriteCatalog(info.pack, info.revision, info.entries, info.bundles, info.files, ft);
      size_t same = 0;
      while (same < mine.size() && same < cat.size() && mine[same] == cat[same]) ++same;
      Say(mine == cat, "pack " + std::to_string(pk.pack_id) + " catalog: WriteCatalog (entries + " + std::to_string(info.bundles.size()) +
                           " bundle record(s)) = the real file byte for byte (" + std::to_string(same) + " of " + std::to_string(cat.size()) + " match)");
    }
    for (int col = 0; col < 2; ++col) {
      std::map<int, std::string> idx;
      for (const auto& f : info.files) {
        const int k = col ? f.b : f.a;
        if (k >= 0) idx[k] = f.name;
      }
      std::vector<io::Pac> pacs(idx.size());
      std::vector<const io::Pac*> ptr;
      bool ok = true;
      for (auto& [k, n] : idx) {
        std::string e;
        ok = ok && io::ReadPac(Read(Find(pk.root, n)), pacs[size_t(k)], e);
        ptr.push_back(&pacs[size_t(k)]);
      }
      const Bytes arc = Read(pk.root / "info" / (col ? "catalog_sd.arc" : "catalog_hd.arc"));
      Say(ok && io::WriteArc(ptr) == arc, "pack " + std::to_string(pk.pack_id) + (col ? " catalog_sd.arc" : " catalog_hd.arc") + " rebuilt from the pacs: identical");
    }
    for (const auto& f : info.files) {
      std::string l = f.name;
      for (char& c : l) c = char(std::tolower((unsigned char)c));
      if (l.rfind("pac\\ch\\chetc_dlc", 0) != 0 && l.rfind("pac\\dlc_hd_dlc", 0) != 0 && l.rfind("pac\\string_dlc", 0) != 0) continue;
      const Bytes orig = Read(Find(pk.root, f.name));
      io::Pac p;
      std::string e;
      io::ReadPac(orig, p, e);
      for (auto& d : p.dirs)
        for (auto& x : d.ents) {
          std::vector<io::Leaf> leaves;
          if (io::ReadPach(x.blob, leaves)) {
            Bytes w = io::WritePach(leaves);
            const size_t n = x.blob.size();
            if (w.size() <= n) w.resize(n, 0);
            x.blob = w;
          }
        }
      Say(io::WritePac(p) == orig, f.name + " re-packed (every PACH re-written): identical");
    }
  }
  if (row_packs == std::set<int>{2, 3, 4, 5})  // (S110 review) the transcribed row rule against IMP_B's count on these four packs
    Say(rows_all == 14 && entries_all == 12, "packs 2-5: AvailabilityRows makes " + std::to_string(rows_all) + " rows from " + std::to_string(entries_all) +
                                                 " entries (14 from 12: the table's entries + 2, filled exactly)");
  // crafted pacs (review): overlapping entries and an entry past the end are refused, not copied
  {
    io::Pac c;
    c.magic = "EPK8";
    c.dirs.resize(1);
    c.dirs[0].tag = "EMD ", c.dirs[0].kind = 1;
    for (int i = 0; i < 2; ++i) {
      io::Entry e;
      e.name = "0001000" + std::to_string(i * 2);
      e.blob.assign(1 << 20, uint8_t(i));
      c.dirs[0].ents.push_back(e);
    }
    Bytes good = io::WritePac(c);
    io::Pac back;
    std::string e;
    Say(io::ReadPac(good, back, e), "a well-formed test pac reads back");
    Bytes over = good;  // entry 1 re-pointed at sector 0 with a different size: overlaps entry 0
    io::W32(&over[0x800 + 12 + 16 + 8], 0), io::W32(&over[0x800 + 12 + 16 + 12], (2 << 20) / 256);  // (the whole data: more bytes copied than the file has)
    Say(!io::ReadPac(over, back, e), "overlapping entries refused (" + e + ")");
    Bytes past = good;
    io::W32(&past[0x800 + 12 + 16 + 8], 100000);
    Say(!io::ReadPac(past, back, e), "an entry past the end refused (" + e + ")");
    std::vector<importer::SourceInfo> none = importer::InspectSources("C:\\no\\such\\file.pac");
    Say(none.size() == 1 && !none[0].ok, "InspectSources on a missing file: '" + none[0].err + "'");
  }
  // deflate round trip
  Bytes t;
  for (int i = 0; i < 200000; ++i) t.push_back(uint8_t("THE ROCK\0SCOTT HALL\0"[i % 20] + (i % 997 == 0)));
  const Bytes leaf = io::ZlibLeaf(t, 0x1230);
  Say(io::Decode(leaf) == t, "zlib leaf round trip (" + std::to_string(t.size()) + " -> " + std::to_string(leaf.size()) + " bytes)");
  Bytes rnd(70000);
  uint32_t s = 1;
  for (auto& c : rnd) s = s * 1103515245 + 12345, c = uint8_t(s >> 16);
  Say(io::Decode(io::ZlibLeaf(rnd, 0x1230)) == rnd, "zlib leaf round trip (70000 random bytes)");
  Say(io::Decode(io::BpeIdentity(rnd)) == rnd, "identity BPE round trip across Gage blocks (70000 bytes)");
  // BC3: a gradient with soft alpha, decoded back by our own reader
  std::vector<uint8_t> img(64 * 64 * 4);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      uint8_t* p = &img[(size_t(y) * 64 + x) * 4];
      p[0] = uint8_t(x * 4), p[1] = uint8_t(y * 4), p[2] = 128, p[3] = uint8_t((x + y) * 2);
    }
  const Bytes dds = importer::bc3::Dds(img, 64, 64);
  double err = 0;
  for (int by = 0; by < 16; ++by)
    for (int bx = 0; bx < 16; ++bx) {
      const uint8_t* blk = &dds[128 + (size_t(by) * 16 + bx) * 16];
      const int a0 = blk[0], a1 = blk[1];
      uint64_t abits = 0;
      for (int k = 0; k < 6; ++k) abits |= uint64_t(blk[2 + k]) << (8 * k);
      const uint16_t c0 = uint16_t(blk[8] | blk[9] << 8), c1 = uint16_t(blk[10] | blk[11] << 8);
      const uint32_t ci = uint32_t(blk[12] | blk[13] << 8 | blk[14] << 16 | uint32_t(blk[15]) << 24);
      float pc[4][3];
      auto u = [](uint16_t v, float* c) { c[0] = float((v >> 11) & 31) * 255 / 31, c[1] = float((v >> 5) & 63) * 255 / 63, c[2] = float(v & 31) * 255 / 31; };
      u(c0, pc[0]), u(c1, pc[1]);
      for (int c = 0; c < 3; ++c) pc[2][c] = (2 * pc[0][c] + pc[1][c]) / 3, pc[3][c] = (pc[0][c] + 2 * pc[1][c]) / 3;
      for (int i = 0; i < 16; ++i) {
        const int ai = int((abits >> (3 * i)) & 7);
        int a = ai == 0 ? a0 : ai == 1 ? a1 : a0 > a1 ? ((8 - ai) * a0 + (ai - 1) * a1) / 7 : 0;
        const uint8_t* p = &img[(size_t(by * 4 + i / 4) * 64 + bx * 4 + i % 4) * 4];
        const float* c = pc[(ci >> (2 * i)) & 3];
        err += std::abs(a - p[3]) + std::abs(c[0] - p[0]) + std::abs(c[1] - p[1]) + std::abs(c[2] - p[2]);
      }
    }
  err /= 64.0 * 64 * 4;
  Say(err < 4.0, "BC3 encode of a 64x64 gradient: mean abs error " + std::to_string(err) + " (< 4)");
}

// ---- Session 101 (lane 199, "why imports fail"): one case per way an imported superstar could miss the game. Each builds its own
// pack under <mods>\fit_<case> from the player's own files (nothing else is written) and checks it with the SAME rule the game
// applies at boot (src\importer\pack_fit.h Problem, through importer::PackProblem). DLC sets are varied with the launcher's own
// switch (dlc_off) and USE MY DLC off (dlc_dir ""), so no pack is copied. --stfs-dir DIR: a folder holding ONE Xbox 360 package
// made from the top pack (tools\dlc\make_test_stfs.py "<dlc>\DLC Pack 4" DIR\pack5 --name "Legends Pack"), for the package case.
static std::string TextOf(const fs::path& p) {
  const Bytes b = Read(p);
  return std::string(b.begin(), b.end());
}
static std::string LineOf(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);)
    if (l.rfind(key + " ", 0) == 0 || l == key) return l;
  return {};
}
static bool EnexLeaves(const Bytes& pacfile, const char* name, io::Pac& pac, std::vector<io::Leaf>& out) {
  std::string e;
  if (!io::ReadPac(pacfile, pac, e)) return false;
  io::Entry* en = pac.Find("ENEX", name);
  return en && io::ReadPach(io::Decode(en->blob), out);
}
static int ProWord(const Bytes& chetc, int slot, size_t off) {
  io::Pac pc;
  std::string e;
  std::vector<io::Leaf> pl;
  io::Entry* pe = nullptr;
  if (!io::ReadPac(chetc, pc, e) || !(pe = pc.Find("CHAR", "PRO ")) || !io::ReadPach(pe->blob, pl)) return -1;
  for (const io::Leaf& l : pl)
    if (int(l.id) == slot) {
      const Bytes r = io::Decode(l.raw);
      return r.size() >= 604 ? io::L16(&r[off]) : -1;
    }
  return -1;
}

// S110 ENTTAB (docs\research\s110\BEAMS.md): the pack's pac\evt\evtdata_dlc06.pac (`built`; empty = no such pac) against the top pack's
// evtdata (`top`, the cumulative source). "" when: table 102 = the source's rows + for each star (slot, e) whose e has a row and whose slot
// has none, e's row with column 0 = slot (the column 5 gate: the beams' light follows the rig); every other table decodes byte-equal to
// the source's. No row of e's = nothing expected (and no pac needed).
using Rows = std::vector<std::vector<long>>;
static Rows ParseRows(const Bytes& t) {
  Rows r;
  std::istringstream in(std::string(t.begin(), std::find(t.begin(), t.end(), uint8_t(0))));
  std::string ln;
  while (std::getline(in, ln)) {
    std::istringstream ls(ln);
    std::vector<long> v;
    long x;
    while (ls >> x) v.push_back(x);
    if (!v.empty()) r.push_back(v);
  }
  return r;
}
static bool DataLeaves(const Bytes& pac, std::vector<io::Leaf>& inner) {
  io::Pac p;
  std::string e;
  std::vector<io::Leaf> outer;
  io::Entry* en = nullptr;
  return io::ReadPac(pac, p, e) && (en = p.Find("CSDT", "DATA")) && io::ReadPach(en->blob, outer) && outer.size() == 1 &&
         io::ReadPach(io::Decode(outer[0].raw), inner);
}

// S110 ENTFX / WINNING (docs\research\s110\IMPREACH.md rows 2-4): the entrance's scene objects, particles and the victory scene of the
// wrestler whose entrance a star uses. SrcEntry finds an entry (directory `tag`, name `name`) in the pac `stem` of the real packs (any
// ready folder pack), else in the disc's files `disc_rels` (XCTD unpacked): the expected side, read here without the importer's code.
static Bytes UnxctdFile(const fs::path& p) {
  Bytes d = Read(p);
  if (!wwe2k14::xctd::IsXctd(d)) return d;
  Bytes o;
  std::string e;
  return wwe2k14::xctd::Unpack(d, o, e) ? o : Bytes();
}


// ---- S110 BANKID (docs\research\s110\SLOTID.md, TEXTID.md)
// The entrance wrestler's bank WORD: his PRO row's u16 at +580 in the top pack's chEtc (the disc's when top_id == 0), read here from the files.
static int TopBankWord(const std::string& game, const std::string& dlc, int top_id, int e) {
  Bytes chetc;
  if (top_id > 0) {
    for (const auto& pk : wwe2k14::dlc::Scan(fs::u8path(dlc)))
      if (pk.status == wwe2k14::dlc::Status::kReady && !pk.package && pk.pack_id == top_id) {
        char nm[64];
        std::snprintf(nm, sizeof nm, "pac/ch/chEtc_dlc%02d.pac", top_id);
        chetc = Read(Find(pk.root, nm));
      }
  } else {
    chetc = UnxctdFile(Find(fs::u8path(game), "pac/ch/chEtc.pac"));
  }
  io::Pac p;
  std::string er;
  std::vector<io::Leaf> pro;
  io::Entry* pe = nullptr;
  if (!io::ReadPac(chetc, p, er) || !(pe = p.Find("CHAR", "PRO ")) || !io::ReadPach(pe->blob, pro)) return 0;
  for (const io::Leaf& l : pro)
    if (int(l.id) == e) {
      const Bytes r = io::Decode(l.raw);
      return r.size() >= 604 ? io::L16(&r[580]) : 0;
    }
  return 0;
}

// Every numeric name of an EVP / EVZ / GMEV / PCE6 directory in the disc's and the packs' pacs, read with io::ReadPac over EVERY pac of
// pac\evt (but the evt* data pacs), gm and prtcls: independent of asset_ids' own directory scan.
static std::set<int> DirNames(const std::string& game, const std::string& dlc) {
  std::set<int> used;
  auto add = [&](const Bytes& d) {
    io::Pac p;
    std::string er;
    if (!io::ReadPac(d, p, er)) return;
    for (const io::Dir& dir : p.dirs)
      if (dir.tag == "EVP " || dir.tag == "EVZ " || dir.tag == "GMEV" || dir.tag == "PCE6")
        for (const io::Entry& en : dir.ents) {
          std::string n = io::Trim(en.name);
          n.erase(0, n.find_first_not_of(' '));
          if (!n.empty() && n.find_first_not_of("0123456789") == std::string::npos) used.insert(std::atoi(n.c_str()));
        }
  };
  auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
  };
  auto scan_dir = [&](const fs::path& dir, bool evt) {
    std::error_code ec2;
    for (const auto& de : fs::directory_iterator(dir, ec2)) {
      const std::string n = lower(de.path().filename().string());
      if (n.size() < 5 || n.compare(n.size() - 4, 4, ".pac") != 0) continue;
      if (evt ? n.rfind("evt", 0) == 0 : !(n.rfind("gm", 0) == 0 || n.rfind("prtcls", 0) == 0)) continue;
      add(UnxctdFile(de.path()));
    }
  };
  const fs::path g = fs::u8path(game);
  scan_dir(g / "pac" / "evt", true);
  scan_dir(g / "pac", false);
  for (const auto& pk : wwe2k14::dlc::Scan(fs::u8path(dlc)))
    if (pk.status == wwe2k14::dlc::Status::kReady && !pk.package) {
      scan_dir(pk.root / "pac" / "evt", true);
      scan_dir(pk.root / "pac", false);
    }
  return used;
}


// Pure checks: the free list on these files, Of(), the gap slots, the planted twins and the save migration.
static void BankIdUnits(const std::string& game, const std::string& dlc) {
  std::printf("dedicated entrance banks (S110 BANKID):\n");
  namespace ai = importer::asset_ids;
  const std::set<int> names = DirNames(game, dlc);
  const std::vector<int> fr = ai::Config().free_numbers;  // the last build's free list (built from the same files)
  bool none = true, legal = true;
  for (int n : fr) {
    bool taken = names.count(n) != 0;
    for (int k = 0; k < 10; ++k) taken = taken || names.count(10000 + 1000 * k + n);
    none = none && !taken;
    legal = legal && n >= 1 && n <= 599 && n != 103 && n != 322 && n != 114 && n != 319 && n != 160 && n != 385 && n != 210 &&
            n != 409 && n != 309 && n != 420 && n != 422 && n != 421;
  }
  // (review) Legal = 1..599 (one class for every range test), minus names, code constants, carried values and character ids. A library of
  // 268 slots (103 + 165 gap ids) needs 268.
  Say(fr.size() >= 268, "BANKID: the free list on these files has " + std::to_string(fr.size()) + " numbers (at least 268 needed: 103 slots 397..499 + 165 gap ids; " +
                            std::to_string(names.size()) + " names read)");
  Say(none && legal, "BANKID: no free number (nor its 10000 + 1000 k + n form) is a name in the disc's / packs' directories (checked with io::ReadPac), all in 1..599");
  {  // (review) carried values, read here from the files with the test's own readers: no free number is a value of the top pack's (and the
     // disc's) /ENEX/LSD bank lists or BIN leaves 0..4, nor a key of table 102, nor a character's id (TEXTID: gap id 110 is a disc motion bank)
    std::set<int> carried;
    std::vector<std::string> pacs = {(fs::u8path(game) / "pac" / "DLC.pac").string()};
    for (const auto& pk : wwe2k14::dlc::Scan(fs::u8path(dlc)))
      if (pk.status == wwe2k14::dlc::Status::kReady && !pk.package) {
        char nm[64];
        std::snprintf(nm, sizeof nm, "pac/DLC_dlc%02d.pac", pk.pack_id);
        const fs::path f = Find(pk.root, nm);
        if (!f.empty()) pacs.push_back(f.string());
      }
    int lists_read = 0;
    for (const std::string& f : pacs) {
      io::Pac p;
      std::vector<io::Leaf> lsd, bin;
      if (EnexLeaves(UnxctdFile(fs::u8path(f)), "LSD ", p, lsd))
        for (const io::Leaf& l : lsd)
          if (l.id == 3 || l.id == 17 || l.id == 18 || l.id == 16 || (l.id >= 8 && l.id <= 11)) {
            const Bytes m = io::Decode(l.raw);
            ++lists_read;
            for (size_t r = 0; m.size() >= 2 && r < io::L16(&m[0]) && 2 + 8 * (r + 1) <= m.size(); ++r) {
              const int v = int16_t(io::L16(&m[2 + 8 * r]));
              carried.insert(v >= 10000 ? (v - 10000) % 1000 : v);
            }
          }
      if (EnexLeaves(UnxctdFile(fs::u8path(f)), "BIN ", p, bin))
        for (const io::Leaf& l : bin)
          if (l.id <= 4) {
            const Bytes m = io::Decode(l.raw);
            for (size_t r = 0; m.size() >= 2 && r < io::L16(&m[0]) && 16 + 20 * (r + 1) <= m.size(); ++r) {
              const int v = int16_t(io::L16(&m[16 + 20 * r + 8]));
              carried.insert(v >= 10000 ? (v - 10000) % 1000 : v);
            }
          }
    }
    std::string hit;
    for (int n : fr)
      if (carried.count(n)) hit += " " + std::to_string(n);
    const bool no110 = std::find(fr.begin(), fr.end(), 110) == fr.end();
    Say(lists_read > 0 && hit.empty() && carried.count(110) && no110,
        "BANKID: no free number is a value of the ENEX bank lists / part leaves (" + std::to_string(carried.size()) + " values from " + std::to_string(lists_read) +
            " lists; 110, a disc motion bank, is not free)" + (hit.empty() ? "" : ": carried and free:" + hit));
  }
  std::set<int> seen;
  bool distinct = true;
  for (int s = importer::kFirstSlot; s <= importer::kLastSlot; ++s) {
    const int b = ai::Of(s).bank;
    distinct = distinct && b > 0 && seen.insert(b).second && ai::Of(s).victory == 6000 + s && ai::Of(s).plate == 500 + s && ai::Of(s).PartBank(2) == 12000 + b;
  }
  Say(distinct && seen.size() == 103, "BANKID: Of(slot) gives 103 distinct banks for slots 397..499 (victory 6000 + slot, plate 500 + slot unchanged)");
  Say(!ai::Of(498).own_plate && !ai::Of(499).own_plate && ai::Of(497).own_plate, "BANKID: slots 498 / 499 keep the 'no own plate' rule");
  {  // gap slots (a library's ids in 100..376) get banks after the 103; the ordinal depends on the slot only
    const std::vector<int> keep = fr;
    ai::SetFreeList(keep, {150, 120});
    const int a = ai::Of(120).bank, b = ai::Of(150).bank, c = ai::Of(397).bank;
    ai::SetFreeList(keep, {120, 150, 333});
    Say(a == keep[103] && b == keep[104] && c == keep[0] && ai::Of(397).bank == c && ai::Of(120).bank == a && ai::Of(150).bank == b && ai::Of(130).bank == -1,
        "BANKID: gap slots take the numbers after the 103 by their rank; an existing slot's bank does not move when another gap slot joins");
    ai::SetFreeList({1, 2});
    Say(!ai::NoBankProblem(399).empty() && ai::NoBankProblem(397).empty(), "BANKID: a slot with no free number is refused ('" + ai::NoBankProblem(399) + "')");
    ai::SetFreeList(keep);
  }
  {  // (review) Assign: a slot keeps his recorded bank across rebuilds, also when another pack's names are added to the scan
    const std::vector<int> keep = fr;
    ai::SetFreeList(keep);
    const std::vector<int> slots = {397, 398, 400, 450};
    std::map<int, int> moved;
    const std::string e1 = ai::Assign(slots, {}, &moved);
    std::map<int, int> rec;
    for (int s : slots) rec[s] = ai::Of(s).bank;
    const bool first = e1.empty() && moved.empty() && rec[397] == keep[0] && rec[398] == keep[1] && rec[400] == keep[3] && rec[450] == keep[53];
    // a DLC pack installed since names keep[1] (398's bank) and keep[0]: the free list loses them (planted)
    std::vector<int> shrunk(keep.begin() + 2, keep.end());
    ai::SetFreeList(shrunk);
    rec[397] = keep[60];  // (397's recorded bank was moved by hand: still free, so he keeps it)
    moved.clear();
    const std::string e2 = ai::Assign(slots, rec, &moved);
    std::set<int> distinct;
    for (int s : slots) distinct.insert(ai::Of(s).bank);
    const bool kept = e2.empty() && ai::Of(397).bank == keep[60] && ai::Of(400).bank == keep[3] && ai::Of(450).bank == keep[53];
    const bool moved_ok = moved.size() == 1 && moved.count(398) && moved[398] == keep[1] && ai::Of(398).bank != keep[1] && distinct.size() == slots.size();
    Say(first && kept && moved_ok,
        "BANKID: Assign: recorded banks are kept while free (397 keeps his hand-moved " + std::to_string(keep[60]) + ", 400 / 450 theirs); 398's bank, now named by "
        "another pack, moves (reported for `was`); no two slots share one");
    // a recorded bank another slot of the pack wants by ordinal is not handed out twice; a bank of a slot that left the pack is not reused first
    ai::SetFreeList(keep);
    const std::string e3 = ai::Assign({397, 398}, {{398, keep[0]}, {499, keep[1]}});
    Say(e3.empty() && ai::Of(398).bank == keep[0] && ai::Of(397).bank != keep[0] && ai::Of(397).bank != keep[1],
        "BANKID: Assign: 398 keeps " + std::to_string(keep[0]) + " (recorded) so 397 does not take it by ordinal, nor 499's old " + std::to_string(keep[1]) + " (got " +
            std::to_string(ai::Of(397).bank) + ")");
    ai::SetFreeList(keep);
  }
  {  // the planted twins
    Say(!ai::NameProblem(names, 410).empty(), "BANKID: twin: a bank that exists on the disc (410) is refused: '" + ai::NameProblem(names, 410) + "'");
    Say(ai::NameProblem(names, ai::Of(397).bank).empty(), "BANKID: his own bank passes the same check");
    Say(!ai::OutputProblem({397}, 397, ai::Of(397).bank).empty() && ai::OutputProblem({ai::Of(397).bank}, 397, ai::Of(397).bank).empty(),
        "BANKID: twin: a pack holding /EVP/<slot> is refused");
    Say(ai::OutputProblem({400}, 400, ai::Of(400).bank, {400}).empty() && !ai::OutputProblem({400}, 400, ai::Of(400).bank, {401}).empty(),
        "BANKID: a name equal to a slot number that is ANOTHER import's assigned bank is his, not a shadow (refused only when no import owns it)");
    Bytes row(1276, 0);
    io::W16(&row[580], uint16_t(ai::Of(397).bank)), io::W16(&row[596], 397);
    Say(!ai::SlotWordProblem(row, 397, ai::Of(397).bank).empty(), "BANKID: twin: a PRO row with +596 = the slot is refused");
  }
  {  // (S110 TEXTID) the old numbering (bank = slot: 410 / 413 / 452 are numbers a real wrestler's motion row carries) is REFUSED by the row writers
    int top = 0;
    Bytes dlcpac;
    for (const auto& pk : wwe2k14::dlc::Scan(fs::u8path(dlc)))
      if (pk.status == wwe2k14::dlc::Status::kReady && !pk.package && pk.pack_id > top && pk.pack_id <= 5) {
        char nm[64];
        std::snprintf(nm, sizeof nm, "pac/DLC_dlc%02d.pac", pk.pack_id);
        const Bytes b = Read(Find(pk.root, nm));
        if (!b.empty()) top = pk.pack_id, dlcpac = b;
      }
    int refused = 0, tried = 0;
    std::string last;
    for (int old_bank : {410, 413, 452, 499}) {
      io::Pac p;
      std::string er;
      if (dlcpac.empty() || !io::ReadPac(dlcpac, p, er)) break;
      ai::SetFreeList({old_bank});
      importer::Superstar s;
      s.slot = importer::kFirstSlot, s.name = "old", s.base_id = 100, s.src_bank = TopBankWord(game, dlc, top, 100);
      std::vector<std::string> lg;
      ++tried;
      if (!importer::enex_rows::PatchEnexLsd(lg, p, {s}, {}, er)) ++refused, last = er;
    }
    ai::SetFreeList(fr);
    Say(tried > 0 && refused > 0, "TEXTID: twin: the old numbering (bank = slot 410 / 413 / 452 / 499) is refused by PatchEnexLsd where a real motion row carries the number (" +
                                      std::to_string(refused) + " of " + std::to_string(tried) + "): '" + last + "'");
  }
  {  // the migration shim (src\superstars_migrate.h): one profile pair
    namespace mg = wwe2k14::superstars_migrate;
    std::vector<uint8_t> live(mg::kProfileBytes, 0), pack(mg::kProfileBytes, 0);
    auto put = [](std::vector<uint8_t>& v, size_t off, int val) { v[off] = uint8_t(val >> 8), v[off + 1] = uint8_t(val); };
    auto get = [](const std::vector<uint8_t>& v, size_t off) { return int(v[off] << 8 | v[off + 1]); };
    const int slot = 397, bank = 17;
    for (size_t off : {576u, 592u, 596u}) put(live, off, slot);  // an old save: bank words = the slot (+ champion word +596 = the slot)
    put(live, 600, 1234);
    for (int k = 0; k < 5; ++k) put(live, 618 + 34 * size_t(k), 10000 + 1000 * k + slot);
    for (size_t off : {576u, 592u, 596u}) put(pack, off, bank);
    const std::vector<uint8_t> before = live;
    std::string what;
    const int n = mg::MigrateProfile(live.data(), pack.data(), slot, &what);
    bool parts = true;
    for (int k = 0; k < 5; ++k) parts = parts && get(live, 618 + 34 * size_t(k)) == 10000 + 1000 * k + bank;
    Say(n == 8 && get(live, 576) == bank && get(live, 592) == bank && get(live, 596) == bank && get(live, 600) == 1234 && parts,
        "BANKID: migration: an old save's profile (bank = slot, part banks 10000 + 1000 k + slot) gets the pack's bank and 10000 + 1000 k + bank (" + std::to_string(n) + " words)");
    const std::vector<uint8_t> after = live;
    Say(mg::MigrateProfile(live.data(), pack.data(), slot, nullptr) == 0 && live == after, "BANKID: migration: an already current profile is not touched");
    std::vector<uint8_t> edited = before;
    put(edited, 576, 55);  // the player picked another motion: not the slot, so not ours
    mg::MigrateProfile(edited.data(), pack.data(), slot, nullptr);
    Say(get(edited, 576) == 55 && get(edited, 592) == bank, "BANKID: migration: a word that is not the slot (the player's own pick) is left");
    std::vector<uint8_t> oldpack(mg::kProfileBytes, 0), l2 = before;
    for (size_t off : {576u, 592u, 596u}) put(oldpack, off, slot);
    Say(mg::MigrateProfile(l2.data(), oldpack.data(), slot, nullptr) == 0 && l2 == before, "BANKID: migration: a pack that still holds bank = slot (an old build) moves nothing");
    // (review) a save made between two builds holds the bank of the FIRST (17); the second moved him to 23 (superstars.txt `was 17`)
    std::vector<uint8_t> mid(mg::kProfileBytes, 0), pack2(mg::kProfileBytes, 0);
    for (size_t off : {576u, 592u}) put(mid, off, bank), put(pack2, off, 23);
    put(mid, 618 + 34, 11000 + bank);
    const std::vector<uint8_t> mid0 = mid;
    const int nw = mg::MigrateProfile(mid.data(), pack2.data(), slot, nullptr, {bank});
    Say(nw == 3 && get(mid, 576) == 23 && get(mid, 592) == 23 && get(mid, 618 + 34) == 11023,
        "BANKID: migration: a save holding his previous bank (`was`) follows the move (" + std::to_string(nw) + " words)");
    std::vector<uint8_t> mid2 = mid0;
    Say(mg::MigrateProfile(mid2.data(), pack2.data(), slot, nullptr) == 0 && mid2 == mid0,
        "BANKID: migration: without `was` the same profile is not touched (only the slot is an old value then)");
  }
  {  // S110 TRONSLOT: the migration of the two MOVIE words (profile +574 / +590 = record +2 / +18), and superstars.txt's `movie` / `movie_was`
    namespace mg = wwe2k14::superstars_migrate;
    auto put = [](std::vector<uint8_t>& v, size_t off, int val) { v[off] = uint8_t(val >> 8), v[off + 1] = uint8_t(val); };
    auto get = [](const std::vector<uint8_t>& v, size_t off) { return int(v[off] << 8 | v[off + 1]); };
    // the offsets, consistent with the bank words the shim already uses: record = profile +572, bank = record +4 / +20 (576 / 592), so the
    // movie words (record +2 / +18) are 574 / 590, two before each bank word; and the PRO FILE row has a 4-byte header, so its +578 / +594 are profile +574 / +590
    static_assert(mg::kMovieWords[0] + 2 == mg::kBankWords[0] && mg::kMovieWords[1] + 2 == mg::kBankWords[1], "movie words sit 2 bytes before the bank words");
    static_assert(mg::kMovieWords[0] == 578 - 4 && mg::kMovieWords[1] == 594 - 4, "PRO file +578 / +594 = profile +574 / +590");
    const int slot = 397, bank = 17;
    std::vector<uint8_t> pack(mg::kProfileBytes, 0), live(mg::kProfileBytes, 0);
    for (size_t off : {576u, 592u}) put(pack, off, bank), put(live, off, bank);  // (banks already current: only the movie words differ)
    put(pack, 574, 107), put(pack, 590, 107);
    for (size_t off : {574u, 590u}) put(live, off, slot);  // a save made before ENTMOV: movie = the slot
    std::string what;
    int n = mg::MigrateProfile(live.data(), pack.data(), slot, &what);
    Say(n == 2 && get(live, 574) == 107 && get(live, 590) == 107, "TRONSLOT: migration: a pre-ENTMOV save (movie words = the slot) gets the pack's movie words (" + std::to_string(n) + " words)");
    std::vector<uint8_t> after = live;
    Say(mg::MigrateProfile(live.data(), pack.data(), slot, nullptr) == 0 && live == after, "TRONSLOT: migration: a current profile is not touched");
    // (main session D2) ONLY the slot is an old movie value: a save holding any valid choice keeps it, also one equal to an earlier importer
    // value (here: built with 107, re-assigned to 845 in the importer, the player picked 107 again in the entrance editor: LEFT)
    std::vector<uint8_t> pack2 = pack;
    put(pack2, 574, 845), put(pack2, 590, 845);
    std::vector<uint8_t> l3 = live;
    Say(mg::MigrateProfile(l3.data(), pack2.data(), slot, nullptr) == 0 && l3 == live && get(l3, 590) == 107,
        "TRONSLOT: migration: a word equal to an earlier importer value (107; the pack now says 845) is LEFT: an in-game pick is never undone");
    std::vector<uint8_t> l4 = live;
    put(l4, 574, 300), put(l4, 590, 300);  // the player picked another movie in the game's own editor
    Say(mg::MigrateProfile(l4.data(), pack2.data(), slot, nullptr, {}) == 0 && get(l4, 574) == 300 && get(l4, 590) == 300, "TRONSLOT: migration: an in-game pick is left");
    std::vector<uint8_t> l6 = live;  // a pre-ENTMOV save on a re-assigned pack: the slot moves to the pack's current word
    for (size_t off : {574u, 590u}) put(l6, off, slot);
    Say(mg::MigrateProfile(l6.data(), pack2.data(), slot, nullptr) == 2 && get(l6, 574) == 845 && get(l6, 590) == 845, "TRONSLOT: migration: movie = slot follows the pack's current word (845)");
    std::vector<uint8_t> l5 = live, oldpack(mg::kProfileBytes, 0);  // a pack that still holds movie = slot (a pre-ENTMOV build): nothing to move to
    for (size_t off : {574u, 590u}) put(l5, off, slot), put(oldpack, off, slot);
    for (size_t off : {576u, 592u}) put(oldpack, off, bank);
    const std::vector<uint8_t> l5b = l5;
    Say(mg::MigrateProfile(l5.data(), oldpack.data(), slot, nullptr) == 0 && l5 == l5b, "TRONSLOT: migration: a pack that still holds movie = slot moves nothing");
    // superstars.txt: `movie` (informational) in any order after `bank` / `was`; an older line without it reads as before; a `movie_was N ...`
    // (written by this lane's first build only) is read past and ignored
    std::istringstream is("pack 6\nstar 397 music 100 bank 17 was 5 6 movie 845 movie_was 107 108\nstar 398 music 107 bank 18\nstar 399 music 107 bank 19 movie 107\n"
                          "star 410 music 100 bank 20 movie_was 9 movie 11 was 4\n");
    const wwe2k14::superstars_fit::Built b = wwe2k14::superstars_fit::ParseBuilt(is);
    Say(b.movie.count(397) && b.movie.at(397) == 845 && b.was.at(397) == std::vector<int>({5, 6}) && !b.movie.count(398) && b.bank.at(398) == 18 &&
            b.movie.at(399) == 107 && b.movie.at(410) == 11 && b.was.at(410) == std::vector<int>({4}),
        "TRONSLOT: superstars.txt: `movie M` parses in any order after `bank B [was ...]`, an old `movie_was` is read past; a line without them reads as before");
  }
}

// S110 TRONSLOT: every number of the custom tron band is free on the player's own files, in every space the note lists (TRONSLOT.md part 2). The
// band is written out HERE (not read from tron_slots_core): a second source for the same numbers, so a band edit in either place fails this.
static bool InBand(int n) { return (n >= 823 && n <= 843) || (n >= 845 && n <= 899) || (n >= 910 && n <= 949); }
static void TronBandUnits(const std::string& game, const std::string& dlc) {
  std::printf("custom tron band (S110 TRONSLOT):\n");
  int band = 0;
  for (int n = 0; n < 1000; ++n) band += InBand(n);
  Say(band == 116 && wwe2k14::tron_slots_core::BandIds().size() == 116 && wwe2k14::tron_slots_core::kBandCount == 116, "TRONSLOT: the band has 116 numbers, here and in tron_slots_core");
  bool same = true;
  for (int n = 0; n < 1000; ++n) same = same && InBand(n) == wwe2k14::tron_slots_core::IsSlotId(n);
  Say(same, "TRONSLOT: tron_slots_core::IsSlotId agrees with the band written out in this test, for 0..999");
  std::vector<std::pair<std::string, fs::path>> roots{{"the disc", fs::u8path(game)}};
  for (const auto& pk : wwe2k14::dlc::Scan(fs::u8path(dlc)))
    if (pk.status == wwe2k14::dlc::Status::kReady && !pk.package) roots.emplace_back("pack " + std::to_string(pk.pack_id), pk.root);
  auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
  };
  auto pacs = [&](const fs::path& root, const char* sub, const char* prefix) {  // every <prefix>*.pac of <root>\pac\<sub>
    std::vector<fs::path> v;
    std::error_code ec;
    const fs::path d = Find(root, std::string("pac") + (*sub ? std::string("/") + sub : std::string()));
    if (d.empty()) return v;
    for (const auto& de : fs::directory_iterator(d, ec)) {
      const std::string n = lower(de.path().filename().string());
      if (n.size() > 4 && n.rfind(prefix, 0) == 0 && n.compare(n.size() - 4, 4, ".pac") == 0) v.push_back(de.path());
    }
    return v;
  };
  int files = 0, tron_hits = 0, mv_hits = 0, lsd_hits = 0, t106_hits = 0, pro_hits = 0, movn_files = 0;
  int msc1_400 = 0, msc1_bits = 0, sheet_hits = 0, bit_hits = 0;  // (review) MSC1 leaves 400..403
  bool twin_ok = true;
  auto sheet_zero = [](const Bytes& m, int n) { return size_t(n) >= m.size() || m[size_t(n)] == 0; };  // sub_822C3168: byte n, 0 past the end
  auto bit_set = [](const Bytes& m, int n) { return size_t(n >> 3) < m.size() && (m[size_t(n >> 3)] >> (n & 7) & 1); };  // sub_822C31A8
  std::set<int> movn_in_band;
  std::string first_hit;
  auto hit = [&](int& counter, const std::string& what) {
    ++counter;
    if (first_hit.empty()) first_hit = what;
  };
  for (const auto& [label, root] : roots) {
    for (int n = 0; n < 1000; ++n)  // titantron files of the disc and of each pack
      if (InBand(n) && !Find(root, "movies/titantron/" + std::to_string(n) + ".bik2").empty()) hit(tron_hits, label + " has titantron\\" + std::to_string(n) + ".bik2");
    for (const fs::path& p : pacs(root, "evt", "evtmov")) {  // MVSW / MVSB (movie-word keyed) and MOVN / MOVL (model key + 500)
      io::Pac pc;
      std::string e;
      if (!io::ReadPac(UnxctdFile(p), pc, e)) continue;
      ++files;
      for (const io::Dir& d : pc.dirs)
        for (const io::Entry& en : d.ents) {
          const std::string nm = io::Trim(en.name);
          if (nm.size() < 2 || nm[0] != 'M' || nm.find_first_not_of("0123456789", 1) != std::string::npos) continue;
          const int n = std::atoi(nm.c_str() + 1);
          if (!InBand(n)) continue;
          if (d.tag == "MVSW" || d.tag == "MVSB") hit(mv_hits, label + " " + p.filename().string() + " " + d.tag + "/" + nm);
          else if (d.tag == "MOVN" || d.tag == "MOVL") movn_in_band.insert(n), ++movn_files;
        }
    }
    for (const fs::path& p : pacs(root, "", "dlc")) {  // LSD lists 0 (MOVIE) and 1 (MINI-TRON): the value column
      io::Pac pc;
      std::vector<io::Leaf> lv;
      if (!EnexLeaves(UnxctdFile(p), "LSD ", pc, lv)) continue;
      ++files;
      for (const io::Leaf& l : lv)
        if (l.id <= 1) {
          const Bytes m = io::Decode(l.raw);
          const size_t cnt = m.size() >= 2 ? io::L16(&m[0]) : 0;
          for (size_t r = 0; r < cnt && 2 + 8 * (r + 1) <= m.size(); ++r)
            if (InBand(int(int16_t(io::L16(&m[2 + 8 * r]))))) hit(lsd_hits, label + " " + p.filename().string() + " LSD list " + std::to_string(l.id) + " row " + std::to_string(r));
        }
    }
    for (const fs::path& p : pacs(root, "evt", "evtdata")) {  // table 106 column 6 (the id a movie id >= 600 is swapped to / matched on)
      std::vector<io::Leaf> inner;
      if (!DataLeaves(UnxctdFile(p), inner)) continue;
      ++files;
      for (const io::Leaf& l : inner)
        if (l.id == 106)
          for (const auto& rw : ParseRows(io::Decode(l.raw)))
            if (rw.size() > 7 && (InBand(int(rw[6])) || InBand(int(rw[7])))) hit(t106_hits, label + " " + p.filename().string() + " table 106 row " + std::to_string(rw[0]));
    }
    // (review) MSC1 (misc*.pac: the packs' DLC<n>/MSC1 and the disc's misc01_Start BATL/MSC1): leaf 400 = one byte per tron id, nonzero = a SHEET
    // tron (big screen key 804, the tron shim's sheet layout for a slot id); leaves 401..403 = the set 1..3 bits (wall / side panels / banner band):
    // every band number must read like a stock wrestler's tron without stage videos (byte nonzero, no bit). Twins: the same readers flag
    // event tron 550 (byte 0) and Kane's 107 (MVSW M107: set 2 bit).
    for (const fs::path& p : pacs(root, "", "misc")) {
      io::Pac pc;
      std::string e;
      if (!io::ReadPac(UnxctdFile(p), pc, e)) continue;
      for (const io::Dir& d : pc.dirs)
        for (const io::Entry& en : d.ents) {
          if (io::Trim(en.name) != "MSC1") continue;
          std::vector<io::Leaf> lv;
          if (!io::ReadPach(io::Decode(en.blob), lv)) continue;
          for (const io::Leaf& l : lv) {
            const Bytes m = io::Decode(l.raw);
            const std::string at = label + " " + p.filename().string() + " " + d.tag + "/MSC1 leaf " + std::to_string(l.id);
            if (l.id == 400) {
              ++msc1_400;
              twin_ok = twin_ok && sheet_zero(m, 550) && !sheet_zero(m, 107);
              for (int n = 0; n < 1000; ++n)
                if (InBand(n) && sheet_zero(m, n)) hit(sheet_hits, at + ": byte " + std::to_string(n) + " is 0 (not a sheet)");
            } else if (l.id >= 401 && l.id <= 403) {
              ++msc1_bits;
              if (l.id == 402) twin_ok = twin_ok && bit_set(m, 107) && !bit_set(m, 550);
              for (int n = 0; n < 1000; ++n)
                if (InBand(n) && bit_set(m, n)) hit(bit_hits, at + ": bit " + std::to_string(n) + " is set");
            }
          }
        }
    }
    for (const fs::path& p : pacs(root, "ch", "chetc")) {  // roster rows: no character id and no PRO movie / mini-tron word in the band
      io::Pac pc;
      std::vector<io::Leaf> pro;
      io::Entry* pe = nullptr;
      std::string er;
      if (!io::ReadPac(UnxctdFile(p), pc, er) ||!(pe = pc.Find("CHAR", "PRO ")) || !io::ReadPach(pe->blob, pro)) continue;
      ++files;
      for (const io::Leaf& l : pro) {
        if (InBand(int(l.id))) hit(pro_hits, label + " " + p.filename().string() + " PRO row id " + std::to_string(l.id));
        const Bytes r = io::Decode(l.raw);
        if (r.size() >= 604)
          for (size_t off : {578u, 586u, 594u, 602u})
            if (InBand(io::L16(&r[off]))) hit(pro_hits, label + " " + p.filename().string() + " PRO " + std::to_string(l.id) + " +" + std::to_string(off));
      }
    }
  }
  std::printf("  read %d pac file(s) on %d root(s) (the disc + the ready real packs)\n", files, int(roots.size()));
  const std::string where = first_hit.empty() ? std::string() : " (first: " + first_hit + ")";
  Say(files >= 8, "TRONSLOT: the readers found the files (" + std::to_string(files) + " pacs: evtmov / DLC LSD / evtdata / chEtc)");
  Say(tron_hits == 0, "TRONSLOT: no band number has a titantron file on the disc or in a pack" + where);
  Say(mv_hits == 0, "TRONSLOT: no band number is an MVSW / MVSB name (the side panel / wall movie ids)" + where);
  Say(lsd_hits == 0, "TRONSLOT: no band number is a value of LSD list 0 (MOVIE) or 1 (MINI-TRON)" + where);
  Say(t106_hits == 0, "TRONSLOT: no band number is in table 106 column 6 / 7" + where);
  Say(pro_hits == 0, "TRONSLOT: no band number is a character id or a PRO movie / mini-tron word" + where);
  Say(msc1_400 >= 1 && msc1_bits >= 3 && twin_ok, "TRONSLOT: MSC1 read (" + std::to_string(msc1_400) + " leaf 400, " + std::to_string(msc1_bits) +
                                                       " leaves 401..403; twins: 550 byte 0, 107 byte nonzero and set 2 bit, 550 no bit: " + (twin_ok ? "flagged" : "NOT flagged") + ")");
  Say(sheet_hits == 0, "TRONSLOT: every band number has a nonzero MSC1 leaf-400 byte (a sheet tron: key 804) in every MSC1, the disc's BATL one too" + where);
  Say(bit_hits == 0, "TRONSLOT: no band number has an MSC1 set 1..3 bit (wall / side panel / banner stage videos)" + where);
  std::string mn;
  for (int n : movn_in_band) mn += " " + std::to_string(n);
  std::printf("  info: MOVN / MOVL name plates keyed by model key + 500 whose name is in the band:%s (not a movie id: TRONSLOT.md part 2, last row)\n",
              mn.empty() ? " none" : mn.c_str());
}

// S110 SEAT-A: the library + frame cases (library_cases.h, library_cases2.h) and main
#include "library_cases.h"
#include "library_cases2.h"
#include "screen_text_cases.h"  // S110 SEAT-C: the screens' sentences + MigrationState + SavePack keys_out
#include "bind_cases.h"         // S110 SEAT3 review: the bind core (src\seat_bind_core.h) against the importer's own cards
#include "guest_library.h"      // S110 SEAT3-R: `--guests N`: a small library with N library characters, for the game's step 3 run

int main(int argc, char** argv) {
  std::string game, dlc, mods, source, ref, name = "Mr. Kennedy", short_name = "KENNEDY", stfs_dir, migrate_dir;
  int slot = 397, base = 100, ent = -1, count = 1, tron_slot = 0, tron_from = 0, guests = 0, residents = 3;
  bool self_only = false, render = false, fit = false, retire = false, library = false, bind = false;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--game") game = next();
    else if (a == "--dlc") dlc = next();
    else if (a == "--mods") mods = next();
    else if (a == "--source") source = next();
    else if (a == "--ref") ref = next();
    else if (a == "--slot") slot = std::atoi(next().c_str());
    else if (a == "--base") base = std::atoi(next().c_str());
    else if (a == "--entrance-from") ent = std::atoi(next().c_str());
    else if (a == "--tron-slot") tron_slot = std::atoi(next().c_str());  // S110 TRONSLOT: a custom tron number (823-843, 845-899, 910-949)
    else if (a == "--tron-from") tron_from = std::atoi(next().c_str());  // S110 TRONSLOT: a wrestler whose movie word he gets
    else if (a == "--name") name = next();
    else if (a == "--short") short_name = next();
    else if (a == "--selftest-only") self_only = true;
    else if (a == "--render") render = true;
    else if (a == "--count") count = std::max(1, std::atoi(next().c_str()));
    else if (a == "--fit-cases") fit = true;  // Session 101 (lane 199), on the library + frame
    else if (a == "--library-cases") library = true;  // S110 SEAT-A: the library against the reference pack, the 500-card library, migration
    else if (a == "--bind-cases") bind = true;  // S110 SEAT3 review: a card re-targeted by the bind core equals the importer's card for that seat
    else if (a == "--guests") guests = std::max(0, std::atoi(next().c_str()));  // S110 SEAT3-R: a library of --residents R + N library characters (guest_library.h)
    else if (a == "--residents") residents = std::max(0, std::atoi(next().c_str()));
    else if (a == "--stfs-dir") stfs_dir = next();
    else if (a == "--migrate-copy") migrate_dir = next();  // (review) a copy of a real <mods> folder holding an old pack: migrated in place
    else if (a == "--retire-cases") retire = true;  // S105 IMP (no game files)
  }
  if (!mods.empty()) ScreenTextCases(mods);  // S110 SEAT-C (no game files)
  if (retire) {
    RetireCasesLib(mods);
    std::printf("failures: %d\n", g_fail);
    return g_fail;
  }
  SelfTest(dlc);
  if (self_only) return g_fail;
  Env env;
  env.game = game, env.dlc = dlc, env.source = source, env.mods_root = mods, env.ref = ref;
  env.gf.SetRoot(fs::u8path(game));
  if (!migrate_dir.empty()) {  // (S110 SEAT-A review) migrate a COPY of a real old pack (--migrate-copy MODS_COPY): LoadPack + BuildPack, the log
    importer::PackState st;
    std::string e;
    const bool rd = importer::LoadPack(migrate_dir, st, e);
    Say(rd, "LoadPack of the copy: " + std::to_string(st.stars.size()) + " character(s)" + (e.empty() ? "" : ": " + e));
    const bool ok = rd && Build(env, migrate_dir, st, dlc, "", &e);
    for (const auto& l : importer::LastBuildLog()) std::printf("  | %s\n", l.c_str());
    Say(ok, "BuildPack (the migration) of the copy" + (e.empty() ? std::string() : ": " + e));
    std::printf("failures: %d\n", g_fail);
    return g_fail;
  }
  if (guests > 0) {
    GuestLibrary(env, guests, residents);
    std::printf("failures: %d\n", g_fail);
    return g_fail;
  }
  if (bind) {
    BindCases(env);
    std::printf("failures: %d\n", g_fail);
    return g_fail;
  }
  if (library) {
    LibraryCases(env);
    std::printf("failures: %d\n", g_fail);
    return g_fail;
  }
  if (fit) {
    FitCasesLib(env, stfs_dir);
    BankIdUnits(game, dlc);  // (after the builds: the free list is the last build's)
    TronBandUnits(game, dlc);
    std::printf("failures: %d\n", g_fail);
    return g_fail;
  }
  std::printf("sources in %s:\n", source.c_str());
  for (const auto& s : importer::InspectSources(source)) {
    std::printf("  '%s' ok=%d err='%s' claimed=%d attires=%d bip=%d suggested='%s'\n", s.entry_name.c_str(), s.ok, s.err.c_str(), s.claimed_id, s.attires, s.bip_bones, s.suggested_name.c_str());
    for (const auto& w : s.warnings) std::printf("    warning: %s\n", w.c_str());
  }
  BuildFlow(env, name, short_name, slot, base, ent, count, tron_from, tron_slot, render);
  if (tron_slot != 0) {  // S110 TRONSLOT: the test pack's library line (the video itself is the main session's: <mods>\titantron\<N>.<ext>)
    const bool w = wwe2k14::tron_slots_core::WriteSlots(fs::u8path(mods) / "titantron", {{tron_slot, "Test Tron", false, {}}});
    Say(w, "TRONSLOT: <mods>\\titantron\\custom.txt holds `" + std::to_string(tron_slot) + "<TAB>Test Tron`");
  }
  std::printf("failures: %d\n", g_fail);
  return g_fail;
}


