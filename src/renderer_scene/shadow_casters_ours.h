// Our caster geometry in the shadow passes (Session 101, lane 99 = SO; docs\renderer\SHADOW_PROMOTION_MAP.md sections 3-5, as built: docs\renderer\SHADOW_CASTERS_OURS.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// ShadowCastersOurs is a DrawReplaceHook (Name "shadow_casters") for the model-part draws of the two shadow passes: T_ShadowMap (site 0x82C04634, the wrestler maps) and T_PlaneShadowMap (0x82BF4D38,
// the plane mask). It replaces ONLY the vertex and index buffers, in the stream's own shadow layout; every constant, palette row, target and the whole chain stay the stream's. Which file mesh a stream
// draw is and every per-draw check is ShadowCore (shadow_casters_core.h); this class adds the renderer side: chCommon.pac entry 0 (one load when the game folder is known) and the upload.
// No game or SDK includes. Threads: SetGameRoot from any thread; everything else on the render thread.
#pragma once
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "replace_factories.h"
#include "shadow_casters_core.h"

namespace rdr_scene {

class ShadowCastersOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  ShadowCastersOurs(rdr::Renderer& renderer, const ShadowOptions& options, bool live);
  ~ShadowCastersOurs() override;
  ShadowCastersOurs(const ShadowCastersOurs&) = delete;
  ShadowCastersOurs& operator=(const ShadowCastersOurs&) = delete;

  void SetGameRoot(const std::string& root);

  const char* Name() const override { return "shadow_casters"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return ShadowCore::Claims(origin); }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override { core_.NoClaimFrame(); }

  std::string Report() const;
  const ShadowCore& core() const { return core_; }

 private:
  void Release(ShadowPrepared& p);

  rdr::Renderer& renderer_;
  bool live_;
  ShadowCore core_;
  std::mutex mu_;
  std::string root_;
  bool proxy_started_ = false;
  std::future<std::shared_ptr<const assets::chcommon_proxy::ProxySet>> proxy_future_;
  double upload_ms_ = 0;
};

// The factory for InstallReplaceFactories() (replace_factories.h, shadow_casters_factory.h).
ReplaceHookHandle MakeShadowCastersHook(const ReplaceFactoryArgs& args);

}  // namespace rdr_scene
