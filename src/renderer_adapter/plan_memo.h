// S102 R23-P (docs\renderer\PLAN_MEMO.md): the frame planners' input digests and their census. No game or SDK includes.
//
// The render thread runs about ten planners a drawn frame (PlanPost, DeadWorkBegin, AliasBegin, ShadowsOursBegin, PreLightComputeBegin, RtSpanPlan, ...), each a scan over the frame's tables.
// The census asks, per planner: how often are its INPUTS exactly the previous drawn frame's (an exact digest of the tables / fields it reads), how often is its OUTPUT the previous frame's,
// and which input digest is the one that changes. The digests below are exact: raw bytes of a table (padding included: frames come from files / one writer, so the bytes are deterministic)
// or every field a planner reads of a struct whose other fields change by the frame (a draw's geometry offsets, a bank index).
//   TOOL ONLY while g_census_on is false (rplay / fview --plan-memo-census): the game never computes a digest unless the memo switch is on.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "capture_reader.h"

namespace rdr_adapter::plan_memo {

inline std::atomic<bool> g_census_on{false};
inline std::atomic<bool> g_time_on{false};   // per-planner wall timers (a relaxed load a planner while off); set by rplay / fview --plan-memo-census / --plan-memo-times

// The planners' own time on the render thread (the bind runs on one thread; the sums are plain integers).
enum Tm : int { kTmPost, kTmThinBegin, kTmDead, kTmTurns, kTmOwnGroup, kTmPlaneBlank, kTmPreLightA, kTmPreLightC, kTmPreLightL, kTmShadows, kTmPlaneBake, kTmAlias, kTmFrameCutsBegin, kTmFrameCutsOp, kTmRtSpan, kTmPostRead, kTmPostBuild, kTmAliasBuild, kTmShadowScan, kTmPropsMatch, kTmCount };
inline const char* const kTmNames[kTmCount] = {"PlanPost", "ThinBeginFrame", "DeadWorkBegin", "TurnsBegin", "OwnGroupPlan", "PlaneBlankBegin", "PreLightAnalyticBegin", "PreLightComputeBegin", "PreLightBegin",
                                               "ShadowsOursBegin", "PlaneBakePlan", "AliasBegin", "FrameCutsBegin", "FrameCutsOp (all ops)", "RtSpanPlan", "  post: ReadStreamChain", "  post: BuildPostPlan", "  alias: copy_alias Build", "  shadows: Scan", "PropsCore::InMatch"};
inline uint64_t g_tm_ns[kTmCount] = {};
inline uint64_t g_tm_calls[kTmCount] = {};
struct Timer {
  int id;
  std::chrono::steady_clock::time_point t0;
  bool on;
  explicit Timer(int i) : id(i), on(g_time_on.load(std::memory_order_relaxed)) {
    if (on) t0 = std::chrono::steady_clock::now();
  }
  ~Timer() {
    if (on) g_tm_ns[id] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), ++g_tm_calls[id];
  }
};
inline void ReportTimes(const char* scene, uint64_t frames) {
  const double n = frames ? double(frames) : 1.0;
  double total = 0;
  for (int i = 0; i < kTmCount; ++i) {
    if (!g_tm_calls[i]) continue;
    std::printf("[plan_memo][time] %s %-24s %.3f ms a frame (%llu calls)\n", scene, kTmNames[i], double(g_tm_ns[i]) / 1e6 / n, static_cast<unsigned long long>(g_tm_calls[i]));
    total += double(g_tm_ns[i]) / 1e6 / n;
  }
  std::printf("[plan_memo][time] %s all planners: %.3f ms a frame over %llu frames\n", scene, total, static_cast<unsigned long long>(frames));
}

