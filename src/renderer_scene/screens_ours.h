// Our stage screens (the ribbon band, the wall behind the stage, the mini-tron) drawn in place (S101 lane 115 = SO; docs\renderer\UNCLAIMED_MODEL_PARTS.md 3.3, SCREENS_OURS.md; the seam:
// src\renderer_adapter\draw_replace_hook.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// ScreensOurs is a DrawReplaceHook for the model-part draws at pass site 0x82C0F4BC (sub_82C0CA08: the stage screens, keys 801-807 of the arena's pac, drawn with yMovieDefault). GEOMETRY ONLY:
// the binder keeps program, state, constants and EVERY texture slot (the video planes are run-time pictures); this hook swaps in OUR vertex and index buffers (checked byte for byte).
// Which file mesh a stream draw is, and every per-draw check, is ScreensCore (screens_ours_core.h); this class adds the renderer side: one worker load per arena, the upload, and the release
// after 300 frames with no arena. No game or SDK includes. Threads: SetRecord / SetGameRoot from any thread; every other call on the render thread.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../assets/screen_game_space.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "screens_ours_core.h"

namespace rdr_scene {

class ScreensOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  ScreensOurs(rdr::Renderer& renderer, const ScreensOursOptions& options);
  ~ScreensOurs() override;
  ScreensOurs(const ScreensOurs&) = delete;
  ScreensOurs& operator=(const ScreensOurs&) = delete;

  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  void Announce(int arena);   // S101 lane 271: the match card names the arena before any record does: the load starts then (render thread; edge-triggered by live.cpp)

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "screens"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return core_.Claims(origin); }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override;

  std::string Report() const;
  bool ready() const { return uploaded_; }

 private:
  struct Loaded {
    int arena = -1;
    std::shared_ptr<assets::screen_gs::ScreenSet> set;
    std::string error;
  };
  void StartLoad(int arena);
  bool Upload(Loaded& loaded);
  void DropAll();
  void Release(ScreensPrepared& p);

  rdr::Renderer& renderer_;
  ScreensOursOptions options_;
  ScreensCore core_;
  std::mutex mu_;   // guards record_ and root_
  std::shared_ptr<const SceneTables> record_;
  std::string root_;
  int arena_ = -1;
  bool uploaded_ = false;
  std::shared_ptr<const assets::screen_gs::ScreenSet> set_;
  std::thread worker_;
  std::atomic<int> load_state_{0};   // 0 idle, 1 loading, 2 done
  Loaded loaded_;
  int load_for_ = -1;
  uint32_t idle_frames_ = 0;
  bool said_no_root_ = false;
  uint64_t not_ready_frames_ = 0, ours_frames_ = 0, frames_seen_ = 0;
  double upload_ms_ = 0;
};

}  // namespace rdr_scene
