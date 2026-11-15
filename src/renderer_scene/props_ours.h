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
#include "props_budget.h"   // S102 T17
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
  std::string BudgetLine() const;   // S102 T17: "[rdr][props_budget] frames N, slices S, bytes B, fallback F, ..." ("" while the switch is off)
  bool ready() const { return uploaded_; }

 private:
  struct Loaded {
    int arena = -1;
    std::shared_ptr<assets::prop_gs::PropSet> props;
    std::shared_ptr<const props_budget::Pre> pre;   // S102 T17: made on the worker (switch on at the load)
    std::string error;
  };
  void StartLoad(int arena);
  bool ReplaceOne(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why);   // S102 T17: Replace's body (Replace times it for fview)
  bool Upload(Loaded& loaded);
  void DropAll();
  void ReleaseGpu();
  void Release(PropsPrepared& p);
  rdr::ImageId MakeTexture(int texture, uint32_t max_mips);   // S102 T17: the texture by index (the worker's tables are indexed the same)
  rdr::ImageId ImageFor(int texture, uint32_t mips);   // our image of file texture `texture` with `mips` mips, made on first use

  // S102 T17 (props_budget.h): the frame's making of buffers / images is held to a budget; a draw past it keeps the stream's draw for the frame.
  void BudgetTick(const rdr_adapter::LoadedFrame* frame);   // FrameReady passes its frame: the budget resets when the GUEST frame changes (a second bind of the same frame, a DrawAndRead part, keeps spending); NoClaimFrame passes null: only re-reads the switch
  uint32_t ImageKey(int texture, uint32_t mips) const;  // the mips an image of (texture, mips) really has (FitMips; always, switch on or off)
  bool RoleWanted(const rdr_adapter::ReplaceIn& in, int slot, int tex) const;   // THE rule for "this role gets our image": need and put() both call it
  size_t Need(const rdr_adapter::ReplaceIn& in, const PropsCore::Decision& d, const PropsPrepared& p);   // the bytes this draw makes now (buffers + images), by the same rules put() makes them
  void NoteMade(uint64_t n);
  void FreeTestOnly();   // the resources only a planted fault makes (hole_buffer_, twice_): freed by ReleaseGpu, DropAll (and so the destructor)
  size_t ImageNeed(int texture, uint32_t mips) const;   // bytes of a texture's image if it is not made yet (0 when made)
  std::shared_ptr<const props_budget::Pre> pre_;
  bool budget_on_ = false;
  uint64_t frame_bytes_ = 0, frame_us_ = 0, frame_made_ = 0;
  uint64_t frame_key_ = 0, first_need_ = 0;
  bool have_key_ = false;
  int plant7_tex_ = -1;
  uint64_t b_first_over_ = 0, b_forced_us_ = 0, b_deferred_pre_ = 0, b_made_bytes_ = 0;
  uint64_t b_frames_ = 0, b_slices_ = 0, b_bytes_ = 0, b_fallback_ = 0, b_deferred_ = 0, b_images_made_ = 0, b_max_frame_bytes_ = 0, b_overrun_ = 0;
  rdr::BufferId hole_buffer_ = rdr::kNoBuffer;   // TESTONLY_RES (plant 2)
  std::vector<rdr::ImageId> twice_;                // TESTONLY_RES (plant 3: the images the plant made again)

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
