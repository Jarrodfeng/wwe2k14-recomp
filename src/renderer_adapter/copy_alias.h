// S101 lane 190 (docs\renderer\SKIP_GAME_WORK_MAP.md section 14.6): the resolve-copy elision by alias. No game or SDK includes.
//
// The 360 resolves a render target into a texture (43-94 copies a frame). Where every reader of the texture goes through the binder's Map(), the copy can be dropped and the
// texture's id aliased to the target it would have been copied from, for the rest of the frame: the readers sample the target directly. This header decides WHICH copies
// (the elide set), from the frame's own ops and slot tables, never from guest memory. The binder (binder.h) skips the copies in the set and answers Map(dest) with the source.
//
// The rule (all must hold for a copy S -> D at op position p; the first that fails is the copy's "why"):
//   - a whole copy: both rects cover the whole of both images (mip 0, layer 0), same size, same format, 2D single mip, a colour format, same scaling; no 16-bit-target
//     conversion, no red/blue swap; S is not transient (a transient image has no sampled view);
//   - D is written by this copy only: no target switch, no other copy and no CPU content upload writes D anywhere in the frame; no op reads D before p (so the next frame, which
//     sees a D the alias never wrote, reads it only after its own copy: checked per frame, and a D that breaks it is banned for good, see Planner::Violations);
//   - D is read after p by draws only, at least one (a copy that nobody reads is candidate D, not this), reading it in its own format (no view reinterpretation); never by a
//     copy, the Present, or through a draw's image-0 slot later in the frame (that draw could read anything: nothing is judged);
//   - S is not written (target switch or copy) after p up to D's last reader, and no reader draw has S among its targets (sampled while attached);
//   - S is not itself an aliased destination of this frame (no chains);
//   - the image is none of the caller's exclusions (shadows_ours images, a copy another hook drops).
// The alias never outlives the frame (the binder clears it at the end of Bind), and is never applied to a part of a frame split at a readback, nor to an isolate or velocity bind.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "capture_reader.h"

namespace rdr_adapter::copy_alias {

// 0 off (default), 1 census (the plan is made and counted, nothing is elided), 2 elide. Live: --wwe2k14_rdr_copy_alias; fview / rplay: --copy-alias N.
inline int g_mode = 0;
// Which classes may be elided (mode 2): bit per Class. Default: the crowd bake only (the 14.4 contract); the 14.5 classes are added by this mask once proven.
// Live: --wwe2k14_rdr_copy_alias_classes; fview / rplay: --copy-alias-classes N.
enum Class : uint8_t { kBake = 0, kShadow, kRim, kPost, kScene, kOther, kClassCount };
inline constexpr uint32_t kClassBake = 1u << kBake;
inline constexpr uint32_t kClassAll = (1u << kClassCount) - 1;
inline uint32_t g_classes = kClassBake;
inline const char* ClassName(int c) {
  static const char* const k[] = {"crowd bake", "shadow", "rim", "post", "scene colour", "other"};
  return c >= 0 && c < kClassCount ? k[c] : "?";
}

// Planted faults (tools only; a player build never sets them): each is a bypass of one rule, or a wrong timing. The checks must catch each (docs 14.6).
enum class Fault : uint8_t {
  None, Empty,      // Empty: the switch is on and the set is empty (the null self-test: byte-equal)
  SrcDirty,         // the source-rewritten rule is skipped
  SubRect,          // the whole-copy rule is skipped
  Format,           // the same-format / same-size rule is skipped
  ReadFirst,        // the "no read before the copy" rule is skipped
  Late,             // the set is the previous frame's, by copy ordinal (lane 152's late-record class)
  Outlive,          // the alias is not cleared at the end of the frame
  NoDrawWrites,     // draws and clears into the source after the copy are not write events (lane 190 round 2, B1)
  Count
};
inline Fault g_fault = Fault::None;
inline int g_list_frame = -1;   // dev: list every copy of this frame with its verdict (fview --copy-alias-list F)
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "empty", "src_dirty", "sub_rect", "format", "read_first", "late", "outlive", "no_draw_writes"};
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}

