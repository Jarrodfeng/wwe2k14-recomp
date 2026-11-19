// The fallback census (Session 103, landing 2, piece P2; docs\renderer\LANDING2_PLAN.md section 3): why a frame was not wholly ours, as ONE line per 600 frames.
//   [scene][2d] frames F, 2d-whole W, fallback B (by reason: scene ..; render ..; hole ..; refused n/a; unknown_site ..; remnant_extra ..; pass_unknown n/a; blend_code n/a); rows n/a, pair n/a; sum ok; ...; in scene NAME
//   A reason kind nobody notes yet (refused / pass_unknown / blend_code until lane L2a's P1 wires them; rows and pair likewise) prints `n/a`, never `-`: `-` means MEASURED zero. kWired[] below is flipped by P1.
//   The exit line is the final window first, then `; run totals: frames .., 2d-whole .., fallback .. (by reason: ..)`; it also says `pending unadopted N` (2D notes no record ever adopted).
// The deletion's precondition (the stream leaves the player build) is B = 0 over a sweep, so the line must be COMPLETE: every frame the render thread handles is opened (Begin),
// gets exactly one first reason or none (whole), and is closed (Close); a window's reason counts sum to B and W + B = frames, and the caller's own count of frames handled is compared
// with the closed count ("uncounted"). A check that does not hold prints SUM MISMATCH / UNCOUNTED instead of "sum ok" (tools\scene\fallback_triage.py reads it).
//
// FIRST REASON, a documented order: when several reasons hold for one frame the kind that comes FIRST in this list wins (the earliest stage of the pipeline); equal kinds: the first noted.
//   1 scene (the hook side: WhyNot, or the missing input bit)   2 render (DrawLive / the render thread)   3 hole (a 2D draw seen, not a row, by site)   4 refused (a row the drawer refused)
//   5 unknown_site (an op-19 from a 2D site that makes no row)   6 remnant_extra (a stream draw after the post node, not row-matched, by caller)   7 pass_unknown   8 blend_code
//
// No game includes, no guest memory, no REX, no renderer: it sits in renderer_game (kept in every build; the stream-free player build still needs it) and is unit-testable standalone
// (tools\scene\fallback_census_unit.cpp). Thread safe: a mutex (a few notes per frame; the hot op-19 hook only notes a site that is not a recorded or particle site).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <vector>

#include "node_switch.h"
#include "scene_frame.h"

