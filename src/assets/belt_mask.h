// S111 STOCKBELT + BELTMASKNAME (docs\research\s111\STOCKBELT.md, docs\research\s111\BELTMASKNAME.md; src\scene_live\belt_colour.h): the R5 recolour masks of the stock belts in pac\cab\CAB.pac.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only (the loader, live_assets.cpp's "beltmask:" entry, and the offline unit tools\scene\stockbelt_unit.cpp compile it).
//
// The file [C, the player's CAB.pac read offline]: an EPAC pac (XCTD-wrapped) whose directory sections CBPM (140 rows: the stock belt prop ids 2001 .. 7009), CBWM (51 rows: the m2 belt sub-keys) and CBLM
// (46 rows: the /STGL ladder rows) are 12-byte rows; each row's PACH holds ONE entry 0, a texture directory with ONE texture `Mask1` (CBPM / CBLM 512 x 512, CBWM 256 x 256), 16-bit, channel masks R 0x0F00
// G 0x00F0 B 0x000F A 0xF000 (= the SPMT masks' layout, kB4G4R4A4), one level. The game requests it by "/CBPM/%04d" (mode 0, the prop id), "/CBWM/%04d" (mode 1) or "/CBLM/%04d" (mode 2, the title's +8)
// (sub_83331E58) and T_SSComp reads its four channels as the four colours' masks. Ours reads ONE mask when a created title's block names it (belt_colour.h MaskEntryOf; the drawer binds it, BindBeltMask).
// S111 STOCKBELT review: CAB.pac (9 MB packed, 35 MB unpacked) is read BY BLOCKS (the S105 SKB3 reader, xctd_ranged.h): the block index and the EPAC directory once, then one slice per asked mask
// (its PACH's own blocks), never the whole unpack. A pac that is not XCTD (a mods-folder copy) is read by plain ranges the same way.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "asset_load.h"

namespace assets {

inline constexpr const char* kCabPac = "pac\\cab\\CAB.pac";
inline constexpr const char* kBeltMaskSections[3] = {"CBPM", "CBWM", "CBLM"};   // by R5 mode (belt_colour.h MaskSection)

// Every id of an EPAC directory section (the walk of PropDirStartIn, collecting).
inline std::set<uint32_t> SectionIds(const Bytes& b, const char* section) {
  std::set<uint32_t> ids;
  if (b.size() < 0x4000) return ids;
  if (std::memcmp(b.data(), "EPAC", 4) != 0 && std::memcmp(b.data(), "EPK8", 4) != 0) return ids;
  std::string cur(reinterpret_cast<const char*>(b.data() + 0x800), 4);
  for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
    const uint8_t* r = b.data() + o;
    const uint32_t st = Le32(b, o + 4);
    const bool is_id = std::isdigit(r[0]) && std::isdigit(r[1]) && std::isdigit(r[2]) && std::isdigit(r[3]);
    if (!is_id) {
      if (r[0] == 0 && st == 0) break;
      if (std::memcmp(r, "LIST", 4) != 0) cur.assign(reinterpret_cast<const char*>(r), 4);
      continue;
    }
    if (cur == section) ids.insert(uint32_t(std::atoi(std::string(reinterpret_cast<const char*>(r), 4).c_str())));
  }
  return ids;
}

// The PACH ordinal of /<section>/<id> in an opened CAB.pac, -1 = no row.
inline int BeltMaskPach(const pac::Pac& p, uint32_t id, const char* section = "CBPM") {
  size_t at = 0;
  if (!PropDirStartIn(p.bytes, id, section, at)) return -1;
  for (size_t i = 0; i < p.pachs.size(); ++i)
    if (p.pachs[i].base == at) return int(i);
  return -1;
}

// The `Mask1` of PACH `ord` of `p` (entry 0 = a texture directory), as the file stores it.
inline bool LoadMask1OfPach(const pac::Pac& p, size_t ord, Texture& out, std::string& err) {
  if (ord >= p.pachs.size()) return err = "PACH " + std::to_string(ord) + " is not in the pac", false;
  const pac::Ent* e = pac::Find(p.pachs[ord], 0);
  if (!e) return err = "PACH " + std::to_string(ord) + " has no entry 0", false;
  TexSet ts;
  if (!ts.AddDir(pac::Decode(p.bytes.data() + e->off, e->size))) return err = "PACH " + std::to_string(ord) + " entry 0 is not a texture directory", false;
  std::string why;
  if (!ts.Peek("Mask1", out, why)) return err = "PACH " + std::to_string(ord) + ": Mask1 " + why, false;
  return true;
}

// The `Mask1` of /<section>/<id> in a WHOLE opened CAB.pac (the unit's reference for the ranged read below). False + `err` when the row / PACH / entry / texture is not what the rule above says.
inline bool LoadBeltMaskFrom(const pac::Pac& p, uint32_t id, Texture& out, std::string& err, const char* section = "CBPM") {
  const int ord = BeltMaskPach(p, id, section);
  if (ord < 0) return err = std::string("no /") + section + "/" + std::to_string(id) + " row in CAB.pac", false;
  return LoadMask1OfPach(p, size_t(ord), out, err);
}