// The digests of one frame. Each names one table or one projection of a table.
enum Dg : int {
  kDgOps,        // the op sequence (kind, index)
  kDgTargets,    // targets + clears + presents tables (raw)
  kDgCopies,     // copies + copy extras (raw)
  kDgDrawShape,  // per draw: vs_hash, ps_hash, vs_patched_hash, fixed, dynamic, texture_first, texture_count, layout, flags (not the geometry offsets, counts or banks)
  kDgDrawGeom,   // per draw: vertex_offset, index_offset, vertex_bytes, index_bytes, vertex_count, index_count (what changes with the stream's geometry)
  kDgBankIdx,    // per draw: vs_bank, ps_bank, bool_loop (indices, not contents)
  kDgSlots,      // texture_slots (raw)
  kDgSamplers,   // samplers (raw)
  kDgFixeds,     // fixeds (raw)
  kDgDynamics,   // dynamics (raw)
  kDgLayouts,    // layouts + elements (raw)
  kDgOrgSites,   // origins: klass, op, flags, call_site, pass_site, part (not object, not the string offsets)
  kDgOrgObject,  // origins: object (a guest address)
  kDgOrgStrings, // origins: technique, asset, widget (string offsets) + the strings table size
  kDgContents,   // the image contents table (image ids and sizes)
  kDgPostPs,     // the pixel banks (contents) of the draws whose origin class is Post
  kDgAllPs,      // every draw's pixel bank contents
  kDgAllVs,      // every draw's vertex bank contents
  kDgRecords,    // the image records the planners ask about (id, size, format, usage, role): set by the caller
  kDgCount
};
inline const char* const kDgNames[kDgCount] = {"ops", "targets", "copies", "draw_shape", "draw_geom", "bank_idx", "slots", "samplers", "fixeds", "dynamics", "layouts", "org_sites", "org_object", "org_strings",
                                               "contents", "post_ps", "all_ps", "all_vs", "records"};

struct Digests {
  uint64_t v[kDgCount] = {};
  uint64_t passes[2048] = {};   // per pass (SetTargets-delimited span): a digest of its ops, targets, draw shapes, slots and origin sites
  uint32_t pass_count = 0;
  uint32_t pass_draws[2048] = {};
};

// A 4-lane multiply-xor hash over raw bytes (no xxhash dependency: the player and the game link this header alone). ~1 cycle / 8 bytes a lane.
inline uint64_t Hash64(const void* data, size_t bytes) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t a = 0x9e3779b97f4a7c15ull, b = 0xc2b2ae3d27d4eb4full, c = 0x165667b19e3779f9ull, d = 0x27d4eb2f165667c5ull;
  size_t n = bytes;
  while (n >= 32) {
    uint64_t w[4];
    std::memcpy(w, p, 32);
    a = (a ^ w[0]) * 0xff51afd7ed558ccdull, b = (b ^ w[1]) * 0xc4ceb9fe1a85ec53ull, c = (c ^ w[2]) * 0xff51afd7ed558ccdull, d = (d ^ w[3]) * 0xc4ceb9fe1a85ec53ull;
    a ^= a >> 32, b ^= b >> 32, c ^= c >> 32, d ^= d >> 32;
    p += 32, n -= 32;
  }
  uint64_t tail = 0;
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    tail = (tail ^ w) * 0xff51afd7ed558ccdull, tail ^= tail >> 32;
    p += 8, n -= 8;
  }
  uint64_t last = 0;
  if (n) std::memcpy(&last, p, n);
  uint64_t h = a ^ (b * 3) ^ (c * 5) ^ (d * 7) ^ tail ^ last * 0x9e3779b97f4a7c15ull ^ bytes;
  h ^= h >> 33, h *= 0xff51afd7ed558ccdull, h ^= h >> 33;
  return h;
}

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h = (h ^ v) * 0x100000001b3ull;
  return h ^ (h >> 29);
}
template <class T>
inline uint64_t Raw(const Table<T>& t) { return t.count ? Hash64(t.data, sizeof(T) * t.count) ^ (uint64_t(t.count) << 40) : 0x1234; }

