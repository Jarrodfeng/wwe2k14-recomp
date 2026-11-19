// Lane C (Session 103): see live_flashes.h. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "live_flashes.h"

#include <cmath>
#include <cstring>

namespace rdr_scene::rf::c {

assets::Matrix FlashWorld(const float pos[3], float scale, const float eye[3]) {
  float f[3] = {eye[0] - pos[0], eye[1] - pos[1], eye[2] - pos[2]};  // toward the eye
  float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (!(len > 1e-6f)) f[0] = 0, f[1] = 0, f[2] = 1, len = 1.f;
  for (float& v : f) v /= len;
  float up[3] = {0.f, 1.f, 0.f};
  if (std::fabs(f[1]) > 0.999f) up[1] = 0.f, up[2] = 1.f;
  float r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};  // up x f
  const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (float& v : r) v /= rl;
  const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};  // f x r
  assets::Matrix m = assets::Identity();
  for (int i = 0; i < 3; ++i) m[size_t(i)] = r[i] * scale, m[size_t(4 + i)] = u[i] * scale, m[size_t(8 + i)] = f[i] * scale, m[size_t(12 + i)] = pos[i];
  return m;
}

// S103 C3: the uniform factor that multiplies the sprite model's own coordinates. A live row (kFxfGameScale) carries the game's multiplier (item +40, flash_fx_core.h) and is used as is;
// an older / synthetic row carries a world half-size, divided by the model's extent.
float FlashFactor(const StageFxRow& r, float model_radius) {
  if (r.flags & kFxfGameScale) return r.scale;
  return model_radius > 0.f ? r.scale / model_radius : r.scale;
}

bool ApplyFlashes(BuildCtx& ctx) {
  if (!(ctx.families & (1u << uint32_t(Family::Flashes)))) return true;
  uint32_t rows = 0;
  for (const StageFxRow& r : ctx.t.stage_fx) rows += r.kind == kFxFlash;
  if (!rows) {
    Report(ctx, Family::Flashes, 0, 0, 0, "");
    return true;
  }
  if (PlantsRef().ignore_flashes) {
    Report(ctx, Family::Flashes, rows, 0, rows, "flashes ignored (planted)");
    return true;
  }
  std::string why;
  bool miss = false;
  ArenaSet* as = EnsureArena(ctx, why, miss);
  if (miss) return false;
  if (!as || as->part[size_t(Part::Flash)].model < 0) {
    Report(ctx, Family::Flashes, rows, 0, rows, as ? "arena " + std::to_string(as->arena) + " has no flash model (key 31000)" : why);
    return true;
  }
  const FamilyModel& fm = as->part[size_t(Part::Flash)];
  uint32_t drawn = 0;
  for (const StageFxRow& r : ctx.t.stage_fx) {
    if (r.kind != kFxFlash) continue;
    float p[3];
    PointToViewer(r.pos, p);
    const assets::Matrix w = FlashWorld(p, FlashFactor(r, fm.radius), ctx.t.camera.eye);
    Object o;
    o.asset = uint32_t(fm.model);
    std::memcpy(o.world.m, w.data(), sizeof o.world.m);
    o.tint[3] = r.alpha;  // c185.x of the draw (crowd_flash.md [C]); the colour table is not fed [U]
    ctx.frame.objects.push_back(o);
    ++drawn;
  }
  ctx.frame.model_lights.push_back(LightsFor(ctx.t, fm, uint32_t(fm.model)));
  Report(ctx, Family::Flashes, rows, drawn, rows - drawn, "");
  return true;
}

}  // namespace rdr_scene::rf::c
