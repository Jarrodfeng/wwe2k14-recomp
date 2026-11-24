// S110 SEAT-A (docs\research\s110\SEAT1.md sections 1 and 3): the per-character LIBRARY of the Superstar Importer.
//
//   <mods>\superstars\library\<KEY>\card.txt            the character as the importer's UI set him up + `inst` (what his files were made for)
//                                rows.bin               his table rows in file form (src\seat_card.h: DAT / PRO / DSBD / BIN / LSD / T102 / NAME)
//                                pac\ch\ch497.pac       ONE seat-free file (S110 LIBSIZE-2, library_file.h): EMD (+ attire aliases), EVP (entrance banks + victory), the UI's
//                                                       own SSFA/B/C (HD renders) and SDFA/B/C (SD renders, our tag); EVZ / GMEV / PCE6 / MOVN / base renders are rows.alias
//                                render_A.png ..        the UI's pictures (SavePack)
//        library\next_key, retired.txt, state.txt        the key counter (never lowered), the retired seats, the UI's enabled flag
//
// KEYS: 8 upper-case hex digits taken from the counter library\next_key (a hex number; the key given out is that number, then the counter is
// written as number + 1 BEFORE the card exists). The counter only grows and is lifted over every key found on disk, so a key is never given out
// twice by this code: not to two cards at once, and not again after its card is removed. (Only a deleted counter AND the removal of the highest
// cards could make a number come round again; the cards themselves are identified by the folder they are in, nothing else refers to a key.)
//
// UNITY INCLUDE: this header is included by importer_core.cpp, inside `namespace importer`, after the builder helpers (Builder, ModPac,
// FindBanks, RetargetEv, BaseRenders, ...): it uses them and is not a stand-alone header.
// What the old BuildPackImpl did for ALL characters per step (chEtc rows, models, entrance banks, ENEX rows, effects, plates, victory, renders)
// is done here for ONE character, into his own folder: adding / removing / changing a character writes only his folder.
#pragma once

