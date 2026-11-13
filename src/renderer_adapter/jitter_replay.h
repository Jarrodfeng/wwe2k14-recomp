// lane 185: the DLSS camera-jitter rule (jitter_rule.h) applied to a loaded capture frame, so rplay can count and show offline which draws the jitter moves,
// with no DLSS runtime and no game run (docs\renderer\WIDE_OURS.md section 7.5). The live reader moves the draws while it records (reader.h ApplyJitter, the same
// jitter::Moves); a capture holds the unjittered frame, so this walks the frame's ops in order and does what ApplyJitter + the present do. Offline only.
// What differs from the live reader, stated: the live reader tests the primitive and VTE bit 0 of the device; a capture has source_prim == 8 (Rect) and
// kDynClipOff (set with the viewport transform off), the same pair wide_replay.h uses. The depth copy: the copy's source image record (format Depth,
// >= 1000 x 500), as wide_replay.h. The phase is fixed (rplay --jitter PHASE), every frame, so a picture can be compared with the unjittered one.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../renderer/dlss_jitter.h"
#include "capture_reader.h"
#include "jitter_rule.h"
#include "wide_rule.h"

namespace rdr_adapter::jitter {

struct Replay {
  Window window;
  bool on = false;
  bool legacy = false;        // --jitter-legacy: the S10b window (the plant)
  bool marker_late = false;   // --jitter-marker-late: the marker is never noted, the window stays open to the end of the frame (the HUD plant)
  bool census = false;        // --jitter-census: one line a draw
  bool no_full_view = false;  // --jitter-no-fullview: the plant (a full-view composite quad is moved like any scene draw)
  bool chain = false;         // --jitter-chain: the window state carries across frames as live (armed only after a frame with its marker; the first frame is unarmed)
  bool zero = false;          // --jitter-zero: the whole path with a jitter of 0 (the identity check)
  uint32_t phase = 0, phases = 8;
  float scale = 1.0f;         // the render scale (the jitter is in render pixels)
  std::function<std::string(uint64_t)> namer;  // vertex shader hash -> program name (rplay: the binder's variant table)
  uint64_t marker_hash = kDefaultMarker;

  struct Count {
    uint32_t draws = 0, moved = 0, moved_after_copy = 0, moved_after_marker = 0, unmoved_after_copy = 0;
  };
  std::map<std::string, Count> classes;  // by program class
  Count total;
  uint32_t frames = 0;
  // storage the loaded frame's tables point into (valid until the next Apply)
  std::vector<DrawDynamic> dynamics_;
  std::vector<Draw> draws_;
  bool started = false;

  static std::string Class(const std::string& name) {
    const auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (has("yParticle")) return "particle";
    if (has("yProp")) return "prop";
    if (has("yCh_gear")) return "wrestler_gear";
    if (has("yCh_skin")) return "wrestler_skin";
    if (has("yCh_Reflect") || has("trans")) return "wrestler_reflect_trans";
    if (has("yCh")) return "wrestler_other";
    if (name.empty()) return "unknown";
    return "other";
  }

  // lane 185: the offline twin of reader.h LiveFullViewRect: 4 vertices of any stride (the position first), PosScale c0.xy and fUseProjMatrix c28.x of the draw's vertex bank.
  static bool FullViewRect(const LoadedFrame& f, const Draw& d, const CaptureReader& cap) {
    if (d.source_prim != 13 || d.vertex_count != 4 || d.vertex_bytes < 48 || d.vertex_bytes % 4 != 0 || d.vs_bank >= f.constant_banks.count) return false;
    const uint32_t stride = d.vertex_bytes / 4;
    if (stride < 12 || stride % 4) return false;
    std::vector<uint8_t> bytes;
    if (!cap.ReadGeometry(d.vertex_offset, d.vertex_bytes, bytes) || bytes.size() < 4 * size_t(stride)) return false;
    wide::FlatQuad q;
    for (int i = 0; i < 4; ++i) {
      float xy[2];
      std::memcpy(xy, bytes.data() + size_t(i) * stride, 8);
      q.x[i] = xy[0], q.y[i] = xy[1];
    }
    const uint32_t* c = f.constant_banks[d.vs_bank].dwords;
    std::memcpy(&q.pos_x, c + 0, 4), std::memcpy(&q.pos_y, c + 1, 4), std::memcpy(&q.use_proj, c + 28 * 4, 4);
    return wide::FullViewQuad(q);
  }

