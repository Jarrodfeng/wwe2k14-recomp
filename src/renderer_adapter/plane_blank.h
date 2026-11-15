// S102 lane G4b (docs\renderer\DEAD_WORK.md, "The blank plane shadow chain"): the plane shadow chain, left out of the stream's walk when the frame's own constants say it draws nothing.
// No game or SDK includes.
//
// The chain (4 passes of the ring-mat plane shadow, lane P3's PLANE_OURS_WHOLE.md): the mask (SetTargets, Clear, 12-36 proxy draws of pass site 0x82BF4D38, Copy into image A), the downscale
// (one draw of call site 0x828159FC, Copy into B), two blurs (one draw each of call site 0x827EEC84, Copy into B). B is the plane-shadow picture the colour passes sample.
//
// THE RULE (decided at record time from the frame's own draws, never from pixels): the chain is BLANK in a frame when
//   every mask draw's vertex bank holds a real module upload (c0.x, the fade distance, is above 0: a zero / missing bank can never pass) and its c6.w is exactly 0, and
//   every blur draw's bank holds c33.x == 1 (a real upload) and c35.x == 0.
// c6.w and c35.x are the same float, module+104, "the weight" the plane module's screen fit sub_827ED960 stores at 0x827EE0E4 (DEAD_WORK.md). The blur's PlaneScale (c35) scales its quad: weight 0
// collapses it; the last blur pass first clears its target to a fixed colour over the whole target (Opus review), so B ends as that clear colour (zero in every capture).
//
// THE LEDGER: a skipped chain leaves B as it was. So a blank chain is first EXECUTED once (it writes B back to the last blur pass's clear colour: the rule's claim); after that it is skipped until a
// frame whose chain is not blank (or irregular, declined, a part of a split frame, a switch-off, an isolate / velocity bind, a persist-only bind that touches a chain image) runs: B is then unknown
// again and the next blank chain is executed once more. The ledger also holds the clear colour: a different one is a first run again. The images the chain writes (its targets, A and B) must be
// touched by nothing outside the chain in the frame, except that draws may READ B; no CPU upload into them. A frame with no chain leaves the ledger alone unless it writes a chain image.
//
// What is skipped: the 4 SetTargets, the Clears, every Draw, the 4 Copies (G4's mechanism: none of the ops is recorded). Not skipped (declined, counted): another plan already drops an op of the chain
// (`others`), or another lane's own pass takes the chain (`chain_taken`).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "capture_reader.h"
#include "dead_work.h"

namespace rdr_adapter::plane_blank {

inline constexpr uint32_t kMaskSite = 0x82BF4D38u;       // pass_site of the mask's proxy draws (T_PlaneShadowMap)
inline constexpr uint32_t kDownCall = 0x828159FCu;       // call_site of the downscale draw (yDownScale)
inline constexpr uint32_t kBlurCall = 0x827EEC84u;       // call_site of the two blur draws (yBlur plane_x, plane_z)

// fview's fallback for the switch (the live table is not running there): --skip-blank-plane. The live switch is wwe2k14_rdr_skip_blank_plane (rdr_switches.h, PerFrame).
inline std::atomic<bool> g_fview{false};

// Planted faults (tools only). Each must change pictures / counts where the rule or the ledger is the only protection.
enum class Fault : uint8_t {
  None,
  Always,         // the rule says blank on every frame that has a chain (Check B: the chain is live: pictures must differ)
  Window,         // the rule says blank on guest frames [g_window_lo, g_window_hi) only (the ledger: pictures differ inside, and ONLY inside)
  NoFirstRun,     // blank frames are skipped without the first executed chain (a unit test fails it: the ledger is the only protection)
  IgnoreOverlap,  // another plan's drop set is not looked at (a unit test fails it: two plans would drop the same ops)
  IgnoreHalf,     // S102 G4c: a half-rate frame (the chain is not drawn at all) counts as the first executed run (a unit test fails it: B was never written)
  DropMask,       // S102 G4c (review F1): the mask pass is dropped although the half rate / G3 can leave it out of a later frame (A goes stale: the unit test fails it)
  IgnoreReaders,  // S102 G4c: a draw that still reads a skipped chain image (not B) is not looked at (a unit test fails it)
  Count
};
inline Fault g_fault = Fault::None;
inline uint64_t g_window_lo = 0, g_window_hi = 0;   // GUEST frames (FrameFileHeader::guest_frame)
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "always", "window", "no_first_run", "ignore_overlap", "ignore_half", "drop_mask", "ignore_readers"};
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}

