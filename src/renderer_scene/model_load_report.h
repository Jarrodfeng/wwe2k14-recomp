// S108 AL1 (docs\research\s108\AL1_plan.md row 4): the ONE reader of what the loader could not give, for EVERY model the live renderer adopts (LiveSceneDraw::AdoptLoaded: arenas, wrestlers,
// props, weapons, crowd bodies). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only (no renderer, no game): the offline unit (tools\weapons\al1_sites_unit.cpp) runs THIS.
//   Model::shortfalls (assets::NoteDropped: a part whose JBOY failed to load, a pac of shared textures that did not open) -> rdr::DrawShortfall (kinds model_part / texture_pac): a finding.
//   a "texture '<name>' not found" warning (dds.h TexSet::Get) -> rdr::DrawNote, a plain line once per (model, texture), at most kMaxNoteLines a run: NOT a finding, because it fires by design
//   (the S101 atlas: 76219 of 140140 texture references name a texture outside their own pac's directories; dds.h: the game binds nothing for a borrowed name). Was wrestlers only
//   (live_assets.cpp WrestlerTexWarnings, S105 A7).
//   LoadCensus: the tally of both (problem.h DrawShortfallTally) as one line, written only when it changed: a jump in the notes (hundreds of names not found) shows in the log.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "../assets/asset_model.h"
#include "../renderer/problem.h"

namespace rdr_scene {

// `what` = the model as the loader was asked for it ("arena 5", "pac\ch\ch394.pac model_key:39400 variant 0"). Called once per adopted model.
inline void ReportLoadedModel(const std::string& what, const assets::Model& m) {
  for (const assets::Model::Shortfall& s : m.shortfalls) {
    const bool pac = std::string(s.kind) == "texture_pac";   // a pac of shared textures: ONE key for every model that needed it (the pac is the thing to find)
    const std::string object = pac ? s.part : what + ": " + s.part;
    rdr::DrawShortfall(s.kind, object.c_str(), 1, 0, pac ? "first seen loading %s: %s" : "%s%s", pac ? what.c_str() : "", s.why.c_str());
  }
  for (const std::string& w : m.warnings) {
    if (w.compare(0, 9, "texture '") != 0 || w.find("not found") == std::string::npos) continue;
    const std::string key = what + "|" + w;
    rdr::DrawNote("texture_not_found", key.c_str(), "[wwe2k14][model_load] %s: %s: the sampler stays unbound (a diffuse then draws white; a normal or wrinkle map just stays off)", what.c_str(), w.c_str());
  }
}

// The census of the shared report and the notes: "" when nothing changed since the last call (the caller logs a non-empty answer).
inline std::string LoadCensus() {
  static rdr::ShortfallTally last;
  static bool any = false;
  const rdr::ShortfallTally t = rdr::DrawShortfallTally();
  if (any && t.distinct == last.distinct && t.unkeyed == last.unkeyed && t.notes == last.notes && t.notes_unkeyed == last.notes_unkeyed) return std::string();
  any = true, last = t;
  char b[320];
  std::snprintf(b, sizeof b, "[scene][load] shortfall census: draw_shortfall %llu (named %llu, past the key store %llu); texture not found notes %llu (written %llu, past the key store %llu)",
                static_cast<unsigned long long>(t.distinct), static_cast<unsigned long long>(t.named), static_cast<unsigned long long>(t.unkeyed), static_cast<unsigned long long>(t.notes),
                static_cast<unsigned long long>(t.notes_named), static_cast<unsigned long long>(t.notes_unkeyed));
  return b;
}

}  // namespace rdr_scene
