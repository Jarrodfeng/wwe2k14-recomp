// S101 lane 123 (SH2, docs\renderer\SHADOWS_OURS.md; design = SHADOW_PROMOTION_MAP.md section 6): OUR shadow passes. The game's shadow maps are re-made as
// images of our own (a size setting: 512 = the 360 look, 256 low-spec, 1024 / 2048 PC presets) and the stream's caster draws and chain draws are re-targeted
// into them with the viewport, scissor and clear rectangles scaled; the copies out of them are whole (no partial rectangles); every reader's slot is bound to
// our final image because the image id it binds (a copy of a shadow target) is remapped to ours. This header is the GPU-free core the binder (binder.h)
// and the offline checker (tools\shadow\shadows_ours_check.cpp) both build: the per-frame classification of which game images belong to which chain
// (decided by draw ORIGIN and the copy graph, never by shader hash or image size), the scale rules and the planted faults. No game or SDK includes.
//
// The chains (all [C], SHADOW_PROMOTION_MAP.md 1.1 and 9):
//   Map   the props map + circle blobs, wrestler map A, map B, the min blurs (draws of pass site 0x82C04634 / call site 0x828131E4, and the chain draws that read
//         a copy of one of those targets into a shadow-role target)
//   Plane the ring mat's plane mask: the 512 plane pass (site 0x82BF4D38), its 256 downscale and the two shadow blurs
// The rim light map is NOT a shadow (shadow_roles.h): its pass is not a caster, so its targets are never classified.
// Not remapped on purpose: the coverage reductions (128 / 32, unnamed role): they are masks the min blurs read per vertex, they keep the stream's size.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "adapter_frame.h"
#include "names.h"
#include "shadow_roles.h"
#include "d3_arena.h"   // lane 228 (N2): IsCasterMarkerCall / CasterPassOfMarker

