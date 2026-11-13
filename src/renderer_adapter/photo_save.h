// lane 201 (S101, GATE_FAULTS row 31, "Photo-mode save failed"): the pure parts of photo mode's save with the new renderer.
//   Stretch / ShownSize  the saved picture is the picture the player SEES. With the screen-shape setting (ultrawide, a phone's 20:9) the
//                        reader draws the scene anamorphic (squeezed by gpu_wide_factor k / gpu_tall_factor t) and the present stretches
//                        it back to the window (renderer.cpp RecordPresent, present_fill); a PNG of the squeezed source looked wrong, so
//                        the save stretches it the same way (bilinear). DLSS's output is already the shown size and is not stretched.
//   AfterAttempt         a request is kept until a presented frame has been written, for kMaxTries presented frames; each failure is
//                        named (Why), so a missing photo is never silent.
// No includes beyond the standard library; unit-tested by src/renderer_adapter/photo_save_test.cpp
// (clang++ -std=c++17 -I src/renderer_adapter -I src/renderer/player src/renderer_adapter/photo_save_test.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rdr_adapter::photo {

enum class Result { Ok, NoPicture, ReadFailed, WriteFailed };
enum class Action { Done, Retry, GiveUp };

inline constexpr uint32_t kMaxTries = 90;      // presented frames (1.5 s at 60): a request that meets a missing picture waits this long
inline constexpr uint32_t kMaxReadTries = 3;   // a failed GPU read-back costs a device wait each time: three, not ninety

inline const char* Why(Result r) {
  switch (r) {
    case Result::Ok: return "written";
    case Result::NoPicture: return "nothing presented yet (or the presented picture was released)";
    case Result::ReadFailed: return "the picture could not be read back from the GPU";
    case Result::WriteFailed: return "the PNG file could not be opened or written (folder, name or disk)";
  }
  return "?";
}

// One attempt's outcome -> what the render thread does with the request. `tries` counts the failed attempts so far.
// Only "no picture yet" is worth waiting for (the next presented frame may have one). A write failure (folder, disk, name) repeats
// itself, so it is final at once; a read-back failure gets kMaxReadTries.
inline Action AfterAttempt(Result r, uint32_t& tries) {
  if (r == Result::Ok) return Action::Done;
  ++tries;
  if (r == Result::WriteFailed) return Action::GiveUp;
  if (r == Result::ReadFailed) return tries >= kMaxReadTries ? Action::GiveUp : Action::Retry;
  return tries >= kMaxTries ? Action::GiveUp : Action::Retry;
}

// Before an attempt: photo mode hides the HUD while it is on (gpu_hide_hud); once that is off the request is stale and a later frame
// would be a gameplay frame with the HUD, so it is dropped.
inline Action Admit(bool hud_hidden) { return hud_hidden ? Action::Retry : Action::GiveUp; }

// A path's UTF-8 bytes for the log (path.string() throws on names the ANSI code page cannot hold).
inline std::string Utf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// The size the picture is shown at, from the source size and the screen-shape factors (1 = none). Never below 1 px, never absurd.
inline void ShownSize(uint32_t w, uint32_t h, float wide, float tall, uint32_t& out_w, uint32_t& out_h) {
  const float k = (std::isfinite(wide) && wide > 1.001f && wide < 8.0f) ? wide : 1.0f;
  const float t = (std::isfinite(tall) && tall > 1.001f && tall < 8.0f) ? tall : 1.0f;
  out_w = std::max<uint32_t>(1, uint32_t(std::floor(float(w) * k + 0.5f)));
  out_h = std::max<uint32_t>(1, uint32_t(std::floor(float(h) * t + 0.5f)));
}

// Bilinear resample of tightly packed RGBA8 (pixel centres aligned, edges clamped), as the present's stretch does.
inline std::vector<uint8_t> Stretch(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h) {
  std::vector<uint8_t> out(size_t(out_w) * out_h * 4);
  if (!w || !h || !out_w || !out_h) return out;
  struct Tap { uint32_t x0, x1; float fx; };
  std::vector<Tap> taps(out_w);  // the x positions and weights, once per photo, not per row
  for (uint32_t x = 0; x < out_w; ++x) {
    const float sx = std::clamp((float(x) + 0.5f) * float(w) / float(out_w) - 0.5f, 0.0f, float(w - 1));
    taps[x].x0 = uint32_t(sx), taps[x].x1 = std::min(taps[x].x0 + 1, w - 1), taps[x].fx = sx - float(taps[x].x0);
  }
  for (uint32_t y = 0; y < out_h; ++y) {
    const float sy = std::clamp((float(y) + 0.5f) * float(h) / float(out_h) - 0.5f, 0.0f, float(h - 1));
    const uint32_t y0 = uint32_t(sy), y1 = std::min(y0 + 1, h - 1);
    const float fy = sy - float(y0);
    for (uint32_t x = 0; x < out_w; ++x) {
      const uint32_t x0 = taps[x].x0, x1 = taps[x].x1;
      const float fx = taps[x].fx;
      for (uint32_t c = 0; c < 4; ++c) {
        const float a = rgba[(size_t(y0) * w + x0) * 4 + c], b = rgba[(size_t(y0) * w + x1) * 4 + c];
        const float d = rgba[(size_t(y1) * w + x0) * 4 + c], e = rgba[(size_t(y1) * w + x1) * 4 + c];
        const float v = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
        out[(size_t(y) * out_w + x) * 4 + c] = uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
      }
    }
  }
  return out;
}

}  // namespace rdr_adapter::photo
