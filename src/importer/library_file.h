// S110 LIBSIZE-2 (docs\research\s110\LIBSIZE.md section 3 "Recommended = B + alias rows (A1)", section 4, "Sites the build will touch" 6-8): ONE seat-free character
// file per card, made for the reference identity (seat_card.h kRefSeat 497 / kRefBank 9997).
//
//   library\<KEY>\pac\ch\ch497.pac   EMD (+ attire aliases) of his model, the source's other directories, EVP (entrance banks 9997 / 10000 + 1000 g + 9997, victory
//                                     6497) and the UI's own renders (SSFx HD, SDFx SD) -- nothing else. A seat or a frame change never writes it.
//   library\<KEY>\rows.bin           his rows for 497 / 9997 (RELO) + the ALIA block: the game's own EVZ / GMEV / PCE6 / MOVN (and the base's renders where he has none of
//                                     his own) are not copied, the game points the seat's names at them (src\seat_alias_core.h).
//   card.txt                         `file_form 2`, `file_sig` (what the file was made from: model, base, entrance, src_bank, renders), `model_sig`.
//
// The file is made again only when its file_sig changes; a frame change makes rows.bin only. After the file is in place the card's source IS the file (ModelOf
// strips what we made), and a sources\ file no card names any more is deleted.
//
// UNITY INCLUDE: library.h includes this file before it closes `namespace lib` (it uses Ctx, FxSources, StarRows, EnexRowsOf, SourceRel ... above it).
#pragma once

// ---------------------------------------------------------------- the reference identity
inline std::string CharRel() { return "pac\\ch\\ch" + std::to_string(sc::kRefSeat) + ".pac"; }  // (lower case: the phone's file system is case-sensitive)
inline fs::path CharFile(const fs::path& root, const std::string& key) { return Under(CardDir(root, key), CharRel()); }
// His own file as the card's `source` names it ("library\<KEY>\pac\ch\ch497.pac" under <mods>\superstars).
inline std::string OwnSourceRel(const std::string& key) { return "library\\" + key + "\\" + CharRel(); }
inline bool IsOwnSource(const fs::path& root, const std::string& key, const std::string& source_abs) {
  return !source_abs.empty() && Lower(SourceRel(root, source_abs)) == Lower(OwnSourceRel(key));
}
// (the card's extra lines: what the build recorded of his source and his file)
inline std::string SourceStat(const fs::path& p) {
  std::error_code ec;
  const uint64_t n = fs::file_size(p, ec);
  if (ec) return {};
  const auto t = fs::last_write_time(p, ec);
  if (ec) return {};
  return std::to_string(n) + " " + std::to_string(static_cast<long long>(t.time_since_epoch().count()));
}
inline std::string Extra(const sc::Card& c, const std::string& key) {
  for (const auto& kv : c.extra)
    if (kv.first == key) return kv.second;
  return {};
}
inline void SetExtra(sc::Card& c, const std::string& key, const std::string& value) {
  c.extra.erase(std::remove_if(c.extra.begin(), c.extra.end(), [&](const auto& kv) { return kv.first == key; }), c.extra.end());
  if (!value.empty()) c.extra.emplace_back(key, value);
}
// FNV-1a of a file (the source pac): 0 when it cannot be read.
inline uint32_t FileSig(const fs::path& p) {
  Bytes d;
  if (!ReadAll(p, d)) return 0;
  const uint32_t v = wwe2k14::dlc::Fnv32(d.data(), d.size());
  return v ? v : 1;
}
inline bool HaveRender(const Superstar& s, int k) {
  const int w = s.render_w[k], h = s.render_h[k];
  return !s.render_rgba[k].empty() && w > 0 && h > 0 && s.render_rgba[k].size() >= size_t(w) * size_t(h) * 4;
}

