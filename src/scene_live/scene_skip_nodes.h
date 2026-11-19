// The scene skips: the hooks this lane adds (Session 103, lane H; docs\renderer\SCENE_SKIPS.md). COMPILED, NOT RUN (0 game runs in this lane).
//
// Each function below had NO hook before (grep of src\ and config\, lint in tools\renderer\scene_skip_unit.py: exactly one REX_HOOK_RAW per function). Every hook
// asks scene_skip::Ours(hook) FIRST; false = the original, byte for byte. True = the side effects SCENE_SKIPS.md lists for that row, then return without the original.
// The existing hooks (draw_origin.h sub_82C04DF0, own_node_hook.h, crowd_hooks.h, bake_skip_hook.h) carry the same test as their first line, marked `// S103 H:`.
// Function addresses and listings: SCENE_SKIPS.md section 3 (each row names the listing it was read from). Included from draw_origin.h after own_node_hook.h.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <atomic>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include "scene_skip.h"
#include "plane_proxy.h"   // S103 A3 (S8)
#include "ring_recompute_contrib.h"   // S103 C3
#include "particle_glue.h"  // S103 F2
#include "quads2d_tap.h"   // S103 MP: RowsThisFrame
#include "liquid_glue.h"   // S103 SWF: the liquid map's items at the sweat node's entry

REX_EXTERN(__imp__sub_82C13A80);
REX_EXTERN(__imp__sub_825D0438);
REX_EXTERN(__imp__sub_825D01F0);
REX_EXTERN(__imp__sub_827EF070);
REX_EXTERN(__imp__sub_827EBE78);
REX_EXTERN(__imp__sub_8279ACD8);
REX_EXTERN(__imp__sub_82254E30);
REX_EXTERN(__imp__sub_82C1E840);
REX_EXTERN(__imp__sub_82BF8118);
REX_EXTERN(__imp__sub_82C1F728);   // S103 T
REX_EXTERN(sub_82A0B7C0);   // the retire scan of one 432-byte particle entry (0 = the entry is retired)
REX_EXTERN(sub_82BA7748);   // the thread slot's current camera (r3 = the table object); 0 = none
REX_EXTERN(sub_82BA77A8);   // the thread's render-pass index word (0 = the colour pass): the crowd entry's own test
REX_EXTERN(sub_827D73C8);   // S103 D3b: returns the pointer to the word the classifier request test reads (0x825185E0)
REX_EXTERN(sub_825133F0);   // S103 D3b: the crowd LOD classifier (serves [manager+252])
REX_EXTERN(sub_8250C7C8);  // the crowd manager's callback registration (the crowd entry calls it before its own manager test)

