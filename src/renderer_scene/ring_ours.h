// Our ring drawn in place inside the game's frame (Session 101, lane 70 = RO; docs\renderer\RING_PROMOTION_MAP.md, the design; docs\renderer\RING_OURS.md, as built; the seam:
// src\renderer_adapter\draw_replace_hook.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// RingOurs is a DrawReplaceHook for the ring pieces (post, turnbuckle, canvas, ropes both LODs, covers, skirts) the game's model-part recorder draws at pass site 0x82C0A950. The binder
// keeps the stream draw's program, state, palette rows (the instance transform and the rope simulation), light map and every run-time slot; this hook swaps in OUR vertex and index
// buffers (built from the player's bgNN.pac in the STREAM's host layout, checked byte for byte) and the file textures. Which file mesh a stream draw is, and every per-draw check, is
// RingCore (ring_ours_core.h); this class adds the renderer side: the worker load when the record names an arena, the upload, the release on an arena change or a load screen.
// No game or SDK includes. Threads: SetRecord / SetGameRoot from any thread; every other call on the render thread.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../assets/ring_game_space.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "ring_ours_core.h"
#include "shadow_join_board.h"   // S101 lane 99

namespace rdr_scene {

class RingOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  RingOurs(rdr::Renderer& renderer, const RingOursOptions& options);
  ~RingOurs() override;
  RingOurs(const RingOurs&) = delete;
  RingOurs& operator=(const RingOurs&) = delete;

  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  void Announce(int arena);   // S101 lane 271: the match card names the arena before any record does: the load starts then (render thread; edge-triggered by live.cpp)

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "ring"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return core_.Claims(origin); }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override;
  void WantJoinDrops(bool on) override { core_.SetDropsWanted(on); }   // lane 129 review F6
  uint32_t JoinDropSeq() const override { return core_.DropSeq(); }   // lane 129 (KC)
  void TakeJoinDrops(std::vector<uint32_t>& out) override { core_.TakeDrops(out); }
  uint32_t AssetEpoch() const override { return epoch_; }   // lane 129 (KC, thin draws): counts every release of a buffer or image a Replace result names (an arena change, a failed upload)

  std::string Report() const;
  bool ready() const { return uploaded_; }

 private:
  struct Loaded {
    int arena = -1;
    std::shared_ptr<assets::ring_gs::RingArena> ring;
    std::string error;
  };
  bool EnsureArena(const SceneTables& record);   // S101 lane 99: FrameReady's arena half, idempotent (ShadowJoinBoard)
  void StartLoad(int arena);
  bool Upload(Loaded& loaded);
  void DropAll();
  void Release(RingPrepared& p);
  rdr::ImageId MakeTexture(const assets::Texture& t, uint32_t max_mips);
  rdr::ImageId ImageFor(int texture, uint32_t mips);   // our image of file texture `texture` with `mips` mips, made on first use

  rdr::Renderer& renderer_;
  RingOursOptions options_;
  RingCore core_;
  std::mutex mu_;   // guards record_ and root_
  std::shared_ptr<const SceneTables> record_;
  std::string root_;
  int arena_ = -1;
  uint32_t epoch_ = 0;   // lane 129 (KC)
  bool uploaded_ = false;
  std::shared_ptr<const assets::ring_gs::RingArena> ring_;
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