// ---------------------------------------------------------------- his file's model, and what a pac holds (header only)
// The directory tags the importer makes (or made, in the old per-seat form). ModelOf takes them out of his file, so a source may hold none of them (MakeFile refuses one).
inline const std::set<std::string>& OurTags() {
  static const std::set<std::string> t = {"EVP ", "EVZ ", "GMEV", "PCE6", "MOVN", "SSFA", "SSFB", "SSFC", "SDFA", "SDFB", "SDFC"};
  return t;
}
// The pac with the directories WE make taken out: the player's own model (and whatever else his source held). The result is a source the importer can build from.
inline Bytes ModelOf(const Bytes& file, std::string& why) {
  io::Pac p;
  std::string e;
  if (!io::ReadPac(file, p, e)) return why = "his file cannot be read (" + e + ")", Bytes();
  p.dirs.erase(std::remove_if(p.dirs.begin(), p.dirs.end(), [](const io::Dir& x) { return OurTags().count(x.tag) != 0; }), p.dirs.end());
  return io::WritePac(p);
}
// The row names of directory `tag` read from the first 0x4000 bytes of a plain pac (trimmed). false = not readable as a pac.
inline bool PacDirNames(const fs::path& path, const char* tag, std::vector<std::string>& names) {
  names.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  Bytes h(io::kHeader);
  f.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()));
  if (size_t(f.gcount()) < size_t(io::kSector) + 12 || (std::memcmp(h.data(), "EPAC", 4) != 0 && std::memcmp(h.data(), "EPK8", 4) != 0)) return false;
  const size_t end = std::min<size_t>(size_t(io::kSector) + io::L32(&h[4]), size_t(f.gcount()));
  size_t p = size_t(io::kSector);
  while (p + 12 <= end) {
    const std::string t(reinterpret_cast<const char*>(&h[p]), 4);
    const size_t words = io::L16(&h[p + 4]), kind = io::L16(&h[p + 6]), rs = kind == 1 ? 16 : 12;
    p += 12;
    if (kind > 1 || (words * 4) % rs || p + words * 4 > end) return false;
    if (t == tag)
      for (size_t i = 0; i < words * 4 / rs; ++i) names.push_back(io::Trim(std::string(reinterpret_cast<const char*>(&h[p + i * rs]), rs - 8)));
    p += words * 4;
  }
  return true;
}

// ---------------------------------------------------------------- the file's signature
inline uint32_t Fnv(const std::string& t) {
  const uint32_t v = wwe2k14::dlc::Fnv32(reinterpret_cast<const uint8_t*>(t.data()), t.size());
  return v ? v : 1;
}
// What the file is made from: his model (the source's FNV: model_sig), base, entrance wrestler, the entrance bank word of that wrestler (src_bank) and his own renders.
inline uint32_t FileSigOf(uint32_t model_sig, int base, int entrance, int src_bank, const uint32_t render[3]) {
  return Fnv("file2 " + sc::detail::Hex8(model_sig) + " base " + std::to_string(base) + " entrance " + std::to_string(entrance) + " src_bank " + std::to_string(src_bank) + " renders " +
             sc::detail::Hex8(render[0]) + " " + sc::detail::Hex8(render[1]) + " " + sc::detail::Hex8(render[2]));
}
inline uint32_t HexExtra(const sc::Card& c, const std::string& key) {
  uint32_t v = 0;
  for (const auto& kv : c.extra)
    if (kv.first == key && !sc::detail::Hex32(kv.second, v)) v = 0;
  return v;
}
inline void StampFile(sc::Card& c, uint32_t model_sig, uint32_t file_sig) {
  sc::SetFileForm(c, sc::kFileForm);
  SetExtra(c, "model_sig", sc::detail::Hex8(model_sig));
  SetExtra(c, "file_sig", sc::detail::Hex8(file_sig));
  c.source_sig = model_sig;
}

// ---------------------------------------------------------------- the alias rows (ALIA block of rows.bin)
// `evp` = the numbers of the EVP banks his file holds (canonical: 9997, 10000 + 1000 g + 9997, 6497). Every name is the CANONICAL one for `ours`; `game` the entrance
// wrestler's own entry in the game's files. (The game asks its node list where each one is; one it does not find gives no row.)
inline std::vector<sc::Alias> AliasesOf(const Superstar& s, const std::vector<int>& evp) {
  const asset_ids::AssetIds ids = CanonIds();
  const int e = s.entrance_from >= 0 ? s.entrance_from : s.base_id;
  std::vector<sc::Alias> v;
  auto mk = [&](const char* tag, uint8_t kind, std::string game, std::string ours) {
    sc::Alias a;
    a.tag = tag, a.kind = kind, a.game = std::move(game), a.ours = std::move(ours);
    v.push_back(std::move(a));
  };
  for (int n : evp) {
    int from = -1;
    if (n == ids.bank) from = s.src_bank;
    else if (n == ids.victory) from = 6000 + e;
    else
      for (int g = 0; g < 10; ++g)
        if (n == ids.PartBank(g)) from = 10000 + 1000 * g + s.src_bank;
    if (from >= 0) mk("EVZ ", 1, io::Pad(std::to_string(from), 8), io::Pad(std::to_string(n), 8));
  }
  mk("GMEV", 0, entrance_fx::BankName(s.src_bank), ids.FxName());
  mk("PCE6", 0, entrance_fx::BankName(s.src_bank), ids.FxName());
  mk("MOVN", 0, entrance_movies::PlateName(e), entrance_movies::PlateNo(ids.plate));
  for (int k = 0; k < 3; ++k)
    if (!HaveRender(s, k)) {  // a picture he gave none of: the base's own, from the game's menu pacs (HD or SD: the game has one of them mounted)
      char g[8], o[8];
      std::snprintf(g, sizeof g, "%04d", s.base_id), std::snprintf(o, sizeof o, "%04d", ids.slot);
      mk((std::string("SSF") + char('A' + k)).c_str(), 0, g, o);
    }
  return v;
}

