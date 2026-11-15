// S102 R21-B (docs\renderer\STAGE_SPLIT.md): rplay --stage-selftest. Synthetic frames (no capture) that carry, every frame, each hazard the two-stage
// render thread must survive, with the exact expected pixels known in advance:
//   - a NEW texture T_f (1024x1024, uploaded with colour A_f) copied into the output P, then RELEASED right after the frame is handed over (live.cpp
//     releases after the bind): hazard 5 (release-after-record). Twin split_no_release_defer: T_f is destroyed by frame f's own Render.
//   - a SHARED texture S re-uploaded every frame with colour B_f and copied into P: hazard 4 (upload ordering). Twin split_no_seal: frame f applies
//     frame f+1's upload of S.
//   - each frame's own copy list in the slot's builder: twins split_shared_builder (one builder: frame f records frame f+1's ops) and split_queue_unbounded
//     (the bind stage runs ahead: more than 2 frames in flight).
//   - an app-overlay texture made each frame the way live.cpp makes it (review F1: MakeOverlayTexture on the bind stage before the hand-over, so its
//     upload is sealed with the frame) must hold colour C_f when frame f has rendered. Twin split_overlay_in_hook: made in the record stage's hook
//     (after the seal): the frame renders before its upload.
//   - the uploads are 4 MB each against a staging cap the gate sets to 8 MB (--staging-cap-mb 8): hazard 6. Twin split_no_producer: the bind stage
//     waits (2 s a call) for room only the record stage frees: a stall the test times (any UpdateImage over 1 s fails).
// After each frame (on the stage that rendered it) P is read back: pixel (0,0) must be A_f and (8,0) must be B_f. The record stage holds each frame
// until the bind stage has handed over the next one (stage_split::g_latch_us), so every hazard is exercised on every frame, not by luck.
// Exit 0 only when every frame matched, no frame was lost, frames in flight stayed <= 2 and no upload stalled; prints PASS / FAIL lines.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "../../renderer_adapter/stage_split.h"
#include "../frame_builder.h"
#include "../problem.h"
#include "../renderer.h"

