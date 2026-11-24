// S110 SEAT3 lane G-A: the unit of src/seat_bind_core.h (the pure core of "any library character in any free seat", docs/research/s110/SEAT3.md).
//   bind_unit.exe      every check must PASS (prints "PASS bind_unit" last); planted twins: tools/seats/bind_selftest.py
// Every payload byte is a made-up pattern (never copied from a game file).
//
// THE BAR (RetargetCard): rows made for seat t and re-targeted to s equal what the importer writes for s directly. Reference bytes:
//   * ENEX BIN / LSD rows: the importer's OWN functions (src/importer/enex_rows.h PatchEnexBin / PatchEnexLsd, asset_ids.h) run on a made-up ENEX
//     pac, read back as library.h EnexRowsOf does (transcribed below: EnexRowsOf needs the importer's Ctx, which needs game files);
//   * DAT / DSBD / PRO rows: library.h StarRows needs the player's chEtc, so its per-star half is TRANSCRIBED below from the source (RefRows);
//     the main session adds the importer-side equality case on real files (importer test_core);
//   * node rows: importer io::WritePac + seats_core.h NodeRows (the game side's own reader) on made-up pacs;
//   * EV blobs: importer_core.cpp RetargetEv's rule (transcribed: RefEv) through io::BpeIdentity (the real function).
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "importer/asset_ids.h"
#include "importer/enex_rows.h"
#include "importer/pack_io.h"
#include "letter_jump_core.h"
#include "seat_bind_core.h"
#include "seats_core.h"

namespace sb = wwe2k14::seat_bind;
namespace sc = wwe2k14::seat_card;
namespace io = importer::io;
using Bytes = io::Bytes;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* name, const std::string& detail = "") {
  std::printf("%s  %-72s %s\n", ok ? "PASS" : "FAIL", name, ok ? "" : detail.c_str());
  (ok ? g_pass : g_fail)++;
}
static std::string N(int64_t v) { return std::to_string(v); }

static void Fill(uint8_t* p, size_t n, int seed) {
  for (size_t i = 0; i < n; ++i) p[i] = uint8_t((i * 31 + size_t(seed) * 17 + 5) & 0xFF);
}
static uint16_t G16(const uint8_t* p, size_t off) { return io::L16(p + off); }

// ============================================================ the made-up base wrestler (what StarRows reads from the player's chEtc)
struct Base {
  Bytes rec, ds, pr, pe;  // DAT 208 (file form), DSBD 80, PRO 1276, the entrance wrestler's PRO (when ent != src)
  int src = 150, ent = 150, ebank = 160;
};
static Base MakeBase(bool moved_entrance) {
  Base b;
  b.ent = moved_entrance ? 160 : 150;
  b.rec.assign(208, 0), b.ds.assign(80, 0), b.pr.assign(1276, 0), b.pe.assign(1276, 0);
  Fill(b.rec.data(), 208, 1), Fill(b.ds.data(), 80, 2), Fill(b.pr.data(), 1276, 3), Fill(b.pe.data(), 1276, 4);
  uint8_t* R = &b.rec[4];
  io::W16(R + 0, 150), io::W16(R + 2, 3), io::W16(R + 4, 150), io::W16(R + 200, 150);
  std::memset(R + 116, 0, 84);
  io::W16(R + 116, 15000), io::W16(R + 118, 15005), io::W16(R + 120, 22203), io::W16(R + 122, 15099), io::W16(R + 196, 15007), io::W16(R + 198, 15042);
  io::W16(R + 100, 15003);  // outside the key range: a chance twin
  // STOCK words that fall in seat 397's key range (the real base 106 holds 40001 / 40101 / 40401 / 40601): the importer keeps them for every seat, they are
  // never recorded, so the bind must never move them (a card made for seat 397 cannot tell them from its own keys by value)
  io::W16(R + 124, 39701), io::W16(R + 126, 39702);
  std::memset(b.ds.data(), 0, 80);
  io::W16(&b.ds[0], 15000), io::W16(&b.ds[2], 15001), io::W16(&b.ds[4], 22204), io::W16(&b.ds[6], 15010), io::W16(&b.ds[40], 15099);
  io::W16(&b.ds[8], 39703);  // a stock word in seat 397's range (see above)
  const int words[][2] = {{576, 150}, {578, 700}, {580, 160}, {582, 160}, {584, 161}, {586, 710}, {588, 150}, {590, 0x1234}, {592, 150},
                          {594, 701}, {596, 160}, {598, 162}, {600, 160}, {602, 711}};
  for (const auto& w : words) io::W16(&b.pr[size_t(w[0])], uint16_t(w[1]));
  for (int k = 0; k < 5; ++k) io::W16(&b.pr[622 + 34 * size_t(k)], uint16_t(sb::PartBank(k, 160)));
  if (moved_entrance) {
    const int pw[][2] = {{578, 720}, {580, 170}, {582, 170}, {584, 171}, {586, 730}, {594, 721}, {596, 170}, {598, 172}, {600, 170}, {602, 731}};
    for (const auto& w : pw) io::W16(&b.pe[size_t(w[0])], uint16_t(w[1]));
    b.ebank = 170;
  } else {
    b.ebank = 160;
  }
  return b;
}
// library.h StarRows, per star, transcribed (tron choices omitted: no custom tron). Every seat-bearing word goes through relo::Rec::Put, exactly as there.
static void RefRows(const Base& B, int slot, int bank, int rank, int pack, sc::SeatRows& r) {
  using RK = importer::relo::sc::RelKind;
  importer::relo::Rec rel;
  const uint32_t src = uint32_t(B.src);
  const uint16_t att_lo = uint16_t(src * 100), att_hi = uint16_t(src * 100 + 99);
  auto att = [&](uint16_t v) { return uint16_t(v - att_lo + uint32_t(slot) * 100); };
  Bytes rec = B.rec;
  uint8_t* R = &rec[4];
  for (int off : {0, 4, 200}) rel.Put(rec, "DAT ", 0, 4 + size_t(off), RK::Seat16, 0, uint32_t(slot));
  rel.Put(rec, "DAT ", 0, 4 + 2, RK::Rank16, 0, uint32_t(rank));
  for (int fam = 0; fam < 3; ++fam) rel.Put(rec, "DAT ", 0, 4 + 6 + 2 * size_t(fam), RK::Text16, fam, 60000 + 2000 * uint32_t(fam) + uint32_t(slot));
  io::W32(R + 12, 99999);
  R[16] = 1;
  rel.Put(rec, "DAT ", 0, 4 + 17, RK::Pack8, 0, uint32_t(pack));
  for (int off = 116; off < 200; off += 2) {
    const uint16_t v = io::L16(R + off);
    if (v >= att_lo && v <= att_hi) rel.Put(rec, "DAT ", 0, 4 + size_t(off), RK::Key16, 0, att(v));
  }
  Bytes ds = B.ds;
  for (size_t off = 0; off + 1 < ds.size(); off += 2) {
    const uint16_t v = io::L16(&ds[off]);
    if (v >= att_lo && v <= att_hi) rel.Put(ds, "DSBD", 0, off, RK::Key16, 0, att(v));
  }
  Bytes pr = B.pr;
  const Bytes& ef = B.ent != B.src ? B.pe : pr;
  const int ebank = io::L16(&ef[580]);
  for (size_t off = 576; off < 604; off += 2) {
    const bool ref = off == 578 || off == 586 || off == 594 || off == 602;
    if (ref) {
      if (B.ent != B.src) io::W16(&pr[off], io::L16(&B.pe[off]));
    } else if (off == 580 || off == 596) {
      rel.Put(pr, "PRO ", 0, off, RK::Bank16, 0, uint32_t(bank));
    } else if (off == 582 || off == 584 || off == 598 || off == 600) {
      const uint16_t v = io::L16(&ef[off]);
      if (v == ebank) rel.Put(pr, "PRO ", 0, off, RK::Bank16, 0, uint32_t(bank));
      else io::W16(&pr[off], v);
    } else if (io::L16(&pr[off]) == src) {
      rel.Put(pr, "PRO ", 0, off, RK::Seat16, 0, uint32_t(slot));
    }
  }
  std::copy(rec.begin(), rec.end(), r.dat.begin());
  std::copy(pr.begin(), pr.end(), r.pro.begin());
  std::copy(ds.begin(), ds.end(), r.dsbd.begin());
  r.seat = slot;
  r.relo = std::move(rel.list), r.has_relo = true;
}

// ============================================================ the made-up ENEX pac (the top pack's two tables) and the importer's own functions on it
static Bytes Leaf5(int e, int src_bank) {
  Bytes m(16 + 16 * 2, 0);
  Fill(m.data(), m.size(), 5);
  io::W16(&m[0], 2);
  io::W16(&m[16 + 0], uint16_t(e)), io::W16(&m[16 + 2], 0x0707);
  for (int k = 0; k < 5; ++k) io::W16(&m[16 + 4 + 2 * size_t(k)], uint16_t(sb::PartBank(k, src_bank)));
  io::W16(&m[32 + 0], 160);
  for (int k = 0; k < 5; ++k) io::W16(&m[32 + 4 + 2 * size_t(k)], uint16_t(7000 + k));
  return m;
}
static importer::io::Pac MakeEnex(int e, int src_bank) {
  importer::io::Pac pac;
  io::Dir d;
  d.tag = "ENEX", d.kind = 1;
  std::vector<io::Leaf> bin, lsd;
  for (int k = 0; k < 5; ++k) {
    Bytes m(16 + 20 * 3, 0);
    Fill(m.data(), m.size(), 10 + k);
    io::W16(&m[0], 3);
    for (int r = 0; r < 3; ++r) {
      const size_t b = 16 + 20 * size_t(r);
      io::W16(&m[b + 0], uint16_t(r)), io::W32(&m[b + 4], r == 0 ? uint32_t(60000 + e) : uint32_t(12345 + r));
      io::W16(&m[b + 8], r == 0 ? uint16_t(sb::PartBank(k, src_bank)) : uint16_t(900 + 10 * k + r)), io::W16(&m[b + 16], uint16_t(50 + r));
    }
    bin.push_back({uint32_t(k), m});
  }
  bin.push_back({5, Leaf5(e, src_bank)});
  const int lists[] = {2, 3, 17, 16, 8, 9, 10, 11};
  for (int list : lists) {
    Bytes m(2 + 8 * 2, 0);
    Fill(m.data(), m.size(), 30 + list);
    io::W16(&m[0], 2);
    int value = list == 2 ? e : (list == 3 || list == 17) ? src_bank : -1;
    if (value < 0) {
      const int k = list == 16 ? 0 : list - 7;  // lists 8 9 10 11 = parts 1..4
      value = int(sb::PartBank(k, src_bank));
    }
    io::W16(&m[2], uint16_t(value)), io::W16(&m[4], 7), io::W32(&m[6], 4242);
    io::W16(&m[10], uint16_t(5000 + list)), io::W16(&m[12], 8), io::W32(&m[14], 4343);
    lsd.push_back({uint32_t(list), m});
  }
  io::Entry eb, el;
  eb.name = "BIN ", eb.blob = io::WritePach(bin);
  el.name = "LSD ", el.blob = io::WritePach(lsd);
  d.ents.push_back(eb), d.ents.push_back(el);
  pac.dirs.push_back(d);
  return pac;
}
// library.h EnexRowsOf's read-back (lines 516-543), transcribed.
static bool EnexFor(const importer::io::Pac& base, int slot, int bank, int e, int src_bank, sc::SeatRows& r, std::string& err) {
  importer::io::Pac b0 = base;
  importer::enex_rows::Table t0, l0;
  if (!t0.Open(b0, "BIN ") || !l0.Open(b0, "LSD ")) return err = "base tables", false;
  size_t n_leaf[6] = {};
  std::map<uint32_t, size_t> n_list;
  for (uint32_t k = 0; k <= 5; ++k) n_leaf[k] = t0.data.count(k) && t0.data[k].size() >= 2 ? io::L16(&t0.data[k][0]) : 0;
  for (auto& [id, m] : l0.data) n_list[id] = m.size() >= 2 ? io::L16(&m[0]) : 0;
  importer::asset_ids::Config().assigned[slot] = bank;
  importer::Superstar s;
  s.slot = slot, s.base_id = e, s.entrance_from = -1, s.src_bank = src_bank, s.name = "Made Up";
  importer::io::Pac pac = base;
  std::map<int, std::set<int>> banks_of_star, parts;
  for (int k = 0; k < 5; ++k) banks_of_star[0].insert(int(sb::PartBank(k, bank)));
  std::vector<std::string> log;
  std::vector<importer::relo::Rec> recs(1);
  if (!importer::enex_rows::PatchEnexBin(log, pac, {s}, banks_of_star, parts, err, &recs) || !importer::enex_rows::PatchEnexLsd(log, pac, {s}, parts, err, &recs)) return false;
  r.relo.insert(r.relo.end(), recs[0].list.begin(), recs[0].list.end());
  importer::enex_rows::Table bin, lsd;
  if (!bin.Open(pac, "BIN ") || !lsd.Open(pac, "LSD ")) return err = "read back", false;
  const Bytes& l5 = bin.data[5];
  const size_t n5 = io::L16(&l5[0]);
  bool got5 = false;
  for (size_t i = 0; i < n5 && !got5; ++i)
    if (io::L16(&l5[16 + 16 * i]) == uint32_t(slot)) std::copy(l5.begin() + ptrdiff_t(16 + 16 * i), l5.begin() + ptrdiff_t(32 + 16 * i), r.bin5.begin()), got5 = true;
  if (!got5) return err = "no template row", false;
  r.binp.clear(), r.lsd.clear();
  for (uint32_t k = 0; k < 5; ++k) {
    const Bytes& m = bin.data[k];
    if (io::L16(&m[0]) != n_leaf[k] + 1) continue;
    sc::SeatRows::Binp x;
    x.k = uint8_t(k);
    std::copy(m.begin() + ptrdiff_t(16 + 20 * n_leaf[k]), m.begin() + ptrdiff_t(36 + 20 * n_leaf[k]), x.row.begin());
    r.binp.push_back(x);
  }
  for (uint32_t id : {2u, 3u, 17u, 16u, 8u, 9u, 10u, 11u}) {
    const auto it = lsd.data.find(id);
    if (it == lsd.data.end()) continue;
    if (io::L16(&it->second[0]) != n_list[id] + 1) continue;
    sc::SeatRows::Lsd x;
    x.list = uint8_t(id);
    std::copy(it->second.begin() + ptrdiff_t(2 + 8 * n_list[id]), it->second.begin() + ptrdiff_t(10 + 8 * n_list[id]), x.row.begin());
    r.lsd.push_back(x);
  }
  return true;
}
// All the card's rows for seat `slot`, as the importer makes them.
static bool MadeFor(const Base& B, int slot, int bank, int rank, int pack, sc::SeatRows& r, std::string& err) {
  r = sc::SeatRows();
  RefRows(B, slot, bank, rank, pack, r);
  const int e = B.ent;
  if (!EnexFor(MakeEnex(e, B.ebank), slot, bank, e, B.ebank, r, err)) return false;
  r.has_t102 = true, r.t102_src_bank = uint32_t(B.ebank);
  r.names[0] = "Made Up", r.names[1] = "M. Up", r.names[2] = "Made Up";
  return true;
}
static std::string Diff(const sc::SeatRows& a, const sc::SeatRows& b) {
  if (a.seat != b.seat) return "seat " + N(a.seat) + " vs " + N(b.seat);
  auto cmp = [](const char* n, const uint8_t* x, const uint8_t* y, size_t len) -> std::string {
    for (size_t i = 0; i < len; ++i)
      if (x[i] != y[i]) return std::string(n) + " +" + N(int64_t(i)) + ": " + N(x[i]) + " vs " + N(y[i]);
    return {};
  };
  std::string d;
  if (!(d = cmp("DAT", a.dat.data(), b.dat.data(), a.dat.size())).empty()) return d;
  if (!(d = cmp("PRO", a.pro.data(), b.pro.data(), a.pro.size())).empty()) return d;
  if (!(d = cmp("DSBD", a.dsbd.data(), b.dsbd.data(), a.dsbd.size())).empty()) return d;
  if (!(d = cmp("BIN5", a.bin5.data(), b.bin5.data(), a.bin5.size())).empty()) return d;
  if (a.binp.size() != b.binp.size()) return "BINP count " + N(int64_t(a.binp.size())) + " vs " + N(int64_t(b.binp.size()));
  for (size_t i = 0; i < a.binp.size(); ++i) {
    if (a.binp[i].k != b.binp[i].k) return "BINP leaf order";
    if (!(d = cmp(("BINP" + N(a.binp[i].k)).c_str(), a.binp[i].row.data(), b.binp[i].row.data(), 20)).empty()) return d;
  }
  if (a.lsd.size() != b.lsd.size()) return "LSD count";
  for (size_t i = 0; i < a.lsd.size(); ++i) {
    if (a.lsd[i].list != b.lsd[i].list) return "LSD list order";
    if (!(d = cmp(("LSD" + N(a.lsd[i].list)).c_str(), a.lsd[i].row.data(), b.lsd[i].row.data(), 8)).empty()) return d;
  }
  if (a.has_relo != b.has_relo || a.relo.size() != b.relo.size()) return "RELO: " + N(int64_t(a.relo.size())) + " entries vs " + N(int64_t(b.relo.size()));
  for (size_t i = 0; i < a.relo.size(); ++i) {
    const sc::Reloc &x = a.relo[i], &y = b.relo[i];
    if (x.tag != y.tag || x.sub != y.sub || x.off != y.off || x.kind != y.kind || x.arg != y.arg) return "RELO entry " + N(int64_t(i)) + " (" + x.tag + " +" + N(x.off) + ")";
  }
  if (a.has_t102 != b.has_t102 || a.t102_src_bank != b.t102_src_bank) return "T102";
  for (int i = 0; i < 3; ++i)
    if (a.names[i] != b.names[i]) return "NAME " + N(i);
  return {};
}

