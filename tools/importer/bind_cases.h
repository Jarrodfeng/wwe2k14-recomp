// S110 LIBSIZE-2 (docs\research\s110\LIBSIZE.md "Sites" item 9): the IMPORTER-SIDE equality of the one seat-free character file, on the player's real files
// (included at the end of test_core.cpp; first form S110 SEAT3 review).
//
//   test_core.exe --game DIR --dlc DIR --mods OUT_DIR --source PAC --bind-cases
//
// THE BAR: a card is made ONCE for the reference identity (seat_card.h kRefSeat 497 / kRefBank 9997): library\<KEY>\pac\ch\ch497.pac (EMD + aliases, EVP, the UI's own
// renders) + rows.bin (RELO + the ALIA block). For EVERY seat s of the build the game's own steps must give what the old per-seat Instantiate gave for s:
//   * the node rows (seats_core.h NodeRows, HD and SD) through RetargetDirRows(497 -> s): ONLY the rows we own (EMD of s, the UI's renders of s), every (directory, name)
//     resolving to the SAME BYTES as the source pac's models (the old Instantiate copied them verbatim: the SOURCE is the reference, read from --source) and the UI's
//     pictures (BpeIdentity(Dds(...)), as the old UiRenders made them);
//   * the seat's EV file (seats_core.h SeatEvFile) = the entrance wrestler's EVP blobs, bank / actor keys moved to s (a test-side re-statement of the old RetargetEv,
//     read from the game's own entrance / winning pacs through the HOST FINDER below);
//   * the alias rows (rows.alias, resolved by seat_alias_core.h FindFirst / AliasNodes over the HOST FINDER's node list = the same game / pack pacs the old byte
//     copies came from, importer::HostGamePacs) point every EVZ / GMEV / PCE6 / MOVN name and every base render at the bytes the old copy held. Where the game's first
//     match is another file than the importer's old search order (FxSources / FindBanks / BaseRenders: kept here as the REFERENCE ORDER, test-side only) the bytes are
//     compared and a difference is LOGGED (claim 14), never hidden. A name no node has gives no row (and the reference must not have found it either).
// Also: RetargetCard against the cards of the same group (identical settings = identical rows for any seat), t -> s -> t, the coincidence scan, planted twins.
// Nothing is written but under --mods.
#pragma once
#include "../../src/seat_alias_core.h"
#include "../../src/seat_bind_core.h"
#include "../../src/seats_core.h"

namespace sb = wwe2k14::seat_bind;
namespace sa = wwe2k14::seat_alias;
namespace seatsn = wwe2k14::seats;

struct BindInst {
  int seat = 0, bank = 0, rank = 0;  // the seat the card sits on in the build (his frame seat, bank, rank)
  Lib lib;                           // the card read back: rows.bin (497 / 9997), his one file parsed
  Bytes file;                        // ch497.pac as written
  int base = 0, ent = 0;
  std::array<Bytes, 3> ui_hd, ui_sd;  // the UI's pictures as the old UiRenders made them (empty = none: the base's is aliased)
};

template <class A, class B>
static void BindDiffBytes(const std::string& what, const A& got, const B& want, std::vector<std::string>& bad) {
  if (got.size() != want.size()) {
    bad.push_back(what + ": " + std::to_string(got.size()) + " bytes, want " + std::to_string(want.size()));
    return;
  }
  std::string at;
  int n = 0;
  for (size_t i = 0; i < got.size(); ++i)
    if (got[i] != want[i] && n++ < 8) {
      char b[40];
      std::snprintf(b, sizeof b, " +%zu %02X/%02X", i, unsigned(got[i]), unsigned(want[i]));
      at += b;
    }
  if (n) bad.push_back(what + ": " + std::to_string(n) + " byte(s) differ (got/want)" + at);
}

static void BindCompareRows(const sc::SeatRows& got, const sc::SeatRows& want, std::vector<std::string>& bad) {
  if (got.seat != want.seat) bad.push_back("rows: seat " + std::to_string(got.seat) + ", want " + std::to_string(want.seat));
  BindDiffBytes("DAT", got.dat, want.dat, bad);
  BindDiffBytes("PRO", got.pro, want.pro, bad);
  BindDiffBytes("DSBD", got.dsbd, want.dsbd, bad);
  BindDiffBytes("BIN5", got.bin5, want.bin5, bad);
  if (got.binp.size() != want.binp.size()) bad.push_back("BINP: " + std::to_string(got.binp.size()) + " rows, want " + std::to_string(want.binp.size()));
  else
    for (size_t i = 0; i < got.binp.size(); ++i) {
      if (got.binp[i].k != want.binp[i].k) bad.push_back("BINP " + std::to_string(i) + ": leaf");
      BindDiffBytes("BINP leaf " + std::to_string(want.binp[i].k), got.binp[i].row, want.binp[i].row, bad);
    }
  if (got.lsd.size() != want.lsd.size()) bad.push_back("LSD: " + std::to_string(got.lsd.size()) + " rows, want " + std::to_string(want.lsd.size()));
  else
    for (size_t i = 0; i < got.lsd.size(); ++i) {
      if (got.lsd[i].list != want.lsd[i].list) bad.push_back("LSD " + std::to_string(i) + ": list");
      BindDiffBytes("LSD list " + std::to_string(want.lsd[i].list), got.lsd[i].row, want.lsd[i].row, bad);
    }
  if (got.has_relo != want.has_relo || got.relo.size() != want.relo.size()) bad.push_back("RELO: " + std::to_string(got.relo.size()) + " entries, want " + std::to_string(want.relo.size()));
  else
    for (size_t i = 0; i < got.relo.size(); ++i) {
      const sc::Reloc &a = got.relo[i], &b = want.relo[i];
      if (a.tag != b.tag || a.sub != b.sub || a.off != b.off || a.kind != b.kind || a.arg != b.arg) bad.push_back("RELO entry " + std::to_string(i) + " (" + b.tag + " +" + std::to_string(b.off) + ")");
    }
  if (got.has_t102 != want.has_t102 || got.t102_src_bank != want.t102_src_bank) bad.push_back("T102");
  for (int k = 0; k < 3; ++k)
    if (got.names[k] != want.names[k]) bad.push_back("NAME " + std::to_string(k));
}