// Why a copy was kept (the first rule that failed). Order = the order the rules are tried.
enum Why : uint8_t {
  kElide = 0, kWhyRef, kWhyRecord, kWhyFormat, kWhyUsage, kWhyRect, kWhyExtra, kWhyExcluded, kWhyBanned, kWhyChain,
  kWhyWriters, kWhyContent, kWhyReadBefore, kWhyReaderKind, kWhyNoReader, kWhyReaderView, kWhyBlind, kWhySrcDirty, kWhyFeedback, kWhyLasting, kWhyReadback, kWhyClassOff, kWhyCount
};
inline const char* WhyName(int w) {
  static const char* const k[] = {"elided", "refs (mip / layer / same image)", "no image record", "format / size / kind differ", "source transient", "sub-rect copy", "16-bit target or swap",
                                  "excluded image (shadows_ours / hook-dropped)", "banned (broke the rule once)", "source is an aliased destination", "dest written elsewhere", "dest has CPU content",
                                  "dest read before the copy", "dest read by a copy / the Present", "no reader (dead)", "reader views it in another format", "blind (image-0 slot later)",
                                  "source rewritten before the last reader", "reader renders into the source", "destination is a lasting surface", "destination was read back by the CPU", "class not enabled"};
  return w >= 0 && w < kWhyCount ? k[w] : "?";
}

// S2 (lane 190 round 2): a destination the game ever locked for a CPU read (reader.h OnTextureLock, the GPU-newer readback) is never elided: a later lock of an elided D would read the
// never-written image. Written by the guest thread at the lock, read by the binder's planner. Once seen, always.
inline std::mutex g_readback_mu;
inline std::unordered_set<uint32_t> g_readback_ids;
inline void NoteReadback(uint32_t image) {
  std::lock_guard lock(g_readback_mu);
  g_readback_ids.insert(image);
}
inline bool WasReadBack(uint32_t image) {
  std::lock_guard lock(g_readback_mu);
  return g_readback_ids.count(image) != 0;
}

struct Census {
  uint64_t frames = 0, frames_planned = 0, frames_skipped = 0;
  uint64_t copies[kClassCount] = {};      // copies seen (planned frames only)
  uint64_t eligible[kClassCount] = {};    // copies the rule admits (class mask ignored): what census mode reports
  uint64_t elided[kClassCount] = {};      // copies actually skipped (mode 2)
  uint64_t eligible_pixels[kClassCount] = {};
  uint64_t why[kWhyCount] = {};
  uint64_t violations = 0;                // a banned-for-good destination (see Planner::Violations)
  double ms = 0;                          // wall ms spent planning (Build, all planned frames)
  uint64_t frames_observed = 0;           // frames bound as parts of a split frame (violations checked, nothing planned)
};

struct Plan {
  std::vector<uint8_t> elide;   // by op index: 1 = the binder skips this copy and aliases its destination to its source
  uint32_t count = 0;
  void Reset(uint32_t ops) { elide.assign(ops, 0), count = 0; }
};

// What the caller knows that the frame does not.
struct Facts {
  std::function<const ImageRecord*(uint32_t)> record;   // an image id's record (null: unknown)
  std::function<bool(uint32_t)> excluded;               // an image another hook owns (shadows_ours images)
  std::function<bool(uint32_t)> op_excluded;            // an op position another hook handles (a copy post_ours drops, a half-rate shadow copy)
  std::function<uint8_t(uint32_t)> role;                // names.h SurfaceRole of an image (0 unknown)
  bool extend_to_present = false;                       // S4: a velocity pass (DLSS) redraws wrestler draws at the Present: every D is read until the frame's last Present
};

class Planner {
 public:
  Census census;

