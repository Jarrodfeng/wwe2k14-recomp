// S102 lane L1 (docs/renderer/PRELIGHT_DIRECT.md section 8, docs/research/part2/R17_look_levers.md Brief A): the pre-light cube bake is NOT DRAWN; the character shaders evaluate its four lights per pixel.
// No game or SDK includes.
//
// The game bakes the lights into a 64x64x6 cube five times a frame (a slot = SetTargets(384x64 strip), one yPreLight 'sss' draw of call site 0x82C20B90, six strip->layer copies) and the character
// materials (yCh_skin, yCh_gear, yCh_trans families) sample the cube at the surface normal (fetch slot 13). The cube is a function of one draw's constants (yPreLight.frag, FX_SSS: four directional
// lights, a clamp, a pull towards the mean, an SSS alpha), so a reader can evaluate that function itself: its fragment program is swapped for the `<name>_pl` twin the shader compiler makes
// (fx_prelight.glsl; FX_PRELIGHT_ANALYTIC), whose block member PreLightK (18 vec4) the binder fills from the group's own draw constants, and its slot-13 cube is not bound at all.
//
// A group (the 8 ops: SetTargets, the draw, six copies) is TAKEN = none of its ops is recorded (G4's drop mechanism) when, in this frame:
//   - the plan is allowed (not an isolate / velocity bind, not a part of a split frame, not a lasting-only bind) and no other planner (G4's dead cut, P1's bake, the post claim, G3's turns, G4b's blank
//     chain, an own group) already drops one of its ops;
//   - the shape is exactly SetTargets + one pre-light draw + six copies, contiguous (as C1 / G14); the draw is the yPreLight 'sss' program with the plain state (no blend, full write mask, no depth,
//     stencil or alpha test); images are the strip (384x64) and a cube (64x64x6), the strip is read / written by nothing else, the cube is written by nothing else, no CPU content, no CPU read-back;
//   - EVERY draw of the frame that samples the cube at slot 13 has an analytic twin (a reader without one would sample a cube nobody baked: declined, counted) and comes AFTER the group (a reader
//     before the bake read last frame's cube); no draw samples the cube at another slot, no copy reads it, it is not a Present source; no thin draw samples slot 13 without showing its textures.
// A taken group with no reader is taken too (a dead cube: nothing in the frame reads it).
// The binder (binder.h, `// S102 L1:` lines) swaps each reader's program and constants at the late point of BindDraw (after the replace seam, so the hooks see the stream's draw as it was).
//
// Constants: PreLightK is PRECOMPUTED on the CPU (Precompute below) from the bake's raw registers into the form fx_prelight.glsl evaluates with the fewest per-pixel ops (the layout is its contract):
// K[0..2] the light directions transposed (x, y, z of the four lights), [3] the colour luma of each light, [4] that times DifLghMaxA.y, [5] (1 - DifLghPrm.x) / 2, [6..8] the light colours transposed (r, g, b
// of the four) times DifLghMaxD.xyz, [9..11] lobe A (4 tune.x, pivot, ceiling, offset | shape.y, -shape.z | the three knots as a quadratic polynomial), [12..14] lobe B, [15..16] unused, [17].x test flag.
// FRAMES WITHOUT A BAKE (the game bakes the cubes on some frames only): the planner remembers, per cube whose bake it dropped, that bake's constants, and twins the readers of every later frame with them
// until the stream bakes the cube again (also after the switch goes off, and for a reader that comes before the bake).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "capture_reader.h"
#include "copy_alias.h"