// The loader's cache (loader thread, under the load lock: no second thread). Opened once per GameFiles / root: the ranged reader, the XCTD block index and the 16 KB EPAC directory; then each asked mask
// decodes its own PACH's bytes only (load_detail::EvtDirRows: the row's start and size) and is kept (bounded). A CAB.pac that cannot be opened is said ONCE (kError), then every ask is kNone.
class BeltMaskCache {
 public:
  enum class Got { kNone /* no such row (or CAB.pac unreadable, said once) */, kMask, kError };
  static constexpr size_t kMaxMasks = 16;   // 128 / 512 KB each
  int plant_row_shift = 0;                  // TEST ONLY (stockbelt_unit --plant slice_next_row): decode the directory row after the asked one
  Got Get(launcher::GameFiles& files, const char* section, uint32_t id, Texture& out, std::string& why) {
    if (!Open(files)) {
      if (said_) return Got::kNone;
      said_ = true;
      return why = why_, Got::kError;
    }
    const std::string sec = section ? section : "";
    const auto ids = ids_.find(sec);
    if (ids == ids_.end() || !ids->second.count(id)) return Got::kNone;
    const auto key = std::make_pair(sec, id);
    if (auto it = masks_.find(key); it != masks_.end()) return out = it->second, Got::kMask;
    const std::string tag = "/" + sec + "/" + std::to_string(id);
    size_t at = 0;
    if (!PropDirStartIn(header_, id, sec.c_str(), at)) return why = tag + ": no directory row", Got::kError;
    auto row = std::lower_bound(rows_.begin(), rows_.end(), std::make_pair(at, size_t(0)));
    if (row == rows_.end() || row->first != at) return why = tag + ": no directory row starts at its PACH", Got::kError;
    if (plant_row_shift > 0 && rows_.end() - row > plant_row_shift) row += plant_row_shift;
    if (row->first >= total_) return why = tag + ": PACH start past the pac", Got::kError;
    pac::Pac slice;
    std::string e;
    if (!ReadRange(row->first, std::min<size_t>(row->second, size_t(total_ - row->first)), slice.bytes, e)) return why = tag + ": " + e, Got::kError;
    pac::Pach h;
    if (!pac::ParsePach(slice.bytes, 0, slice.bytes.size(), h)) return why = tag + ": its PACH does not parse", Got::kError;
    slice.pachs.push_back(std::move(h));
    Texture t;
    if (!LoadMask1OfPach(slice, 0, t, why)) return why = tag + ": " + why, Got::kError;
    ++slices_;
    if (masks_.size() >= kMaxMasks) masks_.clear();
    out = masks_[key] = std::move(t);
    return Got::kMask;
  }
  size_t Ids(const char* section) const {
    const auto it = ids_.find(section ? section : "");
    return it == ids_.end() ? 0 : it->second.size();
  }
  int Slices() const { return slices_; }   // PACH slices decoded (the unit: one per distinct mask asked)
  bool Ranged() const { return xctd_; }   // true = the XCTD block reader (the player's disc pac)

 private:
  bool Open(launcher::GameFiles& files) {
    const std::string root = files.root().string();
    if (tried_ && (files_id_ != static_cast<const void*>(&files) || root_ != root)) {   // another GameFiles / root (the viewer's SetRoot): the old reader is dropped
      tried_ = ok_ = xctd_ = said_ = false;
      rr_ = {};
      ix_ = {};
      header_.clear();
      rows_.clear();
      ids_.clear();
      masks_.clear();
      why_.clear();
    }
    if (tried_) return ok_;
    tried_ = true, files_id_ = &files, root_ = root;
    const auto fail = [&](const std::string& w) { why_ = std::string(kCabPac) + ": " + w; rr_ = {}; return false; };
    if (!files.OpenRange(kCabPac, rr_) || !rr_.read) return fail("cannot open");
    uint8_t magic[4] = {};
    if (rr_.size < 4 || !rr_.read(0, magic, 4)) return fail("cannot read");
    std::string e;
    xctd_ = wwe2k14::xctd_ranged::Be32(magic) == 0x0FF512EDu;
    if (xctd_ && !wwe2k14::xctd_ranged::BuildIndex(rr_.read, rr_.size, ix_, e)) return fail("XCTD block index: " + e);
    total_ = xctd_ ? ix_.out_total : rr_.size;
    if (!ReadRange(0, 0x4000, header_, e)) return fail("directory: " + e);
    if (std::memcmp(header_.data(), "EPAC", 4) != 0 && std::memcmp(header_.data(), "EPK8", 4) != 0) return fail("no EPAC header");
    rows_ = load_detail::EvtDirRows(header_);
    size_t any = 0;
    for (const char* s : kBeltMaskSections) any += (ids_[s] = SectionIds(header_, s)).size();
    if (any == 0) return fail("no CBPM / CBWM / CBLM section in the directory");
    return ok_ = true;
  }
  bool ReadRange(size_t off, size_t len, Bytes& out, std::string& e) {
    if (xctd_) return wwe2k14::xctd_ranged::DecodeRange(rr_.read, ix_, off, len, out, e);
    if (off + len > rr_.size) return e = "range past the file", false;
    out.assign(len, 0);
    return len == 0 || rr_.read(off, out.data(), len) || (e = "read failed", false);
  }
  bool tried_ = false, ok_ = false, xctd_ = false, said_ = false;
  const void* files_id_ = nullptr;
  std::string root_, why_;
  launcher::GameFiles::RangeReader rr_;
  wwe2k14::xctd_ranged::Index ix_;
  uint64_t total_ = 0;
  Bytes header_;
  std::vector<std::pair<size_t, size_t>> rows_;
  std::map<std::string, std::set<uint32_t>> ids_;
  std::map<std::pair<std::string, uint32_t>, Texture> masks_;
  int slices_ = 0;
};
inline BeltMaskCache& TheBeltMasks() {
  static BeltMaskCache c;
  return c;
}

}  // namespace assets
