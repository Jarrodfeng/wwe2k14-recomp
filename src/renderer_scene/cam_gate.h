// S105 A3: the game's "shows a scene camera" facts, carried in SceneHeader::flags (append-only: bits 1-4; an old record has all 0 = not read = drawn as before).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes.
//
// Why (docs\research\s105\A3.md, [C] stream-build captures + the second-bit run; review_A3.md): at the start of the FIRST entrance after a load the game draws NO scene while
//   (1) its shown camera is 0: [[0x836F3AA8] + 1392] == 0 and neither override camera ([mgr + 1412], [[0x83767B74] + 340]) is set, or
//   (2) the entrance load window runs: phase [0x836F3940] == 4, sequence [[[0x839400F8]] + 48] == 9 and the motion camera cur = [0x836F3ABC] is not yet animated ([cur + 1440] == 0).
// The stream-free host drew the arena there because READY was its only gate. The latch (scene_live\inputs_contrib.h Take / Emit) reads the words and stamps the facts into the record; the host
// (renderer_host\host_core.h CamGate / Decide, scene_host.cpp Handle) puts black under the 2D rows on a frame that carries them. Bit (1) applies only before the first 3D picture of a scene
// (host: !State::seen3d: the evidenced load window; the same word reads 0 between entrances and at a match opening, where the game still draws). Bit (4) lets the host hold the black after
// bit (1) ends while the motion camera is still not animated (a bounded hold, kCamHoldMax), which closes the gap between the shown camera and the phase word.
//
// Layout: kCamGateRead = every word of the shown test was readable (the latch thread, a scene manager); kCamGateShown = the game shows a camera; kCamGateUnanimated = the load window (2);
// kCamGateAnimPending = [seq == 9, cur valid, anim == 0], no phase test (the sticky hold's condition). A record that did not read a word never blackens for it (no manager: the CAW editor;
// an unreadable word; the fallback take, which runs with the game threads running; an older record).
#pragma once
#include <cstdint>

namespace rdr_scene {

inline constexpr uint32_t kCamGateRead = 2u;           // SceneHeader::flags bit 1 (bit 0 = kSceneStagePlaying, scene_record.h)
inline constexpr uint32_t kCamGateShown = 4u;          // bit 2: a camera is shown at the latch
inline constexpr uint32_t kCamGateUnanimated = 8u;     // bit 3: the load window (UnanimatedWindow)
inline constexpr uint32_t kCamGateAnimPending = 16u;   // bit 4: sequence 9, a valid motion camera, not yet animated (AnimPending)
inline constexpr uint32_t kCamGateMask = kCamGateRead | kCamGateShown | kCamGateUnanimated | kCamGateAnimPending;

#ifdef WWE2K14_HOST_PLANTS
inline int g_cam_gate_plant = 0;   // test only (tools\renderer\host_core_unit.cpp): 1 = the load-window test looks at the phase word only; 2 = the shown test ignores the override cameras
#define CAM_GATE_PLANT(n) (::rdr_scene::g_cam_gate_plant == (n))
#else
#define CAM_GATE_PLANT(n) false
#endif

// The writer's bits for one latch.
inline constexpr uint32_t CamGateBits(bool read, bool shown, bool unanimated = false, bool anim_pending = false) {
  return (read ? (kCamGateRead | (shown ? kCamGateShown : 0u)) : 0u) | (unanimated ? kCamGateUnanimated : 0u) | (anim_pending ? kCamGateAnimPending : 0u);
}
// The host's questions.
inline constexpr bool NoShownCamera(uint32_t header_flags) { return (header_flags & kCamGateRead) != 0 && (header_flags & kCamGateShown) == 0; }
inline constexpr bool CamUnanimated(uint32_t header_flags) { return (header_flags & kCamGateUnanimated) != 0; }
inline constexpr bool CamAnimPending(uint32_t header_flags) { return (header_flags & kCamGateAnimPending) != 0; }

// A guest pointer worth following (src\step_rate.h:255-257: 0x40000000 .. 0xE0000000).
inline constexpr bool CamPtrValid(uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; }

// The shown test over a word reader rw(guest address, uint32_t& value) -> bool (false = unreadable). read = every word read (the manager exists and is a valid pointer); shown = the shown camera word, or
// the composer's override camera word [mgr + 1412], or [[0x83767B74] + 340] is non-zero (a null override object counts as no override). Any unreadable word: read = false (never black on a failed read).
template <class RW>
inline void ReadShown(RW rw, bool& read, bool& shown) {
  read = shown = false;
  uint32_t mgr = 0, w = 0, ov = 0, obj = 0, ow = 0;
  if (!rw(0x836F3AA8u, mgr) || !CamPtrValid(mgr) || !rw(mgr + 1392u, w) || !rw(mgr + 1412u, ov) || !rw(0x83767B74u, obj)) return;
  if (obj != 0) {
    if (!CamPtrValid(obj) || !rw(obj + 340u, ow)) return;
  }
  read = true;
  shown = w != 0 || (!CAM_GATE_PLANT(2) && (ov != 0 || ow != 0));
}

// Sequence 9, a valid motion camera, not yet animated (no phase test).
template <class RW>
inline bool AnimPending(RW rw) {
  uint32_t p1 = 0, p2 = 0, seq = 0, cur = 0, anim = 0;
  if (!rw(0x839400F8u, p1) || !CamPtrValid(p1) || !rw(p1, p2) || !CamPtrValid(p2) || !rw(p2 + 48u, seq) || seq != 9) return false;
  if (!rw(0x836F3ABCu, cur) || !CamPtrValid(cur) || !rw(cur + 1440u, anim)) return false;
  return anim == 0;
}
// The load window: phase 4 and AnimPending. phase_ok = the phase word was read.
template <class RW>
inline bool UnanimatedWindow(RW rw, bool phase_ok, uint32_t phase) {
  if (!phase_ok || phase != 4) return false;
  if (CAM_GATE_PLANT(1)) return true;   // plant 1: the phase alone
  return AnimPending(rw);
}

}  // namespace rdr_scene
