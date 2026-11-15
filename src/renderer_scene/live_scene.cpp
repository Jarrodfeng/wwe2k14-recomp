// The live scene writer (Session 101, lane 26; contract: live_scene.h, docs\renderer\SCENE_RECORD.md "The live writer").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// No game or SDK includes. Everything under one mutex held for a whole EndFrame: contributors register at startup, sinks attach when a
// capture arms / the case ring starts, so contention is nil, and RemoveSink returning means no EndFrame still holds the sink.
#include "live_scene.h"

#include "../renderer_adapter/rdr_switches.h"   // S102 T11b: wwe2k14_rdr_scene_strings_once, read every frame (no game or SDK includes)
#include "string_pool.h"
#include "swap_once.h"     // S102 SW1: wwe2k14_rdr_swap_once (the swap thread's every-frame work done once)
#include "tables_pool.h"   // S102 SW1

#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <vector>

namespace rdr_scene::live {
namespace {

struct PerContributor {
  Counters sum;
  uint64_t refused = 0;
};

struct State {
  std::mutex mu;
  std::vector<Contributor*> contributors;
  std::vector<PerContributor> window;  // one per contributor, this census window
  std::vector<Sink*> sinks;
  std::atomic<int> sink_count{0};
  std::atomic<uint32_t> contributor_count{0};
  LogFn log = nullptr;
  Totals totals;
  // this census window
  uint32_t frames_in_window = 0;
  uint64_t window_records = 0, window_refused = 0, window_sink_errors = 0;
  uint64_t validate_refused = 0;  // refusals blamed on no single contributor (the tables failed validation)
  uint32_t logged_refusals = 0;
  LatchTotals latch;  // S102 U1 (under mu; violations are the atomics below)
  StringPool pool;                  // S102 T11b: the strings every frame's record repeats (string_pool.h); written by the writer thread under `mu`
  uint64_t pool_window_start = 0;   // the guest frame the pool's census window began at (0 = none yet)
  TablesPool tables_pool;           // S102 SW1: recycled record tables (swap_once bit 1); the table's last owner gives it back from any thread (its own mutex)
  uint64_t so_window_start = 0;     // S102 SW1: the guest frame the swap_once census window began at (0 = none yet)
  uint64_t so_base[8] = {};         // the swap_once counters at the window's start (the line prints the window's counts)
};

// S102 U1: the deferred emit's thread flag and the violation counter live in live_scene.h (tail_detail, inline).
using tail_detail::t_deferred_emit;
using tail_detail::g_violations;
using tail_detail::g_first_violation;

State& S() {
  static State* s = new State;  // leaked: a late call after static destruction never touches freed memory
  return *s;
}

void Log(State& s, const std::string& line) {
  if (s.log) s.log(line.c_str());
}

// A refusal is loud, but never a flood: the first 20 each run, then one in 600.
void LogRefusal(State& s, const std::string& line) {
  const uint32_t n = s.logged_refusals++;
  if (n < 20 || n % 600 == 0) Log(s, "[scene][live] WARN record refused: " + line);
}

}  // namespace

void Register(Contributor* c) {
  if (!c) return;
  State& s = S();
  std::lock_guard lock(s.mu);
  s.contributors.push_back(c);
  s.window.emplace_back();
  s.contributor_count.store(uint32_t(s.contributors.size()), std::memory_order_relaxed);
}

bool Enabled() { return S().sink_count.load(std::memory_order_relaxed) > 0; }

void AddSink(Sink* sink) {
  if (!sink) return;
  State& s = S();
  std::lock_guard lock(s.mu);
  for (Sink* x : s.sinks)
    if (x == sink) return;
  s.sinks.push_back(sink);
  s.sink_count.store(int(s.sinks.size()), std::memory_order_relaxed);
}

void RemoveSink(Sink* sink) {
  State& s = S();
  std::lock_guard lock(s.mu);
  for (size_t i = 0; i < s.sinks.size(); ++i) {
    if (s.sinks[i] == sink) {
      s.sinks.erase(s.sinks.begin() + std::ptrdiff_t(i));
      break;
    }
  }
  s.sink_count.store(int(s.sinks.size()), std::memory_order_relaxed);
}

void SetLog(LogFn fn) {
  State& s = S();
  std::lock_guard lock(s.mu);
  s.log = fn;
}

static bool BadStr(const SceneTables& t, uint32_t f) { return f > t.strings.size(); }

bool Validate(const SceneTables& t, std::string& error) {
  char b[200];
  for (size_t i = 0; i < t.assets.size(); ++i)
    if (BadStr(t, t.assets[i].pac) || BadStr(t, t.assets[i].entry)) {
      std::snprintf(b, sizeof(b), "AssetRef %zu: a string field points outside the strings", i);
      error = b;
      return false;
    }
  for (size_t i = 0; i < t.objects.size(); ++i) {
    const Object& o = t.objects[i];
    if (o.asset >= t.assets.size()) {
      std::snprintf(b, sizeof(b), "Object %zu: asset %u, but there are %zu assets", i, o.asset, t.assets.size());
      error = b;
      return false;
    }
    if (uint64_t(o.bones_first) + o.bones_count > t.bones.size()) {
      std::snprintf(b, sizeof(b), "Object %zu: bones %u+%u past the %zu palette matrices", i, o.bones_first, o.bones_count, t.bones.size());
      error = b;
      return false;
    }
    if (uint64_t(o.params_first) + o.params_count > t.params.size()) {
      std::snprintf(b, sizeof(b), "Object %zu: params %u+%u past the %zu values", i, o.params_first, o.params_count, t.params.size());
      error = b;
      return false;
    }
    if (BadStr(t, o.id)) {
      std::snprintf(b, sizeof(b), "Object %zu: id string points outside the strings", i);
      error = b;
      return false;
    }
  }
  if (!t.crowd_runs.empty() || !t.crowd_members.empty()) {  // S102 T19: the crowd run markers must index their members, assets and palettes inside the tables
    if (const std::string rp0 = CrowdRunsProblem(t); !rp0.empty()) {
      error = rp0;
      return false;
    }
  }
  for (size_t i = 0; i < t.light_refs.size(); ++i)  // S101 lane 109: a light ref must point inside the light states
    if (t.light_refs[i].state >= t.light_states.size()) {
      std::snprintf(b, sizeof(b), "LightRef %zu: state %u, but there are %zu light states", i, t.light_refs[i].state, t.light_states.size());
      error = b;
      return false;
    }
  for (size_t i = 0; i < t.arena_worlds.size(); ++i)  // S101 lane 130: same rule as ParseRecord (layout 1, kinds 1 / 2)
    if (t.arena_worlds[i].layout != 1 || (t.arena_worlds[i].kind != kAwObject && t.arena_worlds[i].kind != kAwClock)) {
      std::snprintf(b, sizeof(b), "ArenaWorld %zu: layout %u kind %u", i, unsigned(t.arena_worlds[i].layout), unsigned(t.arena_worlds[i].kind));
      error = b;
      return false;
    }
  if (const std::string rp = RingTablesProblem(t); !rp.empty()) {  // lane 194: the ring tables (a writer that fills them wrongly is refused, like the circles)
    error = rp;
    return false;
  }
  if (const std::string cp2 = CasterTablesProblem(t); !cp2.empty()) {  // lane 202: the caster tables
    error = cp2;
    return false;
  }
  for (size_t i = 0; i < t.params.size(); ++i)
    if (BadStr(t, t.params[i].name)) {
      std::snprintf(b, sizeof(b), "ParamValue %zu: name string points outside the strings", i);
      error = b;
      return false;
    }
  if (const std::string cp = CircleTablesProblem(t); !cp.empty()) {  // lane 136: the circle tables
    error = cp;
    return false;
  }
  // (lane 139: the plane tables are not refused here: their writer drops its own tables when they do not check out, counted, plane_contrib.h DropIfBad)
  return true;
}

void EndFrame(uint64_t guest_frame) { EndFrame(guest_frame, kNoCaptureFrame); }

static void EndFrameImpl(uint64_t guest_frame, uint32_t capture_frame);
static void BuildFrame(State& s, const FrameInfo& info);  // S102 U1

// S101 lane 26 review: nothing throws into guest code. An allocation failure (the tables, the vectors) refuses the frame like any other fault.
void EndFrame(uint64_t guest_frame, uint32_t capture_frame) {
  if (!Enabled()) return;
  try {
    EndFrameImpl(guest_frame, capture_frame);
  } catch (...) {
    State& s = S();
    try {
      std::lock_guard lock(s.mu);
      ++s.totals.refused;
      ++s.window_refused;
      LogRefusal(s, "guest frame " + std::to_string(guest_frame) + ": out of memory or an unexpected failure building the record");
    } catch (...) {
    }
  }
}

static void EndFrameImpl(uint64_t guest_frame, uint32_t capture_frame) {
  if (!Enabled()) return;  // off = one relaxed load
  State& s = S();
  if (!s.contributor_count.load(std::memory_order_relaxed)) return;  // no class registered yet: nothing to build, no lock taken
  std::lock_guard lock(s.mu);
  if (s.sinks.empty() || s.contributors.empty()) return;  // no sink, or no class has anything to say yet: nothing to build
  ++s.totals.frames;
  ++s.frames_in_window;

  FrameInfo info;
  info.guest_frame = guest_frame;
  info.frame_in_window = s.frames_in_window - 1;
  info.capture_frame = capture_frame;
  BuildFrame(s, info);  // S102 U1: the body moved into BuildFrame (unchanged), shared with LatchFrame / EmitLatched
}

// The record of one frame: every contributor's Emit in registration order, Validate, the sinks. `s.mu` is held by the caller.
static void BuildFrame(State& s, const FrameInfo& info) {
  const uint64_t guest_frame = info.guest_frame;
  // S102 SW1 (wwe2k14_rdr_swap_once, PerFrame, read here every record): the mask the contributors read while this record is built, and the recycled table (bit 1).
  const uint32_t swap_mask = uint32_t(rdr_sw::IntSeen(rdr_sw::kSwapOnce, int64_t(swap_once::g_fallback.load(std::memory_order_relaxed)), guest_frame)) & swap_once::kAll;
  swap_once::g_mask.store(swap_mask, std::memory_order_relaxed);
  if (!(swap_mask & swap_once::kTables)) s.tables_pool.Trim();   // (SW1 review 3: bit 1 off = the free list is released)
  TablesPool::Handle tables = (swap_mask & swap_once::kTables) ? s.tables_pool.Take() : TablesPool::Plain();
  if (swap_mask) {
    swap_once::g_count.frames.fetch_add(1, std::memory_order_relaxed);
    if (s.so_window_start == 0 || guest_frame >= s.so_window_start + 600) {   // the census line, every 600 frames while the lever is on (the S102 rule)
      const swap_once::Counters& c = swap_once::g_count;
      const uint64_t now[8] = {c.frames.load(std::memory_order_relaxed), c.tables_reused.load(std::memory_order_relaxed), c.tables_made.load(std::memory_order_relaxed),
                               c.tables_dropped.load(std::memory_order_relaxed), c.crowd_fast.load(std::memory_order_relaxed), c.crowd_slow.load(std::memory_order_relaxed),
                               c.overlay_posted.load(std::memory_order_relaxed), c.overlay_skipped.load(std::memory_order_relaxed)};
      if (s.so_window_start != 0 && s.log) {
        char line[400];
        std::snprintf(line, sizeof line,
                      "[rdr][swap_once] mask %u, frames %llu; tables: reused %llu, made %llu, dropped (free list full) %llu; crowd members: fast %llu, through the id cache %llu; overlay requests: posted %llu, skipped %llu",
                      swap_mask, (unsigned long long)(now[0] - s.so_base[0]), (unsigned long long)(now[1] - s.so_base[1]), (unsigned long long)(now[2] - s.so_base[2]),
                      (unsigned long long)(now[3] - s.so_base[3]), (unsigned long long)(now[4] - s.so_base[4]), (unsigned long long)(now[5] - s.so_base[5]),
                      (unsigned long long)(now[6] - s.so_base[6]), (unsigned long long)(now[7] - s.so_base[7]));
        s.log(line);
      }
      for (int i = 0; i < 8; ++i) s.so_base[i] = now[i];
      s.so_window_start = guest_frame ? guest_frame : 1;
    }
  }
  // S102 T11b: the string pool (read every frame, at the record's start). The pool takes what the contributors queued last frame, then this frame's table starts as a copy of it;
  // a contributor finds the table's pool with SeedPool() and writes pool fields instead of interning (string_pool.h). Off = nothing here runs.
  if (rdr_sw::BoolSeen(rdr_sw::kSceneStringsOnce, g_scene_strings_once.load(std::memory_order_relaxed), guest_frame)) {
    StringPool& pool = s.pool;
    pool.Commit();
    ++pool.stats.frames;
    if (tables->SeedStrings(pool)) ++pool.stats.seeded;
    if (s.pool_window_start == 0 || guest_frame >= s.pool_window_start + 600) {   // the census line, every 600 frames while the lever is on (R13 / S102 rule)
      if (s.pool_window_start != 0 && s.log) {
        const StringPool::Stats& st = pool.stats;
        char line[420];
        std::snprintf(line, sizeof line,
                      "[rdr][scene_strings_once] frames %llu, taken %llu, declined %llu (by reason: stale crowd %llu, pool full %llu, not fresh %llu), crowd fields from the pool %llu (not yet %llu), arena fields %llu (not yet %llu), pool %zu strings %zu bytes, queued at most %llu, pool resets %llu",
                      (unsigned long long)st.frames, (unsigned long long)st.seeded, (unsigned long long)(st.frames - st.seeded + st.crowd_stale + st.refused_full),
                      (unsigned long long)st.crowd_stale, (unsigned long long)st.refused_full, (unsigned long long)(st.frames - st.seeded), (unsigned long long)st.crowd_fields,
                      (unsigned long long)st.crowd_fallback, (unsigned long long)st.arena_fields, (unsigned long long)st.arena_fallback, pool.Count(), pool.Blob().size(),
                      (unsigned long long)st.pending_max, (unsigned long long)st.resets);
        s.log(line);
      }
      pool.stats = StringPool::Stats{};
      s.pool_window_start = guest_frame ? guest_frame : 1;
    }
  }
  tables->header.source = uint8_t(Source::Live);
  tables->header.guest_frame = guest_frame;

  std::vector<Counters> counters(s.contributors.size());
  bool refused = false;
  for (size_t i = 0; i < s.contributors.size(); ++i) {
    Contributor* c = s.contributors[i];
    try {
      // kScEngineObjects is parallel to objects: once any class has written one, every class's objects get an entry (0 = none). A class that
      // writes engine objects resizes to its own objects first and appends; this keeps the others' rows aligned (S101 contract).
      if (!tables->engine_objects.empty()) tables->engine_objects.resize(tables->objects.size(), 0u);
      c->Emit(info, *tables, counters[i]);
      if (!tables->engine_objects.empty()) tables->engine_objects.resize(tables->objects.size(), 0u);
    } catch (const std::exception& e) {
      ++s.window[i].refused;
      refused = true;
      LogRefusal(s, std::string("contributor \"") + c->Name() + "\" threw at guest frame " + std::to_string(guest_frame) + ": " + e.what());
      break;
    } catch (...) {
      ++s.window[i].refused;
      refused = true;
      LogRefusal(s, std::string("contributor \"") + c->Name() + "\" threw at guest frame " + std::to_string(guest_frame));
      break;
    }
  }
  if (!refused) {
    std::string why;
    if (!Validate(*tables, why)) {
      refused = true;
      ++s.validate_refused;
      LogRefusal(s, "guest frame " + std::to_string(guest_frame) + " failed validation: " + why);
    }
  }
  if (refused) {
    ++s.totals.refused;
    ++s.window_refused;
    return;  // the frame's record is dropped; the game path is unaffected
  }
  tables->DropSeedPool();   // S102 T11b (R20 review F4): the sinks and the render thread get a table that does not point into the writer's mutable pool
  for (size_t i = 0; i < counters.size(); ++i) {
    s.window[i].sum.objects += counters[i].objects;
    s.window[i].sum.unknown += counters[i].unknown;
    s.window[i].sum.stale += counters[i].stale;
  }
  std::shared_ptr<const SceneTables> shared = std::move(tables);   // (S102 SW1: the unique handle's deleter goes with it: the last owner gives the table back to the pool)
  for (Sink* sink : s.sinks) {
    try {
      sink->OnRecordShared(info, shared);
    } catch (...) {
      ++s.totals.sink_errors;
      ++s.window_sink_errors;
    }
  }
  ++s.totals.records;
  ++s.window_records;
}

// ---- S102 U1 (docs\renderer\SWAP_TAIL.md): the frame boundary in two halves ----

static void RefuseLocked(State& s, uint64_t guest_frame, const std::string& why) {
  ++s.totals.refused;
  ++s.window_refused;
  LogRefusal(s, "guest frame " + std::to_string(guest_frame) + ": " + why);
}

static void LatchImpl(uint64_t guest_frame, uint32_t capture_frame, bool allow_defer, Latched& out) {
  State& s = S();
  if (!s.contributor_count.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(s.mu);
  if (s.sinks.empty() || s.contributors.empty()) return;
  ++s.totals.frames;  // (EndFrameImpl's own count: one per frame, here instead of there)
  ++s.frames_in_window;
  FrameInfo info;
  info.guest_frame = guest_frame;
  info.frame_in_window = s.frames_in_window - 1;
  info.capture_frame = capture_frame;
  ++s.latch.latched;
  bool all = true;
  for (size_t i = 0; i < s.contributors.size(); ++i) {
    Contributor* c = s.contributors[i];
#ifdef RDR_TAIL_PLANTS
    if (g_tail_plant == 1 || g_tail_plant == 2 + int(i)) continue;  // planted: "latched" without taking anything (1 = every contributor, 2 + i = only contributor i)
#endif
    bool took = false;
    try {
      took = c->Latch(info);
    } catch (...) {
      ++s.window[i].refused;
      RefuseLocked(s, guest_frame, std::string("contributor \"") + c->Name() + "\" threw in its latch");
      return;
    }
    if (!took) {
      all = false;
      s.latch.not_latchable_name = c->Name();
    }
  }
  if (all && allow_defer) {
    out.emit_deferred = true;
    out.info = info;
    out.contributors = uint32_t(s.contributors.size());
    ++s.latch.deferred;
    return;
  }
  if (!all) ++s.latch.not_latchable;
  BuildFrame(s, info);  // the contributors that latched use what they took (same frame)
}

void LatchFrame(uint64_t guest_frame, uint32_t capture_frame, bool allow_defer, Latched& out) {
  out.emit_deferred = false;
  if (!Enabled()) return;
  try {
    LatchImpl(guest_frame, capture_frame, allow_defer, out);
  } catch (...) {
    out.emit_deferred = false;
    State& s = S();
    try {
      std::lock_guard lock(s.mu);
      RefuseLocked(s, guest_frame, "out of memory or an unexpected failure at the latch");
    } catch (...) {
    }
  }
}

void EmitLatched(Latched& l) {
  if (!l.emit_deferred) return;
  l.emit_deferred = false;
  State& s = S();
  t_deferred_emit = true;
  try {
    std::lock_guard lock(s.mu);
    if (s.contributors.size() != l.contributors) {
      ++s.latch.registry_changed;
      RefuseLocked(s, l.info.guest_frame, "a contributor registered between the latch and the emit");
    } else if (!s.sinks.empty()) {
      BuildFrame(s, l.info);
    }
  } catch (...) {
    try {
      std::lock_guard lock(s.mu);
      RefuseLocked(s, l.info.guest_frame, "out of memory or an unexpected failure building the latched record");
    } catch (...) {
    }
  }
  t_deferred_emit = false;
}

LatchTotals GetLatchTotals() {
  State& s = S();
  LatchTotals t;
  {
    std::lock_guard lock(s.mu);
    t = s.latch;
  }
  t.violations = g_violations.load(std::memory_order_relaxed);
  if (const char* f = g_first_violation.load(std::memory_order_relaxed)) t.first_violation = f;
  return t;
}

void CensusLog(uint64_t from, uint64_t to) {
  State& s = S();
  std::lock_guard lock(s.mu);
  if (!s.frames_in_window && !s.window_refused) return;
  std::string line = "[scene][live] frames " + std::to_string(from) + "-" + std::to_string(to) + ": ";
  for (size_t i = 0; i < s.contributors.size(); ++i) {
    const PerContributor& p = s.window[i];
    line += std::string(i ? ", " : "") + s.contributors[i]->Name() + " objects " + std::to_string(p.sum.objects) + ", fields_unknown " +
            std::to_string(p.sum.unknown);
    if (p.sum.stale) line += ", stale " + std::to_string(p.sum.stale);
    if (p.refused) line += ", REFUSED " + std::to_string(p.refused);
    try {
      std::string extra;  // S101 lane 109: a contributor's own totals
      s.contributors[i]->Census(extra);
      if (!extra.empty()) line += " " + extra;
    } catch (...) {
    }
  }
  line += "; records " + std::to_string(s.window_records) + " of " + std::to_string(s.frames_in_window) + " frames";
  if (s.window_refused) line += ", refused " + std::to_string(s.window_refused) + (s.validate_refused ? " (validation " + std::to_string(s.validate_refused) + ")" : "");
  if (s.window_sink_errors) line += ", sink errors " + std::to_string(s.window_sink_errors);
  for (Sink* sink : s.sinks) {
    std::string part;
    try {
      sink->Census(part);
    } catch (...) {
    }
    if (!part.empty()) line += "; " + part;
  }
  Log(s, line);
  for (PerContributor& p : s.window) p = PerContributor{};
  s.frames_in_window = 0;
  s.window_records = s.window_refused = s.window_sink_errors = s.validate_refused = 0;
}

Totals GetTotals() {
  State& s = S();
  std::lock_guard lock(s.mu);
  return s.totals;
}

}  // namespace rdr_scene::live
