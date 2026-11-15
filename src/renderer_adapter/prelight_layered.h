// S102 lane C1 (docs/renderer/PRELIGHT_DIRECT.md section 7): the pre-light strips' six copies GONE, not batched: ONE render pass per slot into the cube's six layers at once (multiview). No game or SDK includes.
//
// The game bakes each light cube in five slots a frame: one draw into a 384x64 strip target (six 64x64 faces side by side, the yPreLight pass, call site 0x82C20B90) and then SIX copies,
// strip tile j -> layer L(j) of a 64x64x6 cube texture (30 copies a frame). The strip exists only because the Xbox 360 had eDRAM: it is read by nothing else. With the owned pre-light draw
// (small_passes_ours) the strip is not made at all: the slot's SetTargets becomes ONE targets op over the cube's layers 0..5 (a multiview pass, view mask 0x3F), the draw is issued once with our
// `yPreLight_base_layered` vertex program (view v draws tile v onto the whole face), and the six copies are left out. Same five passes a frame as before, 30 copies fewer.
//
// FACE MAP [C, measured: tools/g14/g14_census.py on Bryan / Check B / Brock, every frame; docs/renderer/PRELIGHT_DIRECT.md section 1]: copy k of a group has source rect {64 j, 0, 64, 64} of the strip,
// dest rect {0, 0, 64, 64}, dest layer L. The map is IDENTITY (tile j -> layer j), no flip. The layered program draws tile v into layer v, so the planner REQUIRES the identity (it reads (tile, layer)
// from each group's own six copies and declines a group whose map is anything else: counted `not the identity map`).
//
// The plan (Build) is made once a frame, after every plan whose drop set it must not overlap (G4's dead pass, G3's turns, G4b's blank chain, P1's bake, the post claim, the own groups), and before
// the copy-alias plan (the group's copies are an op another hook drops: copy_alias Facts::op_excluded). A group is DECLINED, with a counter by reason, when:
//   - the switch is off / the frame is not planned (isolate, velocity bind) / the owned pre-light hook (small_passes_ours) is not on;
//   - the renderer has no multiview, or the layered vertex program is not in the registry (the fallback: the stream's SetTargets / draw / six copies run as they are, counted);
//   - any op of the group is in another planner's drop set (dead = G4's, others = G3 / G4b / P1 / post / own groups);
//   - the group is not exactly SetTargets, one draw of the pre-light call site, six copies, in that order and contiguous (no clear, no other op);
//   - the images are not the strip (384x64, one layer, one mip) and a cube (64x64, six layers, one mip, the strip's format), or the six copies are not the identity face map;
//   - the strip is read by anything else in the frame (a draw's texture slot, a copy source other than the group's six, the Present), is a copy destination or another target, has CPU content in
//     this frame, or was ever read back by the CPU; the cube is another target / copy destination in the frame;
//   - the cube image was made without target usage (the switch was off when the image was first made).
// A group that passes but whose draw is NOT drawn by small_passes_ours (the hook refused it) falls back AFTER the fact in the walk: the stream's SetTargets / draw / six copies are issued as
// they are (counted `fallback_not_ours`); the layered pass that was issued first is a wasted pass on those frames (counted), never a wrong picture.
#pragma once
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

