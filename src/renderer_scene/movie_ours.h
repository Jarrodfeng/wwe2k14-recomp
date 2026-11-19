// MovieOurs: the game's movie planes (T_SpriteYUVAlpha, call site 0x82BC7E50) drawn by our own quad and our own YUV -> colour shader, in the stream draw's place
// (Session 101, lane 131; docs\renderer\MOVIE_OURS.md; the seam: src\renderer_adapter\draw_replace_hook.h, its `own` draw). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// What stays the game's: the Bink 2 decoder (Jarrod's decision, IDEAS.md "Video"): it writes the Y / Cb / Cr / alpha planes into guest textures, and the binder hands the
// stream draw those images in slots 0..3 as they were bound this frame. The hook does not copy, hold or re-upload a plane: it draws the very images the stream draw binds.
// What is ours: the quad (built by rdr_movie.vert from one rectangle read from the stream draw's four vertices, checked) and the conversion (rdr_movie.frag, a matrix given
// as data: the game's own BT.601 limited-range coefficients). The stream draw's target, blend, depth, viewport and samplers are kept.
//
// No game or SDK includes. Everything runs on the render thread (the one that binds frames). No game files are read.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../scene_live/movie_planes.h"   // S103 P4: the planes from guest memory (store + upload)

namespace rdr_scene {

inline constexpr uint32_t kMovieCallSite = 0x82BC7E50u;   // sub_82BC7A10's draw call (T_SpriteYUVAlpha) [C: UI_PROMOTION_MAP.md:42, the l125 Bryan / menu captures]

// Planted faults (the acceptance test's twins: each must fail its exact check).
struct MovieFaults {
  bool stale_planes = false;   // the planes of the PREVIOUS claimed draw (the decoder double-buffers: last frame's images)
  bool stale_plane = false;    // S103 P4 (guest planes on): the planes of the PREVIOUS flush of the set are drawn (movie_planes.h Plants::stale_plane)
  bool bt601_vs_709 = false;   // the BT.709 matrix for BT.601 video
  bool no_alpha = false;       // the alpha plane ignored (alpha 1)
  bool swapped_uv = false;     // Cb and Cr exchanged (slots 1 and 2)
  bool half_pixel = false;     // the half-unit shift of the game's placement dropped (the quad sits half a unit off)
  bool uv_flip_v = false;      // the v corners exchanged (the picture upside down)
  bool green_bias = false;     // +0.6 / 255 on the green offset (a rounding-size error: only the exact-1-level ceiling can see it)
  bool no_alpha_test = false;  // the stream's alpha test not applied (a control: the test is invisible on this draw, see MOVIE_OURS.md)
  bool alpha_test_inverted = false;   // the stream's alpha test inverted: the pixels it keeps are discarded
  bool own_count_zero = false; // seam plant: an own draw with count 0 (the binder must refuse it, counted as mismatch)
  bool own_program_zero = false;   // seam plant: an own draw with program 0 (refused)
  static bool Parse(const std::string& list, MovieFaults& out, std::string& error);
};

// S103 P4: the plane images' sink for movie_planes.h PlaneImages: one 8-bit channel images in the renderer (CreateImage / UpdateImage / DestroyImage may be called from any thread).
struct RendererSink {
  rdr::Renderer* renderer = nullptr;
  uint32_t Create(uint32_t w, uint32_t h) const {
    rdr::ImageDesc d;
    d.width = uint16_t(w), d.height = uint16_t(h), d.format = rdr::Format::R8, d.usage = rdr::kUsageSampled;
    return uint32_t(renderer->CreateImage(d));
  }
  void Update(uint32_t id, const uint8_t* data, size_t bytes) const { renderer->UpdateImage(rdr::ImageId(id), 0, 0, data, bytes); }
  void Destroy(uint32_t id) const { renderer->DestroyImage(rdr::ImageId(id)); }
};

class MovieOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  MovieOurs(rdr::Renderer& renderer, const MovieFaults& faults);
  const char* Name() const override { return "movie"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override;
  bool FrameReady(const rdr_adapter::LoadedFrame&) override { return true; }   // needs no record and no file
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  // S103 P4: what the caller knows about the NEXT draw (the 2D row's plane_set / flush stamp, the Y image's size); each is optional and used once. Guest planes only.
  void SetDrawHint(uint32_t plane_set, uint32_t y_w, uint32_t y_h, uint32_t flush_seq) { hint_ = live::movieplanes::DrawHint{plane_set, y_w, y_h, flush_seq}; }   // (today nothing calls it: Replace fills the size from the stream slot-0 image record and the set from the 2D row, review F1)
  std::string Report() const;
  std::string WindowDetail() const override { return Report(); }   // S101 lane 131 (review F1): the kept-by-reason line reaches the live log after every census window   // "decisions D, replaced R, ..." (fview prints it after the seam's census line; live logs it with it)

 private:
  MovieFaults faults_;
  live::movieplanes::PlaneImages<RendererSink> images_;   // S103 P4: the four plane images per display set, ours
  live::movieplanes::DrawHint hint_;
  uint16_t program_ = 0, layout_ = 0;
  // The plane slots of the draw in the current frame, and of the last frame before it that had one (the stale_planes plant draws those). A frame bound again (a frame split at a
  // readback, fview's repeated binds) keeps the same guest frame number and shifts nothing.
  rdr::TextureSlot current_[4], previous_[4];
  uint64_t current_frame_ = 0;
  bool have_current_ = false, have_previous_ = false;
  // The constants of the draw being replaced (the binder pushes them before the next Replace call).
  // S103 I1 (--quads-2d-rows): capture image id -> the renderer's slot, learned from every claimed draw; the plane ids of this frame's row and of the frame before it (the stale_plane plant)
  std::unordered_map<uint32_t, rdr::TextureSlot> row_map_;
  uint32_t row_cur_[4] = {}, row_prev_[4] = {};
  uint64_t row_frame_ = 0;
  bool row_have_ = false, row_have_prev_ = false;
  alignas(16) float vs_[12] = {};
  alignas(16) float fs_[20] = {};
  struct Counts {
    uint64_t decisions = 0, replaced = 0, variant = 0, geometry = 0, quad = 0, constants = 0, alpha_test = 0, planes = 0, guest_replaced = 0, guest_none = 0, guest_refused = 0, guest_hint_set = 0, guest_hint_size = 0;
    float last_rect[4] = {};
    float last_uv[4] = {};
  } n_;
};

}  // namespace rdr_scene
