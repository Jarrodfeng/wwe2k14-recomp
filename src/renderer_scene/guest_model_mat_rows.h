// A guest part's material record (section 72, kScGuestModelMats) and its model's texture names (section 73, kScGuestModelTexNames) in the scene record (S111 EYECUBE, docs\research\s111\EYECUBE.md;
// the shape of UNIFYMAT step 3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure: no game or SDK includes.
// The writer (guest_model_emit.h AppendModelRows) adds one 72 row per sampler the reader kept (every one; one words-only row for a part with none) and one 73 row per (model, t) whose name was read; the
// consumer (guest_model_draw.h BuildModel) asks NameOf for the texture name the sampler of a parameter NAME names, the name the game resolves (sub_82BD9A60). A record without the sections (an older
// one) carries no row: NameOf says so and the caller keeps its old rule.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../scene_live/guest_model_reader.h"
#include "guest_model_rows.h"

namespace rdr_scene::guestmats {

// Whether `g` already holds the 73 row of (model, t). A model's rows are written at once (AppendModelRows), so its names are the table's last rows: the scan stops at another model's row (a model
// written twice in one record would only get a duplicate row, which NameOf never reads).
inline bool HaveName(const GuestModelTables& g, uint32_t model, uint32_t t) {
  for (size_t k = g.tex_names.size(); k-- > 0 && g.tex_names[k].model == model;)
    if (g.tex_names[k].t == t) return true;
  return false;
}
// The writer: part `p` of `model` -> one GuestModelMat row per sampler the reader kept (all of them: this section has no cap), or one words-only row when it kept none, and the names of the t they
// name (once per model and t a record).
inline void AppendRows(uint32_t model, const live::guest_model::PartRead& p, GuestModelTables& g) {
  GuestModelMat row;
  row.model = model, row.part = uint16_t(p.part), row.technique = p.technique, row.cull = p.cull;
  if (p.samplers.empty()) return g.mats.push_back(row);
  for (size_t si = 0; si < p.samplers.size(); ++si) {
    const live::guest_model::SamplerRead& s = p.samplers[si];
    row.sampler = uint16_t(si), row.t = s.t;
    std::memcpy(row.param, s.name, kGmNameBytes);
    g.mats.push_back(row);
    if (s.t < 0 || !s.tex_name[0] || HaveName(g, model, uint32_t(s.t))) continue;
    GuestModelTexName n;
    n.model = model, n.t = uint32_t(s.t);
    std::memcpy(n.name, s.tex_name, kGmNameBytes);
    g.tex_names.push_back(n);
  }
}

// What the record says the sampler of parameter `param` (the part row's tex[i].name: the same 16 bytes, copied from the same read) of `model` / `part` names: joined by the NAME, never by place, so a
// part row that keeps fewer samplers than section 72 carries (its kGmMaxTex) still finds each one's own t. carried = false: no 72 row for the part (a record written before it). name empty: no 72 row
// for that parameter, or no name carried for its t.
struct SamplerName {
  bool carried = false;
  std::string name;
};
inline SamplerName NameOf(const GuestModelTables& g, uint32_t model, uint32_t part, const char (&param)[kGmNameBytes]) {
  SamplerName out;
  for (const GuestModelMat& m : g.mats) {
    if (m.model != model || m.part != part) continue;
    out.carried = true;
    if (m.sampler == kGmMatNoSampler || m.t < 0 || std::memcmp(m.param, param, kGmNameBytes) != 0) continue;
    for (const GuestModelTexName& n : g.tex_names)
      if (n.model == model && n.t == uint32_t(m.t)) {
        out.name.assign(n.name, strnlen(n.name, kGmNameBytes));
        break;
      }
    return out;
  }
  return out;
}

// S111 UNIFY3: the part's record words as section 72 carries them (every row of the part repeats them: [desc+144] the technique, [desc+140] the cull word). carried = false: no 72 row for the part (a record
// written before it): the caller keeps the Material's defaults.
struct RecordWords {
  bool carried = false;
  uint32_t technique = 0, cull = 0;
};
inline RecordWords WordsOf(const GuestModelTables& g, uint32_t model, uint32_t part) {
  RecordWords out;
  for (const GuestModelMat& m : g.mats)
    if (m.model == model && m.part == part) {
      out.carried = true, out.technique = m.technique, out.cull = m.cull;
      break;
    }
  return out;
}

// S111 UNIFY5: the name section 73 holds for (model, t): what the model's texture-name table names at t ([m+268]+16t, sub_82BD9A60's name). Empty = no name carried, or t < 0.
inline std::string TexNameOf(const GuestModelTables& g, uint32_t model, int32_t t) {
  if (t >= 0)
    for (const GuestModelTexName& n : g.tex_names)
      if (n.model == model && n.t == uint32_t(t)) return std::string(n.name, strnlen(n.name, kGmNameBytes));
  return std::string();
}

}  // namespace rdr_scene::guestmats
