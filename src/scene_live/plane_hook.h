// The plane shadow chain, the game-side half (Session 101, lane 139 = SH3 slice 2; docs/renderer/SHADOW_PLANE.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only core: no game or SDK includes. The game plumbing (guest reads, registers, the three calls) is plane_glue.h; tools/shadow/plane_unit.cpp and fview's oracle drive this same code.
//
// Where the values are uploaded: the table plane_sites.h (every row cited from a listing). The module's constants come from sub_827EC600 (ten op-21 calls with exact return addresses), the plane position
// from sub_827EE190 (one call per proxy kind, before its draws), the blur's from sub_827ECD10. The tracker keeps a register file per chain (the recorder's constant state persists between draws, so does
// ours) in which each row remembers the frame that wrote it, and snapshots it when a draw is issued: OnPartDraw (the mask's part draw, LR 0x82BF4D3C) and OnBlurDraw (the blur's op-15 draw, LR 0x827EEC88).
// "Present" means written THIS frame: a row from an earlier frame is never reported (a frame without the module's upload has no module block). Not owned (stated, plane_sites.h): EyePos c72, the palette
// c76.., the blur's pixel constant c34. Read only; never writes guest memory or a register. One drawing thread feeds the tracker; Take runs on the writer's thread (one mutex around the pending lists).
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

#include "../renderer_scene/scene_record.h"
#include "plane_sites.h"

namespace rdr_scene::live::plane {

inline constexpr uint32_t kMaxPending = 512;                // draws held between two Emits (about 40 a frame in the captures)
inline constexpr uint32_t kFileRows = 64;                   // registers 0..63 cover every tracked row (0..16 and 33..40)

struct Stats {
  uint64_t calls = 0, rows_mask = 0, rows_blur = 0, frames = 0, mask_draws = 0, blur_draws = 0, unreadable = 0, orphans = 0, overflow = 0, incomplete_frames = 0, dropped_tables = 0;
};
using ReadBytes = std::function<bool(uint32_t addr, void* dst, size_t n)>;

inline bool ReadRows(const ReadBytes& rd, uint32_t addr, float* dst, size_t floats) {
  uint8_t buf[16 * 4];
  if (floats * 4 > sizeof buf || !rd(addr, buf, floats * 4)) return false;
  for (size_t i = 0; i < floats; ++i) {
    const uint32_t v = (uint32_t(buf[4 * i]) << 24) | (uint32_t(buf[4 * i + 1]) << 16) | (uint32_t(buf[4 * i + 2]) << 8) | buf[4 * i + 3];
    std::memcpy(&dst[i], &v, 4);
  }
  return true;
}

class Tracker {
 public:
  // Cheap test for the game-wide recorder hook: an exact set of return addresses (fifteen compares).
  static bool IsOurs(uint32_t lr) { return FindSite(lr) != nullptr; }

  // One call of the op-21 recorder whose return address is ours: r4 = register, r5 = rows pointer, r6 = row count.
  void OnConstants(uint32_t lr, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t frame, const ReadBytes& rd) {
    Bump(&Stats::calls);
    const Site* s = FindSite(lr);
    if (s ? (r4 != s->reg || r6 != s->rows) : !accept_any_lr_) return Bump(&Stats::orphans);   // not the pattern the listing shows: nothing recorded
    const Chain chain = s ? s->chain : Chain::Mask;   // (the test-only accept_any_lr_ treats a foreign call as a mask upload: the OLD range-based filter, for the plant foreign_upload_between)
    File& f = chain == Chain::Mask ? mask_ : blur_;
    if (r6 == 0 || r6 > 4 || r4 + r6 > kFileRows) return Bump(&Stats::orphans);
    float rows[4 * 4];
    if (!ReadRows(rd, r5, rows, size_t(r6) * 4)) return Bump(&Stats::unreadable);
    for (uint32_t i = 0; i < r6; ++i) {
      std::memcpy(f.v[r4 + i], rows + 4 * i, 16);
      f.stamp[r4 + i] = frame + 1;   // (+1: 0 means never written, also for frame 0)
    }
    Bump(chain == Chain::Mask ? &Stats::rows_mask : &Stats::rows_blur);
  }

  // lane 226 follow-up (S1): the plane PASS ENTRY of sub_82BF22C8 (the caster hook's entry). The register file the tracker keeps is fed by the module's and the proxy kind's uploads (sub_827EC600 from the
  // module draw sub_827EED58, sub_827EE190's c7 before the proxy's draw through vtable +136: plane_sites.h), all OUTSIDE the pass call, so the frame block and c7 exist at the entry whether or not the call
  // then runs. Emits the frame's block once (as OnPartDraw would) and returns c7 written THIS frame in `pos` (false = not written this frame). Without this a skipped call would leave the record with no
  // PlaneFrame (OnPartDraw is inside the call, at LR 0x82BF4D3C).
  bool OnPassEntry(uint32_t frame, float pos[4]) {
    const uint32_t now = frame + 1;
    std::lock_guard<std::mutex> lock(mu_);
    EmitFrameBlockLocked(frame, now);
    if (mask_.stamp[7] != now) return false;
    std::memcpy(pos, mask_.v[7], 16);
    return true;
  }