namespace fallback_census {

enum class Kind : uint8_t { Scene = 0, Render, Hole, Refused, UnknownSite, RemnantExtra, PassUnknown, BlendCode, Count };
inline constexpr const char* kKindName[] = {"scene", "render", "hole", "refused", "unknown_site", "remnant_extra", "pass_unknown", "blend_code"};
static_assert(sizeof(kKindName) / sizeof(kKindName[0]) == size_t(Kind::Count));
// Kinds that have a caller today. The others print `n/a` until lane L2a's P1 wires them (then flip the entry). A note that does arrive on an unwired kind is printed anyway.
// S103 L2aF (review M3): refused, blend_code, rows and pair are wired by live.cpp Draw2DOurs / quads2d_contrib.h (dev --wwe2k14_rdr_2d_ours / --wwe2k14_rdr_2d_record; without those the lines read 0 / -, as measured zero of a layer that is off).
// S103 P7 H2 (review (c)): pass_unknown is wired by the scene host (scene_host.cpp: an Unknown-pass row that is not pixel-placed is dropped and noted); the stream path never notes it, so a stream build reads `-` (measured zero of a note nobody makes).
inline constexpr bool kWired[size_t(Kind::Count)] = {true, true, true, true, true, true, true, true};
inline constexpr bool kRowsWired = true, kPairWired = true;
// The ONE frame bound of the 2D drain: record frame j drains every entry with frame <= j (LANDING2_PLAN section 1: Drain(j), not HookFrameOf(j), not j-1). Census::Adopt and P1's Drain / DrainHoles both use it.
inline constexpr uint64_t DrainBound(uint64_t record_frame) { return record_frame; }

// The scene inputs by bit (LANDING2_PLAN section 3; the order of inputs_core.h). Bit 17 stage_clock.
inline constexpr const char* kInputName[18] = {"post_settings", "model_runtime", "sphere_occ", "wrestler_palette", "light_states", "arena_worlds", "ring_rows", "caster_entries", "circle_points",
                                               "plane_constants", "crowd_snapshot", "particles", "camera", "material_params", "screens", "stage_pools", "arena_models", "stage_clock"};
// rdr_own::WhyNot by value (node_switch.h); the unit static_asserts the order.
inline constexpr const char* kWhyName[] = {"none", "switch_off", "not_ready", "inputs_missing", "hold", "latched", "no_inputs_registered"};

inline std::string BitKey(const char* prefix, int bit) {
  std::string s = std::string(prefix) + "bit" + std::to_string(bit);
  if (bit >= 0 && bit < 18) s += std::string("_") + kInputName[bit];
  return s;
}
inline int LowestBit(uint32_t mask) {
  for (int b = 0; b < 32; ++b)
    if ((mask >> b) & 1u) return b;
  return -1;
}
// A reason key is a token: lower case, [a-z0-9_] only (a space or comma would break the line's own parse).
inline std::string Slug(const std::string& in, size_t cap = 40) {
  std::string s;
  for (char c : in) {
    char o = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    const bool ok = (o >= 'a' && o <= 'z') || (o >= '0' && o <= '9');
    if (ok) s += o;
    else if (!s.empty() && s.back() != '_') s += '_';
    if (s.size() >= cap) break;
  }
  while (!s.empty() && s.back() == '_') s.pop_back();
  return s.empty() ? std::string("unnamed") : s;
}
inline std::string HexKey(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

// The scene (hook-side) reason of the frame, from the Scene3D stamp record j carries (stamp.guest_frame == HookFrameOf(j)). `req` = the registered input mask. Never empty: a stamp that says OURS while the
// caller found the frame not ours (scene_frame::DrawOurs said no: a mode / latch difference) is "stamp_ours_not_drawn".
inline std::string SceneKey(const rdr_own::OwnedPassStamp* stamps, uint64_t n, uint64_t record_frame, uint32_t req) {
  const uint64_t hook = scene_frame::HookFrameOf(record_frame);
  for (uint64_t i = 0; i < n; ++i) {
    if (stamps[i].pass != uint8_t(rdr_own::Pass::Scene3D) || stamps[i].guest_frame != hook) continue;
    if (stamps[i].decision == uint8_t(rdr_own::Decision::Ours)) return "stamp_ours_not_drawn";
    const uint8_t w = stamps[i].why_not;
    if (w == uint8_t(rdr_own::WhyNot::InputsMissing)) {
      const int b = LowestBit(req & ~stamps[i].inputs_mask);   // the first missing bit (the lowest)
      return b >= 0 ? BitKey("inputs_missing_", b) : std::string("inputs_missing_unknown_bit");
    }
    return w < sizeof(kWhyName) / sizeof(kWhyName[0]) ? std::string(kWhyName[w]) : std::string("why_") + std::to_string(w);
  }
  return "no_stamp";   // the record carries no Scene3D stamp for this frame (a frame the hook never decided)
}
// The render-side reason: live.cpp SceneOurs's `why` string (+ the loading flag). The stale-input text names a bit; every other text is cut at its first ':' / '(' so a number never splits a reason.
inline std::string RenderKey(const std::string& why, bool loading) {
  const size_t p = why.find("(first: bit ");
  if (p != std::string::npos) return BitKey("inputs_not_fresh_", std::atoi(why.c_str() + p + 12));
  if (loading) return "loading";
  size_t cut = why.find_first_of(":(");
  return Slug(cut == std::string::npos ? why : why.substr(0, cut));
}

// op-19 sites (LANDING2_PLAN section 2; `q callers 82C1B350`: 36 sites in 18 functions). A site that is a ROW today, or a particle site (lane F, not 2D), is not an unknown 2D site.
// (S103 SVY review: IsRecordedSite had no callers and was deleted; the recorded sites are q2d::ClassOfSite(site) == SiteClass::Row)
inline bool IsParticleSite(uint32_t site) { return site == 0x82A0DA58u || site == 0x82A0E338u || site == 0x82A0ED2Cu; }

class Census {
 public:
  struct Tally {
    uint64_t frames = 0, whole = 0, fallback = 0, orphans = 0, uncounted = 0, rows = 0, pair[4] = {};
    std::map<std::string, uint64_t> by[size_t(Kind::Count)];
  };

  void Begin(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mu_);
    open_.emplace(frame, Slot{});
  }
  // The first reason of an open frame. A frame that is not open takes nothing (counted as an orphan: it would break the sum otherwise).
  void Note(uint64_t frame, Kind kind, const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    NoteLocked(frame, kind, key);
  }
  // A 2D note from a thread that does not know the record frame (the op-19 hook, the hole list): keyed by the hook / tap frame, adopted by the first record j with tap_frame <= DrainBound(j) = j
  // (the Drain(j) rule of LANDING2_PLAN section 1: every entry is attributed exactly once, to the first frame that can own it). Deduped per (tap frame, kind, key); capped (kPendCap), a dropped note counted.
  void Pend(uint64_t tap_frame, Kind kind, const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    const PendKey k{tap_frame, uint8_t(kind), key};
    if (pend_.size() >= kPendCap && !pend_.count(k)) return void(++pend_dropped_);   // capped: the NEW note is dropped and counted (no O(n) erase under the mutex)
    pend_.insert(k);   // one entry per (tap frame, kind, key): an op-19 site called a hundred times in a frame is one reason
  }
  // Called by the render thread when it opens record j: the pending 2D notes up to its hook frame become reasons of j.
  void Adopt(uint64_t record_frame) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint64_t bound = DrainBound(record_frame);
    while (!pend_.empty() && std::get<0>(*pend_.begin()) <= bound) {   // ordered by tap frame: the due entries are a prefix
      NoteLocked(record_frame, Kind(std::get<1>(*pend_.begin())), std::get<2>(*pend_.begin()));
      pend_.erase(pend_.begin());
    }
  }
  void Close(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mu_);
    CloseLocked(frame);
  }
  // Closes every open frame older than `frame` (the next record's open closes the previous: the binder's notes of frame j arrive after SceneOurs(j)).
  void CloseBefore(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mu_);
    while (!open_.empty() && open_.begin()->first < frame) CloseLocked(open_.begin()->first);
  }
  void CloseAll() {
    std::lock_guard<std::mutex> lock(mu_);
    while (!open_.empty()) CloseLocked(open_.begin()->first);
  }
  void NoteRows(uint64_t n) { std::lock_guard<std::mutex> lock(mu_); win_.rows += n, rows_noted_ = true; }
  // A whole TakeFrame's pair census at once (rows by row.frame - j: 0, -1, +1, other).
  void NotePairs(uint64_t same, uint64_t prev, uint64_t next, uint64_t other) {
    std::lock_guard<std::mutex> lock(mu_);
    pair_noted_ = true, win_.pair[0] += same, win_.pair[1] += prev, win_.pair[2] += next, win_.pair[3] += other;
  }
  void NotePair(int delta) { std::lock_guard<std::mutex> lock(mu_); pair_noted_ = true; ++win_.pair[delta == 0 ? 0 : delta == -1 ? 1 : delta == 1 ? 2 : 3]; }

  // The window line when 600 frames have closed since the last one (empty otherwise). `handled` = the caller's own count of frames it handled so far (0 = unknown: no uncounted check).
  // One line per call (call again until empty: a scene change can queue one before the full window's).
  std::string TakeLine(uint64_t handled = 0) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!queued_.empty()) {
      std::string l = std::move(queued_.front());
      queued_.pop_front();
      return l;
    }
    if (win_.frames < kWindow) return std::string();
    return FlushLocked(handled, false);
  }
  // The game's scene changed (session_log.h Change; any thread): the window so far is flushed under the OLD scene's name (the line carries `in scene NAME`, so a triage never has to guess by log position),
  // the next window starts under the new one. The flush is queued; the render thread prints it at its next TakeLine. No uncounted check on such a line (the handled count is not known here).
  void SceneChange(const std::string& new_name) {
    std::lock_guard<std::mutex> lock(mu_);
    if (win_.frames > 0) queued_.push_back(FlushLocked(0, false));
    scene_ = Slug(new_name);
  }
  // At exit: closes everything and prints the window (even a short one) with the run totals.
  std::string FinalLine(uint64_t handled = 0) {
    std::lock_guard<std::mutex> lock(mu_);
    while (!open_.empty()) CloseLocked(open_.begin()->first);
    std::string out;
    for (const std::string& q : queued_) out += q + "\n";   // a scene-change window not yet printed comes first
    queued_.clear();
    return out + FlushLocked(handled, true);
  }
  // Unit access.
  Tally Total() { std::lock_guard<std::mutex> lock(mu_); Tally t = total_; Merge(t, win_); return t; }
  uint64_t closed() { std::lock_guard<std::mutex> lock(mu_); return closed_; }

  uint64_t pending() { std::lock_guard<std::mutex> lock(mu_); return pend_.size(); }
  static constexpr uint64_t kWindow = 600;
  static constexpr size_t kPendCap = 4096;
