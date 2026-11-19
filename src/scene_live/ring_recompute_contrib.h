// The ring ropes' recomputed node rows as a contributor to the scene record (S103 lane C3; docs\renderer\STAGE_LIVE_WRITERS.md section 3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes (the glue and the hook give it the guest reader and the ring object). The rules are ring_recompute_core.h. This file READS the inputs at the frame's latch (game
// threads parked; the rope grid is advanced by the update side, which keeps running under OURS) and appends one RingRows entry (flag kRrRecomputed) + its node rows per rope that is filled: 12 a frame
// (4 sides x 3 ropes). Read only; free when off: with --wwe2k14_rdr_scene = 0 the latch reads nothing and Emit writes nothing (the mode-1 verify run sets it to 1; it writes the entries ALONGSIDE the game's
// own ring_rows entries, told apart by kRrRecomputed).
// The ring object X: NoteRing(X, hook frame), called from the ring colour callback's hook (scene_skip_nodes.h, `// S103 C3:` line; r3 - 24 = X, ring.md section 4), before the skip test, so it is set in every mode; rows only when it was noted in the hook frame being latched.
#pragma once
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include "ring_stamp_flag.h"   // S103: bit 6 switch
#include <string>
#include <vector>

#include "../renderer_game/scene_frame.h"
#include "../renderer_scene/live_scene.h"
#include "ring_recompute_core.h"
#include "ring_rows_core.h"   // ringrows::NodeCount (the palette node count rule)
#include "tail_latch.h"   // LatchMark

namespace rdr_scene::live::ringrecompute {

using ReadFn = bool (*)(uint32_t guest, void* dst, size_t n);
inline uint32_t Swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
inline bool ReadWord(ReadFn read, uint32_t a, uint32_t& v) {
  uint32_t raw = 0;
  if (!read || !read(a, &raw, 4)) return false;
  v = Swap32(raw);
  return true;
}
inline bool ReadFloats(ReadFn read, uint32_t a, size_t count, std::vector<float>& out) {
  std::vector<uint32_t> raw(count);
  if (!read || !read(a, raw.data(), count * 4)) return false;
  out.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t w = Swap32(raw[i]);
    std::memcpy(&out[i], &w, 4);
  }
  return true;
}

// RIDS playback: bit 0 of the byte at 0x837632FA (0x825CE338-0x825CE358, 0x825CFAD8): the ring pieces are submitted from the recorded RIDS entries, not posed from the grid.
inline bool RidsPlayback(ReadFn read) {
  uint32_t w = 0;
  return ReadWord(read, kRidsByteWord, w) && ((w >> 8) & 1u) != 0;   // big-endian word at ...F8: the byte at ...FA is bits 15..8
}
// The alt flag and the grid / table the fill reads (rope AND turnbuckle both need them). false = unreadable (counted).
inline bool ReadGrid(ReadFn read, uint32_t x, Inputs& in, RingCensus& cn) {
  if (in.grid_read) return true;
  uint32_t alt = 0;
  if (!ReadWord(read, x + kAltOff, alt)) { ++cn.unreadable; return false; }
  in.alt = alt != 0;
  if (in.alt) ++cn.alt_frames;
  if (in.alt) {
    if (!ReadFloats(read, x + kTableOff, size_t(kTableElems) * 12 * 3, in.table)) in.table.clear();   // 12 * 64 elements (3 floats each) = 2304 floats
  } else {
    uint32_t g2 = 0;
    if (ReadWord(read, kGridGlobal, g2) && g2) {
      for (uint32_t r = 0; r < kRopes; ++r) {
        uint32_t layer = 0, buf = 0;
        std::vector<float> raw;
        if (!ReadWord(read, g2 + kLayerOff + 4 * r, layer) || !layer || !ReadWord(read, layer + kLayerBuf, buf) || !buf) continue;
        if (!ReadFloats(read, buf, size_t(kRecCount) * kRecStride / 4, raw)) continue;
        in.layer[r].resize(size_t(kRecCount) * 3);
        for (uint32_t i = 0; i < kRecCount; ++i)
          for (int k = 0; k < 3; ++k) in.layer[r][size_t(i) * 3 + size_t(k)] = raw[size_t(i) * (kRecStride / 4) + size_t(k)];
      }
    }
  }
  in.grid_read = true;
  return true;
}

