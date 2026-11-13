// Lane 307: what a skipped frame's lasting work needs bound. No game or SDK includes.
//
// A frame the render thread skips still does its "lasting work": the copies into textures that outlive the frame (an attire's atlas, ...). Before this lane
// every draw of the frame was bound for it (live.cpp put every op except a per-frame copy into lasting_ops): ~a drawn frame's bind cost, half of it thrown
// away (Surface 2026-11-13, bundle 2: 20-28 fps shown). Now only the passes that FEED a lasting copy are bound.
//
// The rule (a backward walk over the frame's ops, like CatchUp's walk from a copy to the SetTargets that rendered its source, but transitive):
//   needed = the set of images whose content, at this point of the frame, a kept op reads.
//   Walking from the last op to the first:
//     - a kept (lasting) copy: its source joins `needed` (no copy counts as a full overwrite: an over-kept earlier writer is safe).
//     - a Draw: the images it samples join `needed` (when its pass is kept).
//     - a pass (SetTargets..next SetTargets) is kept when it holds a lasting copy, or any of its targets is in `needed`. A kept pass keeps all its draws
//       and clears; a full Clear of a target removes it from `needed` (earlier writers of it are not needed by this pass).
//   Draws that carry per-frame (not lasting) copies are dropped as before (live.cpp: PerFrame).
// Draws with an engine object (model parts, D3 markers, thin ordinals) are counted per object over the whole frame, so those passes are kept or dropped TOGETHER (all passes holding such a draw): dropping one pass of an object while keeping another would shift the ordinals of the kept runs.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "capture_reader.h"