// ---------------------------------------------------------------- his file (the pac)
// Reads his model and makes everything of his file we make. `own` = the source is his own file (ModelOf). On success `evp` = the numbers of the EVP banks written.
inline bool MakeFile(Ctx& c, Superstar& s, bool own, io::Pac& pac, std::vector<int>& evp, std::string& err) {
  Builder& b = c.b;
  const int slot = sc::kRefSeat, e = s.entrance_from >= 0 ? s.entrance_from : s.base_id;
  const asset_ids::AssetIds ids = CanonIds();
  evp.clear();
  // ---- the model: EMD 000<claimed>ak -> 000<497>ak; attires the base has and the mod lacks point at the mod's attire 0 (directory aliases)
  {
    if (!c.base_attires.count(s.base_id)) {
      std::set<int>& a = c.base_attires[s.base_id];
      Bytes d;
      io::Pac p;
      std::string x;
      const RealPack* rp = b.PackOfChar(s.base_id);
      const std::string rel = "pac\\ch\\ch" + std::to_string(s.base_id) + (rp ? "_dlc" + rp->Pp() : std::string()) + ".pac";
      if ((rp ? b.ReadPackFile(*rp, rel, d) : b.ReadGame(rel, d)) && io::ReadPac(d, p, x))
        if (io::Dir* emd = p.FindDir("EMD "))
          for (const io::Entry& y : emd->ents)
            if (IsEmdName(y.name) && std::atoi(y.name.substr(3, 3).c_str()) == s.base_id) a.insert(y.name[6] - '0');
    }
    Bytes d;
    if (!ReadAll(U8Path(s.source_pac), d, assets::char_mods::kMaxPac)) return err = s.name + ": cannot read " + s.source_pac, false;
    if (own) {
      std::string why;
      d = ModelOf(d, why);
      if (d.empty()) return err = s.name + ": " + why, false;
    }
    ModPac m;
    const std::string why = ParseModPac(std::move(d), m, nullptr);
    if (!why.empty()) return err = s.name + ": " + why, false;
    // (S110 LIBSIZE-2 review) his file becomes his only copy and ModelOf takes our tags out of it at the next rebuild: a source directory of one of those tags would be
    // lost then, so such a source is refused (as the old per-seat importer refused it: it added every one of them)
    if (!own)
      for (const io::Dir& x : m.pac.dirs)
        if (OurTags().count(x.tag)) return err = s.name + ": his source pac already has a " + io::Trim(x.tag) + " directory (a directory the importer makes)", false;
    pac = std::move(m.pac);
    pac.dirs.reserve(pac.dirs.size() + 24);  // (add_dir below hands out pointers into it)
    io::Dir* emd = pac.FindDir("EMD ");
    char from[8], to[8];
    std::snprintf(from, sizeof from, "000%03d", m.claimed), std::snprintf(to, sizeof to, "000%03d", slot);
    std::string renamed;
    for (io::Entry& x : emd->ents)
      if (x.name.compare(0, 6, from) == 0) x.name.replace(0, 6, to), renamed += " " + x.name;
    const auto& first = m.att.begin()->second;
    std::string aliased;
    for (int a : c.base_attires[s.base_id]) {
      if (m.att.count(a)) continue;
      for (const auto& [kind, idx] : first) {
        io::Entry al;
        al.name = std::string(to) + char('0' + a) + char('0' + kind);
        const int target = emd->ents[size_t(idx)].alias;  // (an alias of an alias points at the real entry)
        al.alias = target >= 0 ? target : idx;
        emd->ents.push_back(al);
        aliased += " " + al.name;
      }
    }
    b.Log("ch" + std::to_string(slot) + ": models" + renamed + (aliased.empty() ? "" : "; attire aliases" + aliased) + " (from " + s.source_pac + ", claimed " + std::to_string(m.claimed) + ")");
  }
  auto add_dir = [&](const char* tag, uint16_t kind) -> io::Dir* {
    if (pac.FindDir(tag)) return nullptr;
    io::Dir d;
    d.tag = tag, d.kind = kind;
    pac.dirs.push_back(std::move(d));
    return &pac.dirs.back();
  };
  // ---- entrance banks (the reference bank + the variants 10000 + 1000 G + bank) and the victory scene 6000 + 497: EVP only (the EVZ is the game's own: an alias row)
  {
    c.PrimeBanks({s.src_bank}, {e});
    std::vector<std::pair<int, Bytes>> out;
    std::string got;
    int actors_total = 0;
    for (int g = -1; g < 10; ++g) {
      const int from = g < 0 ? s.src_bank : 10000 + 1000 * g + s.src_bank, to = g < 0 ? ids.bank : ids.PartBank(g);
      const auto it = c.banks.find(from);
      if (it == c.banks.end()) continue;
      int actors = 0;
      Bytes n = RetargetEv(it->second.evp, e, to, slot, actors);
      if (n.empty()) continue;
      actors_total += actors;
      out.emplace_back(to, std::move(n));
      got += " " + std::to_string(to);
    }
    if (out.empty()) b.warnings.push_back(s.name + ": no entrance bank " + std::to_string(s.src_bank) + " found (entrances may not play)");
    else b.Log("entrance banks" + got + " = wrestler " + std::to_string(e) + "'s bank " + std::to_string(s.src_bank) + " (" + std::to_string(actors_total) + " actor model keys -> " + std::to_string(slot) + "xx)");
    {  // (S110 BANKID) no bank of ours is named by a slot number (the old scheme's collision with a real bank)
      std::set<int> names, mine;
      for (const auto& o : out) names.insert(o.first);
      for (const auto& [sl, bk] : asset_ids::Config().assigned) {
        mine.insert(bk);
        for (int k = 0; k < 10; ++k) mine.insert(10000 + 1000 * k + bk);
      }
      if (const std::string why = asset_ids::OutputProblem(names, slot, ids.bank, mine); !why.empty()) return err = s.name + ": " + why, false;
    }
    // (S110 WINNING) the victory scene: /EVP/6000 + wrestler -> 6000 + 497
    if (const auto w = c.wins.find(6000 + e); w == c.wins.end()) {
      b.warnings.push_back(s.name + ": no victory bank " + std::to_string(6000 + e) + " found (his win plays another scene)");
    } else {
      int actors = 0;
      Bytes n = RetargetEv(w->second.evp, e, ids.victory, slot, actors);
      if (!n.empty()) {
        b.Log("victory bank " + std::to_string(ids.victory) + " = wrestler " + std::to_string(e) + "'s (" + std::to_string(actors) + " actor model keys -> " + std::to_string(slot) + "xx)");
        out.emplace_back(ids.victory, std::move(n));
      }
    }
    std::sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.first < y.first; });
    if (!out.empty()) {
      io::Dir* d = add_dir("EVP ", 1);
      if (!d) return err = s.name + ": his source pac already has an EVP directory", false;
      for (auto& [id, blob] : out) {
        io::Entry p;
        p.name = io::Pad(std::to_string(id), 8), p.blob = std::move(blob);
        d->ents.push_back(std::move(p));
        evp.push_back(id);
      }
    }
  }
  // ---- the game's own entries are aliased, not copied (rows.bin ALIA): the importer only says when the entrance wrestler has none (the warnings as before)
  for (const auto& [stem, section] : {std::pair<const char*, const char*>{"pac\\gm", "GMEV"}, {"pac\\prtcls", "PCE6"}}) {
    const std::string name = entrance_fx::BankName(s.src_bank);
    if (!c.FindIn(FxSources(b, e, stem), section, name))
      b.warnings.push_back(s.name + ": no " + section + " bank " + name + " found in " + stem + " (his entrance plays without those effects)");
    else b.Log(std::string(stem) + ": " + section + " bank " + ids.FxName() + " = an alias of his entrance wrestler's " + name);
  }
  {
    const std::string want = entrance_movies::PlateName(e);
    std::vector<entrance_fx::Src> where = FxSources(b, e, "pac\\evt\\evtmov");
    if (!where.empty()) where.pop_back();  // (the disc's evtmov.pac holds no MOVN: dropped, not read)
    for (entrance_fx::Src& d : FxSources(b, e, "pac\\evt\\evtmov2_2")) where.push_back(std::move(d));
    if (c.FindIn(where, "MOVN", want)) b.Log("MOVN plate " + entrance_movies::PlateNo(ids.plate) + " = an alias of his entrance wrestler's " + want);
    else b.Log(s.name + ": wrestler " + std::to_string(e) + " has no name-plate movie " + want + " (the generic plate plays, as for him)");
  }
  // ---- renders: SSFA / SSFB / SSFC (HD) and SDFA / SDFB / SDFC (SD, our tag) from the UI's pictures; a picture he gave none of is an alias row of the base's own
  for (int sd = 0; sd < 2; ++sd) {
    Renders rs;
    std::array<bool, 3> have;
    UiRenders(s, sd != 0, rs, have);
    for (int k = 0; k < 3; ++k) {
      if (!have[size_t(k)]) continue;
      const std::string tag = std::string(sd ? "SDF" : "SSF") + char('A' + k);
      io::Dir* d = add_dir(tag.c_str(), 0);
      if (!d) return err = s.name + ": his source pac already has a " + tag + " directory", false;
      io::Entry x;
      char key[8];
      std::snprintf(key, sizeof key, "%04d", slot);
      x.name = key, x.blob = std::move(rs[size_t(k)]);
      d->ents.push_back(std::move(x));
    }
  }
  return true;
}