  // Builds the elide set of one frame into `plan` (plan.elide sized to the frame's ops). `mode` 1: counts only (plan stays empty); 2: fills it for the enabled classes.
  // A part of a frame split at a readback (or a persistent-only pass): nothing is planned or elided, but the "written before read" rule is still checked on it, so a D elided in frame N
  // and read by the first part of frame N+1 is counted. `first` = the first part of its guest frame (the later parts inherit what the earlier ones wrote).
  void Observe(const LoadedFrame& f, const Facts& facts, bool part_open) {
    ++census.frames_observed;
    Events(f, facts);
    Violations(split_open_);
    CarryFresh(part_open);
  }

  void Build(const LoadedFrame& f, const Facts& facts, int mode, Plan& plan) {
    const auto t0 = std::chrono::steady_clock::now();
    BuildImpl(f, facts, mode, plan);
    census.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

 private:
  bool split_open_ = false;               // the previous bind was a part with more of its guest frame to come
  std::unordered_set<uint32_t> carry_;    // images written (freshly) by the earlier parts of this guest frame

  void CarryFresh(bool part_open) {
    if (part_open) {
      for (const Ev& e : ev_)
        if (e.kind == kCopyWrite || e.kind == kClearFull || e.kind == kContent) carry_.insert(e.image);
    } else {
      carry_.clear();
    }
    split_open_ = part_open;
  }

  void BuildImpl(const LoadedFrame& f, const Facts& facts, int mode, Plan& plan) {
    plan.Reset(f.ops.count);
    ++census.frames_planned;
    Events(f, facts);
    // Rule "banned for good": a destination an earlier frame's alias left unwritten must be written before it is read in every later frame.
    Violations(split_open_);
    std::unordered_set<uint32_t> elided_dests;
    uint32_t ordinal = 0;
    std::vector<uint8_t> now;   // by copy ordinal: this frame's decisions (for the Late fault)
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      if (op.kind != OpKind::Copy) continue;
      const rdr::CopyOp& copy = f.copies[op.index];
      if (facts.op_excluded && facts.op_excluded(p)) { now.push_back(0); ++ordinal; continue; }
      const int cls = ClassOf(f, facts, copy);
      ++census.copies[cls];
      uint32_t why = Judge(f, facts, p, copy, op.index, elided_dests);
      uint8_t take = 0;
      if (why == kElide) {
        ++census.eligible[cls];
        census.eligible_pixels[cls] += uint64_t(copy.dest_rect.width) * copy.dest_rect.height;
        if (mode >= 2 && (g_classes >> cls & 1u) && g_fault != Fault::Empty) take = 1;
        else if (mode >= 2 && !(g_classes >> cls & 1u)) why = kWhyClassOff;
      }
      if (g_list_frame >= 0 && uint64_t(g_list_frame) + 1 == census.frames_planned)   // dev: one line per copy of the listed frame (fview --copy-alias-list F)
        std::printf("[copy_alias] f%d op %u copy %u -> %u class %s why '%s' rect %ux%u->%ux%u refs m%u l%u / m%u l%u extra t16 %d swap %d\n", g_list_frame, p, copy.source.image, copy.dest.image, ClassName(cls),
                    WhyName(why), copy.source_rect.width, copy.source_rect.height, copy.dest_rect.width, copy.dest_rect.height, copy.source.mip, copy.source.layer, copy.dest.mip, copy.dest.layer,
                    op.index < f.copy_extras.count ? f.copy_extras[op.index].target16 : -1, op.index < f.copy_extras.count ? f.copy_extras[op.index].swap : -1);
      ++census.why[why];
      now.push_back(take);
      if (take) {
        plan.elide[p] = 1, ++plan.count;
        elided_dests.insert(copy.dest.image);
      }
      ++ordinal;
    }
    if (g_fault == Fault::Late && mode >= 2) {   // planted: the previous frame's decisions, applied by copy ordinal
      ordinal = 0;
      plan.count = 0;
      std::fill(plan.elide.begin(), plan.elide.end(), 0);
      for (uint32_t p = 0; p < f.ops.count; ++p) {
        if (f.ops[p].kind != OpKind::Copy) continue;
        if (ordinal < late_.size() && late_[ordinal] && !(facts.op_excluded && facts.op_excluded(p))) plan.elide[p] = 1, ++plan.count;
        ++ordinal;
      }
      late_ = now;
    }
    for (uint32_t p = 0; p < f.ops.count; ++p)
      if (plan.elide[p]) {
        const rdr::CopyOp& c = f.copies[f.ops[p].index];
        ++census.elided[ClassOf(f, facts, c)];
        ever_elided_.insert(c.dest.image);
      }
    CarryFresh(false);
  }

