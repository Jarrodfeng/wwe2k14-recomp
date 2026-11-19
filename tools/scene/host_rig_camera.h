// S103 P7 H1: SynthCamera, copied unchanged from tools/scene/frame_rig.cpp (the rig-only look-at camera for a record that carries none). Rig use only.
#pragma once
#include <cmath>
#include "../../src/renderer_scene/record_io.h"
namespace host_rig_cam {
using namespace rdr_scene;
// --synth-camera (lane G's, moved from rsc_draw): a capture's record carries no camera yet, so a plain look-at at the objects' centroid exercises the draw path. Made in VIEWER space like the
// old tool, then handed back in the record's own space (a Live record's camera is the game's: BuildFrame converts it, so the inverse of that conversion is applied here).
static inline void SynthCamera(SceneTables& t) {
  const bool live = t.header.source == uint8_t(Source::Live);
  double cx = 0, cy = 0, cz = 0;
  size_t n = 0;
  // objects at the origin (the arena slots, the wrestlers before their pose) carry no position: the centroid and the spread come from the placed ones; the view distance scales with the
  // spread (the game's units: a crowd sits thousands of units out)
  const auto placed = [](const Object& o) { return o.world.m[12] != 0 || o.world.m[13] != 0 || o.world.m[14] != 0; };
  for (const Object& o : t.objects)
    if (placed(o)) cx += o.world.m[12], cy += (live ? -1.0 : 1.0) * o.world.m[13], cz += o.world.m[14], ++n;
  if (n) cx /= double(n), cy /= double(n), cz /= double(n);
  double spread = 0;
  for (const Object& o : t.objects)
    if (placed(o)) {
      const double dx = o.world.m[12] - cx, dy = (live ? -1.0 : 1.0) * o.world.m[13] - cy, dz = o.world.m[14] - cz;
      spread += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
  spread = n ? spread / double(n) : 25.0;
  if (spread < 25.0) spread = 25.0;
  const double eye[3] = {cx, cy + 0.35 * spread, cz - 1.6 * spread}, up[3] = {0, 1, 0};
  double z[3] = {cx - eye[0], cy - eye[1], cz - eye[2]};
  const auto norm = [](double* v) { const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int i = 0; i < 3; ++i) v[i] /= l; };
  const auto cross = [](const double* a, const double* b, double* o) { o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0]; };
  double x[3], y[3];
  norm(z), cross(up, z, x), norm(x), cross(z, x, y);
  const double V[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
                        -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]), -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1};
  const double ys = 1.0 / std::tan(0.5 * 40.0 * 3.14159265358979 / 180.0), xs = ys * 9.0 / 16.0, zn = 0.02 * spread, zf = 8.0 * spread;
  const double P[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += V[i * 4 + k] * P[k * 4 + j];
      t.camera.view.m[i * 4 + j] = float(s);
      t.camera.proj.m[i * 4 + j] = i == j ? 1.f : 0.f;
    }
  for (int i = 0; i < 3; ++i) t.camera.eye[i] = float(eye[i]);
  if (live) {  // BuildFrame converts a Live record's camera (view row 1 and the eye's y negate): hand it the unconverted one
    for (int k = 4; k < 8; ++k) t.camera.view.m[k] = -t.camera.view.m[k];
    t.camera.eye[1] = -t.camera.eye[1];
  }
  t.camera.flags = kCamViewIsViewProj;
  t.has_camera = true;
}

}  // namespace host_rig_cam