namespace rdr_adapter {

// ---- switches (live: --wwe2k14_rdr_shadows_ours / --wwe2k14_rdr_shadow_map_size / --wwe2k14_rdr_shadow_plane_scale; rplay / fview: --shadows-ours [--shadow-map-size N]) ----
inline bool g_shadows_ours = false;           // default off on PC and phone
inline uint32_t g_shadow_map_size = 512;      // the maps' edge in texels (64 .. 4096); 512 = the stream's
inline uint32_t g_shadow_plane_percent = 100; // the plane mask's size as a percent of the stream's (512 / 256); 0 = follow the render scale (rounded to a whole number)
inline float g_shadow_render_scale = 1.0f;    // set by the live path / the tools from the renderer's scale: only used when g_shadow_plane_percent == 0

namespace shadows_ours {

enum Chain : uint8_t { kNone = 0, kMap = 1, kPlane = 2 };

// Planted faults (offline tools only; a player build never sets them). R = re-finds a real past bug.
enum class Fault : uint8_t {
  None = 0,
  ScaledMap,             // R: the square maps scaled by the 16:9 rule (S100 13:37: lines at 1.5x / 2x; S80: 4.2% at 1440p)
  CopySwap,              // R: the props map's red / blue copy swap lost: readers bound to our copy read the slot swizzle as identity (S80)
  BlurInputNull,         // R: the chain draws' first texture slot nulled (S100 11:39: the shadow blur's input lost to a nulled fetch slot)
  FloorReadsWrestlerMap, // R: the floor shaders' slot 12 bound to the wrestler map instead of the props map (S98: the viewer's long stretched shadows)
  HalfTexelRescaled,     // the half texel patched to 0.5/N in the readers (review A5): modelled as the map content sliding by (0.5/512 - 0.5/N) of the map
  DupNoInvalidate,       // a whole copy is not made stale when its source is drawn again (review A9)
  NoViewportScale,       // the images are remapped but the draws keep the stream's viewport (review: the footprint must fail)
  PlaneUnscaledChain,    // the plane chain's images are scaled but its draws are not (needs --shadow-plane-scale != 100)
  PSlotsCast,            // the arena's colour-pass draws counted as casters (SHADOW_PROMOTION_MAP.md 1.2: the `p` slots never cast); the size guard is off with it
  Count
};
inline Fault g_fault = Fault::None;
inline const char* FaultName(Fault f) {
  static const char* const k[] = {"none", "scaled_map", "copy_swap", "blur_input_null", "floor_reads_wrestler_map", "half_texel_rescaled", "dup_no_invalidate", "no_viewport_scale", "plane_unscaled_chain", "p_slots_cast"};
  return uint8_t(f) < uint8_t(Fault::Count) ? k[uint8_t(f)] : "?";
}
inline bool ParseFault(const char* text, Fault& out) {
  for (uint8_t i = 0; i < uint8_t(Fault::Count); ++i)
    if (std::strcmp(text, FaultName(Fault(i))) == 0) {
      out = Fault(i);
      return true;
    }
  return false;
}

inline constexpr uint32_t kStreamMapSize = 512;  // the 360's map edge: our factor is g_shadow_map_size / this
inline constexpr uint32_t kColourPassSites[2] = {0x82C0A950u, 0x82C0B94Cu};  // the arena's / props' colour pass (the `p` plant)

// The scale of a chain's images against the stream's, per axis.
struct Factor {
  float x = 1, y = 1;
  bool unit() const { return x == 1.0f && y == 1.0f; }
};
inline uint32_t MapSize() { return g_shadow_map_size < 64 || g_shadow_map_size > 4096 ? kStreamMapSize : g_shadow_map_size; }
inline Factor FactorOf(Chain chain) {
  Factor f;
  if (chain == kMap) {
    f.x = f.y = float(MapSize()) / float(kStreamMapSize);
    if (g_fault == Fault::ScaledMap) f.y = f.x * 9.0f / 16.0f;  // the plant: the 16:9 rule on a square map
  } else if (chain == kPlane) {
    // Design decision 2 (SHADOWS_OURS.md): the plane mask is a SCREEN-space picture of the canvas, so it may follow the render scale; the default is the 360's size.
    float p = g_shadow_plane_percent ? float(g_shadow_plane_percent) / 100.0f : std::round(g_shadow_render_scale < 1.0f ? 1.0f : g_shadow_render_scale);
    if (p < 0.25f) p = 0.25f;
    if (p > 4.0f) p = 4.0f;
    f.x = f.y = p;
  }
  return f;
}
inline uint32_t Scaled(uint32_t value, float factor) {
  const uint32_t v = uint32_t(std::ceil(double(value) * double(factor) - 1e-6));
  return v < 1 ? 1 : v;
}

// The draw's dynamic state for a target scaled by (fx, fy): the viewport and scissor are in the stream's guest pixels and grow with the image. Clip-on draws keep their
// ndc fields (clip space does not depend on the target size). Clip-off draws (pixel space: the chain's quads and grids) have a viewport of E guest pixels: it is rebuilt
// with E' = 8192 / max(1, f / 2) so E' * f never goes past 16384 (the viewport limit every device has); at f <= 2 this is the stream's own value.
inline void ScaleDynamic(DrawDynamic& d, Factor f) {
  if (f.unit()) return;
  if (d.flags & kDynClipOff) {
    const float fs[2] = {f.x, f.y};
    for (int i = 0; i < 2; ++i) {
      const float e = 8192.0f / (fs[i] > 2.0f ? fs[i] / 2.0f : 1.0f);
      d.host.viewport[i] = 0;
      d.host.viewport[2 + i] = e * fs[i];
      d.ndc_scale[i] = d.guest_scale[i] * 2.0f / e;
      d.ndc_offset[i] = (d.guest_offset[i] - e * 0.5f) * 2.0f / e;
    }
  } else {
    d.host.viewport[0] *= f.x, d.host.viewport[1] *= f.y, d.host.viewport[2] *= f.x, d.host.viewport[3] *= f.y;
  }
  rdr::Rect& s = d.host.scissor;
  const int32_t x = int32_t(std::floor(double(s.x) * f.x)), y = int32_t(std::floor(double(s.y) * f.y));
  s.width = Scaled(s.width, f.x), s.height = Scaled(s.height, f.y);
  s.x = x, s.y = y;
}
inline rdr::Rect ScaleRect(rdr::Rect r, Factor f) {
  const int32_t x = int32_t(std::floor(double(r.x) * f.x)), y = int32_t(std::floor(double(r.y) * f.y));
  return {x, y, Scaled(r.width, f.x), Scaled(r.height, f.y)};
}

struct ImageInfo {
  uint16_t width = 0, height = 0;
  uint8_t format = 0, role = 0;
};

// The census (binder.h prints it every 600 frames and fview at the end).
struct Census {
  uint64_t frames = 0;             // frames with at least one classified image
  uint64_t frames_unnamed = 0;     // frames without draw origins (a version-1 capture): nothing classified
  uint64_t images_made = 0;        // our images created
  uint64_t targets_map = 0, targets_plane = 0;  // distinct game images classified so far, by chain
  uint64_t draws_retargeted = 0;   // draws drawn into one of our images (caster draws + chain draws)
  uint64_t clears = 0;
  uint64_t copies_whole = 0, copies_partial_made_whole = 0, copies_dropped = 0;
  uint64_t readers = 0;            // texture slots bound to one of our images
  uint64_t refused = 0;            // classified but not remapped (an image bigger than a shadow map, the record missing): the stream's image stays
  double ms = 0;
};

// What one frame's scan found.
class Plan {
 public:
  std::unordered_map<uint32_t, uint8_t> chain;  // game image id -> Chain (targets, their depth buffers, the copy destinations)
  std::vector<uint32_t> reduce_dests;            // copy destinations of the coverage reductions (128 / 32: unnamed role, kept at the stream's size; review A6: the check compares them)
  std::unordered_map<uint32_t, uint8_t> reduce_targets, reduce_dest_set;
  std::vector<uint32_t> dests_rgba8, dests_16f;  // Map-chain copy destinations by storage (the props map is RGBA8, the wrestler maps RGBA16F): the plant floor_reads_wrestler_map
  uint32_t casters = 0;       // caster draws seen (a check: Check B frame 200 = 15 map + 15 map A/B...; see expect in the checker)
  uint32_t guard_refused = 0;
  bool named = false;         // the frame carried draw origins

