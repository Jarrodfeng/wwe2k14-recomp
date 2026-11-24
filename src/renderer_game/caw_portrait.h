// S103 CPB (docs\research\s103\CPR.md section 6): the Create a Superstar roster portrait, stream-free (OFF) build only. Included by d3d_textures.h under
// `#if !WWE2K14_STREAM()`. The decisions are in caw_portrait_core.h (unit-tested std-only); this file is the guest-memory glue.
//
// G1  sub_82BA86B8 (installs a camera as the thread's current one), only at lr 0x8341C314 = the portrait renderer's call (sub_8341BF58): reads the game's portrait camera and the model object and posts ONE request to the
//     scene host (portrait_mailbox.h), which draws that wrestler offscreen at 736x1280 with that camera (SceneHost::ServePortrait -> LiveSceneDraw::DrawPortrait).
// G2  sub_8341C600 (the portrait renderer's tick): at state 4 (queue the lock / untile job) it returns without running while the host's picture has not landed (up to kWaitTicks), then runs the original.
// G3  xg_untile_to_buffer sub_82CFA318, only the call at lr 0x8341B780 (the job's untile): our picture, or transparent zeros, is written into the job's destination buffer and the original does not run (its source is
//     memory nothing wrote: garbage that the game then encodes and saves into the player's .cas). Every other caller of sub_82CFA318 runs the original unchanged.
// --wwe2k14_rdr_caw_portrait=false: G1 / G2 do nothing, G3 writes the zeros only (never the old garbage).
#pragma once

#include "../keep_stream.h"

#if !WWE2K14_STREAM()

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../frame_stats.h"   // g_guest_frame
#include "../renderer_host/portrait_mailbox.h"
#include "../scene_live/inputs_core.h"   // ReadMat, IsRigidView, IsPerspective, kCamView / kCamProj / kCamNear / kCamFar
#include "../scene_live/safe_guest_pages.h"
#include "../scene_live/wrestler_hook.h"   // GuestRangeReadable, ReadGuest, ReadWord
#include "caw_portrait_core.h"

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_caw_portrait, true, "wwe2k14",
                    "S103 CPB: the stream-free build draws the Create a Superstar roster portrait itself (the edited wrestler, the game's portrait camera, 736x1280, transparent background) and hands it to the game's "
                    "own crop / encode / save. Off = a transparent portrait (zeros): never the old garbage the game used to read from memory nothing wrote.");

REX_EXTERN(__imp__sub_82CFA318);
REX_EXTERN(__imp__sub_82BA86B8);
REX_EXTERN(__imp__sub_8341C600);

