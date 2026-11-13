// RimChainOurs: the rim light map's coverage reductions and blur drawn by our own programs, in the stream draws' places (Session 101, lane 151 = "P3" of docs\renderer\RIM_MAP_OURS.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The chain (read from three captures, tools\rim\rim_chain_census.py; every frame carries it, its constants and geometry never change):
//   rim map 1189 (512x256, the casters, lane 124's RimMapOurs) --copy--> 222;
//   reduction 1: yBlur/flag_any, call 0x82C1F48C, one rectangle of 4 vertices, reads 222, writes 1191 (128x64) --copy--> 224;
//   reduction 2: the same program, reads 224, writes 1192 (32x16) --copy--> 225;
//   blur: yBlur/t2 with the masked grid vertex stage, call 0x82814080, reads 222 and (vertex slot 16) 225, writes 1193 (512x256) --copy--> 223, which the wrestler materials read in slot 15.
// What is ours: the three programs (shaders rdr_rimchain_*) and the quad / grid they draw. What stays the stream's, on purpose: the draws' places in the frame, the targets, the copies, the
// clears (none), the viewport / scissor / system block, the samplers, and every constant (the reductions' four offsets, the blur's 13 offsets and weights and the grid's VertexConvert are
// passed through as bound; the rectangle and the grid are checked against the stream's own vertex bytes). The readers (the wrestler materials) bind the stream's 223 as before.
//
// No game or SDK includes. Everything runs on the render thread. No game files and no scene record are read.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"

namespace rdr_scene {

inline constexpr uint32_t kRimReduceCallSite = 0x82C1F48Cu;   // the generic post queue site the two reductions are recorded from (sub_82C1F48C) [C: tools\rim\rim_chain_inspect.py]
inline constexpr uint32_t kRimBlurCallSite = 0x82814080u;     // sub_82813EB0's draw (T_ShadowMapBlur); the rim blur is its yBlur/t2 draw [C]

// Planted faults (the acceptance test's twins: each must fail its exact check). --replace-fault rimchain.NAME
struct RimChainFaults {
  bool reduction_wrong_size = false;   // R: the reductions' rectangles are half as wide as the stream's: half of each coverage map is never written
  bool blur_no_input = false;          // R: the blur's input slot bound to nothing (a black texture)
  bool blur_input_null = false;        // R (SH2's blur_input_null class): the decision reads the blur's input slot as image 0: the check must refuse every blur draw (texture), the stream's blur stays
  bool rim_reader_stale = false;       // R: the blur reads the blurred map of the PREVIOUS frame (image 223, rewritten only after the blur) instead of this frame's rim map: a reader bound to last frame's image
  bool unmasked = false;               // EXPERIMENT (not a fault): the blur ignores the coverage map and covers the whole target (what dropping the two reductions would give); measured, see RIM_MAP_OURS.md
  bool grid_byte = false;              // R: one byte of the expected grid is wrong: every blur draw fails the compare (verify bad) and the stream's blur stays
  bool no_verify = false;              // control: the per-draw compare of the stream's vertex bytes is skipped
  static bool Parse(const std::string& list, RimChainFaults& out, std::string& error);
};

class RimChainOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  RimChainOurs(rdr::Renderer& renderer, const RimChainFaults& faults, bool live, std::function<bool(uint32_t, uint32_t&, uint32_t&)> image_info);
  const char* Name() const override { return "rim_chain"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override;
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  bool NeedsFragmentBlock() const override { return true; }   // the pixel constants are passed on as bound
  std::string Report() const;
  std::string WindowDetail() const override { return Report(); }

  // The counters the check reads (fview prints Report(); tests read these).
  struct Counts {
    uint64_t decisions = 0, not_ours = 0;
    uint64_t reduce1 = 0, reduce2 = 0, blur = 0;   // replaced, by draw
    uint64_t variant = 0, geometry = 0, constants = 0, texture = 0, alpha = 0, size = 0;   // kept, by reason
    uint64_t verified = 0, verify_bad = 0, size_unchecked = 0;
    uint64_t frames = 0;
  };
  const Counts& counts() const { return n_; }

 private:
  uint32_t BlurDestCaptureId(const rdr_adapter::LoadedFrame& frame, uint32_t draw_index) const;   // the capture id the frame's copy after the blur writes (223); 0 if none

  rdr::Renderer& renderer_;
  RimChainFaults faults_;
  bool live_ = false;
  std::function<bool(uint32_t, uint32_t&, uint32_t&)> image_info_;
  uint16_t reduce_program_ = 0, blur_program_ = 0, layout_ = 0;   // the quad and the grid draw without vertex buffers: one empty layout
  // The constants of the draw being replaced (the binder copies them before the next Replace call).
  alignas(16) float quad_vs_[12] = {};
  alignas(16) float grid_vs_[8] = {};
  alignas(16) uint8_t fs_[512] = {};
  Counts n_;
};

}  // namespace rdr_scene
