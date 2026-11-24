// S110 SEAT-A: the host cases of the library + frame importer (included at the end of test_core.cpp; docs\research\s110\SEAT1.md section 6).
//
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source PAC --ref REF_MODS --library-cases
//       LibraryCases: the library instance of a character against a REFERENCE pack built by today's (pre-library) importer from the same two
//       stars (REF_MODS = `importer_ref.exe --mods REF_MODS --source PAC --count 2 --name "Ref Star" --short REF --base 107 --entrance-from 110`):
//       every table row and pac entry byte-equal, the frame untouched by add / remove / edit, a 500-card library, migration of an old pack.
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source PAC --fit-cases     the "why imports fail" cases on the library + frame
//   test_core.exe --mods OUT_DIR --retire-cases                                      the retired seats (no game files)
//
// Nothing is written but under --mods; the reference is only read. Every equality is first run against planted twins (a copy with one byte
// changed) and must say FAIL for them.
#pragma once
#include <chrono>

#include "../../src/importer/enex_rows.h"
#include "../../src/importer/png.h"
#include "../../src/seat_bind_core.h"
#include "../../src/seat_card.h"

namespace sc = wwe2k14::seat_card;
namespace enex_rows = importer::enex_rows;
using Clock = std::chrono::steady_clock;
static double Secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }
static void Twin(const std::string& name, bool caught) {
  std::printf("  %s twin \"%s\"\n", caught ? "caught" : "NOT CAUGHT", name.c_str());
  if (!caught) ++g_fail;
}