inline void Compute(const LoadedFrame& f, Digests& d) {
  for (uint64_t& x : d.v) x = 0;
  d.pass_count = 0;
  d.v[kDgTargets] = Mix(Mix(Raw(f.targets), Raw(f.clears)), Raw(f.presents));
  d.v[kDgCopies] = Mix(Raw(f.copies), Raw(f.copy_extras));
  d.v[kDgSlots] = Raw(f.texture_slots);
  d.v[kDgSamplers] = Raw(f.samplers);
  d.v[kDgFixeds] = Raw(f.fixeds);
  d.v[kDgDynamics] = Raw(f.dynamics);
  d.v[kDgLayouts] = Mix(Raw(f.layouts), Raw(f.elements));
  d.v[kDgContents] = Raw(f.contents);
  uint64_t ops = 0xabc, shape = 0xdef, geom = 1, bidx = 2, osite = 3, oobj = 4, ostr = 5, postps = 6, allps = 7, allvs = 8;
  for (uint32_t k = 0; k < f.ops.count; ++k) ops = Mix(ops, uint64_t(f.ops[k].kind) << 32 | f.ops[k].index);
  for (uint32_t k = 0; k < f.draws.count; ++k) {
    const Draw& dr = f.draws[k];
    shape = Mix(Mix(Mix(shape, dr.vs_hash), dr.ps_hash), dr.vs_patched_hash);
    shape = Mix(Mix(Mix(shape, uint64_t(dr.fixed) << 32 | dr.dynamic), uint64_t(dr.texture_first) << 32 | dr.texture_count), uint64_t(dr.layout) << 8 | dr.flags);
    geom = Mix(Mix(Mix(geom, dr.vertex_offset), dr.index_offset), uint64_t(dr.vertex_bytes) << 32 | dr.index_bytes);
    geom = Mix(geom, uint64_t(dr.vertex_count) << 32 | dr.index_count);
    bidx = Mix(Mix(bidx, uint64_t(dr.vs_bank) << 32 | dr.ps_bank), dr.bool_loop);
    if (dr.ps_bank < f.constant_banks.count) {
      const uint64_t h = Hash64(f.constant_banks[dr.ps_bank].dwords, sizeof(ConstantBank));
      allps = Mix(allps, h);
      const DrawOrigin* o = f.OriginOf(k);
      if (o && o->klass == DrawClass::Post) postps = Mix(postps, h);
    }
    if (dr.vs_bank < f.constant_banks.count) allvs = Mix(allvs, Hash64(f.constant_banks[dr.vs_bank].dwords, sizeof(ConstantBank)));
  }
  for (uint32_t k = 0; k < f.origins.count; ++k) {
    const DrawOrigin& o = f.origins[k];
    osite = Mix(Mix(osite, uint64_t(uint8_t(o.klass)) << 24 | uint64_t(o.op) << 16 | o.flags), uint64_t(o.call_site) << 32 | o.pass_site);
    osite = Mix(osite, o.part);
    oobj = Mix(oobj, o.object);
    ostr = Mix(Mix(Mix(ostr, o.technique), o.asset), o.widget);
  }
  ostr = Mix(ostr, f.strings.count);
  d.v[kDgOps] = ops, d.v[kDgDrawShape] = shape, d.v[kDgDrawGeom] = geom, d.v[kDgBankIdx] = bidx, d.v[kDgOrgSites] = osite, d.v[kDgOrgObject] = oobj, d.v[kDgOrgStrings] = ostr;
  d.v[kDgPostPs] = postps, d.v[kDgAllPs] = allps, d.v[kDgAllVs] = allvs;
  // the passes
  uint64_t ph = 0;
  uint32_t pd = 0;
  bool open = false;
  const auto close = [&] {
    if (open && d.pass_count < 2048) d.passes[d.pass_count] = ph, d.pass_draws[d.pass_count] = pd, ++d.pass_count;
    open = false;
  };
  for (uint32_t k = 0; k < f.ops.count; ++k) {
    const Op& op = f.ops[k];
    if (op.kind == OpKind::SetTargets) {
      close();
      open = true, ph = 0x9e37, pd = 0;
      const rdr::TargetsOp& t = f.targets[op.index];
      for (uint32_t c = 0; c < t.color_count; ++c) ph = Mix(ph, t.color[c].image);
      ph = Mix(ph, t.depth.image);
    } else if (op.kind == OpKind::Draw) {
      const Draw& dr = f.draws[op.index];
      ph = Mix(Mix(ph, dr.vs_hash), dr.ps_hash);
      for (uint32_t s = 0; s < dr.texture_count; ++s) ph = Mix(ph, f.texture_slots[dr.texture_first + s].image);
      if (const DrawOrigin* o = f.OriginOf(op.index)) ph = Mix(ph, uint64_t(o->call_site) << 32 | o->pass_site);
      ++pd;
    } else if (op.kind == OpKind::Copy) {
      const rdr::CopyOp& c = f.copies[op.index];
      ph = Mix(Mix(ph, uint64_t(c.source.image) << 32 | c.dest.image), uint64_t(c.dest_rect.width) << 32 | c.dest_rect.height);
    } else if (op.kind == OpKind::Clear) {
      ph = Mix(ph, 0xc1ea);
    } else {
      ph = Mix(ph, 0x9e5e);
    }
  }
  close();
}