namespace rdr_adapter {
namespace lasting {

struct Stats {
  uint32_t ops = 0, draws = 0;            // the frame
  uint32_t kept_ops = 0, kept_draws = 0;  // what the lasting bind keeps
  uint32_t passes = 0, kept_passes = 0;
  uint32_t seeds = 0;                     // lasting copies
  uint32_t grouped_kept = 0;              // passes kept only because a ModelPart pass was kept
  uint32_t object_passes = 0, object_passes_kept = 0;   // passes holding a draw with an engine object (D3 / thin), and how many of them the lasting bind keeps
  uint32_t promoted_copies = 0;           // per-frame copies kept because a kept pass samples what they write (the walk goes through them)
  bool d3_blind() const { return object_passes && !object_passes_kept; }   // had object passes, none kept: the bind must not run the D3 / thin per-frame begin / end (an empty object list would look like "every object left")
};


// Which copies are lasting (S100 strip-360 A1, moved here from live.cpp so rplay's skip arm classifies the same way): a copy into the same image and
// rectangle in each of the last kPerFrameStreak frames handled is per-frame work that the drawn frame redoes. Two images used in turn never build a streak.
struct CopySeen {
  uint64_t last = 0;
  uint32_t streak = 0;
  uint32_t missed = 0;  // A1b: skipped frames since it last ran in which it was left out
};
// Lane 307: a copy whose rectangle moves every frame (the match's three 512x512 copies into img214-216: 128,72 256x368 then 120,64 264x376, ...)
// never builds the exact-rectangle streak, so all of them were "lasting" and every skipped match frame re-bound the whole frame for them. They are
// per-frame work like any other: the destination IMAGE is written in each of the last kPerFrameStreak frames, and each frame's rectangle (the frame's
// bounding box of its copies into it) overlaps the previous frame's by at least half of the smaller one. An attire built in tiles (a different part of the atlas
// each step) overlaps nothing and stays lasting. Off with by_image = false (live: --wwe2k14_rdr_perframe_by_image=false).
struct ImageSeen {
  uint64_t last = 0;
  uint32_t streak = 0;
  std::vector<std::array<int32_t, 4>> rects;   // the last frame's copy rectangles into it (x0, y0, x1, y1), at most kMaxRects
};
class CopyStreaks {
 public:
  bool by_image = true;
  uint64_t by_image_left = 0;   // census: copies treated as per-frame only by the image rule
  uint64_t by_image_guarded = 0;   // census: copies the image rule would have dropped but a guard (lasting usage, read before write) kept
  const std::vector<ImageRecord>* images = nullptr;   // the source's image table (index id - 1): kUsageLasting destinations are never dropped by the image rule
  static constexpr uint32_t kMaxRects = 16;
  std::unordered_map<uint64_t, ImageSeen> img_seen;
  std::unordered_set<uint32_t> state_images;   // images read in a frame before the frame's copy into them, or in a frame with no copy into them: they carry content between frames, the image rule never applies
  static uint64_t ImageKey(const rdr::CopyOp& c) { return (uint64_t(c.dest.image) << 16) ^ (uint64_t(c.dest.mip) << 8) ^ c.dest.layer ^ 0x9E3779B97F4A7C15ull; }
  static constexpr uint32_t kPerFrameStreak = 3;
  std::unordered_map<uint64_t, CopySeen> copy_seen;
  uint64_t frames_handled = 0;
  static uint64_t Key(const rdr::CopyOp& c) {
    uint64_t k = 0xcbf29ce484222325ull;
    const auto mix = [&](uint64_t v) { k = (k ^ v) * 0x100000001b3ull; };
    mix(c.dest.image);
    mix(uint64_t(c.dest.mip) << 8 | c.dest.layer);
    mix(uint64_t(uint32_t(c.dest_rect.x)) << 32 | uint32_t(c.dest_rect.y));
    mix(uint64_t(c.dest_rect.width) << 32 | c.dest_rect.height);
    return k;
  }
  void Note(const LoadedFrame& frame) {
    ++frames_handled;
    if (copy_seen.size() > 65536) copy_seen.clear();
    if (by_image) {
      if (img_seen.size() > 65536) img_seen.clear();
      rects_.clear();
      for (const rdr::CopyOp& c : frame.copies) {
        auto& v = rects_[ImageKey(c)];
        if (v.size() <= kMaxRects) v.push_back({c.dest_rect.x, c.dest_rect.y, c.dest_rect.x + int32_t(c.dest_rect.width), c.dest_rect.y + int32_t(c.dest_rect.height)});
      }
      for (const auto& b : rects_) {
        ImageSeen& s = img_seen[b.first];
        bool consecutive = s.last + 1 == frames_handled && b.second.size() <= kMaxRects && s.rects.size() <= kMaxRects;
        // every copy rectangle of this frame overlaps a rectangle of the previous frame by at least half of the smaller one (a raster fill of a big atlas, several tiles a frame, does not)
        for (size_t k = 0; consecutive && k < b.second.size(); ++k) {
          bool hit = false;
          for (const auto& q : s.rects) {
            const auto& r = b.second[k];
            const int64_t ow = std::min(q[2], r[2]) - int64_t(std::max(q[0], r[0])), oh = std::min(q[3], r[3]) - int64_t(std::max(q[1], r[1]));
            const int64_t overlap = ow > 0 && oh > 0 ? ow * oh : 0;
            const int64_t a_prev = int64_t(q[2] - q[0]) * (q[3] - q[1]), a_now = int64_t(r[2] - r[0]) * (r[3] - r[1]);
            if (overlap > 0 && overlap * 2 >= std::min(a_prev, a_now)) { hit = true; break; }
          }
          consecutive = hit;
        }
        s.streak = consecutive ? s.streak + 1 : 1;
        s.last = frames_handled;
        s.rects = b.second;
      }
      // the guard: an image read before its copy this frame, or read in a frame that has no copy into it, holds content from an earlier frame
      guard_state_.clear();
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const Op& op = frame.ops[i];
        if (op.kind == OpKind::Draw) {
          const Draw& d = frame.draws[op.index];
          for (uint32_t k = 0; k < d.texture_count; ++k) GuardRead(frame.texture_slots[d.texture_first + k].image);
        } else if (op.kind == OpKind::Copy) {
          const rdr::CopyOp& c = frame.copies[op.index];
          GuardRead(uint32_t(c.source.image));
          guard_state_[uint32_t(c.dest.image)] |= 1;   // written
        }
      }
      for (const auto& g : guard_state_)
        if ((g.second & 2) || !(g.second & 1)) state_images.insert(g.first);   // read before written, or read and never written this frame
      if (state_images.size() > 65536) state_images.clear();
    }
    for (const rdr::CopyOp& c : frame.copies) {
      CopySeen& s = copy_seen[Key(c)];
      if (s.last == frames_handled) continue;  // twice in one frame
      if (s.last + 1 != frames_handled) s.missed = 0;  // A1b: a broken streak owes nothing (the effect stopped in between)
      s.streak = s.last + 1 == frames_handled ? s.streak + 1 : 1;
      s.last = frames_handled;
    }
  }
  bool PerFrame(const rdr::CopyOp& c, bool* by_image_only = nullptr) {
    const auto it = copy_seen.find(Key(c));
    if (it != copy_seen.end() && it->second.streak >= kPerFrameStreak) return true;
    if (!by_image) return false;
    const auto im = img_seen.find(ImageKey(c));
    if (im == img_seen.end() || im->second.streak < kPerFrameStreak) return false;
    const uint32_t id = uint32_t(c.dest.image);
    const bool lasting_usage = images && id >= 1 && id <= images->size() && (*images)[id - 1].id == id && ((*images)[id - 1].usage & rdr::kUsageLasting);
    if (lasting_usage || state_images.count(id)) {
      ++by_image_guarded;
      return false;
    }
    if (by_image_only) *by_image_only = true;
    return true;
  }
  // seed[i] = 1 for a lasting copy op; a per-frame copy (not state-carrying when keep_state_copies) calls left_out(copy) and is 0.
  template <class F>
  void Classify(const LoadedFrame& frame, bool keep_state_copies, std::vector<uint8_t>& seed, F&& left_out) {
    seed.assign(frame.ops.count, 0);
    const uint32_t stamp = uint32_t(frames_handled);
    const auto mark_read = [&](uint32_t id) {
      if (id >= read_stamp.size()) read_stamp.resize(size_t(id) + 1024, 0);
      read_stamp[id] = stamp;
    };
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::Draw) {
        const Draw& d = frame.draws[op.index];
        for (uint32_t s = 0; s < d.texture_count; ++s) mark_read(frame.texture_slots[d.texture_first + s].image);
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = frame.copies[op.index];
        const uint32_t dest = uint32_t(c.dest.image);
        const bool read_before = dest < read_stamp.size() && read_stamp[dest] == stamp;
        mark_read(uint32_t(c.source.image));
        bool image_only = false;
        const bool per_frame = PerFrame(c, &image_only);
        if (per_frame && !(keep_state_copies && read_before)) {
          by_image_left += image_only;
          left_out(c);
          continue;
        }
        seed[i] = 1;
      }
    }
  }
  void GuardRead(uint32_t id) {
    if (!(guard_state_[id] & 1)) guard_state_[id] |= 2;   // not written yet this frame
  }
  std::unordered_map<uint64_t, std::vector<std::array<int32_t, 4>>> rects_;
  std::unordered_map<uint32_t, uint8_t> guard_state_;   // 1 written, 2 read before any write
  std::vector<uint32_t> read_stamp;  // by the reader's image id: the frame handled that last sampled or copied from it
};