namespace wwe2k14::scene_skip_nodes {

using ::scene_skip::Hook;

inline uint32_t Rd(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
inline void Wr(uint8_t* base, uint32_t a, uint32_t value) {
  const uint32_t v = __builtin_bswap32(value);
  std::memcpy(rex::memory::GuestPtr(base, a), &v, 4);
}

inline constexpr uint32_t kCameraTablePtr = 0x83929A68u;   // 0x83930000 - 26008 (lis -31853 / lwz -26008 in every node's own camera test)
inline constexpr uint32_t kParticleEntrySize = 432;

// S103 RLB: the camera the ring's rope LOD classifier reads: sub_82BA7748 (r3 = [0x83929A68], the game's own call, ctx restored), eye = camera +592 xyz, forward = camera +608 xyz, big-endian floats
// (sub_827405F8: addi r4,r3,592 / 608; RLV.md rows 1, 2, 8, 9). A zero camera word leaves view.has false = every rope HIGH (0x82740624).
inline void ReadRingView(PPCContext& ctx, uint8_t* base, rdr_scene::live::ringrecompute::RopeView& view) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = Rd(base, kCameraTablePtr);
  sub_82BA7748(ctx, base);
  const uint32_t cam = ctx.r3.u32;
  ctx = saved;
  if (cam == 0) return;
  for (uint32_t i = 0; i < 3; ++i) {
    view.eye[i] = std::bit_cast<float>(Rd(base, cam + 592 + 4 * i));
    view.fwd[i] = std::bit_cast<float>(Rd(base, cam + 608 + 4 * i));
  }
  view.has = true;
}

// The particle node's Ours branch: the retire scan alone (listing 0x822550BC-0x82255124, the loop minus the index test and the draw). The walk's three early outs come first,
// in the game's order, and the original runs when one holds (it then does nothing, exactly as today). Returns true when the node was handled here.
inline bool ParticlesRetireOnly(PPCContext& ctx, uint8_t* base, uint32_t node) {
  if (Rd(base, node + 3084) == 0 || Rd(base, node + 3088) == 0) return false;   // 0x82254E3C-0x82254E54
  {
    const PPCContext saved = ctx;
    ctx.r3.u64 = Rd(base, kCameraTablePtr);
    sub_82BA7748(ctx, base);
    const bool no_camera = ctx.r3.u32 == 0;   // 0x82254E64-0x82254E6C
    ctx = saved;
    if (no_camera) return false;
  }
  const PPCContext saved = ctx;
  uint8_t* state = static_cast<uint8_t*>(rex::memory::GuestPtr(base, node + 20));
  uint32_t i = 0;   // a do-while like the game's (0x822550BC-0x82255124): the count is re-read every turn; the first entry needs no count test (the early out proved it non-zero)
  do {
    if (state[i] == 2) {
      ctx.r3.u64 = Rd(base, node + 16) + kParticleEntrySize * i;
      sub_82A0B7C0(ctx, base);
      if (ctx.r3.u32 == 0) state[i] = 0;   // 0x82255108-0x82255110: a 0 return stores r28 = 0 (li r28,0 at 0x82254EC0)
    }
    ++i;
  } while (i < Rd(base, node + 3088));
  ctx = saved;
  return true;
}

// The crowd entry's own early outs (listing 0x82519514-0x82519570), the manager's callback registration it makes before drawing (sub_8250C7C8: the CPU skin / palette update the
// manager schedules is reached through it), nothing else: no constants, no draw, no card loop. Returns true when handled; false = the entry would have done nothing, the original runs.
inline constexpr uint32_t kCrowdGateObject = 0x83767B74u;   // lis -31882 / lwz 31604 at 0x82519514: when non-zero its word +368 non-zero ends the entry
inline constexpr uint32_t kCrowdManagerGlobal = 0x836F4DC0u;   // lis -31889 / lwz 19904 at 0x82519554
inline bool CrowdKeepRegistration(PPCContext& ctx, uint8_t* base, bool* classify_done = nullptr) {
  const uint32_t node = ctx.r3.u32;
  const uint32_t gate = Rd(base, kCrowdGateObject);
  if (gate && Rd(base, gate + 368)) return false;   // 0x82519520-0x82519530
  const PPCContext saved = ctx;
  ctx.r3.u64 = Rd(base, kCameraTablePtr);
  sub_82BA77A8(ctx, base);
  const bool pass_other = ctx.r3.u32 != 0;   // 0x82519538-0x82519544
  ctx = saved;
  if (pass_other) return false;
  if (Rd(base, node + 556) == 0) return false;   // 0x82519548-0x82519550
  const uint32_t manager = Rd(base, kCrowdManagerGlobal);
  ctx.r3.u64 = manager;
  sub_8250C7C8(ctx, base);   // 0x82519560: registers the manager's callbacks (this+32 as context); the original does it before its own manager test
  ctx = saved;
  // S103 D3b (review F6): the LOD classifier request. sub_825185B0 (skipped on an OURS frame) serves it first thing (0x825185E0-0x82518608): if [sub_827D73C8()] == 0 and [manager+252] != 0 it calls
  // sub_825133F0(r3 = the entry's r3 - 32, the argument sub_825185B0 gets) and stores 0 at [manager+252]. Skipped, a request posted during OURS frames would never be served (the latch reader declines on it).
  // Only under the entry's own gates (review of round 2, item 1): [mgr+560] != 0 (0x82519564-6C) and one of [mgr+476/480/484] != 0 (0x82519760-88), else sub_825185B0 never runs and no request is served.
  const bool stands_pass = manager && Rd(base, manager + 560) != 0 && (Rd(base, manager + 476) != 0 || Rd(base, manager + 480) != 0 || Rd(base, manager + 484) != 0);
  if (classify_done) *classify_done = stands_pass;   // the "serve step" is done (served, or the game's own flag word skips it): the latch reader must not decline on a still-set [mgr+252] (item 4)
  if (stands_pass && Rd(base, manager + 252) != 0) {
    ctx.r3.u64 = 0;
    sub_827D73C8(ctx, base);
    const uint32_t flag_addr = ctx.r3.u32;
    ctx = saved;
    if (flag_addr && Rd(base, flag_addr) == 0) {
      ctx.r3.u64 = node - 32;
      sub_825133F0(ctx, base);
      ctx = saved;
      Wr(base, manager + 252, 0);
    }
  }
  return true;
}

// The plane node's Ours branch: both paths of the original zero the module's per-frame proxy list (32 words at module + 1176) and its count (module + 1304); the update task
// sub_827EE380 appends to it (max 32), so a skipped zeroing would fill it for ever (listing 0x827EF090-0x827EF0B0). module = r3 - 4.
// S103 A3 (S8, audit item 4): copy the list BEFORE either path zeroes it (the entry of the node, walked and ours frames alike); the inputs contributor turns the copy into rows at the latch. One relaxed load when
// the board is not registered; module = r3 - 4.
inline void NotePlaneProxies(uint8_t* base, uint32_t node) {
  if (!rdr_scene::live::planeproxy::RegisteredFlag().load(std::memory_order_relaxed) || scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
  const uint32_t module = node - 4;
  uint32_t raw[rdr_scene::live::planeproxy::kWords], count = 0;
  std::memcpy(raw, rex::memory::GuestPtr(base, module + rdr_scene::live::planeproxy::kListOff), sizeof raw);
  std::memcpy(&count, rex::memory::GuestPtr(base, module + rdr_scene::live::planeproxy::kCountOff), 4);
  rdr_scene::live::planeproxy::The().Note(uint32_t(wwe2k14::scene_skip::HookFrame()), raw, count);
}
inline void PlaneZeroProxies(uint8_t* base, uint32_t node) {
  const uint32_t module = node - 4;
  for (uint32_t k = 0; k < 32; ++k) Wr(base, module + 1176 + 4 * k, 0);
  Wr(base, module + 1304, 0);
}

}  // namespace wwe2k14::scene_skip_nodes

// sub_82C13A80: slot 19 of the skinned-model vtable: loops over [model+552] passes and records the model's own command buffer ([model+520]) around sub_82C04DF0 (begin / end recording:
// sub_82C27548 / sub_82C274C0). A skipped sub_82C04DF0 inside it would seal an EMPTY buffer, so the whole builder is skipped (db: model_submit_vtable_slot19_pass_loop; draw_origin.h's hook is on sub_82C04DF0).
REX_HOOK_RAW(sub_82C13A80) {
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::ModelRecord)) return;   // it writes [model+624/628] and puts them back at its end: nothing to keep
  __imp__sub_82C13A80(ctx, base);
}

