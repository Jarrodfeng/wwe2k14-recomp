// S110 FLASHOCC (docs\research\s110\FLASHOCC.md): the crowd camera flash sprite's world, the pure rule (header only: live_flashes.cpp and the offline unit tools\scene\cutgrey_unit.cpp run THESE functions).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game builds the sprite world from the INVERSE VIEW (flash_fx_core.h, the DRAW sub_823B27F0): the sprite faces the camera. Ours faces the frame's viewer eye. The sprite is built from the SceneFrame, never from
// a bare eye: the frame's camera is the viewer's (record_frame.cpp: the live record's eye has its y negated before any class applier runs), the record's own camera (SceneTables::camera) is the game's, with y
// NOT negated. Facing that one tilted every sprite toward an eye mirrored under the floor (FLASHDEPTH.md b8). A SceneFrame argument makes the game-space eye impossible to pass.
#pragma once
#include <cmath>
#include <cstring>

#include "front_end.h"

namespace rdr_scene {

// The world of a flash sprite at `pos` (viewer space) of size `scale`, facing `eye` (the same space): rows right, up, toward-eye, position (row vectors).
inline assets::Matrix FlashWorld(const float pos[3], float scale, const float eye[3]) {
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

// One flash sprite of `frame`: centred at `centre` (viewer space: the row's game position with y negated), `factor` x the model's own coordinates, facing the frame's viewer eye. The colour and alpha are the caller's.
inline FlashSprite FlashSpriteFacing(const SceneFrame& frame, const float centre[3], float factor, int model) {
  FlashSprite s;
  s.model = model;
  const assets::Matrix w = FlashWorld(centre, factor, frame.camera.eye);
  std::memcpy(s.world, w.data(), sizeof s.world);
  std::memcpy(s.centre, centre, sizeof s.centre);
  return s;
}

}  // namespace rdr_scene