namespace lib {

namespace sc = wwe2k14::seat_card;

// (S110 SEAT-A review) The argument for src\seat_card.h's ReadCard / WriteCard / ReadRows / WriteRows: the path itself when they take a
// std::filesystem::path (lane SEAT-B's seat_card.h: a UTF-8 std::string turned back into a path is read as the ANSI code page on Windows, so a
// mods folder outside it failed), else the UTF-8 string SEAT-A0's signatures take. Compiles against both; after the merge it hands the path.
inline auto SeatFile(const fs::path& p) {
  if constexpr (std::is_invocable_v<decltype(&sc::ReadCard), const fs::path&, sc::Card&, std::string&>) return fs::path(p);
  else return PathU8(p);
}

inline fs::path LibRoot(const fs::path& root) { return root / "library"; }
inline fs::path CardDir(const fs::path& root, const std::string& key) { return root / "library" / U8Path(key); }

// ---------------------------------------------------------------- card <-> Superstar
inline std::string OneLine(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r') c = ' ';
  return s;
}
// A source pac under the superstars folder is stored relative to it ("sources\X.pac"); one elsewhere as it is.
inline std::string SourceRel(const fs::path& root, const std::string& abs) {
  std::string r = Lower(PathU8(root)), a = abs;
  for (char& c : r)
    if (c == '/') c = '\\';
  std::string la = Lower(a);
  for (char& c : la)
    if (c == '/') c = '\\';
  if (!r.empty() && r.back() != '\\') r += '\\';
  if (la.size() > r.size() && la.compare(0, r.size(), r) == 0) {
    std::string rel = a.substr(r.size());
    for (char& c : rel)
      if (c == '/') c = '\\';
    return rel;
  }
  return a;
}
inline std::string SourceAbs(const fs::path& root, const std::string& rel) {
  if (rel.empty()) return rel;
  std::error_code ec;
  if (fs::path(U8Path(rel)).is_absolute() || (rel.size() > 1 && rel[1] == ':')) return rel;
  return PathU8(Under(root, rel));
}

inline sc::Card CardOf(const Superstar& s, const fs::path& root) {
  sc::Card c;
  c.key = s.key;
  c.name = OneLine(s.name), c.short_name = OneLine(s.short_name);
  c.base = std::clamp(s.base_id, 0, 9999), c.entrance = std::clamp(s.entrance_from, -1, 9999);
  c.copy_ratings = s.copy_ratings;
  c.source = OneLine(SourceRel(root, s.source_pac));
  c.seat = s.slot >= 0 && s.slot <= 9999 ? s.slot : -1;
  c.tron_from = s.tron_from > 0 ? std::min(s.tron_from, 9999) : 0;
  c.tron_slot = s.tron_slot > 0 ? std::min(s.tron_slot, 9999) : 0;
  for (int k = 0; k < 3; ++k) c.render[k] = !s.render_rgba[k].empty() && s.render_w[k] > 0 && s.render_h[k] > 0;
  return c;
}
// What a change of means "his files must be made again": everything but his seat, key, inst and the unknown lines.
inline bool SameContent(const sc::Card& a, const sc::Card& b) {
  return a.name == b.name && a.short_name == b.short_name && a.base == b.base && a.entrance == b.entrance && a.copy_ratings == b.copy_ratings &&
         a.source == b.source && a.tron_from == b.tron_from && a.tron_slot == b.tron_slot && a.render[0] == b.render[0] && a.render[1] == b.render[1] &&
         a.render[2] == b.render[2];
}
inline fs::path RenderPng(const fs::path& root, const std::string& key, int k) { return CardDir(root, key) / (std::string("render_") + char('A' + k) + ".png"); }

inline void FillSuperstar(const fs::path& root, const sc::Card& c, Superstar& s, bool portraits) {
  s = {};
  s.key = c.key, s.slot = c.seat, s.name = c.name, s.short_name = c.short_name, s.base_id = c.base, s.entrance_from = c.entrance;
  s.tron_from = c.tron_from, s.tron_slot = c.tron_slot, s.copy_ratings = c.copy_ratings;
  s.source_pac = ResolveSource(root, SourceAbs(root, c.source));
  for (int k = 0; k < 3 && portraits; ++k) {
    Bytes f;
    int w = 0, h = 0;
    if (c.render[k] && ReadAll(RenderPng(root, c.key, k), f, 64 << 20) && png::Read(f, s.render_rgba[k], w, h)) s.render_w[k] = w, s.render_h[k] = h;
    else s.render_rgba[k].clear();
  }
}

// ---------------------------------------------------------------- the library folder
// Every card on disk, by key. A folder with no card.txt is not a card (left alone). (S110 SEAT-A review) A folder whose card cannot be read, says
// another key, or is not named by a key (a folder copied by hand) is a PROBLEM: left out of the list with its reason in `problems`, the others
// load; nothing of ours ever deletes or rewrites it (SaveCards only touches the folders this returns). false only when the folder cannot be listed.
inline bool ReadAllCards(const fs::path& root, std::map<std::string, sc::Card>& out, std::string& err, std::vector<std::string>* problems = nullptr) {
  out.clear();
  std::error_code ec;
  const fs::path lib = LibRoot(root);
  if (!fs::is_directory(lib, ec)) return true;
  auto problem = [&](const std::string& p) {
    if (problems) problems->push_back(p);
  };
  std::error_code list_ec;
  for (const auto& e : fs::directory_iterator(lib, list_ec)) {
    const std::string key = PathU8(e.path().filename());
    if (!e.is_directory(ec) || !fs::is_regular_file(e.path() / "card.txt", ec)) continue;
    if (!sc::ValidKey(key)) {
      // (S110 SEAT-C review: these sentences reach the player as they are: the importer's LIBRARY PROBLEMS, the launchers' line, the build's notes)
      problem("The folder library\\" + key + " was not made by the importer (a copy made by hand?). It was left out; nothing in it was changed.");
      continue;
    }
    sc::Card c;
    std::string why;
    if (!sc::ReadCard(SeatFile(e.path() / "card.txt"), c, why)) {
      problem("The folder library\\" + key + " cannot be read (" + why + "). It was left out; nothing in it was changed.");
      continue;
    }
    if (c.key != key) {
      problem("The folder library\\" + key + " holds the superstar of folder " + c.key + " (a copy made by hand?). It was left out; nothing in it was changed.");
      continue;
    }
    out[key] = std::move(c);
  }
  if (list_ec) return err = "the library folder cannot be read: " + list_ec.message(), false;
  return true;
}
inline bool LibraryExists(const fs::path& root) {
  std::error_code ec;
  return fs::is_directory(LibRoot(root), ec);
}
inline std::vector<int> ReadIntFile(const fs::path& p) {
  std::vector<int> v;
  Bytes b;
  if (!ReadAll(p, b, 1 << 20)) return v;
  std::istringstream in(std::string(b.begin(), b.end()));
  for (int x; in >> x;) v.push_back(x);
  return v;
}
inline std::vector<int> ReadRetired(const fs::path& root) { return ReadIntFile(LibRoot(root) / "retired.txt"); }
inline bool ReadEnabled(const fs::path& root) {
  const std::vector<int> v = ReadIntFile(LibRoot(root) / "state.txt");  // "1" / "0"
  return v.empty() || v[0] != 0;
}
// (S110 SEAT-A review) the library's side files (retired.txt, state.txt, next_key) are written aside and renamed over, as pack.json was: a
// crash mid-write never leaves a half file (a truncated retired.txt would hand a retired seat, a save's old row, to a new import)
inline bool WriteTextAtomic(const fs::path& p, const std::string& text) {
  const fs::path part = p.parent_path() / (p.filename().native() + fs::path(".part").native());
  if (!WriteText(part, text)) return false;
  std::error_code ec;
  fs::rename(part, p, ec);
  if (ec) {
    fs::remove(p, ec);
    fs::rename(part, p, ec);
  }
  if (ec) fs::remove(part, ec);
  return !ec;
}
inline bool WriteIfChanged(const fs::path& p, const std::string& text) {
  Bytes old;
  if (ReadAll(p, old, 1 << 24) && std::string(old.begin(), old.end()) == text) return true;
  return WriteTextAtomic(p, text);
}

// The next key (see the top of this file): never one on disk, never a smaller number than the counter.
inline std::string NewKey(const fs::path& root, std::string& err) {
  const fs::path lib = LibRoot(root);
  std::error_code ec;
  fs::create_directories(lib, ec);
  uint64_t next = 1;
  Bytes b;
  if (ReadAll(lib / "next_key", b, 64)) next = std::max<uint64_t>(next, std::strtoull(std::string(b.begin(), b.end()).c_str(), nullptr, 16));
  for (const auto& e : fs::directory_iterator(lib, ec)) {
    const std::string key = PathU8(e.path().filename());
    if (sc::ValidKey(key)) next = std::max<uint64_t>(next, std::strtoull(key.c_str(), nullptr, 16) + 1);
  }
  if (next >= 0xFFFFFFFFull) return err = "the library has used every key", std::string();
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08X", unsigned(next));
  char nb[16];
  std::snprintf(nb, sizeof nb, "%08X", unsigned(next + 1));
  if (!WriteTextAtomic(lib / "next_key", nb)) return err = "cannot write the key counter", std::string();
  return buf;
}

// Makes the library hold exactly `st`: one folder per character, written only where something changed (card text, render pictures).
// A card whose content changed loses its `inst` (he is instantiated again by the next build); a removed card's folder goes, and its seat is
// retired when his files were ever made for it. keys_out[i] = the key of st.stars[i].
inline bool SaveCards(const fs::path& root, const PackState& st, std::vector<std::string>* keys_out, std::string& err) {
  std::error_code ec;
  std::map<std::string, sc::Card> existing;
  if (!ReadAllCards(root, existing, err)) return false;
  fs::create_directories(LibRoot(root), ec);
  const size_t n = st.stars.size();
  std::vector<std::string> keys(n);
  std::set<std::string> claimed;
  for (size_t i = 0; i < n; ++i) {  // 1. a key a stored card has
    const std::string& k = st.stars[i].key;
    if (existing.count(k) && claimed.insert(k).second) keys[i] = k;
  }
  for (size_t i = 0; i < n; ++i) {  // 2. a key the caller names that has no folder yet (and is not claimed)
    const std::string& k = st.stars[i].key;
    if (keys[i].empty() && sc::ValidKey(k) && !existing.count(k) && claimed.insert(k).second) keys[i] = k;
  }
  for (size_t i = 0; i < n; ++i) {  // 3. a keyless entry that holds a seat a stored card holds: that card
    if (!keys[i].empty() || st.stars[i].slot < 0) continue;
    for (const auto& [k, c] : existing)
      if (c.seat == st.stars[i].slot && !claimed.count(k)) {
        claimed.insert(k), keys[i] = k;
        break;
      }
  }
  for (size_t i = 0; i < n; ++i) {  // 4. new
    if (!keys[i].empty()) continue;
    keys[i] = NewKey(root, err);
    if (keys[i].empty()) return false;
    claimed.insert(keys[i]);
  }
  std::vector<int> built_before, new_seats;
  for (const auto& [k, c] : existing)
    if (c.inst.valid && c.seat >= 0) built_before.push_back(c.seat);  // (S110 LIBSIZE-2: inst is the reference identity for every card: the seat he holds)
  // (review) an old pack not migrated yet: its superstars.txt `star` slots were built too (a save may hold their rows), as the old WriteManifest
  // read them; the file leaves <mods>\superstars with the migration (the frame's own file is frame\superstars.txt)
  for (const auto& kv : wwe2k14::superstars_fit::ReadBuilt(root / "superstars.txt").music) built_before.push_back(kv.first);
  for (size_t i = 0; i < n; ++i) {
    const Superstar& s = st.stars[i];
    sc::Card c = CardOf(s, root);
    c.key = keys[i];
    if (c.seat >= 0) new_seats.push_back(c.seat);
    bool changed = false;
    const auto old = existing.find(c.key);
    if (old != existing.end()) {
      c.inst = old->second.inst, c.source_sig = old->second.source_sig, c.extra = old->second.extra;
      if (!SameContent(old->second, c)) changed = true;
    } else {
      changed = true;
    }
    for (int k = 0; k < 3; ++k) {
      const fs::path f = RenderPng(root, c.key, k);
      if (c.render[k]) {
        const Bytes png = png::Write(s.render_rgba[k], s.render_w[k], s.render_h[k]);
        if (png.empty()) return err = "could not make the picture of " + s.name, false;
        Bytes was;
        if (!ReadAll(f, was, 64 << 20) || was != png) {
          if (!WriteAll(f, png)) return err = "could not write " + PathU8(f), false;
          changed = true;
        }
      } else if (fs::exists(f, ec)) {
        fs::remove(f, ec);
        changed = true;
      }
    }
    if (changed) c.inst = {}, c.source_sig = 0;
    std::string why;
    if (old == existing.end() || sc::CardText(old->second) != sc::CardText(c)) {
      fs::create_directories(CardDir(root, c.key), ec);
      if (!sc::WriteCard(SeatFile(CardDir(root, c.key) / "card.txt"), c, &why)) return err = why, false;
    }
  }
  std::vector<int> retired = st.retired;
  {
    const std::vector<int> disk = ReadRetired(root);
    retired.insert(retired.end(), disk.begin(), disk.end());
  }
  retired = importer::slots::MergeRetired(retired, built_before, new_seats);
  for (const auto& [k, c] : existing)
    if (!claimed.count(k)) fs::remove_all(CardDir(root, k), ec);
  std::string rt;
  for (int s : retired) rt += std::to_string(s) + " ";
  if (!WriteIfChanged(LibRoot(root) / "retired.txt", rt)) return err = "could not write retired.txt", false;
  if (!WriteIfChanged(LibRoot(root) / "state.txt", st.enabled ? "1\n" : "0\n")) return err = "could not write state.txt", false;
  if (keys_out) *keys_out = keys;
  return true;
}

// ---------------------------------------------------------------- instantiation
// Everything instantiating reads from the player's files, read ONCE per build and shared by every card.
struct Ctx {
  Builder& b;
  explicit Ctx(Builder& bb) : b(bb) {}
  // the top pack's chEtc (CHAR DAT decoded / DSBD raw / PRO decoded, by roster id) and the next free rank
  std::map<uint32_t, Bytes> dat, dsbd, pro;
  int base_rank = 0;
  bool chetc_loaded = false;
  // the top pack's /ENEX/BIN + /ENEX/LSD (a pac holding only those two entries) and its row counts
  io::Pac enex;
  bool enex_loaded = false;
  size_t n_leaf[6] = {};
  std::map<uint32_t, size_t> n_list;
  std::map<int, BankSrc> banks, wins;
  std::set<int> banks_asked, wins_asked;
  std::map<std::string, std::unique_ptr<io::Pac>> pacs;  // parsed source pacs (effects, plates, base renders), by key
  std::map<int, std::set<int>> base_attires;

