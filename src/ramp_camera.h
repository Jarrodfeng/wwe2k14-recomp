// wwe2k14 - S111 RAMPCAM (Jarrod 2026-11-26): the RAMP CAMERA, a mod, default off. The match camera stands on the side of the ring opposite
// the entrance stage and looks along the ramp at it; everything else is the game's own hard cam (height, pitch, FOV, framing, follow, limits).
// Evidence: docs\research\s111\RAMPCAM.md.
//
// --wwe2k14_ramp_camera = true | false   (default false; read live at every camera update: the pause menu's VISUAL PRESENTATION row
//                                         RAMP CAMERA sets it, lane RAMPMENU; a replay recording decides it, src\replay_carried.h)
//
// How. The game camera (vtable 0x8201D820) stands on one of four sides, field +1124, yaw = 0x83621694[side] = 90 / 180 / 0 / 270 deg. Side 0
// (the hard cam) is the eye on -X looking +X; every arena's stage and ramp lie on +Z, so side 2 (yaw 0) is the ramp camera. The field is HELD
// at 2 where the game reads it next:
//   - at the start of each camera update (src\cinecam.h's hook of sub_8257D8E8 calls BeforeUpdate): the update's side step (sub_8257AE98
//     +0x84..+0xF8) sees the yaw differ and re-poses with sub_8257ADD8, the game's own side change;
//   - around the two game functions that READ the side before writing the next one: sub_8257A1F8 (a motion camera's activation: the next side
//     on a noise-table coin) and sub_8257BD88 (the director / ring-area tracker hand-back re-pick, forced 0 outside HIAC / Chamber): the
//     game's own side is lent back for the call and remembered after it, so its side sequence and the shared noise counter 0x839456D4 run as
//     they would without us, and switching off puts the game's own side back.
// Only with a ring in the arena (sub_824E7668; the game applies a side only there). A Royal Rumble is held too (Jarrod 2026-11-26 21:5x): the
// game's side step skips types 20-23 (sub_822BA5A8), so there Apply makes the side change itself, with the game's own call and rule
// (sub_8257ADD8 when the yaw differs from the side's, +420 and +1864 clear: ramp_camera_core.h Decide). The ring-area tracker's outside view
// is the game's own (the side step, and the Rumble pose, wait while +1864 is set). The camera's ctor (sub_8257A3F8) ends a hold of an earlier
// object at its address.
// The pad needs nothing: the game turns the stick by the shown camera's heading ([0x836F3AA8]+1436 from the key-0 camera's view,
// sub_823C7FB0; sub_82610680 / sub_82610738 in the pad update sub_82611618; the raw-stick readers add it too, e.g. sub_8276B7E8 +0x178..+0x1B0).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "guest_call.h"        // scene_director::Caller
#include "ramp_camera_core.h"  // SideHold, Hold, GiveBack, Forget, Decide (pure)

REXCVAR_DECLARE(bool, wwe2k14_ramp_camera);

REX_EXTERN(__imp__sub_8257A1F8);  // a motion camera's activation: the game camera's next side (r3 = the camera)
REX_EXTERN(__imp__sub_8257BD88);  // the hand-back re-pick (r3 = the camera, r4 = 0 at all three callers: pose now and run the update)
REX_EXTERN(__imp__sub_8257A3F8);  // the game camera's ctor (r3 = the object; also the base of the story / backstage / CAMS subclasses)
REX_EXTERN(sub_824E7668);         // the arena record's byte 24: the arena has a ring
REX_EXTERN(sub_822BA5A8);         // the match type byte is 20..23 (Royal Rumble)
REX_EXTERN(sub_8257ADD8);         // the game's side change pose: r3 = the camera, f1 = the yaw (sets +416 / +1748, the offsets, +1136 = 1)