#ifdef FALLBACK_CENSUS_PLANTS
  int plant = 0;   // test only: 1 = a closed fallback frame's reason is not added to the reason table (the sum check must fail)
#endif

 private:
  struct Slot { bool has = false; Kind kind = Kind::Scene; std::string key; };
  using PendKey = std::tuple<uint64_t, uint8_t, std::string>;

  void NoteLocked(uint64_t frame, Kind kind, const std::string& key) {
    auto it = open_.find(frame);
    if (it == open_.end()) return void(++win_.orphans);
    Slot& s = it->second;
    if (!s.has || uint8_t(kind) < uint8_t(s.kind)) s.has = true, s.kind = kind, s.key = key;   // the earlier kind wins; the same kind keeps its first
  }
  void CloseLocked(uint64_t frame) {
    auto it = open_.find(frame);
    if (it == open_.end()) return;
    ++win_.frames, ++closed_;
    if (!it->second.has) ++win_.whole;
    else {
      ++win_.fallback;
#ifdef FALLBACK_CENSUS_PLANTS
      if (plant == 1) { open_.erase(it); return; }
#endif
      ++win_.by[size_t(it->second.kind)][it->second.key];
    }
    open_.erase(it);
  }
  static void Merge(Tally& a, const Tally& b) {
    a.frames += b.frames, a.whole += b.whole, a.fallback += b.fallback, a.orphans += b.orphans, a.uncounted += b.uncounted, a.rows += b.rows;
    for (int i = 0; i < 4; ++i) a.pair[i] += b.pair[i];
    for (size_t k = 0; k < size_t(Kind::Count); ++k)
      for (const auto& kv : b.by[k]) a.by[k][kv.first] += kv.second;
  }
  static uint64_t ReasonSum(const Tally& t) {
    uint64_t n = 0;
    for (size_t k = 0; k < size_t(Kind::Count); ++k)
      for (const auto& kv : t.by[k]) n += kv.second;
    return n;
  }
  static std::string Group(const Tally& t, Kind k) {
    const auto& m = t.by[size_t(k)];
    if (m.empty()) return std::string(kKindName[size_t(k)]) + (kWired[size_t(k)] ? " -" : " n/a");
    if (k == Kind::PassUnknown) {
      uint64_t n = 0;
      for (const auto& kv : m) n += kv.second;
      return std::string(kKindName[size_t(k)]) + " " + std::to_string((unsigned long long)n);
    }
    std::string s = kKindName[size_t(k)];
    char sep = ' ';
    for (const auto& kv : m) s += sep + kv.first + ":" + std::to_string((unsigned long long)kv.second), sep = ',';
    return s;
  }
  static std::string Body(const Tally& t) {
    std::string line = "frames " + std::to_string((unsigned long long)t.frames) + ", 2d-whole " + std::to_string((unsigned long long)t.whole) + ", fallback " + std::to_string((unsigned long long)t.fallback) + " (by reason: ";
    for (size_t k = 0; k < size_t(Kind::Count); ++k) line += (k ? "; " : "") + Group(t, Kind(k));
    return line + ")";
  }
  std::string FlushLocked(uint64_t handled, bool final_line) {
    Tally& w = win_;
    const uint64_t reasons = ReasonSum(w);
    const bool sum_ok = reasons == w.fallback && w.whole + w.fallback == w.frames;
    const uint64_t seen = total_.frames + w.frames;
    // uncounted: frames the caller handled that this census never closed (a path that forgot Begin). Orphan notes: notes that found no open frame.
    const int64_t uncounted = handled ? int64_t(handled) - int64_t(seen) : 0;
    // The line's first part is ALWAYS the window (also on the exit line: the final window); the run totals follow it on the exit line only.
    std::string line = final_line ? "[scene][2d] exit: " : "[scene][2d] ";
    line += Body(w);
    line += "; rows " + (rows_noted_ || kRowsWired ? std::to_string((unsigned long long)w.rows) : std::string("n/a"));
    line += ", pair " + (pair_noted_ || kPairWired ? "(0 " + std::to_string((unsigned long long)w.pair[0]) + ", -1 " + std::to_string((unsigned long long)w.pair[1]) + ", +1 " + std::to_string((unsigned long long)w.pair[2]) +
                                                          ", other " + std::to_string((unsigned long long)w.pair[3]) + ")" : std::string("n/a"));
    line += sum_ok ? "; sum ok" : "; SUM MISMATCH (reasons " + std::to_string((unsigned long long)reasons) + " vs fallback " + std::to_string((unsigned long long)w.fallback) + ", whole " + std::to_string((unsigned long long)w.whole) + " of " + std::to_string((unsigned long long)w.frames) + ")";
    if (uncounted > 0) line += "; UNCOUNTED " + std::to_string((long long)uncounted) + " (handled " + std::to_string((unsigned long long)handled) + ", closed " + std::to_string((unsigned long long)seen) + ")";
    else if (handled) line += "; none uncounted";
    line += "; orphan notes " + std::to_string((unsigned long long)w.orphans) + ", pending dropped " + std::to_string((unsigned long long)pend_dropped_) + ", window " + std::to_string((unsigned long long)w.frames);
    if (final_line) line += ", pending unadopted " + std::to_string((unsigned long long)pend_.size());   // 2D notes no record ever took (a hole / unknown site after the last frame, or a clock mismatch)
    if (!scene_.empty()) line += "; in scene " + scene_;
    if (final_line) {   // the run totals (every earlier window + this one), after the window part: a reader that wants one window stops at "; run totals:"
      Tally t = total_;
      Merge(t, w);
      line += "; run totals: " + Body(t);
    }
    Merge(total_, w);
    win_ = Tally();
    return line;
  }

  std::mutex mu_;
  std::map<uint64_t, Slot> open_;
  std::set<PendKey> pend_;
  std::deque<std::string> queued_;
  std::string scene_;
  bool rows_noted_ = false, pair_noted_ = false;
  Tally win_, total_;
  uint64_t closed_ = 0, pend_dropped_ = 0;
};