// The ring's inputs at the latch. `in.gates_ok` false = the game's rope pass would not fill anything this frame (or the ring is not there).
inline void ReadInputs(ReadFn read, uint32_t x, Inputs& in, RingCensus& cn) {
  in = Inputs{};
  in.x = x;
  if (!x) { ++cn.no_ring; return; }
  uint32_t g = 0, g20 = 0, gate = 0, mg = 0, mg368 = 0, alt = 0, og = 0;
  if (!ReadWord(read, kRingGlobal, g) || !g || !ReadWord(read, g + 20, g20) || !g20) { ++cn.no_ring; return; }   // [G + 20] = the rope model (0x825CF354-0x825CF370)
  if (!ReadWord(read, x + kGateOff, gate) || !gate) { ++cn.gate_closed; return; }                                 // [X + 96] (0x825CF30C)
  if (ReadWord(read, kModelGate, mg) && mg && ReadWord(read, mg + 368, mg368) && mg368 == 1) { ++cn.gate_closed; return; }   // 0x825CF338-0x825CF350
  if (RidsPlayback(read)) { ++cn.rope_rids; return; }   // the ropes are submitted from RIDS: no grid fill (review 2)
  if (!ReadGrid(read, x, in, cn)) return;
  (void)alt;
  if (ReadWord(read, kOverrideGate, og) && og) { in.override_gate = true; ++cn.override_gate_set; }
  uint32_t rows_max[kSides] = {};
  for (uint32_t h = 0; h < kSides; ++h) {
    uint32_t c = 0;
    if (!ReadWord(read, x + kCountsOff + 4 * h, c)) { ++cn.unreadable; return; }
    in.count[h] = int32_t(c);
    if (in.count[h] < 0 || uint32_t(in.count[h]) > kMaxDesc) { ++cn.bad_count; in.count[h] = 0; continue; }
    in.desc[h].resize(size_t(in.count[h]));
    for (int32_t j = 0; j < in.count[h]; ++j) {
      uint32_t w[3];
      const uint32_t a = x + kEntriesOff + 12 * (kMaxDesc * h + uint32_t(j));
      if (!ReadWord(read, a, w[0]) || !ReadWord(read, a + 4, w[1]) || !ReadWord(read, a + 8, w[2])) { ++cn.unreadable; in.count[h] = 0; in.desc[h].clear(); break; }
      in.desc[h][size_t(j)] = Desc{int32_t(w[0]), int32_t(w[1]), int32_t(w[2])};
      if (int32_t(w[0]) >= 0 && uint32_t(w[0]) < kRows) rows_max[h] = std::max(rows_max[h], w[0] + 1);
    }
  }
  for (uint32_t h = 0; h < kSides; ++h)
    if (rows_max[h] && !ReadFloats(read, x + kBufOff + kBufStride * h, size_t(rows_max[h]) * 16, in.rest[h])) in.rest[h].clear();
  in.gates_ok = true;
}


