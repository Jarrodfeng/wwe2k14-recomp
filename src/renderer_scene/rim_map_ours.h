// The rim light map's casters drawn by us, in place (Session 101, lane 124 = RM; docs\renderer\RIM_MAP_OURS.md; design: SHADOW_PROMOTION_MAP.md sections 1.1, 2.3, 9).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// RimMapOurs is a DrawReplaceHook (Name "rim_map", fate 15, switch g_rim_ours) for the model-part draws of T_RimLightMap (sub_82BF9878, pass site 0x82BFBD58): the wrestlers (and pieces) drawn
// with effect yRimLightMap into the 512x256 RGBA8 rim map the wrestler materials read in slot 15. Their vertices are the shadow maps' (the same skinned 32 B / rigid 24 B host layouts, byte-equal to the
// shadow-map draw of the same object / part where there is one: lane 124's census, tools\rim\rim_inspect.py), so the pairing and the writer are lane 99's ShadowCore, used as it is:
// a rim draw's `pass_site` is neither shadow site, so ShadowCore::Decide takes its wrestler branch (the wrestlers' JoinOf: the file model and the one scale, confirmed through every colour draw of the
// object). Constants, palette rows (the frame's pose), the target, the clear, the reductions (128x64 / 32x16) and the blur (yBlur/t2) stay the stream's: this replaces the vertex and index buffers only.
// No game or SDK includes. Render thread only.
#pragma once
#include <memory>
#include <string>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/shadow_roles.h"
#include "replace_factories.h"
#include "shadow_casters_core.h"

namespace rdr_scene {

// Planted faults (lane 124). Each must fail its exact check; the writer's faults of lane 99 are accepted too (ShadowFaults).
struct RimFaults {
  bool no_pose = false;        // R: the draw's palette rows are overridden with the bind pose (identity bones) instead of the frame's pose: the rim silhouette changes
  bool first_object = false;   // R: one object's join (the first claimed draw's of the frame) used for every object
  bool serial_running = false; // R (review B1): the old early-start dedupe (key and bind number) with this hook's bind number running ahead: the wrestlers start twice in a bind
  bool wrong_target = false;   // R: the hook claims the shadow maps' draws (site 0x82C04634) instead of the rim map's: the draws land in a shadow map
  ShadowFaults core;           // no_verify and the writer's faults
  static bool Parse(const std::string& list, RimFaults& f, std::string& error);
};

class RimMapOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  RimMapOurs(rdr::Renderer& renderer, const RimFaults& faults, bool live);
  ~RimMapOurs() override;
  RimMapOurs(const RimMapOurs&) = delete;
  RimMapOurs& operator=(const RimMapOurs&) = delete;

  const char* Name() const override { return "rim_map"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override;
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override { core_.NoClaimFrame(); }

  std::string Report() const;
  const ShadowCore& core() const { return core_; }

 private:
  void Release(ShadowPrepared& p);

  rdr::Renderer& renderer_;
  RimFaults faults_;
  ShadowCore core_;
  uint32_t first_object_ = 0;      // first_object plant: this frame's first claimed object
  std::vector<float> pose_rows_;   // no_pose plant: identity bone rows
  double upload_ms_ = 0;
};

// The factory for InstallReplaceFactories() (replace_factories.h, rim_map_factory.h).
ReplaceHookHandle MakeRimMapHook(const ReplaceFactoryArgs& args);

}  // namespace rdr_scene