 public:

  // Imports the destinations an earlier plan banned (a violation was seen).
  bool Banned(uint32_t image) const { return banned_.count(image) != 0; }
  const std::unordered_set<uint32_t>& ever_elided() const { return ever_elided_; }

  std::string Report() const {
    char b[512];
    std::string out;
    std::snprintf(b, sizeof b, "[rdr][copy_alias] mode %d classes 0x%X%s frames planned %llu (skipped %llu), violations %llu, observed parts %llu, plan ms %.2f (%.4f a planned frame)\n", g_mode, g_classes,
                  g_fault == Fault::None ? "" : " FAULT", static_cast<unsigned long long>(census.frames_planned), static_cast<unsigned long long>(census.frames_skipped),
                  static_cast<unsigned long long>(census.violations), static_cast<unsigned long long>(census.frames_observed), census.ms,
                  census.frames_planned ? census.ms / double(census.frames_planned) : 0.0);
    out += b;
    for (int c = 0; c < kClassCount; ++c) {
      if (!census.copies[c]) continue;
      std::snprintf(b, sizeof b, "[rdr][copy_alias]   %-12s copies %llu, eligible %llu (%.3f MPix), elided %llu\n", ClassName(c), static_cast<unsigned long long>(census.copies[c]),
                    static_cast<unsigned long long>(census.eligible[c]), double(census.eligible_pixels[c]) / 1e6, static_cast<unsigned long long>(census.elided[c]));
      out += b;
    }
    out += "[rdr][copy_alias]   kept because:";
    for (int w = 1; w < kWhyCount; ++w)
      if (census.why[w]) {
        std::snprintf(b, sizeof b, " [%s] %llu;", WhyName(w), static_cast<unsigned long long>(census.why[w]));
        out += b;
      }
    return out;
  }

 private:
  // Event kinds. Reads: kDrawRead, kCopyRead, kPresentRead. Writes: kTgtWrite (a target switch: the image is a target from here, NOT a refresh), kDrawWrite (a draw into a bound target), kClearPart,
  // kCopyPart (a copy that does not cover the whole image / mip / layer 0); and the FRESH writes that give an image all its pixels: kCopyWrite (a whole copy), kClearFull, kContent (a CPU upload).
  enum Kind : uint8_t { kDrawRead = 0, kCopyRead, kPresentRead, kTgtWrite, kCopyWrite, kContent, kDrawWrite, kClearFull, kClearPart, kCopyPart };
  static bool IsWrite(uint8_t k) { return k >= kTgtWrite; }
  struct Ev {
    uint32_t image, pos;
    uint8_t kind, view;   // view: the slot's view format (0 = the image's own)
    bool operator<(const Ev& o) const { return image != o.image ? image < o.image : pos != o.pos ? pos < o.pos : kind < o.kind; }
  };
  std::vector<Ev> ev_;
  std::vector<uint32_t> tgt_of_;               // by op position: the index of the frame's targets entry in force (0xFFFFFFFF none)
  std::vector<uint32_t> last_blind_;           // size 1: the position of the last draw with an image-0 slot (0xFFFFFFFF none)
  std::unordered_map<uint32_t, uint32_t> site_of_, call_of_;   // image -> pass_site / call_site of the last named draw into it
  std::unordered_set<uint32_t> banned_, ever_elided_;
  std::vector<uint8_t> late_;
  uint32_t last_present_ = 0;   // op position of the frame's last Present (0 none)