struct Census {
  uint64_t frames = 0, frames_unplanned = 0, frames_no_chain = 0, frames_no_origins = 0, frames_not_guest = 0;
  uint64_t chains = 0;          // frames with a regular chain
  uint64_t irregular = 0;       // a chain of another shape (never skipped)
  uint64_t live = 0;            // the rule says not blank: the chain runs
  uint64_t outside = 0;         // blank, but something outside the chain touches its images: runs
  uint64_t declined_overlap = 0;// blank, but another plan already drops an op of the chain: runs (that plan owns it)
  uint64_t declined_taken = 0;  // blank, but another lane's own pass takes the chain: runs (it owns it)
  uint64_t first_run = 0;       // blank, the ledger not clean: run once to write B back to its clear colour
  uint64_t skipped = 0;         // chains left out
  uint64_t forgotten = 0;       // the ledger was cleared by a switch-off / an isolate bind / a persist-only touch / a write outside a chain
  uint64_t targets = 0, clears = 0, draws = 0, copies = 0;   // what the skipped chains held
  uint64_t ns = 0;              // S102 G4c: the planner's own time (Build), summed: the census line prints the mean a frame (it is per frame, never per draw)
  uint64_t mask_kept = 0;       // S102 G4c (review F1): taken frames whose mask pass was kept because a half-rate / turn frame may leave the mask out of the next one
  uint64_t half_frames = 0;     // S102 G4c: frames the half rate leaves the whole chain out of (nothing to take, nothing run: the ledger is untouched unless the frame is not blank)
  uint64_t forced = 0;          // (planted fault) frames the fault called blank although the rule did not
};

struct Chain {
  uint32_t set_op[4], clear_op[4], copy_op[4];
  std::vector<uint32_t> draws[4];
  uint32_t target[4], dest[4], depth[4];
};

// What the caller knows about this bind.
struct Ctx {
  uint64_t guest_frame = 0;
  bool plan_ok = true;        // false: a part of a split frame / a lasting-only bind: not planned
  bool ledger_ok = true;      // false: not a guest-frame bind (isolate / velocity): the ledger is cleared
  bool persist_only = false;  // a lasting-only bind: no chain is planned, the ledger is cleared only if a chain image is touched
  std::function<bool(uint32_t)> half_skips;   // S102 G4c: set on a frame whose half rate leaves shadow targets out: does it leave THIS image's pass out (the binder's OldHalfSkips)? All four chain targets: the chain is NOT executed, whatever the rule says; some: a partial chain, never ours
  bool chain_taken = false;   // another lane's own pass (P3's own plane) takes the chain
  bool keep_mask = false;   // S102 G4c (review F1): the half rate or G3's turns are on: some later frame can leave the mask out and read A (the mask's copy) unredrawn, so A must stay what the stream leaves it
  const uint8_t* turns = nullptr;   // S102 G4c (review F3): G3's per-op turns drop set (size >= ops): a pass it leaves out is the half rate's case, not an overlap
  std::vector<const uint8_t*> others;   // other plans' drop sets (indexed by op, nonzero = dropped; size >= ops): dead_plan_, bake_drop_, post_claim_ (while claimed), G3's turns
};

class Planner {
 public:
  Census census;

  // The switch went off (or the bind is not ours to judge): B is unknown.
  void Forget() {
    if (clean_) ++census.forgotten;
    clean_ = false;
  }
  // Is `image` one of the last chain's images? (copy_alias excludes them while the switch is on: a skipped copy must not be judged by the alias rule.)
  bool IsChainImage(uint32_t image) const { return image && images_.count(image) != 0; }

