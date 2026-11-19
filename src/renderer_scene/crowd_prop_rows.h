// The writer-facing API for the crowd's signs / foam hands and single standing people (Session 103, lane D4; docs\renderer\CROWD_PROPS_LATCH.md "Lane D4").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game, SDK or renderer includes: the latch readers (lane D3: the sign loops' pass 1 / pass 2, the single people),
// the offline rig and the unit all call THESE functions, so a row the reader appends is checked by the same code the unit proves.
//
// A reader reads the game's own tables at the frame's latch (the loop objects, the free-prop array, the person's matrices) and appends ONE row per thing the game would draw:
//   AppendProp(t, in)    a sign / foam hand: the kind (which of the two loops), the model key (0 = the kind's default), the world as the loop's VS constants c76-c78 (12 floats).
//   AppendSingle(t, in)  a single person: the body's asset (BodyAsset adds it), the world (12 floats), the skin palette (appended to the record's kScBones).
// Every refusal is COUNTED with a reason (Counts), never silent and never a half row: a non-finite or degenerate world, an unknown kind, a palette past the cap. The row is NOT appended on a
// refusal; the reader then marks its input stale (a sign missing from the record is a hole in the picture, not a guess: FRAME_CONTRACT 3).
// The applier is LiveCrowd (live_crowd.h): rows the record lists and it cannot draw (a model key not in the arena's audience set, a body that does not load) are counted holes with a reason.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "record_io.h"
#include "scene_record.h"

namespace rdr_scene::crowd_rows {

// The mesh a prop kind draws when its row names no key [C: docs\renderer\CROWD_PROMOTION.md N4, byte joins]: a sign (34 vertices / 77 indices) is the audience set's entry 40000. A foam hand's mesh is
// PER HAND TYPE: 40999 + h (S103 SG, sub_8250B1E8 0x8250B394), so its row names the key; 41016 (h = 17, 140 / 223) is only the fallback of a row that names none (the reader never writes one for a shown hand).
inline constexpr uint32_t kDefaultKeySign = 40000, kDefaultKeyHand = 41016;
inline constexpr uint32_t kMaxProps = 4096, kMaxSingles = 512, kMaxPalette = 512;   // caps per record (the game's loops hold 32 + 96 props; a count past these is a reader fault)
inline uint32_t DefaultKey(uint8_t kind) { return kind == kCpHand ? kDefaultKeyHand : kDefaultKeySign; }
inline uint32_t KeyOf(const CrowdPropRow& r) { return r.model_key ? r.model_key : DefaultKey(r.kind); }

enum class Result : uint8_t { Ok = 0, BadKind, BadWorld, Full, BadPalette, BadAsset };
inline const char* ResultName(Result r) {
  switch (r) {
    case Result::Ok: return "ok";
    case Result::BadKind: return "unknown prop kind";
    case Result::BadWorld: return "world is not finite or has a zero scale";
    case Result::Full: return "table full";
    case Result::BadPalette: return "palette is empty or past the cap or has a non-finite value";
    case Result::BadAsset: return "no body asset";
  }
  return "?";
}

// What the reader did this frame (it prints these; the census sums them).
struct Counts {
  uint32_t props_ok = 0, props_refused = 0, singles_ok = 0, singles_refused = 0;
  Result first_refusal = Result::Ok;
  std::string Text() const {
    char b[200];
    std::snprintf(b, sizeof b, "props %u (refused %u), singles %u (refused %u)%s%s", props_ok, props_refused, singles_ok, singles_refused, first_refusal == Result::Ok ? "" : "; first refusal: ", first_refusal == Result::Ok ? "" : ResultName(first_refusal));
    return b;
  }
};

// The row's 12 floats from a world matrix in the record's convention (row vectors, translation in elements 12..14): row r of the registers = column r of the matrix (CrowdMember::c's form).
inline void ConstantsFromWorld(const Mat4& w, float out[12]) {
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) out[4 * r + k] = w.m[k * 4 + r];
}
// ... and back: the matrix the registers describe (the applier's own copy of the rule lives in live_crowd.cpp WorldRows; the unit proves both are inverse).
inline Mat4 WorldFromConstants(const float c[12]) {
  Mat4 w{};
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) w.m[k * 4 + r] = c[4 * r + k];
  w.m[15] = 1.f;
  return w;
}

// A usable world: all 12 finite, the 3x3 part not collapsed (a zero-scale row draws nothing and is a reader fault: a wrong offset reads zeros).
inline bool& PlantAcceptBadWorld() {   // test only (tools\scene\d4_unit.cpp --plant accept_nan): the writer takes any world
  static bool b = false;
  return b;
}
inline bool WorldUsable(const float c[12]) {
  if (PlantAcceptBadWorld()) return true;
  for (int i = 0; i < 12; ++i)
    if (!std::isfinite(c[i])) return false;
  for (int r = 0; r < 3; ++r) {
    const float len2 = c[4 * r] * c[4 * r] + c[4 * r + 1] * c[4 * r + 1] + c[4 * r + 2] * c[4 * r + 2];
    if (!(len2 > 1e-12f)) return false;
  }
  return true;
}