// The turnbuckle model's inputs at the latch (S103 C3 step 2). false = no rows this frame (counted).
// The turnbuckles have THEIR OWN gates, independent of the ropes' [X + 96] / [G + 20] (sub_825D0340 -> sub_825CE250, review 4): the model gate [0x83767B74]->+368 == 1 stops them, [G + 44] must exist,
// the two index words must not be -1 (0x825D0384-0x825D03BC), RIDS playback (bit 0 of 0x837632FA) goes straight to submit without posing (0x825CE338-0x825CE358), and a model with fewer than 3 nodes is
// not posed (the vtable slot 2 count, 0x825CE36C-0x825CE374). sub_82BA77A8 (the thread's pass index) is the thread slot's and is not read here [U]. `in` carries the grid (read here when the rope gates
// were closed and ReadInputs stopped before it).
inline void ReadTurnbuckleInputs(ReadFn read, uint32_t x, Inputs& in, TbInputs& tb, RingCensus& cn) {
  tb = TbInputs{};
  if (!x) return;   // no ring object noted: ReadInputs counted it
  uint32_t g = 0, model = 0, pal = 0, invp = 0, ia = 0, ib = 0, mg = 0, mg368 = 0;
  if (!ReadWord(read, kRingGlobal, g) || !g) { ++cn.tb_gate; return; }
  if (ReadWord(read, kModelGate, mg) && mg && ReadWord(read, mg + 368, mg368) && mg368 == 1) { ++cn.tb_gate; return; }
  if (!ReadWord(read, g + kTbModelOff, model) || !model) { ++cn.tb_gate; return; }
  if (!ReadWord(read, x + kTbIdxA, ia) || !ReadWord(read, x + kTbIdxB, ib)) { ++cn.tb_unreadable; return; }
  if (ia == 0xFFFFFFFFu || ib == 0xFFFFFFFFu) { ++cn.tb_gate; return; }
  if (RidsPlayback(read)) { ++cn.tb_rids; return; }
  if (!ReadGrid(read, x, in, cn)) { ++cn.tb_unreadable; return; }
  if (in.alt) { ++cn.tb_table_mode; return; }
  if (!ReadWord(read, model + kPaletteOff, pal) || !pal || !ReadWord(read, model + kInvBindOff, invp) || !invp) { ++cn.tb_unreadable; return; }
  int32_t n = 0;
  uint32_t nflags = 0;
  auto rw = [&](uint32_t a, uint32_t& v) { return ReadWord(read, a, v); };
  if (!ringrows::NodeCount(rw, model, n, nflags) || uint32_t(n) > kTbMaxNodes) { ++cn.tb_unreadable; return; }
  if (n < 3) { ++cn.tb_nopose; return; }   // not posed by the game
  tb.model = model, tb.palette = pal, tb.n = uint32_t(n), tb.idxA = int32_t(ia), tb.idxB = int32_t(ib);
  std::vector<float> v;
  if (!ReadFloats(read, kTbVecTable, 16, v)) { ++cn.tb_unreadable; return; }
  for (uint32_t c = 0; c < kTbCorners; ++c) tb.vec[c][0] = v[c * 4], tb.vec[c][1] = v[c * 4 + 1], tb.vec[c][2] = v[c * 4 + 2];
  if (!ReadFloats(read, kTbAngleTable, kTbCorners, v)) { ++cn.tb_unreadable; return; }
  for (uint32_t c = 0; c < kTbCorners; ++c) tb.ang[c] = v[c];
  std::vector<float> two;
  if (!ReadFloats(read, kTbStep, 1, two)) { ++cn.tb_unreadable; return; }
  tb.step = two[0];
  if (!ReadFloats(read, kTbHeight0, 1, two)) { ++cn.tb_unreadable; return; }
  tb.h0 = two[0];
  if (!ReadFloats(read, invp, size_t(n) * 16, tb.inv) || !ReadFloats(read, pal, size_t(n) * 16, tb.pal)) { ++cn.tb_unreadable; return; }
  for (uint32_t h = 0; h < kTbHeights; ++h) {   // the records of corner c: layer h, index 24 (c + 1)
    if (in.layer[h].size() < size_t(kRecCount) * 3) continue;
    tb.rec_ok[h] = true;
    for (uint32_t c = 0; c < kTbCorners; ++c)
      for (int k = 0; k < 3; ++k) tb.rec[h][c][k] = in.layer[h][size_t(TbRecordIndex(c)) * 3 + size_t(k)];
  }
  tb.ok = true;
}

