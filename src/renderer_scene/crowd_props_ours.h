// The crowd's sign / foam-hand draws drawn by us, in place (S101 lane 133 = N4; docs\renderer\CROWD_PROMOTION.md "N4"; the seam: src\renderer_adapter\draw_replace_hook.h). Copyright (c)
// the wwe2k14-recomp authors. BSD-3-Clause.
//
// CrowdPropsOurs is a DrawReplaceHook for the crowd's two per-member loops (sub_82518950 / sub_82518EB8): the draws at call sites 0x82518D60 (T_Audience_LightMap, 34 vertices) and
// 0x825192D0 (T_Audience, 140 vertices). They are static, unskinned meshes drawn once per prop with their own world matrix (c76-c78) and a CPU-written slot-0 picture, in the same program
// and state as the stands' block draws. The binder keeps all of that (program, state, constants, the light-map page, slot 0); this hook swaps in OUR vertex and index buffers (the arena
// pac's audience-set entry, converted to the stream's host vertex: crowd_prop_game_space.h) after comparing them with the stream's bytes (crowd_props_core.h). It needs no scene-record
// member (the record does not carry these props): only the record's arena, to know which pac to read. Fate: crowd_ours (the crowd's). Switch: g_crowd_props_ours.
// No game or SDK includes. Threads: SetRecord / SetGameRoot from any thread; every other call on the render thread.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../assets/crowd_prop_game_space.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "crowd_props_core.h"
#include "record_io.h"

namespace rdr_scene {

struct CrowdPropsOptions {
  std::string game_root;
  CropFaults faults;
  bool wait_for_load = false;   // offline (fview): wait for the worker, so every frame of a capture is judged
};

class CrowdPropsOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  CrowdPropsOurs(rdr::Renderer& renderer, const CrowdPropsOptions& options);
  ~CrowdPropsOurs() override;
  CrowdPropsOurs(const CrowdPropsOurs&) = delete;
  CrowdPropsOurs& operator=(const CrowdPropsOurs&) = delete;

  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  void Announce(int arena);   // S101 lane 271: the match card names the arena before any record does: the load starts then (render thread; edge-triggered by live.cpp)

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "crowd_props"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return CrowdPropsCore::Claims(origin); }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override;
  std::string WindowDetail() const override;

  std::string Report() const;
  bool ready() const { return uploaded_; }

 private:
  struct Loaded {
    int arena = -1;
    std::shared_ptr<assets::crowd_prop_gs::PropSet> props;
    std::string error;
  };
  void StartLoad(int arena);
  bool Upload(Loaded& loaded);
  void DropAll();
  void ReleaseBuffers();

  rdr::Renderer& renderer_;
  CrowdPropsOptions options_;
  CrowdPropsCore core_;
  std::mutex mu_;   // guards record_ and root_
  std::shared_ptr<const SceneTables> record_;
  std::string root_;
  int arena_ = -1;
  bool uploaded_ = false;
  std::shared_ptr<const assets::crowd_prop_gs::PropSet> props_;
  std::vector<rdr::BufferId> vertex_buffers_, index_buffers_;   // per mesh of props_, made on first use (kNoBuffer: not yet)
  rdr::ImageId stale_image_ = rdr::kNoImage;                    // the planted stale light-map page
  rdr::ImageId first_slot0_ = rdr::kNoImage;                    // the planted one-material picture: the first prop draw's slot 0 of the frame
  std::thread worker_;
  std::atomic<int> load_state_{0};   // 0 idle, 1 loading, 2 done
  Loaded loaded_;
  int load_for_ = -1;
  uint32_t idle_frames_ = 0;
  bool said_no_root_ = false;
  uint64_t not_ready_frames_ = 0, ours_frames_ = 0, frames_seen_ = 0, index32_kept_ = 0, slots_set_ = 0, loads_ = 0;
  double upload_ms_ = 0;
};

}  // namespace rdr_scene