namespace rdr {

inline int StageSelfTest(Renderer& renderer, bool split, int frames) {
  namespace ss = rdr_adapter::stage_split;
  g_problem_nonfatal.store(true);  // a planted twin's copy of a destroyed image is counted (copy_dead_image), not a stop
  const auto dead_copies = [] {
    ProblemCount counts[kMaxProblemNames];
    const int n = ProblemCounts(counts, kMaxProblemNames);
    uint64_t dead = 0;
    for (int i = 0; i < n && i < kMaxProblemNames; ++i)
      if (counts[i].name && std::strcmp(counts[i].name, "copy_dead_image") == 0) dead = counts[i].count;
    return dead;
  };
  constexpr uint32_t kBig = 1024;
  const auto colour = [](uint32_t f, uint32_t which) -> uint32_t {  // RGBA8 little-endian word, alpha 255, distinct per frame and per texture
    const uint32_t r = (f * 37u + which * 101u + 11u) & 0xFF, g = (f * 53u + which * 7u + 29u) & 0xFF, b = (f * 17u + which * 151u + 3u) & 0xFF;
    return r | g << 8 | b << 16 | 0xFFu << 24;
  };
  ImageDesc out_desc;
  out_desc.width = 64, out_desc.height = 8, out_desc.format = Format::RGBA8;
  out_desc.usage = uint8_t(kUsageSampled | kUsageTarget | kUsageLasting);
  const ImageId out = renderer.CreateImage(out_desc);
  ImageDesc big;
  big.width = big.height = uint16_t(kBig), big.format = Format::RGBA8, big.usage = kUsageSampled;
  const ImageId shared = renderer.CreateImage(big);
  std::vector<uint32_t> fill(size_t(kBig) * kBig);
  std::atomic<int> bad{0}, read_fail{0}, checked{0};
  std::atomic<uint32_t> first_bad{~0u};
  std::vector<ImageId> overlay(size_t(frames > 0 ? frames : 1), kNoImage);   // review F1: frame f's overlay texture
  std::atomic<int> overlay_bad{0};
  const bool overlay_in_hook = ss::g_plant.load() == ss::kPlantOverlayInHook;
  const auto make_overlay = [&](uint32_t f) {
    const uint32_t c = colour(f, 2);
    const uint32_t texels[16] = {c, c, c, c, c, c, c, c, c, c, c, c, c, c, c, c};
    overlay[f] = renderer.MakeOverlayTexture(4, 4, reinterpret_cast<const uint8_t*>(texels));
  };
  const auto check = [&](uint32_t f) {  // after frame f's Render, on the thread that rendered it
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!renderer.ReadImage(ImageRef{out, 0, 0, 0}, rgba, w, h) || w < 16) {
      ++read_fail;
      return;
    }
    uint32_t a = 0, b = 0;
    std::memcpy(&a, rgba.data(), 4);
    std::memcpy(&b, rgba.data() + 8 * 4, 4);
    if (a != colour(f, 0) || b != colour(f, 1)) {
      if (bad.fetch_add(1) < 3) std::printf("[stage_selftest] frame %u: P(0,0)=%08X want %08X, P(8,0)=%08X want %08X\n", f, a, colour(f, 0), b, colour(f, 1));
      uint32_t want = ~0u;
      first_bad.compare_exchange_strong(want, f);
    }
    {   // review F1: the overlay texture made for this frame holds its colour once the frame has rendered
      std::vector<uint8_t> ov;
      uint32_t ow = 0, oh = 0;
      uint32_t got = 0;
      if (overlay[f] == kNoImage || !renderer.ReadImage(ImageRef{overlay[f], 0, 0, 0}, ov, ow, oh) || ov.size() < 4) got = 1;
      else std::memcpy(&got, ov.data(), 4);
      if (got != colour(f, 2) && overlay_bad.fetch_add(1) < 3) std::printf("[stage_selftest] frame %u: overlay texture %08X want %08X\n", f, got, colour(f, 2));
      renderer.DestroyImage(overlay[f]);
    }
    ++checked;
  };
  std::unique_ptr<ss::RecordStage> stage;
  if (split) {
    ss::g_latch_us.store(200000);
    stage = std::make_unique<ss::RecordStage>(
        renderer, [&](const Frame& frame, const ss::Packet&) { if (overlay_in_hook) make_overlay(uint32_t(frame.tick)); },   // the twin: made after the seal
        [&](const Frame& frame, const ss::Packet&) { check(uint32_t(frame.tick)); });
  }
  FrameBuilder own;
  double worst_upload_ms = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int fi = 0; fi < frames; ++fi) {
    const uint32_t f = uint32_t(fi);
    const ImageId fresh = renderer.CreateImage(big);  // T_f
    const auto u0 = std::chrono::steady_clock::now();
    std::fill(fill.begin(), fill.end(), colour(f, 0));
    renderer.UpdateImage(fresh, 0, 0, fill.data(), fill.size() * 4);
    std::fill(fill.begin(), fill.end(), colour(f, 1));
    renderer.UpdateImage(shared, 0, 0, fill.data(), fill.size() * 4);  // S, rewritten in place every frame
    const double up_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();
    if (up_ms > worst_upload_ms) worst_upload_ms = up_ms;
    FrameBuilder& b = stage ? stage->Acquire() : own;
    b.Begin(f);
    CopyOp c{};
    c.source = ImageRef{fresh, 0, 0, 0};
    c.dest = ImageRef{out, 0, 0, 0};
    c.source_rect = Rect{0, 0, 8, 8};
    c.dest_rect = Rect{0, 0, 8, 8};
    b.Copy(c);
    c.source.image = shared;
    c.dest_rect = Rect{8, 0, 8, 8};
    b.Copy(c);
    if (stage) {
      if (!overlay_in_hook) make_overlay(f);   // live.cpp's TakeOverlay: before Submit seals the frame
      stage->Submit(ss::Packet{});
    } else {
      make_overlay(f);
      renderer.RenderFrame(b.Finish());
      check(f);
    }
    renderer.DestroyImage(fresh);  // released after the hand-over (live.cpp:919's order): must outlive frame f's recording
  }
  if (stage) {
    stage->EndOfInput();
    stage->Stop();
  }
  renderer.WaitIdle();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  int fails = 0;
  const auto say = [&](bool ok, const char* text) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", text), fails += ok ? 0 : 1; };
  char line[256];
  std::snprintf(line, sizeof line, "%s: %d of %d frames checked, %d wrong (first %d), %d unreadable", split ? "split" : "sync", checked.load(), frames, bad.load(),
                first_bad.load() == ~0u ? -1 : int(first_bad.load()), read_fail.load());
  say(checked.load() == frames && bad.load() == 0 && read_fail.load() == 0, line);
  std::snprintf(line, sizeof line, "overlay textures wrong when their frame rendered: %d (review F1, must be 0)", overlay_bad.load());
  say(overlay_bad.load() == 0, line);
  if (stage) {
    std::snprintf(line, sizeof line, "frames in flight max %llu (must be <= 2)", static_cast<unsigned long long>(stage->in_flight_max()));
    say(stage->in_flight_max() <= 2, line);
  }
  std::snprintf(line, sizeof line, "slowest frame's two uploads %.1f ms (a wait for staging room would be >= 1000)", worst_upload_ms);
  say(worst_upload_ms < 1000.0, line);
  std::snprintf(line, sizeof line, "copies of a destroyed image %llu (must be 0)", static_cast<unsigned long long>(dead_copies()));
  say(dead_copies() == 0, line);
  std::snprintf(line, sizeof line, "validation errors %llu", static_cast<unsigned long long>(renderer.validation_errors()));
  say(renderer.validation_errors() == 0, line);
  std::printf("[rplay][stage_selftest] %s (%s, %d frames, %.1f s)\n", fails ? "FAIL" : "PASS", split ? "split" : "sync", frames, secs);
  return fails ? 1 : 0;
}

}  // namespace rdr