  void Apply(LoadedFrame& f, const CaptureReader& cap) {
    if (!on) return;
    dynamics_.assign(f.dynamics.data, f.dynamics.data + f.dynamics.count);
    draws_.assign(f.draws.data, f.draws.data + f.draws.count);
    std::map<uint32_t, uint32_t> moved_of;  // original dynamic index -> its jittered copy's index
    float j[2];
    rdr::DlssJitter(phase, phases, j);
    if (zero) j[0] = j[1] = 0;
    bool scene_done = false, marker_seen = false;  // (marker_seen: independent of the rule, so a plant that fails to close the window is counted)
    if (!started) started = true, window.armed = !chain;  // (the first frame of a chain follows no frame with a marker)
    window.hud = false, window.legacy = legacy;
    if (!chain) window.armed = true;
    for (uint32_t i = 0; i < f.ops.count; ++i) {
      const Op& op = f.ops[i];
      if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = f.copies[op.index];
        const ImageRecord* src = cap.FindImage(c.source.image);
        if (src && src->format == uint8_t(rdr::Format::Depth) && src->width >= 1000 && src->height >= 500) scene_done = true;
      } else if (op.kind == OpKind::Present) {
        ++frames;
        NextFrame(window);
        scene_done = marker_seen = false;
      } else if (op.kind == OpKind::Draw) {
        const Draw& d = f.draws[op.index];
        const DrawDynamic& from = f.dynamics[d.dynamic];
        DrawFacts facts;
        facts.rect = d.source_prim == 8;
        facts.clip_off = from.flags & kDynClipOff;
        facts.empty_viewport = from.flags & kDynEmptyViewport;
        facts.full_view = !no_full_view && FullViewRect(f, d, cap);
        facts.vw = from.host.viewport[2], facts.vh = from.host.viewport[3], facts.vs_hash = d.vs_patched_hash;
        const bool was_marker_seen = marker_seen;
        const bool moves = Moves(window, scene_done, facts, marker_late ? 0 : marker_hash);
        if (d.vs_patched_hash == marker_hash) marker_seen = true;
        const std::string name = namer ? namer(d.vs_hash) : std::string();
        // (only the draws the rule could ever move are classed; a screen-space pass is not a candidate)
        const bool candidate = !(facts.vw < 1200.0f || facts.vh <= 0 || facts.rect || facts.clip_off || facts.empty_viewport);
        for (Count* c : {&total, &classes[Class(name)]}) {
          if (!candidate) continue;
          ++c->draws;
          if (moves) {
            ++c->moved;
            if (scene_done) ++c->moved_after_copy;
            if (was_marker_seen) ++c->moved_after_marker;
          } else if (scene_done) {
            ++c->unmoved_after_copy;
          }
        }
        if (moves) {
          auto it = moved_of.find(d.dynamic);
          if (it == moved_of.end()) {
            DrawDynamic copy = from;
            copy.ndc_offset[0] += 2.0f * j[0] / (facts.vw * scale);
            copy.ndc_offset[1] += 2.0f * j[1] / (facts.vh * scale);
            dynamics_.push_back(copy);
            it = moved_of.emplace(d.dynamic, uint32_t(dynamics_.size() - 1)).first;
          }
          draws_[op.index].dynamic = it->second;
        }
        if (census && candidate) {
          float use_proj = -1, pos_x = 0, pos_y = 0;  // the vertex bank's fUseProjMatrix (c28.x) and PosScale (c0.xy): 0 = pixel-placed, 1 = projected
          if (d.vs_bank < f.constant_banks.count) {
            const uint32_t* c = f.constant_banks[d.vs_bank].dwords;
            std::memcpy(&pos_x, c + 0, 4), std::memcpy(&pos_y, c + 1, 4), std::memcpy(&use_proj, c + 28 * 4, 4);
          }
          char quad[160] = "";  // a 4-vertex rectangle: its corners (x, y of the first three vertices, guest pixels), to tell a full-view or a placed quad
          if (d.vertex_count == 4 && d.vertex_bytes >= 48) {
            std::vector<uint8_t> bytes;
            const uint32_t stride = d.vertex_bytes / 4;
            if (cap.ReadGeometry(d.vertex_offset, d.vertex_bytes, bytes) && bytes.size() >= 4 * size_t(stride) && stride >= 8) {
              float xy[8];
              for (int v = 0; v < 4; ++v) std::memcpy(xy + 2 * v, bytes.data() + size_t(v) * stride, 8);
              std::snprintf(quad, sizeof(quad), "quad (%g,%g) (%g,%g) (%g,%g) (%g,%g) ", xy[0], xy[1], xy[2], xy[3], xy[4], xy[5], xy[6], xy[7]);
            }
          }
          std::printf("[jitter] draw %u vs %016llX %s ps %016llX %s prim %u verts %u c28.x %g posscale %g %g vp_w %.0f %s%s%s%s\n", op.index, (unsigned long long)d.vs_hash, name.c_str(),
                      (unsigned long long)d.ps_hash, namer ? namer(d.ps_hash).c_str() : "", unsigned(d.source_prim), unsigned(d.vertex_count), use_proj, pos_x, pos_y, facts.vw, quad,
                      scene_done ? "after-depth-copy " : "", was_marker_seen ? "after-marker " : "", moves ? "-> MOVED" : "-> left");
        }
      }
    }
    f.dynamics.data = dynamics_.data();
    f.dynamics.count = uint32_t(dynamics_.size());
    f.draws.data = draws_.data();
  }

  void Report() const {
    std::printf("[jitter] mode %s%s phase %u/%u scale %g: %u frames, candidate draws %u, moved %u (after the first depth copy %u, after the marker %u), left after the copy %u\n",
                legacy ? "legacy" : "marker", marker_late ? " marker-late" : "", phase, phases, scale, frames, total.draws, total.moved, total.moved_after_copy, total.moved_after_marker,
                total.unmoved_after_copy);
    for (const auto& item : classes)
      std::printf("[jitter] class %s draws %u moved %u after_copy %u after_marker %u left_after_copy %u\n", item.first.c_str(), item.second.draws, item.second.moved, item.second.moved_after_copy,
                  item.second.moved_after_marker, item.second.unmoved_after_copy);
  }
};

}  // namespace rdr_adapter::jitter