class Pruner {
 public:
  bool plant_no_producers = false;   // TEST ONLY (rplay --skip-prune none): only the lasting copies are bound, never the passes that make their sources
  bool no_promote = false;           // TEST ONLY (rplay --skip-prune nopromote): the walk does not go through per-frame copies (the pre-review behaviour), to separate their effect
 public:
  // seed[i] != 0 for every Copy op i that is lasting work. out receives the kept ops (frame order), `dropped` the others (frame order; Present ops are always kept).
  // plant_incomplete (TEST ONLY): the walk-back stops at the first level (only passes that render a lasting copy's source; nothing they sample): the planted fault.
  void Select(const LoadedFrame& frame, const std::vector<uint8_t>& seed, std::vector<Op>& out, std::vector<Op>& dropped, Stats& stats, bool plant_incomplete = false) {
    out.clear();
    dropped.clear();
    const uint32_t count = frame.ops.count;
    stats = Stats{};
    stats.ops = count;
    ++stamp_;
    if (stamp_ == 0) stamp_ = 1, needed_.assign(needed_.size(), 0);
    // segments
    segs_.clear();
    for (uint32_t i = 0; i < count; ++i) {
      if (frame.ops[i].kind == OpKind::SetTargets || segs_.empty()) segs_.push_back({i, i + 1});
      else segs_.back().end = i + 1;
    }
    keep_.assign(segs_.size(), 0);
    sensitive_.assign(segs_.size(), 0);
    promoted_.assign(count, 0);
    for (size_t s = 0; s < segs_.size(); ++s) {
      for (uint32_t i = segs_[s].begin; i < segs_[s].end; ++i) {
        const Op& op = frame.ops[i];
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          const DrawOrigin* o = frame.OriginOf(op.index);
          if (o && o->object) sensitive_[s] = 1;
        } else if (op.kind == OpKind::Copy && seed[i]) {
          ++stats.seeds;
        }
      }
    }
    stats.passes = uint32_t(segs_.size());
    for (size_t s = segs_.size(); s-- > 0;) {
      const Seg& seg = segs_[s];
      const rdr::TargetsOp* t = frame.ops[seg.begin].kind == OpKind::SetTargets ? &frame.targets[frame.ops[seg.begin].index] : nullptr;
      bool has_seed = false;
      for (uint32_t i = seg.begin; i < seg.end; ++i) {
        has_seed |= frame.ops[i].kind == OpKind::Copy && seed[i];
        // a per-frame copy (not lasting) whose destination a kept pass samples is part of the chain: its pass is kept too
        if (!plant_incomplete && !no_promote && frame.ops[i].kind == OpKind::Copy && !seed[i] && Needed(uint32_t(frame.copies[frame.ops[i].index].dest.image))) has_seed = true;
      }
      bool feeds = has_seed;
      if (t && !feeds) {
        for (uint32_t c = 0; c < t->color_count; ++c) feeds |= Needed(t->color[c].image);
        if (t->depth.image != rdr::kNoImage) feeds |= Needed(t->depth.image);
      }
      keep_[s] = feeds ? 1 : 0;
      if (!feeds) continue;
      // reverse over the pass's ops
      for (uint32_t i = seg.end; i-- > seg.begin;) {
        const Op& op = frame.ops[i];
        if (op.kind == OpKind::Copy && seed[i]) {
          const rdr::CopyOp& c = frame.copies[op.index];
          Need(c.source.image);
        } else if (op.kind == OpKind::Copy && !plant_incomplete && !no_promote) {
          const rdr::CopyOp& c = frame.copies[op.index];
          if (Needed(uint32_t(c.dest.image))) {   // S2: the walk goes through a per-frame copy that makes a texture a kept pass samples
            promoted_[i] = 1;
            ++stats.promoted_copies;
            Need(c.source.image);
          }
        } else if (op.kind == OpKind::Clear && t) {
          const rdr::ClearOp& cl = frame.clears[op.index];
          if (cl.full) {
            if ((cl.flags & rdr::kClearColor) && cl.color_mask == 0xF)
              for (uint32_t c = 0; c < t->color_count; ++c) Unneed(t->color[c].image);
            if ((cl.flags & rdr::kClearDepth) && (cl.flags & rdr::kClearStencil) && t->depth.image != rdr::kNoImage) Unneed(t->depth.image);
          }
        } else if (op.kind == OpKind::Draw && !plant_incomplete) {
          const Draw& d = frame.draws[op.index];
          for (uint32_t k = 0; k < d.texture_count; ++k) Need(frame.texture_slots[d.texture_first + k].image);
        }
      }
    }
    // model-part passes together
    bool any_sensitive_kept = false;
    for (size_t s = 0; s < segs_.size(); ++s) any_sensitive_kept |= keep_[s] && sensitive_[s];
    if (any_sensitive_kept) {
      for (size_t s = 0; s < segs_.size(); ++s)
        if (!keep_[s] && sensitive_[s]) keep_[s] = 2, ++stats.grouped_kept;
    }
    for (size_t s = 0; s < segs_.size(); ++s) {
      if (sensitive_[s]) ++stats.object_passes, stats.object_passes_kept += keep_[s] ? 1 : 0;
      if (keep_[s]) ++stats.kept_passes;
      for (uint32_t i = segs_[s].begin; i < segs_[s].end; ++i) {
        const Op& op = frame.ops[i];
        const bool keep = op.kind == OpKind::Present || (op.kind == OpKind::Copy ? (seed[i] != 0 || promoted_[i] != 0) : (keep_[s] != 0 && !plant_no_producers));
        if (keep) {
          out.push_back(op);
          ++stats.kept_ops;
          if (op.kind == OpKind::Draw) ++stats.kept_draws;
        } else {
          dropped.push_back(op);
        }
      }
    }
  }

 private:
  struct Seg {
    uint32_t begin, end;
  };
  bool Needed(uint32_t id) const { return id < needed_.size() && needed_[id] == stamp_; }
  void Need(uint32_t id) {
    if (!id) return;
    if (id >= needed_.size()) needed_.resize(size_t(id) + 1024, 0);
    needed_[id] = stamp_;
  }
  void Unneed(uint32_t id) {
    if (id < needed_.size() && needed_[id] == stamp_) needed_[id] = 0;
  }
  std::vector<uint32_t> needed_;
  uint32_t stamp_ = 0;
  std::vector<Seg> segs_;
  std::vector<uint8_t> keep_, sensitive_, promoted_;
};

}  // namespace lasting
}  // namespace rdr_adapter
