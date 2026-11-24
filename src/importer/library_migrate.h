// S110 SEAT-A (docs\research\s110\SEAT1.md section 3 "Migration"): an old pack (pack.json + pack6\ + update\ + superstars.txt) becomes a library.
//
// The steps, and where they run:
//   1. one card per star (seat = his slot): BuildPack's first step, lib::SaveCards, from the list LoadPack read out of pack.json (LoadLegacy);
//   2. the frame, banks assigned over ALL seats with the old superstars.txt's recorded banks first (frame::Plan; an old file without `bank` lines
//      has none recorded, so the ordinal numbers apply, as a rebuild of the old pack would have given them);
//   3. every card instantiated (lib::Instantiate);
//   4. VERIFY (Verify below): the frame's catalog parses like the game (frame::Write does it), every rows.bin and pac reads back, and where the old
//      pack was built by the importer that records banks, each card's DAT / DSBD / PRO rows and EMD names equal the old pack's for his slot;
//   5. only then the old pack goes: Retire moves pack6\, update\, renders\, superstars.txt and pack.json aside (every move undone if one fails), the
//      frame is put in place, and Drop copies pack.json to pack.json.old; legacy.old\ is KEPT until the game has said "in N" on the library
//      (RemoveHolds, at a later build: S110 SEAT-A review).
// Nothing of the old pack is touched before step 5, and a failure up to then leaves it as it was (the build removes frame.building, the cards
// stay in the library for the next try).
//
// UNITY INCLUDE: importer_core.cpp, inside `namespace importer`, after frame_pack.h (it uses the JSON reader, ReadAll, Under, ...).
#pragma once