inline Census g_census;

// --- the call sites (one-liners)
inline bool Enabled() { return scene_frame::CurrentMode() == scene_frame::Mode::Ours; }
// live.cpp SceneOurs, first thing: close the previous frames, open this one, take the 2D notes that are due.
inline void BeginFrame(uint64_t record_frame) { g_census.CloseBefore(record_frame), g_census.Begin(record_frame), g_census.Adopt(record_frame); }
inline void NoteScene(uint64_t frame, const std::string& key) { g_census.Note(frame, Kind::Scene, key); }
inline void NoteRender(uint64_t frame, const std::string& key) { g_census.Note(frame, Kind::Render, key); }
inline void NoteRenderWhy(uint64_t frame, const std::string& why, bool loading) { g_census.Note(frame, Kind::Render, RenderKey(why, loading)); }
// A mode-2 frame that reached the render thread with no scene record at all.
inline void NoRecord(uint64_t frame) { BeginFrame(frame), NoteRender(frame, "no_scene_record"); }
// The 2D draw layer (L2a: rows, refusals, blend codes) names the RECORD frame j it is drawing.
inline void NoteRows(uint64_t n) { g_census.NoteRows(n); }
inline void NotePairs(uint64_t same, uint64_t prev, uint64_t next, uint64_t other) { g_census.NotePairs(same, prev, next, other); }
inline void NoteHole2d(uint64_t record_frame, uint32_t site) { g_census.Note(record_frame, Kind::Hole, HexKey(site)); }
inline void NoteRefused(uint64_t record_frame, const char* reason) { g_census.Note(record_frame, Kind::Refused, Slug(reason)); }
inline void NotePassUnknown(uint64_t record_frame) { g_census.Note(record_frame, Kind::PassUnknown, "all"); }
inline void NoteBlendCode(uint64_t record_frame, uint32_t code) { g_census.Note(record_frame, Kind::BlendCode, std::to_string(code)); }
// The binder: a stream draw that reaches the draw on an OURS frame is a remnant draw (under OURS the post chain is skipped, so there is no "after the post node" to test; no row matcher exists before P1: every such
// draw counts, which is the truth: the stream still draws it).
inline void NoteRemnantExtra(uint64_t record_frame, uint32_t caller) { g_census.Note(record_frame, Kind::RemnantExtra, HexKey(caller)); }
// The binder's call: only the FIRST such draw of a Bind is noted (`noted` is reset at the start of Bind): the first reason of a frame is all the census keeps, so the later draws skip the lock and the string.
inline void NoteRemnantOnce(bool& noted, uint32_t scene_frame_id, bool velocity_mode, uint64_t record_frame, uint32_t caller) {
  if (noted || !scene_frame_id || velocity_mode) return;
  noted = true;
  NoteRemnantExtra(record_frame, caller);
}
// The game's scene changed (session_log.h Change).
inline void SceneChange(const std::string& name) { g_census.SceneChange(name); }
// The tap threads (hook / tap frame clock): pended, adopted by BeginFrame.
inline void PendHole(uint32_t tap_frame, uint32_t site) { if (Enabled()) g_census.Pend(tap_frame, Kind::Hole, HexKey(site)); }
// quads2d_tap OnOp19, every op-19 call: a call with no 2D scope open that is not a particle site = unknown_site, a RECORDED site too (a recorded site called outside its scope makes no row). With a scope open the
// row / hole path owns the call (exempt only then).
inline void OnOp19Site(uint32_t tap_frame, uint32_t site, bool scope_open) {
  if (scope_open || !Enabled() || IsParticleSite(site)) return;
  g_census.Pend(tap_frame, Kind::UnknownSite, HexKey(site));
}

}  // namespace fallback_census