// The numbers of the EVP banks a file already holds (header only), as MakeFile would have written them.
inline bool EvpNumbersOf(const fs::path& file, std::vector<int>& evp) {
  std::vector<std::string> names;
  evp.clear();
  if (!PacDirNames(file, "EVP ", names)) return false;
  for (const std::string& n : names) evp.push_back(std::atoi(n.c_str()));
  return true;
}

// Everything of ONE character: his rows (-> rows.bin) and, when `make_file`, his file. `rank` = the Rank16 word the relocation list records (the game writes the seat's).
// Without `make_file` the EVP banks his existing file holds (`file`) are read from its header. On success s.src_bank / s.movie are set.
inline bool Instantiate(Ctx& c, Superstar& s, int rank, bool make_file, bool own, const fs::path& file, sc::SeatRows& rows, io::Pac& pac, std::string& err) {
  s.slot = sc::kRefSeat;
  if (!StarRows(c, s, rank, rows, err)) return false;
  std::vector<int> evp;
  if (make_file) {
    if (!MakeFile(c, s, own, pac, evp, err)) return false;
  } else if (!EvpNumbersOf(file, evp)) {
    return err = s.name + ": his file " + PathU8(file) + " cannot be read", false;
  }
  const asset_ids::AssetIds ids = CanonIds();
  std::set<int> banks_written;
  for (int n : evp)
    if (n != ids.victory) banks_written.insert(n);
  if (!EnexRowsOf(c, s, banks_written, rows, err)) return false;
  rows.alias = AliasesOf(s, evp);
  for (const sc::Alias& a : rows.alias)  // (the game's loader leaves out a card holding an alias row that is not in the reference form)
    if (!wwe2k14::seat_bind::AliasRefForm(a)) return err = s.name + ": the alias row " + a.tag + " '" + a.ours + "' is not in the reference form", false;
  rows.has_t102 = true, rows.t102_src_bank = uint32_t(s.src_bank);  // the game translates key bank(seat) -> his entrance wrestler's bank at the one reader
  rows.names[0] = OneLine(s.name), rows.names[1] = OneLine(s.short_name.empty() ? s.name : s.short_name), rows.names[2] = OneLine(s.name);
  return true;
}