// ======================================================================================================================================================================================================
// S103 C4: covers, posts, skirts (core block "COVERS, POSTS, SKIRTS"). The covers and the posts are called by the same sub_825D0340 as the turnbuckles, so they share its gates (0x825D035C-0x825D03BC: the model
// gate, [G + 44] (the turnbuckle model, tested by the caller), both index words not -1) and the not-replicated paths (RIDS playback, table mode, the alt ring), each counted.
// ======================================================================================================================================================================================================
inline bool ReadModelPal(ReadFn read, uint32_t model, bool need_inv, ModelPal& mp) {
  uint32_t pal = 0, invp = 0;
  int32_t n = 0;
  uint32_t nflags = 0;
  auto rw = [&](uint32_t a, uint32_t& v) { return ReadWord(read, a, v); };
  if (!ReadWord(read, model + kPaletteOff, pal) || !pal) return false;
  if (!ringrows::NodeCount(rw, model, n, nflags) || n <= 0 || uint32_t(n) > kSkirtMaxNodes) return false;
  mp.model = model, mp.palette = pal, mp.n = uint32_t(n);
  if (need_inv && (!ReadWord(read, model + kInvBindOff, invp) || !invp || !ReadFloats(read, invp, size_t(n) * 16, mp.inv))) return false;
  if (!ReadFloats(read, pal, size_t(n) * 16, mp.pal)) return false;
  return true;
}
// The gates the covers and the posts share. false = no rows from either this frame; `why` counter bumped. `in` carries the grid after this (read here when the rope gates were closed).
// S103 CRR: a created arena (the alt ring) no longer ends the read: `alt_ring` (when given) is set and `alt_counter` counts it; the covers keep the normal rule (only the model per corner differs: ring_keys.h) and the posts emit the alt set.
inline bool ReadPieceGates(ReadFn read, uint32_t x, uint32_t g, Inputs& in, RingCensus& cn, uint64_t RingCensus::*gate_counter, uint64_t RingCensus::*unreadable_counter, uint64_t RingCensus::*table_counter, uint64_t RingCensus::*alt_counter, uint64_t RingCensus::*rids_counter, bool* alt_ring = nullptr) {
  uint32_t model = 0, ia = 0, ib = 0, mg = 0, mg368 = 0, arena = 0;
  if (!x || !g) { ++(cn.*gate_counter); return false; }
  if (ReadWord(read, kModelGate, mg) && mg && ReadWord(read, mg + 368, mg368) && mg368 == 1) { ++(cn.*gate_counter); return false; }
  if (!ReadWord(read, g + kTbModelOff, model) || !model) { ++(cn.*gate_counter); return false; }   // [G + 44] != 0 (0x825D0384-0x825D0394, and 0x825CE6B4-0x825CE6C8)
  if (!ReadWord(read, x + kTbIdxA, ia) || !ReadWord(read, x + kTbIdxB, ib)) { ++(cn.*unreadable_counter); return false; }
  if (ia == 0xFFFFFFFFu || ib == 0xFFFFFFFFu) { ++(cn.*gate_counter); return false; }
  if (RidsPlayback(read)) { ++(cn.*rids_counter); return false; }
  if (ReadWord(read, kArenaIdWord, arena) && int32_t(arena) >= 200 && int32_t(arena) <= 249) {   // sub_822AA2F8: the alt ring (CRK section 2)
    ++(cn.*alt_counter);
    if (alt_ring) *alt_ring = true;
  }
  if (!ReadGrid(read, x, in, cn)) { ++(cn.*unreadable_counter); return false; }
  if (in.alt) { ++(cn.*table_counter); return false; }
  return true;
}
inline void ReadCoverInputs(ReadFn read, uint32_t x, Inputs& in, CvInputs& cv, RingCensus& cn) {
  cv = CvInputs{};
  uint32_t g = 0;
  if (!ReadWord(read, kRingGlobal, g) || !g) { ++cn.cv_gate; return; }
  if (!ReadPieceGates(read, x, g, in, cn, &RingCensus::cv_gate, &RingCensus::cv_unreadable, &RingCensus::cv_table_mode, &RingCensus::cv_alt_ring, &RingCensus::cv_rids, &cv.alt)) return;
  uint32_t hi = 0, lo = 0;
  if (!ReadWord(read, g + kCvModelHi, hi) || !hi || !ReadWord(read, g + kCvModelLo, lo)) { ++cn.cv_gate; return; }
  if (!ReadModelPal(read, hi, true, cv.models[0])) { ++cn.cv_unreadable; return; }
  if (lo) ++cn.cv_lod_both;   // S103 C5: the low cover is counted, NOT read or emitted: the LOD test (view distance) is not replicated [U], the high cover stands in (like the rope), so one model is drawn, never both
  for (uint32_t c = 0; c < kCvCorners; ++c) {
    uint32_t cnt = 0, flag = 0;
    if (!ReadWord(read, x + kCvCountOff + 4 * c, cnt)) { ++cn.cv_unreadable; return; }
    cv.count[c] = int32_t(cnt);
    if (cv.count[c] < 0 || uint32_t(cv.count[c]) > kCvMaxDesc) { ++cn.cv_bad; cv.count[c] = 0; return; }
    for (int32_t k = 0; k < cv.count[c]; ++k) {
      uint32_t w[3];
      const uint32_t a = x + kCvDescOff + kCvDescStride * (3 * c + uint32_t(k));
      if (!ReadWord(read, a, w[0]) || !ReadWord(read, a + 4, w[1]) || !ReadWord(read, a + 8, w[2])) { ++cn.cv_unreadable; return; }
      cv.desc[c].push_back(CvDesc{int32_t(w[0]), int32_t(w[1]), int32_t(w[2])});
    }
    std::vector<float> f;
    if (!ReadFloats(read, x + kCvAlphaOff + 4 * c, 1, f)) { ++cn.cv_unreadable; return; }
    cv.alpha_mul[c] = f[0];
    if (!ReadWord(read, x + kCvOvrFlagOff + 4 * c, flag)) { ++cn.cv_unreadable; return; }
    cv.ovr[c] = flag != 0;
    if (cv.ovr[c]) {
      if (!ReadFloats(read, x + kCvOvrVecOff + 16 * c, 3, f)) { ++cn.cv_unreadable; return; }
      cv.ovr_vec[c][0] = f[0], cv.ovr_vec[c][1] = f[1], cv.ovr_vec[c][2] = f[2];
    }
  }
  std::vector<float> f;
  if (!ReadFloats(read, kCvAngBase, 1, f)) { ++cn.cv_unreadable; return; }
  cv.ang_base = f[0];
  if (!ReadFloats(read, kCvAngStep, 1, f)) { ++cn.cv_unreadable; return; }
  cv.ang_step = f[0];
  if (!ReadFloats(read, kCvPi, 1, f)) { ++cn.cv_unreadable; return; }
  cv.pi = f[0];
  if (!ReadFloats(read, kCvNegPi, 1, f)) { ++cn.cv_unreadable; return; }
  cv.neg_pi = f[0];
  if (!ReadFloats(read, kCvTwoPi, 1, f)) { ++cn.cv_unreadable; return; }
  cv.two_pi = f[0];
  cv.ok = true;
}
inline void ReadPostInputs(ReadFn read, uint32_t x, Inputs& in, PostInputs& po, RingCensus& cn) {
  po = PostInputs{};
  uint32_t g = 0;
  if (!ReadWord(read, kRingGlobal, g) || !g) { ++cn.post_unreadable; return; }
  uint64_t RingCensus::*none = &RingCensus::post_unreadable;
  if (!ReadPieceGates(read, x, g, in, cn, none, none, none, &RingCensus::post_alt_ring, none, &po.alt)) return;
  uint32_t hi = 0, lo = 0;
  if (!ReadWord(read, g + kPostModelHi, hi) || !hi || !ReadWord(read, g + kPostModelLo, lo)) { ++cn.post_unreadable; return; }
  if (!ReadModelPal(read, hi, false, po.models[0])) { ++cn.post_unreadable; return; }
  if (lo) ++cn.post_lod_both;   // S103 C5: counted, not read or emitted (see ReadCoverInputs)
  std::vector<float> f;
  if (!ReadFloats(read, kCvAngStep, 1, f)) { ++cn.post_unreadable; return; }
  po.step = f[0];
  if (!ReadFloats(read, kCvPi, 1, f)) { ++cn.post_unreadable; return; }
  po.pi = f[0];
  if (!ReadFloats(read, kCvNegPi, 1, f)) { ++cn.post_unreadable; return; }
  po.neg_pi = f[0];
  if (!ReadFloats(read, kCvTwoPi, 1, f)) { ++cn.post_unreadable; return; }
  po.two_pi = f[0];
  po.ok = true;
}
// S103 C5: which skirts the game poses this frame (SkirtVisibleMask, ring_recompute_core.h). Unreadable = no verdict (counted once): the frame is never clean.
inline void ReadSkirtVisibility(ReadFn read, uint32_t x, SkirtVisIn& v, RingCensus& cn) {
  v = SkirtVisIn{};
  uint32_t g = 0, p = 0, p16 = 0, w = 0;
  if (!x || !ReadWord(read, kRingGlobal, g) || !g) { ++cn.skirt_vis_unreadable; return; }
  for (uint32_t s = 0; s < kSkirtCount; ++s) {
    uint32_t m = 0;
    if (!ReadWord(read, g + kSkirtFirst + 4 * s, m)) { ++cn.skirt_vis_unreadable; return; }
    if (!m) { v.ok = true, v.early_exit = true; return; }   // 0x82609090-0x826090E0: a missing model ends the function before the loop
  }
  if (!ReadWord(read, kSkirtViewGlobal, p)) { ++cn.skirt_vis_unreadable; return; }
  if (!p || !ReadWord(read, p + 16, p16) || !p16) { v.ok = true, v.early_exit = true; return; }
  uint32_t idx = 0, arr = 0;
  if (!ReadWord(read, p + 32, idx) || !ReadWord(read, p + 24, arr)) { ++cn.skirt_vis_unreadable; return; }
  const uint32_t cam = arr + 272u * idx + 144u;
  uint32_t ra = 0, rb = 0, rt = 0;
  if (!ReadWord(read, cam, ra) || !ReadWord(read, cam + 8, rb) || !ReadWord(read, kQuadThreshold, rt)) { ++cn.skirt_vis_unreadable; return; }
  float a, b, thr;
  std::memcpy(&a, &ra, 4), std::memcpy(&b, &rb, 4), std::memcpy(&thr, &rt, 4);
  v.quad = QuadrantOf(a, b, thr);
  if (!ReadWord(read, kSkirtDrawWord, v.draw_word)) { ++cn.skirt_vis_unreadable; return; }
  v.rids = RidsPlayback(read);
  for (uint32_t i = 0; i < 4; ++i)
    if (!ReadWord(read, x + kSkirtStateOff + 4 * i, v.state[i]) || !ReadWord(read, kSkirtSideTable + 4 * i, v.side[i])) { ++cn.skirt_vis_unreadable; return; }
  (void)w;
  v.ok = true;
}
// The skirt skeletons G[332..344] of the VISIBLE sides (`mask`, bit s = model s): palette + node count each. An invisible or absent model has no entry; a model that fails to read is counted once.
inline void ReadSkirts(ReadFn read, uint32_t mask, ModelPal out[kSkirtCount], RingCensus& cn) {
  uint32_t g = 0;
  for (uint32_t s = 0; s < kSkirtCount; ++s) out[s] = ModelPal{};
  if (!mask || !ReadWord(read, kRingGlobal, g) || !g) return;
  for (uint32_t s = 0; s < kSkirtCount; ++s) {
    uint32_t m = 0;
    if (!((mask >> s) & 1u) || !ReadWord(read, g + kSkirtFirst + 4 * s, m) || !m) continue;
    if (!ReadModelPal(read, m, false, out[s])) { out[s] = ModelPal{}; ++cn.skirt_unreadable; }
  }
}

