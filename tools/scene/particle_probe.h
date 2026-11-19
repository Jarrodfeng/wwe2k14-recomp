// S103 lane F: the particle probe of the offline rig (frame_rig --particles-probe). A synthetic particle record (three crossed quads at the game-space point that
// projects to the middle of the screen) drawn through rf::BuildFrame's frame (viewer_space = true) by LiveParticles: the picture with the particles must differ from the
// one without, and the changed pixels must centre on the middle of the picture. A hook that skipped the viewer-space mirror puts the quads at the Y-mirrored point: the
// centroid moves away (--particles-probe-fault no_mirror: the probe must report FAIL). Header only; included by frame_rig.cpp.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_particles.h"
#include "../../src/renderer_scene/record_frame.h"

namespace rdr_scene {

inline bool Invert4(const double m[16], double inv[16]) {  // Gauss-Jordan, row-major
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-12) return false;
    for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
    const double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) inv[r * 4 + c] = a[r][4 + c];
  return true;
}

// Returns true when the probe passes. `report` gets the lines.
inline bool ParticleProbe(rdr::Renderer& renderer, FrontEnd& fe, rf::ModelCache& cache, const SceneTables& t, uint32_t mask, uint32_t w, uint32_t h, bool no_mirror, std::string& report) {
  std::string err;
  SceneFrame f;
  std::vector<rf::ClassReport> reports;
  if (!rf::BuildFrame(t, cache, mask, w, h, f, reports, err)) { report += "particle_probe FAIL: BuildFrame: " + err + "\n"; return false; }
  std::vector<uint8_t> base, with;
  fe.ResetCoverage();
  if (!fe.RenderToPixels(f, base, err)) { report += "particle_probe FAIL: baseline draw: " + err + "\n"; return false; }
  // The game-space point whose viewer-space clip position is (0, 0, 0.5): P_v = clip * inverse(VP'), then the viewer -> game mirror S = diag(1, -1, 1).
  double m[16], inv[16];
  for (int i = 0; i < 16; ++i) m[i] = f.camera.view.m[i];
  if (!Invert4(m, inv)) { report += "particle_probe FAIL: the camera matrix is singular\n"; return false; }
  const double c[4] = {0, 0, 0.5, 1};
  double pv[4];
  for (int j = 0; j < 4; ++j) pv[j] = c[0] * inv[0 * 4 + j] + c[1] * inv[1 * 4 + j] + c[2] * inv[2 * 4 + j] + c[3] * inv[3 * 4 + j];
  const float px = float(pv[0] / pv[3]), py = float(-pv[1] / pv[3]), pz = float(pv[2] / pv[3]);
  // three crossed quads of half-size 1.5 game units, opaque magenta, at P (a camera cannot be edge-on to all three)
  ParticleTables pt;
  ParticleBatch b;
  b.first = 0, b.count = 3, b.frame = 4242;
  pt.batches.push_back(b);
  const float s = 1.5f;
  const float axes[3][2] = {{0, 1}, {0, 2}, {1, 2}};  // the two world axes each quad spans
  const float cs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int q = 0; q < 3; ++q) {
    Particle p{};
    const float pos[3] = {px, py, pz};
    for (int r = 0; r < 3; ++r) p.local_world[4 * r + 3] = pos[r];  // translation
    // world_r = row_r . (x, y, z, 1): the quad's local x spans world axis a, local y spans world axis b
    p.local_world[4 * int(axes[q][0]) + 0] = s;
    p.local_world[4 * int(axes[q][1]) + 1] = s;
    for (int i = 0; i < 4; ++i) {
      p.v[i].pos[0] = cs[i][0], p.v[i].pos[1] = cs[i][1], p.v[i].pos[2] = 0;
      p.v[i].color = 0xFFFF00FFu;  // R 255, G 0, B 255, A 255
      p.v[i].uv[0] = p.v[i].uv[1] = 0.5f;
    }
    pt.particles.push_back(p);
  }
  ParticleFaults pf;
  pf.no_mirror = no_mirror;
  LiveParticles lp(renderer, nullptr, pf);
  lp.SetFrame(4242);
  f.particles = &pt;
  f.stage_hooks.push_back(&lp);
  fe.ResetCoverage();
  if (!fe.RenderToPixels(f, with, err)) { report += "particle_probe FAIL: draw with particles: " + err + "\n"; return false; }
  uint64_t changed = 0;
  double sx = 0, sy = 0;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const size_t i = (size_t(y) * w + x) * 4;
      if (std::abs(int(with[i]) - int(base[i])) + std::abs(int(with[i + 1]) - int(base[i + 1])) + std::abs(int(with[i + 2]) - int(base[i + 2])) > 12) ++changed, sx += x, sy += y;
    }
  const double cx = changed ? sx / double(changed) / w : -1, cy = changed ? sy / double(changed) / h : -1;
  const bool ok = changed >= 40 && std::fabs(cx - 0.5) < 0.08 && std::fabs(cy - 0.5) < 0.08;
  char line[320];
  std::snprintf(line, sizeof line, "particle_probe %s: point (%.2f, %.2f, %.2f), changed pixels %llu, centroid (%.3f, %.3f) of the picture (must be within 0.08 of 0.5), mirrored frames %llu, %s\n",
                ok ? "PASS" : "FAIL", px, py, pz, (unsigned long long)changed, cx, cy, (unsigned long long)lp.stats().viewer_space_mirrored, lp.CensusLine().c_str());
  report += line;
  return ok;
}

}  // namespace rdr_scene