// ---------------------------------------------------------------- the plan for one card: is his file stale?
struct FilePlan {
  bool own = false;      // his source is his own file
  bool stale = true;     // his file must be made (it is missing, in the old form, or its file_sig differs)
  uint32_t model_sig = 0, sig = 0;
  int src_bank = 0;
};
// `s` = the card's superstar with the source resolved. Needs ctx.LoadChEtc (the entrance wrestler's bank word).
inline FilePlan PlanFile(Ctx& c, const fs::path& root, const sc::Card& card, const Superstar& s) {
  FilePlan p;
  p.own = IsOwnSource(root, card.key, s.source_pac);
  const int e = s.entrance_from >= 0 ? s.entrance_from : s.base_id;
  const auto it = c.pro.find(uint32_t(e));
  p.src_bank = it != c.pro.end() ? asset_ids::SourceBank(it->second) : 0;
  if (p.own) {
    p.model_sig = HexExtra(card, "model_sig");
    if (!p.model_sig) {
      Bytes d;
      std::string why;
      if (ReadAll(U8Path(s.source_pac), d, assets::char_mods::kMaxPac)) {
        const Bytes m = ModelOf(d, why);
        if (!m.empty()) p.model_sig = Fnv(std::string(m.begin(), m.end()));
      }
    }
  } else {
    p.model_sig = FileSig(U8Path(s.source_pac));
  }
  uint32_t r[3] = {0, 0, 0};
  for (int k = 0; k < 3; ++k) {
    Bytes png;
    if (card.render[k] && ReadAll(RenderPng(root, card.key, k), png, 64 << 20)) r[k] = Fnv(std::string(png.begin(), png.end()));
  }
  p.sig = p.model_sig ? FileSigOf(p.model_sig, s.base_id, e, p.src_bank, r) : 0;
  std::error_code ec;
  const bool there = fs::is_regular_file(CharFile(root, card.key), ec);
  p.stale = !there || sc::FileForm(card) != sc::kFileForm || !p.sig || HexExtra(card, "file_sig") != p.sig;
  return p;
}