// The ring task's two colour draw callbacks (sub_825D0438: posts, turnbuckles, covers, ropes, the ropes' plane proxy; sub_825D01F0: ropes for the other pass). Each saves per-thread render
// context words, sets pass state and puts both back: nothing to keep from the state. What they WRITE (turnbuckle / cover placement from the rope grid) is lane A's input to move (SCENE_SKIPS.md).
REX_HOOK_RAW(sub_825D0438) {
  rdr_scene::live::ringrecompute::RopeView view;   // S103 RLB: the draw thread's camera as the game's classifier (sub_827405F8) asks for it, read before the skip test (RLV.md rows 2, 14)
  if (rdr_scene::live::ringrecompute::RingViewWanted()) wwe2k14::scene_skip_nodes::ReadRingView(ctx, base, view);
  rdr_scene::live::ringrecompute::NoteRing(ctx.r3.u32 - 24, wwe2k14::scene_skip::HookFrame(), &view);   // S103 C3: the ring object X (ring.md section 4: r3 - 24), in every mode, before the skip test
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::RingColour)) return;
  __imp__sub_825D0438(ctx, base);
}
REX_HOOK_RAW(sub_825D01F0) {
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::RingColour2)) return;
  __imp__sub_825D01F0(ctx, base);
}

REX_HOOK_RAW(sub_827EF070) {
  wwe2k14::scene_skip_nodes::NotePlaneProxies(base, ctx.r3.u32);   // S103 A3 (S8): the list as it stands at the node's entry, before any path zeroes it
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::Plane)) {
    wwe2k14::scene_skip_nodes::PlaneZeroProxies(base, ctx.r3.u32);
    return;
  }
  __imp__sub_827EF070(ctx, base);
}

// The pre-light light-cube manager's per-frame entry: slot choice + sub_827EBA00 (re-bake only when the five light rows changed: the rows it stores are what it compares with next time, so a skipped
// call just makes the next walked frame re-bake). Nothing else is known to read what it writes [U: SCENE_SKIPS.md row 9].
REX_HOOK_RAW(sub_827EBE78) {
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::PreLight)) return;
  __imp__sub_827EBE78(ctx, base);
}