// kStampRingRows: ring_stamp_flag.h (released by the main session 2026-11-16; the evidence is there).

class RingRecomputeContributor final : public Contributor {
 public:
  const char* Name() const override { return "ring_recompute"; }
  void SetReader(ReadFn r) { read_ = r; }
  // The ring callback ran in hook frame `hook_frame` with this object. A noted X that is older than the frame being latched (the ring task is not running: a menu, a replay without the ring) is not used (review F3).
  // S103 RLB: `view` = the draw thread's camera as the hook read it (the rope LOD rule's eye + forward, core.h RopeView); null = no camera (every rope HIGH, the offline tools' old behaviour).
  void NoteRing(uint32_t x, uint64_t hook_frame, const RopeView* view = nullptr) {
    if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;   // mode 0 (every player): the hook stores nothing
    x_.store(x, std::memory_order_relaxed);   // (the view and x are published by the release store of noted_frame_ below)
    if (view && view->has) {
      for (int i = 0; i < 3; ++i) {
        vw_[i].store(std::bit_cast<uint32_t>(view->eye[i]), std::memory_order_relaxed);
        vw_[3 + i].store(std::bit_cast<uint32_t>(view->fwd[i]), std::memory_order_relaxed);
      }
      vw_has_.store(true, std::memory_order_relaxed);
    } else {
      vw_has_.store(false, std::memory_order_relaxed);
    }
    noted_frame_.store(hook_frame, std::memory_order_release);
    noted_calls_.fetch_add(1, std::memory_order_relaxed);
  }
  // S103 C4: the glue gives the stamp (scene_inputs::Capture(kInRingRows, HookFrameOfRecord(record_frame)): the record's frame, converted by the sink). Null = offline tools: never stamps.
  void SetStampSink(void (*fn)(uint64_t record_frame)) { stamp_ = fn; }
  uint64_t NotedFrame() const { return noted_frame_.load(std::memory_order_acquire); }
  uint32_t Ring() const { return x_.load(std::memory_order_relaxed); }