// ---------------------------------------------------------------- writing one character's folder
// (S110 SEAT-A review: the frame rebuild order) His new files are made in <folder>\stage\ (rows.bin, and pac\ch\ch497.pac when the file is made) while his card, rows.bin
// and file stay as they were; Publish moves them in only after the frame is committed, then writes the card's new `inst`.
inline fs::path StageDir(const fs::path& root, const std::string& key) { return CardDir(root, key) / "stage"; }
inline bool WriteStage(const fs::path& root, const std::string& key, sc::SeatRows& rows, const io::Pac* pac, std::string& err) {
  const fs::path st = StageDir(root, key);
  std::error_code ec;
  fs::remove_all(st, ec);
  fs::create_directories(st, ec);
  if (pac) {
    io::Pac copy = *pac;
    const Bytes bytes = io::WritePac(copy);
    if (bytes.empty()) return err = "his pac does not fit a pac directory", false;
    if (!WriteAll(Under(st, CharRel()), bytes)) return err = "cannot write his pac", false;
  }
  return sc::WriteRows(SeatFile(st / "rows.bin"), rows, err);
}
// (S110 LIBSIZE-2 review) His file is his only copy of his model once his source is his own file, so the old file is never removed before the new one is in place:
// it is renamed to pac.old\, the new one renamed in, and only then is pac.old\ removed (a failed second rename puts it back). A build stopped between the two renames
// leaves pac.old\ and no pac\: RestoreOldFile puts it back (BuildPack, before his source is looked for), and his card, written without `file_sig` first, is made again.
inline void RestoreOldFile(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir / "pac", ec) && fs::is_directory(dir / "pac.old", ec)) fs::rename(dir / "pac.old", dir / "pac", ec);
}
// The card without inst first (a crash below leaves him not instantiated, never half; and without `file_sig` when a new file is staged, so a card whose new file
// never got in place is not taken for one that did), then the stage's files in place, then the card with `inst`. After this the card's source is his own file.
inline bool Publish(const fs::path& root, sc::Card card, const sc::Inst& inst, std::string& err) {
  const fs::path dir = CardDir(root, card.key), st = StageDir(root, card.key), old = dir / "pac.old";
  std::error_code ec;
  const bool new_file = fs::exists(st / "pac", ec);
  {
    sc::Card held = card;
    held.inst = {};
    if (new_file) SetExtra(held, "file_sig", "");
    if (!sc::WriteCard(SeatFile(dir / "card.txt"), held, &err)) return false;
  }
  if (new_file) {  // a new file: the old one (and the old per-seat form's pac) is moved aside, the new one moved in, then the old one removed
    RestoreOldFile(dir);
    fs::remove_all(old, ec);
    const bool had = !ec && fs::exists(dir / "pac", ec);
    if (!ec && had) fs::rename(dir / "pac", old, ec);
    if (!ec) {
      fs::rename(st / "pac", dir / "pac", ec);
      if (ec && had) {
        std::error_code e2;
        fs::rename(old, dir / "pac", e2);  // (his old file back in place)
      }
    }
    if (!ec) {
      std::error_code e2;
      fs::remove_all(old, e2);  // (a leftover goes at his next Publish: pac\ holds the new file)
    }
  }
  if (!ec) {
    fs::remove(dir / "rows.bin", ec);
    fs::rename(st / "rows.bin", dir / "rows.bin", ec);
  }
  if (ec) return err = card.name + ": his files could not be put in place: " + ec.message(), false;
  fs::remove_all(st, ec);
  card.inst = inst;
  card.source = OwnSourceRel(card.key);
  SetExtra(card, "source_stat", "");
  return sc::WriteCard(SeatFile(dir / "card.txt"), card, &err);
}

