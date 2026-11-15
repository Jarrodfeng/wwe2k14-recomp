// S102 lane L1b (docs/renderer/PRELIGHT_DIRECT.md section 9): the pre-light cubes written by ONE compute dispatch a frame. No game or SDK includes.
//
// The game bakes the lights into a 64x64x6 RGBA8 cube five times a frame (a slot = SetTargets(384x64 strip), one yPreLight 'sss' draw of call site 0x82C20B90, six strip -> layer copies) and many
// draws sample the cubes. This lever keeps every reader exactly as it is (they sample the cubes) and replaces the WRITING: the 8 ops of each taken slot are not recorded (G4's drop mechanism) and ONE
// DispatchOp, issued where the first taken slot began, writes all the taken cubes from the slots' own draw constants (shaders/rdr_light_cubes.comp: the bake's function at the direction the strip's raster
// would have interpolated for each texel). No strip, no render pass, no copy, no pipeline bind but one.
//
// A slot (group) is TAKEN when, in this frame:
//   - the plan is allowed (not an isolate / velocity bind, not a part of a split frame, not a lasting-only bind), the device can dispatch and the cube was made with storage usage, and no other planner
//     (G4's dead cut, P1's bake, the post claim, G3's turns, G4b's blank chain, an own group, L1's analytic drops) already drops one of its ops;
//   - the shape is exactly SetTargets + one pre-light draw + six copies, contiguous; the draw is the yPreLight 'sss' program with the plain state; the images are the strip (384x64) and an 8-bit RGBA
//     cube (64x64x6); the six copies are this strip's tiles into this cube's layers by the IDENTITY map (tile j -> layer j: the shader draws face j into layer j; C1 / G14 measured it on every frame);
//   - the clip transform is plain (ndc scale x 1, |y| 1, viewport 0 0 384 64, scissor covering the strip: the texel -> grid mapping below is computed from the draw's own ScreenScale, ndc scale and offset);
//   - the owned mesh is the one the shader models (726 vertices, 1380 indices, the six 11 x 11 grids of normalised bilinear corner directions; verified per distinct geometry key);
//   - the strip is read / written by nothing else, the cube is written by nothing else, neither has CPU content or a CPU read-back, no draw / copy / Present reads the cube BETWEEN the dispatch's position
//     (the first taken slot's SetTargets) and this slot's own position (that read would see the new bake where the stream still has the old one).
// A taken slot with no reader is taken too. Slots the planner declines keep the stream's strip and copies (counted by reason).
//
// Constants (PreLightK, fx_prelight.glsl; the compute shader reads the same text): prelight_analytic.h's Precompute of the bake's registers, and [15] = (x scale, x offset, y scale, y offset) taking a cube
// texel to its grid coordinate in cells (computed from the clip transform: 1/6.4, 0, 1/6.4, 0 for the game's draw).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "capture_reader.h"
#include "copy_alias.h"
#include "prelight_analytic.h"

