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
  uint32_t lasting_target_passes = 0;     // S102 K2: object passes kept as seeds because they draw to a lasting render target (only without the group rule)
  uint32_t object_passes_dropped = 0;    // S102 K2 (wwe2k14_rdr_lasting_nogroup): object passes the lasting bind leaves out although another object pass is kept (the group rule would have kept them)
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
  // S102 K1 (piece B, wwe2k14_rdr_guard_decay): the state guard below was insert-only: an image read once before its copy was a lasting seed for the rest of the run, so the match's three moving shadow copies
  // (img214-216) stayed "lasting" in ~80 % of the skipped frames (R16). With guard_decay on, the guard decays on POSITIVE evidence only (review F2): per state image a count of the handled frames that read it AFTER its
  // copy (a per-frame use: the shadow map sampled by the scene) since it was last read BEFORE its copy; the image holds the guard while that count is <= guard_window. A frame with no read of the image does not advance it
  // (an image whose reader went away keeps its guard); a read before the copy resets it (state carried between frames). The window counts HANDLED frames (drawn and skipped), not seconds. Off (default) = insert-only.
  // guard_rule 1 (TEST ONLY, the planted old rule): decay by frames since the last state read, whether or not anything read the image (the first K1 draft): the stale-read ledger below must flag it.
  bool guard_decay = false;
  uint32_t guard_window = 16;   // rplay --guard-decay-frames N (the planted twin: 1)
  uint32_t guard_rule = 0;      // TEST ONLY: 1 = the time-based draft rule
  uint64_t guard_decayed = 0;   // census: copies the old insert-only guard would have held lasting and the decayed guard let the image rule drop (decay on)
  uint64_t would_decay = 0;     // census (decay OFF): copies the decayed guard would have let go: what the switch would take
  struct GuardRec {
    uint64_t last = 0;      // frames_handled at the last read before the copy (0 = no record)
    uint32_t clean = 0;     // handled frames that read it after its copy since then
    uint32_t gap = 0;       // S102 K2 (stale reads): the LONGEST handled-frame gap seen between two reads BEFORE the copy (monotone: a jittered cadence is held by its longest interval; 0 = read once so far)
    uint64_t prev_last = 0; // S102 K2 (review round 2, item 1): the read before copy BEFORE the latest one (Note() has already moved `last` to the current frame when NoteDrawn counts a stale read)
    bool pinned = false;    // S102 K2 (review F1 / F2): never decays: set by a state read in a frame with no copy into the image (it is consumed without being rewritten: nothing makes it per-frame), and by the
                            // first stale read the ledger counts (the image proved it carries state: the guard learned it the hard way)
    bool operator==(const GuardRec& o) const { return last == o.last && clean == o.clean && gap == o.gap && pinned == o.pinned && prev_last == o.prev_last; }
  };
  std::unordered_map<uint32_t, GuardRec> state_last;   // decay ON only (review F4): records of the images in state_images. Seeded from state_images at the current frame when the switch goes on.
  std::vector<GuardRec> watch_;                        // decay OFF: the same records on a flat array (ids < kLeanMax) for the would_decay counter and the gap histogram
  std::unordered_map<uint32_t, GuardRec> watch_big_;   // decay OFF: ids >= kLeanMax
  uint64_t gap_hist[8] = {};   // review F3: gaps (handled frames) between reads-before-copy of one image: 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+
  static uint32_t GapBucket(uint64_t g) { return g <= 1 ? 0 : g <= 2 ? 1 : g <= 4 ? 2 : g <= 8 ? 3 : g <= 16 ? 4 : g <= 32 ? 5 : g <= 64 ? 6 : 7; }
  // S102 K2 (stale reads with the decay, SKIP_COST.md): an image read before its copy only every N > guard_window handled frames (a state value consumed now and then: the Surface's ab11 arms read 100-270 stale reads) was
  // judged by the 16 frames of per-frame reads between two carry reads and dropped, so the skipped frames in between left its copy out and the next carry read found it owed. The guard now also holds while the reads after
  // the copy stay within TWICE the image's own observed carry cadence (`gap`); a single read (gap 0) decays as before (the match's shadow copies). cadence_blind (TEST ONLY, rplay --guard-rule blind) restores the faulty rule.
  bool cadence_blind = false;
  uint32_t pin_expire = 600;   // handled frames of reads after the copy with no read before it after which a pin and the gap are released
  bool pin_blind = false;   // TEST ONLY (--plant-nopin): pins are ignored (the review's run-end / first-stale cases come back)
  bool Alive(const GuardRec& r) const {
    if (guard_rule == 1) return frames_handled - r.last <= guard_window;
    if (r.pinned && !pin_blind && !cadence_blind) return true;
    const uint64_t hold = cadence_blind ? guard_window : std::max<uint64_t>(guard_window, 2ull * r.gap);
    return r.clean <= hold;
  }
  GuardRec* FindRec(uint32_t id, bool create) {   // decay on: state_last; off: the flat watch
    if (guard_decay) {
      auto it = state_last.find(id);
      if (it != state_last.end()) return &it->second;
      if (!create && !state_images.count(id)) return nullptr;
      GuardRec& r = state_last[id];   // the off-to-on edge: seeded from state_images at the current frame
      r.last = frames_handled, r.clean = 0;
      return &r;
    }
    GuardRec* r;
    if (id < kLeanMax) {
      if (id >= watch_.size()) { if (!create && !state_images.count(id)) return nullptr; watch_.resize(size_t(id) + 1024); }
      r = &watch_[id];
    } else {
      if (!create && !state_images.count(id) && !watch_big_.count(id)) return nullptr;
      r = &watch_big_[id];
    }
    if (!r->last) {
      if (!create && !state_images.count(id)) return nullptr;
      r->last = frames_handled, r->clean = 0;
    }
    return r;
  }
  // one image's flags for this frame (1 written, 2 read before its copy, 4 read after its copy; frame-level bits, see the walks)
  void GuardUpdate(uint32_t id, uint8_t f) {
    const bool state = (f & 2) != 0;
    if (state) state_images.insert(id);
    if (state) {
      GuardRec* r = FindRec(id, true);
      if (r->last && r->last < frames_handled) {
        ++gap_hist[GapBucket(frames_handled - r->last)];
        r->gap = std::max(r->gap, uint32_t(std::min<uint64_t>(frames_handled - r->last, 1u << 20)));
      }
      if (!(f & 1)) r->pinned = true;   // read with no copy into it in this frame: consumed, not rebuilt (review F1)
      r->prev_last = r->last;
      r->last = frames_handled, r->clean = 0;
    } else if (f & 4) {
      if (GuardRec* r = FindRec(id, false)) {
        ++r->clean;   // positive evidence: a read after its copy
        if (r->pinned && r->clean > pin_expire) r->pinned = false, r->gap = 0;   // review round 2 item 4: a pin (and the monotone gap) expires after pin_expire clean handled frames with no carry read: one camera cut or one pause must not stop the decay for the run
      }
    }
  }
  void Pin(uint32_t id) {   // the ledger saw a stale read of this image: it carries state (review F2); never decays again
    state_images.insert(id);
    FindRec(id, true)->pinned = true;
  }
  bool HeldByGuard(uint32_t id) {
    if (!guard_decay) return state_images.count(id) != 0;
    const GuardRec* r = FindRec(id, false);
    return r && Alive(*r);
  }
  // review F5 / F6: the stale-read ledger. A copy a skipped frame left out leaves its destination "owed" until a DRAWN frame writes it; a drawn frame that reads an owed image before writing it
  // read content a skipped frame never made (stale_read). It must read 0 (log: [rdr][skip_cost]); the guard decay is only right while it does.
  std::unordered_set<uint32_t> owed_;
  uint64_t stale_read = 0;   // total: drawn-frame reads of an owed image before any write, counted ONCE per owed episode (an image owed since a skipped frame left its copy out; later reads of the same owed content do not count again)
  // S102 K2 (review F1 / a): the stale counter split, so "must be 0" can be asked of guard decisions only. By the path that left the copy out (1 exact-rect streak, 2 image rule on an image never read before its copy,
  // 3 image rule after the DECAYED guard dropped it: the only path the decay is answerable for), by the state-read gap at the stale frame (handled frames since the image's last read before its copy), and by shape:
  // designed = the drawn frame writes the image later in the same frame (the img388 shape: every frame reads the previous copy, the game's own order), run end = the frame has no copy into it (its copy run ended on a skipped frame).
  uint64_t stale_by_path[4] = {}, stale_gap_le16 = 0, stale_gap_gt16 = 0, stale_designed = 0, stale_run_end = 0;
  uint64_t stale_frames = 0;   // PER READ: every drawn-frame read of an owed image before any write (the episode count above stands for unbounded frames when a run end is never written again)
  uint64_t StaleOpen() const { return settled_.size(); }   // gauge: owed images read and still unwritten right now
  std::unordered_map<uint32_t, uint8_t> stale_shape_;       // image -> 1 designed lag, 2 run end (the latest episode)
  uint64_t stale_guard() const { return stale_by_path[3]; }
  std::unordered_map<uint32_t, uint8_t> owed_reason_;
  std::unordered_set<uint32_t> settled_;   // owed images already counted in this episode
  // round 3 F2: the run-end keep decides per RECTANGLE (an atlas image whose rect A run ended while rect B goes on: the next-frame test uses DestRectKey, not the image). The LEDGER stays image-level on purpose: a moving rectangle
  // (the shadow copies) would owe every old position for ever, and a read samples the image, not a rectangle: a rect-level stale read of a continuing atlas is a known blind spot of the ledger (the keep still saves the rect).
  static uint64_t DestRectKey(const rdr::CopyOp& c) {
    uint64_t k = 0xcbf29ce484222325ull;
    const auto mix = [&](uint64_t v) { k = (k ^ v) * 0x100000001b3ull; };
    mix(uint64_t(c.dest.image)), mix(uint64_t(c.dest.mip)), mix(uint64_t(c.dest.layer)), mix(uint64_t(uint32_t(c.dest_rect.x))), mix(uint64_t(uint32_t(c.dest_rect.y))), mix(uint64_t(c.dest_rect.width)), mix(uint64_t(c.dest_rect.height));
    return k;
  }
  void OweRect(uint32_t image, uint64_t) { owed_.insert(image); }
  void ClearRect(uint32_t image, uint64_t) { owed_.erase(image); settled_.erase(image); }
  struct PendOwed { uint32_t id; uint8_t reason; uint64_t rk; };
  std::vector<PendOwed> pend_owed_;   // (image, reason); reason kKept = a copy this frame KEEPS (review round 2 item 3: it clears the debt only when the frame is really skipped, like the owing)
  static constexpr uint8_t kKept = 0xFF;   // left out by the last Classify, not yet owed (live: only a frame that is really skipped owes; a T3 draw-instead frame does not)
  bool defer_owed = false;   // live sets it and calls CommitOwed() on the really-skipped branch; tests and rplay commit at once
  // S102 K2 (round 2, the run-end recovery, wwe2k14_rdr_run_end_keep): the skipped frame that ENDS a copy run left its last copy out and nothing writes the image again (a shipped wrong picture: the image stays one frame
  // old). When the NEXT frame is already queued (a queue skip) its copy table is known: every copy this frame left out whose destination gets no copy in the next frame is kept (seed = 1, no longer owed). `next_dests`
  // = the destination images of the next frame's copies. Returns the kept destinations (live removes them from the catch-up list). run_end_kept counts the copies; the 30 fps rate skips have no next frame and are counted by the caller.
  uint64_t run_end_kept = 0;
  // Round 3 F2: the next-frame test is per RECTANGLE: a left-out copy is NOT kept only when the next frame copies into the same image/mip/layer over a rectangle that overlaps it by at least half of the smaller one (the same rule
  // as the image rule's moving-rectangle streak: a shadow copy that moves a few pixels goes on; an atlas rectangle whose run ended while another rectangle of the image continues does not).
  static bool Overlaps(const rdr::CopyOp& a, const rdr::CopyOp& b) {
    if (a.dest.image != b.dest.image || a.dest.mip != b.dest.mip || a.dest.layer != b.dest.layer) return false;
    const int64_t x0 = std::max<int64_t>(a.dest_rect.x, b.dest_rect.x), y0 = std::max<int64_t>(a.dest_rect.y, b.dest_rect.y);
    const int64_t x1 = std::min<int64_t>(int64_t(a.dest_rect.x) + a.dest_rect.width, int64_t(b.dest_rect.x) + b.dest_rect.width), y1 = std::min<int64_t>(int64_t(a.dest_rect.y) + a.dest_rect.height, int64_t(b.dest_rect.y) + b.dest_rect.height);
    if (x1 <= x0 || y1 <= y0) return false;
    const int64_t inter = (x1 - x0) * (y1 - y0), smaller = std::min<int64_t>(int64_t(a.dest_rect.width) * a.dest_rect.height, int64_t(b.dest_rect.width) * b.dest_rect.height);
    return inter * 2 >= smaller;
  }
  std::vector<uint32_t> KeepRunEnds(const LoadedFrame& frame, std::vector<uint8_t>& seed, const std::vector<rdr::CopyOp>& next_copies) {
    std::vector<uint32_t> kept;
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      if (frame.ops[i].kind != OpKind::Copy || seed[i]) continue;
      const rdr::CopyOp& c = frame.copies[frame.ops[i].index];
      const uint64_t rk = DestRectKey(c);
      bool goes_on = false;
      for (const rdr::CopyOp& nc : next_copies) goes_on |= Overlaps(c, nc);
      if (goes_on) continue;
      const uint32_t dest = uint32_t(c.dest.image);
      seed[i] = 1;
      ++run_end_kept;
      kept.push_back(dest);
      for (size_t k = pend_owed_.size(); k-- > 0;) if (pend_owed_[k].rk == rk && pend_owed_[k].reason != kKept) pend_owed_.erase(pend_owed_.begin() + long(k));
      if (ledger) pend_owed_.push_back({dest, kKept, rk});   // round 3 F1: cleared only when the frame is really skipped (CommitOwed), like the owing: a T3 draw-instead frame keeps nothing
    }
    return kept;
  }
  void CommitOwed() {
    for (const auto& pr : pend_owed_) {
      if (pr.reason == kKept) { ClearRect(pr.id, pr.rk); continue; }
      OweRect(pr.id, pr.rk);
      owed_reason_[pr.id] = pr.reason;
      settled_.erase(pr.id);
    }
    pend_owed_.clear();
  }
  std::unordered_map<uint32_t, uint32_t> stale_by_image;   // the ledger's count by image (the splice test reads the spliced image's own)
  std::unordered_map<uint32_t, uint32_t> by_image_dest;   // copies left out by the image rule, by destination image
  bool ledger = true;   // TEST ONLY (rplay --splice-reader): the classification run for binding does not feed the ledger
  void NoteDrawn(const LoadedFrame& frame) {
    if (owed_.empty()) return;
    std::unordered_set<uint32_t> written, counted, will_write;
    for (uint32_t i = 0; i < frame.ops.count; ++i)
      if (frame.ops[i].kind == OpKind::Copy) will_write.insert(uint32_t(frame.copies[frame.ops[i].index].dest.image));
    const auto read = [&](uint32_t id) {
      if (!owed_.count(id) || written.count(id) || !counted.insert(id).second) return;
      ++stale_frames;
      if (settled_.count(id)) return;
      settled_.insert(id);
      ++stale_read;
      ++stale_by_image[id];
      const auto rs = owed_reason_.find(id);
      ++stale_by_path[rs != owed_reason_.end() && rs->second < 4 ? rs->second : 0];
      const GuardRec* gr = FindRec(id, false);
      const uint64_t before = gr ? (gr->last == frames_handled ? gr->prev_last : gr->last) : 0;   // the read before copy BEFORE this frame's own (Note() has already moved `last` here)
      if (before && frames_handled - before <= 16) ++stale_gap_le16; else ++stale_gap_gt16;
      if (will_write.count(id)) ++stale_designed, stale_shape_[id] = 1; else ++stale_run_end, stale_shape_[id] = 2;
      Pin(id);   // review F2: the first stale read pins the image
    };
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::Draw) {
        const Draw& d = frame.draws[op.index];
        for (uint32_t k = 0; k < d.texture_count; ++k) read(frame.texture_slots[d.texture_first + k].image);
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = frame.copies[op.index];
        read(uint32_t(c.source.image));
        written.insert(uint32_t(c.dest.image));
        ClearRect(uint32_t(c.dest.image), DestRectKey(c));   // a drawn frame's write clears THAT rectangle's debt
      }
    }
  }
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
    {   // S102 S0-4 (J): the image bookkeeping always runs; only PerFrame consults by_image, so a mid-run switch-on finds live streaks
      if (img_seen.size() > 65536) img_seen.clear();
      // S102 RT1 (render_lean bit 4, Q9): note_flat keeps the per-image rectangle lists across frames (a stamp says which belong to this frame), so a recurring image costs no hash node and no vector
      // allocation; the walk below is the same body for both (a lambda), over the same set of images (order is irrelevant: each image's streak is its own).
      if (note_flat) {
        if (rects_flat_.size() > 4096) rects_flat_.clear();
        flat_keys_.clear();
        for (const rdr::CopyOp& c : frame.copies) {
          const uint64_t key = ImageKey(c);
          FlatRects& fr = rects_flat_[key];
          if (fr.stamp != frames_handled) fr.stamp = frames_handled, (plant_note_flat_stale ? void() : fr.rects.clear()), flat_keys_.push_back(key);   // (plant_note_flat_stale: TEST ONLY, the old frame's rectangles stay)
          if (fr.rects.size() <= kMaxRects) fr.rects.push_back({c.dest_rect.x, c.dest_rect.y, c.dest_rect.x + int32_t(c.dest_rect.width), c.dest_rect.y + int32_t(c.dest_rect.height)});
        }
        note_flat_frames++;
      } else {
      rects_.clear();
      for (const rdr::CopyOp& c : frame.copies) {
        auto& v = rects_[ImageKey(c)];
        if (v.size() <= kMaxRects) v.push_back({c.dest_rect.x, c.dest_rect.y, c.dest_rect.x + int32_t(c.dest_rect.width), c.dest_rect.y + int32_t(c.dest_rect.height)});
      }
      }
      const auto note_image = [&](uint64_t key, const std::vector<std::array<int32_t, 4>>& rect_list) {
        struct B { uint64_t first; const std::vector<std::array<int32_t, 4>>& second; } b{key, rect_list};
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
      };
      if (note_flat) {
        for (const uint64_t key : flat_keys_) note_image(key, rects_flat_[key].rects);
      } else {
        for (const auto& b : rects_) note_image(b.first, b.second);
      }
      // the guard: an image read before its copy this frame, or read in a frame that has no copy into it, holds content from an earlier frame
      if (lean) {   // S102 K1 (piece A, wwe2k14_rdr_skip_lean): the same walk on a flat array by image id (ids past kLeanMax go through the map): no node allocation per sampled texture
        if (!plant_lean_no_clear) guard_state_.clear();   // review F1: a flip from off to on must not re-walk last frame's map entries
        for (uint32_t i = 0; i < frame.ops.count; ++i) {
          const Op& op = frame.ops[i];
          if (op.kind == OpKind::Draw) {
            const Draw& d = frame.draws[op.index];
            for (uint32_t k = 0; k < d.texture_count; ++k) LeanRead(frame.texture_slots[d.texture_first + k].image);
          } else if (op.kind == OpKind::Copy) {
            const rdr::CopyOp& c = frame.copies[op.index];
            LeanRead(uint32_t(c.source.image));
            LeanWrite(uint32_t(c.dest.image));
          }
        }
        for (uint32_t id : lean_touched_) {
          const uint8_t f = lean_flags_[id];
          lean_flags_[id] = 0;
          GuardUpdate(id, f);
        }
        lean_touched_.clear();
        for (const auto& g : guard_state_) GuardUpdate(g.first, g.second);
        guard_state_.clear();
      } else {
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
      for (const auto& g : guard_state_) GuardUpdate(g.first, g.second);   // read before written (2) is state; read after its copy (4) is positive evidence for the decay
      }
      if (state_images.size() > 65536) { state_images.clear(); state_last.clear(); watch_.clear(); watch_big_.clear(); }
    }
    for (const rdr::CopyOp& c : frame.copies) {
      CopySeen& s = copy_seen[Key(c)];
      if (s.last == frames_handled) continue;  // twice in one frame
      if (s.last + 1 != frames_handled) s.missed = 0;  // A1b: a broken streak owes nothing (the effect stopped in between)
      s.streak = s.last + 1 == frames_handled ? s.streak + 1 : 1;
      s.last = frames_handled;
    }
  }
  // S102 S0-4 (I): the catch-up switch is off on a drawn frame: what skipped frames owed is forgotten, so a later switch-on does not replay a stale debt.
  void ClearMissed() { for (auto& kv : copy_seen) kv.second.missed = 0; }
  bool PerFrame(const rdr::CopyOp& c, bool* by_image_only = nullptr, uint8_t* reason = nullptr) {
    const auto it = copy_seen.find(Key(c));
    if (it != copy_seen.end() && it->second.streak >= kPerFrameStreak) { if (reason) *reason = 1; return true; }
    if (!by_image) return false;
    const auto im = img_seen.find(ImageKey(c));
    if (im == img_seen.end() || im->second.streak < kPerFrameStreak) return false;
    const uint32_t id = uint32_t(c.dest.image);
    const bool lasting_usage = images && id >= 1 && id <= images->size() && (*images)[id - 1].id == id && ((*images)[id - 1].usage & rdr::kUsageLasting);
    const bool held = HeldByGuard(id);
    if (!guard_decay && held && !lasting_usage) {   // review F5: what the switch would take, counted with it off
      const GuardRec* r = FindRec(id, false);
      if (r && !Alive(*r)) ++would_decay;
    }
    if (lasting_usage || held) {
      ++by_image_guarded;
      return false;
    }
    if (guard_decay && state_images.count(id)) ++guard_decayed;   // S102 K1: the old guard held it, the decayed one does not
    if (reason) *reason = guard_decay && state_images.count(id) ? 3 : 2;
    if (by_image_only) *by_image_only = true;
    return true;
  }
  // seed[i] = 1 for a lasting copy op; a per-frame copy (not state-carrying when keep_state_copies) calls left_out(copy) and is 0.
  template <class F>
  void Classify(const LoadedFrame& frame, bool keep_state_copies, std::vector<uint8_t>& seed, F&& left_out) {
    seed.assign(frame.ops.count, 0);
    pend_owed_.clear();
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
        uint8_t reason = 0;
        const bool per_frame = PerFrame(c, &image_only, &reason);
        if (per_frame && !(keep_state_copies && read_before)) {
          by_image_left += image_only;
          if (image_only) ++by_image_dest[uint32_t(c.dest.image)];   // census / the splice test: which images the image rule (and so the guard) decides for
          if (ledger) pend_owed_.push_back({uint32_t(c.dest.image), reason, DestRectKey(c)});   // review F5: owed until a drawn frame writes it (committed below, or by live on the really-skipped branch)
          left_out(c);
          continue;
        }
        seed[i] = 1;
        if (ledger) pend_owed_.push_back({dest, kKept, DestRectKey(c)});   // review F3: a skipped frame that KEEPS the copy makes the content: nothing is owed (applied by CommitOwed: round 2 item 3)
      }
    }
    if (!defer_owed) CommitOwed();
  }
  // S102 K1 piece A: the flat-array guard walk (flags: 1 written, 2 read before any write; the same bits as guard_state_)
  bool lean = false;
  bool note_flat = false;   // S102 RT1 (wwe2k14_rdr_render_lean bit 4): the per-image rectangle lists are kept across frames (no hash node / vector per image per frame)
  uint64_t note_flat_frames = 0;
  bool plant_note_flat_stale = false;   // TEST ONLY (tools\render_lean\note_flat_test.cpp)
  bool plant_lean_no_clear = false;   // TEST ONLY (guard_decay_test S5b): the flat branch does not clear the map walk's leftovers (review F1)
  bool plant_lean_no_write = false;   // TEST ONLY (guard_decay_test --plant-lean): the flat walk forgets a copy's write, so the state set differs from the map walk
  static constexpr uint32_t kLeanMax = 1u << 20;
  std::vector<uint8_t> lean_flags_;
  std::vector<uint32_t> lean_touched_;
  void LeanRead(uint32_t id) {
    if (id >= kLeanMax) { GuardRead(id); return; }
    if (id >= lean_flags_.size()) lean_flags_.resize(size_t(id) + 1024, 0);
    uint8_t& f = lean_flags_[id];
    if (!f) lean_touched_.push_back(id);   // 0 = untouched this frame (every touch sets a bit)
    f |= (f & 1) ? 4 : 2;
  }
  void LeanWrite(uint32_t id) {
    if (id >= kLeanMax) { guard_state_[id] |= 1; return; }
    if (plant_lean_no_write) return;
    if (id >= lean_flags_.size()) lean_flags_.resize(size_t(id) + 1024, 0);
    uint8_t& f = lean_flags_[id];
    if (!f) lean_touched_.push_back(id);
    f |= 1;
  }
  void GuardRead(uint32_t id) {
    uint8_t& g = guard_state_[id];
    g |= (g & 1) ? 4 : 2;   // 2: read before any write this frame; 4: read after its copy
  }
  std::unordered_map<uint64_t, std::vector<std::array<int32_t, 4>>> rects_;
  struct FlatRects { uint64_t stamp = 0; std::vector<std::array<int32_t, 4>> rects; };   // S102 RT1
  std::unordered_map<uint64_t, FlatRects> rects_flat_;
  std::vector<uint64_t> flat_keys_;
  std::unordered_map<uint32_t, uint8_t> guard_state_;   // 1 written, 2 read before any write
  std::vector<uint32_t> read_stamp;  // by the reader's image id: the frame handled that last sampled or copied from it
};