namespace wwe2k14::caw_portrait {

inline constexpr uint32_t kSingleton = 0x839C03D0;       // the portrait renderer singleton pointer [C CPR section 0]
inline constexpr uint32_t kCameraInstallLr = 0x8341C314; // return address of `bl sub_82BA86B8` at 0x8341C310 in sub_8341BF58 [C CPR section 1]
inline constexpr uint32_t kStateOff = 48, kTextureOff = 64;

inline Census g_census;
inline std::atomic<uint64_t> g_active_id{0};   // the request the current portrait render posted (0 = none)
inline TickGate g_gate;

// The destination as a host pointer when every byte of [guest, guest + n) is inside one readable view (the checks of wrestlers::ReadGuest), else null.
inline uint8_t* GuestRangePtr(uint32_t guest, uint64_t n) {
  if (guest < 0x10000 || uint64_t(guest) + n > 0x100000000ull) return nullptr;
  if (uint64_t(guest) + n > 0x7F000000ull && guest < 0x80000000u) return nullptr;   // MMIO heap
  if (wwe2k14::safe_pages::CrossesE0View(guest, n)) return nullptr;
  auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(guest);
  return host && wwe2k14::wrestlers::GuestRangeReadable(guest, size_t(n)) ? host : nullptr;
}

inline void PrintCensus(const char* what) {
  REXLOG_INFO("[rdr][caw_portrait] {}: requests {} served {} unserved {} late {} failed {} refused {} landed_frame {}", what, g_census.requests, g_census.served, g_census.unserved, g_census.late, g_census.failed,
              g_census.refused, (unsigned long long)g_census.landed_frame);
}

// The game's camera object (view +240, projection +176, near / far +500 / +504: the layout scene_inputs::ReadCamera reads, inputs_core.h:596), for the object at `obj`.
inline bool ReadCameraObject(uint32_t obj, rdr_scene::Camera& out) {
  namespace si = scene_inputs;
  rdr_scene::Camera c;
  if (!si::ReadMat(&wwe2k14::wrestlers::ReadGuestNoNote, obj + si::kCamView, c.view) || !si::ReadMat(&wwe2k14::wrestlers::ReadGuestNoNote, obj + si::kCamProj, c.proj)) return false;   // S110 FEEDREC: the un-noted leaf (si::ReadFn notes)
  if (!si::IsRigidView(c.view.m) || !si::IsPerspective(c.proj.m)) return false;
  uint32_t nw = 0, fw = 0;
  if (si::ReadWord(&wwe2k14::wrestlers::ReadGuestNoNote, obj + si::kCamNear, nw) && si::ReadWord(&wwe2k14::wrestlers::ReadGuestNoNote, obj + si::kCamFar, fw)) {
    std::memcpy(&c.near_z, &nw, 4);
    std::memcpy(&c.far_z, &fw, 4);
  }
  if (!(c.near_z > 0.f) || !(c.far_z > c.near_z) || !std::isfinite(c.far_z)) c.near_z = 1.f, c.far_z = 65536.f;   // the portrait projection's own constants (CPR section 2) when vt[56] has not set them
  const float* v = c.view.m;
  for (int i = 0; i < 3; ++i) c.eye[i] = -(v[12] * v[i * 4 + 0] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
  c.fov_y_deg = 0, c.flags = 0;
  out = c;
  return true;
}

// G1: true when a request was posted.
inline bool OnCameraInstall(uint32_t r30_this, uint32_t r31_model, uint32_t r4_camera) {
  if (!REXCVAR_GET(wwe2k14_rdr_caw_portrait)) return false;
  uint32_t single = 0, state = 0, tex = 0;
  namespace w = wwe2k14::wrestlers;
  if (!w::ReadWord(kSingleton, single) || single != r30_this || !w::ReadWord(r30_this + kStateOff, state) || state != 2 || !w::ReadWord(r30_this + kTextureOff, tex) || !tex || !r31_model) {
    ++g_census.refused;
    return false;
  }
  wwe2k14::scene_host::PortraitRequest req;
  if (!ReadCameraObject(r4_camera, req.camera)) {
    ++g_census.refused;
    return false;
  }
  req.character = r31_model;   // CPB2: r31 is the character object; the record keys on its skeleton [char+48] (caw_portrait_core.h ModelKey)
  req.model = ModelKey(r31_model, [](uint32_t a, uint32_t& v) { return w::ReadWord(a, v); });
  if (!req.model) {
    ++g_census.refused;
    return false;
  }
  req.guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  g_active_id.store(wwe2k14::scene_host::g_portrait.Post(std::move(req)), std::memory_order_release);
  ++g_census.requests;
  return true;
}

// G2: true when this tick is held (the original must not run).
inline bool HoldTick(uint32_t r3_this) {
  const uint64_t id = g_active_id.load(std::memory_order_acquire);
  if (!id || !REXCVAR_GET(wwe2k14_rdr_caw_portrait)) return false;
  uint32_t single = 0, state = 0;
  namespace w = wwe2k14::wrestlers;
  if (!w::ReadWord(kSingleton, single) || single != r3_this || !w::ReadWord(r3_this + kStateOff, state)) return false;
  const bool pending = wwe2k14::scene_host::g_portrait.Poll(id, nullptr) == wwe2k14::scene_host::PortraitStatus::Pending;
  return g_gate.Step(state, pending, kWaitTicks, g_census) == TickAct::Hold;
}

// G3: true when the call was answered here (the original must not run).
inline bool OnUntile(const UntileCall& c) {
  auto rd32 = [](uint32_t a, uint32_t& out) { return wwe2k14::wrestlers::ReadWord(a, out); };
  if (!IsPortraitUntile(c, rd32)) {
    ++g_census.refused;   // the filter refused the portrait's own return address: the original runs (garbage, as before)
    PrintCensus("untile refused");
    return false;
  }
  uint8_t* dst = GuestRangePtr(c.dest, DestBytes(c.pitch));
  if (!dst) {
    ++g_census.refused;
    PrintCensus("destination unreadable");
    return false;
  }
  wwe2k14::scene_host::PortraitResult res;
  const uint64_t id = g_active_id.exchange(0, std::memory_order_acq_rel);
  wwe2k14::scene_host::PortraitStatus status = wwe2k14::scene_host::PortraitStatus::None;
  if (id) status = wwe2k14::scene_host::g_portrait.Poll(id, &res);
  if (status == wwe2k14::scene_host::PortraitStatus::Pending) {   // G2 timed out: the host stops drawing for a game that has moved on (a later answer would be dropped anyway)
    wwe2k14::scene_host::g_portrait.Cancel(id);   // (G2 already counted the timeout as `late`)
  }
  const bool have = REXCVAR_GET(wwe2k14_rdr_caw_portrait) && status == wwe2k14::scene_host::PortraitStatus::Ready && res.width && res.height && res.rgba.size() >= size_t(res.width) * res.height * 4;
  if (have) {
    WritePicture(dst, c.pitch, res.rgba.data(), res.width, res.height);
    ++g_census.served;
    g_census.landed_frame = res.landed_frame;
    PrintCensus("untile served");
  } else {
    WriteZero(dst, c.pitch);   // no picture: transparent, never garbage
    ++g_census.unserved;
    if (id) ++g_census.failed;
    PrintCensus("untile unserved");
  }
  return true;
}

}  // namespace wwe2k14::caw_portrait

REX_HOOK_RAW(sub_82BA86B8) {
  if (uint32_t(ctx.lr) == wwe2k14::caw_portrait::kCameraInstallLr) wwe2k14::caw_portrait::OnCameraInstall(ctx.r30.u32, ctx.r31.u32, ctx.r4.u32);
  __imp__sub_82BA86B8(ctx, base);
}

REX_HOOK_RAW(sub_8341C600) {
  if (wwe2k14::caw_portrait::HoldTick(ctx.r3.u32)) return;
  __imp__sub_8341C600(ctx, base);
}

REX_HOOK_RAW(sub_82CFA318) {
  if (uint32_t(ctx.lr) == wwe2k14::caw_portrait::kUntileLr) {
    const wwe2k14::caw_portrait::UntileCall c{uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
    if (wwe2k14::caw_portrait::OnUntile(c)) return;
  }
  __imp__sub_82CFA318(ctx, base);
}

#endif  // !WWE2K14_STREAM()