  bool LoadChEtc(std::string& err) {
    if (chetc_loaded) return true;
    Bytes d;
    io::Pac p;
    std::string e;
    if (!b.ReadCumulative("pac\\ch\\chEtc", d) || !io::ReadPac(d, p, e)) return err = "cannot read " + b.CumulativeName("pac\\ch\\chEtc") + " of " + b.BaseName(), false;
    io::Entry *dt = p.Find("CHAR", "DAT "), *ds = p.Find("CHAR", "DSBD"), *pr = p.Find("CHAR", "PRO ");
    std::vector<io::Leaf> ldat, ldsbd, lpro;
    if (!dt || !ds || !pr || !io::ReadPach(dt->blob, ldat) || !io::ReadPach(ds->blob, ldsbd) || !io::ReadPach(pr->blob, lpro))
      return err = "the chEtc of " + b.BaseName() + " is not as expected (CHAR DAT / DSBD / PRO)", false;
    base_rank = 0;
    for (const io::Leaf& l : ldat) {
      Bytes r = io::Decode(l.raw);
      if (r.size() == 208 && l.id >= 100 && l.id < 500 && r[kRecHead + 16] == 1) base_rank = std::max<int>(base_rank, io::L16(&r[kRecHead + 2]));
      dat[l.id] = std::move(r);
    }
    for (const io::Leaf& l : ldsbd) dsbd[l.id] = l.raw;
    for (const io::Leaf& l : lpro) pro[l.id] = io::Decode(l.raw);
    return chetc_loaded = true;
  }
  bool LoadEnex(std::string& err) {
    if (enex_loaded) return true;
    Bytes d;
    io::Pac p;
    std::string e;
    if (!b.ReadCumulative("pac\\DLC", d) || !io::ReadPac(d, p, e)) return err = "cannot read " + b.CumulativeName("pac\\DLC") + " of " + b.BaseName(), false;
    io::Dir* en = p.FindDir("ENEX");
    if (!en) return err = "the DLC pac has no ENEX directory", false;
    io::Dir mini;
    mini.tag = en->tag, mini.kind = en->kind;
    for (const char* name : {"BIN ", "LSD "})
      for (const io::Entry& x : en->ents)
        if (x.name == name) {
          io::Entry y;
          y.name = x.name, y.blob = io::Blob(*en, x);
          mini.ents.push_back(std::move(y));
          break;
        }
    enex = {};
    enex.dirs.push_back(std::move(mini));
    enex_rows::Table t;
    if (!t.Open(enex, "BIN ")) return err = "the DLC pac has no ENEX/BIN table", false;
    for (uint32_t k = 0; k <= 5; ++k) n_leaf[k] = t.data.count(k) && t.data[k].size() >= 2 ? io::L16(&t.data[k][0]) : 0;
    enex_rows::Table l;
    if (!l.Open(enex, "LSD ")) return err = "the DLC pac has no ENEX/LSD table", false;
    for (auto& [id, m] : l.data) n_list[id] = m.size() >= 2 ? io::L16(&m[0]) : 0;
    return enex_loaded = true;
  }
  // The entrance / victory banks of these entrance banks / wrestlers, searched once (the disc's pacs are large: one scan for all of them).
  void PrimeBanks(const std::set<int>& want_banks, const std::set<int>& want_wrestlers) {
    std::set<int> nb, nw;
    for (int v : want_banks)
      if (banks_asked.insert(v).second) nb.insert(v);
    for (int v : want_wrestlers)
      if (wins_asked.insert(v).second) nw.insert(v);
    if (!nb.empty())
      for (auto& [id, x] : FindBanks(b, nb)) banks.emplace(id, std::move(x));
    if (!nw.empty())
      for (auto& [id, x] : FindBanks(b, nw, /*victory=*/true)) wins.emplace(id, std::move(x));
  }
  // One scan of the disc's / packs' entrance pacs for all of these cards (the numbers of their entrance wrestlers' banks come from the chEtc rows).
  void PrimeFor(const std::vector<const Superstar*>& stars) {
    std::string err;
    if (!LoadChEtc(err)) return;
    std::set<int> bs, ws;
    for (const Superstar* s : stars) {
      const int e = s->entrance_from >= 0 ? s->entrance_from : s->base_id;
      const auto it = pro.find(uint32_t(e));
      if (it != pro.end() && asset_ids::SourceBank(it->second) > 0) bs.insert(asset_ids::SourceBank(it->second));
      ws.insert(e);
    }
    PrimeBanks(bs, ws);
  }
  // The wrestler's entry in the first of `where` that holds it (parsed pacs are kept).
  const io::Entry* FindIn(const std::vector<entrance_fx::Src>& where, const std::string& section, const std::string& want) {
    for (const entrance_fx::Src& src : where) {
      auto it = pacs.find(src.key);
      if (it == pacs.end()) {
        Bytes d;
        auto p = std::make_unique<io::Pac>();
        std::string e;
        it = pacs.emplace(src.key, src.read(d) && io::ReadPac(d, *p, e) ? std::move(p) : nullptr).first;
      }
      if (!it->second) continue;
      io::Dir* sd = it->second->FindDir(section);
      if (!sd) continue;
      for (const io::Entry& x : sd->ents)
        if (x.alias < 0 && io::Trim(x.name) == want && !x.blob.empty()) return &x;
    }
    return nullptr;
  }
};

// Where an effects / plate pac of wrestler `e` can be read from: his own pack, the other packs (highest first), then the disc.
inline std::vector<entrance_fx::Src> FxSources(Builder& b, int e, const std::string& stem) {
  std::vector<entrance_fx::Src> v;
  std::vector<const RealPack*> order;
  const RealPack* own = b.PackOfChar(e);
  if (own) order.push_back(own);
  for (auto it = b.packs.rbegin(); it != b.packs.rend(); ++it)
    if (&*it != own) order.push_back(&*it);
  for (const RealPack* rp : order) {
    const std::string rel = stem + "_dlc" + rp->Pp() + ".pac";
    if (rp->Has(rel)) v.push_back({"pack " + std::to_string(rp->id) + " " + rel, [&b, rp, rel](Bytes& d) { return b.ReadPackFile(*rp, rel, d); }});
  }
  v.push_back({"disc " + stem, [&b, stem](Bytes& d) { return b.ReadGame(stem + ".pac", d); }});
  return v;
}

// (S110 LIBSIZE-2) The reference identity's ids: every library character is made for seat kRefSeat / bank kRefBank (his bank number is the frame's, given by the game at a bind).
inline asset_ids::AssetIds CanonIds() {
  asset_ids::AssetIds a;
  a.slot = sc::kRefSeat, a.bank = sc::kRefBank, a.victory = 6000 + sc::kRefSeat, a.plate = 500 + sc::kRefSeat, a.own_plate = true;
  return a;
}

// chEtc rows of ONE character (the per-star half of the old PatchChEtc): DAT 208, DSBD 80, PRO 1276, in file form. Fills s.src_bank / s.movie.
inline bool StarRows(Ctx& c, Superstar& s, int rank, sc::SeatRows& r, std::string& err) {
  Builder& b = c.b;
  if (!c.LoadChEtc(err)) return false;
  const uint32_t src = uint32_t(s.base_id), slot = uint32_t(s.slot);
  const auto rit = c.dat.find(src);
  const auto dit = c.dsbd.find(src);
  const auto pit = c.pro.find(src);
  if (rit == c.dat.end() || dit == c.dsbd.end() || pit == c.pro.end() || rit->second.size() != 208 || io::L16(&rit->second[kRecHead]) != src)
    return err = "base wrestler " + std::to_string(src) + " has no usable roster row", false;
  if (!b.top && rit->second[kRecHead + 16] != 1)  // (no DLC: a DLC wrestler's row exists on the disc but he is not in this game)
    return err = "base wrestler " + std::to_string(src) + " comes with DLC the game does not get now (USE MY DLC is off, or you have no "
                 "DLC): switch USE MY DLC on, or pick another base wrestler in the Superstar Importer", false;
  const uint16_t att_lo = uint16_t(src * 100), att_hi = uint16_t(src * 100 + 99);
  auto att = [&](uint16_t v) { return uint16_t(v - att_lo + slot * 100); };
  Bytes rec = rit->second;
  uint8_t* R = &rec[kRecHead];
  // (S110 SEAT3-R) every word below that holds the seat, a key, a text id, the pack code or the rank is written AND recorded by relo::Rec::Put (rows.bin's
  // RELO block): the bind moves exactly these words. A word kept from the base row (a stock key that merely falls in some seat's range) is not recorded.
  relo::Rec rel;
  using RK = relo::sc::RelKind;
  for (int off : {0, 4, 200}) rel.Put(rec, "DAT ", 0, kRecHead + size_t(off), RK::Seat16, 0, slot);
  rel.Put(rec, "DAT ", 0, kRecHead + 2, RK::Rank16, 0, uint32_t(rank));
  for (int fam = 0; fam < 3; ++fam) rel.Put(rec, "DAT ", 0, kRecHead + 6 + 2 * size_t(fam), RK::Text16, fam, 60000 + 2000 * uint32_t(fam) + slot);
  io::W32(R + 12, 99999);  // no Twitter text (the base's would show his handle): seat-free, not recorded
  R[16] = 1;                // roster active
  rel.Put(rec, "DAT ", 0, kRecHead + 17, RK::Pack8, 0, uint32_t(b.pack_id));  // pack code = our catalog pack id (availability through the DLC manager)
  if (!s.copy_ratings) std::fill(R + 32, R + 46, uint8_t(75));
  std::string atts;
  for (int off = 116; off < 200; off += 2) {
    const uint16_t v = io::L16(R + off);
    if (v >= att_lo && v <= att_hi) rel.Put(rec, "DAT ", 0, kRecHead + size_t(off), RK::Key16, 0, att(v)), atts += " " + std::to_string(att(v));
  }
  // DSBD: raw 80-byte rows, the attire id list first
  Bytes ds = dit->second;
  if (ds.size() != sc::kDsbdSize) return err = "base wrestler " + std::to_string(src) + ": his DSBD row is " + std::to_string(ds.size()) + " bytes, not 80", false;
  for (size_t off = 0; off + 1 < ds.size(); off += 2) {
    const uint16_t v = io::L16(&ds[off]);
    if (v >= att_lo && v <= att_hi) rel.Put(ds, "DSBD", 0, off, RK::Key16, 0, att(v));
  }
  // PRO (1276 bytes): the character's own id in the u16 slots +576 .. +602
  Bytes pr = pit->second;
  if (pr.size() != sc::kProSize) return err = "base wrestler " + std::to_string(src) + ": profile row is " + std::to_string(pr.size()) + " bytes, not 1276", false;
  // (S110 ENTMOV) file +578 / +586 / +594 / +602 are ASSET REFERENCES: the movie id and the mini-tron / wall id stay the ENTRANCE wrestler's
  const uint32_t ent = uint32_t(s.entrance_from >= 0 ? s.entrance_from : s.base_id);
  Bytes pe;
  if (ent != src) {
    const auto le = c.pro.find(ent);
    if (le != c.pro.end()) pe = le->second;
    if (pe.size() < 604) return err = "entrance wrestler " + std::to_string(ent) + ": profile row too short", false;
  }
  // (S110 TRONSLOT) the movie word (+578 default / +594 current): a custom slot's number, else another wrestler's OWN movie word, else the
  // entrance wrestler's. The mini-tron / wall words (+586 / +602) stay the entrance wrestler's.
  if (s.tron_slot > 0 && !wwe2k14::tron_slots_core::IsSlotId(s.tron_slot))
    return err = s.name + ": custom tron " + std::to_string(s.tron_slot) + " is not a custom tron number (823-843, 845-899, 910-949)", false;
  Bytes pt;
  if (s.tron_slot <= 0 && s.tron_from > 0) {
    const auto lt = c.pro.find(uint32_t(s.tron_from));
    if (lt != c.pro.end()) pt = lt->second;
    if (pt.size() < 604) return err = s.name + ": the tron wrestler " + std::to_string(s.tron_from) + " has no profile row", false;
  }
  // (S110 BANKID) the entrance bank words (+580 / +596) hold his DEDICATED bank number, never his slot; the champion words are his entrance
  // wrestler's, a value equal to that wrestler's own bank becoming his bank.
  const Bytes& ef = ent != src ? pe : pr;
  const int ebank = asset_ids::SourceBank(ef);
  const asset_ids::AssetIds ids = CanonIds();  // (S110 LIBSIZE-2: the reference identity, whatever seat the card holds)
  if (ebank <= 0) return err = "entrance wrestler " + std::to_string(ent) + ": his profile row holds no entrance bank word", false;
  s.src_bank = ebank;
  int self = 0;
  for (size_t off = 576; off < 604; off += 2) {
    const bool ref = off == 578 || off == 586 || off == 594 || off == 602;
    if (ref) {
      if ((off == 578 || off == 594) && s.tron_slot > 0) io::W16(&pr[off], uint16_t(s.tron_slot));
      else if ((off == 578 || off == 594) && !pt.empty()) io::W16(&pr[off], io::L16(&pt[off]));
      else if (ent != src) io::W16(&pr[off], io::L16(&pe[off]));
    } else if (off == 580 || off == 596) {
      rel.Put(pr, "PRO ", 0, off, RK::Bank16, 0, uint32_t(ids.bank)), ++self;
    } else if (off == 582 || off == 584 || off == 598 || off == 600) {
      const uint16_t v = io::L16(&ef[off]);
      if (v == ebank) rel.Put(pr, "PRO ", 0, off, RK::Bank16, 0, uint32_t(ids.bank));  // (his entrance wrestler's own bank becomes his; another word stays, unrecorded)
      else io::W16(&pr[off], v);
    } else if (io::L16(&pr[off]) == src)  // (the music words +576 / +592 and +588 / +590: the slot)
      rel.Put(pr, "PRO ", 0, off, RK::Seat16, 0, slot), ++self;
  }
  if (const std::string why = asset_ids::SlotWordProblem(pr, int(slot), ids.bank); !why.empty()) return err = why, false;
  s.movie = io::L16(&pr[578]);
  std::copy(rec.begin(), rec.end(), r.dat.begin());
  std::copy(pr.begin(), pr.end(), r.pro.begin());
  std::copy(ds.begin(), ds.end(), r.dsbd.begin());
  r.seat = int(slot);
  r.relo = std::move(rel.list), r.has_relo = true;  // (EnexRowsOf appends the ENEX rows' words)
  b.Log("chEtc row " + std::to_string(slot) + " = row " + std::to_string(src) + ": rank " + std::to_string(rank) + ", texts " + std::to_string(60000 + slot) + "/" +
        std::to_string(62000 + slot) + "/" + std::to_string(64000 + slot) + ", pack code " + std::to_string(b.pack_id) + ", ratings " + (s.copy_ratings ? "copied" : "75") +
        ", attire ids" + atts + ", " + std::to_string(self) + " profile self ids, entrance bank " + std::to_string(ids.bank) + " (wrestler " + std::to_string(ent) +
        "'s bank word " + std::to_string(ebank) + "), movie " + std::to_string(s.movie) +
        (s.tron_slot > 0 ? " (custom tron " + std::to_string(s.tron_slot) + ")" : !pt.empty() ? " (wrestler " + std::to_string(s.tron_from) + "'s own movie word)" : " (wrestler " + std::to_string(ent) + "'s)"));
  return true;
}

// The ENEX rows of ONE character: the old PatchEnexBin / PatchEnexLsd run on a one-star list over a small pac holding the top's two tables, then
// the rows they appended are read back. BINP +0 (the row number) is the leaf's row count in the top's table (the number he gets when appended
// first); the game sets the real index when it appends. `banks` = the entrance banks written into his pac.
inline bool EnexRowsOf(Ctx& c, Superstar& s, const std::set<int>& banks, sc::SeatRows& r, std::string& err) {
  if (!c.LoadEnex(err)) return false;
  io::Pac pac = c.enex;
  const std::vector<Superstar> one = {s};
  std::map<int, std::set<int>> banks_of_star, parts;
  banks_of_star[0] = banks;
  std::vector<relo::Rec> recs(1);  // (S110 SEAT3-R) the words the two writers put in his rows
  const asset_ids::AssetIds canon = CanonIds();  // (S110 LIBSIZE-2) his rows are made for the reference identity, not for the frame's number of his seat
  if (!enex_rows::PatchEnexBin(*c.b.log, pac, one, banks_of_star, parts, err, &recs, &canon) || !enex_rows::PatchEnexLsd(*c.b.log, pac, one, parts, err, &recs, &canon)) return false;
  r.relo.insert(r.relo.end(), recs[0].list.begin(), recs[0].list.end());
  enex_rows::Table bin, lsd;
  if (!bin.Open(pac, "BIN ") || !lsd.Open(pac, "LSD ")) return err = "the ENEX tables did not read back", false;
  const Bytes& l5 = bin.data[5];
  const size_t n5 = io::L16(&l5[0]);
  bool got5 = false;
  for (size_t i = 0; i < n5 && !got5; ++i)
    if (io::L16(&l5[16 + 16 * i]) == uint32_t(s.slot)) std::copy(l5.begin() + ptrdiff_t(16 + 16 * i), l5.begin() + ptrdiff_t(32 + 16 * i), r.bin5.begin()), got5 = true;
  if (!got5) return err = "no template row for " + std::to_string(s.slot) + " was written", false;
  r.binp.clear(), r.lsd.clear();
  for (uint32_t k = 0; k < 5; ++k) {
    const Bytes& m = bin.data[k];
    const size_t n = io::L16(&m[0]);
    if (n != c.n_leaf[k] + 1) continue;
    sc::SeatRows::Binp x;
    x.k = uint8_t(k);
    std::copy(m.begin() + ptrdiff_t(16 + 20 * c.n_leaf[k]), m.begin() + ptrdiff_t(36 + 20 * c.n_leaf[k]), x.row.begin());
    r.binp.push_back(x);
  }
  for (uint32_t id : {2u, 3u, 17u, 16u, 8u, 9u, 10u, 11u}) {
    const auto it = lsd.data.find(id);
    if (it == lsd.data.end()) continue;
    const size_t n = io::L16(&it->second[0]), base = c.n_list[id];
    if (n != base + 1) continue;
    sc::SeatRows::Lsd x;
    x.list = uint8_t(id);
    std::copy(it->second.begin() + ptrdiff_t(2 + 8 * base), it->second.begin() + ptrdiff_t(10 + 8 * base), x.row.begin());
    r.lsd.push_back(x);
  }
  return true;
}

// S110 LIBSIZE-2: Instantiate, the file, its signature, the stage / Publish and the checks moved to library_file.h (one seat-free file per card).
#include "library_file.h"

}  // namespace lib