  std::pair<const Ev*, const Ev*> Range(uint32_t image) const {
    const Ev key{image, 0, 0, 0};
    const auto lo = std::lower_bound(ev_.begin(), ev_.end(), key);
    auto hi = lo;
    while (hi != ev_.end() && hi->image == image) ++hi;
    return {ev_.data() + (lo - ev_.begin()), ev_.data() + (hi - ev_.begin())};
  }

  void Events(const LoadedFrame& f, const Facts& facts) {
    ev_.clear();
    last_present_ = 0;
    site_of_.clear(), call_of_.clear();
    tgt_of_.assign(f.ops.count, 0xFFFFFFFFu);
    last_blind_.assign(1, 0xFFFFFFFFu);
    for (const ContentRef& ref : f.contents) ev_.push_back({ref.image, 0, kContent, 0});
    uint32_t cur = 0xFFFFFFFFu;
    bool wrote = false;   // a draw event was pushed for the targets in force since the last target switch / copy (one per run is enough: the rule asks "written after p")
    const auto push_target_writes = [&](uint32_t p, uint8_t kind, bool colour, bool depth) {
      if (cur == 0xFFFFFFFFu) return;
      const rdr::TargetsOp& t = f.targets[cur];
      if (colour)
        for (uint32_t c = 0; c < t.color_count; ++c)
          if (t.color[c].image) ev_.push_back({t.color[c].image, p, kind, 0});
      if (depth && t.depth.image) ev_.push_back({t.depth.image, p, kind, 0});
    };
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      switch (op.kind) {
        case OpKind::Clear: {   // B1: a clear of the targets in force writes them without a target switch (C25 resolve-with-clear: reader.h pushes it on the SOURCE right after the copy)
          if (g_fault == Fault::NoDrawWrites) break;
          const rdr::ClearOp& c = f.clears[op.index];
          push_target_writes(p, c.full ? kClearFull : kClearPart, (c.flags & rdr::kClearColor) != 0, (c.flags & (rdr::kClearDepth | rdr::kClearStencil)) != 0);
          break;
        }
        case OpKind::SetTargets: {
          wrote = false;
          cur = op.index;
          const rdr::TargetsOp& t = f.targets[op.index];
          for (uint32_t c = 0; c < t.color_count; ++c)
            if (t.color[c].image) ev_.push_back({t.color[c].image, p, kTgtWrite, 0});
          if (t.depth.image) ev_.push_back({t.depth.image, p, kTgtWrite, 0});
          break;
        }
        case OpKind::Draw: {
          tgt_of_[p] = cur;
          if (!wrote && g_fault != Fault::NoDrawWrites) {   // B1: the reader drops a repeated SetTargets and the renderer reopens a pass on the same targets, so draws after the copy still write the source
            push_target_writes(p, kDrawWrite, true, true);
            wrote = true;
          }
          const Draw& d = f.draws[op.index];
          for (uint32_t s = 0; s < d.texture_count; ++s) {
            const TextureSlot& slot = f.texture_slots[d.texture_first + s];
            if (!slot.image) last_blind_[0] = p;
            else ev_.push_back({slot.image, p, kDrawRead, slot.view_format});
          }
          if (cur != 0xFFFFFFFFu) {
            const rdr::TargetsOp& t = f.targets[cur];
            const uint32_t img = t.color_count ? t.color[0].image : t.depth.image;
            if (const DrawOrigin* o = f.OriginOf(op.index)) {
              if (img && o->pass_site) site_of_[img] = o->pass_site;
              if (img && o->call_site) call_of_[img] = o->call_site;
            }
          }
          break;
        }
        case OpKind::Copy: {
          const rdr::CopyOp& c = f.copies[op.index];
          wrote = false;   // draws after the copy are new writes
          ev_.push_back({c.source.image, p, kCopyRead, 0});
          // S3: only a WHOLE copy (mip 0, layer 0, covering a single-mip single-layer image) gives the destination all its pixels; any other copy bans the destination for good
          const ImageRecord* rd = facts.record ? facts.record(c.dest.image) : nullptr;
          const bool whole = rd && !c.dest.mip && !c.dest.layer && rd->mips == 1 && rd->layers == 1 && Full(c.dest_rect, rd->width, rd->height);
          ev_.push_back({c.dest.image, p, whole ? kCopyWrite : kCopyPart, 0});
          if (!whole) banned_.insert(c.dest.image);
          break;
        }
        case OpKind::Present: {
          ev_.push_back({f.presents[op.index].source.image, p, kPresentRead, 0});
          last_present_ = p;
          break;
        }
        default: break;
      }
    }
    std::sort(ev_.begin(), ev_.end());
  }

  // A destination an earlier frame's alias left unwritten must, in every later frame, be written (a copy, a target switch, an upload) before anything reads it. One that is not
  // (the structure changed under us) is banned for good, counted, and the check reports it.
  void Violations(bool inherit) {
    for (const uint32_t image : ever_elided_) {
      if (banned_.count(image)) continue;
      const auto [lo, hi] = Range(image);
      bool fresh = inherit && carry_.count(image) != 0, bad = false;   // (a later part of a split frame inherits what its earlier parts wrote)
      for (const Ev* e = lo; e != hi && !bad; ++e) {
        if (e->kind == kCopyWrite || e->kind == kClearFull || e->kind == kContent) fresh = true;   // S3: a target switch alone is not a refresh
        else if (!fresh && !IsWrite(e->kind)) bad = true;   // a read before the image was freshly written
      }
      if (bad) banned_.insert(image), ++census.violations;
    }
  }

  int ClassOf(const LoadedFrame& f, const Facts& facts, const rdr::CopyOp& c) const {
    (void)f;
    const auto site = site_of_.find(c.source.image);
    const uint32_t s = site == site_of_.end() ? 0 : site->second;
    const uint8_t role = facts.role ? facts.role(c.source.image) : 0;
    if (s == 0x82C01F60u) return kBake;
    if (s == 0x82C04634u || s == 0x82BF4D38u || role == uint8_t(SurfaceRole::ShadowMap)) return kShadow;
    if (s == 0x82BFBD58u || role == uint8_t(SurfaceRole::RimMap)) return kRim;
    if (role == uint8_t(SurfaceRole::PostStep)) return kPost;
    if (role == uint8_t(SurfaceRole::SceneColor)) return kScene;
    return kOther;
  }

  // The planted faults: the rules (and the ones that would catch the same copy next) a fault skips, so that the copies it admits reach the picture.
  static bool Bypass(uint32_t why) {
    switch (g_fault) {
      case Fault::NoDrawWrites: return false;
      case Fault::SrcDirty: return why == kWhySrcDirty || why == kWhyFeedback || why == kWhyWriters;
      case Fault::SubRect: return why == kWhyRect || why == kWhyExtra;
      case Fault::Format: return why == kWhyFormat || why == kWhyRect || why == kWhyExtra || why == kWhyUsage;
      case Fault::ReadFirst: return why == kWhyReadBefore || why == kWhyLasting;   // (a history texture is a lasting surface: S1 refuses it first)
      default: return false;
    }
  }
  static bool Full(const rdr::Rect& r, uint32_t w, uint32_t h) { return r.x == 0 && r.y == 0 && r.width >= w && r.height >= h; }

  uint32_t Judge(const LoadedFrame& f, const Facts& facts, uint32_t p, const rdr::CopyOp& copy, uint32_t copy_index, const std::unordered_set<uint32_t>& elided_dests) const {
    const uint32_t S = copy.source.image, D = copy.dest.image;
    if (!S || !D || S == D || copy.source.mip || copy.source.layer || copy.dest.mip || copy.dest.layer) return kWhyRef;
    const ImageRecord* rs = facts.record ? facts.record(S) : nullptr;
    const ImageRecord* rd = facts.record ? facts.record(D) : nullptr;
    if (!rs || !rd) return kWhyRecord;
    {
      if (rs->width != rd->width || rs->height != rd->height || rs->format != rd->format || rs->kind != rd->kind || rs->kind != uint8_t(rdr::ImageKind::Image2D) || rs->mips != 1 || rd->mips != 1 ||
          rs->layers != 1 || rd->layers != 1 || rs->depth != 1 || rd->depth != 1 || ((rs->usage ^ rd->usage) & rdr::kUsageScaled))
        if (!Bypass(kWhyFormat)) return kWhyFormat;
    }
    if (rs->format == uint8_t(rdr::Format::Depth) || rs->format == uint8_t(rdr::Format::Undefined)) return kWhyFormat;
    if ((rs->usage & rdr::kUsageTransient) && !Bypass(kWhyUsage)) return kWhyUsage;
    if ((!Full(copy.source_rect, rs->width, rs->height) || !Full(copy.dest_rect, rd->width, rd->height)) && !Bypass(kWhyRect)) return kWhyRect;
    if (copy_index < f.copy_extras.count) {
      const CopyExtra& e = f.copy_extras[copy_index];
      if (e.target16 && !Bypass(kWhyExtra)) return kWhyExtra;   // (the red/blue swap is folded into the reading slots' swizzles; the copy itself moves the texels as they are)
    }
    if (facts.excluded && (facts.excluded(S) || facts.excluded(D))) return kWhyExcluded;
    if (banned_.count(D)) return kWhyBanned;
    if ((rd->usage & rdr::kUsageLasting) && !Bypass(kWhyLasting)) return kWhyLasting;   // S1: a later frame reads a lasting surface (a skipped bake's cards read the last bake's destination)
    if (WasReadBack(D)) return kWhyReadback;   // S2
    if (elided_dests.count(S)) return kWhyChain;
    // D's events.
    uint32_t last_reader = 0, readers = 0;
    {
      const auto [lo, hi] = Range(D);
      for (const Ev* e = lo; e != hi; ++e) {
        if (e->kind == kContent) return kWhyContent;
        if (e->kind == kCopyWrite) { if (e->pos != p && !Bypass(kWhyWriters)) return kWhyWriters; continue; }
        if (IsWrite(e->kind)) { if (!Bypass(kWhyWriters)) return kWhyWriters; continue; }
        if (e->pos < p) { if (!Bypass(kWhyReadBefore)) return kWhyReadBefore; continue; }
        if (e->pos == p) continue;   // (the copy's own read of D cannot happen: S != D)
        if (e->kind != kDrawRead) return kWhyReaderKind;
        if (e->view && e->view != rd->format) return kWhyReaderView;
        ++readers, last_reader = std::max(last_reader, e->pos);
      }
    }
    if (!readers) return kWhyNoReader;
    if (facts.extend_to_present && last_present_ > last_reader) last_reader = last_present_;   // S4: the velocity pass redraws at the Present
    if (last_blind_[0] != 0xFFFFFFFFu && last_blind_[0] > p) return kWhyBlind;
    // S after the copy.
    {
      const auto [lo, hi] = Range(S);
      for (const Ev* e = lo; e != hi; ++e)
        if (IsWrite(e->kind) && e->pos > p && e->pos <= last_reader && !Bypass(kWhySrcDirty)) return kWhySrcDirty;
    }
    // A reader that renders into S (the pass was opened before the copy and stays open): sampled while attached.
    {
      const auto [lo, hi] = Range(D);
      for (const Ev* e = lo; e != hi; ++e) {
        if (e->kind != kDrawRead || e->pos <= p) continue;
        const uint32_t t = tgt_of_[e->pos];
        if (t == 0xFFFFFFFFu) continue;
        const rdr::TargetsOp& to = f.targets[t];
        for (uint32_t c = 0; c < to.color_count; ++c)
          if (to.color[c].image == S && !Bypass(kWhyFeedback)) return kWhyFeedback;
        if (to.depth.image == S && !Bypass(kWhyFeedback)) return kWhyFeedback;
      }
    }
    return kElide;
  }
};

}  // namespace rdr_adapter::copy_alias