// ============================================================ made-up node rows (the importer's directory names, Instantiate's lines 576-716)
static importer::io::Pac MakePac(int slot, int bank, bool evp) {
  importer::io::Pac pac;
  int idx = 0;
  auto blob = [&](bool seat_dependent) {
    ++idx;
    Bytes b(40 + 29 * size_t(idx), uint8_t(idx));
    if (seat_dependent) b[0] = uint8_t(slot);  // content that differs per seat (the EVP blobs); same SIZE
    return b;
  };
  auto dir = [&](const char* tag, uint16_t kind, const std::vector<std::string>& names, bool dep = false) {
    io::Dir d;
    d.tag = tag, d.kind = kind;
    for (const std::string& n : names) {
      io::Entry e;
      e.name = n, e.blob = blob(dep);
      d.ents.push_back(e);
    }
    pac.dirs.push_back(d);
  };
  const std::string s3 = sb::Digits3(slot);
  dir("EMD ", 1, {"000" + s3 + "01", "000" + s3 + "12", "000" + s3 + "22"});
  io::Entry al;  // an attire alias, as Instantiate adds
  al.name = "000" + s3 + "32", al.alias = 0;
  pac.dirs[0].ents.push_back(al);
  if (evp) {
    std::vector<std::string> evn = {io::Pad(std::to_string(bank), 8), io::Pad(std::to_string(10000 + bank), 8), io::Pad(std::to_string(12000 + bank), 8),
                                    io::Pad(std::to_string(6000 + slot), 8)};
    dir("EVP ", 1, evn, true);
    dir("EVZ ", 1, evn);
  } else {
    std::vector<std::string> evn = {io::Pad(std::to_string(bank), 8), io::Pad(std::to_string(10000 + bank), 8), io::Pad(std::to_string(12000 + bank), 8),
                                    io::Pad(std::to_string(6000 + slot), 8)};
    // (the EVP blobs still take their place in the numbering so the other blobs are the same as with evp = true: skip 4 blobs)
    idx += 4;
    dir("EVZ ", 1, evn);
  }
  char fx[8], mv[8], rn[8];
  std::snprintf(fx, sizeof fx, "%04d", bank), std::snprintf(mv, sizeof mv, "M%03d", 500 + slot), std::snprintf(rn, sizeof rn, "%04d", slot);
  dir("GMEV", 0, {fx});
  dir("PCE6", 0, {fx});
  dir("MOVN", 0, {mv});
  for (const char* tag : {"SSFA", "SSFB", "SSFC", "SDFA", "SDFB", "SDFC"}) dir(tag, 0, {rn});
  return pac;
}
static bool NodeOf(int slot, int bank, bool evp, bool sd, std::vector<uint8_t>& rows, std::string& why) {
  importer::io::Pac p = MakePac(slot, bank, evp);
  const Bytes f = io::WritePac(p);
  return !f.empty() && wwe2k14::seats::NodeRows(std::vector<uint8_t>(f.begin(), f.begin() + 0x4000), f.size(), sd, rows, why);
}
// The directories of node rows whose tag is in `keep` (copied whole, in order) + the terminator word: what the file's own directories are (S110 LIBSIZE).
static std::vector<uint8_t> KeepTags(const std::vector<uint8_t>& rows, const std::set<std::string>& keep) {
  std::vector<uint8_t> o;
  size_t body = 0;
  std::string why;
  if (!sb::ScanDirs(rows.data(), rows.size(), body, why)) return o;
  for (size_t at = 0; at < body;) {
    const size_t len = 12 + size_t(sb::Be32(&rows[at + 4]) >> 20) * 4;
    if (keep.count(std::string(reinterpret_cast<const char*>(&rows[at]), 4))) o.insert(o.end(), rows.begin() + ptrdiff_t(at), rows.begin() + ptrdiff_t(at + len));
    at += len;
  }
  o.insert(o.end(), rows.begin() + ptrdiff_t(body), rows.end());
  return o;
}
static std::vector<std::string> TagsOf(const std::vector<uint8_t>& rows) {
  std::vector<std::string> v;
  size_t body = 0;
  std::string why;
  if (!sb::ScanDirs(rows.data(), rows.size(), body, why)) return v;
  for (size_t at = 0; at < body; at += 12 + size_t(sb::Be32(&rows[at + 4]) >> 20) * 4) v.emplace_back(reinterpret_cast<const char*>(&rows[at]), 4);
  return v;
}
// A made-up directory appended to `v`: rows {name, sector, size}.
struct RowSpec { std::string name; uint32_t sector, size; };
static void AddDirR(std::vector<uint8_t>& v, const char* tag, int kind, const std::vector<RowSpec>& rows, uint32_t extra_flags = 0) {
  const size_t rs = kind ? 16 : 12;
  uint8_t w[4];
  v.insert(v.end(), tag, tag + 4);
  sb::PutBe32(w, uint32_t(rows.size() * rs / 4) << 20 | (kind ? 0x1000u : 0u) | extra_flags), v.insert(v.end(), w, w + 4);
  sb::PutBe32(w, 0), v.insert(v.end(), w, w + 4);
  for (const RowSpec& r : rows) {
    std::string n = r.name;
    n.resize(rs - 8, ' ');
    v.insert(v.end(), n.begin(), n.end());
    sb::PutBe32(w, r.sector), v.insert(v.end(), w, w + 4);
    sb::PutBe32(w, r.size), v.insert(v.end(), w, w + 4);
  }
}
// rows named `names`: sector i + 1, size i + 2
static void AddDir(std::vector<uint8_t>& v, const char* tag, int kind, const std::vector<std::string>& names, uint32_t extra_flags = 0) {
  std::vector<RowSpec> rows;
  for (size_t i = 0; i < names.size(); ++i) rows.push_back({names[i], uint32_t(i + 1), uint32_t(i + 2)});
  AddDirR(v, tag, kind, rows, extra_flags);
}
static void EndRows(std::vector<uint8_t>& v) { v.insert(v.end(), 4, 0xFF); }
static const std::set<std::string> kOwnTags = {"EMD ", "SSFA", "SSFB", "SSFC", "SDFA", "SDFB", "SDFC"};

// ============================================================ made-up EV tables
static Bytes MakeEv(int e, int header_bank, size_t tail) {
  const int n = 6;
  Bytes ev(0x44 + 32 * size_t(n) + tail, 0);
  Fill(ev.data(), ev.size(), 77);
  std::memcpy(ev.data(), "0FOP", 4);
  io::WB16(&ev[0x20], uint16_t(header_bank));
  io::WB32(&ev[0x40], uint32_t(n));
  for (int i = 0; i < n; ++i) {
    uint8_t* r = &ev[0x44 + 32 * size_t(i)];
    uint32_t x = uint32_t(e) * 100 + uint32_t(i) * 11;
    uint8_t kind = 1;
    if (i == 3) x = 22200u + 5;           // another character's actor: stays
    if (i == 4) kind = 2, x = 39700u + 44;  // not an actor model key (kind 2) though it holds a key of seat 397: stays
    if (i == 5) x = uint32_t(e) * 100 + 99;  // the top of the key range
    io::WB32(r + 12, x);
    r[16] = kind;
  }
  return ev;
}
// importer_core.cpp RetargetEv's rule on the decoded table (transcribed), then the real BpeIdentity.
static Bytes RefEv(Bytes ev, int from, int to_bank, int to_char, uint32_t version = 0x100) {
  io::WB16(&ev[0x20], uint16_t(to_bank));
  const uint32_t n = io::B32(&ev[0x40]) & 0xFFFFFF;
  for (uint32_t i = 0; i < n && 0x44 + 32ull * (i + 1) <= ev.size(); ++i) {
    uint8_t* r = &ev[0x44 + 32 * size_t(i)];
    const uint32_t x = io::B32(r + 12);
    if (r[16] == 1 && int(x / 100) == from) io::WB32(r + 12, uint32_t(to_char) * 100 + x % 100);
  }
  return io::BpeIdentity(ev, version);
}

// ============================================================ the page helpers
static std::vector<sb::Entry> Lib(const std::vector<std::pair<std::string, std::string>>& v, int first_key = 1) {
  std::vector<sb::Entry> out;
  for (const auto& [k, n] : v) {
    sb::Entry e;
    e.key = k, e.name = n;
    out.push_back(e);
  }
  (void)first_key;
  return out;
}
static void Apply(sb::State& st, const sb::Plan& p) {
  for (const sb::Step& s : p.steps)
    if (!s.leave.empty()) st.guests.erase(s.seat);
  for (const sb::Step& s : p.steps)
    if (!s.arrive.empty()) st.guests[s.seat] = s.arrive;
}
static std::string Dump(const sb::State& st) {
  std::string r;
  for (const auto& [s, k] : st.guests) r += N(s) + ":" + k + " ";
  return r;
}
static std::string Steps(const sb::Plan& p) {
  std::string r;
  for (const sb::Step& s : p.steps) r += N(s.seat) + "[" + s.leave + ">" + s.arrive + "] ";
  return r;
}
static std::string Names(const std::vector<sb::Entry>& v) {
  std::string r;
  for (const auto& e : v) r += e.key + " ";
  return r;
}
static std::string LetterKey(const sb::Entry* e) { return e ? e->key : std::string("(null)"); }