// The planners, with the digests each reads (read from the planners' source, see PLAN_MEMO.md section 2) and the digests of an output.
enum Planner : int { kPlPost, kPlAlias, kPlDead, kPlShadows, kPlPreLightC, kPlRtSpan, kPlCount };
inline const char* const kPlNames[kPlCount] = {"post (PlanPost / DecidePost)", "alias (AliasBegin / Planner::Build)", "dead_work (DeadWorkBegin)", "shadows (ShadowsOursBegin / Scan)", "prelight_c (PreLightComputeBegin)", "rt_span (RtSpanPlan)"};
inline uint32_t ReadMask(int p) {
  const auto b = [](int g) { return 1u << g; };
  switch (p) {
    case kPlPost: return b(kDgOps) | b(kDgTargets) | b(kDgCopies) | b(kDgDrawShape) | b(kDgSlots) | b(kDgSamplers) | b(kDgFixeds) | b(kDgDynamics) | b(kDgOrgSites) | b(kDgPostPs) | b(kDgRecords);
    case kPlAlias: return b(kDgOps) | b(kDgTargets) | b(kDgCopies) | b(kDgDrawShape) | b(kDgSlots) | b(kDgOrgSites) | b(kDgContents) | b(kDgRecords);
    case kPlDead: return b(kDgOps) | b(kDgTargets) | b(kDgCopies) | b(kDgDrawShape) | b(kDgSlots) | b(kDgOrgSites) | b(kDgContents) | b(kDgRecords);
    case kPlShadows: return b(kDgOps) | b(kDgTargets) | b(kDgCopies) | b(kDgDrawShape) | b(kDgSlots) | b(kDgOrgSites) | b(kDgRecords);
    case kPlPreLightC: return b(kDgOps) | b(kDgTargets) | b(kDgCopies) | b(kDgDrawShape) | b(kDgSlots) | b(kDgFixeds) | b(kDgDynamics) | b(kDgOrgSites) | b(kDgContents) | b(kDgAllVs) | b(kDgAllPs) | b(kDgRecords);
    case kPlRtSpan: return b(kDgOps) | b(kDgTargets) | b(kDgCopies) | b(kDgDrawShape) | b(kDgSlots) | b(kDgFixeds) | b(kDgDynamics) | b(kDgLayouts) | b(kDgOrgSites) | b(kDgOrgObject) | b(kDgContents) | b(kDgAllVs) | b(kDgAllPs) | b(kDgRecords);
  }
  return 0;
}

