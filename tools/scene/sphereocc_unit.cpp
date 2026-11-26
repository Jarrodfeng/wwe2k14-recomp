// S111 SPHEREOCC unit: VS c168-c171 (SphereOccP / SphereOccV) of a wrestler draw = the game's own sphere occluders of that body (the record's newest row of its
// skeleton, sphere_join.h; game: sub_826DFCC8 -> sub_826DF8E0 -> sub_82BCC888, uploaded by sub_82BE7AC0), re-fed when they change (draw_lists.cpp kStPalette,
// the spheres hashed into the palette stamp as front_end.cpp does). 0 game runs.
//   out\tools\sphereocc_unit.exe   -> PASS / FAIL, exit 0 / 1 (tools\scene\build_sphereocc_unit.ps1 builds and runs it)
// Planted twins run inside and must FAIL: the old resolver input (no block: strength 0), a stale sphere fed although the game did not install it, and the old
// written-once class (mask 0).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/draw_lists.h"
#include "../../src/renderer_scene/sphere_join.h"

using namespace rdr_scene;

namespace {
int g_bad = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_bad;
}

constexpr uint32_t kSkelA = 0xA0001000u, kSkelB = 0xB0002000u, kSkelC = 0xC0003000u;

SphereOccRow Row(uint32_t skel, uint32_t frame, uint32_t ran, float x, float s0, float s1) {
  SphereOccRow r;
  r.flags = kSoSpheres | kSoTable | kSoSkeleton | ran;
  r.object = skel, r.frame = frame, r.count = 2, r.component = skel + 0x100u, r.character = skel + 0x200u, r.calls = 1;
  const float p0[4] = {x, -100.f, 5.f, 12.f}, p1[4] = {x + 30.f, -100.f, 5.f, 10.f};
  const float v0[4] = {0.f, 0.f, 1.f, s0}, v1[4] = {0.f, 0.f, -1.f, s1};
  std::memcpy(r.p[0], p0, 16), std::memcpy(r.p[1], p1, 16), std::memcpy(r.v[0], v0, 16), std::memcpy(r.v[1], v1, 16);
  return r;
}

// fx_skin.glsl sphere_occlusion, one sphere (centre.w = radius, axis.w = strength)
float Occlusion(const float pos[3], const float* centre, const float* axis) {
  const float away[3] = {pos[0] - centre[0], pos[1] - centre[1], pos[2] - centre[2]};
  const float dist = std::sqrt(away[0] * away[0] + away[1] * away[1] + away[2] * away[2]);
  const float al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (al == 0.f || dist == 0.f) return 0.f;
  const float along = std::clamp((axis[0] * away[0] + axis[1] * away[1] + axis[2] * away[2]) / (al * dist), 0.f, 1.f);
  return axis[3] * along * (1.f - std::min(dist / centre[3], 1.f));
}
float BlockOcclusion(const float c[16], const float pos[3]) { return Occlusion(pos, c, c + 8) + Occlusion(pos, c + 4, c + 12); }

// c168-c171 of one draw, fed `block` as front_end.cpp feeds it (cc.sphere_occ = sphere_join::Of(...)); null = the twin: no block.
void Resolve(const float* block, float out[16]) {
  assets::Material m;
  assets::LightSet ls;
  CharCtx c;
  c.mat = &m, c.lights = &ls;
  c.sphere_occ = block;
  std::memset(out, 0, 64);
  if (!ResolveChar("SphereOccP", 2, c, out) || !ResolveChar("SphereOccV", 2, c, out + 8)) std::printf("FAIL  ResolveChar does not know SphereOccP / SphereOccV\n"), ++g_bad;
}

// Two frames of one kept vertex block (SphereOccP at 0, SphereOccV at 32) through the real Diff / Refresh, the palette unchanged, the spheres moved: the palette
// stamp folds the spheres as front_end.cpp does. True = the second frame's block equals the second frame's spheres.
bool KeptRefeeds(uint32_t mask, const float* f1, const float* f2) {
  DrawLists L(1);
  KeptDraw d;
  d.vs.Begin(64);
  d.vs.fields.push_back(KeptField{"SphereOccP", 0, 2, mask});
  d.vs.fields.push_back(KeptField{"SphereOccV", 32, 2, mask});
  const uint64_t palette = 0x1234u;
  const float* frames[2] = {f1, f2};
  for (int i = 0; i < 2; ++i) {
    Stamps st;
    st.h[1] = HashBytes(frames[i], sizeof(float) * 16, palette);
    const uint32_t changed = DrawLists::Diff(d, st, i == 0);
    L.Refresh(d.vs, changed, [&](const KeptField& f, float* out) {
      assets::Material m;
      assets::LightSet ls;
      CharCtx c;
      c.mat = &m, c.lights = &ls, c.sphere_occ = frames[i];
      ResolveChar(f.name, f.vec4s, c, out);
    });
  }
  float got[16];
  std::memcpy(got, d.vs.bytes.data(), 64);
  return std::memcmp(got, f2, 64) == 0;
}
}  // namespace

