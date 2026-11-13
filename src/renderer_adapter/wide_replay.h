// lane 171: the screen-shape rule (wide_rule.h) applied to a loaded capture frame, so rplay / fview can show a 21:9 (or taller) frame offline.
// The live reader squeezes while it records (ApplyWide); a capture holds the 16:9 frame, so this walks the frame's ops in order and does what
// ApplyWide + the present marker do, with the same Decide(). Offline only: 0 game runs. What differs from the live reader, stated:
//   - screen_space: the live reader tests the primitive and VTE bit 0 of the device; a capture has the primitive and kDynClipOff (clip off, set
//     with the viewport transform off for every pixel-placed draw seen). The census (--wide-census) prints the draws where the two could differ.
//   - the depth copy: the live reader tests the copy's source surface (depth, >= 1000 x 500); here the copy's source image record (format Depth).
//   - the first frame's "menu" flag (the frame before) is unknown: a replay starts with menu = false unless asked.
#pragma once
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "capture_reader.h"
#include "wide_rule.h"

namespace rdr_adapter::wide {

struct Replay {
  State state;
  bool drop_full_view = false;          // lane 177 measurement only: a full-view quad writes no colour (an unflashed baseline for the coverage numbers)
  int64_t mute_draw = -1;               // lane 186 measurement only (rplay --wide-mute-draw N): draw N of every frame writes no colour, so a render diff reads its footprint back
  bool clip_off_squeezed_plant = false;  // lane 244 plant (rplay --wide-clipoff-squeezed): clip-off draws squeezed like any other
  bool offscreen_squeezed_plant = false;  // lane 244b plant (rplay --wide-offscreen-squeezed)
  bool all_plant = false;               // lane 244 (rplay --wide-all): gpu_wide_all, which the app sets outside matches (src/ultrawide.h): every draw is squeezed on every frame
  bool from_squeezed = false;           // lane 244 (rplay --wide-from-squeezed): the capture was RECORDED with the wide rule on (its ndc x / y are squeezed): rebuild every draw's ndc x / y from its guest_scale /
                                        // guest_offset and host viewport first (C8 / C14: ndc = guest * 2 / E, (offset - (vx + E / 2)) * 2 / E), then apply the rule as for a 16:9 capture. Offline proof of a rule change on wide captures.
  bool census = false;                  // print every draw the rule squeezes or leaves alone, with the reason (one line a draw)
  struct Totals {
    uint32_t draws = 0, squeezed = 0, scene_after_copy = 0, menu = 0, hud = 0, full_view = 0, post_after_full = 0, post_after_partial = 0;  // lane 177: full_view = full-view quads left unsqueezed
  } totals;
  // storage the loaded frame's tables point into (valid until the next Apply)
  std::vector<DrawDynamic> dynamics_;
  std::vector<Draw> draws_;
  std::vector<DrawFixed> fixeds_;

  void Configure(float wide_factor, float tall_factor, bool legacy, bool no_full_view = false, bool post_rect = false) {
    state = State{};
    state.wide = wide_factor > 1.001f ? wide_factor : 1.0f;
    state.tall = tall_factor > 1.001f ? tall_factor : 1.0f;
    state.hud_after_vs = 0xD6B8DEB0775CC47FULL;                             // the SDK's gpu_dlss_hud_after_vs default (patched hash)
    state.post_vs = {0x1456901233B6564CULL};  // the filter's copy pass only (lane 186: the T_Rect VS 032E8FAC979E01DA left the list: partial rects squeeze, full-view ones are kept by FullViewQuad)
    if (post_rect) state.post_vs.push_back(0x032E8FAC979E01DAULL);  // lane 186 plant (rplay --wide-post-rect): the old list, every T_Rect after the marker unsqueezed
    state.legacy = legacy;
    state.no_full_view = no_full_view;  // lane 177
    state.clip_off_squeezed = clip_off_squeezed_plant;  // lane 244
    state.all = all_plant;
    state.offscreen_squeezed = offscreen_squeezed_plant;  // lane 244b
  }