struct Census {
  uint64_t frames = 0;
  uint64_t dg_same[kDgCount] = {};
  uint64_t dg_same_pair[kDgCount] = {};   // same as 2 frames ago (the 30 fps / half-rate parity)
  uint64_t in_same[kPlCount] = {}, out_same[kPlCount] = {}, in_same_out_same[kPlCount] = {}, in_same_out_diff[kPlCount] = {}, out_same_in_diff[kPlCount] = {};
  uint64_t in_same_last4[kPlCount] = {}, in_same_pair[kPlCount] = {};
  uint64_t in_diff_blame[kPlCount][kDgCount] = {};    // when the input digest changed: how often each read digest was the one (or among those) that changed
  uint64_t lost_blame[kPlCount][kDgCount] = {};       // the same, only in frames whose OUTPUT was equal anyway
  uint64_t out_empty[kPlCount] = {};                  // the planner's output was "nothing planned" (an all-zero plan)
  uint64_t passes = 0, passes_same_prev = 0, draws = 0, draws_in_same_pass = 0, pass_draws_same = 0;
  uint64_t scratch_ns[kPlCount] = {};
  Digests prev, prev2;
  uint64_t key_prev[kPlCount] = {}, out_prev[kPlCount] = {}, key_hist[kPlCount][4] = {}, key_prev2[kPlCount] = {};
  uint32_t have = 0;
  uint64_t key_have[kPlCount] = {};

  uint64_t Key(int p, const Digests& d) const {
    const uint32_t m = ReadMask(p);
    uint64_t h = 77 + uint64_t(p);
    for (int g = 0; g < kDgCount; ++g)
      if (m & (1u << g)) h = Mix(h, d.v[g]);
    return h;
  }

  // `cur` carries the frame's digests (kDgRecords set) and `out[p]` each planner's output digest (0 = not run this frame: the planner's row is skipped); `empty[p]` = the output was empty.
  void Note(const Digests& cur, const uint64_t out[kPlCount], const bool ran[kPlCount], const bool empty[kPlCount]) {
    ++frames;
    if (have) {
      for (int g = 0; g < kDgCount; ++g) dg_same[g] += cur.v[g] == prev.v[g];
      if (have >= 2)
        for (int g = 0; g < kDgCount; ++g) dg_same_pair[g] += cur.v[g] == prev2.v[g];
      // passes: how many of this frame's passes equal a pass of the previous frame (as a multiset)
      std::unordered_map<uint64_t, uint32_t> pool;
      for (uint32_t i = 0; i < prev.pass_count; ++i) ++pool[prev.passes[i]];
      for (uint32_t i = 0; i < cur.pass_count; ++i) {
        ++passes, draws += cur.pass_draws[i];
        auto it = pool.find(cur.passes[i]);
        if (it != pool.end() && it->second) {
          --it->second;
          ++passes_same_prev, pass_draws_same += cur.pass_draws[i];
        }
      }
    }
    for (int p = 0; p < kPlCount; ++p) {
      if (!ran[p]) continue;
      const uint64_t key = Key(p, cur);
      if (key_have[p]) {
        const bool kin = key == key_prev[p], kout = out[p] == out_prev[p];
        in_same[p] += kin, out_same[p] += kout;
        in_same_out_same[p] += kin && kout, in_same_out_diff[p] += kin && !kout, out_same_in_diff[p] += !kin && kout;
        bool in4 = false;
        for (uint64_t i = 0; i < key_have[p] && i < 4; ++i) in4 = in4 || key == key_hist[p][i];
        in_same_last4[p] += in4;
        if (key_have[p] >= 2) in_same_pair[p] += key == key_prev2[p];
        if (!kin) {
          const uint32_t m = ReadMask(p);
          for (int g = 0; g < kDgCount; ++g)
            if ((m & (1u << g)) && cur.v[g] != prev.v[g]) {
              ++in_diff_blame[p][g];
              if (kout) ++lost_blame[p][g];
            }
        }
        out_empty[p] += empty[p];
      }
      key_prev2[p] = key_prev[p], key_prev[p] = key, out_prev[p] = out[p];
      for (int i = 3; i > 0; --i) key_hist[p][i] = key_hist[p][i - 1];
      key_hist[p][0] = key;
      ++key_have[p];
    }
    prev2 = prev;
    prev = cur;
    ++have;
  }