namespace wwe2k14::ramp_camera {

inline std::recursive_mutex g_mutex;  // the update runs nested inside sub_8257BD88 (its slot +4 call), on the same thread
inline SideHold g_hold;

inline bool Plausible(uint32_t a) { return a >= 0x10000u && a < 0xC0000000u; }
inline uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t be;
  std::memcpy(&be, base + address, 4);
  return std::byteswap(be);
}
inline float LoadF32(const uint8_t* base, uint32_t address) { return std::bit_cast<float>(LoadU32(base, address)); }
inline void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t be = std::byteswap(value);
  std::memcpy(base + address, &be, 4);
}
inline bool IsGameCamera(const uint8_t* base, uint32_t cam) { return Plausible(cam) && LoadU32(base, cam) == kGameCameraVtable; }

// The side field gets the ramp side (or, switching off, the game's own side back); in a Royal Rumble the side change is posed too. Called
// where the game reads the side next. Caller saves and restores every register around each guest call.
inline void Apply(PPCContext& ctx, uint8_t* base, uint32_t cam) {
  if (!IsGameCamera(base, cam)) return;
  const bool on = REXCVAR_GET(wwe2k14_ramp_camera);
  std::lock_guard lock(g_mutex);
  if (!on && !g_hold.held) return;  // the default path: nothing read, no guest call, nothing written
  scene_director::Caller call(ctx, base);
  const bool ring = call(sub_824E7668, {cam}) != 0;
  const bool rumble = call(sub_822BA5A8, {cam}) != 0;
  CameraRead c;
  c.field = LoadU32(base, cam + kSideOff);
  c.yaw = LoadF32(base, cam + kYawOff);
  c.no_side = LoadU32(base, cam + kNoSideOff);
  c.outside = LoadU32(base, cam + kOutsideOff);
  float table[4];
  for (uint32_t i = 0; i < 4; ++i) table[i] = LoadF32(base, kSideYawTable + 4 * i);
  const Applied a = Decide(g_hold, cam, c, on, ring, rumble, table);
  if (a.side != c.field) StoreU32(base, cam + kSideOff, a.side);
  if (a.pose) call.withf(sub_8257ADD8, {cam}, double(a.pose_yaw));  // the game's own side change pose (a Rumble's side step is skipped)
  if (a.started || a.ended)
    REXLOG_INFO("[wwe2k14][ramp_camera] {} (camera {:08X}: side {} -> {}; the game's own side {}{})",
                a.started ? "the match camera stands on the ramp side" : "the match camera is the game's own again", cam, c.field, a.side,
                g_hold.game_side, a.pose ? "; Royal Rumble: posed by sub_8257ADD8" : "");
}

// Before a game function that reads the side and writes the next one: the game's own side for the call.
inline void Lend(uint8_t* base, uint32_t cam) {
  if (!IsGameCamera(base, cam)) return;
  std::lock_guard lock(g_mutex);
  const uint32_t field = LoadU32(base, cam + kSideOff);
  const uint32_t side = GiveBack(g_hold, cam, field);
  if (side != field) StoreU32(base, cam + kSideOff, side);
}

// src\cinecam.h's hook of the camera update sub_8257D8E8 calls this before the game's update.
inline void BeforeUpdate(PPCContext& ctx, uint8_t* base, uint32_t cam) { Apply(ctx, base, cam); }

}  // namespace wwe2k14::ramp_camera

REX_HOOK_RAW(sub_8257A1F8) {
  using namespace wwe2k14::ramp_camera;
  const uint32_t cam = ctx.r3.u32;
  Lend(base, cam);
  __imp__sub_8257A1F8(ctx, base);
  Apply(ctx, base, cam);
}

REX_HOOK_RAW(sub_8257BD88) {
  using namespace wwe2k14::ramp_camera;
  const uint32_t cam = ctx.r3.u32;
  Lend(base, cam);
  __imp__sub_8257BD88(ctx, base);
  Apply(ctx, base, cam);
}

// A camera object's life starts here: a hold left on an earlier object at this address is over (ramp_camera_core.h Forget).
REX_HOOK_RAW(sub_8257A3F8) {
  using namespace wwe2k14::ramp_camera;
  const uint32_t cam = ctx.r3.u32;
  __imp__sub_8257A3F8(ctx, base);
  std::lock_guard lock(g_mutex);
  Forget(g_hold, cam);
}
