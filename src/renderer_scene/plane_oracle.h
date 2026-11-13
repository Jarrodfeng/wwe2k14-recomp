// The plane chain's capture-side oracle: a capture's plane draws -> the register uploads the game would have made -> the SHIPPED writer's tracker -> the record entries
// (Session 101, lane 139; docs/renderer/SHADOW_PLANE.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game, SDK or renderer includes: fview's plane check and
// tools/shadow/plane_unit.cpp both run THIS code, so the record the hook plays in the picture proof is the record the tracker produces from guest bytes.
//
// The input file comes from tools/shadow/plane_oracle.py (the stream's plane-chain constants as the capture holds them; numbers only, under out\, never committed). What is faked: the guest's
// memory (rows laid out big-endian at a stack address, as the recorder's r5 points to them) and the call order (module rows once per frame, EyePos when it changes, the palette per draw, the
// blur's four uploads); what is NOT faked is the tracker and the core. No mesh is read by this writer (lane 136's buffer-object note does not apply: only constants are read).
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../scene_live/plane_hook.h"
#include "plane_shadow_core.h"
#include "record_io.h"

namespace rdr_scene::plane::oracle {

inline constexpr uint32_t kMagic = 0x454E4C50u, kVersion = 1;

struct MaskDraw {
  uint32_t object = 0, part = 0;
  float rows[17u][4] = {};   // c0..c16
  float eye[4] = {};
  std::vector<std::array<float, 4>> pal;
};
struct Frame {
  uint32_t capture_frame = 0, guest_frame = 0;
  std::vector<MaskDraw> masks;
  std::vector<std::array<float, 4 * 8u>> blurs;
};
struct File {
  std::vector<Frame> frames;
};

inline bool Load(const std::string& path, File& out, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return error = "cannot open " + path, false;
  const std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t at = 0;
  const auto take = [&](void* dst, size_t n) {
    if (at + n > b.size()) return false;
    std::memcpy(dst, b.data() + at, n);
    at += n;
    return true;
  };
  uint32_t head[3];
  if (!take(head, 12) || head[0] != kMagic || head[1] != kVersion) return error = path + ": not a plane oracle file (magic / version)", false;
  out.frames.clear();
  out.frames.reserve(head[2]);
  for (uint32_t i = 0; i < head[2]; ++i) {
    Frame f;
    uint32_t h[4];
    if (!take(h, 16) || h[2] > 256 || h[3] > 16) return error = path + ": truncated or implausible frame " + std::to_string(i), false;
    f.capture_frame = h[0], f.guest_frame = h[1];
    for (uint32_t m = 0; m < h[2]; ++m) {
      MaskDraw d;
      uint32_t pn = 0;
      if (!take(&d.object, 4) || !take(&d.part, 4) || !take(d.rows, sizeof d.rows) || !take(d.eye, 16) || !take(&pn, 4) || pn > 63u) return error = path + ": truncated mask draw", false;
      d.pal.resize(pn);
      if (pn && !take(d.pal.data(), size_t(pn) * 16)) return error = path + ": truncated palette", false;
      f.masks.push_back(std::move(d));
    }
    for (uint32_t m = 0; m < h[3]; ++m) {
      std::array<float, 4 * 8u> r;
      if (!take(r.data(), sizeof(float) * r.size())) return error = path + ": truncated blur draw", false;
      f.blurs.push_back(r);
    }
    out.frames.push_back(std::move(f));
  }
  return true;
}

class FakeGuest {
 public:
  void PutFloats(uint32_t addr, const float* f, size_t count) {   // big-endian words
    std::vector<uint8_t>& r = regions_[addr];
    r.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
      uint32_t v;
      std::memcpy(&v, &f[i], 4);
      r[4 * i] = uint8_t(v >> 24), r[4 * i + 1] = uint8_t(v >> 16), r[4 * i + 2] = uint8_t(v >> 8), r[4 * i + 3] = uint8_t(v);
    }
  }
  bool Read(uint32_t addr, void* dst, size_t n) const {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return false;
    --it;
    if (addr < it->first || uint64_t(addr) + n > uint64_t(it->first) + it->second.size()) return false;
    std::memcpy(dst, it->second.data() + (addr - it->first), n);
    return true;
  }

 private:
  std::map<uint32_t, std::vector<uint8_t>> regions_;
};

inline constexpr uint32_t kModule = 0x60000000u, kStack = 0x7F000000u;   // the plane module's address in the fake guest; a stack area

// Test-only misbehaviours of the DRIVER (the plants of the unit check): the tracker and the core stay the shipped code.
struct Plant {
  uint32_t drop_lr = 0;            // writer_lr_missing: the hook misses this call site (the call is not delivered)
  bool upload_after_snapshot = false;   // upload_after_snapshot: the first draw of a frame is snapshotted BEFORE the module's uploads
  bool foreign_ignored = false;    // the CONTROL of foreign_upload_between: the same foreign call, delivered to the shipped (exact-LR) tracker, which must ignore it
  float* entry_pos = nullptr;      // lane 226 follow-up (S1): when set, the PASS ENTRY snapshot (Tracker::OnPassEntry) runs INSTEAD of the part draw (a skipped call); the c7 it returns is stored here, entry_ok says whether it was written
  bool* entry_ok = nullptr;
  bool foreign_between = false;    // foreign_upload_between: a call from a foreign return address writes c13 between the module's uploads and the snapshot; the plant also makes the tracker accept it
};