  void Report(const char* scene) const {
    const double n = frames > 1 ? double(frames - 1) : 1.0;
    if (g_time_on.load()) ReportTimes(scene, frames);
    std::printf("[plan_memo][census] %s: %llu drawn frames (the rows below compare each frame with the previous one)\n", scene, static_cast<unsigned long long>(frames));
    for (int g = 0; g < kDgCount; ++g)
      std::printf("[plan_memo][census] %s digest %-12s same as prev %.1f %%, as 2 frames ago %.1f %%\n", scene, kDgNames[g], 100.0 * double(dg_same[g]) / n, 100.0 * double(dg_same_pair[g]) / n);
    std::printf("[plan_memo][census] %s passes: %.1f passes a frame, %.1f %% equal a pass of the previous frame (%.1f %% of the draws sit in such passes)\n", scene, double(passes) / n, passes ? 100.0 * double(passes_same_prev) / double(passes) : 0.0,
                draws ? 100.0 * double(pass_draws_same) / double(draws) : 0.0);
    for (int p = 0; p < kPlCount; ++p) {
      const double m = key_have[p] > 1 ? double(key_have[p] - 1) : 1.0;
      std::printf("[plan_memo][census] %s %s: ran %llu frames: INPUT = prev %.1f %%, = one of last 4 %.1f %%, = 2 ago %.1f %%; OUTPUT = prev %.1f %%; input same + output differs %llu (a missed input); output same + input differs %.1f %%; empty output %.1f %%\n", scene, kPlNames[p],
                  static_cast<unsigned long long>(key_have[p]), 100.0 * double(in_same[p]) / m, 100.0 * double(in_same_last4[p]) / m, 100.0 * double(in_same_pair[p]) / m, 100.0 * double(out_same[p]) / m,
                  static_cast<unsigned long long>(in_same_out_diff[p]), 100.0 * double(out_same_in_diff[p]) / m, 100.0 * double(out_empty[p]) / m);
      std::string blame, lost;
      char b[96];
      for (int g = 0; g < kDgCount; ++g) {
        if (in_diff_blame[p][g]) std::snprintf(b, sizeof b, " %s %.0f %%", kDgNames[g], 100.0 * double(in_diff_blame[p][g]) / m), blame += b;
        if (lost_blame[p][g]) std::snprintf(b, sizeof b, " %s %.0f %%", kDgNames[g], 100.0 * double(lost_blame[p][g]) / m), lost += b;
      }
      std::printf("[plan_memo][census] %s %s: input digests that changed (share of frames):%s\n", scene, kPlNames[p], blame.empty() ? " none" : blame.c_str());
      std::printf("[plan_memo][census] %s %s: ... in frames whose output stayed equal anyway:%s\n", scene, kPlNames[p], lost.empty() ? " none" : lost.c_str());
    }
  }
};

inline Census g_census;

// ---- the switch (wwe2k14_rdr_plan_memo, PerFrame, default 0): bits for the work the planners did every frame whose answer does not change ----------------------------------------------------------
// The census above found no planner whose WHOLE input repeats (0 % exact on every one, Bryan and Brock): a plan memo keyed by the frame would never hit. What does not change is smaller: the answer to "is this
// shader a post-chain shader" (per hash), the image a pass writes (per pass, not per draw), the order events are generated in, how soon a record shows a match. Each bit removes that work; the result is the same bytes (verify mode: every Nth frame runs the old code beside the new and compares).
enum : uint32_t {
  kPostChain = 1,     // ReadStreamChain: the pixel shader first (one registry lookup, a direct-mapped cache), the vertex shader only for a chain pixel shader; one "written by another draw" mark a pass, not a draw; the vectors kept
  kAliasEvents = 2,   // copy_alias Events: the event list ordered by image with a counting sort (generation order already follows the op position); one site / call write a pass, not a draw
  kPropsMatch = 4,    // props: "in a match" decided at the first crowd member / second actor of the record's objects, not by counting every object
  kAll = 7
};
inline std::atomic<uint32_t> g_mask{0};        // the mask this frame's bind reads (written once a bind, by the bind's thread)
inline std::atomic<uint32_t> g_fallback{0};    // the value before the switch table is live (rplay / fview --plan-memo N); the game's table overrides it
inline std::atomic<uint32_t> g_verify{0};      // fview / rplay --plan-memo-verify N: every Nth use runs the old code beside the new and compares (0 = off). TOOL ONLY
inline std::atomic<int> g_plant{0};            // test only (--plan-memo-plant N): 1 post: the pass mark is skipped for another target too; 2 post: the pixel cache is keyed by the low 8 bits of the hash; 3 alias: a sort that ignores the position (each image's events reversed);
                                               // 4 alias: the site mark is skipped while the image is unchanged (not the pair); 5 props: the early exit answers the opposite
