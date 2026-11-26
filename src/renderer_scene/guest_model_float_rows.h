// A guest part's float parameters in the scene record (section 71, kScGuestModelParams) and on the built Material (S111 GUESTHAIR, docs\research\s111\GUESTHAIR.md). Copyright (c) the wwe2k14-recomp authors.
// BSD-3-Clause. Pure: no game or SDK includes. The writer (guest_model_emit.h AppendModelRows) keeps the rows the ONE table takes (assets\material_floats.h, the table jboy.h's file loader uses);
// the consumer (guest_model_draw.h BuildModel) puts them on the part's Material by the same table, so a created superstar's material carries the file values a stock wrestler's does
// (the hair highlight SpeCol1 / SpeCol2 / shifts / exponents / tiles, the specular, rim, SSS, noise, wet values). A record without the section (an older one) builds the fresh Material, as before.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "../assets/material_floats.h"
#include "../scene_live/guest_model_floats.h"
#include "guest_model_rows.h"

namespace rdr_scene::guestfloats {

// S111 UNIFY3 (the UNIFY2 review): the game's parameter-name lookup compares EXACTLY (sub_82BAAEE8 0x82BADF6C..: 16 bytes against each row) and the model build copies the file's names as is, so the
// rows go through the table's one exact compare, as the file loader's do (only the EFFECT name is folded: material_floats.h YchTable). The old folded compare is the table's test twin (matfloat::g_fold_plant).
inline std::string_view NameOf(const char* n) { return std::string_view(n, strnlen(n, kGmNameBytes)); }

// The writer: part `part` of `model`'s float rows that the table takes -> rows. Returns the rows added.
inline uint32_t AppendRows(uint32_t model, uint32_t part, const std::vector<live::guest_model::FloatParamRead>& floats, std::vector<GuestModelParam>& out) {
  uint32_t added = 0;
  for (const live::guest_model::FloatParamRead& f : floats) {
    if (!assets::matfloat::IsFloatParam(NameOf(f.name), f.kind)) continue;
    GuestModelParam row;
    row.model = model, row.part = uint16_t(part), row.kind = uint16_t(f.kind);
    std::memcpy(row.name, f.name, kGmNameBytes);
    std::memcpy(row.v, f.v, sizeof row.v);
    out.push_back(row), ++added;
  }
  return added;
}

// The consumer: puts every row of `model` / `part` on `mat` in record order (the part's own parameter order: a repeated name ends as in the file loader). Returns the material key's
// fragment ("" = no row: the fresh Material), so two parts with the same pictures but other values are two materials.
inline std::string ApplyRows(const GuestModelTables& g, uint32_t model, uint32_t part, assets::Material& mat) {
  uint64_t h = 0xcbf29ce484222325ull;
  const auto mix = [&](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
  };
  uint32_t n = 0;
  for (const GuestModelParam& r : g.params) {
    if (r.model != model || r.part != part) continue;
    if (!assets::matfloat::ApplyFloat(mat, NameOf(r.name), r.kind, r.v, nullptr)) continue;
    mix(r.name, sizeof r.name), mix(&r.kind, sizeof r.kind), mix(r.v, sizeof r.v);
    ++n;
  }
  if (!n) return std::string();
  char b[24];
  std::snprintf(b, sizeof b, "|F%016llx", (unsigned long long)h);
  return b;
}

}  // namespace rdr_scene::guestfloats