// The sweat node: under Ours it returns and does NOT consume its two request flags (+136 init packets, +200 map clear): they stay pending for the game's next WALKED frame, so a fallback frame
// after Ours frames finds the requests the game would have served and builds / clears its own map (review F1). Lane A reads both flags at their writers; ours must treat them as levels, not edges.
// S103 SWF (docs\systems\liquid_map.md): the entry is where the liquid map's live items are final (SWB section 1), so the hook records them first (liquid_glue.h: read only, one relaxed load when no sink wants them). Under Ours our own
// map replaces the game's, so the node's request flag +136 (the clear: create, blood level 0, the Swet reset) must be an EDGE for it: the recorded row carries it and the hook then writes 0, as the game's node does at 0x8279AE00
// after its clear packets (a level would clear our persistent map every frame, a flag nobody consumes). A walked frame leaves the flag to the node. The alt request +200 is left alone (never set: SWB 1c).
REX_HOOK_RAW(sub_8279ACD8) {
  const uint32_t recorded = wwe2k14::liquid_glue::OnNode(ctx.r3.u32, wwe2k14::scene_skip::HookFrame());
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::Sweat)) {
#if !WWE2K14_STREAM()   // (the stream build keeps the game's own map; it never consumes the request itself)
    if ((recorded & 2u) && wwe2k14::scene_skip_nodes::Rd(base, ctx.r3.u32 + rdr_scene::live::liquid::kMapClear) != 0) wwe2k14::scene_skip_nodes::Wr(base, ctx.r3.u32 + rdr_scene::live::liquid::kMapClear, 0);   // only when the recorded row carried the request
#else
    (void)recorded;
#endif
    return;
  }
  __imp__sub_8279ACD8(ctx, base);
}

REX_HOOK_RAW(sub_82254E30) {
  const bool asks_scene = wwe2k14::scene_particle_glue::NoteNode(ctx.r3.u32, ctx.r13.u32, wwe2k14::q2d_tap::RowsThisFrame());   // S103 F2: the pass object, for the particle source (mode 0: one relaxed load); S103 MP: + the thread block (the node's camera) and the 2D row count
  // S103 FLAT (scene_stamp_rule.h): stream-free, a menu backdrop call (screen camera) or a call that draws nothing must not take the Scene3D decision: it would stamp the frame as 3D and no menu / load / results
  // frame could ever be flat. Such a call just walks (the original), as a not-READY menu frame always did. The stream build asks as before.
#if !WWE2K14_STREAM()
  const bool ask = asks_scene;
#else
  const bool ask = true; (void)asks_scene;
#endif
  if (ask && wwe2k14::scene_skip::Ours(::scene_skip::Hook::Particles)) {
    if (wwe2k14::scene_skip_nodes::ParticlesRetireOnly(ctx, base, ctx.r3.u32)) return;
    ::scene_skip::NotSkipped(::scene_skip::Hook::Particles);   // an early out: the original does nothing either
  }
  __imp__sub_82254E30(ctx, base);
}

// sub_82BF8118: the cached-command-buffer replay (op 20), never seen in coverage. NOT skipped (its return value is unknown); this hook only says so the first time it runs under mode 2, because an OURS frame
// would then hold an op-20 replay the contract counts as a double (review F2). The dispatcher sub_82C13C88 picks it when [model+560] is non-zero.
REX_HOOK_RAW(sub_82BF8118) {
  static std::atomic<bool> said{false};
  if (scene_frame::CurrentMode() == scene_frame::Mode::Ours && !said.exchange(true))
    REXLOG_WARN("[scene][skips] sub_82BF8118 (cached command buffer replay, [model+560] != 0) ran under --wwe2k14_rdr_scene=2: its op-20 draws are NOT skipped (a double on an OURS frame); needs its own skip");
  __imp__sub_82BF8118(ctx, base);
}

// The post module's draw node (slot 1): the state save / set / restore it brackets the effects with is balanced, so a skipped walk leaves the device state as it found it (OWN_THE_PASS.md 11).
// The post module's UPDATE task (sub_82C1E7B8, group 20) is a different function and keeps running. The census line is logged from here (once per frame).
// S103 T (SCENE_SKIPS.md 4a): T_FillColorDepth is the RESTORE of the scene colour / depth (images 2 / 5, which the skipped walks never wrote) into the frame pair (8 / 9) [L: POST_PROMOTION_MAP.md row R;
// post_effects.md: sub_827FC798 "restores colour and depth"]. Under OURS the frame image is our picture: the restore would paint the empty scene colour over it (run 7262: black). Its three callers
// (sub_827F10C8, sub_827F0F20, sub_827FC798) are scene draw nodes; the function keeps no state (an op-68 quad with the device states saved and restored around it), so a skipped call leaves nothing
// behind. Menus and every walked frame: Ours() is false and the original runs.
REX_HOOK_RAW(sub_82C1F728) {
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::FillColorDepth)) return;
  __imp__sub_82C1F728(ctx, base);
}

REX_HOOK_RAW(sub_82C1E840) {
  wwe2k14::scene_skip::MaybeLog(::scene_skip::Hook::Post);
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::Post)) return;
  __imp__sub_82C1E840(ctx, base);
}