// ---------------------------------------------------------------- small helpers
static uint64_t Fnv64(uint64_t h, const void* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
  return h;
}
// A digest of every file under `dir` (relative path, size, bytes), sorted: equal digests = no byte changed (and none added or removed).
static uint64_t HashTree(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return 0;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.is_regular_file(ec)) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  uint64_t h = 1469598103934665603ull;
  for (const fs::path& f : files) {
    const std::string rel = fs::relative(f, dir, ec).generic_string();
    const Bytes b = Read(f);
    const uint64_t n = b.size();
    h = Fnv64(h, rel.data(), rel.size());
    h = Fnv64(h, &n, sizeof n);
    h = Fnv64(h, b.data(), b.size());
  }
  return h;
}
static std::string LowerS(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
static std::string Slash(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

struct Env {
  std::string game, dlc, source, mods_root, ref;
  launcher::GameFiles gf;
};
// A fresh mods folder with the source pac extracted into it; `n` stars (the first free seats unless `slots`), base 107, entrance 110.
static std::string FreshMods(const Env& env, const std::string& name, std::string* pac_out) {
  const fs::path m = fs::u8path(env.mods_root) / name;
  std::error_code ec;
  fs::remove_all(m, ec);
  fs::create_directories(m, ec);
  const importer::SourceInfo one = importer::InspectSource(env.source);
  std::string e;
  const std::string mods = Slash(m);
  const std::string pac = importer::ExtractSource(env.source, one.entry_name, mods, &e);
  if (pac_out) *pac_out = pac;
  return mods;
}
static importer::Superstar MakeStar(const std::string& pac, int slot, const std::string& name, int base = 107, int ent = 110) {
  importer::Superstar s;
  s.slot = slot, s.name = name, s.short_name = name.substr(0, std::min<size_t>(name.size(), 8)), s.base_id = base, s.entrance_from = ent, s.source_pac = pac;
  return s;
}
static bool Build(const Env& env, const std::string& mods, const importer::PackState& st, const std::string& dlc, const std::string& off, std::string* err_out = nullptr) {
  std::string e;
  const bool ok = importer::BuildPack(mods, env.gf, dlc, st, nullptr, e, off);
  if (!ok) {
    for (const auto& l : importer::LastBuildLog()) std::printf("  | %s\n", l.c_str());
    std::printf("  build error: %s\n", e.c_str());
  }
  if (err_out) *err_out = e;
  return ok;
}
static fs::path Root(const std::string& mods) { return fs::u8path(mods) / "superstars"; }
static int PackIdOf(const std::string& mods) { return wwe2k14::superstars_fit::ReadBuilt(Root(mods) / "frame" / "superstars.txt").pack_id; }
static std::string Pp(int id) {
  char b[8];
  std::snprintf(b, sizeof b, "%02d", id);
  return b;
}
static void CopyTree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::remove_all(to, ec);
  fs::create_directories(to.parent_path(), ec);
  fs::copy(from, to, fs::copy_options::recursive, ec);
}

// ---------------------------------------------------------------- one character's library instance, read back
struct Lib {
  sc::Card card;
  sc::SeatRows rows;
  io::Pac pac;
  bool ok = false;
  std::string err;
};
static Lib ReadLib(const std::string& mods, const std::string& key) {
  Lib l;
  const fs::path dir = Root(mods) / "library" / key;
  if (!sc::ReadCard(Slash(dir / "card.txt"), l.card, l.err) || !l.card.inst.valid) return l.err = l.err.empty() ? "no inst" : l.err, l;
  if (!sc::ReadRows(Slash(dir / "rows.bin"), l.rows, l.err)) return l;
  // (S110 LIBSIZE-2) ONE seat-free file per card: pac\ch\ch497.pac, the same name for every seat and every frame
  const Bytes d = Read(dir / "pac" / "ch" / ("ch" + std::to_string(sc::kRefSeat) + ".pac"));
  if (!io::ReadPac(d, l.pac, l.err)) return l;
  l.ok = true;
  return l;
}
static const io::Dir* DirOf(const io::Pac& p, const std::string& tag) {
  for (const io::Dir& d : p.dirs)
    if (d.tag == tag) return &d;
  return nullptr;
}
static bool BlobOf(const io::Pac& p, const std::string& tag, const std::string& name, Bytes& out) {
  const io::Dir* d = DirOf(p, tag);
  if (!d) return false;
  for (const io::Entry& e : d->ents)
    if (e.name == name) return out = io::Blob(*d, e), true;
  return false;
}

// ---------------------------------------------------------------- the reference pack (built by the pre-library importer)
// (S110 SEAT-A review) text `id` of a string pac's name tables, read here without the importer's code (the first ZLIB table that holds it)
static bool NameText(io::Pac& str, uint32_t id, std::string& out) {
  io::Dir* sdb = str.FindDir("SDB ");
  if (!sdb) return false;
  for (io::Entry& x : sdb->ents) {
    std::vector<io::Leaf> lv;
    if (!io::ReadPach(io::Blob(*sdb, x), lv)) continue;
    for (const io::Leaf& l : lv) {
      if (l.raw.size() < 18 || std::memcmp(l.raw.data(), "ZLIB", 4) != 0) continue;
      const Bytes t = io::Decode(l.raw);
      const uint32_t cnt = t.size() >= 8 ? io::L32(&t[4]) : 0;
      for (uint32_t i = 0; i < cnt && 8 + 12ull * (i + 1) <= t.size(); ++i) {
        const uint8_t* r = &t[8 + 12 * size_t(i)];
        if (io::L32(r + 8) != id || uint64_t(io::L32(r)) + io::L32(r + 4) > t.size()) continue;
        out.assign(t.begin() + ptrdiff_t(io::L32(r)), t.begin() + ptrdiff_t(io::L32(r) + io::L32(r + 4)));
        while (!out.empty() && out.back() == 0) out.pop_back();
        return true;
      }
    }
  }
  return false;
}

struct Ref {
  io::Pac etc, dlc, ent, win, gm, prt, hd, sd, mov, str;
  bool has_mov = false;
  wwe2k14::superstars_fit::Built built;
  std::map<uint32_t, Bytes> dat, pro, dsbd;
  enex_rows::Table bin, lsd;
};
static bool LoadRef(const std::string& ref_mods, Ref& r, std::string& err) {
  const fs::path p6 = Root(ref_mods) / "pack6" / "pac";
  auto rd = [&](const fs::path& f, io::Pac& p) {
    std::string e;
    return io::ReadPac(Read(f), p, e);
  };
  if (!rd(p6 / "ch" / "chEtc_dlc06.pac", r.etc) || !rd(p6 / "DLC_dlc06.pac", r.dlc) || !rd(p6 / "evt" / "Entrance_dlc06.pac", r.ent) ||
      !rd(p6 / "evt" / "Winning_dlc06.pac", r.win) || !rd(p6 / "gm_dlc06.pac", r.gm) || !rd(p6 / "prtcls_dlc06.pac", r.prt) || !rd(p6 / "DLC_HD_dlc06.pac", r.hd) ||
      !rd(p6 / "DLC_SD_dlc06.pac", r.sd))
    return err = "the reference pack is missing a pac (" + ref_mods + ")", false;
  r.has_mov = rd(p6 / "evt" / "evtmov_dlc06.pac", r.mov);
  if (!rd(p6 / "string_dlc06.pac", r.str)) return err = "the reference pack has no string pac (" + ref_mods + ")", false;
  r.built = wwe2k14::superstars_fit::ReadBuilt(Root(ref_mods) / "superstars.txt");
  std::vector<io::Leaf> a, b, c;
  io::Entry *dt = r.etc.Find("CHAR", "DAT "), *ds = r.etc.Find("CHAR", "DSBD"), *pr = r.etc.Find("CHAR", "PRO ");
  if (!dt || !ds || !pr || !io::ReadPach(dt->blob, a) || !io::ReadPach(ds->blob, b) || !io::ReadPach(pr->blob, c)) return err = "the reference chEtc is not as expected", false;
  for (auto& l : a) r.dat[l.id] = io::Decode(l.raw);
  for (auto& l : b) r.dsbd[l.id] = l.raw;
  for (auto& l : c) r.pro[l.id] = io::Decode(l.raw);
  if (!r.bin.Open(r.dlc, "BIN ") || !r.lsd.Open(r.dlc, "LSD ")) return err = "the reference DLC pac has no ENEX tables", false;
  return true;
}

// (S110 LIBSIZE-2) The reference pack is the OLD per-seat build; a card is now ONE seat-free file for 497 / 9997. SeatForm is the card as the game binds it to `slot`:
// his rows re-targeted (RetargetCard, the reference's bank and rank), his model's names moved to the seat, his EVP blobs re-targeted (RetargetEvBlob: the blob and the
// name the seat's EV file holds). What the old build copied from the game (EVZ GMEV PCE6 MOVN, the base's renders) is NOT in the card: its ALIA rows are checked
// (tools\importer\bind_cases.h resolves them to the bytes).
static Lib SeatForm(const Lib& m, int slot, int bank, int rank, int pack_id) {
  Lib o = m;
  std::string err;
  sc::SeatRows r;
  if (!wwe2k14::seat_bind::RetargetCard(m.rows, sc::kRefSeat, sc::kRefBank, slot, bank, pack_id, rank, r, err)) return o.ok = false, o.err = "RetargetCard: " + err, o;
  o.rows = r;
  for (io::Dir& d : o.pac.dirs)
    for (io::Entry& e : d.ents) {
      if (d.tag == "EMD " && e.name.size() == 8) wwe2k14::seat_bind::RetargetName(&e.name[0], 8, sc::kRefSeat, slot);
      else if (d.tag == "EVP ") {
        const std::string n = io::Trim(e.name);
        e.name = io::Pad(std::to_string(wwe2k14::seat_bind::MapBankNumber(std::atoi(n.c_str()), sc::kRefSeat, slot, sc::kRefBank, bank)), 8);
        const Bytes g = wwe2k14::seat_bind::RetargetEvBlob(e.blob, sc::kRefSeat, sc::kRefBank, slot, bank, err);
        if (g.empty()) return o.ok = false, o.err = "RetargetEvBlob: " + err, o;
        e.blob = g;
      }
    }
  return o;
}
static bool HasAlias(const sc::SeatRows& r, const std::string& tag, const std::string& game, const std::string& ours) {
  for (const sc::Alias& a : r.alias)
    if (a.tag == tag && a.game == game && a.ours == ours) return true;
  return false;
}

// Mine (made once, bound to `slot`) against the reference for the star on `slot` (his bank = the reference's superstars.txt). Returns what differs ("" = nothing).
static std::vector<std::string> Compare(const Lib& canon, const Ref& r, int slot, int ent, const std::function<void(Lib&)>& post = nullptr) {
  std::vector<std::string> bad;
  auto say = [&](const std::string& s) { bad.push_back(s); };
  if (!r.built.bank.count(slot) || !r.dat.count(slot)) return {"the reference has no bank / row for slot " + std::to_string(slot)};
  const int bank = r.built.bank.at(slot);
  Lib m = SeatForm(canon, slot, bank, io::L16(&r.dat.at(slot)[4 + 2]), r.built.pack_id);
  if (!m.ok) return {m.err};
  if (post) post(m);  // (a planted twin changes the card AS BOUND: the words the relocation list writes would undo a change made before)
  const sc::SeatRows& w = m.rows;
  auto part = [&](int k) { return 10000 + 1000 * k + bank; };
  // the three roster rows
  if (Bytes(w.dat.begin(), w.dat.end()) != r.dat.at(slot)) say("DAT row");
  if (!r.dsbd.count(slot) || Bytes(w.dsbd.begin(), w.dsbd.end()) != r.dsbd.at(slot)) say("DSBD row");
  if (!r.pro.count(slot) || Bytes(w.pro.begin(), w.pro.end()) != r.pro.at(slot)) say("PRO row");
  // (the model: CompareModels below, from the reference's ch<slot> pac)
  // entrance banks: names and bytes, the reference's Entrance pac (the EVP only: the EVZ is the game's own, an alias row below)
  std::set<std::string> want_names;
  for (int k = -1; k < 5; ++k) want_names.insert(io::Pad(std::to_string(k < 0 ? bank : part(k)), 8));
  const io::Dir* mp = DirOf(m.pac, "EVP ");
  if (!mp || DirOf(m.pac, "EVZ ")) say("EVP directory (or an EVZ copy that must not be there)");
  else {
    std::set<std::string> mine_names;
    for (size_t i = 0; i < mp->ents.size(); ++i) {
      const std::string& n = mp->ents[i].name;
      Bytes rv;
      const bool victory = n == io::Pad(std::to_string(6000 + slot), 8);
      const io::Pac& src_p = victory ? r.win : r.ent;
      if (!victory) mine_names.insert(n);
      if (!BlobOf(src_p, "EVP ", n, rv)) say("EVP " + io::Trim(n) + " is not in the reference");
      else if (io::Blob(*mp, mp->ents[i]) != rv) say("EVP " + io::Trim(n) + " bytes");
      // the card's alias row: the game's own EVZ of the entrance wrestler's bank, named for the reference identity
      const int num = std::atoi(io::Trim(n).c_str());
      int canon_n = num == bank ? sc::kRefBank : victory ? 6000 + sc::kRefSeat : -1;
      for (int k = 0; k < 5; ++k)
        if (num == part(k)) canon_n = 10000 + 1000 * k + sc::kRefBank;
      bool alias = false;
      for (const sc::Alias& a : canon.rows.alias) alias = alias || (a.tag == "EVZ " && io::Trim(a.ours) == std::to_string(canon_n));
      if (!alias) say("EVZ alias row of bank " + io::Trim(n));
      Bytes rz;
      if (!BlobOf(src_p, "EVZ ", n, rz)) say("EVZ " + io::Trim(n) + " is not in the reference");
    }
    // (the reference holds the variant banks only where the entrance wrestler has them: every name mine holds is a name of his)
    for (const std::string& n : mine_names)
      if (!want_names.count(n)) say("EVP " + io::Trim(n) + " is nobody's bank");
    for (const std::string& n : want_names) {
      Bytes rv;
      if (BlobOf(r.ent, "EVP ", n, rv) && !mine_names.count(n)) say("EVP " + io::Trim(n) + " is in the reference but not in mine");
    }
    Bytes rv;
    const std::string vn = io::Pad(std::to_string(6000 + slot), 8);
    const bool in_ref = BlobOf(r.win, "EVP ", vn, rv);
    bool in_mine = false;
    for (const io::Entry& e : mp->ents) in_mine = in_mine || e.name == vn;
    if (in_ref != in_mine) say("the victory bank is in one and not the other");
  }
  // effects, plate: the game's own entries, an alias row each (no copy in his file)
  for (const char* tag : {"GMEV", "PCE6"}) {
    char nm[8], cn[8];
    std::snprintf(nm, sizeof nm, "%04d", bank), std::snprintf(cn, sizeof cn, "%04d", sc::kRefBank);
    Bytes rb;
    const bool in_ref = BlobOf(tag[0] == 'G' ? r.gm : r.prt, tag, nm, rb);
    bool alias = false;
    for (const sc::Alias& a : canon.rows.alias) alias = alias || (a.tag == tag && a.ours == cn && a.game.size() == 4);
    if (DirOf(m.pac, tag)) say(std::string(tag) + " copied into his file");
    if (in_ref && !alias) say(std::string(tag) + " alias row");
  }
  {
    char nm[8], cn[8];
    std::snprintf(nm, sizeof nm, "M%03d", 500 + slot), std::snprintf(cn, sizeof cn, "M%03d", 500 + sc::kRefSeat);
    Bytes rb;
    const bool in_ref = r.has_mov && BlobOf(r.mov, "MOVN", nm, rb);
    bool alias = false;
    for (const sc::Alias& a : canon.rows.alias) alias = alias || (a.tag == "MOVN" && a.ours == cn);
    if (DirOf(m.pac, "MOVN")) say("MOVN copied into his file");
    if (in_ref && !alias) say("MOVN alias row");
  }
  // renders: the UI gave none here, so the base's own are aliased (SSFx; one row serves the HD and the SD pac) and his file holds none
  for (int k = 0; k < 3; ++k) {
    const std::string t1 = std::string("SSF") + char('A' + k), t2 = std::string("SDF") + char('A' + k);
    bool alias = false;
    for (const sc::Alias& a : canon.rows.alias) alias = alias || (a.tag == t1 && a.ours == "0497");
    if (DirOf(m.pac, t1) || DirOf(m.pac, t2) || !alias) say(t1 + " render: the base's is an alias row, not a copy");
  }
  // ENEX rows
  {
    const Bytes& l5 = const_cast<enex_rows::Table&>(r.bin).data[5];
    Bytes rr;
    for (size_t i = 0; i < io::L16(&l5[0]); ++i)
      if (io::L16(&l5[16 + 16 * i]) == uint32_t(slot)) rr.assign(l5.begin() + ptrdiff_t(16 + 16 * i), l5.begin() + ptrdiff_t(32 + 16 * i));
    if (rr != Bytes(w.bin5.begin(), w.bin5.end())) say("BIN leaf 5 template row");
    for (int k = 0; k < 5; ++k) {
      const Bytes& lk = const_cast<enex_rows::Table&>(r.bin).data[uint32_t(k)];
      Bytes row;
      for (size_t i = 0; i < io::L16(&lk[0]); ++i)
        if (int16_t(io::L16(&lk[16 + 20 * i + 8])) == part(k)) row.assign(lk.begin() + ptrdiff_t(16 + 20 * i), lk.begin() + ptrdiff_t(36 + 20 * i));
      const sc::SeatRows::Binp* mine = nullptr;
      for (const auto& b : w.binp)
        if (b.k == k) mine = &b;
      if (row.empty() != (mine == nullptr)) say("BIN leaf " + std::to_string(k) + " part row presence");
      else if (mine) {
        Bytes a(mine->row.begin(), mine->row.end());
        a[0] = a[1] = row[0] = row[1] = 0;  // (the row number: the game sets the real index; here the first star's number and the second's differ)
        if (a != row) say("BIN leaf " + std::to_string(k) + " part row");
      }
    }
    static const uint32_t lists[] = {2, 3, 17, 16, 8, 9, 10, 11};
    for (uint32_t L : lists) {
      const Bytes& m8 = const_cast<enex_rows::Table&>(r.lsd).data[L];
      int want = -1;
      if (L == 2) want = slot;
      else if (L == 3 || L == 17) want = bank;
      else want = part(L == 16 ? 0 : int(L) - 7);  // 16 -> part 0, 8..11 -> parts 1..4
      Bytes row;
      for (size_t i = 0; i < io::L16(&m8[0]); ++i)
        if (int16_t(io::L16(&m8[2 + 8 * i])) == want) row.assign(m8.begin() + ptrdiff_t(2 + 8 * i), m8.begin() + ptrdiff_t(10 + 8 * i));
      const sc::SeatRows::Lsd* mine = nullptr;
      for (const auto& x : w.lsd)
        if (x.list == L) mine = &x;
      if (row.empty() != (mine == nullptr)) say("LSD list " + std::to_string(L) + " row presence");
      else if (mine && Bytes(mine->row.begin(), mine->row.end()) != row) say("LSD list " + std::to_string(L) + " row");
    }
  }
  // T102 = his entrance wrestler's bank word, the names
  if (!w.has_t102 || !r.pro.count(uint32_t(ent)) || w.t102_src_bank != io::L16(&r.pro.at(uint32_t(ent))[580])) say("T102 (his entrance wrestler's bank)");
  // (S110 SEAT-A review) the three names against the reference's string pac (texts 60000 / 62000 / 64000 + slot)
  for (int k = 0; k < 3; ++k) {
    std::string t;
    if (!NameText(const_cast<io::Pac&>(r.str), uint32_t(60000 + 2000 * k + slot), t) || t != w.names[k]) say("NAME " + std::to_string(60000 + 2000 * k + slot));
  }
  return bad;
}
// The reference's model pac of `slot` against mine (EMD names, aliases and bytes; his file names the reference id 497, the seat's names follow).
static std::vector<std::string> CompareModels(const Lib& m0, const std::string& ref_mods, int slot, const std::function<void(Lib&)>& post = nullptr) {
  std::vector<std::string> bad;
  Lib m = SeatForm(m0, slot, 100, 1, 6);  // (only the model's names are moved here: the bank / rank are not read)
  if (post) post(m);
  io::Pac rp;
  std::string e;
  if (!io::ReadPac(Read(Root(ref_mods) / "pack6" / "pac" / "ch" / ("ch" + std::to_string(slot) + "_dlc06.pac")), rp, e)) return {"the reference model pac cannot be read"};
  const io::Dir *a = DirOf(m.pac, "EMD "), *b = DirOf(rp, "EMD ");
  if (!a || !b || a->ents.size() != b->ents.size()) return {"EMD entry count"};
  for (size_t i = 0; i < a->ents.size(); ++i) {
    if (a->ents[i].name != b->ents[i].name || a->ents[i].alias != b->ents[i].alias) bad.push_back("EMD entry " + std::to_string(i) + " name / alias");
    else if (io::Blob(*a, a->ents[i]) != io::Blob(*b, b->ents[i])) bad.push_back("EMD entry " + b->ents[i].name + " bytes");
  }
  return bad;
}

// ---------------------------------------------------------------- the cases

static void LibraryCases(const Env& env) {
  std::printf("library + frame (S110 SEAT-A):\n");
  const fs::path refp = Root(env.ref) / "pack6" / "pac";
  Ref ref;
  std::string why;
  if (!Say(LoadRef(env.ref, ref, why), "the reference pack reads (" + (env.ref.empty() ? std::string("no --ref given") : env.ref) + ")" + (why.empty() ? "" : ": " + why))) {
    // (S110 SEAT-C review) what --ref wants and how to make one, instead of a bare FAIL
    std::printf("    --ref wants a <mods> folder holding a pack built by the PRE-library importer (superstars\\pack6\\, superstars.txt) from the same\n"
                "    source pac. To make one: build tools\\importer\\test_core.cpp + src\\importer\\importer_core.cpp of commit 77add68d4^ (the parent of\n"
                "    SEAT-A's library commit; `git archive 77add68d4^ src tools/importer tools/env.ps1` into a scratch folder, then the clang++ line\n"
                "    of tools\\importer\\build_test.ps1) as importer_ref.exe, and run:\n"
                "      importer_ref.exe --game GAME --dlc DLC --mods REF_MODS --source PAC --count 2 --name \"Ref Star\" --short REF --base 107 --entrance-from 110\n");
    return;
  }
  // ---- 1. the same two stars through the library
  std::string pac;
  const std::string mods = FreshMods(env, "lib2", &pac);
  importer::PackState st;
  st.stars.push_back(MakeStar(pac, 397, "Ref Star"));
  st.stars.push_back(MakeStar(pac, 398, "Ref Star 2"));
  st.stars[0].short_name = "REF", st.stars[1].short_name = "REF";
  const auto t0 = Clock::now();
  const bool built = Build(env, mods, st, env.dlc, "");
  const double full2 = Secs(t0);
  if (!Say(built, "BuildPack: 2 characters into the library (" + std::to_string(full2).substr(0, 5) + " s)")) return;
  importer::PackState back;
  std::string e;
  Say(importer::LoadPack(mods, back, e) && back.stars.size() == 2 && !back.needs_build && back.stars[0].slot == 397 && back.stars[1].slot == 398 &&
          sc::ValidKey(back.stars[0].key) && back.stars[0].key != back.stars[1].key && back.stars[0].name == "Ref Star",
      "LoadPack: 2 cards, keys " + (back.stars.size() == 2 ? back.stars[0].key + " / " + back.stars[1].key : std::string("?")) + ", nothing left to build");
  const std::string k0 = back.stars.size() == 2 ? back.stars[0].key : "", k1 = back.stars.size() == 2 ? back.stars[1].key : "";
  Lib l0 = ReadLib(mods, k0), l1 = ReadLib(mods, k1);
  if (!Say(l0.ok && l1.ok, "both folders read back (card + rows.bin + pac)" + (l0.ok && l1.ok ? "" : ": " + l0.err + " / " + l1.err))) return;
  {  // the folder of one character as written: file names and sizes
    std::string lst;
    std::error_code ec;
    for (const auto& f : fs::recursive_directory_iterator(Root(mods) / "library" / k0, ec))
      if (f.is_regular_file(ec)) lst += " " + fs::relative(f.path(), Root(mods) / "library" / k0, ec).generic_string() + "=" + std::to_string(f.file_size(ec));
    std::printf("  info: library\\%s:%s\n", k0.c_str(), lst.c_str());
  }
  struct One { Lib* l; int slot; };
  for (One o : {One{&l0, 397}, One{&l1, 398}}) {
    const std::string tag = "slot " + std::to_string(o.slot);
    std::vector<std::string> bad = Compare(*o.l, ref, o.slot, 110);
    for (const std::string& b : CompareModels(*o.l, env.ref, o.slot)) bad.push_back(b);
    std::string all;
    for (const std::string& b : bad) all += " [" + b + "]";
    Say(bad.empty(), tag + ": DAT / DSBD / PRO rows, EMD, EVP / EVZ (+ victory), GMEV / PCE6 / MOVN, SSFx / SDFx, BIN / LSD rows, T102 all equal the reference's" + all);
  }
  {  // 2-kind EPAC: the library pac holds kind-0 and kind-1 directories and survives a write / read
    std::set<int> kinds;
    for (const io::Dir& d : l0.pac.dirs) kinds.insert(d.kind);
    io::Pac copy = l0.pac, back2;
    const Bytes bytes = io::WritePac(copy);
    std::string e2;
    bool same = io::ReadPac(bytes, back2, e2) && back2.dirs.size() == l0.pac.dirs.size();
    for (size_t i = 0; same && i < back2.dirs.size(); ++i) {
      same = back2.dirs[i].tag == l0.pac.dirs[i].tag && back2.dirs[i].kind == l0.pac.dirs[i].kind && back2.dirs[i].ents.size() == l0.pac.dirs[i].ents.size();
      for (size_t j = 0; same && j < back2.dirs[i].ents.size(); ++j)
        same = back2.dirs[i].ents[j].name == l0.pac.dirs[i].ents[j].name && io::Blob(back2.dirs[i], back2.dirs[i].ents[j]) == io::Blob(l0.pac.dirs[i], l0.pac.dirs[i].ents[j]);
    }
    Say(kinds.count(1) && same, "his one file (" + std::to_string(l0.pac.dirs.size()) + " dirs: EMD, EVP) round-trips through io::ReadPac");
    io::Pac mixed;
    io::Dir a, b;
    a.tag = "EMD ", a.kind = 1, b.tag = "SSFA", b.kind = 0;
    io::Entry x, y;
    x.name = "00039702", x.blob = Bytes(300, 7), y.name = "0397", y.blob = Bytes(40, 9);
    a.ents.push_back(x), b.ents.push_back(y);
    mixed.dirs = {a, b};
    io::Pac mb;
    const Bytes mw = io::WritePac(mixed);
    Say(io::ReadPac(mw, mb, e2) && mb.dirs.size() == 2 && mb.dirs[0].kind == 1 && mb.dirs[1].kind == 0 && mb.dirs[1].ents[0].name == "0397" && mb.dirs[0].ents[0].name == "00039702",
        "a hand-made 2-kind EPAC (8-char and 4-char names) round-trips");
  }
  // ---- planted twins: every comparison above must fail for a copy with one change
  {
    auto caught = [&](const std::string& name, const std::function<void(Lib&)>& mutate) {  // the card as the game binds it to 397, one change
      Twin(name, !Compare(l0, ref, 397, 110, mutate).empty() || !CompareModels(l0, env.ref, 397, mutate).empty());
    };
    auto caught_card = [&](const std::string& name, const std::function<void(Lib&)>& mutate) {  // the card as the importer wrote it (alias rows), one change
      Lib t = l0;
      mutate(t);
      Twin(name, !Compare(t, ref, 397, 110).empty());
    };
    auto first_blob = [](io::Pac& p, const char* tag) -> Bytes* {
      for (io::Dir& d : p.dirs)
        if (d.tag == tag && !d.ents.empty()) return &d.ents[0].blob;
      return nullptr;
    };
    caught("DAT: pack code +17 changed", [](Lib& t) { t.rows.dat[4 + 17] ^= 1; });
    caught("DAT: rank changed", [](Lib& t) { t.rows.dat[4 + 2] ^= 1; });
    caught("DSBD: an attire id changed", [](Lib& t) { t.rows.dsbd[0] ^= 1; });
    caught("PRO: the bank word +580 changed", [](Lib& t) { t.rows.pro[580] ^= 1; });
    caught("PRO: the movie word +578 changed", [](Lib& t) { t.rows.pro[578] ^= 1; });
    caught("EMD: a model byte changed", [&](Lib& t) { for (io::Dir& d : t.pac.dirs) if (d.tag == "EMD ") for (io::Entry& x : d.ents) if (x.alias < 0 && !x.blob.empty()) { x.blob[x.blob.size() / 2] ^= 1; return; } });
    caught("EMD: an attire alias dropped", [](Lib& t) { for (io::Dir& d : t.pac.dirs) if (d.tag == "EMD ") d.ents.pop_back(); });
    caught("EVP: an entrance byte changed", [&](Lib& t) { if (Bytes* b = first_blob(t.pac, "EVP ")) (*b)[b->size() / 2] ^= 1; });
    caught_card("EVZ: the alias rows dropped", [](Lib& t) { t.rows.alias.erase(std::remove_if(t.rows.alias.begin(), t.rows.alias.end(), [](const sc::Alias& a) { return a.tag == "EVZ "; }), t.rows.alias.end()); });
    caught("EVZ: a copy of the game's motions in his file", [](Lib& t) { io::Dir d; d.tag = "EVZ ", d.kind = 1; t.pac.dirs.push_back(d); });
    caught("EVP: the victory bank dropped", [](Lib& t) { for (io::Dir& d : t.pac.dirs) if ((d.tag == "EVP " || d.tag == "EVZ ") && !d.ents.empty()) d.ents.pop_back(); });
    caught_card("GMEV: the alias row dropped", [](Lib& t) { t.rows.alias.erase(std::remove_if(t.rows.alias.begin(), t.rows.alias.end(), [](const sc::Alias& a) { return a.tag == "GMEV"; }), t.rows.alias.end()); });
    caught_card("PCE6: the alias row dropped", [](Lib& t) { t.rows.alias.erase(std::remove_if(t.rows.alias.begin(), t.rows.alias.end(), [](const sc::Alias& a) { return a.tag == "PCE6"; }), t.rows.alias.end()); });
    caught_card("SSFA: the base's render alias dropped", [](Lib& t) { t.rows.alias.erase(std::remove_if(t.rows.alias.begin(), t.rows.alias.end(), [](const sc::Alias& a) { return a.tag == "SSFA"; }), t.rows.alias.end()); });
    caught("SDFC: a render copied into his file", [](Lib& t) { io::Dir d; d.tag = "SDFC", d.kind = 0; t.pac.dirs.push_back(d); });
    caught("BIN5: the template row changed", [](Lib& t) { t.rows.bin5[4] ^= 1; });
    caught("BINP: a part row's bank changed", [](Lib& t) { if (!t.rows.binp.empty()) t.rows.binp[0].row[8] ^= 1; });
    caught("BINP: a part row dropped", [](Lib& t) { if (!t.rows.binp.empty()) t.rows.binp.pop_back(); });
    caught("LSD: a list row's value changed", [](Lib& t) { if (!t.rows.lsd.empty()) t.rows.lsd[0].row[0] ^= 1; });
    caught("LSD: a list row dropped", [](Lib& t) { if (!t.rows.lsd.empty()) t.rows.lsd.pop_back(); });
    caught("T102: not his entrance wrestler's bank", [](Lib& t) { t.rows.t102_src_bank += 1; });
    caught("T102: missing", [](Lib& t) { t.rows.has_t102 = false; });
    caught("NAME: a byte of his name changed (review)", [](Lib& t) { t.rows.names[0][0] ^= 1; });
    caught("NAME: his short name changed (review)", [](Lib& t) { t.rows.names[1] += "x"; });
  }
  // ---- 2. add / remove / edit of a card changes no byte of frame\ (and no other card's folder)
  {
    const fs::path frame_dir = Root(mods) / "frame";
    const uint64_t h_frame = HashTree(frame_dir), h0 = HashTree(Root(mods) / "library" / k0), h1 = HashTree(Root(mods) / "library" / k1);
    Say(h_frame != 0, "the frame is there (digest " + std::to_string(h_frame % 100000) + ")");
    importer::PackState cur;
    importer::LoadPack(mods, cur, e);
    // EDIT: a new name for the second star
    importer::PackState ed = cur;
    ed.stars[1].name = "Edited Star";
    const auto te = Clock::now();
    const bool ok_e = importer::SavePack(mods, ed, e) && Build(env, mods, ed, env.dlc, "");
    const double t_edit = Secs(te);
    Say(ok_e && HashTree(frame_dir) == h_frame, "edit of a card (his name): the frame is byte-identical (digest unchanged)");
    Say(HashTree(Root(mods) / "library" / k0) == h0 && HashTree(Root(mods) / "library" / k1) != h1, "edit: the other card's folder is byte-identical, his own was rewritten");
    Lib e1 = ReadLib(mods, k1);
    Say(e1.ok && e1.rows.names[0] == "Edited Star" && e1.card.name == "Edited Star", "edit: his rows.bin names say 'Edited Star'");
    std::printf("  info: one character edited and built: %.2f s (the 2-character build above took %.2f s)\n", t_edit, full2);
    // ADD: a third character
    importer::PackState ad = ed;
    importer::LoadPack(mods, ad, e);
    ad.stars.push_back(MakeStar(importer::ExtractSource(env.source, importer::InspectSource(env.source).entry_name, mods, &e), 400, "Third Star"));  // (a source is copied in for each import: the build removes the copies no card names)
    const bool ok_a = importer::SavePack(mods, ad, e) && Build(env, mods, ad, env.dlc, "");
    Say(ok_a && HashTree(frame_dir) == h_frame && HashTree(Root(mods) / "library" / k0) == h0, "add of a card: the frame and the first card's folder are byte-identical");
    importer::PackState rm;
    importer::LoadPack(mods, rm, e);
    const size_t before = rm.stars.size();
    rm.stars.erase(std::remove_if(rm.stars.begin(), rm.stars.end(), [&](const importer::Superstar& s) { return s.slot == 398; }), rm.stars.end());
    const bool ok_r = importer::SavePack(mods, rm, e) && Build(env, mods, rm, env.dlc, "");
    Say(ok_r && before == 3 && rm.stars.size() == 2 && HashTree(frame_dir) == h_frame && HashTree(Root(mods) / "library" / k0) == h0 &&
            !fs::exists(Root(mods) / "library" / k1) && importer::RetiredSlots(mods) == std::vector<int>{398},
        "remove of a card: the frame is byte-identical, his folder is gone, his seat 398 is retired");
    // a twin for the digest itself: a changed frame byte changes it
    Bytes fb = Read(frame_dir / "superstars.txt");
    fb.push_back('x');
    { std::ofstream(frame_dir / "superstars.txt", std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(fb.data()), std::streamsize(fb.size())); }
    Twin("frame: one byte appended to superstars.txt moves the digest", HashTree(frame_dir) != h_frame);
    fb.pop_back();
    { std::ofstream(frame_dir / "superstars.txt", std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(fb.data()), std::streamsize(fb.size())); }
    Say(HashTree(frame_dir) == h_frame, "(the byte taken out again: digest back)");
    // (S110 SEAT-A review) a seated card whose rows.bin went missing (inst still valid): the game would HOLD him, so the check before PLAY must
    // say build, and the build makes him again (before the review: needs_build false, CheckBeforePlay quiet, the build skipped him).
    // (S110 LIBSIZE-2) His one file is untouched by it: only rows.bin is made (the file's file_sig did not change).
    {
      std::error_code e7;
      const uint64_t h_pac = HashTree(Root(mods) / "library" / k0 / "pac");
      fs::remove(Root(mods) / "library" / k0 / "rows.bin", e7);
      importer::PackState hp;
      importer::LoadPack(mods, hp, e);
      const importer::PlayCheck hc = importer::CheckBeforePlay(mods, env.dlc, "");
      Say(hp.needs_build && hc.build, "a seated card's rows.bin gone: LoadPack needs a build, CheckBeforePlay says build ('" + hc.why + "')");
      Say(Build(env, mods, hp, env.dlc, "") && ReadLib(mods, k0).ok && HashTree(frame_dir) == h_frame && !importer::CheckBeforePlay(mods, env.dlc, "").build && HashTree(Root(mods) / "library" / k0 / "pac") == h_pac && h_pac != 0,
          "the build makes his rows again (his file byte-identical, the frame untouched); nothing left to do");
    }
    // (S110 SEAT3-R) a card whose rows.bin has NO relocation list (made before this lane) cannot be re-targeted: it counts as not made, before play and in a build
    {
      sc::SeatRows old_rows;
      std::string re;
      const fs::path rp = Root(mods) / "library" / k0 / "rows.bin";
      const bool read = sc::ReadRows(Slash(rp), old_rows, re) && old_rows.has_relo && !old_rows.relo.empty();
      old_rows.has_relo = false, old_rows.relo.clear();
      const bool wrote = read && sc::WriteRows(Slash(rp), old_rows, re);
      importer::PackState op;
      importer::LoadPack(mods, op, e);
      const importer::PlayCheck oc = importer::CheckBeforePlay(mods, env.dlc, "");
      Say(wrote && op.needs_build && oc.build, "a card made by an older importer (rows.bin with no RELO list): LoadPack needs a build, CheckBeforePlay says build ('" + oc.why + "')");
      Say(Build(env, mods, op, env.dlc, "") && ReadLib(mods, k0).ok && ReadLib(mods, k0).rows.has_relo && HashTree(frame_dir) == h_frame && !importer::CheckBeforePlay(mods, env.dlc, "").build,
          "the build makes him again, with the list (the frame untouched); nothing left to do");
    }
  }
  // ---- 3. the frame's own shape
  {
    const fs::path frame_dir = Root(mods) / "frame";
    io::CatInfo info;
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    gate[0] = 6u << 16 | 1;
    const std::string cw = io::ParseCatalogLikeGame(Read(frame_dir / "info" / "catalog.dlc"), gate, info);
    const auto fr = wwe2k14::superstars_fit::ReadBuilt(frame_dir / "superstars.txt");
    std::set<int> rows;
    for (const auto& b : info.bundles)
      for (const auto& r : b.rows) rows.insert(r.ch);
    const std::vector<int> seats = importer::FreeSlots(env.gf, env.dlc);
    Say(cw.empty() && info.entries.size() == 1 && info.entries[0].type == 21 && rows == std::set<int>(seats.begin(), seats.end()) && fr.seats == seats &&
            int(fr.seats.size()) == importer::SeatCount(mods),
        "frame: the catalog parses like the game's and holds one type-21 entry with a bundle row for each of the " + std::to_string(seats.size()) +
            " seats (= FreeSlots = SeatCount); 399 is not one" + (cw.empty() ? "" : ": " + cw));
    bool nothing = true;
    std::error_code ec;
    for (const auto& f : fs::recursive_directory_iterator(frame_dir / "pac", ec)) {
      const std::string n = f.path().filename().string();
      nothing = nothing && n.find("ch3") == std::string::npos && n.find("ch4") == std::string::npos && n != "gm_dlc06.pac" && n != "prtcls_dlc06.pac" && n != "Entrance_dlc06.pac" &&
                n != "Winning_dlc06.pac" && n != "evtdata_dlc06.pac" && n != "evtmov_dlc06.pac";
    }
    Say(nothing && !fs::exists(Root(mods) / "update") && !fs::exists(Root(mods) / "pack6"), "frame: no per-seat pac, no gm / prtcls / entrance / evtdata / evtmov pac; no update\\ overlay, no pack6\\");
    // the cumulative pacs are byte copies of the top pack's
    bool same = true;
    for (const char* f : {"ExclusiveAI", "audience", "sound"}) {
      const Bytes a = Read(frame_dir / "pac" / (std::string(f) + "_dlc06.pac"));
      const bool one = !a.empty() && a == Read(refp / (std::string(f) + "_dlc06.pac"));
      if (!one) std::printf("    (%s: frame %zu bytes, reference %zu bytes)\n", f, a.size(), Read(refp / (std::string(f) + "_dlc06.pac")).size());
      same = same && one;
    }
    Say(same, "frame: ExclusiveAI / audience / sound are the byte copies the old pack held");
    // chEtc: the top's own (no seat row edited): the reference's chEtc minus the two rows it added equals the frame's chEtc rows
    {
      io::Pac fp;
      std::string fe;
      std::vector<io::Leaf> fd;
      io::Entry* de = nullptr;
      bool eq = io::ReadPac(Read(frame_dir / "pac" / "ch" / "chEtc_dlc06.pac"), fp, fe) && (de = fp.Find("CHAR", "DAT ")) && io::ReadPach(de->blob, fd);
      int active_seat_rows = 0;
      for (const io::Leaf& l : fd)
        if (l.id >= 397 && l.id <= 499) {
          const Bytes d = io::Decode(l.raw);
          active_seat_rows += d.size() == 208 && d[4 + 16] == 1;
        }
      Say(eq && active_seat_rows == 0, "frame: chEtc is the top pack's (no seat row 397..499 is active: the seats are blank until the game seats a card)");
    }
  }
  // ---- 4. a 500-card library
  {
    const std::string m5 = FreshMods(env, "lib500", &pac);
    const std::vector<int> seats = importer::FreeSlots(env.gf, env.dlc);
    importer::PackState big;
    for (int i = 0; i < 500; ++i) big.stars.push_back(MakeStar(pac, i < int(seats.size()) ? seats[size_t(i)] : -1, "Lib " + std::to_string(i + 1)));
    const auto ts = Clock::now();
    const bool saved = importer::SavePack(m5, big, e);
    const double t_save = Secs(ts);
    const auto tb = Clock::now();
    const bool ok = saved && Build(env, m5, big, env.dlc, "");
    const double t_full = Secs(tb);
    importer::PackState lp;
    importer::LoadPack(m5, lp, e);
    int seated = 0, none = 0;
    std::set<int> used;
    std::set<std::string> keys;
    for (const auto& s : lp.stars) (s.slot >= 0 ? seated : none)++, used.insert(s.slot), keys.insert(s.key);
    Say(ok && lp.stars.size() == 500 && seated == int(seats.size()) && none == 500 - int(seats.size()) && keys.size() == 500 && !lp.needs_build,
        "500-card library builds: " + std::to_string(seated) + " seated, " + std::to_string(none) + " seat none, 500 distinct keys, nothing left to build (save " +
            std::to_string(t_save).substr(0, 5) + " s, build " + std::to_string(t_full).substr(0, 5) + " s)");
    // (S110 SEAT3-R) EVERY card is made: a seated one for his seat, a library character (`seat none`) for the REFERENCE seat = the frame's lowest seat, with that
    // seat's bank: his rows.bin (with the relocation list) and pac are made as for a seated character, so the game has a guest to bind
    int inst_ok = 0, guest_ok = 0;
    const int ref_seat = sc::kRefSeat;
    uint64_t seated_bytes = 0, guest_bytes = 0, guest_card_bytes = 0;
    auto tree_bytes = [](const fs::path& dir) {
      uint64_t n = 0;
      std::error_code te;
      for (const auto& f : fs::recursive_directory_iterator(dir, te))
        if (f.is_regular_file(te)) n += f.file_size(te);
      return n;
    };
    for (const auto& s : lp.stars) {
      const fs::path d = Root(m5) / "library" / s.key;
      const Lib l = ReadLib(m5, s.key);
      if (s.slot >= 0) {
        inst_ok += l.ok && l.card.seat == s.slot && l.card.inst.seat == sc::kRefSeat && l.card.inst.bank == sc::kRefBank && sc::FileForm(l.card) == sc::kFileForm && l.rows.has_relo && l.rows.seat == sc::kRefSeat;
        seated_bytes += tree_bytes(d);
      } else {
        guest_ok += l.ok && l.card.seat == -1 && l.card.inst.seat == ref_seat && l.card.inst.bank == sc::kRefBank && sc::FileForm(l.card) == sc::kFileForm && l.rows.has_relo && l.rows.seat == ref_seat &&
                    !l.rows.relo.empty() && !l.rows.alias.empty();
        guest_bytes += tree_bytes(d);
        std::error_code se;
        guest_card_bytes += fs::file_size(d / "card.txt", se);
      }
    }
    Say(inst_ok == seated && guest_ok == none,
        "500-card library: each of the " + std::to_string(seated) + " seated cards and each of the " + std::to_string(none) + " `seat none` ones has the same form: inst " + std::to_string(ref_seat) +
            " / " + std::to_string(sc::kRefBank) + ", file_form 2, rows.bin (RELO + ALIA) and ONE file pac\\ch\\ch497.pac");
    std::printf("    library size on disk: %.1f MB seated (%d cards), %.1f MB unseated (%d cards; before S110 SEAT3-R only their card.txt, %.1f KB); the full build took %.2f s\n",
                double(seated_bytes) / 1048576.0, seated, double(guest_bytes) / 1048576.0, none, double(guest_card_bytes) / 1024.0, t_full);
    const fs::path fd = Root(m5) / "frame";
    const uint64_t hf = HashTree(fd);
    // one change: a seated card edited -> only his folder is written (timing), the frame stays
    importer::PackState ed = lp;
    ed.stars[4].name = "Lib 5 edited";
    const std::string key5 = ed.stars[4].key, key6 = ed.stars[5].key;
    const uint64_t h6 = HashTree(Root(m5) / "library" / key6), h5 = HashTree(Root(m5) / "library" / key5);
    const auto t1 = Clock::now();
    const bool ok1 = importer::SavePack(m5, ed, e) && Build(env, m5, ed, env.dlc, "");
    const double t_one = Secs(t1);
    Say(ok1 && HashTree(fd) == hf && HashTree(Root(m5) / "library" / key6) == h6 && HashTree(Root(m5) / "library" / key5) != h5,
        "500 cards: one character edited -> only his folder changes, the frame is byte-identical (" + std::to_string(t_one).substr(0, 5) + " s; the full build was " +
            std::to_string(t_full).substr(0, 6) + " s)");
    // an unseated card edited: only his folder is made again (for the reference seat), the frame stays
    importer::PackState ed2 = ed;
    ed2.stars[300].name = "Unseated edited";
    const std::string key300 = ed2.stars[300].key, key301 = ed2.stars[301].key;
    const uint64_t h300 = HashTree(Root(m5) / "library" / key300), h301 = HashTree(Root(m5) / "library" / key301);
    const bool ok2 = importer::SavePack(m5, ed2, e) && Build(env, m5, ed2, env.dlc, "");
    Say(ok2 && HashTree(fd) == hf && HashTree(Root(m5) / "library" / key300) != h300 && HashTree(Root(m5) / "library" / key301) == h301 && ReadLib(m5, key300).ok,
        "500 cards: an unseated card edited: his folder is made again for the reference seat, the next card's folder and the frame are byte-identical");
    // a seated card removed: his seat is free, the first unseated card takes it (seated again to the full count)
    importer::PackState rm = ed2;
    rm.stars.erase(rm.stars.begin() + 7);
    const bool ok3 = importer::SavePack(m5, rm, e) && Build(env, m5, rm, env.dlc, "");
    importer::PackState lp2;
    importer::LoadPack(m5, lp2, e);
    int seated2 = 0;
    for (const auto& s : lp2.stars) seated2 += s.slot >= 0;
    Say(ok3 && HashTree(fd) == hf && lp2.stars.size() == 499 && seated2 == seated && !lp2.needs_build,
        "500 cards: one seated card removed -> the frame is byte-identical, a card that had no seat takes the free one (" + std::to_string(seated2) + " seated of 499)");
    // ---- (S110 LIBSIZE-2) a frame change and a seat change write NO character file (hash of every card's pac folder before / after); an old-form card is rebuilt
    {
      auto pac_hashes = [&](const importer::PackState& p) {
        std::map<std::string, uint64_t> h;
        for (const auto& s2 : p.stars) h[s2.key] = HashTree(Root(m5) / "library" / s2.key / "pac");
        return h;
      };
      auto rows_hashes = [&](const importer::PackState& p) {
        std::map<std::string, uint64_t> h;
        for (const auto& s2 : p.stars) {
          const Bytes rb = Read(Root(m5) / "library" / s2.key / "rows.bin");
          h[s2.key] = Fnv64(1469598103934665603ull, rb.data(), rb.size());
        }
        return h;
      };
      importer::PackState cur;
      importer::LoadPack(m5, cur, e);
      const std::map<std::string, uint64_t> before_pac = pac_hashes(cur), before_rows = rows_hashes(cur);
      int zero = 0;
      for (const auto& kv : before_pac) zero += kv.second == 0;
      // a SEAT change: two seated cards swap seats
      importer::PackState sw = cur;
      std::swap(sw.stars[0].slot, sw.stars[1].slot);
      const auto ts2 = Clock::now();
      const bool ok_s = importer::SavePack(m5, sw, e) && Build(env, m5, sw, env.dlc, "");
      const double t_seat = Secs(ts2);
      importer::PackState after_s;
      importer::LoadPack(m5, after_s, e);
      const bool sw_pac = pac_hashes(after_s) == before_pac, sw_rows = rows_hashes(after_s) == before_rows, sw_frame = HashTree(fd) == hf;
      Say(ok_s && zero == 0 && after_s.stars.size() == cur.stars.size() && cur.stars.size() == 499 && sw_pac && sw_rows && sw_frame && !after_s.needs_build,
          "500 cards: two cards swap seats -> no character file and no rows.bin is written (hash of every pac folder and rows.bin before / after), the frame is byte-identical, nothing left to build (" +
              std::to_string(t_seat).substr(0, 5) + " s) [ok " + std::to_string(ok_s) + " zero " + std::to_string(zero) + " n " + std::to_string(after_s.stars.size()) + " pac " + std::to_string(sw_pac) + " rows " +
              std::to_string(sw_rows) + " frame " + std::to_string(sw_frame) + " needs_build " + std::to_string(after_s.needs_build) + "]");
      Say(after_s.stars[0].slot == sw.stars[0].slot && after_s.stars[1].slot == sw.stars[1].slot, "500 cards: the swap is in the cards (seat " + std::to_string(sw.stars[0].slot) + " / " + std::to_string(sw.stars[1].slot) + ")");
      // a FRAME change: another top pack (pack 5 off): the frame is rebuilt, every card's rows.bin is made again, no character file
      const auto tf = Clock::now();
      std::string wf;
      const bool ok_f = importer::BuildPack(m5, env.gf, env.dlc, after_s, nullptr, wf, "5");
      const double t_frame = Secs(tf);
      importer::PackState after_f;
      importer::LoadPack(m5, after_f, e);
      const std::map<std::string, uint64_t> rows_f = rows_hashes(after_f);
      int rows_moved = 0;
      for (const auto& kv : rows_f) rows_moved += kv.second != before_rows.at(kv.first);
      Say(ok_f && pac_hashes(after_f) == before_pac && HashTree(fd) != hf && rows_moved > 0 && !importer::CheckBeforePlay(m5, env.dlc, "5").build,
          "500 cards: a frame change (another top pack) -> NO character file is written (hash of every pac folder before / after), " + std::to_string(rows_moved) +
              " rows.bin are made again, the frame is new, nothing left to build (" + std::to_string(t_frame).substr(0, 5) + " s; the first build " + std::to_string(t_full).substr(0, 6) + " s)" + (ok_f ? "" : ": " + wf));
      std::printf("    500-card library: frame change %.2f s, seat change %.2f s, the whole first build %.2f s\n", t_frame, t_seat, t_full);
      // an OLD-FORM card (inst for a seat, no file_form, ch<seat>_dlc<PP>.pac, his source in sources\) is rebuilt; the other cards stay byte-identical
      {
        importer::PackState cur2;
        importer::LoadPack(m5, cur2, e);
        const std::string ko = cur2.stars[10].key, kn = cur2.stars[11].key;
        const fs::path od = Root(m5) / "library" / ko;
        sc::Card oc;
        std::string oe;
        std::error_code e6;
        bool made_old = sc::ReadCard(Slash(od / "card.txt"), oc, oe);
        fs::create_directories(Root(m5) / "sources", e6);
        const fs::path old_src = Root(m5) / "sources" / "old form.pac";
        fs::copy_file(fs::u8path(env.source), old_src, fs::copy_options::overwrite_existing, e6);  // (an old-form card's source is the player's plain pac)
        made_old = made_old && !e6;
        fs::create_directories(od / "pac2" / "ch", e6);
        fs::rename(od / "pac" / "ch" / "ch497.pac", od / "pac2" / "ch" / "ch400_dlc06.pac", e6);
        fs::remove_all(od / "pac", e6);
        fs::rename(od / "pac2", od / "pac", e6);
        oc.inst.seat = 400, oc.inst.bank = 12, oc.source = "sources\\old form.pac", oc.extra.erase(std::remove_if(oc.extra.begin(), oc.extra.end(), [](const auto& kv) { return kv.first == "file_form" || kv.first == "file_sig" || kv.first == "model_sig"; }), oc.extra.end());
        made_old = made_old && sc::WriteCard(Slash(od / "card.txt"), oc, &oe);
        const uint64_t hn = HashTree(Root(m5) / "library" / kn);
        importer::PackState lo;
        const bool rd = importer::LoadPack(m5, lo, e);
        const importer::PlayCheck pc = importer::CheckBeforePlay(m5, env.dlc, "5");
        Say(made_old && rd && lo.needs_build && pc.build, "500 cards: a card in the old per-seat form (inst for seat 400, no file_form, ch400_dlc06.pac, his source in sources\\) makes LoadPack need a build and CheckBeforePlay say build ('" + pc.why + "')");
        std::string wo;
        const bool ok_o = importer::BuildPack(m5, env.gf, env.dlc, lo, nullptr, wo, "5");
        const Lib ln = ReadLib(m5, ko);
        std::error_code e5;
        Say(ok_o && ln.ok && ln.card.inst.seat == sc::kRefSeat && ln.card.inst.bank == sc::kRefBank && sc::FileForm(ln.card) == sc::kFileForm && !fs::exists(od / "pac" / "ch" / "ch400_dlc06.pac", e5) &&
                fs::is_regular_file(od / "pac" / "ch" / "ch497.pac", e5) && HashTree(Root(m5) / "library" / kn) == hn && !fs::exists(old_src, e5) && ln.card.source == "library\\" + ko + "\\pac\\ch\\ch497.pac" &&
                !importer::CheckBeforePlay(m5, env.dlc, "5").build,
            "500 cards: the old-form card is rebuilt into the one form (ch497.pac, file_form 2, his source is his own file, the sources\\ copy gone, no ch400 pac), the next card's folder is byte-identical" + (ok_o ? "" : ": " + wo) +
                " [ok " + std::to_string(ok_o) + " read " + std::to_string(ln.ok) + " inst " + std::to_string(ln.card.inst.seat) + "/" + std::to_string(ln.card.inst.bank) + " form " + std::to_string(sc::FileForm(ln.card)) + " ch400 " +
                std::to_string(fs::exists(od / "pac" / "ch" / "ch400_dlc06.pac", e5)) + " ch497 " + std::to_string(fs::is_regular_file(od / "pac" / "ch" / "ch497.pac", e5)) + " next " + std::to_string(HashTree(Root(m5) / "library" / kn) == hn) +
                " src " + std::to_string(!fs::exists(old_src, e5)) + " source '" + ln.card.source + "' check " + std::to_string(!importer::CheckBeforePlay(m5, env.dlc, "5").build) + "]");
      }
    }
    std::error_code ec;
    fs::remove_all(fs::u8path(m5), ec);
  }
  // ---- 5. migration of an old pack (the reference pack, copied)
  {
    auto old_hash = [&](const fs::path& r) {
      uint64_t h = HashTree(r / "pack6");
      h ^= HashTree(r / "update") * 3;
      for (const char* f : {"pack.json", "superstars.txt"}) {
        const Bytes b = Read(r / f);
        h = Fnv64(h, b.data(), b.size());
      }
      return h;
    };
    // with `bank` lines (the reference was built by the importer that writes them)
    const fs::path mw = fs::u8path(env.mods_root) / "mig_with";
    CopyTree(Root(env.ref), mw / "superstars");
    const std::string mws = Slash(mw);
    importer::PackState old;
    Say(importer::LoadPack(mws, old, e) && old.stars.size() == 2 && old.stars[0].slot == 397 && old.stars[0].key.empty() && old.needs_build, "migration: LoadPack reads the old pack.json (2 characters, no keys), needs a build");
    const importer::PlayCheck pc = importer::CheckBeforePlay(mws, env.dlc, "");
    Say(pc.build && pc.stars == 2, "migration: CheckBeforePlay says build: '" + pc.why + "'");
    const fs::path r = Root(mws);
    {  // (review) an earlier attempt's legacy.old\ (it may hold the only copy of something): kept under a numbered name, never wiped
      std::error_code e9;
      fs::create_directories(r / "legacy.old", e9);
      std::ofstream(r / "legacy.old" / "earlier.txt") << "an earlier attempt\n";
    }
    const bool ok = Build(env, mws, old, env.dlc, "");
    bool compared = false;
    for (const auto& l : importer::LastBuildLog()) compared = compared || l.find("2 of 2 characters compared with the old pack") != std::string::npos;
    Say(ok && compared, "migration with `bank` lines: built, and both characters' rows and models were compared with the old pack and are equal");
    Say(!fs::exists(r / "pack6") && !fs::exists(r / "update") && !fs::exists(r / "pack.json") && !fs::exists(r / "superstars.txt") && fs::exists(r / "pack.json.old") &&
            fs::exists(r / "frame" / "info" / "catalog.dlc") && fs::exists(r / "sources"),
        "migration: pack6\\ update\\ superstars.txt gone, pack.json kept as pack.json.old, the frame in place, sources\\ kept");
    Say(fs::exists(r / "legacy.old" / "pack6" / "info" / "catalog.dlc") && fs::exists(r / "legacy.old" / "pack.json") && fs::exists(r / "legacy.old.2" / "earlier.txt"),
        "migration (review): the old pack is KEPT in legacy.old\\ (pack6\\, pack.json), the earlier attempt's legacy.old\\ is kept as legacy.old.2\\");
    {
      importer::PackState again;
      importer::LoadPack(mws, again, e);
      Build(env, mws, again, env.dlc, "");
      Say(fs::exists(r / "legacy.old") && fs::exists(r / "legacy.old.2"), "migration (review): a build before the game said \"in N\" keeps legacy.old\\");
      std::ofstream(r / "game_status.txt", std::ios::trunc) << "in 2\n";
      Build(env, mws, again, env.dlc, "");
      Say(!fs::exists(r / "legacy.old") && !fs::exists(r / "legacy.old.2"), "migration (review): the game said \"in 2\" (it booted on the library): the next build removes legacy.old\\ and legacy.old.2\\");
    }
    importer::PackState nw;
    importer::LoadPack(mws, nw, e);
    Say(nw.stars.size() == 2 && nw.stars[0].slot == 397 && nw.stars[1].slot == 398 && nw.stars[0].name == "Ref Star" && nw.stars[1].name == "Ref Star 2" && !nw.needs_build && sc::ValidKey(nw.stars[0].key),
        "migration: the list after is the same two characters on the same seats (397, 398), keyed, nothing left to build");
    bool all = nw.stars.size() == 2;
    std::string diff;
    for (size_t i = 0; all && i < nw.stars.size(); ++i) {
      Lib l = ReadLib(mws, nw.stars[i].key);
      std::vector<std::string> bad = l.ok ? Compare(l, ref, nw.stars[i].slot, 110) : std::vector<std::string>{"unreadable"};
      for (const std::string& b : bad) diff += " [" + b + "]";
      all = all && bad.empty();
    }
    Say(all && importer::PackProblem(mws, env.dlc, "").empty() && !importer::CheckBeforePlay(mws, env.dlc, "").build, "migration: the migrated cards equal the reference pack's rows and entries" + diff);
    // without `bank` lines (a pack from before BANKID: superstars.txt has no bank): the ordinal numbers apply, nothing to compare
    const fs::path mn = fs::u8path(env.mods_root) / "mig_without";
    CopyTree(Root(env.ref), mn / "superstars");
    {
      std::ofstream(Root(Slash(mn)) / "superstars.txt", std::ios::binary | std::ios::trunc) << "pack 6\nbuilt_on 5\nstar 397 music 110\nstar 398 music 110\n";
    }
    // (S110 SEAT-A review) the planted twins of the old pack without `bank` lines: each refuses the migration and leaves the old pack as it was
    // (before the review nothing was compared there: every one of these went through and the old pack was deleted)
    {
      auto plant = [&](const std::string& name, const std::function<void(const fs::path&)>& change, bool want_refused, const std::function<void(importer::PackState&)>& edit = nullptr) {
        const fs::path mt = fs::u8path(env.mods_root) / "mig_twin";
        CopyTree(Root(env.ref), mt / "superstars");
        const fs::path r = Root(Slash(mt));
        std::ofstream(r / "superstars.txt", std::ios::binary | std::ios::trunc) << "pack 6\nbuilt_on 5\nstar 397 music 110\nstar 398 music 110\n";
        change(r);
        const uint64_t h = old_hash(r);
        importer::PackState o;
        importer::LoadPack(Slash(mt), o, e);
        if (edit) edit(o);
        std::string w;
        const bool built = Build(env, Slash(mt), o, env.dlc, "", &w);
        if (want_refused) Twin(name + " (" + w + ")", !built && w.find("your old pack is untouched") != std::string::npos && old_hash(r) == h && !fs::exists(r / "frame"));
        else Say(built && !fs::exists(r / "pack6"), name);
        std::error_code e5;
        fs::remove_all(mt, e5);
      };
      auto rewrite_pac = [](const fs::path& f, const std::function<void(io::Pac&)>& change) {
        io::Pac p;
        std::string pe;
        if (!io::ReadPac(Read(f), p, pe)) return;
        change(p);
        const Bytes w = io::WritePac(p);
        std::ofstream(f, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
      };
      auto chetc_leaf = [&](const fs::path& r, const char* tag, uint32_t id, size_t off) {
        rewrite_pac(r / "pack6" / "pac" / "ch" / "chEtc_dlc06.pac", [&](io::Pac& p) {
          io::Entry* x = p.Find("CHAR", tag);
          std::vector<io::Leaf> lv;
          if (!x || !io::ReadPach(x->blob, lv)) return;
          for (io::Leaf& l : lv)
            if (l.id == id) {
              Bytes d = io::Decode(l.raw);
              d[off] ^= 1;
              l.raw = std::string(tag) == "DSBD" ? d : io::BpeIdentity(d, 0x100);
            }
          x->blob = io::WritePach(lv);
        });
      };
      plant("no bank lines: a model byte of 397 changed in the old pack", [&](const fs::path& r) {
        rewrite_pac(r / "pack6" / "pac" / "ch" / "ch397_dlc06.pac", [](io::Pac& p) {
          if (io::Dir* d = p.FindDir("EMD "))
            for (io::Entry& x : d->ents)
              if (x.alias < 0 && !x.blob.empty()) { x.blob[x.blob.size() / 2] ^= 1; return; }
        });
      }, true);
      plant("no bank lines: a DSBD byte of 397 changed", [&](const fs::path& r) { chetc_leaf(r, "DSBD", 397, 0); }, true);
      plant("no bank lines: a PRO byte of 397 outside the asset words (+100) changed", [&](const fs::path& r) { chetc_leaf(r, "PRO ", 397, 100); }, true);
      plant("no bank lines: the music word +576 of 397 changed", [&](const fs::path& r) { chetc_leaf(r, "PRO ", 397, 576); }, true);
      plant("no bank lines: a name byte of 397 (text 60397) changed in the old string pac", [&](const fs::path& r) {
        rewrite_pac(r / "pack6" / "pac" / "string_dlc06.pac", [](io::Pac& p) {
          io::Dir* sdb = p.FindDir("SDB ");
          if (!sdb) return;
          for (io::Entry& x : sdb->ents) {
            std::vector<io::Leaf> lv;
            if (!io::ReadPach(x.blob, lv)) continue;
            bool hit = false;
            for (io::Leaf& l : lv) {
              if (l.raw.size() < 18 || std::memcmp(l.raw.data(), "ZLIB", 4) != 0) continue;
              Bytes t = io::Decode(l.raw);
              const uint32_t cnt = t.size() >= 8 ? io::L32(&t[4]) : 0;
              for (uint32_t i = 0; i < cnt && 8 + 12ull * (i + 1) <= t.size(); ++i)
                if (io::L32(&t[8 + 12 * i + 8]) == 60397 && io::L32(&t[8 + 12 * i + 4]) > 1) t[io::L32(&t[8 + 12 * i])] ^= 1, hit = true;
              if (hit) l.raw = io::ZlibLeaf(t, io::L32(&l.raw[4]));
              if (hit) break;
            }
            if (hit) { x.blob = io::WritePach(lv); return; }
          }
        });
      }, true);
      plant("no bank lines: an old portrait renders\\397_A.png the card does not get", [&](const fs::path& r) {
        std::error_code e6;
        fs::create_directories(r / "renders", e6);
        const Bytes png = importer::png::Write(std::vector<uint8_t>(4 * 4 * 4, 200), 4, 4);
        std::ofstream(r / "renders" / "397_A.png", std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
      }, true, [](importer::PackState& o) { for (auto& s : o.stars) for (auto& v : s.render_rgba) v.clear(); });
      // and what is NOT a difference: the asset words (+578, older rules), a pack saved but not built, a character edited since the old build
      plant("no bank lines: the movie word +578 of 397 differs (older rules): migrated", [&](const fs::path& r) { chetc_leaf(r, "PRO ", 397, 578); }, false);
      plant("a pack saved but not built (needs_build true) whose DSBD of 397 differs: migrated (his old bytes are not his any more)", [&](const fs::path& r) {
        chetc_leaf(r, "DSBD", 397, 0);
        std::string t = TextOf(r / "pack.json");
        const size_t at = t.find("\"needs_build\": false");
        if (at != std::string::npos) t.replace(at, 20, "\"needs_build\": true");
        std::ofstream(r / "pack.json", std::ios::binary | std::ios::trunc) << t;
      }, false);
      plant("397 renamed in the importer before the first build (his DSBD differs too): migrated, not compared", [&](const fs::path& r) { chetc_leaf(r, "DSBD", 397, 0); }, false,
            [](importer::PackState& o) { o.stars[0].name = "Renamed Star"; });
    }
    importer::PackState old2;
    importer::LoadPack(Slash(mn), old2, e);
    const bool ok2 = Build(env, Slash(mn), old2, env.dlc, "");
    bool all_cmp = false;
    for (const auto& l : importer::LastBuildLog()) all_cmp = all_cmp || l.find("2 of 2 characters compared") != std::string::npos;
    importer::PackState nw2;
    importer::LoadPack(Slash(mn), nw2, e);
    Say(ok2 && all_cmp && nw2.stars.size() == 2 && !fs::exists(Root(Slash(mn)) / "pack6") && fs::exists(Root(Slash(mn)) / "pack.json.old") && !nw2.needs_build,
        "migration without `bank` lines: built on the ordinal numbers, both characters compared (rows outside the asset words, names, models), old pack out, pack.json.old kept");
    // a failed migration leaves the old pack intact: the source pac is gone
    const fs::path mf = fs::u8path(env.mods_root) / "mig_fail";
    CopyTree(Root(env.ref), mf / "superstars");
    std::error_code ec;
    fs::remove_all(Root(Slash(mf)) / "sources", ec);
    {  // pack.json's recorded path points into the reference's folder: make it a path that is not there
      std::string t = TextOf(Root(Slash(mf)) / "pack.json");
      for (size_t at = t.find("\"source_pac\": \""); at != std::string::npos; at = t.find("\"source_pac\": \"", at + 1))  // (every star's: review)
        t.insert(at + 15, "Z:\\\\nowhere\\\\");
      std::ofstream(Root(Slash(mf)) / "pack.json", std::ios::binary | std::ios::trunc) << t;
    }
    // (S110 SEAT-A review) both source pacs gone: each model is recovered from the old pack's own model pac (into sources\), compared with the
    // old pack as usual, and the migration completes (before the review: refused at every build, the characters stuck in the old pack)
    const fs::path mf2 = fs::u8path(env.mods_root) / "mig_fail_nopac";
    CopyTree(Root(Slash(mf)), mf2 / "superstars");
    fs::remove(Root(Slash(mf2)) / "pack6" / "pac" / "ch" / "ch397_dlc06.pac", ec);
    {
      importer::PackState old3;
      importer::LoadPack(Slash(mf), old3, e);
      std::string why3;
      const bool ok3 = Build(env, Slash(mf), old3, env.dlc, "", &why3);
      int said = 0;
      for (const auto& mv : importer::LastBuildMoves()) said += mv.find("recovered from the old pack") != std::string::npos;
      bool cmp2 = false;
      for (const auto& l : importer::LastBuildLog()) cmp2 = cmp2 || l.find("2 of 2 characters compared") != std::string::npos;
      importer::PackState nw3;
      importer::LoadPack(Slash(mf), nw3, e);
      bool equal = nw3.stars.size() == 2, src_there = true;
      std::string diff;
      for (const auto& s : nw3.stars) {
        const Lib l = ReadLib(Slash(mf), s.key);
        std::vector<std::string> bad = l.ok ? Compare(l, ref, s.slot, 110) : std::vector<std::string>{"unreadable"};
        for (const std::string& b : CompareModels(l, env.ref, s.slot)) bad.push_back(b);
        for (const std::string& b : bad) diff += " [" + b + "]";
        equal = equal && bad.empty();
        src_there = src_there && fs::is_regular_file(fs::u8path(s.source_pac)) && LowerS(s.source_pac).find(LowerS("library\\" + s.key + "\\pac\\ch\\ch497.pac")) != std::string::npos;  // (his own file is his source now)
      }
      Say(ok3 && said == 2 && cmp2 && equal && src_there && !fs::exists(Root(Slash(mf)) / "pack6") && !nw3.needs_build,
          "migration with both source pacs gone (review): models recovered from the old pack into sources\\, both compared, the cards equal the reference" +
              (why3.empty() ? diff : ": " + why3) + (ok3 && said == 2 && cmp2 && equal && src_there ? std::string() :
              " (said " + std::to_string(said) + ", compared " + std::to_string(cmp2) + ", equal " + std::to_string(equal) + ", sources " + std::to_string(src_there) +
              ", needs_build " + std::to_string(nw3.needs_build) + ")"));
    }
    {  // the source AND the old pack's model pac of 397 gone: nothing to recover from; the migration stops and the old pack stays as it was
      const uint64_t before = old_hash(Root(Slash(mf2)));
      importer::PackState old3;
      importer::LoadPack(Slash(mf2), old3, e);
      std::string why3;
      const bool ok3 = Build(env, Slash(mf2), old3, env.dlc, "", &why3);
      Say(!ok3 && why3.find("cannot read") != std::string::npos && old_hash(Root(Slash(mf2))) == before && !fs::exists(Root(Slash(mf2)) / "frame") &&
              !fs::exists(Root(Slash(mf2)) / "frame.building") && !fs::exists(Root(Slash(mf2)) / "legacy.old"),
          "failed migration (a source pac and its old model pac gone): BuildPack says '" + why3 + "'; pack6\\ update\\ pack.json superstars.txt are byte-identical, no frame");
    }
    fs::remove_all(mf2, ec);
    // a failed verify: the old pack's DSBD row for 397 differs from what the cards hold (planted in the copy)
    const fs::path mv = fs::u8path(env.mods_root) / "mig_verify";
    CopyTree(Root(env.ref), mv / "superstars");
    {
      const fs::path etc = Root(Slash(mv)) / "pack6" / "pac" / "ch" / "chEtc_dlc06.pac";
      io::Pac p;
      std::string pe;
      std::vector<io::Leaf> lv;
      io::Entry* ds = nullptr;
      if (io::ReadPac(Read(etc), p, pe) && (ds = p.Find("CHAR", "DSBD")) && io::ReadPach(ds->blob, lv)) {
        for (io::Leaf& l : lv)
          if (l.id == 397) l.raw[0] ^= 1;
        ds->blob = io::WritePach(lv);
        const Bytes w = io::WritePac(p);
        std::ofstream(etc, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
      }
    }
    const uint64_t before4 = old_hash(Root(Slash(mv)));
    importer::PackState old4;
    importer::LoadPack(Slash(mv), old4, e);
    std::string why4;
    const bool ok4 = Build(env, Slash(mv), old4, env.dlc, "", &why4);
    Say(!ok4 && why4.find("your old pack is untouched") != std::string::npos && why4.find("DSBD") != std::string::npos && old_hash(Root(Slash(mv))) == before4 && !fs::exists(Root(Slash(mv)) / "frame"),
        "failed migration (the old pack's row differs from the card's): refused with '" + why4 + "'; the old pack is byte-identical, no frame");
    {  // (S110 SEAT-A review) the same planted row, but pack.json says it was saved and not built (needs_build true): the old bytes are not his
       // any more, so they are not compared and the migration goes on (before the review: refused at every build, for good)
      std::string t = TextOf(Root(Slash(mv)) / "pack.json");
      const size_t at = t.find("\"needs_build\": false");
      if (at != std::string::npos) t.replace(at, 20, "\"needs_build\": true");
      std::ofstream(Root(Slash(mv)) / "pack.json", std::ios::binary | std::ios::trunc) << t;
      importer::PackState old5;
      importer::LoadPack(Slash(mv), old5, e);
      std::string why5;
      Say(Build(env, Slash(mv), old5, env.dlc, "", &why5) && !fs::exists(Root(Slash(mv)) / "pack6"), "an old pack with `bank` lines saved but not built (its 397 row differs): migrated" + (why5.empty() ? "" : ": " + why5));
    }
    for (const char* d : {"mig_with", "mig_without", "mig_fail", "mig_verify"}) fs::remove_all(fs::u8path(env.mods_root) / d, ec);
  }
  // ---- 6. (S110 SEAT-A review) the design points decided after the review
  {
    std::string pac6;
    const std::string m6 = FreshMods(env, "lib_review", &pac6);
    const fs::path src2 = fs::u8path(pac6).parent_path() / "second copy.pac";  // (a second source file: one card's can go without the other's)
    std::error_code e8;
    fs::copy_file(fs::u8path(pac6), src2, fs::copy_options::overwrite_existing, e8);
    importer::PackState s6;
    s6.stars.push_back(MakeStar(pac6, 397, "Rev One"));
    s6.stars.push_back(MakeStar(Slash(src2), 398, "Rev Two"));
    Say(Build(env, m6, s6, env.dlc, ""), "review: two characters built");
    importer::PackState l6;
    importer::LoadPack(m6, l6, e);
    const std::string ka = l6.stars.size() == 2 ? l6.stars[0].key : "", kb = l6.stars.size() == 2 ? l6.stars[1].key : "";
    const fs::path lib6 = Root(m6) / "library";
    {  // 7. (S110 LIBSIZE-2) what the file is made from changes (his base wrestler): his file is made again, the other card's folder is untouched; a NAME edit makes rows only
      importer::PackState x;
      importer::LoadPack(m6, x, e);
      const uint64_t ha = HashTree(lib6 / ka / "pac"), hb = HashTree(lib6 / kb / "pac");
      Say(x.stars.size() == 2 && ha != 0 && hb != 0 && !x.needs_build, "review: both folders hold their one file (nothing to build)");
      if (x.stars.size() == 2) x.stars[0].name = "Rev One Renamed";
      Say(importer::SavePack(m6, x, e) && Build(env, m6, x, env.dlc, "") && HashTree(lib6 / ka / "pac") == ha && HashTree(lib6 / kb / "pac") == hb && ReadLib(m6, ka).card.name == "Rev One Renamed",
          "review: a name edit makes his rows and card only: no character file is written (the pac folders are byte-identical)");
      if (x.stars.size() == 2) x.stars[0].base_id = 100;
      Say(importer::SavePack(m6, x, e) && Build(env, m6, x, env.dlc, "") && HashTree(lib6 / ka / "pac") != ha && HashTree(lib6 / kb / "pac") == hb && ReadLib(m6, ka).ok && !importer::CheckBeforePlay(m6, env.dlc, "").build,
          "review: a new base wrestler changes the file's file_sig: his file is made again, the other folder untouched; nothing left to do");
    }
    {  // 2. a stale list still names the deleted sources\ file (and an edit): his model is taken from his own library file; no file is written, the model bytes are the same
      const Lib before_b = ReadLib(m6, kb);
      const uint64_t hb = HashTree(lib6 / kb / "pac");
      fs::remove(src2, e8);
      importer::PackState x;
      importer::LoadPack(m6, x, e);
      if (x.stars.size() == 2) x.stars[1].name = "Rev Two Edited", x.stars[1].source_pac = Slash(src2);  // (a launcher's list from before the build)
      importer::SavePack(m6, x, e);
      std::string w;
      const bool ok = Build(env, m6, x, env.dlc, "", &w);
      const Lib after_b = ReadLib(m6, kb);
      bool said = false;
      for (const auto& mv : importer::LastBuildMoves()) said = said || mv.find("his model is taken from his own library file") != std::string::npos;
      const io::Dir *d1 = DirOf(before_b.pac, "EMD "), *d2 = DirOf(after_b.pac, "EMD ");
      bool same = d1 && d2 && d1->ents.size() == d2->ents.size();
      for (size_t i = 0; same && i < d1->ents.size(); ++i)
        same = d1->ents[i].name == d2->ents[i].name && d1->ents[i].alias == d2->ents[i].alias && io::Blob(*d1, d1->ents[i]) == io::Blob(*d2, d2->ents[i]);
      Say(ok && said && after_b.ok && after_b.card.name == "Rev Two Edited" && same && HashTree(lib6 / kb / "pac") == hb && after_b.card.source == "library\\" + kb + "\\pac\\ch\\ch497.pac" &&
              fs::is_regular_file(Root(m6) / fs::u8path(after_b.card.source)),
          "review: his sources\\ file deleted, a stale list, an edit: his model comes from his own library file (the same model bytes, his file byte-identical, his source = his file)" + (w.empty() ? "" : ": " + w));
    }
    {  // 9. (S110 LIBSIZE-2 review) his file is his only copy of his model now: it is never left behind his card, never lost between Publish's steps; a sources\ file
       // no card ever named (a pick the importer has not added yet) is never deleted
      const fs::path kdir = lib6 / kb, inbox = Root(m6) / "sources" / "inbox pick.pac";
      const fs::path kfile = kdir / "pac" / "ch" / ("ch" + std::to_string(sc::kRefSeat) + ".pac");
      std::error_code e9;
      fs::copy_file(kfile, inbox, fs::copy_options::overwrite_existing, e9);
      const uint64_t h_old = HashTree(kdir / "pac");
      importer::PackState x;
      importer::LoadPack(m6, x, e);
      if (x.stars.size() == 2) x.stars[1].entrance_from = 107;  // (what his file is made from changes: his file must be made again)
      importer::SavePack(m6, x, e);
      bool built = false, intact = false;
      {
        std::ifstream hold(kfile, std::ios::binary);  // (his file open, as in a running game: his folder cannot be moved or emptied)
        built = Build(env, m6, x, env.dlc, "");
        intact = HashTree(kdir / "pac") == h_old && !ReadLib(m6, kb).ok;
      }
      Say(built && intact, "review: his file is open while his new one is put in place: the build goes on, his old file is whole, his card not made");
      const bool again = Build(env, m6, x, env.dlc, "");
      const uint64_t h_new = HashTree(kdir / "pac");
      Say(again && ReadLib(m6, kb).ok && h_new != h_old && h_new != 0 && !fs::exists(kdir / "pac.old") && !fs::exists(kdir / "stage") && !importer::CheckBeforePlay(m6, env.dlc, "").build,
          "review: the next build makes his new file (his card never claimed it over his old file); nothing left to do");
      // the state a build stopped between Publish's two renames leaves: his card without inst / file_sig (Publish's first write), his old file in pac.old\, the new
      // one in stage\, no pac\ (his card's source is his own file: no other copy of his model exists)
      importer::PackState y;
      importer::LoadPack(m6, y, e);
      if (y.stars.size() == 2) y.stars[1].entrance_from = 110;  // (back: the file h_old was)
      importer::SavePack(m6, y, e);
      CopyTree(kdir / "pac", kdir / "stage" / "pac");
      fs::rename(kdir / "pac", kdir / "pac.old", e9);
      {
        sc::Card c;
        std::string ce;
        if (sc::ReadCard(Slash(kdir / "card.txt"), c, ce)) {
          c.inst = {};
          c.extra.erase(std::remove_if(c.extra.begin(), c.extra.end(), [](const auto& kv) { return kv.first == "file_sig"; }), c.extra.end());
          sc::WriteCard(Slash(kdir / "card.txt"), c, &ce);
        }
      }
      importer::PackState z;
      importer::LoadPack(m6, z, e);
      std::string w9;
      const bool healed = !e9 && Build(env, m6, z, env.dlc, "", &w9);
      Say(healed && ReadLib(m6, kb).ok && HashTree(kdir / "pac") == h_old && !fs::exists(kdir / "pac.old") && !importer::CheckBeforePlay(m6, env.dlc, "").build,
          "review: a build stopped between Publish's two renames: his old file is put back and his file made again from it (the same bytes as before)" + (w9.empty() ? "" : ": " + w9));
      Say(fs::is_regular_file(inbox), "review: a sources\\ file no card ever named (a pick not added yet) is kept by every build");
      fs::remove(inbox, e9);
    }
    {  // 3. the order: a frame rebuild whose commit fails (the frame in use) leaves every card claiming the frame ON DISK; the next build heals
      const fs::path ff = Root(m6) / "frame" / "superstars.txt";
      const uint32_t sig0 = wwe2k14::superstars_fit::ReadBuilt(ff).frame_sig;
      importer::PackState x;
      importer::LoadPack(m6, x, e);
      auto all_claim = [&](uint32_t sig) {
        int n = 0;
        for (const auto& s : x.stars) {
          sc::Card c;
          std::string ce;
          n += sc::ReadCard(Slash(lib6 / s.key / "card.txt"), c, ce) && (!c.inst.valid || c.inst.frame_sig == sig);
        }
        return n == int(x.stars.size());
      };
      bool failed = false;
      {
        std::ifstream hold(ff);  // (an open file in frame\: its rename fails, as with a running game)
        std::string w;
        failed = !importer::BuildPack(m6, env.gf, env.dlc, x, nullptr, w, "5");  // pack 5 off: another top, so the frame is rebuilt
      }
      const uint32_t sig_disk = wwe2k14::superstars_fit::ReadBuilt(ff).frame_sig;
      Say(failed && sig_disk == sig0 && all_claim(sig_disk), "review: a frame rebuild that cannot commit: no card claims a frame that is not on disk (every inst = the frame on disk)");
      Say(Build(env, m6, x, env.dlc, "5") && all_claim(wwe2k14::superstars_fit::ReadBuilt(ff).frame_sig) && wwe2k14::superstars_fit::ReadBuilt(ff).frame_sig != sig0 &&
              !importer::CheckBeforePlay(m6, env.dlc, "5").build,
          "review: the next build (frame free) commits the new frame and every card follows it; nothing left to do");
      Build(env, m6, x, env.dlc, "");  // (back to all DLC on)
    }
    {  // 5. folders that are not readable cards: listed as problems, the rest load and build, never touched
      CopyTree(lib6 / ka, lib6 / "000000F0");        // a hand copy keeping its card (key line = ka)
      CopyTree(lib6 / ka, lib6 / "Rev One - Copy");  // a hand copy under a name that is not a key
      fs::create_directories(lib6 / "000000F1", e8);
      std::ofstream(lib6 / "000000F1" / "card.txt", std::ios::binary | std::ios::trunc) << "not a card\n";
      const uint64_t h0 = HashTree(lib6 / "000000F0"), h1 = HashTree(lib6 / "Rev One - Copy"), h2 = HashTree(lib6 / "000000F1");
      importer::PackState x;
      const bool rd = importer::LoadPack(m6, x, e);
      const importer::PlayCheck pc = importer::CheckBeforePlay(m6, env.dlc, "");
      Say(rd && x.stars.size() == 2 && x.problems.size() == 3 && !pc.build && pc.why.find("cannot use") != std::string::npos,
          "review: 3 broken / copied card folders: LoadPack reads the 2 cards and lists 3 problems; CheckBeforePlay says '" + pc.why + "'");
      const bool sv = importer::SavePack(m6, x, e) && Build(env, m6, x, env.dlc, "");
      Say(sv && HashTree(lib6 / "000000F0") == h0 && HashTree(lib6 / "Rev One - Copy") == h1 && HashTree(lib6 / "000000F1") == h2,
          "review: SavePack and BuildPack go on; the three folders are byte-identical (never deleted or rewritten)");
    }
    fs::remove_all(fs::u8path(m6), e8);
  }
  // ---- 10. (S110 LIBSIZE-2 review) his file holds his source whole before the source goes; a source holding a directory the importer makes is refused, not stripped
  {
    std::string pac9;
    const std::string m9 = FreshMods(env, "lib_holds", &pac9);
    std::error_code e9;
    const fs::path keep = fs::u8path(env.mods_root) / "lib_holds_source.pac", keep2 = fs::u8path(env.mods_root) / "lib_holds_extra.pac",
                   twin1 = fs::u8path(env.mods_root) / "lib_holds_twin.pac";
    fs::copy_file(fs::u8path(pac9), keep, fs::copy_options::overwrite_existing, e9);
    io::Pac sp;
    std::string pe;
    const bool src_ok = io::ReadPac(UnxctdFile(fs::u8path(pac9)), sp, pe);
    auto write_pac = [](io::Pac p, const fs::path& to) {
      const Bytes b = io::WritePac(p);
      std::ofstream(to, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
      return !b.empty();
    };
    io::Pac withfx = sp;  // his pac + a GMEV directory: ModelOf takes GMEV out of a file it rebuilds from, so it would be lost once his sources\ copy is gone
    {
      io::Dir d;
      d.tag = "GMEV", d.kind = 0;
      io::Entry x;
      x.name = "0110", x.blob = Bytes(512, 7);
      d.ents.push_back(x);
      withfx.dirs.push_back(d);
    }
    const fs::path fxsrc = Root(m9) / "sources" / "with gmev.pac";
    const bool fx_ok = src_ok && write_pac(withfx, fxsrc);
    importer::PackState s9;
    s9.stars.push_back(MakeStar(pac9, 397, "Holds One"));
    s9.stars.push_back(MakeStar(Slash(fxsrc), 398, "Holds Fx"));
    const bool b9 = fx_ok && Build(env, m9, s9, env.dlc, "");
    importer::PackState l9;
    importer::LoadPack(m9, l9, e);
    std::string k1, k2;
    for (const auto& s : l9.stars) (s.name == "Holds One" ? k1 : k2) = s.key;
    bool said = false;
    for (const auto& mv : importer::LastBuildMoves()) said = said || mv.find("GMEV directory") != std::string::npos;
    Say(b9 && said && !k2.empty() && !ReadLib(m9, k2).ok && fs::is_regular_file(fxsrc),
        "review: a source holding a GMEV directory (a directory the importer makes) is refused with that reason, and his source is kept");
    const fs::path file = Root(m9) / "library" / k1 / "pac" / "ch" / ("ch" + std::to_string(sc::kRefSeat) + ".pac");
    Say(!k1.empty() && ReadLib(m9, k1).ok && !fs::exists(fs::u8path(pac9)) && importer::FileHoldsSource(Slash(keep), false, Slash(file)).empty() &&
            importer::FileHoldsSource(Slash(file), true, Slash(file)).empty(),
        "review: his file holds his source whole (against his source, and against itself as his own source)");
    {  // twins: one byte of his model changed in a copy of his file; a directory of his source that his file lacks
      Bytes fb = Read(file);
      io::Pac fp;
      size_t at = 0;
      if (io::ReadPac(fb, fp, pe))
        if (const io::Dir* d = DirOf(fp, "EMD "))
          for (const io::Entry& x : d->ents)
            if (x.alias < 0 && x.blob.size() > 300 && !at) at = size_t(io::kHeader) + size_t(x.sec) * io::kSector + 200;
      if (at && at < fb.size()) fb[at] ^= 0x5A;
      std::ofstream(twin1, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(fb.data()), std::streamsize(fb.size()));
      Twin("his file with one model byte changed", at > 0 && !importer::FileHoldsSource(Slash(keep), false, Slash(twin1)).empty());
      io::Pac extra = sp;
      io::Dir d;
      d.tag = "XTRA", d.kind = 0;
      io::Entry x;
      x.name = "0001", x.blob = Bytes(256, 3);
      d.ents.push_back(x);
      extra.dirs.push_back(d);
      Twin("his source holds a directory his file lacks", src_ok && write_pac(extra, keep2) && !importer::FileHoldsSource(Slash(keep2), false, Slash(file)).empty());
    }
    for (const fs::path& p : {keep, keep2, twin1}) fs::remove(p, e9);
    fs::remove_all(fs::u8path(m9), e9);
  }
  // ---- 8. (review) MOVN with a wrestler who HAS a name plate: his plate equals the disc's M<500 + e> byte for byte
  {
    io::Pac disc;
    std::string pe;
    const fs::path f = Find(fs::u8path(env.game), "pac\\evt\\evtmov2_2.pac");
    std::vector<int> plates;
    if (!f.empty() && io::ReadPac(UnxctdFile(f), disc, pe))
      if (io::Dir* d = disc.FindDir("MOVN"))
        for (const io::Entry& x : d->ents) {
          const std::string n = io::Trim(x.name);
          if (n.size() == 4 && n[0] == 'M' && x.alias < 0 && !x.blob.empty()) {
            const int e9 = std::atoi(n.c_str() + 1) - 500;
            if (e9 >= importer::kMinBase && e9 <= importer::kMaxBase) plates.push_back(e9);
          }
        }
    bool done = false;
    for (int pe9 : plates) {
      std::string pac;
      const std::string mp = FreshMods(env, "lib_plate", &pac);
      importer::PackState sp;
      sp.stars.push_back(MakeStar(pac, 397, "Plate Star", 107, pe9));
      std::string w;
      if (!importer::BuildPack(mp, env.gf, env.dlc, sp, nullptr, w, "")) continue;  // (a wrestler the entrance tables cannot copy: the next one)
      importer::PackState lp;
      importer::LoadPack(mp, lp, e);
      Lib l = lp.stars.empty() ? Lib() : ReadLib(mp, lp.stars[0].key);
      Bytes mine, want;
      char nm[8], wn[8];
      std::snprintf(nm, sizeof nm, "M%03d", 500 + 397), std::snprintf(wn, sizeof wn, "M%03d", 500 + pe9);
      // (S110 LIBSIZE-2) the plate is the game's own entry: his card holds an alias row from the disc's M<500 + e> to the reference identity's M997 (bind_cases.h resolves it to the bytes)
      std::snprintf(nm, sizeof nm, "M%03d", 500 + sc::kRefSeat);
      const bool got = l.ok && BlobOf(disc, "MOVN", wn, want) && !DirOf(l.pac, "MOVN");
      const bool has = got && HasAlias(l.rows, "MOVN", wn, nm);
      Say(has, "review: MOVN: a star with entrance wrestler " + std::to_string(pe9) + " holds the alias row " + wn + " -> " + nm + " (the disc's plate, " + std::to_string(want.size()) + " bytes, is not copied)");
      if (has) {
        sc::SeatRows t = l.rows;
        for (sc::Alias& a : t.alias)
          if (a.tag == "MOVN") a.game[3] ^= 1;
        Twin("MOVN: the alias names another plate", !HasAlias(t, "MOVN", wn, nm));
      }
      std::error_code e9;
      fs::remove_all(fs::u8path(mp), e9);
      done = true;
      break;
    }
    Say(done, "review: a wrestler with a name plate on the disc was found and built (" + std::to_string(plates.size()) + " plates in evtmov2_2.pac)");
  }
  std::printf("failures so far: %d\n", g_fail);
}
