// S103 CAR (BCA G1): the merge of a Create-an-Arena venue's four part PACHs into one synthetic pac (see car_sel.h for the why and the registry). Pure over pac::Pac; the file reads are in BuildFromFiles.
// Header only (the unit tools\scene\car_arena_unit.cpp includes it). Needs stb_image.h before it (pac.h).
//
// Merge rules (all by the files, BCA / asset.db of the player's own pacs):
//   * ids <400, 800-815, 995-999, 1012, 2xxxx, 31000-32010, 4xxxx, 5xxxx, 6xxxx are unique across the three PACHs (a duplicate is counted: first wins, order CASD, CAEN, CARG);
//   * texture directories: CASD 400 -> 400, CAEN 401 -> 401, CARG 402 -> 402, CAEN 400 -> 403, CARG 400 -> 404 (LoadArena reads 400-404 for a created arena);
//   * the light entries 30000 / 30002 / 30003: the CARL PACH's (BCA: the light set the stage takes), the CASD PACH's only when CARL has none; 30001 (the stage motions) is CAEN's;
//   * 50001 (the slot text): the three texts joined, CARG then CAEN then CASD.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "car_sel.h"
#include "pac.h"

namespace assets {
namespace car {

struct PartSrc {
  const pac::Pac* pac = nullptr;
  const pac::Pach* pach = nullptr;
};

// Merges the four parts into `out` (one PACH, bytes copied: the parts may be freed). A part may be absent (null): the merge then lacks its band. false = nothing usable (no scenery PACH at all).
inline bool MergeParts(const PartSrc& casd, const PartSrc& caen, const PartSrc& carg, const PartSrc& carl, pac::Pac& out, MergeInfo& info, std::string& err) {
  out = {};
  const bool ok_casd = casd.pac && casd.pach, ok_caen = caen.pac && caen.pach, ok_carg = carg.pac && carg.pach, ok_carl = carl.pac && carl.pach;
  info.have_casd = ok_casd, info.have_caen = ok_caen, info.have_carg = ok_carg, info.have_carl = ok_carl;
  if (!ok_casd && !ok_caen && !ok_carg) return err = "created arena: no scenery part (CASD / CAEN / CARG) could be read", false;
  pac::Pach merged;
  auto has = [&](uint32_t id) {
    for (const pac::Ent& e : merged.ents)
      if (e.id == id) return true;
    return false;
  };
  auto append = [&](uint32_t id, const uint8_t* d, size_t n) {
    while (out.bytes.size() % 16) out.bytes.push_back(0);
    pac::Ent e;
    e.id = id, e.off = out.bytes.size(), e.size = n;
    out.bytes.insert(out.bytes.end(), d, d + n);
    merged.ents.push_back(e);
  };
  enum Src { kCasd, kCaen, kCarg, kCarl };
  auto take = [&](const PartSrc& s, Src which) {
    for (const pac::Ent& e : s.pach->ents) {
      uint32_t id = e.id;
      if (id == 50001) continue;   // joined below
      if (which == kCarl) {        // the light entries are the CARL PACH's; its other entries (none shipped) are kept first-wins
      } else if ((id == 30000 || id == 30002 || id == 30003) && ok_carl && pac::Find(*carl.pach, id)) {
        continue;                  // the CARL PACH has this light entry: it wins
      } else if (id == 400) {
        id = which == kCasd ? 400u : which == kCaen ? 403u : 404u;
      }
      if (has(id)) { ++info.duplicates; continue; }
      if (e.off > s.pac->bytes.size() || e.size > s.pac->bytes.size() - e.off) continue;
      append(id, s.pac->bytes.data() + e.off, e.size);
      info.part_of[id] = uint8_t(which);
      if (id < 400) {
        uint32_t* n = which == kCasd ? &info.slots_casd : which == kCaen ? &info.slots_caen : &info.slots_carg;
        uint32_t* lo = which == kCasd ? &info.slot_min_casd : which == kCaen ? &info.slot_min_caen : &info.slot_min_carg;
        uint32_t* hi = which == kCasd ? &info.slot_max_casd : which == kCaen ? &info.slot_max_caen : &info.slot_max_carg;
        ++*n, *lo = std::min(*lo, id), *hi = std::max(*hi, id);
      }
      if (id == 30000 || id == 30002) ++(which == kCarl ? info.lights_from_carl : info.lights_from_casd);
    }
  };
  if (ok_carl) take(carl, kCarl);
  if (ok_casd) take(casd, kCasd);
  if (ok_caen) take(caen, kCaen);
  if (ok_carg) take(carg, kCarg);
  Bytes text;   // 50001: the slot text of the three scenery parts, CARG then CAEN then CASD
  for (const PartSrc* s : {&carg, &caen, &casd}) {
    if (!s->pac || !s->pach) continue;
    const pac::Ent* e = pac::Find(*s->pach, 50001);
    if (!e || e->off > s->pac->bytes.size() || e->size > s->pac->bytes.size() - e->off) continue;
    const Bytes t = pac::Decode(s->pac->bytes.data() + e->off, e->size);
    if (t.empty()) continue;
    text.insert(text.end(), t.begin(), t.end());
    if (text.back() != '\n') text.push_back('\n');
    info.text_joined = true;
  }
  if (!text.empty()) append(50001, text.data(), text.size());
  merged.base = 0, merged.end = out.bytes.size();
  info.entries = uint32_t(merged.ents.size());
  out.pachs.push_back(std::move(merged));
  return true;
}

// The part PACH `idx` of a top-level pac, else PACH 0 (the game's own fallback for a missing part: sub_8342A980 / sub_8342A8D8 retry index 0). `fell_back` says it did.
inline const pac::Pach* PartPach(const pac::Pac& p, uint32_t idx, uint32_t& used, bool& fell_back) {
  fell_back = false;
  if (idx < p.pachs.size()) return used = idx, &p.pachs[idx];
  if (p.pachs.empty()) return nullptr;
  fell_back = true, used = 0;
  return &p.pachs[0];
}

// Opens the player's own part pacs and merges the venue `sel` names. The four pacs are unpacked here and dropped when the merge is made (the merged copy holds only the entries used).
inline bool BuildFromFiles(launcher::GameFiles& files, const Sel& sel, pac::Pac& out, MergeInfo& info, std::string& err) {
  info = MergeInfo();
  pac::Pac ring, ent, stud, light;
  std::string e1, e2, e3, e4;
  const bool o_ring = pac::Open(files, kPacRing, ring, e1), o_ent = pac::Open(files, kPacEnt, ent, e2), o_stud = pac::Open(files, kPacStudium, stud, e3), o_light = pac::Open(files, kPacLight, light, e4);
  PartSrc s_casd, s_caen, s_carg, s_carl;
  if (o_stud) s_casd = {&stud, PartPach(stud, sel.casd, info.casd, info.casd_fallback)};
  if (o_ent) s_caen = {&ent, PartPach(ent, sel.caen, info.caen, info.caen_fallback)};
  if (o_ring) {
    uint32_t unused = 0;
    bool fb = false;
    s_carg = {&ring, PartPach(ring, 0, unused, fb)};   // /CARG/0000: the only ring part
  }
  if (o_light) s_carl = {&light, PartPach(light, sel.carl, info.carl, info.carl_fallback)};
  if (!MergeParts(s_casd, s_caen, s_carg, s_carl, out, info, err)) {
    err += " (" + std::string(o_stud ? "" : e3 + "; ") + (o_ent ? "" : e2 + "; ") + (o_ring ? "" : e1 + "; ") + ")";
    return false;
  }
  return true;
}

}  // namespace car
}  // namespace assets