namespace rdr_adapter::prelight_layered {

// fview / rplay: --prelight-direct (the table's fallback until the first refresh; live reads the PerFrame table every frame).
inline std::atomic<bool> g_fview{false};

inline constexpr uint32_t kPreLightCallSite = 0x82C20B90u;   // sub_82C20AB8's draw (db: prelight_slots.md)
inline constexpr uint32_t kStripW = 384, kStripH = 64, kFace = 64, kFaces = 6;
inline constexpr const char* kLayeredVertex = "yPreLight_base_layered_vertex";        // our vertex program: view v draws tile v onto its face (yPreLight.vert FX_LAYERED)
inline constexpr const char* kLayeredVertexSwap = "yPreLight_base_layered_swap_vertex";  // the planted twin's program: view v draws tile 5 - v

// Planted faults (tools only). Each must be CAUGHT by tools\g14\g14_check.py.
enum class Fault : uint8_t {
  None,
  SwapFaces,       // the layered pass draws tile 5 - v into layer v (the swap program): the cube faces must differ
  MissingLayer,    // the layered pass covers layers 0..4 only: layer 5 is never written (the cube hash must differ)
  KeepCopies,      // the six copies are issued too: the renderer's copy count must not fall (counts) and the copy of a strip that was never made breaks the faces
  FallbackNoStrip, // the "no multiview" fallback is taken (the group is declined for it) but the strip's ops are still left out: the stream's copies read a strip nobody made
  PhantomReader,   // a synthetic reader of the first group's strip is added (the strip scan must decline it)
  NoStripCheck,    // the strip-reader scan is skipped: with PhantomReader the group is taken although the strip is read (taken_with_reader must stay 0: it does not)
  IgnoreOverlap,   // the other planners' drop sets are not looked at (with a planted G4 cut the group is taken although its ops are dropped: taken_overlapping)
  NdcOffsetUnscaled, // the stream's x offset is NOT multiplied by 6 (the half-pixel rule is left at the strip's size: the cube differs)
  HalfViewport,    // Judge sees the host viewport at half width (a viewport that is not the strip's must be DECLINED, counted)
  NoGeometry,      // the layered draw is not issued although the hook said yes (C1 review F4: the group must fall back and equal the stream)
  BadPad,          // a synthetic pad = 6 on the first one-layer target of the frame (C1 review F8: a renderer Problem, no crash, an ordinary pass)
  ShrinkScissor,   // the face scissor is one texel smaller on each axis (C1 review F3: the missing row / column must show, the cube differs)
  Count
};
inline Fault g_fault = Fault::None;
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "swap_faces", "missing_layer", "keep_copies", "fallback_no_strip", "phantom_reader", "no_strip_check", "ignore_overlap", "ndc_offset_unscaled", "half_viewport", "no_geometry", "bad_pad", "shrink_scissor"};
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}
// A fault is a bit set: the twins that need two (phantom_reader + no_strip_check) set both. g_fault_bits is what the code reads; g_fault holds the first named.
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
  kTaken = 0, kNotPlanned, kHookOff, kNoMultiview, kDeadSkip, kOverlap, kShape, kClipTransform, kImages, kFaceMap, kNotIdentity, kStripRead, kStripWritten, kCubeWritten, kContent, kReadback, kNoTarget, kDepthUsed, kWhyCount
};
inline const char* WhyName(int w) {
  static const char* const k[] = {"taken", "frame not planned (isolate / velocity / part of a split frame)", "small_passes_ours is off", "no multiview (or no layered program): the stream keeps its strip and copies", "G4's dead skip took the slot",
                                  "another planner's drop set holds an op", "not SetTargets + one pre-light draw + six copies", "the draw's clip transform / viewport / scissor is not the strip's (ndc scale x 1, |y| 1, viewport 0 0 384 64, scissor covering it)", "images are not strip + cube", "copies are not a face map", "the face map is not the identity (tile j -> layer j)",
                                  "the strip is read by something else", "the strip is written by something else", "the cube is written by something else", "CPU content in the frame",
                                  "the strip was read back by the CPU", "the cube has no target usage", "the draw tests or writes depth / stencil"};
  return w >= 0 && w < kWhyCount ? k[w] : "?";
}

struct Face {
  uint32_t copy_op = 0;
  uint32_t tile = 0;    // the strip tile (source rect x = 64 * tile)
  uint32_t layer = 0;   // the cube layer
};
struct Group {
  uint32_t set_op = 0, draw_op = 0;
  uint32_t strip = 0, cube = 0;     // the game's image ids
  Face face[kFaces];
  bool fallback = false;            // set by the walk: the face-0 draw was not ours; the stream's ops are issued
  bool shape_ok = false;            // the ops are SetTargets, one pre-light draw, six copies (judged in Judge)
};

struct Plan {
  std::vector<Group> groups;
  std::vector<uint8_t> role;        // by op index: 0 none, 1 set, 2 draw, 3 copy
  std::vector<uint16_t> group_of;   // by op index
  uint32_t count = 0;               // groups taken
  void Reset(uint32_t ops) { groups.clear(), role.assign(ops, 0), group_of.assign(ops, 0), count = 0; }
};