namespace rdr_adapter::prelight_compute {

// fview / rplay: --prelight-compute (the table's fallback until the first refresh; live reads the PerFrame table every frame).
inline std::atomic<bool> g_fview{false};
inline std::atomic<bool> g_storage_always{false};   // rplay: an --arms run that toggles the switch (arm kind 6): the cubes are made with storage usage whatever the first frame's arm says

inline constexpr uint32_t kPreLightCallSite = 0x82C20B90u;   // sub_82C20AB8's draw (db: prelight_slots.md)
inline constexpr uint32_t kStripW = 384, kStripH = 64, kFace = 64, kFaces = 6;
inline constexpr uint32_t kTagGroups = 8;                    // the per-image tag word: strip bits 0-7, cube bits 8-15 (review F4)
inline constexpr uint32_t kMaxGroups = 5;                    // rdr::kMaxLightCubes: a frame's dispatch writes at most five cubes; the rest keep the stream's bake
inline constexpr uint32_t kVec4 = rdr::kLightCubeVec4;
inline constexpr uint32_t kVertices = 726, kIndices = 1380, kVertexBytes = 726 * 24, kIndexBytes = 1380 * 2;

// Planted faults (tools only). Each must be CAUGHT by tools\l1b\l1b_check.py (the shader plant and the barrier plant are renderer flags: rdr::g_light_cube_plant).
enum class Fault : uint8_t {
  None,
  HalfTexelDropped,  // the clip offsets (the game's half-pixel rule) are left out of the texel -> grid mapping: every texel is sampled half a texel off (the cube must differ)
  StaleK,            // the dispatch keeps the FIRST planned frame's constants for the whole run (constants from an older frame: the cube must differ where the lights move)
  KeepOps,           // the group's 8 ops are recorded as well as dispatched: the counts must not fall (the check fails on that)
  PhantomReader,     // a synthetic read of the first cube between the dispatch and its slot is added to the second group (that group must be declined)
  NoReaderCheck,     // the read-between scan is skipped: with PhantomReader the group is taken although it would be read too early (taken_read_between must stay 0: it does not)
  IgnoreOverlap,     // the other planners' drop sets are not looked at (with a planted G4 cut a group is taken although its ops are dropped: taken_overlapping)
  BadMesh,           // the mesh model check sees a vertex 1 % off: every group must be DECLINED ("mesh")
  ShiftedScreen,     // review F3: ScreenScale's x offset is moved a quarter of a cell: the texel -> grid mapping leaves [0, 10] and the group must be DECLINED ("clip transform")
  NoIndexCap,        // review F4: the 8-group cap of the per-image tag bits is not applied (a ninth group aliases the first's bits: the synthetic 17-group test must see it taken or mis-judged)
  Count
};
inline Fault g_fault = Fault::None;
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "half_texel_dropped", "stale_k", "keep_ops", "phantom_reader", "no_reader_check", "ignore_overlap", "bad_mesh", "shifted_screen", "no_index_cap"};
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}
inline uint32_t g_fault_bits = 0;
inline bool Planted(Fault f) { return (g_fault_bits >> uint32_t(f)) & 1u; }
inline bool AddFault(const char* name) {
  Fault f;
  if (!ParseFault(name, f)) return false;
  if (g_fault == Fault::None) g_fault = f;
  g_fault_bits |= 1u << uint32_t(f);
  return true;
}

enum Why : uint8_t {
  kTaken = 0, kNotPlanned, kNoCompute, kDeadSkip, kOverlap, kShape, kNotSss, kState, kImages, kFormat, kNotIdentity, kClipTransform, kMesh, kStripRead, kStripWritten, kCubeWritten, kContent, kReadback,
  kNoConstants, kNoStorage, kReadBetween, kTooMany, kWhyCount
};
inline const char* WhyName(int w) {
  static const char* const k[] = {"taken", "frame not planned (isolate / velocity / part of a split frame)", "the device cannot dispatch", "G4's dead skip took the slot", "another planner's drop set holds an op",
                                  "not SetTargets + one pre-light draw + six copies", "the draw is not the yPreLight sss program", "the draw's state is not plain (blend / write mask / depth / stencil / alpha test)",
                                  "images are not strip + cube", "the cube is not an 8-bit RGBA image", "the copies are not the identity face map (tile j -> layer j)",
                                  "the draw's clip transform / viewport / scissor is not the plain strip's", "the mesh is not the one the shader models", "the strip is read by something else", "the strip is written by something else",
                                  "the cube is written by something else", "CPU content in the frame", "the strip or cube was read back by the CPU", "no constants for the draw", "the cube has no storage usage",
                                  "the cube is read between the dispatch and its slot", "more than five slots in the frame"};
  static_assert(sizeof(k) / sizeof(k[0]) == kWhyCount, "WhyName: one name per Why");
  return w >= 0 && w < kWhyCount ? k[w] : "?";
}

struct Group {
  uint32_t set_op = 0, draw_op = 0, copy_op[kFaces] = {};
  uint32_t strip = 0, cube = 0;     // the game's image ids
  float k[kVec4 * 4] = {};          // the constants the dispatch evaluates (PreLightK, [15] = the texel -> grid mapping)
  bool shape_ok = false;
};

