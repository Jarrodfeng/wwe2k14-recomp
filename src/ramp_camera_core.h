#pragma once
// Pure inline: no hook, no cvar, no static. S111 RAMPCAM (docs\research\s111\RAMPCAM.md): the side hold of the ramp camera.
// src\ramp_camera.h is the glue; tools\camera\ramp_camera_test.cpp is the unit.
#include <cstdint>

namespace wwe2k14::ramp_camera {

inline constexpr uint32_t kGameCameraVtable = 0x8201D820u;  // the match game camera (ctor sub_8257A3F8, update sub_8257D8E8)
inline constexpr uint32_t kSideOff = 1124;                   // camera +1124: the side 0..3; yaw = 0x83621694[side] = 90 / 180 / 0 / 270 deg
inline constexpr uint32_t kRampSide = 2;                     // yaw 0: the eye on -Z looking +Z, along the ramp at the stage (every arena's
                                                             // entry-50003 stage and ramp lie on +Z); side 0, the hard cam, is the eye on -X
inline constexpr uint32_t kYawOff = 416;                     // camera +416: the yaw the side step compares with the side's (sub_8257AE98 +0x70)
inline constexpr uint32_t kNoSideOff = 420;                  // camera +420: non-zero = the game's side step is off (sub_8257AE98 +0x60..+0x68)
inline constexpr uint32_t kOutsideOff = 1864;                // camera +1864: the ring-area tracker's outside view (the side step waits)
inline constexpr uint32_t kSideYawTable = 0x83621694u;       // f32[4]: the side's yaw (read by sub_8257AE98 +0x2C, sub_8257BD88 +0x16C)

struct SideHold {
  bool held = false;       // the field holds kRampSide for us
  uint32_t cam = 0;        // the camera object it is held for
  uint32_t game_side = 0;  // the side the game itself would show now
  uint32_t last = 0;       // what the field held when we last touched it
};

// Where the game reads the side next (the camera update's start, after a game writer): the value the field must hold. want = the switch is
// on and the game applies a side in this match. A field that differs from what we left is the game's own new side.
inline uint32_t Hold(SideHold& s, uint32_t cam, uint32_t field, bool want) {
  if (s.held && cam != s.cam) s.held = false;  // another camera object (a new match): its field is the game's
  if (!want) {
    if (!s.held) return field;
    s.held = false;
    return field == s.last ? s.game_side : field;  // the game's own side back
  }
  if (!s.held || field != s.last) s.game_side = field;
  s.held = true;
  s.cam = cam;
  s.last = kRampSide;
  return kRampSide;
}

// Before a game function that reads the side and writes the next one (sub_8257A1F8, sub_8257BD88): the game's own side for the call, so its
// sequence runs as it would without us. Hold after the call records what it wrote.
inline uint32_t GiveBack(SideHold& s, uint32_t cam, uint32_t field) {
  if (!s.held || cam != s.cam) return field;
  if (field != s.last) s.game_side = field;
  s.last = s.game_side;
  return s.game_side;
}

// A camera object's life starts at `cam` (the ctor sub_8257A3F8 ran there): a hold of an earlier object at the same address is over, so
// the new object's own side is never taken for one we left (review: the next match's camera can get the old one's heap block).
inline void Forget(SideHold& s, uint32_t cam) {
  if (s.cam == cam) s = SideHold{};
}

// The hold's gate: the switch and a ring (the game applies a side only with a ring, sub_8257AE98 +0x30..+0x50). A Royal Rumble is held too
// (Jarrod 2026-11-26 21:5x: "a match people [often] requested"); the game's side step skips it (sub_822BA5A8, types 20-23), so Decide poses it.
inline bool Wants(bool on, bool ring) { return on && ring; }

// A Royal Rumble's side change, made by the game's own rule for one (sub_8257AE98 +0x60..+0xE4): when +420 and the outside flag +1864 are
// clear and the yaw +416 differs from the side's table yaw, sub_8257ADD8(cam, table[side]) re-poses the camera. So it fires on the frame the
// hold starts or ends and after a game pose at another yaw (a hand-back's re-pick poses at table[0], sub_8257BD88 +0x13C..+0x1D4), never on a
// frame whose yaw already is the side's. Exact float compare, as the game's fcmpu.
inline bool RumblePose(float yaw, float side_yaw, uint32_t no_side, uint32_t outside) {
  return no_side == 0 && outside == 0 && yaw != side_yaw;
}

struct CameraRead {
  uint32_t field = 0;    // +1124
  float yaw = 0.0f;      // +416
  uint32_t no_side = 0;  // +420
  uint32_t outside = 0;  // +1864
};

struct Applied {
  uint32_t side = 0;       // what +1124 must hold now
  bool pose = false;       // a Royal Rumble: sub_8257ADD8(cam, pose_yaw)
  float pose_yaw = 0.0f;
  bool started = false;    // the hold began on this camera (the log line)
  bool ended = false;      // the hold ended on this camera
};

// One Apply (src\ramp_camera.h) from what it read: on = the switch, ring = sub_824E7668, rumble = sub_822BA5A8, table = 0x83621694.
inline Applied Decide(SideHold& s, uint32_t cam, const CameraRead& c, bool on, bool ring, bool rumble, const float table[4]) {
  const bool was = s.held && s.cam == cam;
  Applied a;
  a.side = Hold(s, cam, c.field, Wants(on, ring));
  a.started = !was && s.held;
  a.ended = was && !s.held;
  if (rumble && (s.held || a.ended)) {
    const float side_yaw = table[a.side & 3];
    if (RumblePose(c.yaw, side_yaw, c.no_side, c.outside)) {
      a.pose = true;
      a.pose_yaw = side_yaw;
    }
  }
  return a;
}

}  // namespace wwe2k14::ramp_camera
