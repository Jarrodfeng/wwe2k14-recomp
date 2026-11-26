// A material record's two words -> Material (S111 UNIFY3, docs\research\s111\UNIFY3.md; UNIFYMAT step 3 and the start of step 5's one resolver). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: Material only. Two readers call it with the SAME two words, so the stock and the created-superstar materials cannot drift: the file loader (jboy.h: the mesh record's +0x90 / +0x8C) and the guest
// model build (guest_model_draw.h BuildModel: section 72's words, read from [desc+144] / [desc+140] of the game's byte copy of that record, sub_82BDB460 +0x308 memcpy 184).
#pragma once
#include <cstdint>

#include <string_view>

#include "asset_model.h"
#include "material_floats.h"   // S111 UNIFY5: BeginRecord (the effect's name table)

namespace assets::matrec {

// technique: the record word +0x90, the technique index the colour pass hands the effect (S103 D1; sub_82C04DF0 0x82C05AF8). cull_word: the record word +0x8C as stored; bit 0x2 set = the game's cull block
// (sub_82C04DF0 0x82C0A880-0x82C0A914) draws the mesh once with back faces culled, for every family (S105 C3); the front end's family / stage switches turn exactly these off (cull_from_record).
inline void ApplyRecordWords(Material& mat, uint32_t technique, uint32_t cull_word) {
  mat.technique = technique;
  mat.cull_word = cull_word;
  mat.cull_from_record = (cull_word & 0x2u) != 0;
  mat.cull_back = mat.cull_from_record;
}

// S111 UNIFY5 (docs\research\s111\UNIFY5.md; UNIFYMAT step 5): the record's HEADER -> Material, the first step of both builders. `effect` = the record's +0x7C effect name (the file loader: the file's
// spelling; the guest build: the registry's name for the game's in-memory one, guest_model_draw.h PipelineEffect), then its name table (matfloat::BeginRecord: the family and the yCh missing-power default)
// and the two words. Every later step (the samplers, the floats) reads what this sets.
inline void BeginMaterial(Material& mat, std::string_view effect, uint32_t technique, uint32_t cull_word) {
  mat.shader = std::string(effect);   // (a plain field write: q guestdiff reads it as both builders' fill)
  matfloat::BeginRecord(mat, effect);
  ApplyRecordWords(mat, technique, cull_word);
}

}  // namespace assets::matrec