struct Plan {
  std::vector<uint8_t> drop;        // by op index: 1 = not recorded (the taken groups' 8 ops each)
  std::vector<Group> groups;        // the taken groups, in frame order
  uint32_t first_op = 0;            // the op where the dispatch is issued (the first taken group's SetTargets)
  uint32_t count = 0;
  void Reset(uint32_t ops) { drop.assign(ops, 0), groups.clear(), count = 0, first_op = 0; }
};

struct Census {
  uint64_t frames = 0, frames_with_groups = 0, frames_taken = 0;
  uint64_t groups = 0, taken = 0;
  uint64_t declined[kWhyCount] = {};
  uint64_t dispatches = 0, ops_dropped = 0, cubes_written = 0;
  uint64_t taken_read_between = 0;   // the audit: a taken group whose cube an independent scan finds read between the dispatch and its slot (NoReaderCheck + PhantomReader makes it > 0)
  uint64_t taken_overlapping = 0;    // the audit: a taken group with an op in another planner's drop set
  uint64_t plan_ns = 0, plan_runs = 0;   // the planner's own time, two clock reads a frame
  uint64_t meshes_checked = 0;
};

struct Ctx {
  bool plan_ok = false;       // false: an isolate / velocity bind, a part of a split frame, a lasting-only bind
  bool enabled = false;
  bool compute = false;       // the renderer can dispatch
  uint64_t guest_frame = 0;
  std::function<const ImageRecord*(uint32_t)> record;
  std::function<bool(uint32_t)> cube_has_storage;   // the renderer image of this capture id was made with storage usage
  const uint8_t* dead = nullptr;                     // G4's drop set (by op index), may be null
  std::vector<const uint8_t*> others;                // the other planners' drop sets
  std::function<bool(const Draw&)> is_prelight_sss;                    // the draw's programs are yPreLight base + sss
  std::function<bool(const Draw&, float* k)> fetch_k;                  // the draw's PS bank -> the bake's raw registers (prelight_analytic.h layout, kVec4 * 4 floats)
  std::function<bool(const Draw&, float* screen)> fetch_screen;        // the draw's VS bank -> ScreenScale (4 floats)
  std::function<bool(const Draw&)> mesh_ok;                            // the draw's mesh is the modelled one (the binder reads the geometry; cached by key)
};

class Planner {
 public:
  Census census;
  std::vector<float> prev_k;   // the first planned frame's constants of its first group (the StaleK twin)