struct Census {
  uint64_t frames = 0, frames_with_groups = 0, frames_taken = 0;   // a frame is "taken" when at least one group is
  uint64_t groups = 0, taken = 0;
  uint64_t declined[kWhyCount] = {};
  uint64_t passes_layered = 0, copies_dropped = 0, strips_unmade = 0, fallback_not_ours = 0;   // layered passes issued; copies left out; strip passes not issued (SetTargets + draw)
  uint64_t taken_with_reader = 0;    // the audit: a taken group whose strip an independent scan finds read (the planted no_strip_check + phantom_reader makes it > 0)
  uint64_t taken_overlapping = 0;    // the audit: a taken group with an op in another planner's drop set
};

struct Ctx {
  bool plan_ok = false;     // false: an isolate / velocity bind, a part of a split frame, nothing to plan
  bool hook_on = false;     // small_passes_ours is on and its hook is registered
  bool multiview = false;   // the renderer can render one pass into six layers AND the layered vertex program exists
  std::function<const ImageRecord*(uint32_t)> record;
  std::function<bool(uint32_t)> cube_has_target;   // the renderer image of this capture id was made with target usage
  const uint8_t* dead = nullptr;                   // G4's drop set (by op index), may be null
  std::vector<const uint8_t*> others;               // the other planners' drop sets
  uint64_t guest_frame = 0;
};

class Planner {
 public:
  Census census;

  void Build(const LoadedFrame& f, const Ctx& ctx, Plan& plan) {
    plan.Reset(f.ops.count);
    ++census.frames;
    if (!f.ops.count || !f.origins.count) return;
    // find the candidate groups first (a group needs no switch to be seen); only a frame that has one counts
    std::vector<Group> found;
    FindGroups(f, found);
    if (found.empty()) return;
    ++census.frames_with_groups;
    bool phantom_used = false;
    for (Group& g : found) {
      ++census.groups;
      const Why why = Judge(f, ctx, g, plan, phantom_used);
      ++census.declined[why];
      if (why == kNoMultiview && Planted(Fault::FallbackNoStrip)) {   // planted: the fallback is counted as declined, but the strip pass is left out all the same (the stream's copies then read a strip nobody made)
        const uint16_t idx = uint16_t(plan.groups.size());
        plan.groups.push_back(g);
        plan.role[g.set_op] = 4, plan.role[g.draw_op] = 4;
        plan.group_of[g.set_op] = plan.group_of[g.draw_op] = idx;
        ++plan.count;   // (so the walk acts on the roles; the census counts it as declined)
        continue;
      }
      if (why != kTaken) continue;
      ++census.taken;
      const uint16_t idx = uint16_t(plan.groups.size());
      plan.groups.push_back(g);
      plan.role[g.set_op] = 1, plan.role[g.draw_op] = 2;
      plan.group_of[g.set_op] = plan.group_of[g.draw_op] = idx;
      for (const Face& fc : g.face) plan.role[fc.copy_op] = 3, plan.group_of[fc.copy_op] = idx;
      ++plan.count;
    }
    if (plan.count) ++census.frames_taken;
  }

  std::string Report() const {
    char b[900];
    std::string reasons;
    for (int w = 1; w < kWhyCount; ++w)
      if (census.declined[w]) {
        std::snprintf(b, sizeof b, "%s%s %llu", reasons.empty() ? "" : "; ", WhyName(w), static_cast<unsigned long long>(census.declined[w]));
        reasons += b;
      }
    std::snprintf(b, sizeof b,
                  "[rdr][prelight_layered] frames %llu (with strips %llu, taken %llu), groups %llu, taken %llu, declined %llu%s%s | layered passes %llu, strip passes not made %llu, copies dropped %llu, fell back after the fact (draw not ours) %llu | audit: taken with a reader %llu, taken overlapping %llu",
                  static_cast<unsigned long long>(census.frames), static_cast<unsigned long long>(census.frames_with_groups), static_cast<unsigned long long>(census.frames_taken),
                  static_cast<unsigned long long>(census.groups), static_cast<unsigned long long>(census.taken), static_cast<unsigned long long>(census.groups - census.taken), reasons.empty() ? "" : " (", reasons.c_str(),
                  static_cast<unsigned long long>(census.passes_layered), static_cast<unsigned long long>(census.strips_unmade), static_cast<unsigned long long>(census.copies_dropped), static_cast<unsigned long long>(census.fallback_not_ours),
                  static_cast<unsigned long long>(census.taken_with_reader), static_cast<unsigned long long>(census.taken_overlapping));
    std::string out = b;
    if (!reasons.empty()) out.insert(out.find(" | layered passes"), ")");
    return out;
  }