  bool Latch(const FrameInfo& fi) override {
    if (!read_) return false;
    latched_.Clear();
    if (scene_frame::CurrentMode() != scene_frame::Mode::Off) Take(fi.guest_frame, latched_);
    mark_.Set(fi.guest_frame);
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    Out scratch;
    const Out* o = &latched_;
    if (!mark_.Use(fi.guest_frame, Name())) {
      if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
      if (InDeferredEmit() || !read_) { ++c.stale; return; }
      Take(fi.guest_frame, scratch);
      o = &scratch;
    }
    const uint32_t base = uint32_t(t.ring_nodes.size());
    t.ring_nodes.insert(t.ring_nodes.end(), o->pool.begin(), o->pool.end());
    for (RingRows r : o->rows) {
      r.first_node += base;
      t.ring_rows.push_back(r);
    }
    c.objects += uint32_t(o->rows.size());
  }
  void Census(std::string& out) override {
    const RingCensus c = Stats();
    std::string mm;
    {
      std::lock_guard<std::mutex> lk(cmu_);
      mm = last_mismatch_;
    }
    auto n = [](uint64_t v) { return std::to_string((unsigned long long)v); };
    out += "[ring_recompute frames " + n(c.frames) + ", ropes " + n(c.ropes) + ", rows " + n(c.rows) + ", nodes " + n(c.nodes_total) + ", no_ring " + n(c.no_ring) + ", gate_closed " + n(c.gate_closed) +
           ", unreadable " + n(c.unreadable) + ", bad_count " + n(c.bad_count) + ", bad_index " + n(c.bad_index) + ", tb_rows " + n(c.tb_rows) + " (bad_index " + n(c.tb_bad_index) + " table_mode " + n(c.tb_table_mode) + " unreadable " + n(c.tb_unreadable) + "), tb_gate " + n(c.tb_gate) + " tb_rids " + n(c.tb_rids) + " tb_nopose " + n(c.tb_nopose) + " rope_rids " + n(c.rope_rids) + ", no_call " + n(c.no_call) + ", noted_calls " + n(c.noted_calls) + ", delta[-3..3,other,never] " + n(c.delta[0]) + " " + n(c.delta[1]) + " " + n(c.delta[2]) + " " + n(c.delta[3]) + " " + n(c.delta[4]) + " " + n(c.delta[5]) + " " + n(c.delta[6]) + " " + n(c.delta[7]) + " " + n(c.delta[8]) + (mm.empty() ? std::string() : "; " + mm) + ", alt_frames " + n(c.alt_frames) + ", override_gate_set " + n(c.override_gate_set) + ", covers " + n(c.cv_rows) + " (hidden " + n(c.cv_hidden) + " bad " + n(c.cv_bad) + " unreadable " + n(c.cv_unreadable) + " gate " + n(c.cv_gate) + " table " + n(c.cv_table_mode) + " alt_ring " + n(c.cv_alt_ring) + " rids " + n(c.cv_rids) + " lod_both " + n(c.cv_lod_both) + "), posts " + n(c.post_rows) + " (unreadable " + n(c.post_unreadable) + " lod_both " + n(c.post_lod_both) + " alt_ring " + n(c.post_alt_ring) + "), skirts " + n(c.skirt_rows) + " (unreadable " + n(c.skirt_unreadable) + " vis_unreadable " + n(c.skirt_vis_unreadable) + " none_visible " + n(c.skirt_vis_none) + "), ring_rows whole clean " + n(c.whole_clean) + " unclean " + n(c.whole_unclean) + " [rope " + n(c.unclean_rope) + " tb " + n(c.unclean_tb) + " cover " + n(c.unclean_cover) + " post " + n(c.unclean_post) + " skirt " + n(c.unclean_skirt) + " override " + n(c.unclean_override) + "]]";
    out += " [ring_lod ours lo " + n(c.lod_lo) + " hi " + n(c.lod_hi) + " nocam " + n(c.lod_nocam) + "]";
  }
  struct Out {
    std::vector<RingRows> rows;
    std::vector<RingNodeRows> pool;
    void Clear() { rows.clear(), pool.clear(); }
  };
  void Take(uint64_t frame, Out& out) {
    std::lock_guard<std::mutex> lk(cmu_);
    ++census_.frames;
    // STAMP = the callback's own frame (the record frame), the game's call stamp convention: run 16 compare (tools scene ring_compare.cpp): our rows of record j equal the game's RingRows of record j+1 (stamped j), 912 of 912.
    const uint32_t stamp = uint32_t(frame);
    census_.noted_calls = noted_calls_.load(std::memory_order_relaxed);
    {
      const uint64_t nf = NotedFrame();
      const int64_t d = nf == ~0ull ? 99 : int64_t(nf) - int64_t(frame);
      ++census_.delta[nf == ~0ull ? 8 : (d >= -3 && d <= 3 ? size_t(d + 3) : 7)];
      if (nf != ~0ull && nf != stamp && !logged_) {   // one line, the first mismatch (dev question of run 13)
        logged_ = true;
        last_mismatch_ = "[ring_recompute] first mismatch: noted frame " + std::to_string((unsigned long long)nf) + " latched guest_frame " + " (guest_frame " + std::to_string((unsigned long long)frame) + ")";
      }
    }
    // CLOCK (run 14, wwe2k14_7267.log): the ring callback is a DRAW-half task and reads g_guest_frame AFTER the swap published the next frame: every one of 2239 notes was exactly +1 against the hook
    // frame the record carries (noted 4007, latched hook frame 4006, record guest_frame 4007; delta histogram all in the +1 bucket). So a note belongs to record j when noted == j (= the record's guest_frame),
    // not HookFrameOf(j). The ring state read at this latch is the same record's: the callback ran inside the walks this latch closes, the grid is advanced by the update half (it keeps running under OURS).
    // The row STAMP is the callback frame itself (the game's own call stamp: see below).
    if (NotedFrame() != uint64_t(frame)) {   // the ring callback did not run for this record: no rows (and never the object of an earlier scene)
      ++census_.no_call;
      return;
    }
    Inputs in;
    ReadInputs(read_, Ring(), in, census_);
    in.view.has = vw_has_.load(std::memory_order_relaxed);   // the same hook frame's camera (NotedFrame() == frame above)
    for (int i = 0; i < 3; ++i) {
      in.view.eye[i] = std::bit_cast<float>(vw_[i].load(std::memory_order_relaxed));
      in.view.fwd[i] = std::bit_cast<float>(vw_[3 + i].load(std::memory_order_relaxed));
    }
    WholeRing whole;
    whole.override_gate = in.override_gate;
    size_t mark = out.rows.size();
    if (in.gates_ok)
      for (uint32_t h = 0; h < kSides; ++h)
        for (uint32_t r = 0; r < kRopes; ++r) AppendRope(in, h, r, stamp, out.rows, out.pool, census_);
    whole.ropes = uint32_t(out.rows.size() - mark);
    TbInputs tb;   // independent of the rope gates
    ReadTurnbuckleInputs(read_, Ring(), in, tb, census_);
    mark = out.rows.size();
    for (uint32_t c = 0; c < kTbCorners; ++c)
      for (uint32_t h = 0; h < kTbHeights; ++h) AppendTurnbuckle(tb, c, h, stamp, out.rows, out.pool, census_);
    whole.turnbuckles = uint32_t(out.rows.size() - mark);
    // S103 C4: covers, posts, skirts (S103 C5: ONE LOD model each: the high one stands in, the low ones are not read), 
    CvInputs cv;
    ReadCoverInputs(read_, Ring(), in, cv, census_);
    for (uint32_t mi = 0; mi < kCvModels; ++mi) {
      mark = out.rows.size();
      const uint64_t hidden0 = census_.cv_hidden;
      for (uint32_t r25 = 0; r25 < kCvCorners; ++r25)
        for (uint32_t r28 = 0; r28 < kCvLayers; ++r28) AppendCover(cv, in, mi, r25, r28, stamp, out.rows, out.pool, census_);
      if (mi == 0) whole.covers = uint32_t(out.rows.size() - mark), whole.covers_hidden = uint32_t(census_.cv_hidden - hidden0);
    }
    PostInputs po;
    ReadPostInputs(read_, Ring(), in, po, census_);
    for (uint32_t mi = 0; mi < kPostModels; ++mi) {
      mark = out.rows.size();
      for (uint32_t i = 0; i < PostCalls(po); ++i) AppendPost(po, mi, i, stamp, out.rows, out.pool, census_);   // (S103 CRR: 8 calls in a created arena)
      if (mi == 0) whole.posts = uint32_t(out.rows.size() - mark), whole.posts_expected = PostCalls(po);
    }
    // S103 C5: only the skirts the game poses for the camera quadrant (3 of 4 in the usual view), and the whole-ring test wants EXACTLY that many
    SkirtVisIn svis;
    ReadSkirtVisibility(read_, Ring(), svis, census_);
    const uint32_t vmask = SkirtVisibleMask(svis);
    if (svis.ok && !vmask) ++census_.skirt_vis_none;
    ModelPal sk[kSkirtCount];
    ReadSkirts(read_, vmask, sk, census_);
    mark = out.rows.size();
    for (uint32_t s = 0; s < kSkirtCount; ++s) AppendSkirt(sk[s], s, stamp, out.rows, out.pool, census_);
    whole.skirts = uint32_t(out.rows.size() - mark);
    whole.skirts_expected = svis.ok ? int32_t(PopCount4(vmask)) : -1;
    // input bit 6: the whole ring read clean for THIS record (the ring callback ran for it: the noted-frame check above), stamped in the hook clock by the glue's sink
    // S103 C5: HELD (kStampRingRows = false): the verdict is counted (whole_clean) but the input stays open until a batched mode-1 run passes ring_compare for RC_CLASS = cover / turnbuckle / skirt
    // and the skirt visibility rule is confirmed against the game's own count (docs\renderer\STAGE_LIVE_WRITERS.md 3c).
    const bool clean = WholeRingClean(whole, census_);
    if (kStampRingRows && clean && stamp_) stamp_(frame);
  }
  RingCensus Stats() const {
    std::lock_guard<std::mutex> lk(cmu_);
    return census_;
  }