  // The mesh the shader models: six 11 x 11 grids (x = 16 f + 1.6 c, y = -1.6 r, the normalised bilinear direction of the face's corners), strips of 22 vertices with a restart after each.
  static bool CheckMesh(const uint8_t* verts, uint32_t vbytes, uint32_t vcount, const uint8_t* idx, uint32_t ibytes, uint32_t icount, bool plant_bad) {
    if (!verts || !idx || vcount != kVertices || icount != kIndices || vbytes != kVertexBytes || ibytes != kIndexBytes) return false;
    static const float kCorner[24][3] = {{1, 1, 1},   {1, 1, -1},  {1, -1, 1},  {1, -1, -1}, {-1, 1, -1}, {-1, 1, 1},  {-1, -1, -1}, {-1, -1, 1}, {-1, 1, -1}, {1, 1, -1}, {-1, 1, 1}, {1, 1, 1},
                                         {-1, -1, 1}, {1, -1, 1},  {-1, -1, -1}, {1, -1, -1}, {-1, 1, 1},  {1, 1, 1},  {-1, -1, 1},  {1, -1, 1},  {1, 1, -1},  {-1, 1, -1}, {1, -1, -1}, {-1, -1, -1}};
    for (uint32_t f = 0; f < kFaces; ++f)
      for (uint32_t r = 0; r < 11; ++r)
        for (uint32_t c = 0; c < 11; ++c) {
          float v[6];
          std::memcpy(v, verts + (size_t(f) * 121 + r * 11 + c) * 24, 24);
          const float u = float(c) * 0.1f, w = float(r) * 0.1f;
          float d[3], len = 0;
          for (int a = 0; a < 3; ++a) {
            const float top = kCorner[f * 4][a] * (1 - u) + kCorner[f * 4 + 1][a] * u, bottom = kCorner[f * 4 + 2][a] * (1 - u) + kCorner[f * 4 + 3][a] * u;
            d[a] = top * (1 - w) + bottom * w, len += d[a] * d[a];
          }
          len = std::sqrt(len);
          if (std::fabs(v[0] - (16.0f * float(f) + 1.6f * float(c))) > 1e-3f || std::fabs(v[1] + 1.6f * float(r)) > 1e-3f || std::fabs(v[2]) > 1e-3f) return false;
          const float scale = plant_bad && f == 0 && r == 5 && c == 5 ? 1.01f : 1.0f;   // the planted twin: the model is 1 % off at one vertex
          for (int a = 0; a < 3; ++a)
            if (std::fabs(v[3 + a] - d[a] / len * scale) > 2e-5f) return false;
        }
    for (uint32_t f = 0; f < kFaces; ++f)
      for (uint32_t k = 0; k < 10; ++k)
        for (uint32_t j = 0; j < 23; ++j) {
          uint16_t want;
          if (j == 22) want = 0xFFFF;
          else if ((j & 1u) == 0) want = uint16_t(f * 121 + (k + 1) * 11 + j / 2);
          else want = uint16_t(f * 121 + k * 11 + j / 2);
          uint16_t got;
          std::memcpy(&got, idx + (size_t(f) * 230 + k * 23 + j) * 2, 2);
          if (got != want) return false;
        }
    return true;
  }

