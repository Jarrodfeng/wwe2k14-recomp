// The plane shadow chain's constants from the scene record (Session 101, lane 139 = SH3 slice 2; docs/renderer/SHADOW_PLANE.md; design SHADOW_PROMOTION_MAP.md section 6).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// PlaneShadowOurs is a DrawReplaceHook (Name "plane_shadow", fate 19, switch g_plane_shadow_ours) for the stream's plane-chain draws: the mask's part draws (pass site 0x82BF4D38, T_PlaneShadowMap) and the two
// blur draws (call site 0x827EEC84, T_PlaneShadowBlur). Lane 159 (SH3 slice 3): THE DRAWS ARE OURS. Each is made with OUR program (rdr_plane_mask / rdr_plane_blur, src/renderer/shaders) and OUR constants
// block (ReplaceOut::own with stream_geometry: the draw keeps its place, target, state, textures, and the STREAM's vertex / index streams = the proxy meshes and the blur quad). The constants come from the
// scene record (the fade array, the plane position, PlaneInv, the camera; the blur's step, matrix and scale), each proven word for word against the stream's own before the draw is ours. Lane 139's honest
// list of what the record does not own STAYS the stream's, copied from the stream draw into our block: EyePos c72, the bone palette c76.. and the bone-count switches (mask), the coverage threshold c34 (blur's
// pixel constant); the geometry, targets, copies and the downscale draw are the stream's (plane_sites.h; SHADOW_PLANE.md). Entries join by the guest-frame STAMP and (object, part, nth) - never by the record's index - with the stamp ->
// stream-frame offset learned from the first entry that verifies. A draw that does not verify stays the stream's, counted by reason. Composition with lane 99's shadow_casters: it Claims the plane
// site first and does NOT pass a draw on unless it answers NotOurs, so with it on this hook sees none of the mask draws it claimed (the census says: mask claimed / blur claimed). No game or SDK includes.
// Render thread only.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/shadow_roles.h"
#include "plane_shadow_core.h"
#include "replace_factories.h"

namespace rdr_scene {

class PlaneShadowOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  PlaneShadowOurs(rdr::Renderer& renderer, const plane::Faults& faults);
  bool ok() const { return mask_program_ != 0 && blur_program_ != 0; }   // lane 159 (N5): false when a driver refused one of our modules (the factory then says "not started")
  const char* Name() const override { return "plane_shadow"; }
  bool NeedsFragmentBlock() const override { return true; }   // lane 159: the blur keeps the stream's c34 (a pixel constant), read from ReplaceIn::fs_block
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return origin.pass_site == plane::kMaskSite || origin.call_site == plane::kBlurCallSite; }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoteMask(bool joined) override { ++(joined ? stats_.joined : stats_.alone); }   // lane 255: the binder's result for this replaced mask draw
  uint32_t JoinFault() const override { return faults_.join_double ? 1u : faults_.join_drop ? 2u : 0u; }
  std::string WindowDetail() const override;   // lane 205: Report() plus the first refused blur draw of the window as hex

  void SetRecord(const std::shared_ptr<const SceneTables>& record);
  std::string Report() const;
  static std::string FormatRefusal(const plane::MatchDiff& d, uint32_t stamp, uint32_t nth, uint32_t have, bool incomplete);

  struct Stats {
    uint64_t frames = 0, frames_not_ready = 0, claimed = 0, claimed_mask = 0, claimed_blur = 0, replaced = 0, replaced_mask = 0, replaced_blur = 0;
    uint64_t kept_unjoined = 0, kept_mismatch = 0, kept_rows = 0, kept_late = 0, kept_variant = 0, kept_nothing = 0, relearned = 0;
    uint64_t kept_state = 0, kept_inputs = 0, kept_incomplete = 0;   // kept_incomplete (lane 205): the entry had nothing to prove (no blur bit); counted inside `mismatch`, not inside `constants`
      // lane 159: the stream draw asks for an alpha test our program has no twin for / an input the stream's block does not carry (EyePos, the palette, c34)
    uint64_t by_delta[plane::kDeltaLast - plane::kDeltaFirst + 1] = {};
    uint64_t joined = 0, alone = 0;   // lane 255: mask draws made with the shadow casters' mesh AND our program (binder join)
    uint64_t rows_compared = 0, rows_min = 0, rows_max = 0;   // rows verified per replaced draw: min / max (a draw with fewer than the module's 17 has lost a row)
  };
  const Stats& stats() const { return stats_; }
  int delta() const { return delta_known_ ? delta_ : 99; }

 private:
  plane::Faults faults_;
  plane::Board board_;
  uint32_t stream_stamp_ = 0, blur_ordinal_ = 0;
  std::unordered_map<uint64_t, uint32_t> mask_nth_;   // (object, part) -> draws seen this frame
  bool delta_known_ = false;
  int delta_ = 0;
  uint32_t misses_ = 0;
  mutable std::string first_refused_;   // lane 205: the window line's refused-blur detail, emptied when printed
  bool locked_ = false;   // R stamp_lock
  // lane 159: our programs and the two constant blocks (the mask's 1,232 bytes and the blur's 112, std140 as the shaders declare them)
  uint16_t mask_program_ = 0, blur_program_ = 0;
  alignas(16) float mask_vs_[308] = {};
  alignas(16) float blur_vs_[28] = {};
  alignas(16) float fs_[4] = {};
  Stats stats_;
};

ReplaceHookHandle MakePlaneShadowHook(const ReplaceFactoryArgs& args);   // defined in plane_shadow_ours.cpp

}  // namespace rdr_scene