  // The mask's part draw (LR 0x82BF4D3C): r3 = the model object, r4 = the part index.
  void OnPartDraw(uint32_t object, uint32_t part, uint32_t frame) {
    const uint32_t now = frame + 1;
    std::lock_guard<std::mutex> lock(mu_);
    EmitFrameBlockLocked(frame, now);
    PlaneDraw d;
    d.kind = kPlaneMask, d.frame = frame, d.object = object, d.part = part, d.ordinal = Ordinal(now);
    if (mask_.stamp[7] == now) std::memcpy(d.plane_pos, mask_.v[7], 16), d.have |= kPlaneDrawHavePos;
    Push(d, &Stats::mask_draws);
  }

  // The blur's draw (LR 0x827EEC88).
  void OnBlurDraw(uint32_t frame) {
    const uint32_t now = frame + 1;
    std::lock_guard<std::mutex> lock(mu_);
    PlaneDraw d;
    d.kind = kPlaneBlur, d.frame = frame, d.ordinal = Ordinal(now);
    const bool ok = blur_.stamp[33] == now && blur_.stamp[35] == now && blur_.stamp[40] == now && blur_.stamp[36] == now && blur_.stamp[37] == now && blur_.stamp[38] == now && blur_.stamp[39] == now;
    if (ok) {
      std::memcpy(d.blur_scale, blur_.v[33], 16);
      std::memcpy(d.blur_weight, blur_.v[35], 16);
      std::memcpy(d.blur_matrix, blur_.v[36], 64);
      std::memcpy(d.blur_point, blur_.v[40], 16);
      d.have |= kPlaneDrawHaveBlur;
    }
    Push(d, &Stats::blur_draws);
  }

  // The frames and draws gathered since the last call (oldest first).
  void Take(std::vector<PlaneFrame>& frames, std::vector<PlaneDraw>& draws) {
    std::lock_guard<std::mutex> lock(mu_);
    frames.swap(frames_);
    draws.swap(draws_);
    frames_.clear();
    draws_.clear();
  }
  Stats GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
  }
  void TestAcceptAnyLr(bool on) { accept_any_lr_ = on; }   // TEST ONLY (the plant foreign_upload_between): the original range-style filter
  void CountDropped() {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.dropped_tables;
  }

 private:
  struct File {
    float v[kFileRows][4] = {};
    uint32_t stamp[kFileRows] = {};   // frame + 1 of the last write; 0 = never
  };
  void EmitFrameBlockLocked(uint32_t frame, uint32_t now) {   // (mu_ held) the frame's module block, once per frame, from the rows written THIS frame
    if (frames_emitted_for_ == now) return;
    frames_emitted_for_ = now;
    PlaneFrame pf;
    pf.frame = frame;
    bool fade = true;
    for (uint32_t r = 0; r < 7; ++r) fade = fade && mask_.stamp[r] == now;
    if (fade) {
      for (uint32_t j = 0; j < 7; ++j) pf.fade[j] = mask_.v[j][0];
      for (uint32_t j = 1; j < 4; ++j) pf.fade[6 + j] = mask_.v[6][j];
      pf.have |= kPlaneHaveFade;
    }
    if (mask_.stamp[8] == now) std::memcpy(pf.plane_normal, mask_.v[8], 16), pf.have |= kPlaneHaveNormal;
    bool fit = true, cam = true;
    for (uint32_t r = 0; r < 4; ++r) fit = fit && mask_.stamp[9 + r] == now, cam = cam && mask_.stamp[13 + r] == now;
    if (fit) std::memcpy(pf.fit_matrix, mask_.v[9], 64), pf.have |= kPlaneHaveFit;
    if (cam) std::memcpy(pf.camera, mask_.v[13], 64), pf.have |= kPlaneHaveCamera;
    if (pf.have != kPlaneHaveAll) ++stats_.incomplete_frames;
    ++stats_.frames;
    frames_.push_back(pf);
  }
  uint32_t Ordinal(uint32_t now) {   // (mu_ held)
    if (ord_for_ != now) ord_for_ = now, ord_ = 0;
    return ord_++;
  }
  void Push(const PlaneDraw& d, uint64_t Stats::*counter) {   // (mu_ held)
    if (draws_.size() >= kMaxPending) {
      ++stats_.overflow;
      return;
    }
    draws_.push_back(d);
    ++(stats_.*counter);
  }
  void Bump(uint64_t Stats::*field) {
    std::lock_guard<std::mutex> lock(mu_);
    ++(stats_.*field);
  }
  mutable std::mutex mu_;
  File mask_, blur_;
  uint32_t frames_emitted_for_ = 0, ord_for_ = 0, ord_ = 0;
  bool accept_any_lr_ = false;
  std::vector<PlaneFrame> frames_;
  std::vector<PlaneDraw> draws_;
  Stats stats_;
};

}  // namespace rdr_scene::live::plane
