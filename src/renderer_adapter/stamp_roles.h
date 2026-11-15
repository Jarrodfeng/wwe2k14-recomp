// S102 G6: which GPU class (api.h GpuClass) a capture image's passes and copies are stamped under: the role -> class table, the role the stamps derive for images the capture
// does not name, and the scene-size pick behind it. Header-only, no game or SDK includes (tools\renderer\stamp_roles_test.cpp is its unit test; tools\renderer\gpu_class_lint.py keeps the
// python copies of the table in step with it). Names and classes only: no picture reads any of it.
#pragma once
#include <cstdint>
#include <vector>

#include "../renderer/api.h"
#include "adapter_frame.h"
#include "names.h"

namespace rdr_adapter::stamp_roles {

// Test plants (rplay --gpu-class-plant): 1 scene named as post, 2 copies' destinations unnamed, 3 frame buffer unnamed.
// No `default:` and every SurfaceRole listed: a new role is a compile error here (-Werror=switch) until it is given a class.
inline uint8_t GpuClassOfRole(uint8_t role, int plant = 0) {
  switch (SurfaceRole(role)) {
    case SurfaceRole::SceneColor: case SurfaceRole::SceneDepth: return plant == 1 ? rdr::kGpuPost : rdr::kGpuColor;
    case SurfaceRole::ShadowMap: return rdr::kGpuShadow;
    case SurfaceRole::RimMap: return rdr::kGpuRim;
    case SurfaceRole::PostStep: return rdr::kGpuPost;
    case SurfaceRole::AtlasTarget: return rdr::kGpuCards;
    case SurfaceRole::Present: return rdr::kGpuOutput;
    case SurfaceRole::MenuPicture: case SurfaceRole::TronPicture: case SurfaceRole::Ui: return rdr::kGpuUi;
    case SurfaceRole::FrameBuffer: return plant == 3 ? rdr::kGpuOther : rdr::kGpuFrame;
    case SurfaceRole::CopyDest: return plant == 2 ? rdr::kGpuOther : rdr::kGpuCopies;
    case SurfaceRole::LightStrip: return rdr::kGpuPrelight;
    case SurfaceRole::FeedbackMap: return rdr::kGpuFeedback;
    case SurfaceRole::Unnamed: case SurfaceRole::Other: return rdr::kGpuOther;  // what no role names: the gate holds it under 10 % on Bryan
    case SurfaceRole::Count: return rdr::kGpuOther;
  }
  return rdr::kGpuOther;  // (a value outside the enum)
}

// The game's screen images are 16:9 (the height within 16 pixels): the same rule as reader.h ScreenShapedRule, copied here because that header holds game hooks (tools\renderer\gpu_class_lint.py
// checks the two bodies stay equal).
inline bool ScreenShaped16x9(uint32_t width, uint32_t height) {
  if (width < 64) return false;
  const int64_t diff = int64_t(width) * 9 - int64_t(height) * 16;
  return diff >= -16 * 16 && diff <= 16 * 16;
}

// The scene target's size: among the SceneColor records that are screen-shaped (the scene creator also makes the 736x1280 roster picture and the editor keeps spare scene targets),
// the largest, the newest (highest id) on a tie. false = none yet (a menu before any scene).
inline bool PickSceneSize(const std::vector<ImageRecord>& records, uint16_t& w, uint16_t& h) {
  const ImageRecord* best = nullptr;
  for (const ImageRecord& r : records) {
    if (SurfaceRole(r.role) != SurfaceRole::SceneColor || !ScreenShaped16x9(r.width, r.height)) continue;
    if (!best || uint32_t(r.width) * r.height > uint32_t(best->width) * best->height || (uint32_t(r.width) * r.height == uint32_t(best->width) * best->height && r.id > best->id)) best = &r;
  }
  if (!best) return false;
  w = best->width, h = best->height;
  return true;
}

// The role a record has FOR THE STAMPS. A declared role stands. An image with none (unnamed, or the old catch-all `other`) is named from facts the capture holds: written by resolve copies
// (ImageFlags kImageCopyDest, a fact the reader sets) = copy_dest; a render-target surface the size of the scene target that is not a copy destination = frame_buffer (post, sprites, text,
// particles and the movie draw into it; the present copies from it). Derived, not declared: [U] which creator makes it (db draw_submit.json calls 0x82BC39E0 a colour + texture pair, the
// Bryan capture's images 8 / 9 are colour + depth with no creator row), so no creator row says frame_buffer.
inline uint8_t RoleForStamps(const ImageRecord& r, uint16_t scene_w, uint16_t scene_h) {
  const SurfaceRole role = SurfaceRole(r.role);
  if (role != SurfaceRole::Unnamed && role != SurfaceRole::Other) return r.role;
  if (r.flags & kImageCopyDest) return uint8_t(SurfaceRole::CopyDest);
  if (scene_w && r.source == kSourceSurface && (r.usage & rdr::kUsageTarget) && r.width == scene_w && r.height == scene_h) return uint8_t(SurfaceRole::FrameBuffer);
  return r.role;
}

}  // namespace rdr_adapter::stamp_roles