// ---------------------------------------------------------------- checks
// His source as the card names it (a moved mods folder: the copy in sources\).
inline fs::path SourceOf(const fs::path& root, const sc::Card& c) { return U8Path(ResolveSource(root, SourceAbs(root, c.source))); }
// The source changed since he was made (size or time differ from the recorded ones). A missing file or no record is not a change; his own file is never a change.
inline bool SourceChanged(const fs::path& root, const sc::Card& c) {
  const std::string was = Extra(c, "source_stat");
  if (was.empty() || c.source.empty()) return false;
  const std::string now = SourceStat(SourceOf(root, c));
  return !now.empty() && now != was;
}

// A source pac that is gone, recovered from a pac already instantiated for him in the OLD per-seat form (the old pack's ch<slot>_dlcPP.pac, or his old library pac):
// ModelOf takes our directories out and the rest is written into <root>\sources\ under a new name. (A card in the new form needs none: his own file is his source.)
inline std::string RecoverSource(const fs::path& root, const fs::path& from, const std::string& name, std::string& why) {
  Bytes d;
  if (!ReadAll(from, d)) return why = "his old pac " + PathU8(from) + " cannot be read", std::string();
  std::string e;
  const Bytes out = ModelOf(d, e);
  ModPac m;
  if (out.empty() || !(e = ParseModPac(out, m, nullptr)).empty()) return why = "his old pac is not a character pac" + (e.empty() ? std::string() : " (" + e + ")"), std::string();
  std::error_code ec;
  const fs::path dir = root / "sources";
  fs::create_directories(dir, ec);
  std::string safe = assets::char_mods::SafeName(name + " (recovered)");
  if (safe.empty()) safe = "character (recovered)";
  fs::path dst = dir / U8Path(safe + ".pac");
  for (int k = 2; fs::exists(dst, ec); ++k) dst = dir / U8Path(safe + " " + std::to_string(k) + ".pac");
  if (!WriteAll(dst, out)) return why = "cannot write " + PathU8(dst), std::string();
  return PathU8(dst);
}
// His instantiated pac in his folder (any seat / pack number), "" when none.
inline fs::path LibraryPacOf(const fs::path& root, const std::string& key) {
  std::error_code ec;
  if (key.empty()) return {};
  for (const auto& e : fs::directory_iterator(CardDir(root, key) / "pac" / "ch", ec))
    if (e.is_regular_file(ec) && Lower(PathU8(e.path().extension())) == ".pac") return e.path();
  return {};
}

// rows.bin holds the relocation list (the RELO block, src\seat_card.h): framing walked only, no block parsed. A card made before S110 SEAT3-R has none:
// the game cannot re-target it, so it counts as not made.
inline bool RowsHaveRelo(const fs::path& p) {
  Bytes b;
  if (!ReadAll(p, b, 1 << 20) || b.size() < 10 || std::memcmp(b.data(), sc::kRowsMagic, 8) != 0) return false;
  for (size_t at = 10; at + 8 <= b.size();) {
    const size_t len = sc::detail::Get32(&b[at + 4]);
    if (std::memcmp(&b[at], "RELO", 4) == 0) return true;
    if (len > b.size() - at - 8) return false;
    at += 8 + len;
  }
  return false;
}
// His card is in the one form (inst for the reference identity, file_form 2).
inline bool InOneForm(const sc::Card& c) { return c.inst.valid && c.inst.seat == sc::kRefSeat && c.inst.bank == sc::kRefBank && sc::FileForm(c) == sc::kFileForm; }
// (S110 SEAT-A review) His instance's files are on disk (rows.bin with its relocation list + his one file): the cheap check before PLAY (the rows.bin framing is read,
// nothing else). A card in the old per-seat form is not made.
inline bool InstanceFilesThere(const fs::path& root, const sc::Card& card) {
  std::error_code ec;
  const fs::path dir = CardDir(root, card.key);
  return InOneForm(card) && fs::is_regular_file(dir / "rows.bin", ec) && fs::is_regular_file(CharFile(root, card.key), ec) && RowsHaveRelo(dir / "rows.bin");
}