class Pruner {
 public:
  bool plant_no_producers = false;   // TEST ONLY (rplay --skip-prune none): only the lasting copies are bound, never the passes that make their sources
  bool no_promote = false;           // TEST ONLY (rplay --skip-prune nopromote): the walk does not go through per-frame copies (the pre-review behaviour), to separate their effect
  // S102 K2 (R16 piece C, wwe2k14_rdr_lasting_nogroup): false = the model-part group rule is not applied: a pass holding an engine-object draw is kept only when it feeds a seed. The binder then counts the D3 / thin call
  // ordinals over the whole frame's ops (binder.h ord_full_ops), so a dropped object pass shifts nothing. group_plant (TEST ONLY): 1 = the group rule is applied to about half of the object passes only (a hash of the pass and the frame number: the half-removed rule).
  bool group = true;
  int group_plant = 0;
  bool plant_no_attach = false;   // TEST ONLY (--plant-attach / rplay --skip-prune noattach / fview D3_NOGROUP_PLANT=noattach): a kept pass's own colour / depth attachments are not marked needed (review F1)
  bool lasting_targets = false;   // S102 K2 (review F4, sub-switch wwe2k14_rdr_lasting_nogroup_targets, default off): without the group rule, an object pass that draws to a kUsageLasting render target is kept as a seed
  bool plant_no_lasting_target = false;   // TEST ONLY (rplay / fview): without the group rule, an object pass that draws to a lasting render target is NOT kept as a seed (the J1 failure: a skipped frame's bake is dropped for good)
  const std::vector<ImageRecord>* images = nullptr;   // the source's image table (index id - 1), for TargetsLasting (kUsageLasting render targets); null = no pass counts as lasting
  bool TargetsLasting(const rdr::TargetsOp& t) const {
    if (!images) return false;
    const auto lasting_id = [&](uint32_t id) { return id >= 1 && id <= images->size() && (*images)[id - 1].id == id && ((*images)[id - 1].usage & rdr::kUsageLasting); };
    for (uint32_t c = 0; c < t.color_count; ++c)
      if (lasting_id(uint32_t(t.color[c].image))) return true;
    return t.depth.image != rdr::kNoImage && lasting_id(uint32_t(t.depth.image));
  }
  std::vector<uint8_t> kept_mask;    // S102 K2: per op of the last Select's frame: 1 = kept (the binder's ordinal view needs to know which runs the bind keeps)
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
      // S102 K2: the group rule also kept, by accident, every object pass that DRAWS TO a lasting render target (a bake whose product a later frame reads straight from the target, no copy: the crowd cards' atlas, role 6 with the
      // lasting flag). Without it (group = false) such a pass is lasting work in its own right: kept as a seed, with a counter (stats.lasting_target_passes). Only a pass with an engine-object draw: what the group rule covered, nothing more.
      if (!group && lasting_targets && !has_seed && t && sensitive_[s] && !plant_no_lasting_target && TargetsLasting(*t)) has_seed = true, ++stats.lasting_target_passes;
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
          // S102 K2 (review F1): a kept pass's own attachments are inputs too: a draw blends over the colour target's earlier content and depth-tests against the depth target's. Reverse walk: a full clear
          // earlier in the pass unmarks them again (above), so only an attachment the pass does NOT clear first pulls its earlier writers in. Only without the group rule (the shipped bind is the baseline).
          if (!group && !plant_no_attach && t) {
            for (uint32_t c = 0; c < t->color_count; ++c) Need(t->color[c].image);
            if (t->depth.image != rdr::kNoImage) Need(t->depth.image);
          }
        }
      }
    }
    // model-part passes together (the group rule; S102 K2: off with `group` = false: the binder's ordinals no longer depend on which object passes are bound)
    bool any_sensitive_kept = false;
    for (size_t s = 0; s < segs_.size(); ++s) any_sensitive_kept |= keep_[s] && sensitive_[s];
    if (any_sensitive_kept) {
      std::unordered_set<uint32_t> kept_sites;   // group_plant 2 (diagnosis, review F2c): the group rule only for passes that hold a draw of a pass site a kept object pass holds
      if (group_plant == 2)
        for (size_t s = 0; s < segs_.size(); ++s)
          if (keep_[s] && sensitive_[s])
            for (uint32_t i = segs_[s].begin; i < segs_[s].end; ++i)
              if (frame.ops[i].kind == OpKind::Draw) { const DrawOrigin* o = frame.OriginOf(frame.ops[i].index); if (o && o->object) kept_sites.insert(o->pass_site); }
      for (size_t s = 0; s < segs_.size(); ++s) {
        if (!sensitive_[s]) continue;
        bool site_match = false;
        if (group_plant == 2)
          for (uint32_t i = segs_[s].begin; i < segs_[s].end && !site_match; ++i)
            if (frame.ops[i].kind == OpKind::Draw) { const DrawOrigin* o = frame.OriginOf(frame.ops[i].index); site_match = o && o->object && kept_sites.count(o->pass_site); }
        size_t first_kept = segs_.size(), last_kept = 0;
        for (size_t q = 0; q < segs_.size(); ++q) if (keep_[q] && sensitive_[q]) { if (first_kept == segs_.size()) first_kept = q; last_kept = q; }
        const bool before_first = group_plant == 3 && s < first_kept, after_last = group_plant == 4 && s > last_kept;   // diagnosis: which dropped passes matter (review F2c)
        const bool apply = group || site_match || before_first || after_last || (group_plant == 1 && (((uint32_t(s) + stamp_ * 7u) * 2654435761u >> 16) & 1u));   // the planted half rule: about half of the object passes (a hash of the pass and the frame) are grouped, the rest are dropped (their kept successors' ordinals shift)
        if (!keep_[s] && apply) keep_[s] = 2, ++stats.grouped_kept;
        else if (!keep_[s]) ++stats.object_passes_dropped;
      }
    }
    kept_mask.assign(count, 0);
    for (size_t s = 0; s < segs_.size(); ++s) {
      if (sensitive_[s]) ++stats.object_passes, stats.object_passes_kept += keep_[s] ? 1 : 0;
      if (keep_[s]) ++stats.kept_passes;
      for (uint32_t i = segs_[s].begin; i < segs_[s].end; ++i) {
        const Op& op = frame.ops[i];
        const bool keep = op.kind == OpKind::Present || (op.kind == OpKind::Copy ? (seed[i] != 0 || promoted_[i] != 0) : (keep_[s] != 0 && !plant_no_producers));
        if (keep) {
          kept_mask[i] = 1;
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
