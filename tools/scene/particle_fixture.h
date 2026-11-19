// S103 F5: a labelled particle FIXTURE for the offline rig (frame_rig, WWE_RDR_PARTICLE_FIXTURE). The cases we have (runs 26-29: gameplay windows) hold no particle table (the live reader never
// paired a frame before F5) and the stream of those windows holds no particle draw, so there is no game picture of a flare to compare with. The fixture puts ONE camera-facing flare into
// the record's particle tables, in the form the live reader writes (a batch + a LocalWorld + four vertices), so the whole of OUR path (tables -> texture lookup by name -> blend -> depth ->
// colour pass) runs on a real frame of a real record. It is a fixture, never a live path: the report line says so.
//   WWE_RDR_PARTICLE_FIXTURE      the sprite entry ("p0107/2/1/Light0001"); "none" = an UNNAMED texture (the white stand-in: the twin)
//   WWE_RDR_PARTICLE_FIXTURE_BLEND  additive (default; code 1) | alpha (code 0): the twin for a wrong blend
//   WWE_RDR_PARTICLE_FIXTURE_SIZE   fraction of the picture height the flare spans (default 0.05)
// Header only; included by frame_rig.cpp.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/particle_tables.h"
#include "../../src/renderer_scene/record_io.h"
#include "particle_probe.h"   // Invert4

namespace rdr_scene {

inline bool InstallParticleFixture(SceneTables& t, const SceneFrame& f, std::string& report) {
  const char* env = std::getenv("WWE_RDR_PARTICLE_FIXTURE");
  if (!env || !*env) return false;
  const std::string entry = env;
  const char* be = std::getenv("WWE_RDR_PARTICLE_FIXTURE_BLEND");
  const std::string bs = be ? be : "additive";
  const bool additive = bs != "alpha" && bs != "subtract";   // "subtract" = code 2 (S103 F5 review: it must DARKEN)
  const char* se = std::getenv("WWE_RDR_PARTICLE_FIXTURE_SIZE");
  const float frac = se ? float(std::atof(se)) : 0.05f;
  double m[16], inv[16];
  for (int i = 0; i < 16; ++i) m[i] = f.camera.view.m[i];
  if (!Invert4(m, inv)) { report += "particle_fixture: the camera matrix is singular, no fixture\n"; return false; }
  // the viewer-space point at clip (0, 0, 0.5) -> game space (mirror y), as particle_probe.h
  const double c[4] = {0, 0, 0.5, 1};
  double pv[4];
  for (int j = 0; j < 4; ++j) pv[j] = c[0] * inv[0 * 4 + j] + c[1] * inv[1 * 4 + j] + c[2] * inv[2 * 4 + j] + c[3] * inv[3 * 4 + j];
  const float P[3] = {float(pv[0] / pv[3]), float(-pv[1] / pv[3]), float(pv[2] / pv[3])};
  // the camera position (viewer space = row 3 of the inverse) -> game space
  const float E[3] = {float(inv[12] / inv[15]), float(-inv[13] / inv[15]), float(inv[14] / inv[15])};
  const float dist = std::sqrt((P[0] - E[0]) * (P[0] - E[0]) + (P[1] - E[1]) * (P[1] - E[1]) + (P[2] - E[2]) * (P[2] - E[2]));
  // The rig's camera is the BORROWED ViewProj (kCamViewIsViewProj): its 3x3 is not the camera's axes. So the flare's axes are made from the eye -> point direction (game space: y DOWN, so up = -y),
  // and its size from the projection itself: half-size = frac / (NDC length of one world unit along `right` at P), i.e. the flare spans `frac` of the picture height.
  float fwd[3] = {(P[0] - E[0]) / dist, (P[1] - E[1]) / dist, (P[2] - E[2]) / dist};
  const float u0[3] = {0, -1, 0};
  float right[3] = {u0[1] * fwd[2] - u0[2] * fwd[1], u0[2] * fwd[0] - u0[0] * fwd[2], u0[0] * fwd[1] - u0[1] * fwd[0]};
  const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (int r = 0; r < 3; ++r) right[r] /= rl;
  float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
  const auto ndc = [&](const float g[3], double o[2]) {   // game point -> NDC xy (viewer space: y mirrored; row vector x matrix)
    const double x = g[0], y = -g[1], z = g[2];
    double c[4];
    for (int j = 0; j < 4; ++j) c[j] = x * m[0 * 4 + j] + y * m[1 * 4 + j] + z * m[2 * 4 + j] + m[3 * 4 + j];
    o[0] = c[0] / c[3], o[1] = c[1] / c[3];
  };
  double n0[2], n1[2];
  ndc(P, n0);
  const float Pr[3] = {P[0] + right[0], P[1] + right[1], P[2] + right[2]};
  ndc(Pr, n1);
  const double per_unit = std::sqrt((n1[0] - n0[0]) * (n1[0] - n0[0]) + (n1[1] - n0[1]) * (n1[1] - n0[1]));
  const float half = per_unit > 1e-9 ? float(frac / per_unit) : 1.0f;
  Particle p{};
  for (int r = 0; r < 3; ++r) p.local_world[4 * r + 0] = right[r] * half, p.local_world[4 * r + 1] = up[r] * half, p.local_world[4 * r + 2] = fwd[r], p.local_world[4 * r + 3] = P[r];
  const float cs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int i = 0; i < 4; ++i) {
    p.v[i].pos[0] = cs[i][0], p.v[i].pos[1] = cs[i][1], p.v[i].pos[2] = 0;
    p.v[i].color = 0xFFFFFFFFu;   // white, alpha 255: the texture shapes the flare
    p.v[i].uv[0] = cs[i][0] > 0 ? 1.f : 0.f, p.v[i].uv[1] = cs[i][1] > 0 ? 1.f : 0.f;
    p.v[i].uv2[0] = p.v[i].uv[0], p.v[i].uv2[1] = p.v[i].uv[1];
  }
  t.particles.Clear();
  ParticleBatch b;
  b.first = 0, b.count = 1, b.frame = uint32_t(t.header.guest_frame);
  b.blend = bs == "subtract" ? kParticleReverseSubtract : additive ? kParticleAdditive : kParticleAlpha;
  if (entry != "none") {
    AssetRef a;
    a.kind = uint8_t(AssetKind::Texture);
    a.pac = t.AddString("pac\\prtcls.pac"), a.entry = t.AddString(entry);
    t.assets.push_back(a);
    b.texture = uint32_t(t.assets.size() - 1);
  }
  t.particles.batches.push_back(b);
  t.particles.particles.push_back(p);
  char line[400];
  std::snprintf(line, sizeof line, "particle_fixture (a FIXTURE: one flare put into the record by the rig, not a live read): entry \"%s\", blend %s, at game (%.1f, %.1f, %.1f), half-size %.2f at distance %.1f\n", entry.c_str(), additive ? "additive" : "alpha", P[0], P[1], P[2], half, dist);
  report += line;
  return true;
}

}  // namespace rdr_scene
