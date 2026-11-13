// Our props of other pacs (commentary table, ring steps, announcer microphone) drawn in place (S101 lane 84 = PO; docs\renderer\UNCLAIMED_MODEL_PARTS.md 3.2, PROPS_OURS.md; the seam:
// src\renderer_adapter\draw_replace_hook.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// PropsOurs is a DrawReplaceHook for the model-part draws at pass site 0x82C0A950 that the arena, wrestlers and ring hooks pass on as NotOurs (registered after the ring). Like RingOurs:
// the binder keeps program, state, palette rows and run-time slots; this hook swaps in OUR vertex and index buffers (checked byte for byte) and content-checked file textures. Which file
// mesh a stream draw is, and every per-draw check, is PropsCore (props_ours_core.h); this class adds the renderer side: ONE worker load when the record shows a match (not per arena),
// the upload, and the release after 300 frames with no match.
// No game or SDK includes. Threads: SetRecord / SetGameRoot from any thread; every other call on the render thread.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../assets/prop_game_space.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "props_ours_core.h"

namespace rdr_scene {

class PropsOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  PropsOurs(rdr::Renderer& renderer, const PropsOursOptions& options);
  ~PropsOurs() override;
  PropsOurs(const PropsOurs&) = delete;
  PropsOurs& operator=(const PropsOurs&) = delete;

  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  void Announce(int arena);   // S101 lane 271: the match card names the arena before any record does: the load starts then (render thread; edge-triggered by live.cpp)

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "props"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return core_.Claims(origin); }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override;

  std::string Report() const;
  bool ready() const { return uploaded_; }

 private:
  struct Loaded {
    int arena = -1;
    std::shared_ptr<assets::prop_gs::PropSet> props;
    std::string error;
  };
  void StartLoad(int arena);
  bool Upload(Loaded& loaded);
  void DropAll();
  void ReleaseGpu();
  void Release(PropsPrepared& p);
  rdr::ImageId MakeTexture(const assets::Texture& t, uint32_t max_mips);
  rdr::ImageId ImageFor(int texture, uint32_t mips);   // our image of file texture `texture` with `mips` mips, made on first use

  rdr::Renderer& renderer_;
  PropsOursOptions options_;
  PropsCore core_;
  std::mutex mu_;   // guards record_ and root_
  std::shared_ptr<const SceneTables> record_;
  std::string root_;
  int arena_ = -1;
  bool uploaded_ = false;
  std::shared_ptr<const assets::prop_gs::PropSet> props_;
  std::map<uint64_t, rdr::ImageId> images_;   // (file texture, mips) -> our image (kNoImage: could not be made)
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
