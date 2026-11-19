// S103 lane T: census hooks (docs\renderer\TRACE_CENSUS.md). One hook per function; sub_827E9740 already has its hook (own_node_hook.h), whose first line calls Note. Each hook
// counts only while --wwe2k14_rdr_scene is not 0 (scene_frame::CurrentMode() != Off: one relaxed load otherwise), logs one line [wwe2k14][census] on the first call, on every
// new (caller, a, b) key and every 600th call, and calls the original. No game state is touched. Remove a hook below to retire its census.
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../dev_build.h"    // IsDevBuild
#include "../frame_stats.h"  // g_guest_frame
#include "../renderer_game/scene_frame.h"
#include "scene_skip.h"   // S103 C3: the flash pass skip
#include "trace_census_core.h"
#include "light_index_hook.h"   // S103 A3 (bit 4)

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_census, false, "wwe2k14",
                    "S103 lane T (dev only): log the [wwe2k14][census] lines of seven scene functions (callers, arguments) while --wwe2k14_rdr_scene is not 0. Off by default");

REX_EXTERN(__imp__sub_827F10C8);
REX_EXTERN(__imp__sub_82815840);
REX_EXTERN(__imp__sub_827F9D88);
REX_EXTERN(__imp__sub_82C0F528);
REX_EXTERN(__imp__sub_826203E8);
REX_EXTERN(__imp__sub_82BCC018);   // S103 A3 (bit 4, round 6): the real R writer (memcpy 896 bytes into the slot's render-camera record)
REX_EXTERN(__imp__sub_82812E90);

namespace wwe2k14::trace_census_hook {

inline trace_census::Table& T(trace_census::Site s) {
  static trace_census::Table tables[int(trace_census::Site::Count)];
  return tables[int(s)];
}

// a / b: the two argument registers that make a key with the caller (see the Site comments); the line prints them as hex.
inline void Note(trace_census::Site s, const PPCContext& ctx, uint32_t a, uint32_t b) {
  static const bool dev = wwe2k14::IsDevBuild();   // S103 P4F: DEV ONLY switch, +dev builds only (one cached bool)
  if (!dev || !REXCVAR_GET(wwe2k14_rdr_scene_census)) return;   // dev only, default off (review B2): the timed flips pay one load
  const auto mode = ::scene_frame::CurrentMode();
  if (mode == ::scene_frame::Mode::Off) return;
  trace_census::Table& t = T(s);
  const trace_census::Result r = trace_census::Note(t, uint32_t(ctx.lr), a, b);
  if (r.log) REXLOG_INFO("[wwe2k14][census] {}", trace_census::Format(s, t, r, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), int(mode)));
}

}  // namespace wwe2k14::trace_census_hook

REX_HOOK_RAW(sub_827F10C8) {
  wwe2k14::trace_census_hook::Note(trace_census::Site::FillNode, ctx, 0, 0);
  __imp__sub_827F10C8(ctx, base);
}
REX_HOOK_RAW(sub_82815840) {
  wwe2k14::trace_census_hook::Note(trace_census::Site::PlaneBlur, ctx, 0, 0);
  __imp__sub_82815840(ctx, base);
}
REX_HOOK_RAW(sub_827F9D88) {
  wwe2k14::trace_census_hook::Note(trace_census::Site::PlaneBind, ctx, ctx.r4.u32, ctx.r5.u32);   // r4 = controller, r5 = channel (STAGE_LIVE_WRITERS.md section 2)
  __imp__sub_827F9D88(ctx, base);
}
REX_HOOK_RAW(sub_82C0F528) {
  // S103 C3: under Ours the game's flash sprite is not drawn (ours is: kFxFlash rows, flash_fx_core.h). Only the pass call is skipped: its caller sub_823B27F0 still runs the visibility test, the
  // done mark and the +52 flag around it (the writer reads that state), and ignores the result. Evidence the pass draws only flashes: run 10 (wwe2k14_078.log) 60 of 60 logged calls came from sub_823B27F0
  // (0x823B2BC8); no other caller of the method is known (first 60 calls of one entrance: [U] beyond that).
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::FlashPass)) {
    wwe2k14::trace_census_hook::Note(trace_census::Site::Flash, ctx, ctx.r3.u32, 0);
    return;
  }
  wwe2k14::trace_census_hook::Note(trace_census::Site::Flash, ctx, ctx.r3.u32, 0);
  __imp__sub_82C0F528(ctx, base);
}
// S103 A3 (bit 4, E4's finding + review round 6): the wrestler draw nodes install their EMBEDDED object through sub_82620488, which reaches sub_826203E8 only when ungated with an override word >= 0.
// Not every install ends in sub_8261FE90 (it returns without installing when its global is 0) and some skip it: THE writer of R is sub_82BCC018(G, src) = memcpy(R, src, 896), called by sub_8261FE90
// (src = obj + 48), directly by sub_823B27F0 (flash items, src = item + 64, after sub_826203E8) and by sub_827FD698 (two sites, src = item + 64). Hooking it sees exactly the copies that happen, with
// the SOURCE of every one (the block the model's draws will read). Nothing else hooks it (grep of src / config).
REX_HOOK_RAW(sub_82BCC018) {
  wwe2k14::light_index_hook::OnInstallObjectHook(ctx);   // r4 = src
  __imp__sub_82BCC018(ctx, base);
}
REX_HOOK_RAW(sub_826203E8) {
  wwe2k14::light_index_hook::OnInstallHook(ctx);   // S103 A3 (bit 4): (index, mode) -> the thread slot's current install (light_index_core.h)
  wwe2k14::trace_census_hook::Note(trace_census::Site::LightInstall, ctx, ctx.r3.u32, ctx.r4.u32);   // index, mode (INPUTS_A2.md)
  __imp__sub_826203E8(ctx, base);
}
REX_HOOK_RAW(sub_82812E90) {
  wwe2k14::trace_census_hook::Note(trace_census::Site::CircleDraw, ctx, 0, 0);
  __imp__sub_82812E90(ctx, base);
}