int main() {
  namespace sbm = sb;
  // ============================== the moved helpers (their cases from seats_proto_unit.cpp)
  {
    uint8_t from_row[sb::kRowSize] = {}, to_row[sb::kRowSize] = {}, out[sb::kRowSize] = {};
    auto synth = [](uint8_t* row, int id, int rank, int key_from) {
      std::memset(row, 0, sb::kRowSize);
      sb::PutBe16(row + 0, uint16_t(id)), sb::PutBe16(row + 2, uint16_t(rank)), sb::PutBe16(row + 4, uint16_t(id));
      sb::PutBe16(row + 6, uint16_t(60000 + id)), sb::PutBe16(row + 8, uint16_t(62000 + id)), sb::PutBe16(row + 10, uint16_t(64000 + id));
      row[16] = 1, row[17] = 6;
      for (int i = 0; i < 4; ++i) sb::PutBe16(row + 116 + 2 * i, uint16_t(key_from * 100 + 1 + i));
      sb::PutBe16(row + 124, uint16_t(key_from * 100 + 99)), sb::PutBe16(row + 126, uint16_t(key_from * 100 + 100)), sb::PutBe16(row + 128, uint16_t(key_from * 100 - 1));
      sb::PutBe16(row + 194, uint16_t(key_from * 100 + 7)), sb::PutBe16(row + 200, uint16_t(id));
    };
    synth(from_row, 398, 9, 398), synth(to_row, 397, 8, 397);
    sb::PutBe16(from_row + 70, 39805), sb::PutBe16(from_row + 100, 39800), sb::PutBe16(from_row + 114, 39802), sb::PutBe16(from_row + 196, 39803), sb::PutBe16(from_row + 198, 39804);
    int keys = 0;
    std::string why;
    const bool ok = sb::MakeRow(from_row, to_row, 397, 398, out, keys, why);
    Check(ok && why.empty(), "moved: MakeRow accepts two active imported rows", why);
    Check(sb::Be16(out + 0) == 397 && sb::Be16(out + 4) == 397 && sb::Be16(out + 200) == 397, "moved: the id halfwords +0 / +4 / +200 are TO");
    Check(sb::Be16(out + 2) == 8, "moved: TO's own rank (+2) is kept");
    Check(sb::Be16(out + 6) == 60397 && sb::Be16(out + 8) == 62397 && sb::Be16(out + 10) == 64397, "moved: TO's name text ids stay");
    Check(sb::Be16(out + 116) == 39701 && sb::Be16(out + 124) == 39799 && sb::Be16(out + 194) == 39707, "moved: keys 398nn -> 397nn incl. the top and +194");
    Check(sb::Be16(out + 126) == 39900 && sb::Be16(out + 128) == sb::Be16(from_row + 128), "moved: a key field one past / one below the range is not a key");
    Check(keys == 8, "moved: eight keys changed", N(keys));
    Check(sb::Be16(out + 70) == 39805 && sb::Be16(out + 100) == 39800 && sb::Be16(out + 114) == 39802, "moved: chance twins outside +116..+198 are not rewritten");
    Check(sb::Be16(out + 196) == 39703 && sb::Be16(out + 198) == 39704, "moved: +196 / +198 (the importer's range) ARE rewritten");
    uint8_t inactive[sb::kRowSize];
    std::memcpy(inactive, to_row, sb::kRowSize);
    inactive[16] = 0;
    Check(!sb::CheckSeats(397, 397, to_row, from_row).empty(), "moved: refused: same id");
    Check(!sb::CheckSeats(300, 398, to_row, from_row).empty() && !sb::CheckSeats(397, 343, to_row, from_row).empty() && !sb::CheckSeats(397, 500, to_row, from_row).empty(),
          "moved: refused: an id that is not an imported seat");
    Check(!sb::CheckSeats(399, 398, to_row, from_row).empty(), "moved: refused: 399 is the importer's reserved slot");
    Check(!sb::CheckSeats(397, 398, inactive, from_row).empty() && !sb::CheckSeats(397, 398, to_row, inactive).empty(), "moved: refused: an inactive row");
    Check(!sb::CheckSeats(397, 398, nullptr, from_row).empty() && !sb::CheckSeats(397, 398, to_row, nullptr).empty(), "moved: refused: a null row (never a crash)");
    // directory rows
    auto add_dir = [](std::vector<uint8_t>& v, const char* tag, int kind, const std::vector<std::string>& names) {
      const size_t rs = kind ? 16 : 12;
      uint8_t w[4];
      v.insert(v.end(), tag, tag + 4);
      sb::PutBe32(w, uint32_t(names.size() * rs / 4) << 20 | (kind ? 0x1000u : 0u));
      v.insert(v.end(), w, w + 4);
      sb::PutBe32(w, 0);
      v.insert(v.end(), w, w + 4);
      uint32_t n = 100;
      for (const std::string& s : names) {
        v.insert(v.end(), s.begin(), s.end());
        sb::PutBe32(w, n++), v.insert(v.end(), w, w + 4);
        sb::PutBe32(w, n++), v.insert(v.end(), w, w + 4);
      }
    };
    std::vector<uint8_t> rows;
    add_dir(rows, "EMD ", 1, {"00039801", "00039802", "00099901", "ABCDEFGH", "00039899"});
    add_dir(rows, "MDL ", 0, {"0398", "0123"});
    uint8_t w4[4];
    sb::PutBe32(w4, 0xFFFF0003u), rows.insert(rows.end(), w4, w4 + 4);
    uint8_t term[4];
    sb::PutBe32(term, 0xFFFF0007u);
    std::vector<uint8_t> nr;
    size_t renamed = 0, body = 0;
    Check(sb::CopyDirsRetarget(rows.data(), rows.size(), 398, 397, term, nr, renamed, why) && renamed == 3, "moved: CopyDirsRetarget renames three 8-character names", N(int64_t(renamed)));
    Check(nr.size() == rows.size() && nr.size() > 100 && std::memcmp(&nr[12], "00039701", 8) == 0 && std::memcmp(&nr[12 + 48], "ABCDEFGH", 8) == 0,
          "moved: model names renamed, strangers and other ids untouched");
    Check(nr.size() > 4 && sb::Be32(&nr[nr.size() - 4]) == 0xFFFF0007u && sb::ScanDirs(nr.data(), nr.size(), body, why) && body == nr.size() - 4, "moved: the copy ends at TO's terminator word");
    const uint32_t game_len = uint32_t(rows.size() - 4);
    Check(sb::NodeSpan(game_len) == rows.size() && sb::ScanDirs(rows.data(), sb::NodeSpan(game_len), body, why), "moved: NodeSpan = +52 + the terminator word");
    Check(!sb::ScanDirs(rows.data(), game_len, body, why), "moved: read as +52 bytes alone the rows have no terminator");
    char n8[9] = "10039801", n4[5] = "0398";
    Check(!sb::RetargetName(n8, 8, 398, 397) && sb::RetargetName(n4, 4, 398, 397) && std::strcmp(n4, "0397") == 0, "moved: RetargetName: 1003980x is no model name; 0398 -> 0397");
    char p[48] = {};
    std::strcpy(p, "DLC6:\\PAC\\CH\\CH397_DLC06.PAC");
    Check(sb::PathIsCharPac(p, 397) && !sb::PathIsCharPac(p, 39) && !sb::PathIsCharPac(p, 398), "moved: PathIsCharPac matches CH397_DLC06.PAC only for 397");
  }

  // ============================== RetargetCard: the bar
  const int t = 397, s = 450, bank_t = 210, bank_s = 233, pack = 6, rank_t = 11, rank_s = 24;
  for (int moved = 0; moved < 2; ++moved) {
    const Base B = MakeBase(moved != 0);
    const std::string tag = moved ? " (entrance wrestler != base)" : " (entrance wrestler = base)";
    sc::SeatRows at_t, at_s, got, back;
    std::string err;
    const bool made = MadeFor(B, t, bank_t, rank_t, pack, at_t, err) && MadeFor(B, s, bank_s, rank_s, pack, at_s, err);
    Check(made, ("made-up cards instantiated for t and s through the importer's functions" + tag).c_str(), err);
    if (!made) continue;
    int words = 0;
    const bool ok = sb::RetargetCard(at_t, t, bank_t, s, bank_s, pack, rank_s, got, err, &words);
    Check(ok, ("RetargetCard accepts the card" + tag).c_str(), err);
    const std::string d = Diff(got, at_s);
    Check(d.empty(), ("THE BAR: t -> s equals the rows the importer writes for s, every block" + tag).c_str(), d);
    Check(words > 40, ("RetargetCard counts the words it changed" + tag).c_str(), N(words));
    // the rank is WRITTEN from the caller's number, never carried from the card (two cards made for the same old seat carry the same rank)
    sc::SeatRows other, other2;
    sb::RetargetCard(at_t, t, bank_t, s, bank_s, pack, 777, other, err);
    sb::RetargetCard(at_t, t, bank_t, 451, bank_s, pack, 778, other2, err);
    Check(sb::Le16(&other.dat[4 + 2]) == 777 && sb::Le16(&other2.dat[4 + 2]) == 778 && sb::Le16(&other.dat[4 + 2]) != rank_t,
          ("DAT +2 is the rank given, not the card's (two guests from one instance do not collide)" + tag).c_str(), N(sb::Le16(&other.dat[6])));
    sc::SeatRows ex = at_s;
    io::W16(&ex.dat[6], 777);
    Check(Diff(other, ex).empty(), ("t -> s with another rank: the bar but for the rank" + tag).c_str(), Diff(other, ex));
    // and back
    sb::RetargetCard(got, s, bank_s, t, bank_t, pack, rank_t, back, err);
    Check(Diff(back, at_t).empty(), ("t -> s -> t gives the original bytes back" + tag).c_str(), Diff(back, at_t));
    sc::SeatRows same;
    sb::RetargetCard(at_t, t, bank_t, t, bank_t, pack, rank_t, same, err);
    Check(Diff(same, at_t).empty(), ("t == s is the identity on every block" + tag).c_str(), Diff(same, at_t));
  }
  // (S110 LIBSIZE-1 review) THE BAR from the reference identity every library file is now made for (kRefSeat / kRefBank = 497 / 9997, the bank outside the
  // game's 1..599 and its part banks 19997..28997): the importer's card names every relocation kind, and each one moves to s exactly as the importer writes s
  for (int moved = 0; moved < 2; ++moved) {
    const Base B = MakeBase(moved != 0);
    const std::string tag = moved ? " (reference identity; entrance wrestler != base)" : " (reference identity; entrance wrestler = base)";
    sc::SeatRows at_r, at_s, got;
    std::string err;
    const bool made = MadeFor(B, sc::kRefSeat, sc::kRefBank, rank_t, pack, at_r, err) && MadeFor(B, s, bank_s, rank_s, pack, at_s, err);
    Check(made, ("made-up cards instantiated for 497 / 9997 and for s through the importer's functions" + tag).c_str(), err);
    if (!made) continue;
    std::set<int> kinds;
    for (const sc::Reloc& e : at_r.relo) kinds.insert(int(e.kind));
    Check(int(kinds.size()) == sc::kRelKindLast - sc::kRelKindFirst + 1, ("the reference card names every relocation kind" + tag).c_str(), N(int64_t(kinds.size())));
    const bool ok = sb::RetargetCard(at_r, sc::kRefSeat, sc::kRefBank, s, bank_s, pack, rank_s, got, err);
    Check(ok, ("RetargetCard accepts the reference card" + tag).c_str(), err);
    const std::string d = Diff(got, at_s);
    Check(ok && d.empty(), ("THE BAR: 497 / 9997 -> s equals the rows the importer writes for s, every block" + tag).c_str(), d);
  }
  {  // each recorded word, by name, on one importer card; a word the list does not name stays whatever it holds
    const Base B = MakeBase(true);
    sc::SeatRows at_t, r;
    std::string err;
    MadeFor(B, t, bank_t, rank_t, pack, at_t, err);
    io::W16(&at_t.dat[4 + 70], uint16_t(t * 100 + 5)), io::W16(&at_t.dat[4 + 100], uint16_t(t * 100 + 6)), io::W16(&at_t.dat[4 + 114], uint16_t(t * 100 + 7));  // chance twins outside the key range
    io::W32(&at_t.dat[4 + 12], 79000 + t);                                 // the old rule moved a 79000 + t text id; the importer never writes one: not recorded, stays
    io::W16(&at_t.pro[622 + 34 * 2], uint16_t(sb::PartBank(2, bank_t)));  // a part word at his own bank in PRO: not written by the importer, not recorded, stays
    if (!sb::RetargetCard(at_t, t, bank_t, s, bank_s, pack, rank_s, r, err)) r = at_t, Check(false, "RetargetCard takes the importer's card", err);  // (the checks below then fail by value)
    const uint8_t* R = &r.dat[4];
    Check(io::L16(R + 0) == s && io::L16(R + 4) == s && io::L16(R + 200) == s, "field DAT +0 / +4 / +200 (the id) = s");
    Check(io::L16(R + 2) == rank_s, "field DAT +2 (rank) = the rank given for s");
    Check(io::L16(R + 6) == 60000 + s && io::L16(R + 8) == 62000 + s && io::L16(R + 10) == 64000 + s, "field DAT +6 / +8 / +10 (name text ids) = 6x000 + s");
    Check(io::L32(R + 12) == 79000u + t, "DAT +12: a word the importer never wrote (79000 + t) is not in the list: it stays");
    Check(R[17] == pack, "field DAT +17 (pack code) = the frame's");
    Check(io::L16(R + 116) == s * 100 + 0 && io::L16(R + 118) == s * 100 + 5 && io::L16(R + 122) == s * 100 + 99 && io::L16(R + 198) == s * 100 + 42,
          "field DAT keys +116..+198 (t x 100 + n) = s x 100 + n");
    Check(io::L16(R + 120) == 22203, "a key of another id stays");
    Check(io::L16(R + 124) == t * 100 + 1 && io::L16(R + 126) == t * 100 + 2, "COINCIDENCE: stock words in t's key range at unrecorded offsets (next to a recorded key) stay");
    Check(io::L16(&r.dsbd[8]) == t * 100 + 3, "COINCIDENCE: a stock DSBD word in t's key range stays");
    Check(io::L16(R + 70) == t * 100 + 5 && io::L16(R + 100) == t * 100 + 6 && io::L16(R + 114) == t * 100 + 7, "a chance word equal to t x 100 + n outside +116..+198 stays");
    sc::SeatRows other_pack;
    sb::RetargetCard(at_t, t, bank_t, s, bank_s, 9, rank_s, other_pack, err);
    Check(other_pack.dat[4 + 17] == 9, "field DAT +17 (pack code) = the frame's pack, whatever the card held");
    Check(io::L16(&r.dsbd[0]) == s * 100 + 0 && io::L16(&r.dsbd[2]) == s * 100 + 1 && io::L16(&r.dsbd[40]) == s * 100 + 99, "field DSBD attire keys = s x 100 + n");
    Check(io::L16(&r.dsbd[4]) == 22204, "DSBD: a word naming another id's key stays");
    Check(G16(r.pro.data(), 576) == s && G16(r.pro.data(), 588) == s && G16(r.pro.data(), 592) == s, "field PRO music words (file +576 / +592 = live +572 / +588) = s; +588 too");
    Check(G16(r.pro.data(), 590) == 0x1234, "PRO +590: a word that was not the seat stays");
    Check(G16(r.pro.data(), 580) == bank_s && G16(r.pro.data(), 596) == bank_s, "field PRO bank words (+580 / +596) = bank(s)");
    Check(G16(r.pro.data(), 582) == bank_s && G16(r.pro.data(), 600) == bank_s && G16(r.pro.data(), 584) == 171 && G16(r.pro.data(), 598) == 172,
          "field PRO champion words: his own bank moves, another wrestler's stays");
    Check(G16(r.pro.data(), 578) == 720 && G16(r.pro.data(), 586) == 730 && G16(r.pro.data(), 594) == 721 && G16(r.pro.data(), 602) == 731, "PRO movie / tron words (+578 / +586 / +594 / +602) are seat-free: stay");
    Check(G16(r.pro.data(), 622 + 34 * 2) == sb::PartBank(2, bank_t) && G16(r.pro.data(), 622) == sb::PartBank(0, 160), "PRO part words (+622 + 34 k): the importer writes none, the list names none: they stay");
    Check(io::L16(&r.bin5[0]) == s, "field BIN5 +0 (template id) = s");
    bool parts = true;
    for (int k = 0; k < 5; ++k) parts = parts && io::L16(&r.bin5[4 + 2 * size_t(k)]) == sb::PartBank(k, bank_s);
    Check(parts, "field BIN5 part banks +4.. = 10000 + 1000 k + bank(s)");
    bool binp = r.binp.size() == 5;
    for (const auto& b : r.binp) binp = binp && io::L32(&b.row[4]) == 60000u + s && io::L16(&b.row[8]) == sb::PartBank(b.k, bank_s) && io::L16(&b.row[16]) == 1100 + s;
    Check(binp, "field BINP text +4 / bank +8 / unlock +16 = s's");
    bool lsd = r.lsd.size() == 8;
    for (const auto& l : r.lsd) {
      const uint16_t v = io::L16(&l.row[0]);
      const bool val = l.list == 2 ? v == s : (l.list == 3 || l.list == 17) ? v == bank_s : v >= 10000;
      const bool txt = l.list == 2 ? io::L32(&l.row[4]) == 4242 : io::L32(&l.row[4]) == 60000u + s;
      lsd = lsd && val && txt && io::L16(&l.row[2]) == 1100 + s;
    }
    Check(lsd, "field LSD value (list 2: the seat; 3 / 17: the bank; part lists: the part bank), unlock 1100 + s, text 60000 + s (not list 2's)");
    Check(r.has_t102 && r.t102_src_bank == uint32_t(B.ebank) && r.names[0] == "Made Up" && r.seat == s, "T102 (his entrance wrestler's bank) and the names stay; rows.seat = s");
    Check(r.has_relo && r.relo.size() == at_t.relo.size(), "the relocation list goes on with the card (a re-targeted card can be re-targeted again)");
    sc::SeatRows bad;
    Check(!sb::RetargetCard(at_t, t, bank_t, 99, bank_s, pack, rank_s, bad, err) && !err.empty(), "refused: a seat outside 100..499", err);
    Check(!sb::RetargetCard(at_t, t + 1, bank_t, s, bank_s, pack, rank_s, bad, err) && err.find("made for seat") != std::string::npos, "refused: rows made for another seat than t", err);
    Check(!sb::RetargetCard(at_t, t, 0, s, bank_s, pack, rank_s, bad, err) && err.find("bank number") != std::string::npos && !sb::RetargetCard(at_t, t, bank_t, s, 70000, pack, rank_s, bad, err) &&
              err.find("bank number") != std::string::npos,
          "refused: a bank outside 1..65535 (by its own rule, before any word is read)", err);
    Check(!sb::RetargetCard(at_t, t, bank_t, s, bank_s, 300, rank_s, bad, err), "refused: a pack code outside 0..255");
    Check(!sb::RetargetCard(at_t, t, bank_t, s, bank_s, pack, -1, bad, err) && err.find("rank") != std::string::npos && !sb::RetargetCard(at_t, t, bank_t, s, bank_s, pack, 70000, bad, err) &&
              err.find("rank") != std::string::npos,
          "refused: a rank outside 0..65535 (no 'keep the card's'), by its own rule", err);
    sc::SeatRows alias = at_t;
    Check(sb::RetargetCard(alias, t, bank_t, s, bank_s, pack, rank_s, alias, err) && alias.seat == s, "out may alias in");
    // (S110 SEAT3-R) only the recorded words move: every byte that differs between the card and its re-target lies inside a word the list names
    {
      auto named = [&](const char* tag, int sub, size_t off) {
        for (const sc::Reloc& e : at_t.relo)
          if (e.tag == tag && e.sub == sub && off >= e.off && off < size_t(e.off) + sc::RelWidth(e.kind)) return true;
        return false;
      };
      int stray = 0, moved = 0;
      auto scan = [&](const char* tag, int sub, const uint8_t* a, const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; ++i)
          if (a[i] != b[i]) ++moved, stray += !named(tag, sub, i);
      };
      scan("DAT ", 0, at_t.dat.data(), r.dat.data(), at_t.dat.size()), scan("PRO ", 0, at_t.pro.data(), r.pro.data(), at_t.pro.size());
      scan("DSBD", 0, at_t.dsbd.data(), r.dsbd.data(), at_t.dsbd.size()), scan("BIN5", 0, at_t.bin5.data(), r.bin5.data(), at_t.bin5.size());
      for (size_t i = 0; i < at_t.binp.size(); ++i) scan("BINP", at_t.binp[i].k, at_t.binp[i].row.data(), r.binp[i].row.data(), 20);
      for (size_t i = 0; i < at_t.lsd.size(); ++i) scan("LSD ", at_t.lsd[i].list, at_t.lsd[i].row.data(), r.lsd[i].row.data(), 8);
      Check(stray == 0 && moved > 100, "every byte the re-target changed lies in a recorded word (and many did change)", N(stray) + " stray, " + N(moved) + " changed");
    }
  }
  {  // each KIND, on a hand-made list: the named words move by their rule; the same values at unnamed offsets stay
    sc::SeatRows x;
    x.seat = t, x.has_relo = true;
    x.binp.push_back({2, {}}), x.lsd.push_back({3, {}});
    auto rel = [&](const char* tag, int sub, size_t off, sc::RelKind k, int arg) {
      sc::Reloc e;
      e.tag = tag, e.sub = uint8_t(sub), e.off = uint16_t(off), e.kind = k, e.arg = uint8_t(arg);
      x.relo.push_back(e);
    };
    using K = sc::RelKind;
    io::W16(&x.dat[4], uint16_t(t)), rel("DAT ", 0, 4, K::Seat16, 0);
    io::W16(&x.dat[8], uint16_t(t));  // the same value, not named
    io::W16(&x.dat[6], 5), rel("DAT ", 0, 6, K::Rank16, 0);
    io::W16(&x.dat[12], uint16_t(62000 + t)), rel("DAT ", 0, 12, K::Text16, 1);
    io::W16(&x.dat[130], uint16_t(t * 100 + 7)), rel("DAT ", 0, 130, K::Key16, 0);
    io::W16(&x.dat[132], uint16_t(t * 100 + 8));  // a key-valued word, not named
    x.dat[21] = 3, rel("DAT ", 0, 21, K::Pack8, 0);
    io::W16(&x.pro[580], uint16_t(bank_t)), rel("PRO ", 0, 580, K::Bank16, 0);
    io::W16(&x.pro[582], uint16_t(bank_t));  // his bank at an unnamed offset
    io::W16(&x.pro[640], uint16_t(sb::PartBank(3, bank_t))), rel("PRO ", 0, 640, K::PartBank16, 3);
    io::W16(&x.dsbd[2], uint16_t(t * 100 + 1)), rel("DSBD", 0, 2, K::Key16, 0);
    io::W16(&x.dsbd[4], uint16_t(t * 100 + 2));
    io::W16(&x.bin5[0], uint16_t(t)), rel("BIN5", 0, 0, K::Seat16, 0);
    io::W32(&x.binp[0].row[4], 60000u + t), rel("BINP", 2, 4, K::Text32, 0);
    io::W16(&x.binp[0].row[8], uint16_t(sb::PartBank(2, bank_t))), rel("BINP", 2, 8, K::PartBank16, 2);
    io::W16(&x.binp[0].row[16], uint16_t(1100 + t)), rel("BINP", 2, 16, K::Unlock16, 0);
    io::W16(&x.lsd[0].row[0], uint16_t(bank_t)), rel("LSD ", 3, 0, K::Bank16, 0);
    io::W16(&x.lsd[0].row[2], uint16_t(1100 + t)), rel("LSD ", 3, 2, K::Unlock16, 0);
    io::W32(&x.lsd[0].row[4], 64000u + t), rel("LSD ", 3, 4, K::Text32, 2);
    std::string err;
    std::string chk;
    Check(sc::CheckRelo(x, chk), "the hand-made list is a valid RELO list", chk);
    sc::SeatRows y;
    const bool took = sb::RetargetCard(x, t, bank_t, s, bank_s, 9, 777, y, err);
    Check(took, "RetargetCard takes the hand-made list", err);
    if (!took) y = x;  // (the checks below then fail by value, never by reading a block that is not there)
    Check(io::L16(&y.dat[4]) == s && io::L16(&y.dat[8]) == t, "kind Seat16: the named word is s, the same value elsewhere stays");
    Check(io::L16(&y.dat[6]) == 777, "kind Rank16: the rank given");
    Check(io::L16(&y.dat[12]) == 62000 + s, "kind Text16 (arg 1): 62000 + s");
    Check(io::L16(&y.dat[130]) == s * 100 + 7 && io::L16(&y.dat[132]) == t * 100 + 8, "kind Key16: s x 100 + n, n kept; a key-valued word that is not named stays");
    Check(y.dat[21] == 9, "kind Pack8: the frame's pack code");
    Check(io::L16(&y.pro[580]) == bank_s && io::L16(&y.pro[582]) == bank_t, "kind Bank16: his bank becomes the seat's; the same value at an unnamed offset stays");
    Check(io::L16(&y.pro[640]) == sb::PartBank(3, bank_s), "kind PartBank16 (arg 3): 10000 + 3000 + bank(s)");
    Check(io::L16(&y.dsbd[2]) == s * 100 + 1 && io::L16(&y.dsbd[4]) == t * 100 + 2, "DSBD: a named key moves, an unnamed one stays");
    Check(io::L16(&y.bin5[0]) == s, "BIN5 block: Seat16");
    Check(io::L32(&y.binp[0].row[4]) == 60000u + s && io::L16(&y.binp[0].row[8]) == sb::PartBank(2, bank_s) && io::L16(&y.binp[0].row[16]) == 1100 + s, "BINP leaf 2: Text32, PartBank16 (arg 2), Unlock16");
    Check(io::L16(&y.lsd[0].row[0]) == bank_s && io::L16(&y.lsd[0].row[2]) == 1100 + s && io::L32(&y.lsd[0].row[4]) == 64000u + s, "LSD list 3: Bank16, Unlock16, Text32 (arg 2: 64000 + s)");
    // refusals
    sc::SeatRows no_list = x;
    no_list.has_relo = false, no_list.relo.clear();
    Check(!sb::RetargetCard(no_list, t, bank_t, s, bank_s, 9, 5, y, err) && err.find("older importer") != std::string::npos && err.find("rebuild") != std::string::npos,
          "refused: rows with no relocation list (made before S110 SEAT3-R): 'made by an older importer: rebuild'", err);
    sc::SeatRows wrong = x;
    io::W16(&wrong.dat[4], uint16_t(t + 1));
    Check(!sb::RetargetCard(wrong, t, bank_t, s, bank_s, 9, 5, y, err) && err.find("disagree") != std::string::npos, "refused: a named word does not hold what the importer wrote for t", err);
    sc::SeatRows far_key = x;
    io::W16(&far_key.dat[130], uint16_t((t + 1) * 100));
    Check(!sb::RetargetCard(far_key, t, bank_t, s, bank_s, 9, 5, y, err), "refused: a named key outside t's key range", err);
    sc::SeatRows odd_kind = x;
    odd_kind.relo[0].kind = sc::RelKind(0);
    Check(!sb::RetargetCard(odd_kind, t, bank_t, s, bank_s, 9, 5, y, err) && err.find("unknown kind") != std::string::npos, "refused: an entry of a kind the core does not know", err);
    sc::SeatRows no_block = x;
    no_block.lsd.clear();
    Check(!sb::RetargetCard(no_block, t, bank_t, s, bank_s, 9, 5, y, err) && err.find("outside the rows") != std::string::npos, "refused: an entry that names a block the rows do not hold", err);
    sc::SeatRows past = x;
    past.relo[0].off = 207;  // a 16-bit word at the last byte of DAT
    Check(!sb::RetargetCard(past, t, bank_t, s, bank_s, 9, 5, y, err) && err.find("outside the rows") != std::string::npos, "refused: an entry that runs past its block", err);
    Check(!sb::RetargetCard(x, t, bank_t, s, 60000, 9, 5, y, err) && err.find("16 bits") != std::string::npos, "refused: a part bank for s that does not fit 16 bits (10000 + 3000 + 60000)", err);
  }

  // ============================== RetargetDirRows (S110 LIBSIZE: it keeps only the rows we own)
  for (int sd = 0; sd < 2; ++sd) {
    std::vector<uint8_t> rt, rs_, got, back;
    std::string why;
    const bool ok = NodeOf(t, bank_t, true, sd != 0, rt, why) && NodeOf(s, bank_s, true, sd != 0, rs_, why);
    Check(ok, sd ? "made-up node rows (SD) read by seats_core NodeRows" : "made-up node rows (HD) read by seats_core NodeRows", why);
    if (!ok) continue;
    const std::vector<uint8_t> rs_own = KeepTags(rs_, kOwnTags);  // (the made-up pacs lay their sectors out alike: the same sector / size words)
    size_t renamed = 0;
    Check(sb::RetargetDirRows(rt, t, s, bank_t, bank_s, false, got, why, &renamed), "RetargetDirRows t -> s", why);
    Check(got == rs_own, sd ? "node rows (SD) t -> s = the rows we own of the importer's rows for s (EMD + renders), EVP dropped" : "node rows (HD) t -> s = the rows we own of the importer's rows for s (EMD + renders), EVP dropped",
          "sizes " + N(int64_t(got.size())) + " vs " + N(int64_t(rs_own.size())));
    Check(renamed == 7, "renamed: EMD x4 and the three renders", N(int64_t(renamed)));
    bool foreign = false;
    for (const std::string& tag : TagsOf(got)) foreign = foreign || !kOwnTags.count(tag);
    Check(!TagsOf(got).empty() && !foreign, "no EVP / EVZ / GMEV / PCE6 / MOVN directory is registered: the game's entries come by alias rows");
    std::vector<uint8_t> back_ex = KeepTags(rt, kOwnTags);
    Check(sb::RetargetDirRows(got, s, t, bank_s, bank_t, false, back, why) && back == back_ex, "node rows t -> s -> t = the original's own rows");
    std::vector<uint8_t> same;
    Check(sb::RetargetDirRows(rt, t, t, bank_t, bank_t, true, same, why) && same == KeepTags(rt, {"EMD ", "EVP ", "SSFA", "SSFB", "SSFC", "SDFA", "SDFB", "SDFC"}),
          "node rows t == s with the EVP kept: his own rows, EVP included");
    Check(!sb::RetargetDirRows(rt, t, s, bank_t, bank_s, true, got, why) && !why.empty(), "refused: EVP rows kept for a moved seat", why);
    Check(got.empty(), "a refused RetargetDirRows leaves nothing");
    size_t nsp = 0;
    Check(sb::RetargetDirRows(rt, t, s, bank_t, bank_s, false, got, why) && got.size() > 4 && sb::ScanDirs(got.data(), got.size(), nsp, why) && nsp == got.size() - 4 &&
              sb::Be32(&got[got.size() - 4]) == sb::Be32(&rt[rt.size() - 4]),
          "the result scans clean, ends at the original terminator word (NodeSpan = +52 + 4)");
  }
  {
    std::vector<uint8_t> rt, got;
    std::string why;
    NodeOf(t, bank_t, true, false, rt, why);
    std::vector<uint8_t> trunc(rt.begin(), rt.end() - 4);
    Check(!sb::RetargetDirRows(trunc, t, s, bank_t, bank_s, false, got, why), "refused: rows with no terminator", why);
    Check(!sb::RetargetDirRows(rt, 12, s, bank_t, bank_s, false, got, why), "refused: a seat outside 100..499");
    // a hex-named directory is not one of the importer's: not registered (it used to be copied as it was)
    std::vector<uint8_t> hex;
    AddDir(hex, "EMD ", 1, {"000" + sb::Digits3(t) + "01"}, 0x10000u);
    EndRows(hex);
    Check(sb::RetargetDirRows(hex, t, s, bank_t, bank_s, false, got, why) && got.size() == 4, "a hex-name directory (flag 0x10000) is dropped", why);
    // (S110 LIBSIZE claim 9) a second id's EMD row and a tag the importer did not make are NOT kept: they would shadow a real wrestler's entries
    std::vector<uint8_t> mix;
    AddDir(mix, "EMD ", 1, {"000" + sb::Digits3(t) + "01", "000" + sb::Digits3(123) + "01", "000" + sb::Digits3(t) + "02"});
    AddDir(mix, "SSFA", 0, {"0" + sb::Digits3(122), "0" + sb::Digits3(t)});
    AddDir(mix, "ZZZZ", 0, {"0001"});
    AddDir(mix, "EMD2", 1, {"000" + sb::Digits3(t) + "01"});
    AddDir(mix, "SSFB", 0, {"0" + sb::Digits3(122)});  // a directory left with no row of his goes whole
    EndRows(mix);
    std::vector<uint8_t> ex;
    // the kept rows keep THEIR sector / size words (the made-up ones count from the row's place in the source directory: his EMD rows were rows 0 and 2, the SSFA one row 1)
    AddDirR(ex, "EMD ", 1, {{"000" + sb::Digits3(s) + "01", 1, 2}, {"000" + sb::Digits3(s) + "02", 3, 4}});
    AddDirR(ex, "SSFA", 0, {{"0" + sb::Digits3(s), 2, 3}});
    EndRows(ex);
    const bool ok = sb::RetargetDirRows(mix, t, s, bank_t, bank_s, false, got, why);
    Check(ok && TagsOf(got) == std::vector<std::string>({"EMD ", "SSFA"}) && got == ex, "a second id's EMD row, a render row of another id and a foreign tag are not kept; the lengths follow", why);
  }
  {  // the row names by directory (the alias rows use the same rules, src\seat_alias_core.h): EVZ / EVP numbers, GMEV / PCE6 %04d, MOVN M%03d, EMD / renders
    auto nm = [&](const char* tag, int kind, std::string n, int to_seat, int to_bank) {
      n.resize(kind ? 8 : 4, ' ');
      sb::RetargetRowName(tag, unsigned(kind), &n[0], t, to_seat, bank_t, to_bank);
      return n;
    };
    Check(nm("EVZ ", 1, "210", s, bank_s) == "233     " && nm("EVP ", 1, "210", s, bank_s) == "233     ", "row name: an EVZ / EVP bank number moves bank_t -> bank_s");
    auto pad8 = [](std::string x) { return x.resize(8, ' '), x; };
    Check(nm("EVZ ", 1, std::to_string(sb::PartBank(3, bank_t)), s, bank_s) == pad8(std::to_string(sb::PartBank(3, bank_s))),"row name: a part bank 10000 + 1000 k + bank moves with it");
    Check(nm("EVZ ", 1, std::to_string(6000 + t), s, bank_s) == std::to_string(6000 + s) + "    ", "row name: the victory scene 6000 + t moves to 6000 + s");
    Check(nm("EVZ ", 1, "211", s, bank_s) == "211     " && nm("EVZ ", 1, "12ab", s, bank_s) == "12ab    ", "row name: another bank, and a name that is not a number, stay");
    Check(nm("EVZ ", 0, "0210", s, bank_s) == "0210", "row name: a 4-byte EVZ row is not a bank name");
    Check(nm("GMEV", 0, "0210", s, bank_s) == "0233" && nm("PCE6", 0, "0210", s, bank_s) == "0233", "row name: a GMEV / PCE6 %04d name moves bank_t -> bank_s");
    Check(nm("GMEV", 0, "0211", s, bank_s) == "0211", "row name: another effects bank stays");
    Check(nm("MOVN", 0, "M" + std::to_string(500 + t), s, bank_s) == "M" + std::to_string(500 + s) && nm("MOVN", 0, "M" + std::to_string(500 + 123), s, bank_s) == "M" + std::to_string(500 + 123),
          "row name: the plate M<500 + t> moves to M<500 + s>, another plate stays");
    Check(nm("EMD ", 1, "000" + sb::Digits3(t) + "07", s, bank_s) == "000" + sb::Digits3(s) + "07" && nm("SSFA", 0, "0" + sb::Digits3(t), s, bank_s) == "0" + sb::Digits3(s),
          "row name: a model name and a render name move t -> s");
    Check(nm("ZZZZ", 1, "210", s, bank_s) == "210     ", "row name: a directory the rules do not know is left alone");
  }


  // ============================== RetargetEvBlob
  // ============================== RetargetEvBlob
  {
    const int e = 150;
    const Bytes ev = MakeEv(e, bank_t, 40);
    std::string err;
    int actors = 0;
    const int header_banks[] = {bank_t, int(sb::PartBank(1, bank_t)), 6000 + t};
    const int header_to[] = {bank_s, int(sb::PartBank(1, bank_s)), 6000 + s};
    for (int i = 0; i < 3; ++i) {
      Bytes evi = MakeEv(e, header_banks[i], 40);
      const Bytes blob_t = RefEv(evi, e, header_banks[i], t), blob_s = RefEv(evi, e, header_to[i], s);
      const Bytes got = sb::RetargetEvBlob(blob_t, t, bank_t, s, bank_s, err, &actors);
      Check(got == blob_s, i == 0 ? "EV blob (entrance bank): t -> s = io::BpeIdentity(RetargetEv for s)" : i == 1 ? "EV blob (part bank 11000 + bank): t -> s = the importer's for s"
                                                                                                                      : "EV blob (victory 6000 + t): t -> s = the importer's for s",
            err);
      Check(actors == 5 - 1 + 0 || actors == 4, "EV blob: the kind-1 actor keys of t (4) moved, another character's and a kind-2 record stay", N(actors));
      // every other byte unchanged
      size_t diffs = 0;
      for (size_t k = 0; k < blob_t.size() && k < got.size(); ++k) diffs += blob_t[k] != got[k];
      Check(got.size() == blob_t.size() && diffs >= 1 && diffs <= 2 + 4 * 3, "EV blob: only the bank word and the actor key bytes differ", N(int64_t(diffs)));
    }
    // positions: header bank at 21 + 0x20 = +0x35, actor 0 X at 21 + 0x44 + 12
    const Bytes blob_t = RefEv(ev, e, bank_t, t);
    const Bytes got = sb::RetargetEvBlob(blob_t, t, bank_t, s, bank_s, err);
    Check(got.size() > 0x35 + 2 && ((got[0x35] << 8) | got[0x36]) == bank_s, "EV blob: the header bank is at file +0x35");
    Check(got.size() > 21 + 0x44 + 15 && io::B32(&got[21 + 0x44 + 12]) == uint32_t(s) * 100u, "EV blob: actor 0's X is at 21 + 0x44 + 12");
    Check(sb::RetargetEvBlob(blob_t, t, bank_t, t, bank_t, err) == blob_t, "EV blob: t == s is the identity");
    const Bytes back = sb::RetargetEvBlob(got, s, bank_s, t, bank_t, err);
    Check(back == blob_t, "EV blob: t -> s -> t gives the blob back");
    // (S110 SEAT3 review) a real entrance bank decodes to up to ~100 KB (the importer's card of Lashley: 98764 bytes), which io::BpeIdentity cuts into
    // 0xFFFF-byte chunks; and a blob read through a pac row carries the row's 256-byte rounding. Both are the importer's own blobs: re-targeted.
    {
      const int nrec = 2200;  // 0x44 + 32 x 2200 = 70468 raw bytes: two chunks, actors in both
      Bytes big(0x44 + 32 * size_t(nrec), 0);
      Fill(big.data(), big.size(), 91);
      std::memcpy(big.data(), "0FOP", 4);
      io::WB32(&big[0x40], uint32_t(nrec));
      for (int i = 0; i < nrec; ++i) {
        uint8_t* r = &big[0x44 + 32 * size_t(i)];
        io::WB32(r + 12, uint32_t(e) * 100 + uint32_t(i % 100)), r[16] = 1;
      }
      const Bytes mt = RefEv(big, e, bank_t, t), ms = RefEv(big, e, bank_s, s);
      Check(mt.size() > 21 + 0xFFFF + 5 && mt[21 + 0xFFFF] == 0xFF && mt[21 + 0xFFFF + 2] == 0xFE, "(setup) io::BpeIdentity makes a two-chunk blob of 70468 raw bytes");
      int moved = 0;
      Check(sb::RetargetEvBlob(mt, t, bank_t, s, bank_s, err, &moved) == ms && moved == nrec, "EV blob of two identity chunks: t -> s = the importer's for s, the actors of both chunks moved",
            err + " actors " + N(moved));
      Bytes pt = mt, ps = ms;
      pt.resize((pt.size() + 255) / 256 * 256, 0), ps.resize(pt.size(), 0);
      Check(pt.size() > mt.size() && sb::RetargetEvBlob(pt, t, bank_t, s, bank_s, err) == ps, "EV blob read through a pac row (zeros to the 256-byte unit): re-targeted, the padding kept", err);
      Bytes one_t = RefEv(ev, e, bank_t, t), one_s = RefEv(ev, e, bank_s, s);
      one_t.resize(one_t.size() + 255, 0), one_s.resize(one_t.size(), 0);
      Check(sb::RetargetEvBlob(one_t, t, bank_t, s, bank_s, err) == one_s, "EV blob of one chunk + 255 zero bytes: re-targeted, the padding kept", err);
      Bytes pad256 = RefEv(ev, e, bank_t, t);
      pad256.resize(pad256.size() + 256, 0);
      Check(sb::RetargetEvBlob(pad256, t, bank_t, s, bank_s, err).empty(), "EV blob: 256 trailing bytes (more than a row's unit pads) are refused", err);
      Bytes padnz = RefEv(ev, e, bank_t, t);
      padnz.push_back(0), padnz.push_back(7);
      Check(sb::RetargetEvBlob(padnz, t, bank_t, s, bank_s, err).empty(), "EV blob: a non-zero byte after the last chunk is refused", err);
      Bytes c2 = mt;
      c2[21 + 0xFFFF + 1] = 0x10;
      Check(sb::RetargetEvBlob(c2, t, bank_t, s, bank_s, err).empty() && err.find("compressed") != std::string::npos, "EV blob: a second chunk that is not the identity table is refused", err);
      Bytes under = mt;
      io::W32(&under[12], 0xFFFF + 10);
      Check(sb::RetargetEvBlob(under, t, bank_t, s, bank_s, err).empty(), "EV blob: an expanded-size word smaller than the chunks hold is refused", err);
    }
    // refusals
    Bytes comp = blob_t;
    comp[16] = 0x10;
    Check(sb::RetargetEvBlob(comp, t, bank_t, s, bank_s, err).empty() && err.find("compressed") != std::string::npos, "EV blob: a compressed blob (chunk table is not FF 80 FE) is refused", err);
    Bytes zl = blob_t;
    std::memcpy(zl.data(), "ZLIB", 4);
    Check(sb::RetargetEvBlob(zl, t, bank_t, s, bank_s, err).empty() && !err.empty(), "EV blob: a ZLIB blob is refused", err);
    Check(sb::RetargetEvBlob(Bytes(blob_t.begin(), blob_t.begin() + 20), t, bank_t, s, bank_s, err).empty(), "EV blob: a blob cut inside the header is refused (no crash)");
    Bytes cut(blob_t.begin(), blob_t.end() - 3);
    Check(sb::RetargetEvBlob(cut, t, bank_t, s, bank_s, err).empty(), "EV blob: a blob with bytes missing is refused");
    Bytes trail = blob_t;
    trail.push_back(0);
    trail[8] = uint8_t(trail[8] + 1);  // (the packed-size word agrees with the longer blob: only the chunk size says it is wrong)
    Check(sb::RetargetEvBlob(trail, t, bank_t, s, bank_s, err).empty(), "EV blob: trailing bytes (a second chunk's start) are refused");
    Bytes sizes = blob_t;
    sizes[8] ^= 1;
    Check(sb::RetargetEvBlob(sizes, t, bank_t, s, bank_s, err).empty(), "EV blob: a packed-size word that disagrees is refused");
    Bytes nofop = ev;
    std::memcpy(nofop.data(), "XXXX", 4);
    Check(sb::RetargetEvBlob(io::BpeIdentity(nofop), t, bank_t, s, bank_s, err).empty() && err.find("0FOP") != std::string::npos, "EV blob: not an EV table is refused", err);
    Check(sb::RetargetEvBlob(Bytes(), t, bank_t, s, bank_s, err).empty(), "EV blob: an empty blob is refused (no crash)");
    // an actor count past the table is bounded
    Bytes lie = MakeEv(e, bank_t, 56);
    io::WB32(&lie[0x40], 0xFFFFFF);
    uint8_t* part = &lie[0x44 + 32 * 7];  // a record that starts inside the table but does not fit: never read as one
    io::WB32(part + 12, uint32_t(t) * 100 + 1), part[16] = 1;
    const Bytes lie_t = RefEv(lie, e, bank_t, t), lie_s = RefEv(lie, e, bank_s, s);
    Check(sb::RetargetEvBlob(lie_t, t, bank_t, s, bank_s, err) == lie_s, "EV blob: an actor count that runs past the table stops at the last whole record", err);
  }

  // ============================== the EV file
  {
    std::vector<sb::EvIn> in;
    for (int i = 0; i < 3; ++i) {
      sb::EvIn e;
      e.name = i == 0 ? "233" : i == 1 ? "11233" : "6450";
      e.blob.assign(size_t(300 + 1900 * i), uint8_t(0x40 + i));
      in.push_back(e);
    }
    sb::EvFile f;
    std::string err;
    Check(sb::BuildEvFile(in, f, err) && f.rows.size() == 3, "EV file: builds", err);
    if (f.rows.size() == 3) {
      Check(f.rows[0].sector == 0 && f.rows[0].size == 2, "EV file: row 0 at sector 0, 300 bytes = 2 units", N(f.rows[0].size));
      Check(f.rows[1].sector == 1 && f.rows[1].size == 9, "EV file: row 1 at sector 1 (the next 2 KB boundary), 2200 bytes = 9 units", N(f.rows[1].size) + " " + N(f.rows[1].sector));
      Check(f.rows[2].sector == 3, "EV file: row 2 after row 1's two sectors", N(f.rows[2].sector));
      Check(f.rows[0].name == "233     " && f.rows[1].name == "11233   ", "EV file: names padded to 8 with spaces");
      Check(f.bytes.size() == 0x4000u + 0x800u * 6 && f.bytes[0] == 0 && f.bytes[0x3FFF] == 0, "EV file: 0x4000 zero header, then whole sectors", N(int64_t(f.bytes.size())));
      bool at_ok = f.bytes.size() > 0x4000 + 0x800 * 4;
      if (at_ok) at_ok = f.bytes[0x4000] == 0x40 && f.bytes[0x4000 + 0x800] == 0x41 && f.bytes[0x4000 + 0x800 * 3] == 0x42;
      Check(at_ok, "EV file: each blob starts at 0x4000 + sector x 0x800");
      Check(sb::EvBlobAt(f, f.rows[1]).size() == 9 * 256 && sb::EvBlobAt(f, f.rows[1])[0] == 0x41, "EV file: EvBlobAt cuts the row's size (256-byte units)");
      Check(sb::EvBlobAt(f, f.rows[0]).size() == 512, "EV file: EvBlobAt row 0 = 2 units");
      uint8_t term[4] = {0xFF, 0xFF, 0x00, 0x05};
      const std::vector<uint8_t> nr = sb::EvNodeRows(f, term);
      size_t body = 0;
      std::string why;
      Check(nr.size() == 12 + 3 * 16 + 4 && std::memcmp(nr.data(), "EVP ", 4) == 0 && sb::Be32(&nr[4]) == (12u << 20 | 0x1000u) && sb::ScanDirs(nr.data(), nr.size(), body, why) && body == 12 + 48,
            "EV node rows: one EVP directory of 3 sixteen-byte rows, terminator, scans clean", why);
      Check(nr.size() >= 12 + 48 && std::memcmp(&nr[12 + 16], "11233   ", 8) == 0 && sb::Be32(&nr[12 + 16 + 8]) == 1 && sb::Be32(&nr[12 + 16 + 12]) == 9, "EV node rows: row 1 = name, sector 1, size 9 (big-endian)");
    }
    sb::EvFile g;
    std::vector<sb::EvIn> bad = in;
    bad[1].name = in[0].name;
    Check(!sb::BuildEvFile(bad, g, err), "EV file: refused: a name twice", err);
    bad = in, bad[0].name = "123456789";
    Check(!sb::BuildEvFile(bad, g, err), "EV file: refused: a name longer than 8");
    bad = in, bad[2].blob.clear();
    Check(!sb::BuildEvFile(bad, g, err), "EV file: refused: an empty blob");
    Check(sb::BuildEvFile({}, g, err) && g.rows.empty() && g.bytes.size() == 0x4000, "EV file: no blobs: just the header");
  }

  // ============================== the page
  {
    const std::vector<int> frame = {397, 398, 400, 401, 402};
    auto cat = [&](std::vector<sb::Entry> v, const std::set<int>& held, int cap) { return sb::Catalog::Make(v, frame, held, cap); };
    // sorted order, case, duplicates
    {
      auto c = cat(Lib({{"00000003", "charlie"}, {"00000009", "ALPHA"}, {"00000002", "bravo"}, {"00000001", "Alpha"}, {"00000001", "Alpha again (same key)"}}), {}, 0);
      Check(Names(c.library) == "00000001 00000009 00000002 00000003 ", "page: the library is sorted by name, case-insensitive, ties by key; a key listed twice counts once", Names(c.library));
    }
    {
      auto cu = sb::Catalog::Make(Lib({{"0000000A", "Abel"}}), {402, 397, 400, 397, 398}, {}, 0);
      Check(cu.Bindable() == std::vector<int>({397, 398, 400, 402}), "page: the frame's seats in any order and twice are bound in ascending seat order, once");
    }
    // 3 cards, cap 2, no resident
    {
      auto c = cat(Lib({{"0000000A", "Cora"}, {"0000000B", "Abel"}, {"0000000C", "Bea"}}), {}, 2);
      sb::State st;
      Check(c.Bindable() == std::vector<int>({397, 398}), "page: bindable seats = the first 2 free frame seats (cap 2)");
      Check(c.PageCount() == 2, "page: 3 cards, 2 seats: 2 pages", N(c.PageCount()));
      sb::Plan p0 = c.Assign(0, st);
      Check(p0.steps.size() == 2 && p0.steps[0].seat == 397 && p0.steps[0].arrive == "0000000B" && p0.steps[1].seat == 398 && p0.steps[1].arrive == "0000000C", "page 0 = {Abel, Bea} at {397, 398} in ascending seat order", Steps(p0));
      Apply(st, p0);
      sb::Plan p1 = c.Assign(1, st);
      Check(p1.steps.size() == 2 && p1.steps[0].seat == 397 && p1.steps[0].leave == "0000000B" && p1.steps[0].arrive == "0000000A" && p1.steps[1].seat == 398 && p1.steps[1].leave == "0000000C" && p1.steps[1].arrive.empty(),
            "the last page is shorter: Cora at 397, seat 398 unbound", Steps(p1));
      Apply(st, p1);
      Check(Dump(st) == "397:0000000A ", "page 1 applied: only Cora is seated", Dump(st));
      Check(c.Flip(1, +1) == 0 && c.Flip(0, -1) == 1 && c.Flip(0, +1) == 1 && c.Flip(1, -1) == 0, "RT / LT wrap both ways");
      const sb::State at1 = st;
      Apply(st, c.Assign(c.Flip(1, +1), st));
      Apply(st, c.Assign(c.Flip(0, -1), st));
      Check(Dump(st) == Dump(at1), "flipping forward then back gives the same page again", Dump(st));
      Apply(st, c.Assign(0, st));
      Check(Dump(st) == "397:0000000B 398:0000000C ", "page 0 again: the first page", Dump(st));
      sb::Plan again = c.Assign(0, st);
      Check(again.steps.empty(), "a seat whose page entry is the man already there is not touched", Steps(again));
      Check(c.Assign(4, st).page == 0 && c.Assign(-1, st).page == 1, "Assign takes the page modulo the page count");
    }
    // residents and held seats are never bound
    {
      sb::Entry r1;
      r1.key = "000000R1", r1.name = "Resident One", r1.seat = 397;
      std::vector<sb::Entry> v = {r1};
      for (const auto& e : Lib({{"0000000A", "Anna"}, {"0000000B", "Bob"}, {"0000000C", "Cy"}, {"0000000D", "Di"}})) v.push_back(e);
      auto c = sb::Catalog::Make(v, frame, {400}, 0);
      sb::State st;
      Check(c.Bindable() == std::vector<int>({398, 401, 402}), "page: bindable = frame minus residents (397) minus held (400)", N(int64_t(c.Bindable().size())));
      Check(c.library.size() == 4 && c.PageCount() == 2, "page: a resident is not in the library; 4 cards over 3 seats = 2 pages");
      bool never = true;
      for (int k = 0; k < 2; ++k)
        for (const sb::Step& s_ : c.Assign(k, st).steps) never = never && s_.seat != 397 && s_.seat != 400;
      Check(never, "a resident's seat and a held seat are never in a step");
      Check(c.Assign(0, st).steps.size() == 3 && c.Assign(1, st).steps.size() == 1, "page 0 fills 3 seats, page 1 one");
    }
    // a library smaller than the seats
    {
      auto c = cat(Lib({{"0000000A", "Solo"}}), {}, 0);
      sb::State st;
      sb::Plan p = c.Assign(0, st);
      Check(c.Bindable().size() == 5 && c.PageCount() == 1 && p.steps.size() == 1 && p.steps[0].seat == 397 && p.steps[0].arrive == "0000000A", "a library smaller than the seats: one page, one step (empty seats are not touched)", Steps(p));
    }
    // (S110 SEAT3-R d1) held by the screen: the page SIZE is fixed for the session; the held seat keeps its man and is not re-bound, the others bind as planned
    {
      auto c = cat(Lib({{"0000000A", "Abel"}, {"0000000B", "Bea"}, {"0000000C", "Cora"}}), {}, 2);
      sb::State st;
      st.guests[397] = "0000000A";
      st.guests[398] = "0000000B";
      st.screen_held.insert(397);  // another panel picked Abel
      Check(c.Bindable() == std::vector<int>({397, 398}), "held by the screen: the bindable seats do not change (397 stays in the page size)");
      Check(c.PageCount() == 2 && c.Assign(0, st).pages == 2 && c.Assign(1, st).pages == 2, "held by the screen: the page count is the same with and without the hold");
      sb::State free_st = st;
      free_st.screen_held.clear();
      Check(c.PageOf("0000000C", st).page == 1 && c.PageOf("0000000C", free_st).page == 1, "held by the screen: the page of a character does not move with the hold");
      sb::Plan p0 = c.Assign(0, st);
      Check(p0.steps.empty(), "held: page 0 = {Abel (held), Bea}: both are there already, nothing is re-bound", Steps(p0));
      sb::Plan p1 = c.Assign(1, st);
      Check(p1.steps.size() == 1 && p1.steps[0].seat == 398 && p1.steps[0].leave == "0000000B" && p1.steps[0].arrive == "0000000C",
            "held: page 1 = {Cora}: Cora's slot is the held seat 397, so she takes the next free seat 398; 397 keeps Abel", Steps(p1));
      bool never = true;
      for (int k = 0; k < 4; ++k)
        for (const sb::Step& s_ : c.Assign(k, st).steps) never = never && s_.seat != 397 && s_.leave != "0000000A" && s_.arrive != "0000000A";
      Check(never, "a character picked by another panel stays put across a flip: no step touches his seat or him");
      Apply(st, c.Assign(c.Flip(0, +1), st));
      Check(st.guests.count(397) && st.guests[397] == "0000000A" && st.guests[398] == "0000000C", "after the flip the held man is still at 397", Dump(st));
      // the SAME slices for the page whether or not a seat is held: the first RB with the cursor on a guest rebinds the next page (it used to rebind nothing)
      auto c2 = cat(Lib({{"0000000A", "Abel"}, {"0000000B", "Bea"}, {"0000000C", "Cora"}, {"0000000D", "Dan"}}), {}, 2);
      sb::State g;
      g.guests[397] = "0000000A", g.guests[398] = "0000000B";
      g.screen_held.insert(398);
      const sb::Plan q = c2.Assign(c2.Flip(0, +1), g);
      Check(q.pages == 2 && q.page == 1 && q.steps.size() == 1 && q.steps[0].seat == 397 && q.steps[0].arrive == "0000000C",
            "held: the first flip with the cursor on a guest still rebinds (page 1 = {Cora, Dan}: Cora takes 397, Dan's slot 398 is held)", Steps(q));
      // the same man never listed twice: after any flip he is seated once
      int seats_of_a = 0;
      for (const auto& [seat, k] : st.guests) seats_of_a += k == "0000000A";
      Check(seats_of_a == 1, "a character already seated is never listed twice", Dump(st));
    }
    // (S110 SEAT3-R d2) a man who is on the new page and already sits on a bindable seat KEEPS it: no export, no purge, no re-arrival
    {
      sb::Catalog c = sb::Catalog::Make(Lib({{"0000000A", "Ann"}, {"0000000B", "Bob"}, {"0000000C", "Cy"}, {"0000000D", "Di"}, {"0000000E", "Eve"}, {"0000000F", "Fay"}}),
                                        {397, 398, 400, 401}, {}, 0);
      sb::State st;
      st.guests[397] = "0000000A", st.guests[398] = "0000000E", st.guests[400] = "0000000C", st.guests[401] = "0000000D";  // Eve sits on 398 although page 0 seats the others
      const sb::Plan p = c.Assign(1, st);  // page 1 = {Eve, Fay}
      bool eve_moved = false;
      for (const sb::Step& s_ : p.steps) eve_moved = eve_moved || s_.seat == 398 || s_.leave == "0000000E" || s_.arrive == "0000000E";
      Check(!eve_moved, "keep: Eve is on page 1 and sits on 398: no step names her seat or her", Steps(p));
      Check(p.steps.size() == 3 && p.steps[0].seat == 397 && p.steps[0].leave == "0000000A" && p.steps[0].arrive == "0000000F" && p.steps[1].seat == 400 && p.steps[1].leave == "0000000C" &&
                p.steps[1].arrive.empty() && p.steps[2].seat == 401 && p.steps[2].leave == "0000000D" && p.steps[2].arrive.empty(),
            "keep: Fay takes the lowest free seat 397 (Eve's slot 398 is hers); 400 and 401 are emptied; only changing seats are steps", Steps(p));
      sb::State after = st;
      Apply(after, p);
      Check(Dump(after) == "397:0000000F 398:0000000E ", "keep: after the flip Eve is still on 398 and Fay on 397", Dump(after));
      Check(c.Assign(1, after).steps.empty(), "keep: the same page again changes nothing");
      Check(c.RowAfter("0000000E", 1, st) == 1 && c.RowAfter("0000000F", 1, st) == 0 && c.RowAfter("0000000A", 1, st) == -1,
            "keep: the listing after the flip is by seat: Fay (397) then Eve (398); Ann is gone", N(c.RowAfter("0000000E", 1, st)) + " " + N(c.RowAfter("0000000F", 1, st)));
      const std::map<int, int> rk = c.Ranks(10, 1, st);
      Check(rk.size() == 2 && rk.count(398) && rk.count(397) && rk.find(398)->second == 11 && rk.find(397)->second == 12, "keep: the ranks follow the names, not the seats: Eve 11 (seat 398), Fay 12 (seat 397)", N(int64_t(rk.size())));
      // a man on the page whose slot is free but who sits elsewhere keeps his seat too; the stale seat of a man who left the bindable range is vacated
      sb::State st3;
      st3.guests[401] = "0000000F";  // Fay sits on 401 (page 1's slot for her is 398)
      const sb::Plan p3 = c.Assign(1, st3);
      Check(p3.steps.size() == 1 && p3.steps[0].seat == 397 && p3.steps[0].arrive == "0000000E", "keep: Fay keeps 401; Eve (slot 397) arrives; nobody else moves", Steps(p3));
      // slot first: a man who needs a seat takes the seat his slot names when it is free, not the lowest free one
      sb::State st4;
      st4.guests[400] = "0000000E";  // Eve (page 1, index 0) sits on 400
      const sb::Plan p4 = c.Assign(1, st4);
      Check(p4.steps.size() == 1 && p4.steps[0].seat == 398 && p4.steps[0].arrive == "0000000F", "keep: Eve keeps 400; Fay takes her own slot 398 (free), not the lowest free seat 397", Steps(p4));
      // the men whose slots are taken fill the free seats in page order, ascending
      sb::State st5;
      st5.guests[397] = "0000000C", st5.guests[398] = "0000000D";
      const sb::Plan p5 = c.Assign(0, st5);  // page 0 = {Ann, Bob, Cy, Di}
      Check(p5.steps.size() == 2 && p5.steps[0].seat == 400 && p5.steps[0].arrive == "0000000A" && p5.steps[1].seat == 401 && p5.steps[1].arrive == "0000000B",
            "keep: Cy and Di keep 397 / 398; Ann and Bob, whose slots are taken, fill the free seats in page order, ascending (400, 401)", Steps(p5));
    }
    // a stale guest (the cap fell) is unbound
    {
      auto c = cat(Lib({{"0000000A", "Abel"}}), {}, 2);
      sb::State st;
      st.guests[402] = "0000000X";
      sb::Plan p = c.Assign(0, st);
      bool stale = false;
      for (const sb::Step& s_ : p.steps) stale = stale || (s_.seat == 402 && s_.leave == "0000000X" && s_.arrive.empty());
      Check(stale, "a guest on a seat that is no longer bindable is unbound", Steps(p));
      st.screen_held.insert(402);  // ... unless the screen holds that seat (a pick of another panel): then he stays
      bool kept402 = true;
      for (const sb::Step& s_ : c.Assign(0, st).steps) kept402 = kept402 && s_.seat != 402;
      Check(kept402, "a guest on a non-bindable seat the screen holds is not unbound");
    }
    // no seats, no cards
    {
      auto c0 = sb::Catalog::Make({}, {}, {}, 0);
      sb::State st;
      Check(c0.PageCount() == 0 && c0.Assign(0, st).steps.empty() && c0.Flip(0, 1) == 0, "no cards, no seats: no pages, no steps (no crash)");
      auto c1 = sb::Catalog::Make(Lib({{"0000000A", "Abel"}}), {397}, {397}, 0);
      Check(c1.PageCount() == 0 && c1.Assign(0, st).steps.empty(), "every seat held: no pages");
      Check(!c1.PageOf("0000000A", st).seated && c1.PageOf("0000000A", st).page == -1 && c1.PageOf("0000000A", st).found, "no bindable seat: found, not seated, no page");
    }
    // the listing and the row
    {
      sb::Entry r1;
      r1.key = "000000R1", r1.name = "Randy", r1.seat = 401;
      std::vector<sb::Entry> v = {r1};
      for (const auto& e : Lib({{"0000000A", "Abel"}, {"0000000B", "Bea"}})) v.push_back(e);
      auto c = sb::Catalog::Make(v, frame, {}, 0);
      sb::State st;
      const auto list = c.Listing(0, st);
      Check(list.size() == 3 && list[0].seat == 397 && list[0].key == "0000000A" && list[1].seat == 398 && list[2].seat == 401 && list[2].key == "000000R1", "listing: guests at the low seats, the resident at 401, ascending by seat");
      Check(c.RowAfter("0000000A", 0, st) == 0 && c.RowAfter("0000000B", 0, st) == 1 && c.RowAfter("000000R1", 0, st) == 2 && c.RowAfter("nope", 0, st) == -1, "RowAfter: the row position of each, -1 for a man not seated");
    }
    // PageOf
    {
      sb::Entry r1;
      r1.key = "000000R1", r1.name = "Randy", r1.seat = 401;
      std::vector<sb::Entry> v = {r1};
      for (const auto& e : Lib({{"0000000A", "Abel"}, {"0000000B", "Bea"}, {"0000000C", "Cora"}, {"0000000D", "Dan"}, {"0000000E", "Eve"}})) v.push_back(e);
      auto c = sb::Catalog::Make(v, frame, {}, 2);
      sb::State st;
      const sb::PageRef rr = c.PageOf("000000R1", st);
      Check(rr.found && rr.seated && rr.seat == 401, "PageOf: a resident is already seated (no flip)");
      const sb::PageRef pc = c.PageOf("0000000C", st), pe = c.PageOf("0000000E", st);
      Check(pc.found && !pc.seated && pc.page == 1 && pe.page == 2, "PageOf: Cora on page 1, Eve on page 2 (2 seats a page)", N(pc.page) + " " + N(pe.page));
      Check(!c.PageOf("zzz", st).found && !c.PageOf("", st).found, "PageOf: an unknown key is not found (no crash)");
      st.guests[398] = "0000000C";
      const sb::PageRef ps = c.PageOf("0000000C", st);
      Check(ps.seated && ps.seat == 398, "PageOf: a guest bound now is already seated");
      st.screen_held.insert(398);
      sb::Plan p = c.Assign(0, st);
      bool rebound = false;
      for (const sb::Step& s_ : p.steps) rebound = rebound || s_.seat == 398 || s_.arrive == "0000000C" || s_.leave == "0000000C";
      Check(c.PageOf("0000000C", st).seated && !rebound, "the target held by another panel: listed, already seated, nothing re-bound", Steps(p));
    }
  }

  // ============================== the letter index
  {
    auto names = [](const std::vector<std::pair<std::string, std::string>>& v, int resident_at = -1) {
      std::vector<sb::Entry> out;
      for (size_t i = 0; i < v.size(); ++i) {
        sb::Entry e;
        e.key = v[i].first, e.name = v[i].second;
        if (int(i) == resident_at) e.seat = 401;
        out.push_back(e);
      }
      return out;
    };
    const std::vector<int> frame = {397, 398, 400, 401};
    auto c = sb::Catalog::Make(names({{"00000002", "Ann"}, {"00000001", "ann"}, {"00000003", "Bob"}, {"00000004", "Zed"}, {"00000005", "9Lives"},
                                      {"00000006", "\xC3\x89" "dgar"}, {"00000007", "Randy"}},
                                     6),
                               frame, {}, 2);
    std::string order;
    for (const auto& e : c.all) order += e.key + " ";
    Check(order == "00000005 00000001 00000002 00000003 00000006 00000007 00000004 ", "letter: the list is # (9Lives), A (ann, Ann by key), B, E (Edgar with its accent), R (a resident), Z", order);
    Check(sb::BucketOf("9Lives") == '#' && sb::BucketOf("ann") == 'A' && sb::BucketOf("\xC3\x89" "dgar") == 'E' && sb::BucketOf("\xC3\x86" "var") == 'A' && sb::BucketOf("\xC3\x97x") == '#',
          "letter: a digit is '#'; a Latin-1 letter folds to its base (E-acute -> E, AE -> A); the multiplication sign is '#'");
    Check(sb::BucketOf("\xC5\x92uvre") == '#' && sb::BucketOf("\xCE\xB1" "lpha") == '#', "letter: a letter beyond Latin-1 is '#'");
    // (S110 SEAT3 review) ONE rule with the letter jump (src\letter_jump_core.h Initial): a letter jump must land on a page that holds its letter
    {
      std::string bad;
      auto same = [&](const std::string& n) {
        if (sb::BucketOf(n) != wwe2k14::letter_jump::Initial(n)) bad += "[" + n + "] ";
      };
      for (const char* n : {"Ann", " Alex", "  zed", "The Rock", "the  rock", "THE ", "The", "Theo", "", "   ", "9Lives", "\xC3\x89" "dgar", "\xC9" "dgar", "\xC3", "\xFF" "x",
                            "\xC3\x9F" "a", "\xE2\x80\x9C" "Q", "_x", "\x7F", "The \xC3\x89ric", "THE"})
        same(n);
      for (uint32_t c = 1; c < 0x800; ++c) {  // every first code point below U+0800 (1- and 2-byte UTF-8)
        std::string n = c < 0x80 ? std::string(1, char(c)) : std::string{char(0xC0 | (c >> 6)), char(0x80 | (c & 0x3F))};
        same(n + "x");
      }
      for (int b = 0x80; b < 0x100; ++b) same(std::string(1, char(b)) + "x");  // a raw byte that starts no UTF-8 sequence there
      Check(bad.empty(), "collation: BucketOf = letter_jump::Initial for every first character (leading spaces, THE + spaces, raw Latin-1 bytes, empty)", bad);
      auto cl = sb::Catalog::Make(Lib({{"1", "Sam"}, {"2", "The Rock"}, {"3", " Zed"}, {"4", "the   abe"}, {"5", "\xC9" "ric"}, {"6", "9Lives"}, {"7", "Tim"}, {"8", ""}, {"9", "Bob"}, {"10", "\xC3\x97" "x"}}), {}, {}, 0);
      std::string order;
      bool runs = true;
      uint8_t prev = 0;
      for (const auto& e : cl.all) {
        const uint8_t b = uint8_t(sb::BucketOf(e.name));
        runs = runs && b >= prev, prev = b, order += e.key;
      }
      Check(runs && order == "86104952173", "collation: the sorted list is in the letter jump's buckets (no letter, # incl. the multiplication sign, A, B, E, R, S, T, Z): each bucket one run", order);
    }
    Check(sb::BucketOf("zed") == 'Z' && sb::BucketOf("ZED") == 'Z', "letter: case-insensitive");
    Check(sb::BucketOf("The Rock") == 'R' && sb::BucketOf("the rock") == 'R' && sb::BucketOf("Theo") == 'T' && sb::BucketOf("The ") == 'T' && sb::BucketOf("The") == 'T',
          "collation: a leading 'THE ' is dropped (The Rock -> R); Theo, 'The ' and 'The' alone are not");
    // the first entry of each letter, and its page
    Check(LetterKey(sb::FirstOfLetter(c, 'A')) == "00000001" && LetterKey(sb::FirstOfLetter(c, 'e')) == "00000006" && LetterKey(sb::FirstOfLetter(c, '#')) == "00000005" &&
              LetterKey(sb::FirstOfLetter(c, 'R')) == "00000007",
          "letter: FirstOfLetter = the first entry of the bucket (any case; '#'; a resident's)");
    Check(sb::FirstOfLetter(c, 'C') == nullptr && sb::FirstOfLetter(c, '1') == nullptr && sb::FirstOfLetter(c, '\0') == nullptr, "letter: an empty bucket, or no bucket, has no first entry");
    sb::State none;
    Check(sb::PageOfLetter(c, 'R', none).seated && sb::PageOfLetter(c, 'R', none).seat == 401, "letter: a resident's letter needs no page (already seated)");
    Check(sb::PageOfLetter(c, 'C', none).found == false, "letter: an empty bucket: not found (the caller skips it)");
    auto empty = sb::Catalog::Make({}, frame, {}, 0);
    Check(sb::FirstOfLetter(empty, 'A') == nullptr && !sb::PageOfLetter(empty, 'A', none).found, "letter: an empty list: nothing (no crash)");
    auto one = sb::Catalog::Make(names({{"00000001", "Solo"}}), frame, {}, 0);
    Check(LetterKey(sb::FirstOfLetter(one, 'S')) == "00000001" && sb::PageOfLetter(one, 'S', none).page == 0, "letter: a library of one: his letter's first is himself, page 0");
    // a bucket larger than a page, a resident target, a held target
    auto big = sb::Catalog::Make(names({{"00000011", "Sam"}, {"00000012", "Sara"}, {"00000013", "Sean"}, {"00000014", "Seth"}, {"00000015", "Sue"}, {"00000016", "Ted"}, {"00000017", "Abe"}}, 6), frame, {}, 2);
    sb::State st;
    std::string ord;
    for (const auto& e : big.all) ord += e.name + " ";
    Check(ord == "Abe Sam Sara Sean Seth Sue Ted ", "letter: the big list in order", ord);
    const sb::Entry* sj = sb::FirstOfLetter(big, 'S');
    Check(LetterKey(sj) == "00000011", "letter: the first entry of the bucket larger than a page is Sam", LetterKey(sj));
    // (Abe is the resident, sorted first; Sam .. Ted = 6 library cards over 2 seats = 3 pages; the S bucket spans pages 0..2)
    const sb::PageRef pr = big.PageOf("00000011", st);
    Check(!pr.seated && pr.page >= 0, "letter: the first entry of the big bucket needs a page flip", N(pr.page));
    const sb::PageRef p5 = big.PageOf("00000015", st);
    Check(p5.page > pr.page, "letter: the rest of the bucket is reached by paging (a later page)", N(p5.page));
    sb::Plan plan = big.Assign(pr.page, st);
    Apply(st, plan);
    Check(big.RowAfter("00000011", pr.page, sb::State()) >= 0, "letter: after binding his page the target has a row in the list");
    const sb::PageRef res = big.PageOf("00000016", sb::State());
    Check(res.found && !res.seated && big.all[0].key == "00000017" && big.PageOf("00000017", st).seated, "letter: a resident target (Abe) answers already seated: no flip");
    sb::State held;
    held.guests[397] = "00000012";
    held.screen_held.insert(397);
    const sb::Entry* jump = sb::FirstOfLetter(big, 'S');
    bool touched = false;
    for (int k = 0; k < 3; ++k)
      for (const sb::Step& s_ : big.Assign(k, held).steps) touched = touched || s_.seat == 397 || s_.arrive == "00000012" || s_.leave == "00000012";
    Check(jump && big.PageOf("00000012", held).seated && !touched, "letter: a target held by another panel is listed and seated: nothing is re-bound");
  }

  // ============================== the ranks (LETTERJUMP.md claims 7-8: unique, above the game's, alphabetical across residents and guests)
  {
    const int base_rank = 95;
    const std::vector<int> frame = {397, 398, 400, 401, 402};
    std::vector<sb::Entry> v;
    auto add = [&](const char* key, const char* name, int seat) {
      sb::Entry e;
      e.key = key, e.name = name, e.seat = seat;
      v.push_back(e);
    };
    add("000000R1", "Mick", 401);
    add("000000R2", "The Alpha", 402);  // a resident whose name sorts under A
    add("0000000A", "Zack", -1);
    add("0000000B", "bob", -1);
    add("0000000C", "Carl", -1);
    add("0000000D", "Dave", -1);
    auto c = sb::Catalog::Make(v, frame, {}, 2);
    sb::State st;
    auto uniq = [](const std::map<int, int>& m) {
      std::set<int> s;
      for (const auto& [seat, r] : m) s.insert(r);
      return s.size() == m.size();
    };
    const std::map<int, int> r0 = c.Ranks(base_rank, 0, st);
    // page 0 (cap 2, bindable 397 / 398) = bob, Carl: listed = The Alpha (402), bob (397), Carl (398), Mick (401)
    Check(r0.size() == 4 && uniq(r0), "ranks: every listed import has one, all unique", N(int64_t(r0.size())));
    bool above = true;
    for (const auto& [seat, r] : r0) above = above && r > base_rank;
    auto rk = [](const std::map<int, int>& m, int seat) {
      const auto it = m.find(seat);
      return it == m.end() ? -1 : it->second;
    };
    Check(above && rk(r0, 402) == base_rank + 1, "ranks: all above base_rank; the first (The Alpha, 'THE ' dropped) is base_rank + 1", N(rk(r0, 402)));
    Check(rk(r0, 397) == base_rank + 2 && rk(r0, 398) == base_rank + 3 && rk(r0, 401) == base_rank + 4,
          "ranks: alphabetical across residents and guests together (Alpha < bob < Carl < Mick), seats do not matter");
    Apply(st, c.Assign(0, st));
    const std::map<int, int> same = c.Ranks(base_rank, 0, st);
    Check(same == r0, "ranks: stable when a flip changes nothing (page 0 again with page 0 bound)");
    // page 1 = Dave, Zack
    const std::map<int, int> r1 = c.Ranks(base_rank, 1, st);
    Check(r1.size() == 4 && uniq(r1) && rk(r1, 402) == base_rank + 1 && rk(r1, 397) == base_rank + 2 && rk(r1, 401) == base_rank + 3 && rk(r1, 398) == base_rank + 4,
          "ranks: page 1 = Alpha, Dave (397), Mick, Zack (398): the resident Mick's rank moved down one (the list rebuild picks it up)");
    // two cards made for one old seat: the same card rank, two seats: ranks from the model never collide
    Check(rk(r1, 397) != rk(r1, 398), "ranks: two guests never share one");
    // held by the screen: his rank follows the listing too
    sb::State held;
    held.guests[397] = "0000000D";
    held.screen_held.insert(397);
    const std::map<int, int> rh = c.Ranks(base_rank, 0, held);
    // (S110 SEAT3-R d1) page 0 = {bob, Carl}; bob's slot is the held seat 397 (skipped), Carl takes 398; the listing is Alpha (402), Carl (398), Dave (397), Mick (401)
    Check(uniq(rh) && rh.size() == 4 && rk(rh, 402) == base_rank + 1 && rk(rh, 398) == base_rank + 2 && rk(rh, 397) == base_rank + 3 && rk(rh, 401) == base_rank + 4,
          "ranks: a guest held by a panel keeps his seat and is ranked in the alphabet (Alpha, Carl, then Dave, Mick)", N(rk(rh, 397)));
    Check(sb::Catalog::Make({}, frame, {}, 0).Ranks(base_rank, 0, st).empty(), "ranks: nobody listed, no ranks (no crash)");
  }

  // ============================== the card's EV refs, their names, the blob's own length (lane G-B)
  {
    auto dir = [](const char* tag, uint32_t kind, const std::vector<std::array<uint8_t, 16>>& rows16) {
      std::vector<uint8_t> v(tag, tag + 4);
      uint8_t w[4];
      sb::PutBe32(w, uint32_t(rows16.size() * (kind ? 16 : 12) / 4) << 20 | (kind ? 0x1000u : 0u));
      v.insert(v.end(), w, w + 4);
      sb::PutBe32(w, 0);
      v.insert(v.end(), w, w + 4);
      for (const auto& r : rows16) v.insert(v.end(), r.begin(), r.begin() + (kind ? 16 : 12));
      return v;
    };
    auto row = [](const char* name8, uint32_t sector, uint32_t size) {
      std::array<uint8_t, 16> r{};
      std::memcpy(r.data(), name8, 8);
      sb::PutBe32(&r[8], sector);
      sb::PutBe32(&r[12], size);
      return r;
    };
    std::vector<uint8_t> rows = dir("EMD ", 1, {row("00039701", 7, 9)});
    const std::vector<uint8_t> evp = dir("EVP ", 1, {row("160     ", 3, 5), row("10160   ", 4, 6), row("6150    ", 5, 7)});
    const std::vector<uint8_t> evz = dir("EVZ ", 1, {row("170     ", 8, 8)});
    rows.insert(rows.end(), evp.begin(), evp.end());
    rows.insert(rows.end(), evz.begin(), evz.end());
    const uint8_t term[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    rows.insert(rows.end(), term, term + 4);
    std::vector<sb::EvRef> refs;
    std::string err;
    Check(sb::EvpRefs(rows, refs, err) && refs.size() == 3, "EV refs: the three rows of the EVP directory (not the EMD / EVZ ones)", err + " n=" + N(int64_t(refs.size())));
    if (refs.size() == 3)
      Check(refs[0].name == "160" && refs[1].name == "10160" && refs[2].name == "6150" && refs[0].sector == 3 && refs[0].size == 5 && refs[2].sector == 5 && refs[2].size == 7,
            "EV refs: names without their padding, sector and size as the rows have them");
    std::vector<uint8_t> noevp = dir("EMD ", 1, {row("00039701", 7, 9)});
    noevp.insert(noevp.end(), term, term + 4);
    Check(sb::EvpRefs(noevp, refs, err) && refs.empty(), "EV refs: a card with no EVP directory: none, no error", err);
    std::vector<uint8_t> open = rows;
    open.resize(open.size() - 4);
    Check(!sb::EvpRefs(open, refs, err), "EV refs: rows with no terminator are refused");
    Check(sb::EvName("160", 150, 397, 160, 170) == "170" && sb::EvName("10160", 150, 397, 160, 170) == "10170" && sb::EvName("6150", 150, 397, 160, 170) == "6397",
          "EV name: the bank, a part bank and 6000 + t move to the seat's");
    Check(sb::EvName("999", 150, 397, 160, 170) == "999" && sb::EvName("ab", 150, 397, 160, 170) == "ab" && sb::EvName("1234567", 150, 397, 160, 170) == "1234567",
          "EV name: another number, a word, a seven-digit string stay");
    std::vector<uint8_t> blob(21 + 40, 0);
    std::memcpy(blob.data(), "BPE ", 4);
    sb::PutLe32(&blob[8], 45);  // packed + 16 = 61 = the whole blob
    std::vector<uint8_t> padded = blob;
    padded.resize(256, 0);
    Check(sb::EvBlobTrim(padded) == blob, "EV trim: a BPE blob read in whole 256-byte units is cut at its own length");
    std::vector<uint8_t> raw = padded;
    std::memcpy(raw.data(), "ZLIB", 4);
    Check(sb::EvBlobTrim(raw) == raw, "EV trim: a blob that is not BPE is left as it was");
    std::vector<uint8_t> lie = padded;
    sb::PutLe32(&lie[8], 5000);
    Check(sb::EvBlobTrim(lie) == lie, "EV trim: a header that claims more than was read is left as it was");
  }

  // ============================== ReaskStores
  {
    const sb::Reask idle = sb::ReaskStores(450, 0, 450), loaded = sb::ReaskStores(450, 2, 450), loading = sb::ReaskStores(450, 1, 450), cancelling = sb::ReaskStores(450, 3, 450);
    Check(idle.touch && !idle.cancel_first && idle.stores.size() == 2 && idle.stores[0].off == 32 && idle.stores[0].value == 0 && idle.stores[1].off == 36 && idle.stores[1].value == 1,
          "re-ask: an idle entry of the seat: +32 = 0, +36 = 1");
    Check(loaded.touch && !loaded.cancel_first && loaded.stores.size() == 2 && loaded.stores[1].off == 36, "re-ask: a loaded entry: the same two stores");
    Check(loading.touch && loading.cancel_first && loading.stores.size() == 3 && loading.stores[0].off == 52 && loading.stores[0].value == 3 && loading.stores[2].off == 36 && loading.stores[2].value == 1,
          "re-ask: a loading entry: the loader's cancel, +52 = 3, then +32 = 0, +36 = 1");
    Check(cancelling.touch && cancelling.again && cancelling.stores.empty() && !cancelling.cancel_first, "re-ask: a cancelling entry (state 3) is left, ask again later");
    const sb::Reask other = sb::ReaskStores(451, 2, 450);
    Check(!other.touch && other.stores.empty() && !sb::ReaskStores(450, 2, -1).touch, "re-ask: an entry of another id is not touched; a negative seat touches nothing");
  }

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  std::printf("%s bind_unit\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