  void Build(const LoadedFrame& f, const Ctx& ctx, Plan& plan) {
    plan.Reset(f.ops.count);
    ++census.frames;
    if (!f.ops.count || !f.origins.count) return;
    found_.clear();
    FindGroups(f, found_);
    if (found_.empty()) return;
    ++census.frames_with_groups;
    const size_t n = found_.size();
    verdict_.assign(n, kTaken);
    overlapping_.assign(n, 0);
    // 1. the group-level judgement (no scan of the frame)
    uint32_t candidates = 0;
    for (size_t i = 0; i < n; ++i) {
      ++census.groups;
      verdict_[i] = i >= kTagGroups && !Planted(Fault::NoIndexCap) ? kTooMany : Judge(f, ctx, found_[i], overlapping_[i]);   // review F4: the tag words hold one strip bit and one cube bit for each of 8 groups: a ninth would alias the first's bits
      if (verdict_[i] == kTaken && ++candidates > kMaxGroups) verdict_[i] = kTooMany;
    }
    // 2. ONE pass over the frame's ops with a tag per image id (a candidate's strip: bit i, its cube: bit 8 + i): who reads, writes or copies the strip / the cube
    uint32_t max_id = 0;
    for (size_t i = 0; i < n && i < 16; ++i)
      if (verdict_[i] == kTaken) max_id = std::max(max_id, std::max(found_[i].strip, found_[i].cube));
    tag_.assign(size_t(max_id) + 1, 0);
    uint32_t first_set = 0xFFFFFFFFu;   // the first candidate's SetTargets: where the dispatch would be issued
    for (size_t i = 0; i < n && i < 16; ++i)
      if (verdict_[i] == kTaken) {
        tag_[found_[i].strip] |= uint32_t(1u << i), tag_[found_[i].cube] |= uint32_t(0x100u << i);
        first_set = std::min(first_set, found_[i].set_op);
      }
    const auto tag = [&](uint32_t image) -> uint32_t { return image < tag_.size() ? tag_[image] : uint32_t(0); };
    const auto mine = [&](size_t i, uint32_t p) { return p >= found_[i].set_op && p <= found_[i].copy_op[kFaces - 1]; };
    const auto fail = [&](size_t i, Why why) {
      if (verdict_[i] == kTaken) verdict_[i] = why;
    };
    strip_reads_.assign(n, 0);
    between_.assign(n, 0);
    const bool phantom = Planted(Fault::PhantomReader);
    bool phantom_used = false;
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      // a read of a cube: counts when it is after the dispatch's position and before the cube's own slot
      const auto cube_read = [&](uint32_t m) {
        for (size_t i = 0; i < n && i < 16; ++i)
          if ((m & (0x100u << i)) && verdict_[i] == kTaken && p > first_set && p < found_[i].set_op) ++between_[i];
      };
      if (op.kind == OpKind::Draw) {
        const Draw& d = f.draws[op.index];
        for (uint32_t s = 0; s < d.texture_count && d.texture_first + s < f.texture_slots.count; ++s) {
          const uint32_t m = tag(f.texture_slots[d.texture_first + s].image);
          if (!m) continue;
          for (size_t i = 0; i < n && i < 16; ++i)
            if (m & (1u << i)) ++strip_reads_[i];
          cube_read(m);
        }
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = f.copies[op.index];
        const uint32_t ms = tag(c.source.image), md = tag(c.dest.image);
        if (ms | md) {
          for (size_t i = 0; i < n && i < 16; ++i) {
            if ((ms & (1u << i)) && !mine(i, p)) ++strip_reads_[i];
            if (md & (1u << i)) fail(i, kStripWritten);
            if ((md & (0x100u << i)) && !mine(i, p)) fail(i, kCubeWritten);
          }
          cube_read(ms);
        }
      } else if (op.kind == OpKind::Present) {
        const uint32_t m = tag(f.presents[op.index].source.image);
        if (m) {
          for (size_t i = 0; i < n && i < 16; ++i)
            if (m & (1u << i)) ++strip_reads_[i];
          cube_read(m);
        }
      } else if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = f.targets[op.index];
        for (uint32_t c = 0; c <= t.color_count; ++c) {   // the colour targets, then the depth target
          const uint32_t image = c < t.color_count ? t.color[c].image : t.depth.image;
          const uint32_t m = tag(image);
          if (!m) continue;
          for (size_t i = 0; i < n && i < 16; ++i) {
            if (p == found_[i].set_op) continue;   // the group's own strip target
            if (m & (1u << i)) fail(i, kStripWritten);
            if (m & (0x100u << i)) fail(i, c < t.color_count ? kCubeWritten : kStripWritten);
          }
        }
      }
    }
    if (phantom)   // planted: the second candidate's cube is read once between the dispatch and its slot
      for (size_t i = 1; i < n && i < 16 && !phantom_used; ++i)
        if (verdict_[i] == kTaken && found_[i].set_op > first_set) ++between_[i], phantom_used = true;
    const bool check_between = !Planted(Fault::NoReaderCheck);
    for (size_t i = 0; i < n && i < 16; ++i) {
      if (strip_reads_[i]) fail(i, kStripRead);
      if (between_[i] && check_between) fail(i, kReadBetween);
    }
    for (const ContentRef& c : f.contents) {
      const uint32_t m = tag(c.image);
      if (m)
        for (size_t i = 0; i < n && i < 16; ++i)
          if (m & (0x101u << i)) fail(i, kContent);
    }
    for (size_t i = 0; i < n && i < 16; ++i)
      if (verdict_[i] == kTaken && (copy_alias::WasReadBack(found_[i].strip) || copy_alias::WasReadBack(found_[i].cube))) fail(i, kReadback);
    // 3. the plan
    for (size_t i = 0; i < n; ++i) {
      Why why = verdict_[i];
      if (why == kTaken && !plan.groups.empty() && std::memcmp(found_[i].k + 15 * 4, plan.groups[0].k + 15 * 4, 16) != 0) why = kClipTransform;   // one direction table per dispatch: another mapping keeps the stream's bake
      ++census.declined[why];
      if (why != kTaken) continue;
      ++census.taken;
      if (between_[i]) ++census.taken_read_between;
      if (overlapping_[i]) ++census.taken_overlapping;
      Group g = found_[i];
      if (plan.groups.empty()) plan.first_op = g.set_op;
      plan.groups.push_back(g);
      ++plan.count;
      if (!Planted(Fault::KeepOps)) {
        plan.drop[g.set_op] = plan.drop[g.draw_op] = 1;
        for (uint32_t c : g.copy_op) plan.drop[c] = 1;
      }
    }
    if (plan.count) {
      ++census.frames_taken;
      if (Planted(Fault::StaleK)) {   // the planted twin: the dispatch gets the first planned frame's constants for the whole run
        if (prev_k.size() == kVec4 * 4)
          for (Group& g : plan.groups) std::memcpy(g.k, prev_k.data(), sizeof(g.k));
        else
          prev_k.assign(plan.groups[0].k, plan.groups[0].k + kVec4 * 4);
      }
    }
  }

  std::string Report() const {
    char b[1100];
    std::string reasons;
    for (int w = 1; w < kWhyCount; ++w)
      if (census.declined[w]) {
        std::snprintf(b, sizeof b, "%s%s %llu", reasons.empty() ? "" : "; ", WhyName(w), static_cast<unsigned long long>(census.declined[w]));
        reasons += b;
      }
    std::snprintf(b, sizeof b,
                  "[rdr][prelight_compute] frames %llu (with slots %llu, taken %llu), groups %llu, taken %llu, declined %llu%s%s | dispatches %llu, cubes written %llu, ops dropped %llu | audit: taken read between %llu, taken overlapping %llu | planner %.1f us a frame, meshes checked %llu",
                  static_cast<unsigned long long>(census.frames), static_cast<unsigned long long>(census.frames_with_groups), static_cast<unsigned long long>(census.frames_taken),
                  static_cast<unsigned long long>(census.groups), static_cast<unsigned long long>(census.taken), static_cast<unsigned long long>(census.groups - census.taken), reasons.empty() ? "" : " (", reasons.c_str(),
                  static_cast<unsigned long long>(census.dispatches), static_cast<unsigned long long>(census.cubes_written), static_cast<unsigned long long>(census.ops_dropped),
                  static_cast<unsigned long long>(census.taken_read_between), static_cast<unsigned long long>(census.taken_overlapping), census.plan_runs ? double(census.plan_ns) / 1000.0 / double(census.plan_runs) : 0.0,
                  static_cast<unsigned long long>(census.meshes_checked));
    std::string out = b;
    if (!reasons.empty()) out.insert(out.find(" | dispatches"), ")");
    return out;
  }

 private:
  std::vector<Group> found_;
  std::vector<Why> verdict_;
  std::vector<uint8_t> overlapping_;
  std::vector<uint32_t> tag_, strip_reads_, between_;

  // The ops from a SetTargets up to the next one: [Draw (pre-light call site), Copy x 6], nothing else. The rest is judged later (so a miss is counted by its reason).
  static void FindGroups(const LoadedFrame& f, std::vector<Group>& out) {
    const uint32_t n = f.ops.count;
    for (uint32_t p = 0; p < n; ++p) {
      if (f.ops[p].kind != OpKind::SetTargets) continue;
      uint32_t q = p + 1;
      while (q < n && f.ops[q].kind != OpKind::SetTargets) ++q;
      bool has_pre = false;   // a pre-light span's FIRST draw is the pre-light draw
      for (uint32_t r = p + 1; r < q; ++r)
        if (f.ops[r].kind == OpKind::Draw) {
          const DrawOrigin* o = f.OriginOf(f.ops[r].index);
          has_pre = o && o->call_site == kPreLightCallSite;
          break;
        }
      if (!has_pre) { p = q - 1; continue; }
      Group g;
      g.set_op = p;
      const rdr::TargetsOp& t = f.targets[f.ops[p].index];
      g.strip = t.color_count ? t.color[0].image : 0;
      g.shape_ok = (q - p == 1 + 1 + kFaces) && t.color_count == 1;
      if (g.shape_ok) {
        g.shape_ok = f.ops[p + 1].kind == OpKind::Draw;
        g.draw_op = p + 1;
        for (uint32_t k = 0; k < kFaces && g.shape_ok; ++k) {
          g.shape_ok = f.ops[p + 2 + k].kind == OpKind::Copy;
          g.copy_op[k] = p + 2 + k;
        }
        if (g.shape_ok) {
          const DrawOrigin* o = f.OriginOf(f.ops[g.draw_op].index);
          g.shape_ok = o && o->call_site == kPreLightCallSite;
        }
        if (g.shape_ok) g.cube = f.copies[f.ops[g.copy_op[0]].index].dest.image;
      }
      out.push_back(g);
      p = q - 1;
    }
  }

  // The per-group checks that need no scan of the frame (the scan is Build's one pass).
  Why Judge(const LoadedFrame& f, const Ctx& ctx, Group& g, uint8_t& overlapping) {
    if (!ctx.enabled || !ctx.plan_ok) return kNotPlanned;
    if (!ctx.compute) return kNoCompute;
    if (!g.shape_ok) return kShape;
    const auto in_drop = [&](const uint8_t* set, uint32_t op) { return set && op < f.ops.count && set[op]; };
    uint32_t ops[2 + kFaces] = {g.set_op, g.draw_op};
    for (uint32_t k = 0; k < kFaces; ++k) ops[2 + k] = g.copy_op[k];
    bool dead = false, overlap = false;
    for (uint32_t op : ops) {
      dead = dead || in_drop(ctx.dead, op);
      for (const uint8_t* s : ctx.others) overlap = overlap || in_drop(s, op);
    }
    if (dead && !Planted(Fault::IgnoreOverlap)) return kDeadSkip;
    if (overlap && !Planted(Fault::IgnoreOverlap)) return kOverlap;
    overlapping = (dead || overlap) ? 1 : 0;
    const Draw& pd = f.draws[f.ops[g.draw_op].index];
    if (!ctx.is_prelight_sss || !ctx.is_prelight_sss(pd)) return kNotSss;
    const DrawFixed& fx = f.fixeds[pd.fixed];
    if (fx.state.depth_test || fx.state.depth_write || fx.state.stencil_test || fx.alpha_test || fx.state.blend[0].enable || fx.state.blend[0].write_mask != 15) return kState;
    const ImageRecord* strip = ctx.record ? ctx.record(g.strip) : nullptr;
    const rdr::CopyOp& c0 = f.copies[f.ops[g.copy_op[0]].index];
    g.cube = c0.dest.image;
    const ImageRecord* cube = ctx.record ? ctx.record(g.cube) : nullptr;
    if (!strip || !cube || strip->width != kStripW || strip->height != kStripH || strip->layers != 1 || strip->mips != 1 || cube->width != kFace || cube->height != kFace || cube->layers != kFaces || cube->mips != 1 ||
        cube->format != strip->format)
      return kImages;
    if (cube->format != 1) return kFormat;   // rdr::Format::RGBA8: the storage format the shader writes (BGRA storage is optional on a device, and the shader names rgba8)
    if (ctx.cube_has_storage && !ctx.cube_has_storage(g.cube)) return kNoStorage;
    // the six copies: this strip's tiles into this cube's layers by the identity map (tile j -> layer j, whole faces)
    uint32_t tile_seen = 0;
    for (uint32_t k = 0; k < kFaces; ++k) {
      const rdr::CopyOp& c = f.copies[f.ops[g.copy_op[k]].index];
      if (c.source.image != g.strip || c.source.mip != 0 || c.source.layer != 0 || c.dest.image != g.cube || c.dest.mip != 0 || c.dest_rect.x != 0 || c.dest_rect.y != 0 || c.dest_rect.width != kFace ||
          c.dest_rect.height != kFace || c.source_rect.y != 0 || c.source_rect.width != kFace || c.source_rect.height != kFace || c.source_rect.x < 0 || c.source_rect.x % int32_t(kFace) != 0)
        return kNotIdentity;
      const uint32_t tile = uint32_t(c.source_rect.x) / kFace, layer = c.dest.layer;
      if (tile >= kFaces || layer != tile) return kNotIdentity;
      tile_seen |= 1u << tile;
    }
    if (tile_seen != 0x3F) return kNotIdentity;
    // the clip transform: plain, and the texel -> grid mapping derived from it
    const DrawDynamic& dd = f.dynamics[pd.dynamic];
    const rdr::Rect& sc = dd.host.scissor;
    if (!(dd.ndc_scale[0] == 1.0f && (dd.ndc_scale[1] == 1.0f || dd.ndc_scale[1] == -1.0f) && dd.host.viewport[0] == 0.0f && dd.host.viewport[1] == 0.0f && dd.host.viewport[2] == float(kStripW) &&
          dd.host.viewport[3] == float(kStripH) && sc.x <= 0 && sc.y <= 0 && sc.x + int64_t(sc.width) >= int64_t(kStripW) && sc.y + int64_t(sc.height) >= int64_t(kStripH)))
      return kClipTransform;
    float screen[4];
    if (!ctx.fetch_screen || !ctx.fetch_screen(pd, screen)) return kNoConstants;
    const double sx = screen[0], sy = screen[1], oy = screen[3];
    double ox = screen[2];
    if (Planted(Fault::ShiftedScreen)) ox += 0.25 * 1.6 * sx;   // a quarter of a cell, in the draw's own clip units
    const double nsx = dd.ndc_scale[0], nsy = dd.ndc_scale[1], nox = Planted(Fault::HalfTexelDropped) ? 0.0 : double(dd.ndc_offset[0]), noy = Planted(Fault::HalfTexelDropped) ? 0.0 : double(dd.ndc_offset[1]);
    if (!(std::fabs(sx * nsx) > 1e-9 && std::fabs(sy * nsy) > 1e-9)) return kClipTransform;
    // the mesh puts tile v at x = 16 v .. 16 v + 16: a tile is 64 pixels, so 4 pixels a mesh unit (the mapping below is the tile 0's; the other tiles differ by exactly 16 v)
    if (std::fabs(double(kStripW) * nsx * sx / 2.0 - 4.0) > 1e-4) return kClipTransform;
    if (!ctx.mesh_ok || !ctx.mesh_ok(pd)) return kMesh;
    float raw[kVec4 * 4];
    if (!ctx.fetch_k || !ctx.fetch_k(pd, raw)) return kNoConstants;
    prelight_analytic::Planner::Precompute(raw, g.k);
    const double cell = 1.6;   // a grid cell is 1.6 mesh units
    const double px_to_clip_x = 2.0 / double(kStripW), px_to_clip_y = 2.0 / double(kStripH);
    const double xs = px_to_clip_x / (nsx * sx * cell), xo = ((0.5 * px_to_clip_x - 1.0 - nox) / nsx - ox) / (sx * cell);
    const double ys = -px_to_clip_y / (nsy * sy * cell), yo = -(((0.5 * px_to_clip_y - 1.0 - noy) / nsy - oy) / (sy * cell));
    // review F3: BOTH axes must be the game's mapping (one texel = 1/6.4 of a cell) and keep every texel's grid coordinate inside the mesh [0, 10] (the direction table is made from K[15] alone and is only valid there)
    const auto axis_ok = [](double scale, double offset) {
      const double a = offset, b = 63.0 * scale + offset;
      return std::fabs(std::fabs(scale) - 1.0 / 6.4) < 1e-4 && std::min(a, b) >= -1e-4 && std::max(a, b) <= 10.0 + 1e-4;
    };
    if (!axis_ok(xs, xo) || !axis_ok(ys, yo)) return kClipTransform;
    g.k[15 * 4 + 0] = float(xs), g.k[15 * 4 + 1] = float(xo), g.k[15 * 4 + 2] = float(ys), g.k[15 * 4 + 3] = float(yo);
    return kTaken;
  }
};

}  // namespace rdr_adapter::prelight_compute
