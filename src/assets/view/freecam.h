// Session 98 (plan step 11): the Arena Viewer's free camera. Same scheme as src\cinecam.h (our code): hold the right mouse button to look,
// W/S forward / back along the view, A/D strafe, E/Q world up / down, Shift x4, Ctrl x0.25 (speed and mouse look), wheel = FOV
// (x0.9 per notch, Ctrl x0.98, 2..120 degrees), Z/X roll, R roll 0; a pad drives the same (left stick move, right stick look,
// triggers up / down, bumpers zoom). Movement uses the host clock (the caller passes the seconds since the last tick) and eases
// (smoothing 0 = raw). No Win32 here: the platform fills FreeCamCtl's input fields, this header turns them into a Camera pose
// (camera.h's `free` mode). The phone port reuses it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>

#include "camera.h"

namespace view {

constexpr float kFreeFovMin = 2.0f, kFreeFovMax = 120.0f;
constexpr float kFreeSpeedDefault = 300.0f, kFreeSensDefault = 0.12f, kFreeSmoothDefault = 0.2f;

struct FreeKeys {  // held keys (the platform sets them each tick)
  bool fwd = false, back = false, left = false, right = false, up = false, down = false;
  bool fast = false, fine = false, roll_left = false, roll_right = false, roll_reset = false;
};

struct FreeCamCtl {
  // tuning (the FREECAM panel)
  float speed = kFreeSpeedDefault;       // world units per second
  float sensitivity = kFreeSensDefault;  // degrees per mouse pixel
  float smoothing = kFreeSmoothDefault;  // 0 = raw, 1 = floaty (time constants 0.3 s move, 0.1 s look, 0.15 s zoom)
  // input (the platform writes; Tick consumes)
  FreeKeys keys;
  bool looking = false;          // right mouse button held (the platform hides + locks the cursor)
  float mouse_dx = 0, mouse_dy = 0;  // mouse pixels since the last tick
  int wheel = 0;                 // wheel notches since the last tick (+ = zoom in)
  float pad_move_x = 0, pad_move_y = 0, pad_look_x = 0, pad_look_y = 0, pad_up = 0, pad_zoom = 0;  // -1..1 after the dead zone
  // state
  float vel[3] = {0, 0, 0};
  float roll_rate = 0, look_yaw = 0, look_pitch = 0;
  float fov_target = 40, last_fov = 40;
  float dt_avg = 1.0f / 60;
};

inline float FreeEase(float x, float target, float dt, float tau) {
  if (tau <= 0) return target;
  return x + (target - x) * (1.0f - std::exp(-dt / tau));
}
inline float WrapDeg(float a) {
  a = std::fmod(a + 180.0f, 360.0f);
  if (a < 0) a += 360.0f;
  return a - 180.0f;
}

// Orbit -> free keeps the view on screen: the eye stays, the free yaw / pitch are the orbit ones turned round (the orbit looks from the
// target back at it; forward = -offset). An explicit view-projection (a game pose) is dropped: the free camera starts at its eye.
// With an explicit view-projection (use_vp) the pose is read back from the matrix: forward = column 3 (clip.w = view z), right = column 0,
// up = column 1 (row-vector convention, camera.h), fov from the column 1 / column 3 length ratio.
inline void FreeFromOrbit(Camera& c, float pos[3], float& yaw, float& pitch, float* roll = nullptr, float* fov = nullptr) {
  V3 e = Eye(c);
  pos[0] = e.x, pos[1] = e.y, pos[2] = e.z;
  if (roll) *roll = 0;
  if (c.use_vp) {
    V3 fw = Norm({c.vp[3], c.vp[7], c.vp[11]}), r = Norm({c.vp[0], c.vp[4], c.vp[8]});
    const float l1 = std::sqrt(c.vp[1] * c.vp[1] + c.vp[5] * c.vp[5] + c.vp[9] * c.vp[9]);
    const float l3 = std::sqrt(c.vp[3] * c.vp[3] + c.vp[7] * c.vp[7] + c.vp[11] * c.vp[11]);
    if ((fw.x != 0 || fw.y != 0 || fw.z != 0) && std::isfinite(fw.x + fw.y + fw.z)) {
      yaw = std::atan2(fw.x, fw.z) / kDeg;
      pitch = std::clamp(std::asin(std::clamp(fw.y, -1.0f, 1.0f)) / kDeg, -89.5f, 89.5f);
      if (roll) {
        V3 r0 = FreeRight(yaw, pitch), u0 = Cross(fw, r0);
        *roll = std::atan2(-Dot(r, u0), Dot(r, r0)) / kDeg;
      }
      if (fov && l1 > 1e-12f && l3 > 0) *fov = std::clamp(2.0f * std::atan(l3 / l1) / kDeg, 2.0f, 120.0f);
      return;
    }
  }
  yaw = WrapDeg(c.yaw + 180.0f);
  pitch = std::clamp(-c.pitch, -89.5f, 89.5f);
}
inline void EnterFree(Camera& c, FreeCamCtl& f) {
  if (c.free && !c.use_vp) return;
  float p[3], y, pt, rl = 0, fv = c.fov;
  FreeFromOrbit(c, p, y, pt, &rl, &fv);
  c.use_vp = false;
  c.fov = fv;
  std::memcpy(c.fpos, p, sizeof p);
  c.fyaw = y, c.fpitch = pt, c.froll = rl;
  c.free = true;
  std::memset(f.vel, 0, sizeof f.vel);
  f.roll_rate = f.look_yaw = f.look_pitch = 0;
  f.fov_target = f.last_fov = c.fov;
}
// Free -> orbit: the target goes in front of the camera at the orbit distance, and the orbit angles look back along the view.
inline void LeaveFree(Camera& c, FreeCamCtl& f) {
  if (!c.free) return;
  V3 fw = FreeForward(c.fyaw, c.fpitch);
  c.dist = std::max(1.0f, c.dist);
  for (int i = 0; i < 3; i++) c.target[i] = c.fpos[i] + (&fw.x)[i] * c.dist;
  c.yaw = WrapDeg(c.fyaw + 180.0f);
  c.pitch = std::clamp(-c.fpitch, -89.5f, 89.5f);
  c.free = false;  // (f.looking is left to the platform: it ends the look, restoring the cursor)
  std::memset(f.vel, 0, sizeof f.vel);
  f.roll_rate = f.look_yaw = f.look_pitch = 0;
}
inline void ToggleFree(Camera& c, FreeCamCtl& f) {
  if (c.free) LeaveFree(c, f);
  else EnterFree(c, f);
}

// One frame. `dt_raw` = seconds since the last tick (host clock). Does nothing (but drains the accumulators) unless the camera is free.
inline void FreeTick(Camera& c, FreeCamCtl& f, float dt_raw) {
  const float mdx = f.mouse_dx, mdy = f.mouse_dy;
  const int wheel = f.wheel;
  f.mouse_dx = f.mouse_dy = 0;
  f.wheel = 0;
  if (!c.free) return;
  // Steps use the running average of the frame time (uneven steps read as judder); a gap over 0.1 s restarts it.
  if (dt_raw > 0.1f || dt_raw <= 0) f.dt_avg = 1.0f / 60;
  else f.dt_avg += (dt_raw - f.dt_avg) * 0.05f;
  const float dt = f.dt_avg;
  const float sm = std::clamp(f.smoothing, 0.0f, 1.0f);
  const FreeKeys& k = f.keys;
  const bool fine = k.fine;
  // Look: the mouse while the right button is held, the right stick at up to 90 degrees per second (fine: a quarter).
  const float look_scale = f.sensitivity * (fine ? 0.25f : 1.0f);
  if (f.looking) {
    f.look_yaw += mdx * look_scale;
    f.look_pitch -= mdy * look_scale;
  }
  f.look_yaw += f.pad_look_x * 90.0f * dt * (fine ? 0.25f : 1.0f);
  f.look_pitch += f.pad_look_y * 90.0f * dt * (fine ? 0.25f : 1.0f);
  {
    const float a = sm > 0 ? 1.0f - std::exp(-dt / (0.1f * sm)) : 1.0f;
    c.fyaw = WrapDeg(c.fyaw + f.look_yaw * a);
    c.fpitch = std::clamp(c.fpitch + f.look_pitch * a, -89.0f, 89.0f);
    f.look_yaw -= f.look_yaw * a;
    f.look_pitch -= f.look_pitch * a;
  }
  // Zoom: an outside change of the FOV (panel, cam_set) wins over the eased target.
  if (std::fabs(c.fov - f.last_fov) > 1e-4f) f.fov_target = c.fov;
  if (wheel) f.fov_target = std::clamp(f.fov_target * std::pow(fine ? 0.98f : 0.9f, float(wheel)), kFreeFovMin, kFreeFovMax);
  if (f.pad_zoom != 0) f.fov_target = std::clamp(f.fov_target * std::pow(0.5f, f.pad_zoom * dt), kFreeFovMin, kFreeFovMax);
  if (std::fabs(f.fov_target - c.fov) > 1e-4f) c.fov = std::clamp(FreeEase(c.fov, f.fov_target, dt, 0.15f * sm), kFreeFovMin, kFreeFovMax);
  f.last_fov = c.fov;
  // Move: W/S along the view, A/D level left / right, E/Q world up / down; one speed in every direction.
  float speed = f.speed;
  if (k.fast) speed *= 4;
  if (fine) speed *= 0.25f;
  const V3 fw = FreeForward(c.fyaw, c.fpitch), r = FreeRight(c.fyaw, c.fpitch), up = UpVector();
  const bool key_move = k.fwd || k.back || k.left || k.right || k.up || k.down;
  const float mf = float(k.fwd) - float(k.back) + f.pad_move_y;
  const float mr = float(k.right) - float(k.left) + f.pad_move_x;
  const float mu = float(k.up) - float(k.down) + f.pad_up;
  const float dir[3] = {mf * fw.x + mr * r.x + mu * up.x, mf * fw.y + mr * r.y + mu * up.y, mf * fw.z + mr * r.z + mu * up.z};
  const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  for (int i = 0; i < 3; ++i) {
    const float target = (len > 1.0f || key_move) && len > 0 ? dir[i] / len * speed : dir[i] * speed;  // (a part-way stick moves slower)
    f.vel[i] = FreeEase(f.vel[i], target, dt, 0.3f * sm);
    c.fpos[i] += f.vel[i] * dt;
  }
  // Roll: X right / Z left at 45 degrees per second (eased); R resets it.
  f.roll_rate = FreeEase(f.roll_rate, 45.0f * (float(k.roll_right) - float(k.roll_left)), dt, 0.3f * sm);
  c.froll = WrapDeg(c.froll + f.roll_rate * dt);
  if (k.roll_reset) c.froll = 0, f.roll_rate = 0;
}

}  // namespace view
