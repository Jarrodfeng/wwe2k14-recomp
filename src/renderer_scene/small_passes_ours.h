// The wrestlers' sweat map and skin pre-light passes drawn by us, in place (Session 101, lane 135; docs\renderer\SMALL_PASSES.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// SmallPassesOurs is a DrawReplaceHook (Name "small_passes", fate 18, switch g_small_passes_ours) for two GPU-only passes that read no file:
//  - the sweat map, yLiquidMap (sub_82799B60, call sites 0x8279A19C x2 / 0x82799CB4 / 0x82799DC8): four disc patches a frame into the 1024x512 scratch map (c1287), whose patches are copied on into the
//    persistent sweat map (c1216) that the wrestler skin and gear materials sample in slot 14 and that the next frame's passes read back (the feedback loop);
//  - the skin pre-light, yPreLight (sub_82C20AB8, call 0x82C20B90): five draws a frame of six 11x11 cube faces into a 384x64 target each, copied on into 64x64x6 cube maps the characters sample in slot 13.
// The program (our yLiquidMap / yPreLight effects, the fx registry's generated variants), the constants, the textures, the targets and the copies stay the stream's; this hook replaces the geometry with
// OUR meshes (small_passes_core.h: the rules the game builds them by) and checks, per draw, that they equal the stream's. Because the persistent maps and the feedback copy are the stream's own ops,
// frame N+1's sweat map is built from OUR frame N output without further work. No game or SDK includes. Render thread only.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "small_passes_core.h"   // (no replace_factories.h here: that header includes this class's factory)

namespace rdr_scene {

// Planted faults (lane 135). Each must fail its exact check.
struct SmallFaults {
  bool sweat_no_prev = false;               // R: the sweat draws read a cleared image in slot 0 (the feedback input: the previous map) instead of the persistent map: the sweat never accumulates
  bool slot_null = false;                   // R (SH2's blur_input_null class): the decision reads the sweat draws' fetch slot 0 as image 0; the stream's image is there, so the check must refuse them (texture)
  bool prelight_wrong_target_size = false;  // R: the cube faces are built for 8-unit tiles (a 192x32 target) instead of 16: both the vertex compare and the clip-range check refuse them
  bool no_verify = false;                   // control: no per-draw compare (the geometry is right, nothing moves)
  bool disc_last_vertex = false;            // R (rule 4, the compare covers every vertex): +1e-4 on the sweat mesh's last vertex (186) strength: every sweat draw must be refused (vertices)
  bool cube_last_vertex = false;            // R: +1e-4 on the pre-light mesh's last vertex (725) direction, positions right (the clip check passes): every pre-light draw must be refused (vertices, not clip)
  bool cube_index_order = false;            // R: the pre-light strips' vertex pairs swapped: every pre-light draw must be refused (indices)
  bool disc_no_restart = false;             // R: the sweat strips' restart indices replaced by a repeat of the last index: every sweat draw must be refused (indices)
  bool sweat_one_frame = false;             // R: the previous map is a cleared image on ONE frame only: later frames still differ from the clean arm (the persistent map carries it over)
  static bool Parse(const std::string& list, SmallFaults& f, std::string& error);
};

struct SmallPassesStats {
  uint64_t decisions = 0, replaced = 0, sweat = 0, prelight = 0;
  uint64_t kept[uint8_t(rdr_adapter::KeepWhy::Count)] = {};
  uint64_t read_failed = 0, verified = 0, bad_vertices = 0, bad_indices = 0, bad_shape = 0, bad_layout = 0, bad_clip = 0, null_slot = 0, bad_variant = 0;
  uint64_t frames_ready = 0, frames_idle = 0;
};

class SmallPassesOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  SmallPassesOurs(rdr::Renderer& renderer, const SmallFaults& faults, bool live);
  ~SmallPassesOurs() override;
  SmallPassesOurs(const SmallPassesOurs&) = delete;
  SmallPassesOurs& operator=(const SmallPassesOurs&) = delete;

  const char* Name() const override { return "small_passes"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override;
  bool FrameReady(const rdr_adapter::LoadedFrame&) override { return ++stats_.frames_ready, ready_; }
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override { ++stats_.frames_idle; }

  std::string Report() const;
  const SmallPassesStats& stats() const { return stats_; }

 private:
  bool Upload();
  void Drop();

  rdr::Renderer& renderer_;
  SmallFaults faults_;
  bool live_ = false, ready_ = false;
  rdr::BufferId disc_vertex_ = rdr::kNoBuffer, disc_index_ = rdr::kNoBuffer, cube_vertex_ = rdr::kNoBuffer, cube_index_ = rdr::kNoBuffer;
  rdr::ImageId zero_image_ = rdr::kNoImage;                       // sweat_no_prev: a cleared texel
  std::vector<small_passes::DiscVertex> disc_;
  std::vector<small_passes::CubeVertex> cube_;
  std::vector<uint16_t> disc_idx_, cube_idx_;
  std::pair<uint64_t, uint64_t> verified_disc_{~0ull, ~0ull}, verified_cube_{~0ull, ~0ull};   // live: the (vertex offset, index offset) whose bytes passed
  uint64_t one_frame_at_ = 0;                                     // sweat_one_frame: the frames_ready count of the frame that gets the cleared map (0 = not chosen yet)
  std::string first_fail_;
  SmallPassesStats stats_;
};

}  // namespace rdr_scene