 private:
  ReadFn read_ = nullptr;
  void (*stamp_)(uint64_t) = nullptr;
  std::atomic<uint32_t> x_{0};
  std::atomic<uint32_t> vw_[6] = {};   // RLB: eye xyz, forward xyz (float bits) of the hook's camera
  std::atomic<bool> vw_has_{false};
  std::atomic<uint64_t> noted_calls_{0};
  bool logged_ = false;
  std::string last_mismatch_;
  std::atomic<uint64_t> noted_frame_{~0ull};
  LatchMark mark_;
  Out latched_;
  mutable std::mutex cmu_;
  RingCensus census_;
};

inline RingRecomputeContributor& RingRecompute() {
  static RingRecomputeContributor s;
  return s;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterRingRecompute() {   // idempotent
  if (!RegisteredFlag().exchange(true)) Register(&RingRecompute());
}
inline bool RingRecomputeRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }
inline void NoteRing(uint32_t x, uint64_t hook_frame, const RopeView* view = nullptr) { RingRecompute().NoteRing(x, hook_frame, view); }   // the hook's one call: relaxed stores
// The hook reads the camera only when something will use it (mode 0 = the stream build: nothing is stored, so nothing is asked).
inline bool RingViewWanted() { return scene_frame::CurrentMode() != scene_frame::Mode::Off; }

}  // namespace rdr_scene::live::ringrecompute