  // lane 177: the live reader's shape test (a T_Rect: prim 13, four vertices of one RGB32F element, stride 12), applied to a capture draw: the corners come
  // from geometry.bin (host-converted, little-endian), PosScale and fUseProjMatrix from the draw's vertex bank (c0.xy, c28.x).
  static bool ReadQuad(const LoadedFrame& f, const Draw& d, const CaptureReader& cap, FlatQuad& q) {  // lane 186: split out so the census can print the box
    if (!(d.flags & kDrawInline) || d.source_prim != 13 || d.vertex_count != 4 || d.vertex_bytes < 48 || d.layout == 0xFFFFFFFFu || d.layout >= f.layouts.count) return false;
    const VertexLayout& lay = f.layouts[d.layout];
    if (lay.element_count != 1 || lay.host_stride != 12 || lay.element_first >= f.elements.count || f.elements[lay.element_first].vk_format != 106) return false;
    if (d.vs_bank >= f.constant_banks.count) return false;
    std::vector<uint8_t> bytes;
    if (!cap.ReadGeometry(d.vertex_offset, 48, bytes) || bytes.size() < 48) return false;
    for (int i = 0; i < 4; ++i) {
      float xyz[3];
      std::memcpy(xyz, bytes.data() + 12 * i, 12);
      q.x[i] = xyz[0], q.y[i] = xyz[1];
    }
    const uint32_t* c = f.constant_banks[d.vs_bank].dwords;
    std::memcpy(&q.pos_x, c + 0, 4), std::memcpy(&q.pos_y, c + 1, 4), std::memcpy(&q.use_proj, c + 28 * 4, 4);
    return true;
  }
  static bool FullViewDraw(const LoadedFrame& f, const Draw& d, const CaptureReader& cap) {
    FlatQuad q;
    return ReadQuad(f, d, cap, q) && FullViewQuad(q);
  }