  Chain Of(uint32_t id) const {
    const auto it = chain.find(id);
    return it == chain.end() ? kNone : Chain(it->second);
  }

  // `info(id)` -> ImageInfo of a game image id (width 0 = unknown). Ops are walked in order: copies and chain draws classify what they feed, so every class is
  // known before the real bind starts (the bind needs a target's class at its SetTargets, which comes before its draws).
  template <class LoadedFrameT, class InfoFn>
  void Scan(const LoadedFrameT& frame, InfoFn info) {
    chain.clear();
    dests_rgba8.clear(), dests_16f.clear(), reduce_dests.clear(), reduce_targets.clear(), reduce_dest_set.clear();
    casters = guard_refused = 0;
    named = frame.origins.count != 0;
    if (!named) return;
    uint32_t color = 0, depth = 0;
    const auto guard = [&](uint32_t id) {  // a shadow map is never as big as the screen: refuse anything over 512 on a side (the plant p_slots_cast has no guard)
      if (g_fault == Fault::PSlotsCast) return true;
      const ImageInfo i = info(id);
      if (!i.width || i.width > kStreamMapSize || i.height > kStreamMapSize) {
        ++guard_refused;
        return false;
      }
      return true;
    };
    const auto classify = [&](uint32_t id, Chain c) {
      if (!id || c == kNone) return;
      auto found = chain.find(id);
      if (found == chain.end() && guard(id)) chain.emplace(id, uint8_t(c));
    };
    for (const Op& op : frame.ops) {
      if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = frame.targets[op.index];
        color = t.color_count ? t.color[0].image : 0;
        depth = t.depth.image;
      } else if (op.kind == OpKind::Draw) {
        const DrawOrigin* o = frame.OriginOf(op.index);
        const Draw& d = frame.draws[op.index];
        const uint32_t target = color ? color : depth;
        if (!target) continue;
        const uint8_t role = info(target).role;
        if (o && d3::IsCasterMarkerCall(o->call_site)) {   // lane 228 (review N2): a D3 caster marker stands for its pass's caster draws (the skip arm leaves no other draw in a pass whose objects are all marked): it classifies its target as they would
          const uint32_t mp = d3::CasterPassOfMarker(o->call_site);
          if (mp == 1u || mp == 2u) {   // 1 map, 2 plane; the rim map has its own role, not a chain here
            ++casters;
            const Chain c = mp == 2u ? kPlane : kMap;
            classify(color, c), classify(depth, c);
          }
        } else if (shadow_roles::IsCaster(o, d.ps_hash, d.vs_patched_hash, role) && shadow_roles::Named(o)) {
          ++casters;
          const Chain c = o->pass_site == shadow_roles::kPlanePassSite ? kPlane : kMap;
          classify(color, c), classify(depth, c);
        } else if (g_fault == Fault::PSlotsCast && o && o->klass == DrawClass::ModelPart &&
                   (o->pass_site == kColourPassSites[0] || o->pass_site == kColourPassSites[1])) {
          classify(color, kMap), classify(depth, kMap);  // the plant: the arena's colour-pass draws counted as casters
        } else if (role != uint8_t(SurfaceRole::ShadowMap) && role != uint8_t(SurfaceRole::RimMap) && !chain.count(target) && d.texture_count) {
          for (uint32_t s = 0; s < d.texture_count; ++s)
            if ((Of(frame.texture_slots[d.texture_first + s].image) != kNone || reduce_dest_set.count(frame.texture_slots[d.texture_first + s].image)) && o &&
                o->call_site == 0x82C1F514u) {  // the coverage reduction (site from SHADOW_PROMOTION_MAP.md 1.1)
              reduce_targets[target] = 1;
              break;
            }
        } else if (role == uint8_t(SurfaceRole::ShadowMap) && !chain.count(target)) {
          // A chain draw: it draws a shadow-role target from a copy of a classified target.
          for (uint32_t s = 0; s < d.texture_count; ++s) {
            const Chain c = Of(frame.texture_slots[d.texture_first + s].image);
            if (c != kNone) {
              classify(color, c), classify(depth, c);
              break;
            }
          }
        }
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = frame.copies[op.index];
        if (reduce_targets.count(c.source.image)) reduce_dests.push_back(c.dest.image), reduce_dest_set[c.dest.image] = 1;
        const Chain src = Of(c.source.image);
        if (src != kNone && !chain.count(c.dest.image)) {
          classify(c.dest.image, src);
          if (src == kMap && chain.count(c.dest.image)) (info(c.dest.image).format == uint8_t(rdr::Format::RGBA16F) ? dests_16f : dests_rgba8).push_back(c.dest.image);
        }
      }
    }
  }
};

}  // namespace shadows_ours
}  // namespace rdr_adapter