 private:
  static bool CoversTile(const rdr::Rect& r, uint32_t x, uint32_t w, uint32_t h) { return r.x == int32_t(x) && r.y == 0 && r.width == w && r.height == h; }

  // The ops from a SetTargets up to the next one: [Draw (pre-light call site), Copy x 6], nothing else. The images and rects are judged later (so a miss is counted by its reason).
  static void FindGroups(const LoadedFrame& f, std::vector<Group>& out) {
    const uint32_t n = f.ops.count;
    for (uint32_t p = 0; p < n; ++p) {
      if (f.ops[p].kind != OpKind::SetTargets) continue;
      uint32_t q = p + 1;
      while (q < n && f.ops[q].kind != OpKind::SetTargets) ++q;
      // a span is a pre-light candidate when its first draw is the pre-light call site
      bool has_pre = false;
      for (uint32_t r = p + 1; r < q; ++r)
        if (f.ops[r].kind == OpKind::Draw) {
          const DrawOrigin* o = f.OriginOf(f.ops[r].index);
          has_pre = has_pre || (o && o->call_site == kPreLightCallSite);
        }
      if (!has_pre) { p = q - 1; continue; }
      Group g;
      g.set_op = p;
      const rdr::TargetsOp& t = f.targets[f.ops[p].index];
      g.strip = t.color_count ? t.color[0].image : 0;
      g.shape_ok = (q - p == 1 + 1 + kFaces) && t.color_count == 1;   // (the game's strip pass carries a depth target the draw does not use: the face passes have none)
      if (g.shape_ok) {
        g.shape_ok = f.ops[p + 1].kind == OpKind::Draw;
        g.draw_op = p + 1;
        for (uint32_t k = 0; k < kFaces && g.shape_ok; ++k) {
          g.shape_ok = f.ops[p + 2 + k].kind == OpKind::Copy;
          g.face[k].copy_op = p + 2 + k;
        }
        if (g.shape_ok) {
          const DrawOrigin* o = f.OriginOf(f.ops[g.draw_op].index);
          g.shape_ok = o && o->call_site == kPreLightCallSite;
        }
      }
      out.push_back(g);
      p = q - 1;
    }
  }