// Node rows (big-endian arc form + terminator) as (tag, flags, rows of {name, sector, size}).
struct BindRow { std::string tag, name; uint32_t flags = 0, sector = 0, size = 0; };
static bool BindSplit(const std::vector<uint8_t>& rows, std::vector<BindRow>& out, std::string& why) {
  out.clear();
  size_t body = 0;
  if (!sb::ScanDirs(rows.data(), rows.size(), body, why)) return false;
  for (size_t at = 0; at < body;) {
    const uint32_t flags = sb::Be32(&rows[at + 4]), words = flags >> 20, kind = (flags >> 12) & 15;
    const size_t rs = kind ? 16 : 12, n = words * 4u / rs;
    const std::string tag(reinterpret_cast<const char*>(&rows[at]), 4);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* r = &rows[at + 12 + i * rs];
      out.push_back({tag, std::string(reinterpret_cast<const char*>(r), rs - 8), flags, sb::Be32(r + rs - 8), sb::Be32(r + rs - 4)});
    }
    at += 12 + size_t(words) * 4;
  }
  return true;
}
static Bytes BindAt(const Bytes& file, uint32_t sector, uint32_t size) {
  const uint64_t at = io::kHeader + uint64_t(sector) * io::kSector, len = uint64_t(size) * 256;
  if (at >= file.size()) return {};
  return Bytes(file.begin() + ptrdiff_t(at), file.begin() + ptrdiff_t(std::min<uint64_t>(file.size(), at + len)));
}
static bool BindNodeRows(const Bytes& file, bool sd, std::vector<uint8_t>& out, std::string& why) {
  if (file.size() < io::kHeader) return why = "the pac is shorter than its header", false;
  return seatsn::NodeRows(std::vector<uint8_t>(file.begin(), file.begin() + ptrdiff_t(io::kHeader)), file.size(), sd, out, why);
}

// ---------------------------------------------------------------- the HOST FINDER: the game's pacs as the game's pac list (importer::HostGamePacs), HD and SD
struct HostGame {
  std::vector<importer::HostPac> pacs[2];     // [0] HD list, [1] SD list (the game has one of them mounted)
  std::vector<sa::NodeView> nodes[2];         // the node rows of each pac, same index (empty rows = not a plain pac: never matched)
  std::map<std::string, Bytes> cache;
  size_t cached = 0;
  std::string mods;
  launcher::GameFiles gf;
  std::string dlc;
  bool Open(const Env& env, const std::string& mods_dir) {
    mods = mods_dir, gf = env.gf, dlc = env.dlc;
    for (int sd = 0; sd < 2; ++sd) {
      pacs[sd] = importer::HostGamePacs(mods_dir, env.gf, env.dlc, "", sd != 0);
      for (const importer::HostPac& p : pacs[sd]) {
        sa::NodeView v;
        v.path = p.label;
        Bytes d;
        std::vector<uint8_t> rows;
        std::string why;
        if (p.read(d) && d.size() >= io::kHeader && BindNodeRows(d, false, rows, why)) v.rows = std::move(rows);
        nodes[sd].push_back(std::move(v));
      }
    }
    return !pacs[0].empty() && !pacs[1].empty();
  }
  const Bytes& File(int sd, const std::string& label) {
    const std::string k = std::to_string(sd) + label;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    if (cached > (900u << 20)) cache.clear(), cached = 0;
    Bytes d;
    for (const importer::HostPac& p : pacs[sd])
      if (p.label == label) p.read(d);
    cached += d.size();
    return cache.emplace(k, std::move(d)).first->second;
  }
  Bytes Row(int sd, const std::string& label, uint32_t sector, uint32_t size) { return BindAt(File(sd, label), sector, size); }
};
struct BindHit { std::string label; uint32_t sector = 0, size = 0; bool found = false; };
// The first pac of `order` (indexes into nodes[sd]) that holds (tag, name): what the importer's old finders did over their own search orders.
static BindHit BindFirstIn(const HostGame& H, int sd, const std::vector<size_t>& order, const std::string& tag, const std::string& name) {
  BindHit h;
  for (size_t i : order) {
    std::vector<sa::NodeView> one = {H.nodes[sd][i]};
    if (sa::FindFirst(one, tag, name, h.label, h.sector, h.size)) return h.found = true, h;
  }
  return h;
}
static int BindPackOf(const std::string& label) {  // "pack N rel" -> N, a disc pac -> -1
  return label.rfind("pack ", 0) == 0 ? std::atoi(label.c_str() + 5) : -1;
}
static bool BindHas(const std::string& label, const std::string& part) { return LowerS(label).find(LowerS(part)) != std::string::npos; }
// The importer's old orders (library.h FxSources / importer_core.cpp FindBanks / BaseRenders), as index lists into one host list.
static std::vector<size_t> BindOrderBanks(const HostGame& H, int sd, const char* stem) {  // FindBanks: the disc's files sorted, then the packs ascending
  std::vector<size_t> disc, packs;
  for (size_t i = 0; i < H.pacs[sd].size(); ++i) {
    const std::string& l = H.pacs[sd][i].label;
    if (!BindHas(l, std::string("pac\\evt\\") + stem)) continue;
    (BindPackOf(l) < 0 ? disc : packs).push_back(i);
  }
  std::sort(packs.begin(), packs.end(), [&](size_t a, size_t b) { return BindPackOf(H.pacs[sd][a].label) < BindPackOf(H.pacs[sd][b].label); });
  disc.insert(disc.end(), packs.begin(), packs.end());
  return disc;
}
static std::vector<size_t> BindOrderFx(const HostGame& H, int sd, const std::string& stem, int own, bool packs_only = false) {  // FxSources: his own pack, the others from the highest, the disc
  std::vector<size_t> v;
  for (size_t i = 0; i < H.pacs[sd].size(); ++i)
    if (BindPackOf(H.pacs[sd][i].label) == own && own >= 0 && BindHas(H.pacs[sd][i].label, stem + "_dlc")) v.push_back(i);
  for (size_t i = 0; i < H.pacs[sd].size(); ++i) {  // (the host list is newest first)
    const std::string& l = H.pacs[sd][i].label;
    if (BindPackOf(l) >= 0 && BindPackOf(l) != own && BindHas(l, stem + "_dlc")) v.push_back(i);
  }
  if (!packs_only)
    for (size_t i = 0; i < H.pacs[sd].size(); ++i)
      if (BindPackOf(H.pacs[sd][i].label) < 0 && BindHas(H.pacs[sd][i].label, stem + ".pac")) v.push_back(i);
  return v;
}