int main() {
  // The record: A was updated twice (the newest row wins: sphere 0 close, strength 0.9); B's update installed sphere 0 only (sphere 1's nodes are missing: its
  // row values are the component's stale ones, 7.7); C has no 'CCSO' component (no row).
  std::vector<SphereOccRow> rows;
  rows.push_back(Row(kSkelA, 500, kSoSphere0Ran | kSoSphere1Ran, 10.f, 0.2f, 0.f));
  rows.push_back(Row(kSkelB, 501, kSoSphere0Ran, 50.f, 0.6f, 7.7f));
  rows.push_back(Row(kSkelA, 501, kSoSphere0Ran | kSoSphere1Ran, 12.f, 0.9f, 0.f));
  std::vector<sphere_join::Body> bodies;
  Check(sphere_join::CarryBody(rows, bodies, 0, kSkelA), "body 0 (skeleton A) carries its row");
  Check(sphere_join::CarryBody(rows, bodies, 1, kSkelB), "body 1 (skeleton B) carries its row");
  Check(!sphere_join::CarryBody(rows, bodies, 2, kSkelC) && !sphere_join::CarryBody(rows, bodies, 3, 0), "a body with no row (C, engine 0) carries nothing");
  Check(bodies.size() == 2 && sphere_join::Of(bodies, 2) == nullptr, "two blocks; body 2 has none");
  // Review (S111): the join is counted, so the one check run can tell "rows recorded but no body joined" (a key mismatch: the old strength 0, silently) from a working join.
  Check(sphere_join::Joins().joined.load() == 2 && sphere_join::Joins().no_row.load() == 2, "join census: 2 bodies joined, 2 without a row");
  Check(sphere_join::Joins().Text() == "bodies joined 2, without a row 2", "join census text (appended to the [spheres ...] census line)");

  float a[16], b[16], cblk[16];
  Resolve(sphere_join::Of(bodies, 0), a);
  Resolve(sphere_join::Of(bodies, 1), b);
  Resolve(sphere_join::Of(bodies, 2), cblk);
  const SphereOccRow& ra = rows[2];
  Check(std::memcmp(a, ra.p[0], 16) == 0 && std::memcmp(a + 4, ra.p[1], 16) == 0 && std::memcmp(a + 8, ra.v[0], 16) == 0 && std::memcmp(a + 12, ra.v[1], 16) == 0,
        "A: c168 = P0, c169 = P1, c170 = V0, c171 = V1 of the NEWEST row (frame 501)");
  Check(a[11] == 0.9f, "A: c170.w = the close sphere's strength 0.9");
  const SphereOccRow& rb = rows[1];
  Check(std::memcmp(b, rb.p[0], 16) == 0 && std::memcmp(b + 8, rb.v[0], 16) == 0, "B: sphere 0 as recorded");
  Check(std::memcmp(b + 4, sphere_join::kNeutral + 4, 16) == 0 && std::memcmp(b + 12, sphere_join::kNeutral + 12, 16) == 0, "B: sphere 1 (not installed by the game) = the neutral sphere, strength 0");
  Check(std::memcmp(cblk, sphere_join::kNeutral, 64) == 0, "C: no row = the neutral block");

  // The darkening: a vertex 6 units along A's sphere-0 axis (radius 12) is occluded 0.9 x 1 x 0.5 = 0.45; B's skipped sphere darkens nothing.
  const float near_a[3] = {ra.p[0][0], ra.p[0][1], ra.p[0][2] + 6.f};
  const float occ_a = BlockOcclusion(a, near_a);
  Check(std::fabs(occ_a - 0.45f) < 1e-5f, "A: the shader's occlusion at the vertex = 0.45");
  const float near_b1[3] = {rb.p[1][0], rb.p[1][1], rb.p[1][2] - 5.f};
  Check(BlockOcclusion(b, near_b1) == 0.f, "B: no darkening from the sphere the game did not install");

  // Twin 1: the old feed (no block: c170.w = 0) must lose the darkening.
  float twin[16];
  Resolve(nullptr, twin);
  const bool twin_unfed = BlockOcclusion(twin, near_a) != occ_a;
  // Twin 2: the stale sphere fed although the game did not install it.
  float stale[16];
  sphere_join::Compose(&rb, stale, true);
  const bool twin_stale = std::memcmp(stale + 12, sphere_join::kNeutral + 12, 16) != 0;
  // The kept lists: the spheres move, the palette does not.
  const uint32_t mask = FieldMask("SphereOccP");
  Check(mask == uint32_t(kStPalette) && FieldMask("SphereOccV") == uint32_t(kStPalette), "draw_lists.cpp: SphereOccP / SphereOccV are kStPalette (re-fed when the palette stamp moves)");
  float a_old[16];   // A's block one update earlier (frame 500)
  sphere_join::Compose(&rows[0], a_old);
  Check(KeptRefeeds(mask, a_old, a), "kept block: a sphere change alone re-feeds c168-c171");
  const bool twin_class = !KeptRefeeds(0u, a_old, a);
  std::printf("%s  planted twin (no block: the old strength 0): %s\n", twin_unfed ? "ok  " : "FAIL", twin_unfed ? "caught" : "NOT caught");
  std::printf("%s  planted twin (a sphere the game skipped fed stale): %s\n", twin_stale ? "ok  " : "FAIL", twin_stale ? "caught" : "NOT caught");
  std::printf("%s  planted twin (SphereOcc written once, mask 0): %s\n", twin_class ? "ok  " : "FAIL", twin_class ? "caught" : "NOT caught");
  if (!twin_unfed) ++g_bad;
  if (!twin_stale) ++g_bad;
  if (!twin_class) ++g_bad;
  std::printf("%s sphereocc_unit: c168-c171 = the body's own recorded sphere occluders\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}
