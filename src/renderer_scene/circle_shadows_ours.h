// The circle blob shadows drawn by us, from the scene record (Session 101, lane 136 = SH3 slice 1; docs/renderer/SHADOW_CIRCLES.md; design SHADOW_PROMOTION_MAP.md section 6).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// CircleShadowsOurs is a DrawReplaceHook (Name "circles", fate 16, switch g_circles_ours) for the stream's T_CircleShadow draws (call site 0x828131E4: one disc per character, 4 a frame in the
// Brock entrance). The draw keeps its place, its target (SH2 re-targets the props map into our image: the draw is drawn into it like any other) and its program (the generated yCircleShadow);
// the hook supplies the disc mesh (the record's CircleMesh, uploaded once) and the vertex constants c0 (ShadowParam), c4-c7 (ViewProj) and c8-c28 (the 21 body points) from the record's
// CircleShadow entry of the same character and frame. Entries join by the guest-frame STAMP (never by the record's index: SKIP_GAME_WORK_MAP.md section 8), with the stamp -> stream-frame offset
// learned from the first entry that verifies. Every draw is checked against the stream draw's own constants (and the mesh against its vertex bytes when the geometry is readable): a draw that
// does not verify stays the stream's, counted by reason. No game or SDK includes. Render thread only.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/shadow_roles.h"
#include "circle_shadow_core.h"
#include "replace_factories.h"

namespace rdr_scene {

class CircleShadowsOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  CircleShadowsOurs(rdr::Renderer& renderer, const circles::Faults& faults, bool live);
  ~CircleShadowsOurs() override;
  CircleShadowsOurs(const CircleShadowsOurs&) = delete;
  CircleShadowsOurs& operator=(const CircleShadowsOurs&) = delete;

  const char* Name() const override { return "circles"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return origin.call_site == rdr_adapter::shadow_roles::kCircleCallSite; }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override {}
  std::string WindowDetail() const override { return Report(); }

  // The record for the frame about to be bound (entries are added to the board; an older record's entries stay, bounded).
  void SetRecord(const std::shared_ptr<const SceneTables>& record);
  void SetRecordNow(const std::shared_ptr<const SceneTables>& record);   // (SetRecord without the late_record delay)
  std::string Report() const;

  struct Stats {
    uint64_t frames = 0, frames_not_ready = 0, claimed = 0, replaced = 0;
    uint64_t kept_unjoined = 0, kept_mismatch = 0, kept_variant = 0, kept_not_ready = 0, kept_shape = 0, kept_rows = 0, kept_late = 0, kept_mesh_wait = 0, index_bad = 0;
    uint32_t last_shape[5] = {};   // the stream draw's vertex count, index count, stride, index width, vs block bytes (the first shape refusal)
    circles::RowDiff first_diff;   // lane 215 (log first): the first constants refusal of the run: register, lane, both words
    uint32_t first_diff_stamp = 0, first_diff_ordinal = 0;
    uint64_t mesh_checked = 0, mesh_bad = 0, mesh_unreadable = 0, upload_failed = 0, relearned = 0;
    uint64_t by_delta[circles::kDeltaLast - circles::kDeltaFirst + 1] = {};
    double upload_ms = 0;
  };
  const Stats& stats() const { return stats_; }
  int delta() const { return delta_known_ ? delta_ : 99; }

 private:
  bool UploadMesh();
  void ReleaseMesh();

  rdr::Renderer& renderer_;
  circles::Faults faults_;
  circles::Board board_;
  std::unordered_map<uint64_t, CircleShadow> first_pose_;   // bind_pose plant: the first entry seen per (slot, character)
  uint32_t stream_stamp_ = 0, ordinal_ = 0;
  bool delta_known_ = false;
  int delta_ = 0;
  uint32_t misses_ = 0;
  uint64_t mesh_gpu_[2] = {0, 0};          // vertex, index buffer ids (0 = none)
  bool mesh_uploaded_ = false, mesh_verified_ = false;
  CircleMesh mesh_;                        // the mesh the buffers hold
  float rows_[circles::kRows * 4] = {};
  std::shared_ptr<const SceneTables> held_;   // late_record: the record waiting one call
  uint32_t records_seen_ = 0, shift_seen_ = 0;
  Stats stats_;
};

ReplaceHookHandle MakeCirclesHook(const ReplaceFactoryArgs& args);   // defined in circle_shadows_ours.cpp

}  // namespace rdr_scene