struct PropIn {
  uint8_t kind = kCpSign;   // CrowdPropKind
  uint32_t model_key = 0;       // 0 = the kind's default
  uint16_t variant = 0;
  uint32_t flags = kCpfFree;    // CrowdPropFlags
  uint32_t picture = 0;
  uint32_t file_key = 0;        // S103 CSF: a sign's file key (CrowdPropRow::file_key)
  float c[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

inline Result AppendProp(SceneTables& t, const PropIn& in, Counts* counts = nullptr) {
  Result r = Result::Ok;
  if (in.kind != kCpSign && in.kind != kCpHand) r = Result::BadKind;
  else if (!WorldUsable(in.c)) r = Result::BadWorld;
  else if (t.crowd_props.size() >= kMaxProps) r = Result::Full;
  if (r != Result::Ok) {
    if (counts) {
      ++counts->props_refused;
      if (counts->first_refusal == Result::Ok) counts->first_refusal = r;
    }
    return r;
  }
  CrowdPropRow row;
  row.kind = in.kind, row.variant = in.variant, row.model_key = in.model_key, row.flags = in.flags, row.picture = in.picture, row.file_key = in.file_key;
  std::memcpy(row.c, in.c, sizeof row.c);
  t.crowd_props.push_back(row);
  if (counts) ++counts->props_ok;
  return Result::Ok;
}

// The body asset of person `type` at `lod` (default LOD 0) of `arena` in the record's asset table (added once, found again on the next call): exactly the AssetRef a CrowdRun names (entry 20000 + 40 * LOD + type,
// live_assets.cpp's rule; S103 C1: the LOD 1 / 2 holders, entity index 20..59).
inline uint32_t BodyAsset(SceneTables& t, int arena, int type, int lod = 0) {
  char pac[40], entry[16];
  std::snprintf(pac, sizeof pac, "pac\\bg\\bg%02d.pac", arena);
  std::snprintf(entry, sizeof entry, "%d", 20000 + 40 * lod + type);
  const uint32_t pf = t.AddString(pac), ef = t.AddString(entry);
  for (size_t i = 0; i < t.assets.size(); ++i)
    if (t.assets[i].kind == uint8_t(AssetKind::Model) && t.assets[i].pac == pf && t.assets[i].entry == ef && t.assets[i].variant == 0) return uint32_t(i);
  AssetRef a;
  a.kind = uint8_t(AssetKind::Model), a.pac = pf, a.entry = ef, a.variant = 0;
  t.assets.push_back(a);
  return uint32_t(t.assets.size() - 1);
}

// S103 SN (replaces S103 C1 review fix 4's equality rule): a single row's palette (the entity's node count) must COVER the chosen body: row_nodes >= slots, where slots = 1 + the highest palette slot any kept
// vertex of the body uses with a weight (the index PoseBody reads; 0 = unknown, refused). The old rule (row nodes == the body FILE's skeleton count) refused every stands-form palette: the stands draw the same bodies
// with a 17-entry palette, shorter than the file skeleton, with 0 bad bones (docs/research/s103/SN_singles.md A2). The applier (live_crowd.cpp) refuses a row that does not cover, counted (singles_node_mismatch);
// a palette whose count differs from the file skeleton but covers is drawn and only counted (singles_nodes_differ).
inline bool SingleNodesMatch(uint32_t body_slots, uint32_t row_nodes) { return body_slots != 0 && row_nodes >= body_slots; }

struct SingleIn {
  uint32_t asset = 0xFFFFFFFFu;   // BodyAsset(...)
  uint8_t type = 0;
  uint32_t flags = 0;             // CrowdSingleFlags
  float c[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  const Mat4* palette = nullptr;  // the person's skin matrices (file space, row vectors), one per skeleton node; null / 0 = the stored pose
  uint32_t nodes = 0;
};

inline Result AppendSingle(SceneTables& t, const SingleIn& in, Counts* counts = nullptr) {
  Result r = Result::Ok;
  if (in.asset >= t.assets.size()) r = Result::BadAsset;
  else if (!WorldUsable(in.c)) r = Result::BadWorld;
  else if (t.crowd_singles.size() >= kMaxSingles) r = Result::Full;
  else if (in.nodes > kMaxPalette || (in.nodes && !in.palette)) r = Result::BadPalette;
  else
    for (uint32_t i = 0; i < in.nodes && r == Result::Ok; ++i)
      for (int k = 0; k < 16; ++k)
        if (!std::isfinite(in.palette[i].m[k])) { r = Result::BadPalette; break; }
  if (r != Result::Ok) {
    if (counts) {
      ++counts->singles_refused;
      if (counts->first_refusal == Result::Ok) counts->first_refusal = r;
    }
    return r;
  }
  CrowdSingleRow row;
  row.asset = in.asset, row.type = in.type, row.flags = in.flags;
  std::memcpy(row.c, in.c, sizeof row.c);
  char id[32];
  std::snprintf(id, sizeof id, "crowd:single:%zu", t.crowd_singles.size());
  row.id = t.AddString(id);
  row.bones_first = uint32_t(t.bones.size()), row.bones_count = in.nodes;
  for (uint32_t i = 0; i < in.nodes; ++i) t.bones.push_back(in.palette[i]);
  t.crowd_singles.push_back(row);
  if (counts) ++counts->singles_ok;
  return Result::Ok;
}

}  // namespace rdr_scene::crowd_rows