  // One frame, in capture order. A frame ends with its present (the markers for the next one).
  void Apply(LoadedFrame& f, const CaptureReader& cap) {
    if (!state.on()) return;
    dynamics_.assign(f.dynamics.data, f.dynamics.data + f.dynamics.count);
    draws_.assign(f.draws.data, f.draws.data + f.draws.count);
    fixeds_.assign(f.fixeds.data, f.fixeds.data + f.fixeds.count);
    if (from_squeezed) {  // lane 244
      dynamics_.reserve(size_t(f.dynamics.count) * 2 + 2);  // squeezed copies are appended below; `from` points into this table, so it must not move
      for (DrawDynamic& dy : dynamics_) {
        if (dy.flags & kDynEmptyViewport) continue;
        for (int ax = 0; ax < 2; ++ax) {
          const float e = dy.host.viewport[2 + ax];
          if (!(e > 0)) continue;
          dy.ndc_scale[ax] = dy.guest_scale[ax] * 2.0f / e;
          dy.ndc_offset[ax] = (dy.guest_offset[ax] - (dy.host.viewport[ax] + e * 0.5f)) * 2.0f / e;
        }
      }
      f.dynamics.data = dynamics_.data();
    }
    std::map<uint32_t, uint32_t> mute_of;  // lane 177: original fixed index -> its colour-mute copy
    std::map<uint32_t, uint32_t> squeezed_of;  // original dynamic index -> its squeezed copy's index
    for (uint32_t i = 0; i < f.ops.count; ++i) {
      const Op& op = f.ops[i];
      if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = f.copies[op.index];
        const ImageRecord* src = cap.FindImage(c.source.image);
        if (src && src->format == uint8_t(rdr::Format::Depth) && src->width >= 1000 && src->height >= 500) state.scene_done = true;
      } else if (op.kind == OpKind::Present) {
        NextFrame(state, all_plant);
      } else if (op.kind == OpKind::Draw) {
        Draw& d = draws_[op.index];
        const DrawDynamic& from = f.dynamics[d.dynamic];
        const bool screen_space = d.source_prim == 8 || (from.flags & kDynClipOff);
        const bool was_scene_done = state.scene_done, was_hud = state.hud, was_menu = state.menu;
        const bool full_view_quad = state.no_full_view ? false : FullViewDraw(f, d, cap);  // lane 177: found by shape
        const Verdict v = Decide(state, screen_space, from.host.viewport[2], d.vs_patched_hash, full_view_quad, from.flags & kDynClipOff);  // lane 244
        ++totals.draws;
        totals.full_view += v.full_view;
        if (v.post_after) ++(FullViewDraw(f, d, cap) ? totals.post_after_full : totals.post_after_partial);  // lane 177 audit: what the hash rule (gpu_wide_post_vs) leaves alone, by shape
        if ((v.full_view && drop_full_view) || int64_t(op.index) == mute_draw) {  // measurement only (rplay --wide-drop-fullview)
          auto mt = mute_of.find(d.fixed);
          if (mt == mute_of.end()) {
            DrawFixed mute = f.fixeds[d.fixed];
            for (auto& b : mute.state.blend) b.write_mask = 0;
            fixeds_.push_back(mute);
            mt = mute_of.emplace(d.fixed, uint32_t(fixeds_.size() - 1)).first;
          }
          d.fixed = mt->second;
        }
        if (v.squeeze && !(from.flags & kDynEmptyViewport)) {
          ++totals.squeezed;
          if (was_menu) ++totals.menu;
          else if (was_hud) ++totals.hud;
          else if (was_scene_done) ++totals.scene_after_copy;  // the draws the S5 rule left unsqueezed (legacy: they are not squeezed, so not counted)
          auto it = squeezed_of.find(d.dynamic);
          if (it == squeezed_of.end()) {
            DrawDynamic copy = from;
            if (state.wide > 1.001f) copy.ndc_scale[0] /= state.wide, copy.ndc_offset[0] /= state.wide;
            if (state.tall > 1.001f) copy.ndc_scale[1] /= state.tall, copy.ndc_offset[1] /= state.tall;
            dynamics_.push_back(copy);
            it = squeezed_of.emplace(d.dynamic, uint32_t(dynamics_.size() - 1)).first;
          }
          d.dynamic = it->second;
        }
        if (census) {
          FlatQuad bq;
          if (ReadQuad(f, d, cap, bq) && bq.pos_x > 0 && bq.pos_y > 0) {  // lane 186 audit: the quad's box in view pixels, for the near-full-rect sweep
            float x0 = bq.x[0], x1 = bq.x[0], y0 = bq.y[0], y1 = bq.y[0];
            for (int i = 1; i < 4; ++i) x0 = std::min(x0, bq.x[i]), x1 = std::max(x1, bq.x[i]), y0 = std::min(y0, bq.y[i]), y1 = std::max(y1, bq.y[i]);
            std::printf("[wide-box] draw %u box %.1f %.1f %.1f %.1f view %.0f %.0f\n", op.index, x0, y0, x1, y1, 2.0f / bq.pos_x, 2.0f / bq.pos_y);
          }
        }
        if (census)
          std::printf("[wide] draw %u vs %016llX ps %016llX prim %u vp_w %.0f %s%s%s%s -> %s [inline %d index_bytes %u guest_stride %u elements %u]\n", op.index, (unsigned long long)d.vs_hash, (unsigned long long)d.ps_hash,
                      unsigned(d.source_prim), from.host.viewport[2], screen_space ? "screen-space " : "", was_scene_done ? "after-depth-copy " : "",
                      was_hud ? "hud " : "", v.post_after ? "post-after " : "", v.squeeze ? "SQUEEZE" : (v.full_view ? "FULL-VIEW (unsqueezed)" : (v.pixel_placed ? "PIXEL-PLACED (clip off, unsqueezed)" : (v.offscreen ? "OFFSCREEN (unsqueezed)" : "left"))),
                      (d.flags & kDrawInline) ? 1 : 0, d.index_bytes, d.layout < f.layouts.count ? f.layouts[d.layout].guest_stride : 0u, d.layout < f.layouts.count ? f.layouts[d.layout].element_count : 0u);
      }
    }
    f.dynamics.data = dynamics_.data();
    f.dynamics.count = uint32_t(dynamics_.size());
    f.draws.data = draws_.data();
    f.fixeds.data = fixeds_.data();
    f.fixeds.count = uint32_t(fixeds_.size());
  }
};

}  // namespace rdr_adapter::wide