  // Plans one frame into `plan` (1 = not recorded).
  void Build(const LoadedFrame& f, const Ctx& ctx, dead_work::Plan& plan) {
    struct Clock {   // S102 G4c: what the planner itself costs the render thread (two clock reads a frame)
      uint64_t& sum;
      std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
      ~Clock() { sum += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
    } clock{census.ns};
    plan.Reset(f.ops.count);
    if (!ctx.ledger_ok) { ++census.frames_not_guest; Forget(); return; }   // F11: isolate / velocity
    ++census.frames;
    if (ctx.persist_only) {   // F6: a lasting-only bind runs few ops; it clears the ledger only when it writes a chain image
      if (clean_ && Writes(f)) Forget();
      return;
    }
    if (!ctx.plan_ok) { ++census.frames_unplanned; Forget(); return; }   // a part of a split frame: whatever ran there is not ours to judge
    if (!f.origins.count) { ++census.frames_no_origins; Forget(); return; }
    Chain ch;
    const int found = Find(f, ch);
    if (found == 0) {   // F8: no chain, but something may write its images
      ++census.frames_no_chain;
      if (clean_ && Writes(f)) Forget();
      return;
    }
    if (found < 0) { ++census.irregular; Forget(); return; }
    ++census.chains;
    Remember(ch);
    bool blank = Blank(f, ch);
    const bool forced_window = g_fault == Fault::Window && ctx.guest_frame >= g_window_lo && ctx.guest_frame < g_window_hi;
    if ((g_fault == Fault::Always || forced_window) && !blank) blank = true, ++census.forced;
    // S102 G4c: the half rate leaves some or all of the chain's passes out of this drawn frame (R17: the mask and its copy are in the learned shadow set; the blur passes need not be).
    // What the stream would run is what counts: B ends as the last blur pass's clear colour when that pass runs, and stays as it was when it does not.
    bool hs[4] = {false, false, false, false};
    int half = 0;
    if (ctx.half_skips && g_fault != Fault::IgnoreHalf)
      for (int k = 0; k < 4; ++k) hs[k] = ctx.half_skips(ch.target[k]);
    if (ctx.turns && g_fault != Fault::IgnoreHalf)   // review F3: G3's turn leaves a pass out exactly as the half rate does
      for (int k = 0; k < 4; ++k) hs[k] = hs[k] || ctx.turns[ch.set_op[k]];
    for (int k = 0; k < 4; ++k) half += hs[k] ? 1 : 0;
    if (half == 4) {   // the chain does not run at all: nothing to take, and a blank chain here is NOT the ledger's first executed run (B is not written back)
      ++census.half_frames;
      if (!blank || !Alone(f, ch)) Forget();   // a live chain / a writer outside: what B holds is unknown from here (the half rate keeps the last drawn one)
      return;
    }
    if (!blank) { ++census.live; Forget(); return; }
    if (ctx.chain_taken) { ++census.declined_taken; Forget(); return; }
    if (g_fault != Fault::IgnoreOverlap && Overlaps(f, ch, ctx)) { ++census.declined_overlap; Forget(); return; }
    if (!Alone(f, ch)) { ++census.outside; Forget(); return; }
    float colour[4];
    const rdr::ClearOp& last = f.clears[f.ops[ch.clear_op[3]].index];   // (Find guarantees a full, colour-flagged clear on pass 3)
    std::memcpy(colour, last.color, sizeof colour);
    float colour0[4];
    std::memcpy(colour0, f.clears[f.ops[ch.clear_op[0]].index].color, sizeof colour0);
    const bool same = clean_ && final_ == ch.dest[3] && std::memcmp(colour, colour_, sizeof colour) == 0 && std::memcmp(colour0, colour0_, sizeof colour0) == 0;
    // B is written by the passes 1-3 whatever A holds (the last blur pass clears it and draws a collapsed quad), so a first run needs the last blur pass only; this lane never leaves A stale (keep_mask).
    if (!same && hs[3]) {   // S102 G4c: B is unknown and the stream would not run the last blur pass: nothing is written back, nothing is ours to take
      ++census.half_frames;
      Forget();
      return;
    }
    std::memcpy(colour_, colour, sizeof colour);
    std::memcpy(colour0_, colour0, sizeof colour0);
    final_ = ch.dest[3];
    clean_ = true;
    if (!same && g_fault != Fault::NoFirstRun) { ++census.first_run; return; }   // run it once: B goes back to the clear colour
    ++census.skipped;
    const bool keep_mask = (ctx.keep_mask || half > 0) && g_fault != Fault::DropMask;   // review F1: A (the mask's copy) is read by the downscale of a frame that leaves the mask out: never leave it stale
    if (keep_mask) ++census.mask_kept;
    for (int k = keep_mask ? 1 : 0; k < 4; ++k) {   // (a pass the half rate already leaves out is dropped too, and not counted: the census holds what this lane really removed)
      const uint32_t real = hs[k] ? 0u : 1u;
      plan.drop[ch.set_op[k]] = 1;
      ++plan.count, census.targets += real;
      if (ch.clear_op[k] != kNone) plan.drop[ch.clear_op[k]] = 1, ++plan.count, census.clears += real;
      for (uint32_t d : ch.draws[k]) plan.drop[d] = 1, ++plan.count, census.draws += real;
      plan.drop[ch.copy_op[k]] = 1;
      ++plan.count, census.copies += real;
    }
  }

  std::string Report() const {
    char b[720];
    std::snprintf(b, sizeof b,
                  "[rdr][plane_blank] frames %llu (unplanned %llu, no chain %llu, no origins %llu, not guest %llu), chains %llu: live %llu, outside %llu, declined (overlap %llu, taken %llu), irregular %llu, first run %llu, skipped %llu "
                  "(targets %llu, clears %llu, draws %llu, copies %llu), ledger cleared %llu, half-rate frames %llu, mask kept %llu, planner %.1f us a frame%s",
                  static_cast<unsigned long long>(census.frames), static_cast<unsigned long long>(census.frames_unplanned), static_cast<unsigned long long>(census.frames_no_chain),
                  static_cast<unsigned long long>(census.frames_no_origins), static_cast<unsigned long long>(census.frames_not_guest), static_cast<unsigned long long>(census.chains),
                  static_cast<unsigned long long>(census.live), static_cast<unsigned long long>(census.outside), static_cast<unsigned long long>(census.declined_overlap),
                  static_cast<unsigned long long>(census.declined_taken), static_cast<unsigned long long>(census.irregular), static_cast<unsigned long long>(census.first_run),
                  static_cast<unsigned long long>(census.skipped), static_cast<unsigned long long>(census.targets), static_cast<unsigned long long>(census.clears),
                  static_cast<unsigned long long>(census.draws), static_cast<unsigned long long>(census.copies), static_cast<unsigned long long>(census.forgotten), static_cast<unsigned long long>(census.half_frames), static_cast<unsigned long long>(census.mask_kept), (census.frames + census.frames_not_guest) ? double(census.ns) / double(census.frames + census.frames_not_guest) / 1000.0 : 0.0,
                  g_fault == Fault::None ? "" : " FAULT");
    return b;
  }

 private:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  bool clean_ = false;     // B is known to hold the last blur pass's clear colour: the last chain that ran was blank (and nothing since has touched it)
  uint32_t final_ = 0;     // the image id B the ledger is about
  float colour_[4] = {};   // the clear colour B was last written with
  float colour0_[4] = {};  // S102 G4c (review F1b): the mask pass's own full clear colour: A is that colour after a blank mask (proved on captures: tools/renderer/g4c_check.py), a different one is a first run again
  std::unordered_set<uint32_t> images_;   // the last chain's targets, depths and destinations

  void Remember(const Chain& ch) {
    images_.clear();
    for (int k = 0; k < 4; ++k) {
      images_.insert(ch.target[k]);
      images_.insert(ch.dest[k]);
      if (ch.depth[k]) images_.insert(ch.depth[k]);
    }
  }

  // F14: a vertex constant through the block API (T1b: a lazy bank resolves one 64-byte block, not the whole 4 KB bank).
  static float Reg(const LoadedFrame& f, const Draw& d, uint32_t row, uint32_t lane) {
    const uint32_t* block = f.constant_banks.Block(d.vs_bank, row / 4);
    float v;
    std::memcpy(&v, &block[(row % 4) * 4 + lane], 4);
    return v;
  }

  // Does this frame write any image of the last chain (a target, a copy destination, CPU content)? Used where no chain is planned.
  bool Writes(const LoadedFrame& f) const {
    for (const ContentRef& c : f.contents)
      if (IsChainImage(c.image)) return true;
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = f.targets[op.index];
        for (uint32_t c = 0; c < t.color_count; ++c)
          if (IsChainImage(t.color[c].image)) return true;
        if (IsChainImage(t.depth.image)) return true;
      } else if (op.kind == OpKind::Copy) {
        if (IsChainImage(f.copies[op.index].dest.image)) return true;
      }
    }
    return false;
  }

  // 0 = no chain in this frame, 1 = a regular chain (filled in), -1 = a chain of another shape.
  static int Find(const LoadedFrame& f, Chain& ch) {
    const uint32_t n = f.ops.count;
    uint32_t first = kNone;
    for (uint32_t i = 0; i < n; ++i) {
      if (f.ops[i].kind != OpKind::Draw) continue;
      const DrawOrigin* o = f.OriginOf(f.ops[i].index);
      if (o && o->pass_site == kMaskSite) { first = i; break; }
    }
    if (first == kNone) return 0;
    uint32_t i = first;
    while (i > 0 && f.ops[i].kind != OpKind::SetTargets) --i;
    if (f.ops[i].kind != OpKind::SetTargets) return -1;
    static const int kKind[4] = {0, 1, 2, 2};
    for (int k = 0; k < 4; ++k) {
      if (i >= n || f.ops[i].kind != OpKind::SetTargets) return -1;
      const rdr::TargetsOp& t = f.targets[f.ops[i].index];
      if (!t.color_count || !t.color[0].image) return -1;
      ch.set_op[k] = i;
      ch.clear_op[k] = kNone;
      ch.draws[k].clear();
      ch.target[k] = t.color[0].image;
      ch.depth[k] = t.depth.image;
      ++i;
      if (i < n && f.ops[i].kind == OpKind::Clear) ch.clear_op[k] = i++;
      while (i < n && f.ops[i].kind == OpKind::Draw) {
        const DrawOrigin* o = f.OriginOf(f.ops[i].index);
        if (!o) return -1;
        const bool ok = kKind[k] == 0 ? o->pass_site == kMaskSite : kKind[k] == 1 ? o->call_site == kDownCall : (o->call_site == kBlurCall && o->pass_site != kMaskSite);
        if (!ok) return -1;
        ch.draws[k].push_back(i++);
      }
      if (ch.draws[k].empty() || (kKind[k] != 0 && ch.draws[k].size() != 1) || i >= n || f.ops[i].kind != OpKind::Copy) return -1;
      ch.copy_op[k] = i;
      const rdr::CopyOp& cp = f.copies[f.ops[i].index];
      if (cp.source.image != ch.target[k] || !cp.dest.image) return -1;
      ch.dest[k] = cp.dest.image;
      ++i;
    }
    // F9: the last blur pass starts with a full, colour-flagged clear (B's content after a skip is then the clear colour, whatever the blur drew).
    if (ch.clear_op[3] == kNone) return -1;
    const rdr::ClearOp& last = f.clears[f.ops[ch.clear_op[3]].index];
    if (!(last.flags & rdr::kClearColor) || !last.full) return -1;
    if (ch.clear_op[0] == kNone) return -1;   // S102 G4c (review F1b): the mask pass starts with a full, colour-flagged clear too
    const rdr::ClearOp& mask_clear = f.clears[f.ops[ch.clear_op[0]].index];
    if (!(mask_clear.flags & rdr::kClearColor) || !mask_clear.full) return -1;
    // F10: what follows the chain is a pass of its own (a SetTargets), a Present or the end: no stray op that belongs to the chain's last pass.
    if (i < n && f.ops[i].kind != OpKind::SetTargets && f.ops[i].kind != OpKind::Present) return -1;
    return 1;
  }

  // The rule: every mask draw has a real bank with c6.w == 0, every blur draw a real bank with c35.x == 0 (and c33.x == 1).
  static bool Blank(const LoadedFrame& f, const Chain& ch) {
    for (uint32_t op : ch.draws[0]) {
      const Draw& d = f.draws[f.ops[op].index];
      if (d.flags & kDrawThin) return false;   // a thin draw has no record of its own constants
      if (!(Reg(f, d, 0, 0) > 0.0f)) return false;   // c0.x: the fade distance (500 in every capture): a zero bank is no evidence
      if (!(Reg(f, d, 6, 3) == 0.0f)) return false;  // c6.w: the weight
    }
    for (int k = 2; k < 4; ++k) {
      const Draw& d = f.draws[f.ops[ch.draws[k][0]].index];
      if (d.flags & kDrawThin) return false;
      if (!(Reg(f, d, 33, 0) == 1.0f)) return false;  // c33.x: a real blur upload
      if (!(Reg(f, d, 35, 0) == 0.0f)) return false;  // c35.x: the weight
    }
    return true;
  }

  // F4: another plan already drops an op of the chain.
  static bool Overlaps(const LoadedFrame& f, const Chain& ch, const Ctx& ctx) {
    for (const uint8_t* other : ctx.others) {
      if (!other) continue;
      for (int k = 0; k < 4; ++k) {
        if (other[ch.set_op[k]] || other[ch.copy_op[k]]) return true;
        if (ch.clear_op[k] != kNone && other[ch.clear_op[k]]) return true;
        for (uint32_t d : ch.draws[k])
          if (other[d]) return true;
      }
    }
    (void)f;
    return false;
  }

  // Nothing outside the chain writes the chain's images, nothing but a draw reads them (only B), no CPU upload into them.
  static bool Alone(const LoadedFrame& f, const Chain& ch) {
    std::vector<uint8_t> in(f.ops.count, 0);
    for (int k = 0; k < 4; ++k) {
      in[ch.set_op[k]] = in[ch.copy_op[k]] = 1;
      if (ch.clear_op[k] != kNone) in[ch.clear_op[k]] = 1;
      for (uint32_t d : ch.draws[k]) in[d] = 1;
    }
    const uint32_t final_b = ch.dest[3];
    const auto mine = [&](uint32_t image) {
      if (!image) return false;
      for (int k = 0; k < 4; ++k)
        if (image == ch.target[k] || image == ch.dest[k] || (ch.depth[k] && image == ch.depth[k])) return true;
      return false;
    };
    for (const ContentRef& c : f.contents)
      if (mine(c.image)) return false;
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      if (in[p]) continue;
      const Op& op = f.ops[p];
      switch (op.kind) {
        case OpKind::SetTargets: {
          const rdr::TargetsOp& t = f.targets[op.index];
          for (uint32_t c = 0; c < t.color_count; ++c)
            if (mine(t.color[c].image)) return false;
          if (mine(t.depth.image)) return false;
          break;
        }
        case OpKind::Draw: {
          const Draw& d = f.draws[op.index];
          for (uint32_t k = 0; k < d.texture_count; ++k) {
            const uint32_t image = f.texture_slots[d.texture_first + k].image;
            if (mine(image) && image != final_b && g_fault != Fault::IgnoreReaders) return false;   // only B is read outside, and it is the one the ledger knows
          }
          break;
        }
        case OpKind::Copy: {
          const rdr::CopyOp& c = f.copies[op.index];
          if (mine(c.source.image) || mine(c.dest.image)) return false;
          break;
        }
        case OpKind::Present:
          if (mine(f.presents[op.index].source.image)) return false;
          break;
        default: break;
      }
    }
    return true;
  }
};

}  // namespace rdr_adapter::plane_blank
