// S102 R21-A step 5 (docs\renderer\FRAME_FACTS.md section 4): the census that prices "plan once per shape". TOOL ONLY (rplay --frame-shape FILE): never linked into the game's hot path.
// For each drawn frame it hashes what each planner reads as input (the op kind + target + pass-site sequence, the copy list, the draw count per pass, the image ids of the copies)
// and reports the share of frames whose hash equals the previous drawn frame's and the share equal to one of the last 4. The mapping from a planner to its inputs is the brief's:
//   post (PlanPost): the op sequence with pass sites + the draw count per pass;     alias (Planner::Build): the copy list + the render target sequence;
//   dead (DeadWork): the copy list + the draw count per pass;                         shadows (ShadowsOursBegin): the op kinds + pass sites + the draw count per pass;
//   streaks (CopyStreaks::Note): the image ids of the copies.
// The inputs are read from the frame's tables as the planners read them (the real planners also read constants and image roles; a shape equal here may still differ in a value):
// the number is an UPPER bound on "plan once per shape" (unconfirmed until a planner is run on the shape key).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "binder.h"

namespace rdr_adapter {

struct FrameShape {
  static constexpr int kPlanners = 5;
  static constexpr const char* kNames[kPlanners] = {"post", "alias", "dead_work", "shadows", "streaks"};
  uint64_t frames = 0, same_prev[kPlanners] = {}, same_last4[kPlanners] = {};
  uint64_t last[kPlanners][4] = {};
  uint32_t have[kPlanners] = {};

  static void Mix(uint64_t& h, uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; }

  void Note(const LoadedFrame& f) {
    uint64_t post = 1469598103934665603ull, alias = post, dead = post, shadows = post, streaks = post;
    uint64_t pass_draws = 0, pass_count = 0;
    const auto close_pass = [&](bool open) {
      if (!open) return;
      Mix(post, 0x1000 + pass_draws), Mix(dead, 0x1000 + pass_draws), Mix(shadows, 0x1000 + pass_draws);
      pass_draws = 0, ++pass_count;
    };
    bool open = false;
    for (uint32_t k = 0; k < f.ops.count; ++k) {
      const Op& op = f.ops[k];
      switch (op.kind) {
        case OpKind::SetTargets: {
          close_pass(open), open = true;
          uint64_t t = 7;
          if (op.index < f.targets.count) {
            const rdr::TargetsOp& to = f.targets[op.index];
            for (uint32_t c = 0; c < to.color_count; ++c) Mix(t, uint32_t(to.color[c].image));
            Mix(t, uint32_t(to.depth.image));
          }
          Mix(post, t), Mix(alias, t), Mix(shadows, t);
          break;
        }
        case OpKind::Draw: {
          ++pass_draws;
          const DrawOrigin* o = f.OriginOf(op.index);
          const uint64_t site = o ? uint64_t(o->pass_site) + 1 : 0;
          Mix(post, site), Mix(shadows, site);
          break;
        }
        case OpKind::Copy: {
          if (op.index >= f.copies.count) break;
          const rdr::CopyOp& c = f.copies[op.index];
          Mix(alias, uint64_t(uint32_t(c.source.image)) << 32 | uint32_t(c.dest.image));
          Mix(alias, uint64_t(uint32_t(c.dest_rect.width)) << 32 | uint32_t(c.dest_rect.height));
          Mix(dead, uint64_t(uint32_t(c.source.image)) << 32 | uint32_t(c.dest.image));
          Mix(streaks, uint64_t(uint32_t(c.source.image)) << 32 | uint32_t(c.dest.image));
          Mix(post, 0x2000), Mix(shadows, 0x2000);
          break;
        }
        case OpKind::Clear: Mix(post, 0x3000), Mix(shadows, 0x3000); break;
        case OpKind::Present: Mix(post, 0x4000), Mix(shadows, 0x4000); break;
      }
    }
    close_pass(open);
    const uint64_t h[kPlanners] = {post, alias, dead, shadows, streaks};
    ++frames;
    for (int p = 0; p < kPlanners; ++p) {
      const uint32_t n = have[p] < 4 ? have[p] : 4;
      if (n && h[p] == last[p][0]) ++same_prev[p];
      bool in4 = false;
      for (uint32_t i = 0; i < n; ++i) in4 = in4 || h[p] == last[p][i];
      if (in4) ++same_last4[p];
      for (int i = 3; i > 0; --i) last[p][i] = last[p][i - 1];
      last[p][0] = h[p];
      ++have[p];
    }
  }

  void Report(const char* scene) const {
    for (int p = 0; p < kPlanners; ++p)
      std::printf("[rplay][frame_shape] %s %s: drawn frames %llu, input hash = the previous frame's %.1f %%, = one of the last 4 %.1f %%\n", scene, kNames[p], static_cast<unsigned long long>(frames),
                  frames ? 100.0 * double(same_prev[p]) / double(frames) : 0.0, frames ? 100.0 * double(same_last4[p]) / double(frames) : 0.0);
  }
};

}  // namespace rdr_adapter
