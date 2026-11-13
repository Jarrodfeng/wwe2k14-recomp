// Our crowd camera flashes drawn in place inside the game's frame (Session 101, lane 68; docs\renderer\PARTICLES_PROMOTION_MAP.md sections 2-3 lane F,
// docs\formats\crowd_flash.md; the seam: src\renderer_adapter\draw_replace_hook.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// FlashOurs is a DrawReplaceHook for the model-part pass at pass site 0x82C139FC (sub_82C0F528, after the post combine): one draw per flash, always the same
// object, part 0, 20 vertices / 33 indices [C: Check B, 1,071 draws; Bryan 125]. The binder keeps the stream draw's program, state, every constant (world c76-c78, centre
// c214, colours) and the scene-depth copy in slot 17 (the vertex stage's hidden-flash test: the game's own image, never ours); this hook swaps in OUR geometry (the
// arena pac's entry 31000, floats, game space) and OUR texture (the model's flash01) in slot 0. The model is the loaded arena's (the record names the arena, as the
// arena hook does): bg00..bg56 hold byte-equal geometry (entry 31000 is missing in bg23, bg66-68, bg76-80) and the same texture except bg28-bg30 (another flash01) [C: md5 of the entry and of flash01 in every bgNN.pac, lane 68].
//
// No game or SDK includes. Threads: SetRecord / SetGameRoot from any thread; every other call on the render thread (the one that binds frames).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../assets/flash_game_space.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/image_content_table.h"
#include "record_io.h"

namespace rdr_scene {

inline constexpr uint32_t kFlashPassSite = 0x82C139FCu;   // the flash pass's sub_82BDED40 call [C: PARTICLES map 1.1, census.py FLASH_PASS]

// Planted faults (the acceptance test's twins: each must fail its exact check; PARTICLES map sections 5 / 9).
struct FlashFaults {
  bool no_depth = false;      // R: slot 17 (the scene-depth copy) replaced by ours (a 1x1 zero image): no flash is ever hidden by the scene
  bool first_arena = false;   // the first arena's pac (bg00) for every arena. The flash geometry and texture of bg00 are byte-equal to bg04's [C], so on Check B this twin cannot differ: it is the control that says so
  bool wrong_arena = false;   // bg28's pac for every arena: the same geometry but another texture (bg28-bg30), so the picture moves while every size / count check still passes
  bool count_off = false;     // the expected index count is one too many: every draw is refused (mismatch)
  bool all_mips = false;      // R (found building this lane): our image keeps all the file's mip levels instead of the stream slot's max_level + 1 (the game's image has 3 of 7): small flashes sample levels the game clamps off, max 9 levels off on Check B frame 340
  bool Any() const { return no_depth || first_arena || wrong_arena || count_off || all_mips; }
};

struct FlashOursOptions {
  std::string game_root;     // the game files (game_data\extracted); read when the first record names an arena
  FlashFaults faults;
  bool verify_content = false;   // offline (fview): compare every vertex (position, colour, uv) of every draw with the file's; live: first and last vertex, when the draw's blob key changes
  // The capture image record behind a frame texture slot (ImageRecord id -> width, height); null: the slot-0 size is not checked and the stream's image stays (counted).
  std::function<bool(uint32_t record_id, uint32_t& w, uint32_t& h)> image_info;
  // The stream image's mip count (ImageContent::mips) when the owner feeds it (replace_factories.h image_content); null: max_level + 1 alone.
  std::function<bool(uint32_t record_id, rdr_adapter::ImageContent& out)> image_content;
};

// OUR vertex (40 bytes): what yFlashDefault inputs read (inPosition, inBoneId uint 0, inNormal, inColor, inUv).
struct FlashVertex {
  float pos[3];
  uint32_t bone_id;   // yFlashDefault reads inBoneId as an integer input (validation: the pipeline format must be an integer one); 0 = the first bone
  float normal[3];
  uint8_t color[4];
  float uv[2];
};
static_assert(sizeof(FlashVertex) == 40, "flash vertex layout");

struct FlashStats {
  uint64_t decisions = 0, replaced = 0;
  uint64_t kept[uint8_t(rdr_adapter::KeepWhy::Count)] = {};
  uint64_t bad_vertices = 0, bad_indices = 0, bad_part = 0, bad_content = 0, bad_size = 0, bad_view = 0, bad_asset = 0;
  uint64_t size_unchecked = 0, no_slot0 = 0, objects = 0;
  uint64_t verified_full = 0, verified_ends = 0;
};

class FlashOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  FlashOurs(rdr::Renderer& renderer, const FlashOursOptions& options);
  ~FlashOurs() override;
  FlashOurs(const FlashOurs&) = delete;
  FlashOurs& operator=(const FlashOurs&) = delete;

  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  void Announce(int arena);   // S101 lane 271: the match card names the arena before any record does: the load starts then (render thread; edge-triggered by live.cpp)

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "flash"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return origin.klass == rdr_adapter::DrawClass::ModelPart && origin.pass_site == kFlashPassSite; }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override;

  const FlashStats& stats() const { return stats_; }
  std::string Report() const;
  int arena() const { return arena_; }
  bool ready() const { return uploaded_; }

  // The vertex layout (by name, from the variant's inputs) our 40-byte vertex feeds; false when the variant reads an input we do not have (skin words...).
  static bool BuildLayout(int vs, rdr::VertexLayoutDesc& out);
  uint16_t LayoutFor(int vs);   // 0xFFFF when BuildLayout refuses
  static uint32_t ExpectedIndexCount(const assets::flash_gs::FlashMesh& m, bool count_off) { return m.StreamIndexCount() + (count_off ? 1u : 0u); }

 private:
  struct Loaded {                // the worker's result (CPU only)
    int arena = -1;
    std::shared_ptr<assets::flash_gs::FlashMesh> mesh;
    std::string error;
  };
  void StartLoad(int arena);
  bool Upload(Loaded& loaded);
  void DropAll();
  rdr::ImageId ImageFor(uint32_t levels);

  rdr::Renderer& renderer_;
  FlashOursOptions options_;
  std::mutex mu_;                           // guards record_ and root_
  std::shared_ptr<const SceneTables> record_;
  std::string root_;
  int arena_ = -1;                          // the arena being loaded / loaded
  bool uploaded_ = false;
  std::shared_ptr<const assets::flash_gs::FlashMesh> mesh_;
  rdr::BufferId vertex_ = rdr::kNoBuffer, index_ = rdr::kNoBuffer;
  uint32_t index_count_ = 0;
  rdr::ImageId no_depth_image_ = rdr::kNoImage;
  std::unordered_map<uint32_t, rdr::ImageId> images_;   // the model texture by mip-level count (ImageFor)
  std::thread worker_;
  std::atomic<int> load_state_{0};          // 0 idle, 1 loading, 2 done
  Loaded loaded_;
  int load_for_ = -1;
  std::unordered_map<int, uint16_t> layouts_;
  std::unordered_map<uint32_t, uint64_t> verified_key_;   // object -> the vertex key whose first / last vertex passed (live)
  std::unordered_map<uint32_t, bool> objects_;
  uint32_t idle_frames_ = 0;
  bool said_variant_ = false, said_no_root_ = false;
  uint64_t not_ready_frames_ = 0, ours_frames_ = 0, frames_seen_ = 0;
  std::string first_fail_;
  FlashStats stats_;
};

}  // namespace rdr_scene