// Files read back from his stage before Publish (`pac` = the stage's file when it was made, else his folder's): rows.bin parses and is the reference's, the pac holds his EMD.
// "" = fine. The whole pac is parsed when it was just written; an existing one is judged by its header.
inline std::string CheckInstance(const fs::path& dir, const fs::path& pac, bool parse_pac, const sc::Card& card) {
  if (!card.inst.valid) return "no inst";
  sc::SeatRows rows;
  std::string err;
  if (!sc::ReadRows(SeatFile(dir / "rows.bin"), rows, err)) return err;
  if (rows.seat != card.inst.seat) return "rows.bin is for seat " + std::to_string(rows.seat) + ", the card says " + std::to_string(card.inst.seat);
  if (!rows.has_relo) return "rows.bin has no relocation list";
  if (parse_pac) {
    Bytes d;
    io::Pac p;
    if (!ReadAll(pac, d) || !io::ReadPac(d, p, err)) return "his pac cannot be read" + (err.empty() ? std::string() : ": " + err);
    if (!p.FindDir("EMD ")) return "his pac has no EMD directory";
  } else {
    std::vector<std::string> names;
    if (!PacDirNames(pac, "EMD ", names) || names.empty()) return "his pac has no EMD directory";
  }
  return {};
}

// (S110 LIBSIZE-2 review) His new file, read back, holds his source whole before the source can go: every directory of the source (ModelOf of it when the source is
// his own file) is in the file with the same entries in the same order and the same bytes, the EMD names moved from the claimed id to 497 as MakeFile moves them;
// the file may add EMD alias rows (the attires of his base) and nothing else. A blob may end in zeros to its 256-byte unit (a source's last entry cut at its file's
// end). "" = it does, else what differs.
inline std::string HoldsSource(const fs::path& source, bool own, const fs::path& file) {
  Bytes d, f;
  std::string why, e;
  if (!ReadAll(source, d, assets::char_mods::kMaxPac)) return "his source " + PathU8(source) + " cannot be read again";
  if (own && (d = ModelOf(d, why)).empty()) return why;
  ModPac src;
  if (!(why = ParseModPac(std::move(d), src, nullptr)).empty()) return "his source: " + why;
  io::Pac got;
  if (!ReadAll(file, f)) return "his new file cannot be read back";
  const Bytes m = ModelOf(f, why);
  if (m.empty() || !io::ReadPac(m, got, e)) return "his new file cannot be read back" + (why.empty() ? std::string() : " (" + why + ")");
  char from[8], to[8];
  std::snprintf(from, sizeof from, "000%03d", src.claimed), std::snprintf(to, sizeof to, "000%03d", sc::kRefSeat);
  auto same = [](const Bytes& a, const Bytes& b) {
    if (b.size() < a.size() || b.size() - a.size() >= 256 || !std::equal(a.begin(), a.end(), b.begin())) return false;
    return std::all_of(b.begin() + ptrdiff_t(a.size()), b.end(), [](uint8_t v) { return v == 0; });
  };
  for (const io::Dir& sd : src.pac.dirs) {
    const io::Dir* nd = got.FindDir(sd.tag);
    if (!nd) return "his file has no " + io::Trim(sd.tag) + " directory";
    const bool emd = sd.tag == "EMD ";
    if (nd->ents.size() < sd.ents.size() || (!emd && nd->ents.size() != sd.ents.size()))
      return io::Trim(sd.tag) + ": his file holds " + std::to_string(nd->ents.size()) + " entries, his source " + std::to_string(sd.ents.size());
    for (size_t i = 0; i < nd->ents.size(); ++i) {
      const io::Entry& ne = nd->ents[i];
      if (i >= sd.ents.size()) {
        if (ne.alias < 0) return "EMD " + ne.name + ": an entry his source does not hold";
        continue;
      }
      std::string want = sd.ents[i].name;
      if (emd && want.compare(0, 6, from) == 0) want.replace(0, 6, to);
      if (ne.name != want) return io::Trim(sd.tag) + " entry " + std::to_string(i) + ": " + ne.name + ", his source's " + want;
      if (!same(io::Blob(sd, sd.ents[i]), io::Blob(*nd, ne))) return io::Trim(sd.tag) + " " + io::Trim(want) + ": its bytes differ from his source's";
    }
  }
  return {};
}