inline bool Has(uint32_t bit) { return (g_mask.load(std::memory_order_relaxed) & bit) != 0; }
inline std::atomic<uint32_t> g_max_image{0};   // the highest image id a counting sort cleared counters for (the census line prints it)
inline bool Plant(int n) { return g_plant.load(std::memory_order_relaxed) == n; }

// Census counters: relaxed adds in fixed slots (no names, no strings, no locks); the line is formatted only when printed.
enum Ct : int {
  kCtPostFast, kCtPostCacheHit, kCtPostCacheMiss, kCtPostLabelSkip, kCtPostLabelDo,
  kCtAliasFast, kCtAliasSortCounting, kCtAliasSortFallback, kCtAliasSiteSkip, kCtAliasSiteDo,
  kCtPropsHit, kCtPropsMiss,
  kCtVerified, kCtMismatch, kCtCount
};
inline std::atomic<uint64_t> g_ct[kCtCount];
inline void Count(int c, uint64_t n = 1) { g_ct[c].fetch_add(n, std::memory_order_relaxed); }
inline uint64_t Get(int c) { return g_ct[c].load(std::memory_order_relaxed); }

// Verify: due on every Nth call of one site (a per-site counter), tool only.
inline bool VerifyDue(int site) {
  static std::atomic<uint64_t> n[8];
  const uint32_t v = g_verify.load(std::memory_order_relaxed);
  return v && (n[site & 7].fetch_add(1, std::memory_order_relaxed) % v) == 0;
}
inline std::string& FirstMismatch() {
  static std::string s;
  return s;
}
inline void Mismatch(const char* what, const std::string& detail) {
  if (g_ct[kCtMismatch].fetch_add(1, std::memory_order_relaxed) == 0) FirstMismatch() = std::string(what) + ": " + detail;
}

inline std::string Line() {
  char b[900];
  std::snprintf(b, sizeof b,
                "[rdr][plan_memo] mask %u: post chain fast %llu (pixel cache hits %llu, misses %llu; pass marks skipped %llu, made %llu); alias events fast %llu (counting sort %llu, std::sort fallback %llu; site marks skipped %llu, made %llu); "
                "props match early exits %llu, full scans %llu; verified %llu, mismatched %llu, max image id %u%s%s",
                g_mask.load(), static_cast<unsigned long long>(Get(kCtPostFast)), static_cast<unsigned long long>(Get(kCtPostCacheHit)), static_cast<unsigned long long>(Get(kCtPostCacheMiss)),
                static_cast<unsigned long long>(Get(kCtPostLabelSkip)), static_cast<unsigned long long>(Get(kCtPostLabelDo)), static_cast<unsigned long long>(Get(kCtAliasFast)),
                static_cast<unsigned long long>(Get(kCtAliasSortCounting)), static_cast<unsigned long long>(Get(kCtAliasSortFallback)), static_cast<unsigned long long>(Get(kCtAliasSiteSkip)),
                static_cast<unsigned long long>(Get(kCtAliasSiteDo)),
                static_cast<unsigned long long>(Get(kCtPropsHit)), static_cast<unsigned long long>(Get(kCtPropsMiss)),
                static_cast<unsigned long long>(Get(kCtVerified)), static_cast<unsigned long long>(Get(kCtMismatch)), g_max_image.load(std::memory_order_relaxed), Get(kCtMismatch) ? "; first mismatch: " : "", Get(kCtMismatch) ? FirstMismatch().c_str() : "");
  return b;
}

}  // namespace rdr_adapter::plan_memo
