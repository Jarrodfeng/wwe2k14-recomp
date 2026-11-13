// The live scene writer (Session 101, lane 26; contract: live_scene.h, docs\renderer\SCENE_RECORD.md "The live writer").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// No game or SDK includes. Everything under one mutex held for a whole EndFrame: contributors register at startup, sinks attach when a
// capture arms / the case ring starts, so contention is nil, and RemoveSink returning means no EndFrame still holds the sink.
#include "live_scene.h"

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
};

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

  auto tables = std::make_shared<SceneTables>();
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
  for (size_t i = 0; i < counters.size(); ++i) {
    s.window[i].sum.objects += counters[i].objects;
    s.window[i].sum.unknown += counters[i].unknown;
    s.window[i].sum.stale += counters[i].stale;
  }
  std::shared_ptr<const SceneTables> shared = std::move(tables);
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