// Lays the module's memory out as the game holds it (the 10-float fade array at +68 that sub_827EC600 reads in overlapping rows, the camera block at +1072, the fit block at +1312) and replays the
// rows of kSites: ONE source of truth (plane_sites.h), the very table the tracker accepts.
inline void PutModule(FakeGuest& g, const MaskDraw& m) {
  float fade[10];
  for (int j = 0; j < 7; ++j) fade[j] = m.rows[j][0];
  for (int j = 1; j < 4; ++j) fade[6 + j] = m.rows[6][j];
  g.PutFloats(kModule + 68, fade, 10);
  g.PutFloats(kModule + 1072, m.rows[13], 16);
  g.PutFloats(kModule + 1312, m.rows[9], 16);
  g.PutFloats(kStack + 80, m.rows[8], 4);    // sub_827EC600's plane normal (r1+80)
}
inline uint32_t SourceOf(const live::plane::Site& s, uint32_t stack_slot) {
  using live::plane::What;
  switch (s.what) {
    case What::Fade: return kModule + 68 + 4 * s.index;
    case What::Camera: return kModule + 1072;
    case What::FitMatrix: return kModule + 1312;
    case What::PlaneNormal: return kStack + 80;
    default: return stack_slot;   // PlanePos and the blur's rows are built in the callers' frames
  }
}

inline void DriveFrame(live::plane::Tracker& t, FakeGuest& g, const Frame& frame, uint32_t stamp, const Plant& plant = Plant()) {
  using live::plane::Chain;
  using live::plane::What;
  const live::plane::ReadBytes rd = [&g](uint32_t a, void* dst, size_t n) { return g.Read(a, dst, n); };
  const auto call = [&](const live::plane::Site& s, uint32_t at) {
    if (plant.drop_lr && s.lr == plant.drop_lr) return;
    t.OnConstants(s.lr, s.reg, at, s.rows, stamp, rd);
  };
  const auto module_uploads = [&](const MaskDraw& m) {
    PutModule(g, m);
    for (const live::plane::Site& s : live::plane::kSites)
      if (s.chain == Chain::Mask && s.what != What::PlanePos) call(s, SourceOf(s, 0));
  };
  bool first = true;
  float last_pos[4] = {};
  bool have_pos = false;
  for (const MaskDraw& d : frame.masks) {
    if (first && !plant.upload_after_snapshot) module_uploads(d);
    if (!have_pos || std::memcmp(last_pos, d.rows[7], 16) != 0) {   // c7 is uploaded per proxy kind, before its draws
      g.PutFloats(kStack + 0x100, d.rows[7], 4);
      for (const live::plane::Site& s : live::plane::kSites)
        if (s.what == What::PlanePos) call(s, kStack + 0x100);
      std::memcpy(last_pos, d.rows[7], 16);
      have_pos = true;
    }
    if (first && (plant.foreign_between || plant.foreign_ignored)) {
      const float junk[16] = {9, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6};
      g.PutFloats(kStack + 0x200, junk, 16);
      t.TestAcceptAnyLr(plant.foreign_between);
      t.OnConstants(0x82BF2500u, 13, kStack + 0x200, 4, stamp, rd);   // a call inside sub_82BF22C8 (an engine-parameter slot), NOT one of the table's rows
      t.TestAcceptAnyLr(false);
    }
    if (plant.entry_pos) {
      const bool ok = t.OnPassEntry(stamp, plant.entry_pos);
      if (plant.entry_ok) *plant.entry_ok = ok;
    } else {
      t.OnPartDraw(d.object, d.part, stamp);
    }
    if (first && plant.upload_after_snapshot) module_uploads(d);
    first = false;
  }
  for (const auto& b : frame.blurs) {
    // the blur's rows as sub_827ECD10 builds them (c33 scale, c35 weight, c36-c39 matrix, c40 point) in its own frame
    g.PutFloats(kStack + 0x300, b.data() + 4 * 7, 4);    // c40
    g.PutFloats(kStack + 0x310, b.data() + 4 * 0, 4);    // c33
    g.PutFloats(kStack + 0x320, b.data() + 4 * 2, 4);    // c35
    g.PutFloats(kStack + 0x330, b.data() + 4 * 3, 16);   // c36-c39
    for (const live::plane::Site& s : live::plane::kSites) {
      if (s.chain != Chain::Blur) continue;
      call(s, s.what == What::BlurPoint ? kStack + 0x300 : s.what == What::BlurScale ? kStack + 0x310 : s.what == What::BlurWeight ? kStack + 0x320 : kStack + 0x330);
    }
    t.OnBlurDraw(stamp);
  }
}

// A scene record holding the frame's tables, as the contributor writes it.
inline void FillRecord(SceneTables& t, const std::vector<PlaneFrame>& frames, const std::vector<PlaneDraw>& draws, uint64_t guest_frame) {
  t.header.guest_frame = guest_frame;
  t.plane_frames = frames;
  t.plane_draws = draws;
}

}  // namespace rdr_scene::plane::oracle
