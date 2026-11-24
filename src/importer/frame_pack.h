// S110 SEAT-A (docs\research\s110\SEAT1.md section 2 "The frame"): the FRAME pack, <mods>\superstars\frame\.
//
// One DLC pack for the whole library, with NOTHING per character: the catalog lists every seat (one type-21 entry, bundle rows (s, 0, 0) for each
// seat), the top pack's cumulative pacs are copied byte for byte (chEtc, string, DLC, sound, audience, ExclusiveAI), DLC_HD / DLC_SD hold the
// store-banner leaf only, and superstars.txt records what it was built on and the numbers per seat:
//     pack 6 / built_on 5 / top 5 238A188F / frame 1A2B3C4D / seat S bank B rank R [was ..]  (one per seat)
// It is built when the DLC the game gets changes (its pack id, the top roster pack, the seat list), never for a character. Every card's `inst`
// names the frame's digest (`frame`), so a rebuilt frame makes the cards instantiate again and nothing else does.
//
// UNITY INCLUDE: importer_core.cpp, inside `namespace importer`, after library.h.
#pragma once

namespace frame {

struct Info {
  int pack_id = 0;
  uint16_t revision = 0;
  int top_id = 0;           // the top roster pack (0 = the disc's own files)
  uint32_t top_sig = 0;
  std::vector<int> seats;   // ascending
  std::map<int, int> bank, rank;
  std::map<int, std::vector<int>> was;
  uint32_t sig = 0;
};

// The digest the cards carry: everything their rows and pacs depend on besides the bank numbers (a card carries its own bank): the pack id (his
// DAT row's pack code, his pac's name), the top pack (the chEtc his rows were cloned from) and the seat list (his rank).
inline uint32_t Sig(const Info& f) {
  std::string t = "frame1\npack " + std::to_string(f.pack_id) + "\ntop " + std::to_string(f.top_id) + " " + wwe2k14::seat_card::detail::Hex8(f.top_sig) + "\nseats";
  for (int s : f.seats) t += " " + std::to_string(s);
  // the seat -> bank / rank assignment too (lane B: the signature must change when it does; never for a card added / removed / edited)
  for (int s : f.seats) t += "\n" + std::to_string(s) + " " + std::to_string(f.bank.count(s) ? f.bank.at(s) : 0) + " " + std::to_string(f.rank.count(s) ? f.rank.at(s) : 0);
  const uint32_t v = wwe2k14::dlc::Fnv32(reinterpret_cast<const uint8_t*>(t.data()), t.size());
  return v ? v : 1;
}

inline fs::path Dir(const fs::path& root) { return root / "frame"; }

inline std::string Text(const Info& f, const std::set<int>& built_on) {
  std::string t = "# WWE 2K14 Superstar Importer FRAME: read by the game at boot (src\\superstars.h). Written by the importer; one `seat` per seat.\npack " +
                  std::to_string(f.pack_id) + "\nbuilt_on";
  for (int id : built_on) t += " " + std::to_string(id);
  char buf[64];
  std::snprintf(buf, sizeof buf, "\ntop %d %08X\nframe %08X\n", f.top_id, unsigned(f.top_sig), unsigned(f.sig));
  t += buf;
  for (int s : f.seats) {
    t += "seat " + std::to_string(s) + " bank " + std::to_string(f.bank.count(s) ? f.bank.at(s) : 0) + " rank " + std::to_string(f.rank.count(s) ? f.rank.at(s) : 0);
    if (const auto w = f.was.find(s); w != f.was.end() && !w->second.empty()) {
      t += " was";
      for (int v : w->second) t += " " + std::to_string(v);
    }
    t += "\n";
  }
  return t;
}

// The built frame on disk (superstars.txt); false when there is none or it is not a frame file.
inline bool ReadInfo(const fs::path& root, Info& f) {
  std::error_code ec;
  const fs::path txt = Dir(root) / "superstars.txt";
  if (!fs::is_regular_file(txt, ec)) return false;
  const wwe2k14::superstars_fit::Built b = wwe2k14::superstars_fit::ReadBuilt(txt);
  if (!b.found || b.seats.empty() || !b.frame_sig) return false;
  f = {};
  f.pack_id = b.pack_id, f.top_id = b.top < 0 ? 0 : b.top, f.top_sig = b.top_sig, f.seats = b.seats, f.bank = b.bank, f.rank = b.rank, f.was = b.was, f.sig = b.frame_sig;
  return true;
}

// Does the frame on disk fit what the build would make now (`want`: pack id, top, seats filled in)? Fresh = nothing to rewrite.
inline bool Fresh(const fs::path& root, const Info& want, Info& stored) {
  std::error_code ec;
  if (!ReadInfo(root, stored) || !fs::is_regular_file(Dir(root) / "info" / "catalog.dlc", ec)) return false;
  if (stored.pack_id != want.pack_id || stored.top_id != want.top_id || stored.top_sig != want.top_sig || stored.seats != want.seats || stored.sig != Sig(stored)) return false;
  for (int s : want.seats)
    if (!stored.bank.count(s) || !stored.rank.count(s)) return false;
  return true;
}

// The bank numbers and ranks of every seat (the same assignment the old build made for the characters, now over ALL seats): the recorded banks
// first while they are still free, then the ordinal's number, else the first free one. `recorded` = the last frame's (or the old pack's superstars.txt).
inline bool Plan(Builder& b, lib::Ctx& c, const wwe2k14::superstars_fit::Built& recorded, Info& f, std::string& err) {
  if (!c.LoadChEtc(err)) return false;
  std::set<int> used = UsedNames(b);
  const size_t names = used.size();
  {  // the values the bank-valued tables carry and the roster's character ids: the disc's files and the top pack's cumulative ones
    Bytes d;
    io::Pac p;
    std::string e;
    for (int top = 0; top < 2; ++top) {
      const bool ok_dlc = top ? b.ReadCumulative("pac\\DLC", d) : b.ReadGame("pac\\DLC.pac", d);
      if (ok_dlc && io::ReadPac(d, p, e)) asset_ids::AddCarried(p, used);
      if (top ? b.ReadCumulative("pac\\evt\\evtdata", d) : b.ReadGame("pac\\evt\\evtdata.pac", d)) asset_ids::AddTableValues(d, used);
      const bool ok_etc = top ? b.ReadCumulative("pac\\ch\\chEtc", d) : b.ReadGame("pac\\ch\\chEtc.pac", d);
      if (ok_etc && io::ReadPac(d, p, e)) asset_ids::AddRosterIds(p, used);
    }
  }
  const std::vector<int> fr = asset_ids::FreeNumbers(used);
  asset_ids::SetFreeList(fr);
  std::map<int, int> moved;
  if (const std::string why = asset_ids::Assign(f.seats, recorded.bank, &moved); !why.empty()) return err = why, false;
  f.bank.clear(), f.rank.clear(), f.was.clear();
  int i = 0;
  for (int s : f.seats) {
    const int bank = asset_ids::Of(s).bank;
    if (const std::string why = asset_ids::NameProblem(used, bank); !why.empty()) return err = "seat " + std::to_string(s) + ": " + why, false;
    f.bank[s] = bank;
    f.rank[s] = c.base_rank + (++i);
    std::vector<int> was;
    if (const auto it = recorded.was.find(s); it != recorded.was.end()) was = it->second;
    if (const auto m = moved.find(s); m != moved.end()) was.push_back(m->second);
    was.erase(std::remove_if(was.begin(), was.end(), [&](int w) { return w == bank || w <= 0 || w == s; }), was.end());
    std::sort(was.begin(), was.end()), was.erase(std::unique(was.begin(), was.end()), was.end());
    if (!was.empty()) f.was[s] = was;
  }
  b.Log("entrance banks: " + std::to_string(fr.size()) + " numbers free (" + std::to_string(names) + " directory names, " + std::to_string(used.size()) + " numbers used in all); " +
        std::to_string(f.seats.size()) + " seats numbered" + (moved.empty() ? "" : "; " + std::to_string(moved.size()) + " moved: recorded bank no longer free"));
  return true;
}

// Writes the frame into `out` (an empty folder the caller made): see the top. `built_on` = the packs it was built on.
inline bool Write(Builder& b, const fs::path& out, const Info& f, std::string& err) {
  std::error_code ec;
  fs::create_directories(out / "info", ec);
  fs::create_directories(out / "pac" / "ch", ec);
  std::vector<std::pair<std::string, io::Pac>> written;  // (catalog name, the pac's directory with blobs cleared): the arcs
  auto keep = [&](const std::string& rel, io::Pac& p) {
    for (io::Dir& d : p.dirs)
      for (io::Entry& e : d.ents) e.blob.clear(), e.blob.shrink_to_fit();
    written.emplace_back(rel, std::move(p));
  };
  // 1. the top pack's cumulative pacs, byte for byte
  for (const char* base : {"ExclusiveAI", "audience", "sound", "string", "DLC"}) {
    const std::string src = std::string("pac\\") + base, from = b.CumulativeName(src), to = b.Rel(src);
    Bytes d;
    io::Pac p;
    std::string e;
    if (!b.ReadCumulative(src, d) || !io::ReadPac(d, p, e)) return err = "cannot read " + from + " of " + b.BaseName(), false;
    if (!WriteAll(Under(out, to), d)) return err = "cannot write " + to, false;
    keep(to, p);
    b.Log(to + " = copy of " + b.BaseName() + "'s");
  }
  {
    const std::string from = b.CumulativeName("pac\\ch\\chEtc"), to = b.Rel("pac\\ch\\chEtc");
    Bytes d;
    io::Pac p;
    std::string e;
    if (!b.ReadCumulative("pac\\ch\\chEtc", d) || !io::ReadPac(d, p, e)) return err = "cannot read " + from, false;
    if (!WriteAll(Under(out, to), d)) return err = "cannot write " + to, false;
    keep(to, p);
    b.Log(to + " = copy of " + b.BaseName() + "'s");
  }
  // 2. the catalog's one entry (bundle rows for every seat) and DLC_HD / DLC_SD: the store banner leaf only
  std::vector<io::CatEntry> real_entries;
  for (const RealPack& rp : b.packs) real_entries.insert(real_entries.end(), rp.entries.begin(), rp.entries.end());
  const pack_entry::PackEntryPlan plan = pack_entry::PlanPackEntry(real_entries, f.seats, uint16_t(b.pack_id), Latin1("Superstar Importer"));  // (the catalog's name text is Windows-1252)
  if (!plan.error.empty()) return err = plan.error, false;
  for (int sd = 0; sd < 2; ++sd) {
    const std::string res = sd ? "SD" : "HD", from = b.CumulativeName("pac\\DLC_" + res);
    io::Pac topp;
    Bytes d;
    std::string e;
    if (!b.ReadCumulative("pac\\DLC_" + res, d) || !io::ReadPac(d, topp, e)) return err = "cannot read " + from, false;
    io::Pac p;
    p.magic = "EPAC", p.version = topp.version, p.trailer = topp.trailer;
    io::Entry* tex = topp.Find("IGST", "TEX ");
    std::vector<io::Leaf> leaves;
    if (!tex || !io::ReadPach(tex->blob, leaves) || leaves.empty()) return err = from + ": no store banners", false;
    Bytes generic = leaves.front().raw;  // leaf 0: the generic card
    for (const auto& l : leaves)
      if (l.id == 0) generic = l.raw;
    {  // one leaf for the pack's one item, under the entry's id
      io::Leaf l;
      l.id = plan.entry.id;
      l.raw = generic;
      leaves.push_back(std::move(l));
    }
    std::sort(leaves.begin(), leaves.end(), [](auto& x, auto& y) { return x.id < y.id; });  // ids ascending, as the real PACHs
    io::Dir ig;
    ig.tag = "IGST";
    io::Entry te;
    te.name = "TEX ";
    te.blob = io::WritePach(leaves, /*dedupe=*/true);
    ig.ents.push_back(std::move(te));
    p.dirs.push_back(std::move(ig));
    const std::string rel = b.Rel("pac\\DLC_" + res);
    const Bytes bytes = io::WritePac(p);
    if (bytes.empty() || !WriteAll(Under(out, rel), bytes)) return err = "cannot write DLC_" + res, false;
    keep(rel, p);
  }
  b.Log("DLC_HD / DLC_SD: the store banner leaf " + std::to_string(plan.entry.id) + " only (a copy of leaf 0, shared data)");
  // 3. catalog + arcs: DLC_HD (0, -1), DLC_SD (-1, 0), every other pac a == b running in sorted order
  {
    std::vector<io::CatFile> files;
    std::vector<std::pair<std::string, const io::Pac*>> sorted;
    for (auto& [n, p] : written) sorted.emplace_back(n, &p);
    std::sort(sorted.begin(), sorted.end(), [](auto& x, auto& y) { return x.first < y.first; });
    std::vector<const io::Pac*> hd, sdv;
    hd.push_back(nullptr), sdv.push_back(nullptr);
    int k = 1;
    for (auto& [n, p] : sorted) {
      io::CatFile cf;
      cf.name = n;
      if (n.find("DLC_HD_") != std::string::npos) cf.a = 0, cf.b = -1, hd[0] = p;
      else if (n.find("DLC_SD_") != std::string::npos) cf.a = -1, cf.b = 0, sdv[0] = p;
      else cf.a = cf.b = int16_t(k++), hd.push_back(p), sdv.push_back(p);
      files.push_back(cf);
    }
    if (!hd[0] || !sdv[0]) return err = "internal: no DLC_HD / DLC_SD written", false;
    const std::vector<io::CatEntry> entries = {plan.entry};  // ONE type-21 entry; the seats are its bundle rows
    const uint16_t revision = f.revision;
    const Bytes cat = io::WriteCatalog(uint16_t(b.pack_id), revision, entries, plan.bundles, files, FileTimeNow());
    if (!WriteAll(out / "info" / "catalog.dlc", cat) || !WriteAll(out / "info" / "catalog_hd.arc", io::WriteArc(hd)) || !WriteAll(out / "info" / "catalog_sd.arc", io::WriteArc(sdv)))
      return err = "cannot write the catalog", false;
    // check it the way the game reads it (the gate opened for our pack id, as the game side does)
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    gate[0] = uint32_t(b.pack_id) << 16 | 1;
    io::CatInfo info;
    if (const std::string why = io::ParseCatalogLikeGame(cat, gate, info); !why.empty()) return err = "internal: the catalog does not parse: " + why, false;
    b.Log("info\\catalog.dlc: pack " + std::to_string(b.pack_id) + " revision " + std::to_string(revision) + ", one type-21 entry (id " + std::to_string(plan.entry.id) + ") with " +
          std::to_string(plan.bundles.size()) + " bundle record(s) for " + std::to_string(f.seats.size()) + " seats, " + std::to_string(files.size()) + " files");
  }
  // 4. superstars.txt
  if (!WriteText(out / "superstars.txt", Text(f, b.built_on))) return err = "superstars.txt could not be written", false;
  return true;
}

// frame.building -> frame (the old frame aside until the new one is in). false + why when the old one is in use.
inline bool Commit(const fs::path& root, std::string& err) {
  std::error_code ec;
  const fs::path fin = Dir(root), old = root / "frame.old", stage = root / "frame.building";
  fs::remove_all(old, ec);
  if (fs::exists(fin, ec)) {
    fs::rename(fin, old, ec);
    if (ec) return err = "the old frame is in use (close the game first): " + ec.message(), false;
  }
  fs::rename(stage, fin, ec);
  if (ec) {
    std::error_code e2;
    fs::rename(old, fin, e2);
    return err = "could not put the new frame in place: " + ec.message(), false;
  }
  fs::remove_all(old, ec);
  return true;
}

}  // namespace frame