// The source pac's models, the reference for the EMD rows (the old Instantiate copied them verbatim).
struct BindSrc {
  std::map<std::pair<int, int>, Bytes> emd;  // (attire, kind) -> bytes
  int a0 = 0;
  bool ok = false;
};
static BindSrc BindSource(const std::string& path) {
  BindSrc s;
  io::Pac p;
  std::string e;
  if (!io::ReadPac(UnxctdFile(fs::u8path(path)), p, e)) return s;
  const io::Dir* d = DirOf(p, "EMD ");
  if (!d) return s;
  std::map<int, int> votes;
  for (const io::Entry& x : d->ents)
    if (x.name.size() == 8 && x.name.compare(0, 3, "000") == 0) ++votes[std::atoi(x.name.substr(3, 3).c_str())];
  if (votes.empty()) return s;
  const int claimed = std::max_element(votes.begin(), votes.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
  for (const io::Entry& x : d->ents)
    if (x.name.size() == 8 && x.name.compare(0, 3, "000") == 0 && std::atoi(x.name.substr(3, 3).c_str()) == claimed) s.emd[{x.name[6] - '0', x.name[7] - '0'}] = io::Blob(*d, x);
  if (s.emd.empty()) return s;
  s.a0 = s.emd.begin()->first.first;
  s.ok = true;
  return s;
}
// The old RetargetEv, restated: the decoded EV table with the header bank and the entrance wrestler's actor keys moved (the importer wrote it BpeIdentity'd; compared decoded).
static Bytes BindRefEv(const Bytes& raw, int e, int to_bank, int to_char) {
  Bytes ev = io::Decode(raw);
  if (ev.size() < 0x44 || std::memcmp(ev.data(), "0FOP", 4) != 0) return {};
  io::WB16(&ev[0x20], uint16_t(to_bank));
  const uint32_t n = io::B32(&ev[0x40]) & 0xFFFFFF;
  for (uint32_t i = 0; i < n && 0x44 + 32ull * (i + 1) <= ev.size(); ++i) {
    uint8_t* r = &ev[0x44 + 32 * size_t(i)];
    const uint32_t x = io::B32(r + 12);
    if (r[16] == 1 && int(x / 100) == e) io::WB32(r + 12, uint32_t(to_char) * 100 + x % 100);
  }
  return ev;
}

struct BindStat { int rows = 0, blobs = 0, aliases = 0, claim14 = 0; std::vector<std::string> log; };

// Everything that differs between the card made ONCE (T) and what the old per-seat build gave for seat `s` ("" = none). `H` = the host finder, `src` = the source's models.
static std::vector<std::string> BindCard(HostGame& H, const BindInst& T, int s, int bank_s, const BindSrc& src, const std::string& lib_root, int own_pack_e, int own_pack_base, BindStat& st) {
  std::vector<std::string> bad;
  std::string err;
  // ---- node rows, HD and SD: ONLY what we own, every row resolving to the reference bytes
  for (int sd = 0; sd < 2; ++sd) {
    const std::string arc = sd ? "SD" : "HD";
    std::vector<uint8_t> rt, mapped;
    std::string why;
    if (!BindNodeRows(T.file, sd != 0, rt, why)) {
      bad.push_back(arc + " node rows: " + why);
      continue;
    }
    if (!sb::RetargetDirRows(rt, sc::kRefSeat, s, sc::kRefBank, bank_s, false, mapped, err)) {
      bad.push_back(arc + " RetargetDirRows refused: " + err);
      continue;
    }
    std::vector<BindRow> a;
    if (!BindSplit(mapped, a, why)) {
      bad.push_back(arc + " node rows do not split: " + why);
      continue;
    }
    std::set<std::pair<int, int>> emd_seen;
    std::set<std::string> render_seen;
    for (const BindRow& r : a) {
      const std::string w = arc + " " + r.tag + " row '" + r.name + "'";
      if (r.tag == "EMD " && r.name.size() == 8 && r.name.compare(0, 3, "000") == 0 && std::atoi(r.name.substr(3, 3).c_str()) == s) {
        const int at = r.name[6] - '0', k = r.name[7] - '0';
        const auto it = src.emd.count({at, k}) ? src.emd.find({at, k}) : src.emd.find({src.a0, k});  // (an attire the mod lacks: the mod's first attire, as the importer aliased it)
        if (it == src.emd.end()) bad.push_back(w + ": the source has no such model");
        else BindDiffBytes(w + " content", BindAt(T.file, r.sector, r.size), it->second, bad);
        emd_seen.insert({at, k});
        ++st.rows;
      } else if (r.tag.compare(0, 3, sd ? "SSF" : "SSF") == 0 && r.tag[3] >= 'A' && r.tag[3] <= 'C' && r.name == (std::string("0") + std::to_string(s))) {
        const size_t k = size_t(r.tag[3] - 'A');
        const Bytes& want = sd ? T.ui_sd[k] : T.ui_hd[k];
        if (want.empty()) bad.push_back(w + ": a render the UI did not give");
        else {
          Bytes got = BindAt(T.file, r.sector, r.size);  // (the row's 256-byte rounding follows the blob)
          if (got.size() >= want.size()) got.resize(want.size());
          BindDiffBytes(w + " content", got, want, bad);
        }
        render_seen.insert(r.tag);
        ++st.rows;
      } else {
        bad.push_back(w + ": a row that is not his survived the re-target");
      }
    }
    for (const auto& [ak, bytes] : src.emd)
      if (!emd_seen.count(ak)) bad.push_back(arc + " EMD model attire " + std::to_string(ak.first) + " kind " + std::to_string(ak.second) + " of the source is not a row");
    for (int k = 0; k < 3; ++k)
      if (!(sd ? T.ui_sd[size_t(k)] : T.ui_hd[size_t(k)]).empty() && !render_seen.count(std::string("SSF") + char('A' + k))) bad.push_back(arc + " the UI's render " + char('A' + k) + " is not a row");
  }
  // ---- the seat's EV file against the entrance wrestler's EVP blobs of the game's own pacs
  {
    const io::Dir* ep = DirOf(T.lib.pac, "EVP ");
    sb::EvFile f;
    int actors = 0, blobs = 0;
    if (!ep) bad.push_back("EVP: his file has no EVP directory");
    else if (!seatsn::SeatEvFile(lib_root, T.lib.card, s, bank_s, f, err, &actors, &blobs)) bad.push_back("SeatEvFile refused: " + err);
    else {
      if (int(f.rows.size()) != int(ep->ents.size())) bad.push_back("EV file: " + std::to_string(f.rows.size()) + " rows, his file holds " + std::to_string(ep->ents.size()) + " EVP blobs");
      const int src_bank = T.lib.card.inst.src_bank;
      for (const io::Entry& x : ep->ents) {
        const int n = std::atoi(io::Trim(x.name).c_str());
        int from = -1;
        if (n == sc::kRefBank) from = src_bank;
        else if (n == 6000 + sc::kRefSeat) from = 6000 + T.ent;
        else
          for (int g = 0; g < 10; ++g)
            if (n == 10000 + 1000 * g + sc::kRefBank) from = 10000 + 1000 * g + src_bank;
        const bool victory = n == 6000 + sc::kRefSeat;
        const BindHit h = BindFirstIn(H, 0, BindOrderBanks(H, 0, victory ? "Winning" : "Entrance"), "EVP ", io::Pad(std::to_string(from), 8));
        if (from < 0 || !h.found) {
          bad.push_back("EVP " + io::Trim(x.name) + ": the entrance wrestler's bank " + std::to_string(from) + " is in none of the game's pacs");
          continue;
        }
        const Bytes raw = H.Row(0, h.label, h.sector, h.size);
        const std::string name_s = io::Pad(sb::EvName(io::Trim(x.name), sc::kRefSeat, s, sc::kRefBank, bank_s), 8);
        // (a) the importer's blob (made for 497 / 9997) is the old RetargetEv for the reference identity, (b) the seat's is the old one for s
        if (BindRefEv(raw, T.ent, n, sc::kRefSeat) != io::Decode(io::Blob(*ep, x))) bad.push_back("EVP " + io::Trim(x.name) + ": his blob is not the entrance wrestler's with the reference identity");
        const sb::EvRow* row = nullptr;
        for (const sb::EvRow& r : f.rows)
          if (r.name == name_s) row = &r;
        if (!row) {
          bad.push_back("EV file: no row '" + name_s + "'");
          continue;
        }
        const Bytes want = BindRefEv(raw, T.ent, sb::MapBankNumber(n, sc::kRefSeat, s, sc::kRefBank, bank_s), s);
        if (want != io::Decode(Bytes(sb::EvBlobAt(f, *row)))) bad.push_back("EV file row " + name_s + ": not the entrance wrestler's blob moved to seat " + std::to_string(s));
        ++st.blobs;
      }
    }
  }
  // ---- the alias rows: first the SET (restated here from the card's truth: his EVP banks, his source bank, his entrance wrestler, his base, the renders the UI gave)
  {
    std::set<std::string> want, have;
    const int src_bank = T.lib.card.inst.src_bank;
    char gm[8], cm[8], bs[8], rf[8];
    std::snprintf(gm, sizeof gm, "%04d", src_bank), std::snprintf(cm, sizeof cm, "%04d", sc::kRefBank), std::snprintf(bs, sizeof bs, "%04d", T.base), std::snprintf(rf, sizeof rf, "%04d", sc::kRefSeat);
    if (const io::Dir* ep = DirOf(T.lib.pac, "EVP "))
      for (const io::Entry& x : ep->ents) {
        const int n = std::atoi(io::Trim(x.name).c_str());
        int from = -1;
        if (n == sc::kRefBank) from = src_bank;
        else if (n == 6000 + sc::kRefSeat) from = 6000 + T.ent;
        else
          for (int g = 0; g < 10; ++g)
            if (n == 10000 + 1000 * g + sc::kRefBank) from = 10000 + 1000 * g + src_bank;
        want.insert("EVZ |" + io::Pad(std::to_string(from), 8) + "|" + io::Pad(std::to_string(n), 8));
      }
    want.insert(std::string("GMEV|") + gm + "|" + cm), want.insert(std::string("PCE6|") + gm + "|" + cm);
    char pm[8], pc[8];
    std::snprintf(pm, sizeof pm, "M%03d", 500 + T.ent), std::snprintf(pc, sizeof pc, "M%03d", 500 + sc::kRefSeat);
    want.insert(std::string("MOVN|") + pm + "|" + pc);
    for (int k = 0; k < 3; ++k)
      if (T.ui_hd[size_t(k)].empty()) want.insert(std::string("SSF") + char('A' + k) + "|" + bs + "|" + rf);
    for (const sc::Alias& a : T.lib.rows.alias) have.insert(a.tag + "|" + a.game + "|" + a.ours);
    for (const std::string& w : want)
      if (!have.count(w)) bad.push_back("alias rows: missing '" + w + "'");
    for (const std::string& h : have)
      if (!want.count(h)) bad.push_back("alias rows: unexpected '" + h + "'");
  }
  // ---- every name resolves (through the game's pac list) to the bytes the old byte copy held
  for (int sd = 0; sd < 2; ++sd) {
    std::map<int, std::vector<sc::Alias>> wants;
    std::map<int, int> bank_of;
    wants[s] = T.lib.rows.alias, bank_of[s] = bank_s;
    std::map<std::string, std::vector<uint8_t>> by_path;
    std::vector<std::string> missing;
    if (!sa::AliasNodes(wants, bank_of, H.nodes[sd], by_path, err, &missing)) {
      bad.push_back("AliasNodes refused: " + err);
      continue;
    }
    std::map<std::string, std::vector<BindRow>> rows_of;
    for (const auto& [path, v] : by_path) {
      std::string why;
      if (!BindSplit(v, rows_of[path], why)) bad.push_back("alias rows of " + path + " do not split: " + why);
    }
    for (const sc::Alias& a : T.lib.rows.alias) {
      const bool render = a.tag.compare(0, 3, "SSF") == 0;
      if (render && sd == 0 && false) continue;
      std::string name_s = a.ours;
      name_s.resize(8, '\0');
      sb::RetargetRowName(a.tag, a.kind, &name_s[0], sc::kRefSeat, s, sc::kRefBank, bank_s);
      name_s.resize(sc::AliasNameLen(a.kind));
      const std::string wl = std::string(sd ? "SD " : "HD ") + a.tag + " '" + a.game + "'";
      const std::string what = wl + " -> '" + name_s + "' of seat " + std::to_string(s);
      // the importer's old source of this name
      BindHit ref;
      if (a.tag == "EVZ ") {
        const bool victory = io::Trim(a.ours) == std::to_string(6000 + sc::kRefSeat);
        ref = BindFirstIn(H, sd, BindOrderBanks(H, sd, victory ? "Winning" : "Entrance"), "EVZ ", a.game);
      } else if (a.tag == "GMEV" || a.tag == "PCE6") {
        ref = BindFirstIn(H, sd, BindOrderFx(H, sd, a.tag == "GMEV" ? "pac\\gm" : "pac\\prtcls", own_pack_e), a.tag, a.game);
      } else if (a.tag == "MOVN") {
        std::vector<size_t> o = BindOrderFx(H, sd, "pac\\evt\\evtmov", own_pack_e, true);
        for (size_t i : BindOrderFx(H, sd, "pac\\evt\\evtmov2_2", own_pack_e)) o.push_back(i);
        ref = BindFirstIn(H, sd, o, "MOVN", a.game);
      } else if (render) {
        std::vector<size_t> o;
        for (size_t i = 0; i < H.pacs[sd].size(); ++i) {
          const std::string& l = H.pacs[sd][i].label;
          const bool menu = BindHas(l, std::string("menu") + (sd ? "SD" : "HD") + ".pac") || BindHas(l, std::string("DLC_") + (sd ? "SD" : "HD") + "_dlc");
          if (menu && BindPackOf(l) == own_pack_base) o.push_back(i);  // (BaseRenders: the base's own pack's DLC_HD / DLC_SD, else the disc's menu pac)
        }
        ref = BindFirstIn(H, sd, o, a.tag, a.game);
      }
      // what the game's list gives
      BindHit got;
      std::vector<sa::NodeView> all = H.nodes[sd];
      got.found = sa::FindFirst(all, a.tag, a.game, got.label, got.sector, got.size);
      const bool plate_skip = a.tag == "MOVN" && !sb::OwnPlate(s);
      const std::vector<BindRow>* rows = got.found && rows_of.count(got.label) ? &rows_of[got.label] : nullptr;
      const BindRow* hit = nullptr;
      if (rows)
        for (const BindRow& r : *rows)
          if (r.tag == a.tag && r.name == name_s) hit = &r;
      ++st.aliases;
      if (plate_skip) {
        if (hit) bad.push_back(what + ": a MOVN row for a seat with no plate of its own");
        continue;
      }
      if (!got.found) {
        if (ref.found) bad.push_back(what + ": the importer's old search found it in " + ref.label + ", the game's pac list has no such name");
        continue;
      }
      if (!hit) {
        bad.push_back(what + ": no row in the alias node of " + got.label);
        continue;
      }
      if (hit->sector != got.sector || hit->size != got.size) bad.push_back(what + ": the row's sector / size are not the game's entry");
      if (!ref.found) {
        st.log.push_back("claim 14: " + wl + " is in " + got.label + "; the importer's old search found nothing (the old build warned: not found)");
        ++st.claim14;
        continue;
      }
      const Bytes now = H.Row(sd, got.label, hit->sector, hit->size), was = H.Row(sd, ref.label, ref.sector, ref.size);
      if (now != was) {
        st.log.push_back("claim 14: " + wl + ": the game's first match is " + got.label + " (" + std::to_string(now.size()) + " bytes), the importer's old search took " + ref.label + " (" +
                         std::to_string(was.size()) + " bytes): the bytes DIFFER");
        ++st.claim14;
      } else if (got.label != ref.label) {
        st.log.push_back("claim 14: " + wl + ": the first match is " + got.label + ", the old search took " + ref.label + " (same bytes)");
        ++st.claim14;
      }
    }
  }
  return bad;
}

static std::string BindJoin(const std::vector<std::string>& v) {
  std::string all;
  for (size_t i = 0; i < v.size() && i < 12; ++i) all += " [" + v[i] + "]";
  if (v.size() > 12) all += " [... " + std::to_string(v.size() - 12) + " more]";
  return all;
}

// ---- (review suspicion a) a word of a real base row that equals a seat (or a seat's key range) only by chance, at an offset a value scan would move.
constexpr size_t kScanKeyBeg = sb::kRecHead + sb::kKeyBeg, kScanKeyEnd = sb::kRecHead + sb::kKeyEnd;
constexpr size_t kScanProSelf[4] = {576, 588, 590, 592};  // the PRO words the importer sets to the seat when they equal the base id
// Returns seat -> the base rows holding a word of that seat's key range (DAT +116..+199 / DSBD), as stock keys the importer keeps.
static std::map<int, std::set<uint32_t>> BindCoincidenceScan(const std::string& mods, int pack_id) {
  io::Pac p;
  std::string e;
  const fs::path f = Root(mods) / "frame" / "pac" / "ch" / ("chEtc_dlc" + Pp(pack_id) + ".pac");
  io::Entry *dt = nullptr, *ds = nullptr, *pr = nullptr;
  std::vector<io::Leaf> ld, lds, lp;
  if (!Say(io::ReadPac(Read(f), p, e) && (dt = p.Find("CHAR", "DAT ")) && (ds = p.Find("CHAR", "DSBD")) && (pr = p.Find("CHAR", "PRO ")) && io::ReadPach(dt->blob, ld) &&
               io::ReadPach(ds->blob, lds) && io::ReadPach(pr->blob, lp),
           "coincidence scan: the frame's chEtc (the top pack's rows) reads"))
    return {};
  std::map<uint32_t, Bytes> dat, dsbd, pro;
  for (const io::Leaf& l : ld) dat[l.id] = io::Decode(l.raw);
  for (const io::Leaf& l : lds) dsbd[l.id] = l.raw;
  for (const io::Leaf& l : lp) pro[l.id] = io::Decode(l.raw);
  int bases = 0, pro_hits = 0;
  std::map<int, std::set<uint32_t>> by_seat;  // the seat whose key range a stock word lies in -> the base rows holding it
  std::map<uint32_t, std::set<std::string>> where;  // the stock word -> "DAT +off" / "DSBD +off"
  auto seat_key = [](uint32_t v) { return v >= 39700 && v <= 49999; };
  for (const auto& [id, r] : dat) {
    if (id < 100 || id >= 397 || r.size() != 208 || io::L16(&r[4]) != id) continue;  // a base the importer can copy: a real row of his own id
    ++bases;
    for (size_t off = kScanKeyBeg; off < kScanKeyEnd; off += 2)
      if (const uint32_t v = io::L16(&r[off]); seat_key(v)) by_seat[int(v / 100)].insert(id), where[v].insert("DAT +" + std::to_string(off));
    if (const auto d = dsbd.find(id); d != dsbd.end())
      for (size_t off = 0; off + 1 < d->second.size(); off += 2)
        if (const uint32_t v = io::L16(&d->second[off]); seat_key(v)) by_seat[int(v / 100)].insert(id), where[v].insert("DSBD +" + std::to_string(off));
    if (const auto q = pro.find(id); q != pro.end() && q->second.size() == 1276)
      for (size_t off : kScanProSelf) {
        const uint32_t v = io::L16(&q->second[off]);
        if (v >= 397 && v <= 499 && v != id) ++pro_hits, std::printf("    PRO %u +%zu = %u\n", id, off, v);
      }
  }
  Say(bases > 200 && pro_hits == 0, "coincidence scan of the " + std::to_string(bases) + " base rows: no PRO self word (+576 +588 +590 +592) names a seat 397..499 (" + std::to_string(pro_hits) + ")");
  for (const auto& [seat, ids] : by_seat) {
    std::string l, w;
    for (uint32_t id : ids) l += " " + std::to_string(id);
    for (const auto& [v, at] : where)
      if (int(v / 100) == seat) {
        w += " " + std::to_string(v) + " (";
        for (const std::string& a : at) w += (w.back() == '(' ? "" : ", ") + a;
        w += ")";
      }
    std::printf("    NAMED stock keys in seat %d's key range:%s; held by %zu base row(s):%s\n", seat, w.c_str(), ids.size(), l.substr(0, 300).c_str());
  }
  return by_seat;
}

static void BindCases(const Env& env) {
  std::printf("the one seat-free character file against the old per-seat build (S110 LIBSIZE-2):\n");
  std::string pac;
  const std::string mods = FreshMods(env, "bind", &pac);
  struct Group { int base, ent; std::vector<int> seats; };
  // (entrance 385 has a name-plate movie, M885 in pack 4's evtmov: the MOVN row; 498 / 499 have no plate of their own, asset_ids.h:201; base 106's
  // DAT / DSBD hold the stock keys 40001 40101 40401 40601, in the key ranges of seats 400 401 404 406: the last group seats him on 401 404 406 456)
  const std::vector<Group> groups = {{107, 110, {397, 412, 482}}, {100, 100, {400, 455, 499}}, {107, 385, {403, 430, 498}}, {106, 110, {401, 404, 406, 456}}};
  importer::PackState st;
  for (const Group& g : groups)
    for (int s : g.seats) {
      st.stars.push_back(MakeStar(pac, s, "Bind Star", g.base, g.ent));
      if (g.base == 106)  // (106 has no renders of his own in menuHD / menuSD: the card gets the UI's three pictures, seat-free)
        for (int k = 0; k < 3; ++k) st.stars.back().render_rgba[k].assign(8 * 8 * 4, uint8_t(0x40 + 0x20 * k)), st.stars.back().render_w[k] = st.stars.back().render_h[k] = 8;
    }
  const Bytes source_bytes = Read(fs::u8path(pac));  // (the build removes the sources\ copy once his own file is his source: the reference is read first)
  if (!Say(Build(env, mods, st, env.dlc, ""), "BuildPack: the same source on seats 397 412 482 (base 107, entrance 110), 400 455 499 (base 100, entrance 100), 403 430 498 (base 107, "
                                              "entrance 385: a name plate), 401 404 406 456 (base 106, entrance 110)"))
    return;
  const BindSrc src = BindSource(env.source);
  Say(src.ok && !source_bytes.empty(), "the source's models read: " + std::to_string(src.emd.size()) + " (attire, kind) entries, first attire " + std::to_string(src.a0));
  for (const auto& l : importer::LastBuildLog())
    if (l.find("not in the game") != std::string::npos) std::printf("  | %s\n", l.c_str());
  importer::PackState back;
  std::string e;
  importer::LoadPack(mods, back, e);
  const int pack_id = PackIdOf(mods);
  const Bytes ft = Read(Root(mods) / "frame" / "superstars.txt");
  const seatsn::Frame fr = seatsn::ParseFrame(std::string(ft.begin(), ft.end()));
  std::map<int, BindInst> inst;
  bool all_read = true;
  for (const importer::Superstar& s : back.stars) {
    BindInst b;
    b.seat = s.slot, b.base = s.base_id, b.ent = s.entrance_from >= 0 ? s.entrance_from : s.base_id;
    const seatsn::FrameSeat* fs_ = fr.Find(s.slot);
    if (fs_) b.bank = fs_->bank, b.rank = fs_->rank;
    b.lib = ReadLib(mods, s.key);
    b.file = Read(Root(mods) / "library" / s.key / "pac" / "ch" / ("ch" + std::to_string(sc::kRefSeat) + ".pac"));
    const bool ok = fs_ && b.lib.ok && b.lib.rows.seat == sc::kRefSeat && b.lib.card.inst.seat == sc::kRefSeat && b.lib.card.inst.bank == sc::kRefBank &&
                    sc::FileForm(b.lib.card) == sc::kFileForm && !b.file.empty() && b.lib.card.seat == s.slot;
    all_read = all_read && ok;
    for (int sd = 0; sd < 2; ++sd)
      for (int k = 0; k < 3; ++k)
        if (!s.render_rgba[k].empty()) {
          static const int hw[3] = {512, 512, 256}, hh[3] = {512, 512, 256}, sw[3] = {512, 512, 256}, sh[3] = {256, 256, 256};
          const int dw = sd ? sw[k] : hw[k], dh = sd ? sh[k] : hh[k];
          (sd ? b.ui_sd : b.ui_hd)[size_t(k)] = io::BpeIdentity(importer::bc3::Dds(importer::bc3::Resize(s.render_rgba[k], s.render_w[k], s.render_h[k], dw, dh), dw, dh));
        }
    std::string evp, al;
    if (const io::Dir* d = DirOf(b.lib.pac, "EVP "))
      for (const io::Entry& x : d->ents) evp += " " + io::Trim(x.name) + "(" + std::to_string(io::Blob(*d, x).size()) + ")";
    std::map<std::string, int> by_tag;
    for (const sc::Alias& a : b.lib.rows.alias) ++by_tag[a.tag];
    for (const auto& [t, n] : by_tag) al += " " + t + "x" + std::to_string(n);
    std::printf("  info: seat %d key %s bank %d rank %d, file %zu bytes, %zu directories, EVP%s; alias rows%s\n", s.slot, s.key.c_str(), b.bank, b.rank, b.file.size(),
                b.lib.pac.dirs.size(), evp.c_str(), al.c_str());
    inst[s.slot] = std::move(b);
  }
  if (!Say(all_read && inst.size() == 13, "thirteen cards read back: seat-free (inst 497 / 9997, file_form 2, rows.bin for 497), one file each, the card's seat = his frame seat")) return;
  {
    int with_relo = 0, entries = 0, aliases = 0;
    for (const auto& [seat, b] : inst) with_relo += b.lib.rows.has_relo, entries += int(b.lib.rows.relo.size()), aliases += int(b.lib.rows.alias.size());
    Say(with_relo == 13 && entries > 13 * 50 && aliases >= 13 * 6, "every card's rows.bin holds the relocation list (" + std::to_string(entries) + " entries) and the ALIA block (" + std::to_string(aliases) + " alias rows)");
    bool dirs_ok = true;
    for (const auto& [seat, b] : inst)
      for (const io::Dir& d : b.lib.pac.dirs)
        if (d.tag != "EMD " && d.tag != "EVP " && d.tag.compare(0, 3, "SSF") != 0 && d.tag.compare(0, 3, "SDF") != 0) dirs_ok = false, std::printf("    seat %d holds a directory '%s'\n", seat, d.tag.c_str());
    bool ref_form = true;
    for (const auto& [seat, b] : inst)
      for (const sc::Alias& a : b.lib.rows.alias) ref_form = ref_form && sb::AliasRefForm(a);
    Say(ref_form, "every alias row is in the reference form (sb::AliasRefForm: the game's loader leaves a card with any other out)");
    Say(dirs_ok, "his file holds only what we make (EMD, EVP, the UI's SSFx / SDFx): no EVZ / GMEV / PCE6 / MOVN copy");
    std::error_code ec;
    Say(!fs::exists(Root(mods) / "library" / back.stars[0].key / "pac" / "ch" / "ch397_dlc06.pac", ec) && !fs::exists(Root(mods) / "sources" / "x", ec), "no ch<seat>_dlc<PP>.pac is written");
  }
  HostGame H;
  const bool host_ok = H.Open(env, mods);
  if (!Say(host_ok, "the host finder: the game's own pacs as a pac list (HD " + std::to_string(H.pacs[0].size()) + " pacs, SD " + std::to_string(H.pacs[1].size()) + ")")) return;
  {
    int plain = 0;
    for (const sa::NodeView& n : H.nodes[0]) plain += !n.rows.empty();
    std::printf("    %d of %zu pacs give node rows (the rest are not plain pacs and are never matched)\n", plain, H.nodes[0].size());
  }
  const std::string lib_root = Slash(Root(mods) / "library");
  std::map<int, int> pack_of;  // character id -> the DLC pack holding his model (-1 = the disc)
  for (const Group& g : groups)
    for (int c : {g.base, g.ent})
      if (!pack_of.count(c)) pack_of[c] = importer::HostPackOfChar(mods, env.gf, env.dlc, "", c);
  const std::map<int, std::set<uint32_t>> stock = BindCoincidenceScan(mods, pack_id);
  Say(stock.count(400) && stock.count(401) && stock.count(404) && stock.count(406), "the coincidence is real in the player's files: stock words of the base rows lie in the key ranges of seats 400 401 404 406 (the NAMED lines above)");
  // ---- every card against EVERY seat of the build (a library card is bound to any seat, a seated one is re-targeted to his own)
  std::vector<int> all_seats;
  for (const Group& g : groups)
    for (int s : g.seats) all_seats.push_back(s);
  BindStat stat;
  int cards_ok = 0, pairs = 0, pairs_ok = 0;
  std::set<std::string> logged;
  for (const Group& g : groups)
    for (int t : g.seats) {
      const BindInst& T = inst[t];
      int bad_n = 0;
      std::string first;
      BindStat cs;
      for (int s : all_seats) {
        const std::vector<std::string> bad = BindCard(H, T, s, inst[s].bank, src, lib_root, pack_of[T.ent], pack_of[T.base], cs);
        ++pairs, pairs_ok += bad.empty(), bad_n += !bad.empty();
        if (!bad.empty() && first.empty()) first = "seat " + std::to_string(s) + ":" + BindJoin(bad);
      }
      cards_ok += bad_n == 0;
      stat.rows += cs.rows, stat.blobs += cs.blobs, stat.aliases += cs.aliases, stat.claim14 += cs.claim14;
      for (const std::string& l : cs.log) logged.insert("card " + std::to_string(t) + " (entrance " + std::to_string(T.ent) + ", base " + std::to_string(T.base) + ") " + l);
      Say(bad_n == 0, "card made for seat " + std::to_string(t) + " (base " + std::to_string(T.base) + ", entrance " + std::to_string(T.ent) + "): bound to each of the 13 seats, his node rows (own rows only), EMD / render bytes, the EV file and every alias row equal the old per-seat build" +
                          (bad_n ? " -- " + std::to_string(bad_n) + " seat(s) differ, first " + first : std::string()));
    }
  Say(cards_ok == 13 && pairs == 13 * 13 && pairs_ok == pairs, "bind cases: " + std::to_string(pairs_ok) + " of " + std::to_string(pairs) + " (card, seat) pairs equal (" + std::to_string(stat.rows) + " node rows, " + std::to_string(stat.blobs) +
                                                                " EVP blobs + EV files, " + std::to_string(stat.aliases) + " alias rows resolved)");
  std::printf("  claim 14 (the game's first match against the importer's old search order): %d difference(s) over all pairs, %zu distinct:\n", stat.claim14, logged.size());
  for (const std::string& l : logged) std::printf("    %s\n", l.c_str());
  // ---- RetargetCard: the card's rows to every seat; identical settings = identical rows for any seat (the cards of one group differ only by their key), and the seat's own words are the seat's
  {
    int rows_ok = 0, rows_n = 0;
    std::string first;
    for (const Group& g : groups)
      for (int s : all_seats) {
        sc::SeatRows ref;
        std::string err;
        const bool r0 = sb::RetargetCard(inst[g.seats[0]].lib.rows, sc::kRefSeat, sc::kRefBank, s, inst[s].bank, pack_id, inst[s].rank, ref, err);
        bool okk = r0 && ref.seat == s && io::L16(&ref.dat[4]) == uint32_t(s) && io::L16(&ref.dat[4 + 2]) == uint32_t(inst[s].rank) && io::L16(&ref.pro[580]) == uint32_t(inst[s].bank);
        std::vector<std::string> bad;
        for (size_t i = 1; okk && i < g.seats.size(); ++i) {
          sc::SeatRows other;
          okk = sb::RetargetCard(inst[g.seats[i]].lib.rows, sc::kRefSeat, sc::kRefBank, s, inst[s].bank, pack_id, inst[s].rank, other, err);
          if (okk) BindCompareRows(other, ref, bad), okk = bad.empty();
        }
        ++rows_n, rows_ok += okk;
        if (!okk && first.empty()) first = "seat " + std::to_string(s) + " " + err + BindJoin(bad);
      }
    Say(rows_ok == rows_n, "rows.bin: RetargetCard(497 / 9997 -> each seat) of the cards of one group agree, the seat, the rank and the bank word are the seat's (" + std::to_string(rows_ok) + " of " + std::to_string(rows_n) + ")" + first);
  }
  // ---- 497 -> s -> 497 gives the file's own rows back
  for (const Group& g : groups) {
    const int s = g.seats[1];
    const BindInst& T = inst[g.seats[0]];
    std::vector<uint8_t> rt, m1, m2, base_rows;
    std::string why, err;
    bool back_ok = BindNodeRows(T.file, false, rt, why) && sb::RetargetDirRows(rt, sc::kRefSeat, s, sc::kRefBank, inst[s].bank, false, m1, err) &&
                   sb::RetargetDirRows(m1, s, sc::kRefSeat, inst[s].bank, sc::kRefBank, false, m2, err) && sb::RetargetDirRows(rt, sc::kRefSeat, sc::kRefSeat, sc::kRefBank, sc::kRefBank, false, base_rows, err);
    sc::SeatRows a, b2;
    std::vector<std::string> bad;
    back_ok = back_ok && sb::RetargetCard(T.lib.rows, sc::kRefSeat, sc::kRefBank, s, inst[s].bank, pack_id, inst[s].rank, a, err) &&
              sb::RetargetCard(a, s, inst[s].bank, sc::kRefSeat, sc::kRefBank, pack_id, 0, b2, err);
    Say(back_ok && m2 == base_rows, "497 -> " + std::to_string(s) + " -> 497: the node rows (his own rows) come back as they were; the rows re-target both ways" + err);
  }
  // ---- planted twins: each comparison above must fail for a card with one change
  {
    const int t = groups[0].seats[0], s = groups[0].seats[1];
    BindStat sink;
    auto caught = [&](const std::string& name, const std::function<void(BindInst&)>& mutate) {
      BindInst x = inst[t];
      mutate(x);
      Twin(name, !BindCard(H, x, s, inst[s].bank, src, lib_root, pack_of[x.ent], pack_of[x.base], sink).empty());
    };
    auto file_dir_row = [](BindInst& x, const char* tag, size_t field) {  // a byte of the first row of directory `tag` in his pac file
      for (size_t p = io::kSector; p + 12 <= io::kHeader; p += 4)
        if (std::memcmp(&x.file[p], tag, 4) == 0) { x.file[p + 12 + field] ^= 1; return; }
    };
    caught("EMD: a model byte in his file", [](BindInst& x) {
      for (size_t p = io::kSector; p + 12 <= io::kHeader; p += 4)
        if (std::memcmp(&x.file[p], "EMD ", 4) == 0) {
          const uint32_t sec = io::L32(&x.file[p + 12 + 8]);
          x.file[io::kHeader + size_t(sec) * io::kSector + 100] ^= 1;
          return;
        }
    });
    caught("EMD: the first row's name (a model of another id)", [&](BindInst& x) { file_dir_row(x, "EMD ", 3); });
    caught("EVP: a byte of the first blob's decoded table", [](BindInst& x) {
      for (io::Dir& d : x.lib.pac.dirs)
        if (d.tag == "EVP " && !d.ents.empty()) d.ents[0].blob[sb::kEvBlobHead + sb::kEvBankOff + 1] ^= 1;
    });
    caught("alias: a game name that no pac holds", [](BindInst& x) { if (!x.lib.rows.alias.empty()) x.lib.rows.alias[0].game[3] = 'Z'; });
    caught("alias: a GMEV alias that points at another bank", [](BindInst& x) { for (sc::Alias& a : x.lib.rows.alias) if (a.tag == "GMEV") { a.game = "0001"; return; } });
    caught("alias: a PCE6 alias dropped (its row is missing for the seat)", [](BindInst& x) { x.lib.rows.alias.erase(std::remove_if(x.lib.rows.alias.begin(), x.lib.rows.alias.end(), [](const sc::Alias& a) { return a.tag == "PCE6"; }), x.lib.rows.alias.end()); });
    caught("alias: an EVZ alias that names the wrong bank", [](BindInst& x) { for (sc::Alias& a : x.lib.rows.alias) if (a.tag == "EVZ ") { a.game = io::Pad("77", 8); return; } });
    caught("renders: an SSF alias that names another wrestler's picture", [](BindInst& x) { for (sc::Alias& a : x.lib.rows.alias) if (a.tag == "SSFA") { a.game = "0399"; return; } });
    caught("the model of the source: a different attire map (an expected entry dropped)", [](BindInst& x) { x.file.resize(io::kHeader); });
  }
}