namespace migrate {

namespace sc = wwe2k14::seat_card;
using lib::CardDir;

// The old layout: a manifest or a built pack, and no frame yet. (A library without a frame is a migration that stopped: it continues.)
inline bool Legacy(const fs::path& root) {
  std::error_code ec;
  if (fs::is_directory(frame::Dir(root), ec)) return false;
  return fs::exists(root / "pack.json", ec) || fs::exists(root / "pack6", ec);
}

// The old pack.json read as a list (the old LoadPackImpl): slot -> seat, no keys. portraits = the PNGs of renders\<slot>_X.png.
inline bool LoadLegacy(const fs::path& root, PackState& out, bool portraits, std::string& err) {
  out = {};
  Bytes b;
  std::error_code ec;
  if (!fs::exists(root / "pack.json", ec)) return true;
  if (!ReadAll(root / "pack.json", b, 16 << 20)) return err = "cannot read pack.json", false;
  const std::string text(b.begin(), b.end());
  Json j;
  JsonParser p{text};
  if (!p.Val(j) || j.t != Json::kObj) return err = "pack.json is damaged", false;
  out.enabled = j.Bool("enabled", true);
  if (const Json* a = j.Get("stars"); a && a->t == Json::kArr)
    for (const Json& x : a->a) {
      Superstar s;
      s.slot = x.Int("slot", -1), s.name = x.Str("name"), s.short_name = x.Str("short_name");
      s.base_id = x.Int("base_id", 100), s.entrance_from = x.Int("entrance_from", -1);
      s.tron_from = x.Int("tron_from", 0), s.tron_slot = x.Int("tron_slot", 0);  // (an older pack.json: both 0 = his entrance wrestler's tron)
      s.copy_ratings = x.Bool("copy_ratings", true), s.source_pac = ResolveSource(root, x.Str("source_pac"));
      for (int k = 0; k < 3 && portraits; ++k) {
        Bytes f;
        int w = 0, h = 0;
        const fs::path png = root / "renders" / (std::to_string(s.slot) + "_" + char('A' + k) + ".png");
        if (ReadAll(png, f, 64 << 20) && png::Read(f, s.render_rgba[k], w, h)) s.render_w[k] = w, s.render_h[k] = h;
        else s.render_rgba[k].clear();
      }
      out.stars.push_back(std::move(s));
    }
  if (const Json* a = j.Get("retired"); a && a->t == Json::kArr)
    for (const Json& x : a->a)
      if (x.t == Json::kNum) out.retired.push_back(int(x.n));
  out.needs_build = true;  // (the old pack is not the library: the next build migrates it)
  return true;
}

// The old pack's recorded numbers (superstars.txt): banks, `was`, music; the slots it was built for.
inline wwe2k14::superstars_fit::Built LegacyBuilt(const fs::path& root) { return wwe2k14::superstars_fit::ReadBuilt(root / "superstars.txt"); }

// Text `id` of a string pac's name tables (the first table holding it: PatchStrings writes the same text into each language's), NULs cut.
inline bool OldText(const io::Pac& str, uint32_t id, std::string& out) {
  for (const io::Dir& d : str.dirs) {
    if (d.tag != "SDB ") continue;
    for (const io::Entry& e : d.ents) {
      std::vector<io::Leaf> leaves;
      if (!io::ReadPach(io::Blob(d, e), leaves)) continue;
      for (const io::Leaf& l : leaves) {
        if (l.raw.size() < 18 || std::memcmp(l.raw.data(), "ZLIB", 4) != 0) continue;
        const Bytes t = io::Decode(l.raw);
        if (t.size() < 8 || io::L32(&t[0]) != 0) continue;
        const uint32_t cnt = io::L32(&t[4]);
        if (cnt > 200000 || 8 + 12ull * cnt > t.size()) continue;
        for (uint32_t i = 0; i < cnt; ++i) {
          const uint8_t* r = &t[8 + 12 * size_t(i)];
          const uint32_t off = io::L32(r), len = io::L32(r + 4);
          if (io::L32(r + 8) != id || uint64_t(off) + len > t.size()) continue;
          out.assign(t.begin() + ptrdiff_t(off), t.begin() + ptrdiff_t(off + len));
          while (!out.empty() && out.back() == 0) out.pop_back();
          return true;
        }
      }
    }
  }
  return false;
}

// Step 4's comparison with the old pack (S110 SEAT-A review: a migration that compares nothing may not drop the old pack). Every card seated on a
// slot the old pack BUILT (superstars.txt `star`) whose settings are still pack.json's for that slot, while pack.json says the pack was built after
// its last save (needs_build false), must equal the old pack where the rules agree:
//   - DAT (rank masked: it follows the seat order), DSBD, the model (EMD names, aliases and bytes), the three names (string pac); and each old
//     portrait renders\<slot>_X.png has one in his folder (it may differ: retaken since);
// "His settings" = pack.json's name, short, base, entrance, ratings, tron and the source's file name.
//   - PRO: whole when the old file recorded a bank and the frame kept it; else (a pack from before BANKID, the bank was his slot, and the
//     asset words +576..+603 followed older rules: ENTMOV / BANKID / TRONSLOT) every byte outside +576..+603 and the music words +576 / +592.
// A card edited since (or a pack saved but not built) has old bytes that are not his any more: not compared, logged. `checked` = cards compared,
// `skipped` = old-built cards left out for that reason. "" = fine.
inline std::string Verify(const fs::path& root, const std::vector<sc::Card>& cards, const frame::Info& f, const std::string& pp,
                          const wwe2k14::superstars_fit::Built& old, int& checked, int* skipped = nullptr, const std::set<int>* recovered = nullptr) {
  checked = 0;
  if (skipped) *skipped = 0;
  std::error_code ec;
  std::vector<const sc::Card*> todo;
  for (const sc::Card& c : cards)
    if (c.inst.valid && c.seat >= 0 && old.music.count(c.seat)) todo.push_back(&c);  // (S110 LIBSIZE-2: inst is the reference identity: the old slot is the card's seat)
  if (todo.empty()) return {};  // (no card sits on a slot the old pack built: it holds nothing of theirs)
  // pack.json as the old importer left it (a save after the update writes the cards, not it)
  bool stale = true;
  std::map<int, const Json*> star_of;
  Json j;
  Bytes jb;
  std::string jt;
  if (ReadAll(root / "pack.json", jb, 16 << 20)) {
    jt.assign(jb.begin(), jb.end());
    JsonParser jp{jt};
    if (jp.Val(j) && j.t == Json::kObj) {
      stale = j.Bool("needs_build", false);
      if (const Json* a = j.Get("stars"); a && a->t == Json::kArr)
        for (const Json& x : a->a) star_of[x.Int("slot", -1)] = &x;
    }
  }
  auto same_settings = [&](const sc::Card& c) {
    const auto it = star_of.find(c.seat);
    if (stale || it == star_of.end()) return false;
    const Json& x = *it->second;
    const auto file_of = [](const std::string& p) {  // (the source by its file name: a moved mods folder changes the folder, not the file)
      const size_t cut = p.find_last_of("\\/");
      return Lower(cut == std::string::npos ? p : p.substr(cut + 1));
    };
    // (a source recovered from the old pack's own pac, `recovered`: his source by another name, the same model)
    return (file_of(x.Str("source_pac")) == file_of(c.source) || (recovered && recovered->count(c.seat))) && x.Str("name") == c.name && x.Str("short_name") == c.short_name && x.Int("base_id", 100) == c.base && x.Int("entrance_from", -1) == c.entrance &&
           x.Bool("copy_ratings", true) == c.copy_ratings && std::max(0, x.Int("tron_from", 0)) == c.tron_from && std::max(0, x.Int("tron_slot", 0)) == c.tron_slot;
  };
  std::vector<const sc::Card*> cmp;
  for (const sc::Card* c : todo) {
    if (same_settings(*c)) cmp.push_back(c);
    else if (skipped) ++*skipped;
  }
  if (cmp.empty()) return {};
  const fs::path old_ch = root / "pack6" / "pac" / "ch";
  fs::path old_etc;
  std::string old_pp;
  for (const auto& e : fs::directory_iterator(old_ch, ec)) {
    const std::string n = Lower(PathU8(e.path().filename()));
    if (n.rfind("chetc_dlc", 0) == 0 && n.size() == 9 + 2 + 4) old_etc = e.path(), old_pp = n.substr(9, 2);
  }
  if (old_etc.empty()) return "the old pack has no roster pac to compare with";
  io::Pac old_str;
  {
    Bytes sb;
    std::string se;
    if (!ReadAll(root / "pack6" / "pac" / ("string_dlc" + old_pp + ".pac"), sb) || !io::ReadPac(sb, old_str, se)) return "the old pack's string pac cannot be read";
  }
  Bytes d;
  io::Pac p;
  std::string e;
  if (!ReadAll(old_etc, d) || !io::ReadPac(d, p, e)) return "the old pack's roster pac cannot be read";
  std::vector<io::Leaf> ldat, ldsbd, lpro;
  io::Entry *dt = p.Find("CHAR", "DAT "), *ds = p.Find("CHAR", "DSBD"), *pr = p.Find("CHAR", "PRO ");
  if (!dt || !ds || !pr || !io::ReadPach(dt->blob, ldat) || !io::ReadPach(ds->blob, ldsbd) || !io::ReadPach(pr->blob, lpro)) return "the old pack's roster pac is not as expected";
  auto leaf = [](std::vector<io::Leaf>& v, uint32_t id) -> io::Leaf* {
    for (auto& l : v)
      if (l.id == id) return &l;
    return nullptr;
  };
  for (const sc::Card* cp : cmp) {
    const sc::Card& c = *cp;
    const int slot = c.seat;
    // (a slot the old pack recorded a bank for, which the frame kept: his whole PRO row is today's rules' too)
    const bool whole_pro = old.bank.count(slot) && f.bank.count(slot) && old.bank.at(slot) == f.bank.at(slot);
    sc::SeatRows ref_rows, rows;
    std::string why;
    if (!sc::ReadRows(lib::SeatFile(lib::StageDir(root, c.key) / "rows.bin"), ref_rows, why)) return c.name + ": " + why;  // (his new files: not published yet)
    // (S110 LIBSIZE-2) his rows are the reference identity's: re-targeted to the old slot the way the game's bind does it, then compared
    if (!f.bank.count(slot) || !f.rank.count(slot)) return c.name + ": the frame holds no seat " + std::to_string(slot);
    if (!wwe2k14::seat_bind::RetargetCard(ref_rows, sc::kRefSeat, sc::kRefBank, slot, f.bank.at(slot), f.pack_id, f.rank.at(slot), rows, why)) return c.name + ": " + why;
    io::Leaf *od = leaf(ldat, uint32_t(slot)), *os = leaf(ldsbd, uint32_t(slot)), *op = leaf(lpro, uint32_t(slot));
    if (!od || !os || !op) return c.name + ": the old pack has no roster row for slot " + std::to_string(slot);
    Bytes dat = io::Decode(od->raw);
    if (dat.size() != 208) return c.name + ": the old pack's DAT row is " + std::to_string(dat.size()) + " bytes";
    Bytes mine(rows.dat.begin(), rows.dat.end());
    dat[kRecHead + 2] = dat[kRecHead + 3] = mine[kRecHead + 2] = mine[kRecHead + 3] = 0;  // (the rank follows the seat order, not the old list's order)
    if (dat != mine) return c.name + ": his DAT row differs from the old pack's row for slot " + std::to_string(slot);
    if (os->raw != Bytes(rows.dsbd.begin(), rows.dsbd.end())) return c.name + ": his DSBD row differs from the old pack's row for slot " + std::to_string(slot);
    {
      Bytes op_b = io::Decode(op->raw), mp_b(rows.pro.begin(), rows.pro.end());
      if (op_b.size() != mp_b.size()) return c.name + ": the old pack's PRO row is " + std::to_string(op_b.size()) + " bytes";
      if (!whole_pro)
        for (size_t off = 576; off < 604; ++off)
          if (off != 576 && off != 577 && off != 592 && off != 593) op_b[off] = mp_b[off] = 0;  // (the asset words: older rules; the music words stay)
      if (op_b != mp_b) return c.name + ": his PRO row differs from the old pack's row for slot " + std::to_string(slot) + (whole_pro ? "" : " (outside the asset words)");
    }
    for (int k = 0; k < 3; ++k) {  // his names: the old pack's string pac
      std::string t;
      if (!OldText(old_str, uint32_t((k == 0 ? 60000 : k == 1 ? 62000 : 64000) + slot), t)) return c.name + ": the old pack has no name text " + std::to_string(60000 + 2000 * k + slot);
      if (t != rows.names[k]) return c.name + ": his name text " + std::to_string(60000 + 2000 * k + slot) + " '" + rows.names[k] + "' differs from the old pack's '" + t + "'";
    }
    for (int k = 0; k < 3; ++k) {  // the UI's portraits: an old renders\<slot>_X.png is in his card too (a picture retaken since may differ; none may be lost)
      Bytes a, b2;
      std::vector<uint8_t> pa, pb;
      int wa = 0, ha = 0, wb = 0, hb = 0;
      const bool had = ReadAll(root / "renders" / (std::to_string(slot) + "_" + char('A' + k) + ".png"), a, 64 << 20) && png::Read(a, pa, wa, ha);
      const bool has = c.render[k] && ReadAll(lib::RenderPng(root, c.key, k), b2, 64 << 20) && png::Read(b2, pb, wb, hb);
      if (had && !has) return c.name + ": his portrait " + char('A' + k) + " of the old pack is not in his library folder";
    }
    // the model: EMD names, aliases and bytes
    io::Pac oldp, newp;
    Bytes b1, b2;
    if (!ReadAll(old_ch / ("ch" + std::to_string(slot) + "_dlc" + old_pp + ".pac"), b1) || !io::ReadPac(b1, oldp, e)) return c.name + ": the old pack's model pac cannot be read";
    if (!ReadAll(Under(lib::StageDir(root, c.key), lib::CharRel()), b2) || !io::ReadPac(b2, newp, e)) return c.name + ": his model pac cannot be read";
    io::Dir *o = oldp.FindDir("EMD "), *n = newp.FindDir("EMD ");
    if (!o || !n || o->ents.size() != n->ents.size()) return c.name + ": his EMD list differs from the old pack's";
    for (size_t i = 0; i < o->ents.size(); ++i) {
      std::string nn = n->ents[i].name;  // (his file names the reference id 497: the old pack's slot)
      if (nn.size() == 8) wwe2k14::seat_bind::RetargetName(&nn[0], 8, sc::kRefSeat, slot);
      if (o->ents[i].name != nn || o->ents[i].alias != n->ents[i].alias || io::Blob(*o, o->ents[i]) != io::Blob(*n, n->ents[i]))
        return c.name + ": his EMD entry " + o->ents[i].name + " differs from the old pack's";
    }
    ++checked;
  }
  return {};
}

// Step 5. Moves the old pack's parts into legacy.old\ (undone on any failure); Drop deletes them (pack.json stays as pack.json.old).
inline bool Retire(const fs::path& root, std::string& err) {
  std::error_code ec;
  const fs::path hold = root / "legacy.old";
  if (fs::exists(hold, ec)) {  // (review) an earlier attempt's legacy.old may hold the only copy of something: kept, under a numbered name
    fs::path aside;
    for (int k = 2; aside.empty() || fs::exists(aside, ec); ++k) aside = root / ("legacy.old." + std::to_string(k));
    fs::rename(hold, aside, ec);
    if (ec) return err = "the folder legacy.old (an earlier migration's old pack) could not be moved aside: " + ec.message(), false;
  }
  fs::create_directories(hold, ec);
  std::vector<std::string> moved;
  for (const char* name : {"pack6", "update", "renders", "superstars.txt", "pack.json"}) {
    if (!fs::exists(root / name, ec)) continue;
    fs::rename(root / name, hold / name, ec);
    if (ec) {
      const std::string why = ec.message();
      for (const std::string& m : moved) fs::rename(hold / m, root / m, ec);
      fs::remove_all(hold, ec);
      return err = std::string("the old pack could not be moved aside (close the game first): ") + name + ": " + why, false;
    }
    moved.push_back(name);
  }
  return true;
}
inline void Restore(const fs::path& root) {
  std::error_code ec;
  const fs::path hold = root / "legacy.old";
  for (const char* name : {"pack6", "update", "renders", "superstars.txt", "pack.json"})
    if (fs::exists(hold / name, ec)) fs::rename(hold / name, root / name, ec);
  fs::remove_all(hold, ec);
}
// (review) legacy.old\ itself is KEPT (the whole old pack: 33 MB for two characters) until the game has booted on the frame and said "in N"
// (game_status.txt, removed by every build); RemoveHolds, at the start of a later build, then deletes it and any numbered one.
inline void Drop(const fs::path& root) {
  std::error_code ec;
  const fs::path hold = root / "legacy.old";
  fs::remove(root / "pack.json.old", ec);
  if (fs::exists(hold / "pack.json", ec)) fs::copy_file(hold / "pack.json", root / "pack.json.old", ec);
  fs::remove_all(root / "pack6.old", ec);
  fs::remove_all(root / "pack6.building", ec);
  fs::remove_all(root / "update.building", ec);
}
// legacy.old\ and legacy.old.N\ go once the game has reported "in N" on the library (the status file a build removes: so it was written by a boot
// after the migration) and no old pack is left to migrate. Returns how many folders were removed.
inline int RemoveHolds(const fs::path& root, const fs::path& mods_dir) {
  if (Legacy(root) || wwe2k14::superstars_fit::ReadStatus(mods_dir).rfind("in ", 0) != 0) return 0;
  std::error_code ec;
  int n = 0;
  std::vector<fs::path> holds;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    const std::string f = PathU8(e.path().filename());
    if (f == "legacy.old" || f.rfind("legacy.old.", 0) == 0) holds.push_back(e.path());
  }
  for (const fs::path& h : holds) {
    std::error_code e2;
    fs::remove_all(h, e2);
    n += !e2;
  }
  return n;
}

}  // namespace migrate
