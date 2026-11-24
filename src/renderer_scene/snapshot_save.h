// S108 lane SHOT-A (docs\research\s108\SHOT_plan.md 7): the pure steps of the subject-shot saver, so they are proven offline (tools\scene\snapshot_unit.cpp) and the scene host only wires them.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; no game, SDK or renderer includes.
//   BodyScaleRows      the scale the host drew each wrestler with (section 68)
//   AddLiquidImage     one map's persistent image, mip 0 RGBA8, into sections 69 / 70
//   FillGuestBytes     every guest-model row that names bytes an earlier record carried (kGmNoBytes) gets them from the host's byte store, so the file is whole on its own
// Hard rule 1: what these write is data built from the game's files: local only, never committed.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "body_scale.h"
#include "record_io.h"
#include "rows_snapshot.h"

namespace rdr_scene::snapshot {

// One row per wrestler object (id "actor:slot:N") whose scale the host knows. A wrestler with no scale (a hole in the host) gets NO row, as the host did not draw it; `holes` counts them.
inline void BodyScaleRows(const SceneTables& t, std::vector<BodyScaleRow>& out, uint32_t& holes) {
  out.clear(), holes = 0;
  for (size_t i = 0; i < t.objects.size(); ++i) {
    const std::string_view id = t.Str(t.objects[i].id);
    if (id.rfind("actor:slot:", 0) != 0) continue;
    const uint32_t addr = i < t.engine_objects.size() ? t.engine_objects[i] : 0u;
    if (!addr) { ++holes; continue; }
    const std::string sid(id);
    bool conflict = false;
    const float s = BodyScaleFor(true, addr, false, false, &sid, &conflict);
    if (!(s > 0.f)) { ++holes; continue; }
    BodyScaleRow r;
    r.object = addr, r.scale = s;
    out.push_back(r);
  }
}

// Appends map `map`'s image (w x h RGBA8, tight) to the three tables. False = the bytes do not match the size (nothing added).
inline bool AddLiquidImage(SceneTables& t, uint32_t map, uint32_t object, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
  if (!w || !h || rgba.size() != size_t(w) * h * 4) return false;
  LiquidImageRow r;
  r.map = map, r.object = object, r.width = w, r.height = h, r.nbytes = uint32_t(rgba.size()), r.offset = t.liquid_image_bytes.size();
  t.liquid_image_bytes.insert(t.liquid_image_bytes.end(), rgba.begin(), rgba.end());
  t.liquid_images.push_back(r);
  return true;
}

struct FillCounts { uint32_t filled = 0, missing = 0; };

// `get(hash)` -> shared_ptr<const std::vector<uint8_t>> (null = the store has none). Bytes are appended to the blob section 8-byte aligned and the row's offset set. A hash the store does not hold stays
// kGmNoBytes (a hole the rig reports, never a guess) and is counted in `missing`.
template <class Lookup>
FillCounts FillGuestBytes(SceneTables& t, Lookup get) {
  FillCounts c;
  GuestModelTables& g = t.guest_models;
  const auto put = [&](uint64_t hash, size_t need, uint32_t& off) {
    if (off != kGmNoBytes) return;
    auto b = get(hash);
    if (!b || b->size() < need || need == 0) { ++c.missing; return; }
    while (g.blobs.size() & 7) g.blobs.push_back(0);
    if (g.blobs.size() + need >= 0xFFFFFFF0u) { ++c.missing; return; }
    off = uint32_t(g.blobs.size());
    g.blobs.insert(g.blobs.end(), b->begin(), b->begin() + need);
    ++c.filled;
  };
  for (GuestModelPart& p : g.parts) {
    put(p.vtx_hash, size_t(p.vertex_count) * p.stride, p.vtx_off);
    put(p.idx_hash, size_t(p.index_count) * 2, p.idx_off);
  }
  for (GuestModelTexture& x : g.textures) put(x.hash, x.nbytes, x.off);
  return c;
}

}  // namespace rdr_scene::snapshot
