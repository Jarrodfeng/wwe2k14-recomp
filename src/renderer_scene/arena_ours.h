// Our arena drawn in place inside the game's frame (Session 101, lane 40; docs\renderer\ARENA_PROMOTION_MAP.md, the design; the seam:
// src\renderer_adapter\draw_replace_hook.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// ArenaOurs is a DrawReplaceHook for the stage slots (0-399) the game's model-part recorder draws at call sites 0x82C0A950 and 0x82C0B94C. The binder
// keeps the stream draw's program, state, constants and every run-time texture (light-map page, shadow maps); this hook swaps in OUR geometry (the file
// mesh in the game's space, floats) and OUR file textures (diffuse, normal map, specular map). Which file mesh a stream draw is, and every per-draw check,
// is ArenaCore (arena_ours_core.h); this class adds the renderer side: the worker load when the record names an arena, the upload, the vertex layouts
// built by name, the release on an arena change or a load screen.
//
// No game or SDK includes. Threads: SetRecord / SetGameRoot from any thread; every other call on the render thread (the one that binds frames).
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../assets/arena_game_space.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "arena_ours_core.h"

namespace rdr_scene {

class ArenaOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  ArenaOurs(rdr::Renderer& renderer, const ArenaOursOptions& options);
  ~ArenaOurs() override;
  ArenaOurs(const ArenaOurs&) = delete;
  ArenaOurs& operator=(const ArenaOurs&) = delete;

  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  void Announce(int arena);   // S101 lane 271: the match card names the arena before any record does: the load starts then (render thread; edge-triggered by live.cpp)

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "arena"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return core_.Claims(origin); }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override;

  const ArenaCore& core() const { return core_; }
  std::string Report() const;
  int arena() const { return arena_; }
  bool ready() const { return uploaded_; }
  uint32_t AssetEpoch() const override { return epoch_; }  // lane 122 (thin draws): counts DropAll, which destroys the buffers and images a thin template names
  uint32_t JoinDropSeq() const override { return core_.DropSeq(); }  // lane 122 (review 2)
  void TakeJoinDrops(std::vector<uint32_t>& out) override { core_.TakeDrops(out); }

  // The vertex layout (by name, from the variant's inputs) our 36-byte vertex feeds; 0xFFFF when the variant reads an input we do not have (tangents...).
  // Exposed for the offline test.
  uint16_t LayoutFor(int vs, bool wide = false);   // S101 lane 49: wide = the 44-byte skin vertex

 private:
  struct MeshGpu {
    rdr::BufferId vertex = rdr::kNoBuffer, index = rdr::kNoBuffer;
    uint32_t index_count = 0;
    bool wide = false;   // S101 lane 49: the vertex buffer holds SkinVertex (44 bytes)
  };
  struct Loaded {                // the worker's result (CPU only)
    int arena = -1;
    std::shared_ptr<assets::arena_gs::StageArena> stage;
    std::string error;
  };

  void StartLoad(int arena);
  bool Upload(Loaded& loaded);
  void DropAll();
  rdr::ImageId MakeTexture(const assets::Texture& t);

  rdr::Renderer& renderer_;
  ArenaOursOptions options_;
  ArenaCore core_;
  std::mutex mu_;                           // guards record_ and root_
  std::shared_ptr<const SceneTables> record_;
  std::string root_;
  int arena_ = -1;                          // the arena being loaded / loaded
  bool uploaded_ = false;
  uint32_t epoch_ = 0;                      // lane 122: ++ in DropAll
  std::shared_ptr<const assets::arena_gs::StageArena> stage_;
  std::vector<MeshGpu> meshes_;             // parallel to stage_->meshes
  std::vector<rdr::ImageId> images_;        // parallel to stage_->textures (kNoImage: could not be made)
  std::thread worker_;
  std::atomic<int> load_state_{0};          // 0 idle, 1 loading, 2 done
  Loaded loaded_;
  int load_for_ = -1;
  std::unordered_map<int, uint16_t> layouts_;
  uint32_t idle_frames_ = 0;
  bool said_variant_ = false, said_no_root_ = false;
  uint64_t not_ready_frames_ = 0, ours_frames_ = 0;
  uint64_t frames_seen_ = 0;
};

}  // namespace rdr_scene