 public:
  // Judge one candidate group. `phantom_used` carries the planted phantom reader (the first group only).
  Why Judge(const LoadedFrame& f, const Ctx& ctx, Group& g, Plan& plan, bool& phantom_used) {
    (void)plan;
    if (!ctx.plan_ok) return kNotPlanned;
    if (!ctx.hook_on) return kHookOff;
    if (!ctx.multiview) return kNoMultiview;
    if (!g.shape_ok) return kShape;
    // the ops of the group against the other planners' drop sets
    const auto in_drop = [&](const uint8_t* set, uint32_t op) { return set && op < f.ops.count && set[op]; };
    uint32_t ops[2 + kFaces] = {g.set_op, g.draw_op};
    for (uint32_t k = 0; k < kFaces; ++k) ops[2 + k] = g.face[k].copy_op;
    bool dead = false, overlap = false;
    for (uint32_t op : ops) {
      dead = dead || in_drop(ctx.dead, op);
      for (const uint8_t* s : ctx.others) overlap = overlap || in_drop(s, op);
    }
    if (dead && !Planted(Fault::IgnoreOverlap)) return kDeadSkip;
    if (overlap && !Planted(Fault::IgnoreOverlap)) return kOverlap;
    const bool overlapping = dead || overlap;
    // the face passes have no depth target: the draw must not use one
    {
      const DrawFixed& fx = f.fixeds[f.draws[f.ops[g.draw_op].index].fixed];
      if (fx.state.depth_test || fx.state.depth_write || fx.state.stencil_test) return kDepthUsed;
    }
    // F5 (C1 review): the layered program and its face viewport replace the strip's clip transform: they are right only when that transform is the plain one
    {
      const DrawDynamic& dd = f.dynamics[f.draws[f.ops[g.draw_op].index].dynamic];
      float vw = dd.host.viewport[2];
      if (Planted(Fault::HalfViewport)) vw *= 0.5f;
      const rdr::Rect& sc = dd.host.scissor;
      const bool plain = dd.ndc_scale[0] == 1.0f && (dd.ndc_scale[1] == 1.0f || dd.ndc_scale[1] == -1.0f) && dd.host.viewport[0] == 0.0f && dd.host.viewport[1] == 0.0f && vw == float(kStripW) &&
                         dd.host.viewport[3] == float(kStripH) && sc.x <= 0 && sc.y <= 0 && sc.x + int64_t(sc.width) >= int64_t(kStripW) && sc.y + int64_t(sc.height) >= int64_t(kStripH);
      if (!plain) return kClipTransform;
    }
    // images
    const ImageRecord* strip = ctx.record ? ctx.record(g.strip) : nullptr;
    const rdr::CopyOp& c0 = f.copies[f.ops[g.face[0].copy_op].index];
    g.cube = c0.dest.image;
    const ImageRecord* cube = ctx.record ? ctx.record(g.cube) : nullptr;
    if (!strip || !cube || strip->width != kStripW || strip->height != kStripH || strip->layers != 1 || strip->mips != 1 || cube->width != kFace || cube->height != kFace || cube->layers != kFaces ||
        cube->mips != 1 || cube->format != strip->format)
      return kImages;
    // the face map, from the group's own copies
    uint32_t tile_seen = 0, layer_seen = 0;
    for (uint32_t k = 0; k < kFaces; ++k) {
      const rdr::CopyOp& c = f.copies[f.ops[g.face[k].copy_op].index];
      if (c.source.image != g.strip || c.source.mip != 0 || c.source.layer != 0 || c.dest.image != g.cube || c.dest.mip != 0 || c.dest_rect.x != 0 || c.dest_rect.y != 0 || c.dest_rect.width != kFace ||
          c.dest_rect.height != kFace || c.source_rect.y != 0 || c.source_rect.width != kFace || c.source_rect.height != kFace || c.source_rect.x < 0 || c.source_rect.x % int32_t(kFace) != 0)
        return kFaceMap;
      const uint32_t tile = uint32_t(c.source_rect.x) / kFace, layer = c.dest.layer;
      if (tile >= kFaces || layer >= kFaces) return kFaceMap;
      tile_seen |= 1u << tile, layer_seen |= 1u << layer;
      g.face[k].tile = tile, g.face[k].layer = layer;
    }
    if (tile_seen != 0x3F || layer_seen != 0x3F) return kFaceMap;
    for (uint32_t k = 0; k < kFaces; ++k)
      if (g.face[k].tile != g.face[k].layer) return kNotIdentity;   // the layered program draws tile v into layer v
    // the strip: read / written by nothing else; the cube: written by nothing else
    uint32_t strip_reads = 0;
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      const bool mine = (p >= g.set_op && p <= g.face[kFaces - 1].copy_op);
      if (op.kind == OpKind::Draw) {
        const Draw& d = f.draws[op.index];
        for (uint32_t s = 0; s < d.texture_count; ++s)
          if (f.texture_slots[d.texture_first + s].image == g.strip) ++strip_reads;
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = f.copies[op.index];
        if (c.source.image == g.strip && !mine) ++strip_reads;
        if (c.dest.image == g.strip) return kStripWritten;
        if (c.dest.image == g.cube && !mine) return kCubeWritten;
      } else if (op.kind == OpKind::Present) {
        if (f.presents[op.index].source.image == g.strip) ++strip_reads;
      } else if (op.kind == OpKind::SetTargets && p != g.set_op) {
        const rdr::TargetsOp& t = f.targets[op.index];
        for (uint32_t c = 0; c < t.color_count; ++c) {
          if (t.color[c].image == g.strip) return kStripWritten;
          if (t.color[c].image == g.cube) return kCubeWritten;
        }
        if (t.depth.image == g.strip || t.depth.image == g.cube) return kStripWritten;
      }
    }
    if (Planted(Fault::PhantomReader) && !phantom_used) phantom_used = true, ++strip_reads;
    const bool scan_off = Planted(Fault::NoStripCheck);
    if (strip_reads && !scan_off) return kStripRead;
    for (const ContentRef& c : f.contents)
      if (c.image == g.strip || c.image == g.cube) return kContent;
    if (copy_alias::WasReadBack(g.strip)) return kReadback;
    if (ctx.cube_has_target && !ctx.cube_has_target(g.cube)) return kNoTarget;
    if (strip_reads) ++census.taken_with_reader;
    if (overlapping) ++census.taken_overlapping;
    return kTaken;
  }
};

}  // namespace rdr_adapter::prelight_layered