namespace rdr_adapter::prelight_analytic {

// fview / rplay: --prelight-analytic (the table's fallback until the first refresh; live reads the PerFrame table every frame).
inline std::atomic<bool> g_fview{false};
inline int64_t g_off_from = -1;   // fview TEST ONLY (--prelight-analytic-off-from N): the switch reads off N frames after the first bound one (the switch-off plant)

inline constexpr uint32_t kPreLightCallSite = 0x82C20B90u;   // sub_82C20AB8's draw (db: prelight_slots.md)
inline constexpr uint32_t kStripW = 384, kStripH = 64, kFace = 64, kFaces = 6;
inline constexpr uint32_t kReaderSlot = 13;                  // the light cube's fetch slot in the character shaders
inline constexpr uint32_t kRegisterBase = 232;               // compile_shaders.py ANALYTIC_REGISTER: the twin's PreLightK range (the binder overwrites it)
inline constexpr uint32_t kVec4 = 18;                        // PreLightK's size in vec4
inline constexpr uint8_t kNoGroup = 0xFF;

// Planted faults (tools only). Each must be CAUGHT by tools\l1\l1_check.py.
enum class Fault : uint8_t {
  None,
  LightMissing,    // light 0's colour is zero in the constants (the picture must differ)
  WrongSpace,      // the light directions are rotated (x, y, z) -> (y, z, x): the wrong light space (the picture must differ)
  AlphaDropped,    // DifLghMaxA.y = 0: the scattering term is dropped (the picture must differ)
  StaleK,          // the readers keep the FIRST planned frame's constants for the whole run (a slot never refreshed: the picture must differ where the lights move)
  KeepBake,        // the readers are twinned but the bake is still drawn: the switch's count claim (ops / copies fall) must FAIL
  PhantomReader,   // a synthetic reader without a twin is added to the first group (the group must be declined)
  NoReaderCheck,   // the reader check is skipped: with PhantomReader the group is taken although a reader would sample the unbaked cube (taken_with_untwinned must stay 0: it does not)
  IgnoreOverlap,   // the other planners' drop sets are not looked at (with a planted G4 cut a group is taken although its ops are dropped: taken_overlapping)
  Quantise,        // TEST ONLY (not a fault: the measuring arm): the shader rounds the result to 8 bits as the RGBA8 cube stored it, so what is left of the picture difference is the cube's sampling, not its precision
  FloatFormat,     // the planner sees a float-format cube (a format the 8-bit bake clamp would not match): every group must be DECLINED ("format")
  EarlyReader,     // the first reader of every group is treated as coming BEFORE the bake (the reorder plant): with the cube memory it is twinned with the remembered lights
  NoTwin,          // the readers keep the stream's program and cube although the bake is not drawn (the cube nobody baked is sampled): the pictures must differ, readers_missed counts it
  Count
};
inline Fault g_fault = Fault::None;
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "light_missing", "wrong_space", "alpha_dropped", "stale_k", "keep_bake", "phantom_reader", "no_reader_check", "ignore_overlap", "quantise", "float_format", "early_reader", "no_twin"};
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
  kTaken = 0, kNotPlanned, kDeadSkip, kOverlap, kShape, kNotSss, kState, kImages, kStripRead, kStripWritten, kCubeWritten, kContent, kReadback, kNoConstants,
  kReaderUntwinned, kReaderOtherSlot, kReaderBefore, kCubeCopied, kThinBlind, kFormat, kSwitchOff, kWhyCount
};
inline const char* WhyName(int w) {
  static const char* const k[] = {"taken", "frame not planned (isolate / velocity / part of a split frame)", "G4's dead skip took the slot", "another planner's drop set holds an op", "not SetTargets + one pre-light draw + six copies",
                                  "the draw is not the yPreLight sss program", "the draw's state is not plain (blend / write mask / depth / stencil / alpha test)", "images are not strip + cube", "the strip is read by something else",
                                  "the strip is written by something else", "the cube is written by something else", "CPU content in the frame", "the strip or cube was read back by the CPU", "no constants for the draw",
                                  "a reader of the cube has no analytic twin", "the cube is sampled at another slot", "a reader comes before the bake", "the cube is a copy source / Present source", "a thin draw samples slot 13 unseen", "the cube is not an 8-bit UNORM RGBA / BGRA image", "the switch is off (the stream bakes; remembered cubes are twinned until it does)"};
  return w >= 0 && w < kWhyCount ? k[w] : "?";
}

struct Group {
  uint32_t set_op = 0, draw_op = 0, copy_op[kFaces] = {};
  uint32_t strip = 0, cube = 0;     // the game's image ids
  float k[kVec4 * 4] = {};          // the constants the readers evaluate (PreLightK)
  uint32_t readers = 0;
  bool shape_ok = false;
  bool overlapping = false;         // some (not all) ops of the group are in another planner's drop set (taken only with the planted ignore_overlap)
  bool elsewhere = false;           // ALL 8 ops are dropped by another planner already (dead cut, G3 turn, ...): taken without dropping anything, the readers get fresh constants
};

struct Plan {
  std::vector<uint8_t> drop;           // by op index: 1 = not recorded (the taken groups' 8 ops each)
  std::vector<uint8_t> reader_group;   // by draw index: the group whose constants the draw evaluates (kNoGroup: not a reader)
  std::vector<Group> groups;           // the taken groups
  uint32_t count = 0;                  // groups taken
  uint32_t readers = 0;
  bool on() const { return count > 0 || readers > 0; }   // a taken group, or readers of a cube whose last bake was dropped (no group this frame)
  void Reset(uint32_t ops, uint32_t draws) {
    drop.assign(ops, 0), reader_group.assign(draws, kNoGroup), groups.clear(), count = 0, readers = 0;
  }
};

struct Census {
  uint64_t frames = 0, frames_with_groups = 0, frames_taken = 0;
  uint64_t groups = 0, taken = 0;
  uint64_t declined[kWhyCount] = {};
  uint64_t ops_dropped = 0, readers_planned = 0, readers_twinned = 0, readers_missed = 0, bake_draws_dropped = 0, copies_dropped = 0;
  uint64_t taken_with_untwinned = 0;   // the audit: a taken group with a reader that has no twin (NoReaderCheck + PhantomReader makes it > 0)
  uint64_t taken_overlapping = 0;      // the audit: a taken group with an op in another planner's drop set
  uint64_t taken_elsewhere = 0;        // groups taken because another planner drops all their ops (the cube is not baked this frame either)
  uint64_t orphan_readers = 0, stale_reads = 0;   // readers of a cube whose last bake was dropped in a frame with no bake of it / such a cube read by something that cannot be twinned (the audit wants 0)
  uint64_t plan_ns = 0, plan_runs = 0; // the planner's own time (a frame with the switch on): two clock reads a frame, nothing per draw
};

struct Ctx {
  bool real_frame = false;  // a guest frame with ops (not an isolate / velocity bind). A lasting-only bind and either part of a split frame ARE planned: twinning drops no op, and a taken group's ops are dropped like any plan's
  bool enabled = false;     // the switch is on this frame (off: groups are never taken, remembered cubes are twinned until the stream bakes them)
  uint64_t guest_frame = 0;
  std::function<const ImageRecord*(uint32_t)> record;
  const uint8_t* dead = nullptr;                    // G4's drop set (by op index), may be null
  std::vector<const uint8_t*> others;               // the other planners' drop sets
  std::function<bool(const Draw&)> is_prelight_sss;                       // the draw's programs are yPreLight base + sss
  std::function<bool(const Draw&, float* k)> fetch_k;                     // the draw's PS bank -> PreLightK (kVec4 * 4 floats)
  // 0: the draw's program does not sample `slot` as a cube (not a reader); 1: it does and has an analytic twin (slot 13 only); 2: it does and cannot be twinned
  std::function<int(uint32_t draw_index, uint32_t slot)> reader_kind;
};

class Planner {
 public:
  Census census;
  std::vector<float> prev_k;   // the previous planned frame's constants of its first group (the StaleK twin)

 private:
  static constexpr size_t kMaxGroups = 8;   // the tag word holds one strip bit and one cube bit per group
  struct Reader {
    uint32_t draw, group;
  };
  std::vector<Group> found_;
  std::vector<Why> verdict_;
  std::vector<uint32_t> tag_;
  std::unordered_map<uint32_t, std::vector<float>> unbaked_;   // cube id -> PreLightK of its last bake, which was dropped: a frame with no bake of it still has readers that need those lights
  std::vector<uint32_t> strip_reads_, untwinned_;
  std::vector<Reader> readers_, orphan_readers_;
  std::vector<uint32_t> orphan_ids_;
  std::vector<int> cover_;
  std::vector<uint8_t> early_done_;
  std::vector<uint8_t> orphan_bad_;
  std::vector<uint8_t> index_;

 public:

  bool HasMemory() const { return !unbaked_.empty(); }
  void Forget() { unbaked_.clear(); }   // (not called when the switch goes off: remembered cubes stay twinned until the stream bakes them)

  void Build(const LoadedFrame& f, const Ctx& ctx, Plan& plan) {
    plan.Reset(f.ops.count, f.draws.count);
    ++census.frames;
    if (!ctx.real_frame || !f.ops.count || !f.origins.count) return;
    std::vector<Group>& found = found_;
    found.clear();
    FindGroups(f, found);
    if (found.empty() && unbaked_.empty()) return;
    if (!found.empty()) ++census.frames_with_groups;
    const size_t n = found.size();
    std::vector<Why>& verdict = verdict_;
    verdict.assign(n, kTaken);
    // 1. the group-level judgement (no scan of the frame)
    for (size_t i = 0; i < n; ++i) {
      ++census.groups;
      verdict[i] = !ctx.enabled ? kSwitchOff : i < kMaxGroups ? Judge(f, ctx, found[i]) : kShape;   // (more than kMaxGroups candidates in a frame: the rest keep the stream's bake)
    }
    // 2. ONE pass over the frame's ops with a tag per image id (a taken group's strip: bit i, its cube: bit 8 + i; a remembered cube: bit 16 + j): who reads, writes or copies the strip / the cube, and
    //    every reader of a cube in op order.
    uint32_t max_id = 0;
    for (size_t i = 0; i < n && i < kMaxGroups; ++i)
      if (verdict[i] == kTaken) max_id = std::max(max_id, std::max(found[i].strip, found[i].cube));
    orphan_ids_.clear();
    for (const auto& kv : unbaked_)
      if (orphan_ids_.size() < kMaxGroups) orphan_ids_.push_back(kv.first), max_id = std::max(max_id, kv.first);
    cover_.assign(orphan_ids_.size(), -1);   // the group of this frame that bakes remembered cube j (any verdict: the stream bakes it when the group is declined)
    for (size_t j = 0; j < orphan_ids_.size(); ++j)
      for (size_t i = 0; i < n; ++i)
        if (found[i].shape_ok && found[i].cube == orphan_ids_[j]) cover_[j] = int(i);
    tag_.assign(size_t(max_id) + 1, 0);
    for (size_t j = 0; j < orphan_ids_.size(); ++j) tag_[orphan_ids_[j]] |= uint32_t(0x10000u << j);
    orphan_bad_.assign(orphan_ids_.size(), 0);
    orphan_readers_.clear();
    for (size_t i = 0; i < n && i < kMaxGroups; ++i)
      if (verdict[i] == kTaken) tag_[found[i].strip] |= uint32_t(1u << i), tag_[found[i].cube] |= uint32_t(0x100u << i);
    const auto tag = [&](uint32_t image) -> uint32_t { return image < tag_.size() ? tag_[image] : uint32_t(0); };
    const auto mine = [&](size_t i, uint32_t p) { return p >= found[i].set_op && p <= found[i].copy_op[kFaces - 1]; };
    const auto fail = [&](size_t i, Why why) {
      if (verdict[i] == kTaken) verdict[i] = why;
    };
    strip_reads_.assign(n, 0);
    untwinned_.assign(n, 0);
    readers_.clear();
    const bool phantom = Planted(Fault::PhantomReader);
    const bool early = Planted(Fault::EarlyReader) && census.frames >= 10;   // (from the 10th planned frame on: by then the memory holds the cubes)
    std::vector<uint8_t>& early_done = early_done_;
    early_done.assign(n, 0);
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      if (op.kind == OpKind::Draw) {
        const uint32_t di = op.index;
        const Draw& d = f.draws[di];
        bool saw13 = false;
        for (uint32_t s = 0; s < d.texture_count && d.texture_first + s < f.texture_slots.count; ++s) {
          const TextureSlot& t = f.texture_slots[d.texture_first + s];
          if (!t.image) continue;
          const uint32_t m = tag(t.image);
          if (!m) continue;
          bool as_orphan = false;   // this draw's read of the cube is twinned with the REMEMBERED lights (the cube holds the last dropped bake): no group of this frame has baked it yet, or none does
          for (size_t j = 0; j < orphan_ids_.size(); ++j) {
            if (!(m & (0x10000u << j))) continue;
            const int g = cover_[j];
            bool before = g < 0 || p < found[size_t(g)].copy_op[kFaces - 1];   // (a declined / not-taken group: the stream's bake is recorded and later readers read it)
            if (g >= 0 && early && !early_done[size_t(g)] && p > found[size_t(g)].copy_op[kFaces - 1] && ctx.reader_kind && ctx.reader_kind(di, t.slot) == 1 && t.slot == kReaderSlot) early_done[size_t(g)] = 1, before = true;   // the planted reorder
            if (!before) continue;
            const int kind = ctx.reader_kind ? ctx.reader_kind(di, t.slot) : 2;
            if (kind == 0) continue;
            if (t.slot == kReaderSlot && kind == 1) orphan_readers_.push_back({di, uint32_t(j)}), saw13 = true, as_orphan = true;
            else orphan_bad_[j] = 1;
          }
          for (size_t i = 0; i < n && i < kMaxGroups; ++i) {
            if (m & (1u << i)) ++strip_reads_[i];   // a draw's texture slot holds the strip
            if (!(m & (0x100u << i)) || verdict[i] != kTaken) continue;
            const int kind = ctx.reader_kind ? ctx.reader_kind(di, t.slot) : 2;
            if (kind == 0) continue;   // the draw's program does not sample that slot: not a reader
            if (t.slot != kReaderSlot) {
              fail(i, kReaderOtherSlot);
              continue;
            }
            saw13 = true;
            if (kind != 1) {
              ++untwinned_[i];
              continue;
            }
            if (p < found[i].copy_op[kFaces - 1]) {
              if (!as_orphan) fail(i, kReaderBefore);   // before the bake and no remembered lights for the cube: the stream's bake stays
              continue;
            }
            if (as_orphan) continue;   // (the planted reorder already handed this read to the remembered lights)
            readers_.push_back({di, uint32_t(i)});
          }
        }
        if ((d.flags & kDrawThin) && !saw13 && ctx.reader_kind && ctx.reader_kind(di, kReaderSlot) != 0)   // a thin draw that samples slot 13 but shows no texture there: its cube is unknown
          for (size_t i = 0; i < n; ++i) fail(i, kThinBlind);
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = f.copies[op.index];
        const uint32_t ms = tag(c.source.image), md = tag(c.dest.image);
        if (ms | md)
          for (size_t i = 0; i < n && i < kMaxGroups; ++i) {
            if ((ms & (1u << i)) && !mine(i, p)) ++strip_reads_[i];
            if (ms & (0x100u << i)) fail(i, kCubeCopied);
            if (md & (1u << i)) fail(i, kStripWritten);
            if ((md & (0x100u << i)) && !mine(i, p)) fail(i, kCubeWritten);
          }
      } else if (op.kind == OpKind::Present) {
        const uint32_t m = tag(f.presents[op.index].source.image);
        if (m)
          for (size_t i = 0; i < n && i < kMaxGroups; ++i) {
            if (m & (1u << i)) ++strip_reads_[i];
            if (m & (0x100u << i)) fail(i, kCubeCopied);
          }
      } else if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = f.targets[op.index];
        for (uint32_t c = 0; c <= t.color_count; ++c) {   // the colour targets, then the depth target
          const uint32_t image = c < t.color_count ? t.color[c].image : t.depth.image;
          const uint32_t m = tag(image);
          if (!m) continue;
          for (size_t i = 0; i < n && i < kMaxGroups; ++i) {
            if (p == found[i].set_op) continue;   // the group's own strip target
            if (m & (1u << i)) fail(i, kStripWritten);
            if (m & (0x100u << i)) fail(i, c < t.color_count ? kCubeWritten : kStripWritten);
          }
        }
      }
    }
    for (size_t i = 0; i < n && i < kMaxGroups; ++i)
      if (strip_reads_[i]) fail(i, kStripRead);
    for (const ContentRef& c : f.contents) {
      const uint32_t m = tag(c.image);
      if (m)
        for (size_t i = 0; i < n && i < kMaxGroups; ++i)
          if (m & (0x101u << i)) fail(i, kContent);
    }
    for (size_t i = 0; i < n && i < kMaxGroups; ++i)
      if (verdict[i] == kTaken && (copy_alias::WasReadBack(found[i].strip) || copy_alias::WasReadBack(found[i].cube))) fail(i, kReadback);
    if (phantom)
      for (size_t i = 0; i < n; ++i)
        if (verdict[i] == kTaken) {
          ++untwinned_[i];
          break;
        }
    // 3. the plan
    std::vector<uint8_t>& index = index_;
    index.assign(n, kNoGroup);
    const bool check = !Planted(Fault::NoReaderCheck);
    for (size_t i = 0; i < n; ++i) {
      Why why = verdict[i];
      if (why == kTaken && untwinned_[i]) {
        if (check) why = kReaderUntwinned;
        else ++census.taken_with_untwinned;
      }
      verdict[i] = why;   // (the final verdict: the memory below reads it)
      if (ctx.enabled) ++census.declined[why];
      if (why != kTaken) continue;
      ++census.taken;
      if (found[i].overlapping) ++census.taken_overlapping;
      if (found[i].elsewhere) ++census.taken_elsewhere;
      Group g = found[i];
      for (const Reader& r : readers_)
        if (r.group == i) ++g.readers;
      index[i] = uint8_t(plan.groups.size());
      plan.groups.push_back(g);
      ++plan.count;
      if (!Planted(Fault::KeepBake) && !g.elsewhere) {
        plan.drop[g.set_op] = plan.drop[g.draw_op] = 1;
        for (uint32_t c : g.copy_op) plan.drop[c] = 1;
      }
    }
    for (const Reader& r : readers_)
      if (index[r.group] != kNoGroup) plan.reader_group[r.draw] = index[r.group], ++plan.readers;
    // the remembered cubes' readers (before the memory is updated: they use the lights the cube holds NOW, i.e. the last dropped bake)
    for (size_t j = 0; j < orphan_ids_.size(); ++j) {
      if (orphan_bad_[j]) {
        ++census.stale_reads;
        continue;
      }
      bool any = false;
      for (const Reader& r : orphan_readers_) any = any || r.group == j;
      if (!any) continue;
      Group g;
      std::memcpy(g.k, unbaked_[orphan_ids_[j]].data(), sizeof(g.k));
      g.cube = orphan_ids_[j];
      const uint8_t idx = uint8_t(plan.groups.size());
      plan.groups.push_back(g);
      for (const Reader& r : orphan_readers_)
        if (r.group == j) plan.reader_group[r.draw] = idx, ++plan.readers, ++census.orphan_readers;
    }
    // the cube memory: a cube whose bake was taken (dropped here, or by another planner already) holds these lights from now on; a cube whose stream bake IS recorded (the group declined, or the switch off)
    // holds the stream's bake and is forgotten. A cube whose ops another planner drops in part is forgotten too (what it holds is unknown).
    for (size_t i = 0; i < n; ++i) {
      if (!found[i].cube || !found[i].shape_ok) continue;
      if (verdict[i] == kTaken) unbaked_[found[i].cube].assign(found[i].k, found[i].k + kVec4 * 4);
      else unbaked_.erase(found[i].cube);
    }
    for (size_t j = 0; j < orphan_ids_.size(); ++j)
      if (orphan_bad_[j] && cover_[j] < 0) unbaked_.erase(orphan_ids_[j]);   // (read by something that cannot be twinned and no group re-bakes it)
    census.readers_planned += plan.readers;
    if (plan.count) {
      ++census.frames_taken;
      if (Planted(Fault::StaleK) && !plan.groups.empty()) {   // the planted twin: the readers get the previous planned frame's constants
        if (prev_k.size() == kVec4 * 4)
          for (Group& g : plan.groups) std::memcpy(g.k, prev_k.data(), sizeof(g.k));
        else
          prev_k.assign(found[0].k, found[0].k + kVec4 * 4);   // (frozen at the FIRST planned frame: a slot whose constants were never refreshed)
      }
    }
  }

  std::string Report() const {
    char b[1000];
    std::string reasons;
    for (int w = 1; w < kWhyCount; ++w)
      if (census.declined[w]) {
        std::snprintf(b, sizeof b, "%s%s %llu", reasons.empty() ? "" : "; ", WhyName(w), static_cast<unsigned long long>(census.declined[w]));
        reasons += b;
      }
    std::snprintf(b, sizeof b,
                  "[rdr][prelight_analytic] frames %llu (with strips %llu, taken %llu), groups %llu, taken %llu, declined %llu%s%s | readers planned %llu, twinned %llu, missed %llu | bake draws dropped %llu, copies dropped %llu | audit: taken with an untwinned reader %llu, taken overlapping %llu | orphan readers %llu, stale reads %llu, taken with the ops already dropped %llu | planner %.1f us a frame",
                  static_cast<unsigned long long>(census.frames), static_cast<unsigned long long>(census.frames_with_groups), static_cast<unsigned long long>(census.frames_taken),
                  static_cast<unsigned long long>(census.groups), static_cast<unsigned long long>(census.taken), static_cast<unsigned long long>(census.groups - census.taken), reasons.empty() ? "" : " (", reasons.c_str(),
                  static_cast<unsigned long long>(census.readers_planned), static_cast<unsigned long long>(census.readers_twinned), static_cast<unsigned long long>(census.readers_missed),
                  static_cast<unsigned long long>(census.bake_draws_dropped), static_cast<unsigned long long>(census.copies_dropped), static_cast<unsigned long long>(census.taken_with_untwinned),
                  static_cast<unsigned long long>(census.taken_overlapping), static_cast<unsigned long long>(census.orphan_readers), static_cast<unsigned long long>(census.stale_reads), static_cast<unsigned long long>(census.taken_elsewhere), census.plan_runs ? double(census.plan_ns) / 1000.0 / double(census.plan_runs) : 0.0);
    std::string out = b;
    if (!reasons.empty()) out.insert(out.find(" | readers planned"), ")");
    return out;
  }

 private:
  // The ops from a SetTargets up to the next one: [Draw (pre-light call site), Copy x 6], nothing else. The rest is judged later (so a miss is counted by its reason).
  static void FindGroups(const LoadedFrame& f, std::vector<Group>& out) {
    const uint32_t n = f.ops.count;
    for (uint32_t p = 0; p < n; ++p) {
      if (f.ops[p].kind != OpKind::SetTargets) continue;
      uint32_t q = p + 1;
      while (q < n && f.ops[q].kind != OpKind::SetTargets) ++q;
      bool has_pre = false;   // a pre-light span's FIRST draw is the pre-light draw (looking at every draw of every span cost ~50 us a frame)
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
  Why Judge(const LoadedFrame& f, const Ctx& ctx, Group& g) {
    if (!g.shape_ok) return kShape;
    const auto in_drop = [&](const uint8_t* set, uint32_t op) { return set && op < f.ops.count && set[op]; };
    uint32_t ops[2 + kFaces] = {g.set_op, g.draw_op};
    for (uint32_t k = 0; k < kFaces; ++k) ops[2 + k] = g.copy_op[k];
    uint32_t dropped = 0;   // how many of the 8 ops another planner (G4's dead cut, P1, post, G3's turns, G4b, an own group) drops
    for (uint32_t op : ops) {
      bool any = in_drop(ctx.dead, op);
      for (const uint8_t* s : ctx.others) any = any || in_drop(s, op);
      dropped += any ? 1u : 0u;
    }
    // ALL 8 dropped elsewhere: the cube is not baked this frame either, so it is TAKEN with fresh constants (the readers need them; nothing is dropped here). Some but not all: unknown state, declined.
    if (dropped == 8 && !Planted(Fault::IgnoreOverlap)) g.elsewhere = true;
    else if (dropped && !Planted(Fault::IgnoreOverlap)) return kOverlap;
    const bool overlapping = dropped != 0 && !g.elsewhere;
    const Draw& pd = f.draws[f.ops[g.draw_op].index];
    if (!ctx.is_prelight_sss || !ctx.is_prelight_sss(pd)) return kNotSss;
    {
      const DrawFixed& fx = f.fixeds[pd.fixed];
      if (fx.state.depth_test || fx.state.depth_write || fx.state.stencil_test || fx.alpha_test || fx.state.blend[0].enable || fx.state.blend[0].write_mask != 15) return kState;
    }
    const ImageRecord* strip = ctx.record ? ctx.record(g.strip) : nullptr;
    const rdr::CopyOp& c0 = f.copies[f.ops[g.copy_op[0]].index];
    g.cube = c0.dest.image;
    const ImageRecord* cube = ctx.record ? ctx.record(g.cube) : nullptr;
    if (!strip || !cube || strip->width != kStripW || strip->height != kStripH || strip->layers != 1 || strip->mips != 1 || cube->width != kFace || cube->height != kFace || cube->layers != kFaces || cube->mips != 1 ||
        cube->format != strip->format)
      return kImages;
    // F4: the analytic path clamps to 0..1 and is judged against an 8-bit bake: only an 8-bit UNORM RGBA / BGRA cube (rdr::Format 1 / 3) is taken
    const uint32_t format = Planted(Fault::FloatFormat) ? 16u : uint32_t(cube->format);
    if (format != 1 && format != 3) return kFormat;
    for (uint32_t k = 0; k < kFaces; ++k) {   // the six copies are this strip's tiles into this cube (their layout is not read: no face of the cube is made at all)
      const rdr::CopyOp& c = f.copies[f.ops[g.copy_op[k]].index];
      if (c.source.image != g.strip || c.dest.image != g.cube) return kShape;
    }
    float raw[kVec4 * 4];
    if (!ctx.fetch_k || !ctx.fetch_k(pd, raw)) return kNoConstants;
    ApplyConstantFault(raw);
    Precompute(raw, g.k);
    if (Planted(Fault::Quantise)) g.k[17 * 4] = 1.0f;
    g.overlapping = overlapping;
    return kTaken;
  }

 public:
  // F7: the constant-only terms of the bake's function, computed once per group on the CPU. raw = the bake's registers as ctx.fetch_k copies them (the old PreLightK layout: DifLghVec x4, DifLghCol x4,
  // DifLghMaxD, x of DifLghPrm x4, DifLghMaxA, SssLobeA x3, SssLobeB x3); k = the layout the header comment and fx_prelight.glsl name.
  static void Precompute(const float* raw, float* k) {
    std::memset(k, 0, kVec4 * 16);
    const float* vec = raw;            // 4 x vec4
    const float* col = raw + 4 * 4;    // 4 x vec4
    const float* maxd = raw + 8 * 4;
    const float* prm = raw + 9 * 4;    // x of each light's DifLghPrm
    const float* maxa = raw + 10 * 4;
    for (int i = 0; i < 4; ++i) {
      k[0 * 4 + i] = vec[i * 4 + 0], k[1 * 4 + i] = vec[i * 4 + 1], k[2 * 4 + i] = vec[i * 4 + 2];
      const float lum = col[i * 4 + 0] * 0.7553793787956238f + col[i * 4 + 1] * 0.2366930991411209f + col[i * 4 + 2] * 0.00792752020061016f;
      k[3 * 4 + i] = lum;
      k[4 * 4 + i] = lum * maxa[1];
      k[5 * 4 + i] = 0.5f * (1.0f - prm[i]);
      k[6 * 4 + i] = col[i * 4 + 0] * maxd[0], k[7 * 4 + i] = col[i * 4 + 1] * maxd[1], k[8 * 4 + i] = col[i * 4 + 2] * maxd[2];
    }
    for (int lobe = 0; lobe < 2; ++lobe) {
      const float* tune = raw + (11 + 3 * lobe) * 4;
      const float* knots = raw + (12 + 3 * lobe) * 4;
      const float* shape = raw + (13 + 3 * lobe) * 4;
      float* a = k + (9 + 3 * lobe) * 4;
      float* b = a + 4;
      float* c = a + 8;
      a[0] = 4.0f * tune[0], a[1] = tune[3], a[2] = tune[2], a[3] = tune[1];   // 4 tune.x, pivot, ceiling, offset
      b[0] = shape[1], b[1] = -shape[2];                                       // bias, -scale
      c[0] = knots[0] - 0.5f, c[1] = 2.0f * (knots[1] - knots[0]), c[2] = knots[0] - 2.0f * knots[1] + knots[2];   // the quadratic through three knots, minus the 0.5 the lobe subtracts
    }
  }

 private:
  static void ApplyConstantFault(float* k) {
    if (Planted(Fault::LightMissing)) k[4 * 4 + 0] = k[4 * 4 + 1] = k[4 * 4 + 2] = 0.0f;   // light 0's colour (raw layout; Bryan and Check B light the wrestlers with light 0)
    if (Planted(Fault::AlphaDropped)) k[10 * 4 + 1] = 0.0f;
    if (Planted(Fault::WrongSpace))
      for (int i = 0; i < 4; ++i) {
        float* v = k + i * 4;
        const float x = v[0], y = v[1], z = v[2];
        v[0] = y, v[1] = z, v[2] = x;
      }
  }
};

}  // namespace rdr_adapter::prelight_analytic
