// wwe2k14 - Session 101 lane 83 (docs\renderer\SKIP_GAME_WORK_MAP.md section 1.3): the passive cost counters that settle the map's [U]s.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only; NO game or SDK includes, so the unit test (tools\cost\) runs this exact header.
//
// What is measured (all behind ONE dev switch, --wwe2k14_cost_counters, default false; off = one relaxed load and a branch per hook):
//   1. record  : the game thread's time recording a class: the whole span of sub_82C04DF0 (the model draw, wrestlers / arena / ring props), the
//                part draws sub_82BDED40 that run outside such a span (the flashes' pass, props), the crowd block / card loops. "part" is the
//                part-draw share of it (sub_82BDED40 inside or outside a span). The class comes from the model object (a table the binder fills
//                when it replaces a draw: NoteObject) and the pass site (the flashes). An object the binder never replaced is class "other".
//   2. replay  : the render thread's time in the replay dispatcher sub_82BB53D0, per record, summed by opcode (outermost call only).
//                It INCLUDES the reader (our draw hook runs inside the op-15 replay): "excl reader" subtracts the reader column.
//   3. reader  : our OnDrawIndexed (DrawCommon, from the lock) split by the draw's origin class; inline-vertex draws are not counted.
//   4. replace : Binder::TryReplace's existing per-class timer (ReplaceCounters::ms), published by the live thread.
// Lane 94 (accuracy): the replay counter times 1 record in 2^g_sample_shift (default 16; a per-thread xorshift picks them, so a periodic record
//   pattern cannot alias with the sampler), counts every record exactly, and keeps a per-opcode table (0-127). Per op: ticks = sampled ticks * n / sampled n.
//   The two TSC reads' own cost (Overhead(): an empty back-to-back pair, measured once at start) is subtracted from every span family, per counted span.
// Every number is a sum of TSC ticks (x64) or steady_clock ns (other CPUs), turned into time only when printed (TicksPerNs).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#endif

namespace wwe2k14::cost {

enum Cls : uint8_t { kArena, kWrestlers, kRing, kFlash, kCrowd, kOther, kClasses };
inline constexpr const char* kClsName[kClasses] = {"arena", "wrestlers", "ring", "flash", "crowd", "other"};
// The flashes' pass site (draw_replace_hook.h, FLASH_OURS.md): the origin pass_site of a flash draw.
inline constexpr uint32_t kFlashSite = 0x82C139FCu;

// Replay opcodes listed on their own; anything else is "other".
inline constexpr int kOps = 8;
inline constexpr uint32_t kOpList[kOps - 1] = {15, 19, 21, 28, 35, 68, 81};
inline int OpSlot(uint32_t op) {
  for (int i = 0; i < kOps - 1; ++i)
    if (kOpList[i] == op) return i;
  return kOps - 1;
}
inline const char* OpName(int i) {
  static const char* const k[kOps] = {"op15", "op19", "op21", "op28", "op35", "op68", "op81", "other"};
  return k[i];
}

// The full per-opcode table (lane 94) and the S50 groups (REPLAY_OTHER_RECORDS.md section 2).
inline constexpr int kAllOps = 128;
inline int OpIndex(uint32_t op) { return op < uint32_t(kAllOps) ? int(op) : kAllOps - 1; }
enum Grp : uint8_t { kGSetters, kGBanked, kGStream, kGBools, kGComps, kGEffect, kGObjCall, kGPasses, kGRet, kGResource, kGListed, kGRest, kGroups };
inline constexpr const char* kGrpName[kGroups] = {"setters", "banked", "stream", "bools", "comps", "effect", "objcall", "passes", "ret82", "resource", "listed", "rest"};
inline Grp GroupOf(int op) {
  switch (op) {
    case 3: case 4: case 5: case 80: return kGSetters;
    case 12: case 14: return kGStream;
    case 27: case 34: case 41: return kGBools;
    case 6: case 8: case 9: case 10: return kGEffect;
    case 43: case 44: return kGObjCall;
    case 45: case 46: case 47: case 48: case 49: case 64: case 65: case 72: case 73: case 74: case 75: return kGPasses;
    case 82: return kGRet;
    case 15: case 19: case 21: case 28: case 35: case 68: case 81: return kGListed;
    default: break;
  }
  if (op >= 83 && op <= 103) return kGBanked;
  if ((op >= 22 && op <= 26) || (op >= 29 && op <= 33) || (op >= 36 && op <= 40)) return kGComps;
  if (op >= 50 && op <= 60) return kGResource;
  return kGRest;
}
inline std::atomic<int> g_sample_shift{4};  // time 1 replay record in 2^shift (0 = every record; the unit test uses 0 for exact sums)

// ---- the switch, the clock, the test seams --------------------------------------------------------------------------------------------------
inline std::atomic<bool> g_on{false};
inline bool On() { return g_on.load(std::memory_order_relaxed); }
// Test seams (the unit test only; never set in the game): a fake clock, and a planted wiring fault (docs: the planted twins must fail the test).
inline bool g_fake_clock = false;
inline std::atomic<uint64_t> g_fake_ticks{0};
enum class Fault : int { None = 0, WrongClass = 1, WrongReplayOp = 2, ReaderToRecord = 3, SkipOp = 4 };
inline constexpr int kSkippedOp = 12;  // Fault::SkipOp: the sampler never sees this opcode
inline Fault g_fault = Fault::None;

inline uint64_t Ticks() {
  if (g_fake_clock) return g_fake_ticks.load(std::memory_order_relaxed);
#if defined(_M_X64) || defined(__x86_64__)
  return __rdtsc();
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
inline double TicksPerNs() {
  static const double ratio = [] {
#if defined(_M_X64) || defined(__x86_64__)
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = __rdtsc();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(2)) {
    }
    const auto c1 = std::chrono::steady_clock::now();
    const uint64_t t1 = __rdtsc();
    return double(t1 - t0) / double(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
#else
    return 1.0;
#endif
  }();
  return ratio;
}

// The cost of the instrument itself: the ticks an EMPTY span measures (two back-to-back TSC reads). Measured once, on the first Snapshot with the
// counters on; 0 under the fake clock. Subtracted per counted span in Snapshot().
inline std::atomic<double> g_overhead_ticks{-1.0};
inline double MeasureOverheadTicks() {
  if (g_fake_clock) return 0.0;
  uint64_t sum = 0;
  constexpr int kN = 200000;
  for (int i = 0; i < kN; ++i) {
    const uint64_t a = Ticks();
    const uint64_t b = Ticks();
    sum += b - a;
  }
  return double(sum) / kN;
}
inline double Overhead() {
  double v = g_overhead_ticks.load(std::memory_order_relaxed);
  if (v < 0) {
    v = MeasureOverheadTicks();
    g_overhead_ticks.store(v, std::memory_order_relaxed);
  }
  return v;
}
inline uint64_t Net(uint64_t ticks, uint64_t n, double ovh) {
  const double v = double(ticks) - double(n) * ovh;
  return v > 0 ? uint64_t(v) : 0;
}

// ---- the object -> class table (the binder publishes, the hooks read) --------------------------------------------------------------------
// Direct mapped with two probes; a collision evicts (the object then reads as "other" until the binder replaces one of its draws again).
inline constexpr uint32_t kTable = 4096;
inline std::atomic<uint64_t> g_objects[kTable];  // (object << 8) | class + 1; 0 = empty
inline uint32_t Slot(uint32_t object, uint32_t probe) { return (((object * 0x9E3779B1u) >> 20) + probe) & (kTable - 1); }
inline void NoteObject(uint32_t object, Cls cls) {
  if (!object) return;
  const uint64_t v = (uint64_t(object) << 8) | (uint64_t(cls) + 1);
  for (uint32_t p = 0; p < 2; ++p) {
    std::atomic<uint64_t>& s = g_objects[Slot(object, p)];
    const uint64_t cur = s.load(std::memory_order_relaxed);
    if (cur == 0 || (cur >> 8) == object) { s.store(v, std::memory_order_relaxed); return; }
  }
  g_objects[Slot(object, 0)].store(v, std::memory_order_relaxed);
}
inline Cls ObjectClass(uint32_t object) {
  for (uint32_t p = 0; p < 2; ++p) {
    const uint64_t cur = g_objects[Slot(object, p)].load(std::memory_order_relaxed);
    if (cur && (cur >> 8) == object) return Cls((cur & 0xFF) - 1);
  }
  return kOther;
}
// The class of a model draw / span: the flashes by their pass site, everything else by the object.
inline Cls ClassOf(uint32_t object, uint32_t pass_site) {
  Cls c = pass_site == kFlashSite ? kFlash : ObjectClass(object);
  if (g_fault == Fault::WrongClass) c = c == kArena ? kWrestlers : c == kWrestlers ? kArena : c;  // plant: the two biggest classes swapped
  return c;
}
// The reader's class: a crowd submitter first (the origin table), then as above.
inline Cls ClassOfDraw(uint32_t object, uint32_t pass_site, bool crowd_submitter) { return crowd_submitter ? kCrowd : ClassOf(object, pass_site); }

// ---- the accumulators ------------------------------------------------------------------------------------------------------------------
struct Totals {  // a plain snapshot; deltas of two snapshots are one window
  uint64_t record[kClasses] = {}, record_n[kClasses] = {}, part[kClasses] = {}, part_n[kClasses] = {};
  uint64_t replay = 0, replay_n = 0, replay_op[kOps] = {}, replay_op_n[kOps] = {};
  uint64_t op_est[kAllOps] = {}, op_n[kAllOps] = {};  // lane 94: per opcode, estimated ticks (net of overhead) and exact record counts
  double ovh_ticks = 0;
  uint64_t reader[kClasses] = {}, reader_n[kClasses] = {};
  double replace_ms[kClasses] = {};
};
struct Live {
  std::atomic<uint64_t> record[kClasses], record_n[kClasses], part[kClasses], part_n[kClasses];
  std::atomic<uint64_t> op_n[kAllOps], op_sn[kAllOps], op_ticks[kAllOps];  // lane 94: exact count, sampled count, sampled ticks
  std::atomic<uint64_t> reader[kClasses], reader_n[kClasses];
  std::atomic<double> replace_ms[kClasses];
};
inline Live g_live;
inline void Add(std::atomic<uint64_t>& a, uint64_t v) { a.fetch_add(v, std::memory_order_relaxed); }
// Per-thread replay accumulators (no atomics on the hot path); flushed into g_live every kFlushEvery records and by Snapshot() for its own thread.
struct ReplayTls {
  uint32_t n[kAllOps] = {}, sn[kAllOps] = {};
  uint64_t ticks[kAllOps] = {};
  uint32_t pending = 0, rng = 0x9E3779B9u;
};
inline thread_local ReplayTls t_replay;
inline constexpr uint32_t kFlushEvery = 8192;
inline void FlushReplayTls() {
  ReplayTls& t = t_replay;
  for (int i = 0; i < kAllOps; ++i) {
    if (t.n[i]) g_live.op_n[i].fetch_add(t.n[i], std::memory_order_relaxed), t.n[i] = 0;
    if (t.sn[i]) g_live.op_sn[i].fetch_add(t.sn[i], std::memory_order_relaxed), g_live.op_ticks[i].fetch_add(t.ticks[i], std::memory_order_relaxed), t.sn[i] = 0, t.ticks[i] = 0;
  }
  t.pending = 0;
}
inline Totals Snapshot() {
  FlushReplayTls();
  Totals t;
  const double ovh = Overhead();
  t.ovh_ticks = ovh;
  const auto ld = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  for (int i = 0; i < kClasses; ++i) {
    t.record_n[i] = ld(g_live.record_n[i]), t.part_n[i] = ld(g_live.part_n[i]), t.reader_n[i] = ld(g_live.reader_n[i]);
    t.record[i] = Net(ld(g_live.record[i]), t.record_n[i], ovh), t.part[i] = Net(ld(g_live.part[i]), t.part_n[i], ovh);
    t.reader[i] = Net(ld(g_live.reader[i]), t.reader_n[i], ovh);
    t.replace_ms[i] = g_live.replace_ms[i].load(std::memory_order_relaxed);
  }
  for (int op = 0; op < kAllOps; ++op) {
    const uint64_t n = ld(g_live.op_n[op]), sn = ld(g_live.op_sn[op]);
    uint64_t est = 0;
    if (sn) est = uint64_t(double(Net(ld(g_live.op_ticks[op]), sn, ovh)) * double(n) / double(sn));
    t.op_n[op] = n, t.op_est[op] = est;
    const int slot = OpSlot(uint32_t(op));
    t.replay_op[slot] += est, t.replay_op_n[slot] += n, t.replay += est, t.replay_n += n;
  }
  return t;
}
inline void ResetForTest() {
  for (int i = 0; i < kClasses; ++i) {
    g_live.record[i] = 0, g_live.record_n[i] = 0, g_live.part[i] = 0, g_live.part_n[i] = 0, g_live.reader[i] = 0, g_live.reader_n[i] = 0;
    g_live.replace_ms[i].store(0.0);
  }
  for (int i = 0; i < kAllOps; ++i) g_live.op_n[i] = 0, g_live.op_sn[i] = 0, g_live.op_ticks[i] = 0;
  t_replay = ReplayTls{};
  g_overhead_ticks = -1.0;
  for (auto& o : g_objects) o.store(0);
}
inline Totals Minus(const Totals& n, const Totals& p) {
  Totals d;
  for (int i = 0; i < kClasses; ++i) {
    d.record[i] = n.record[i] - p.record[i], d.record_n[i] = n.record_n[i] - p.record_n[i], d.part[i] = n.part[i] - p.part[i], d.part_n[i] = n.part_n[i] - p.part_n[i];
    d.reader[i] = n.reader[i] - p.reader[i], d.reader_n[i] = n.reader_n[i] - p.reader_n[i];
    d.replace_ms[i] = n.replace_ms[i] - p.replace_ms[i];
  }
  d.replay = n.replay - p.replay, d.replay_n = n.replay_n - p.replay_n;
  for (int i = 0; i < kOps; ++i) d.replay_op[i] = n.replay_op[i] - p.replay_op[i], d.replay_op_n[i] = n.replay_op_n[i] - p.replay_op_n[i];
  for (int i = 0; i < kAllOps; ++i) d.op_est[i] = n.op_est[i] > p.op_est[i] ? n.op_est[i] - p.op_est[i] : 0, d.op_n[i] = n.op_n[i] - p.op_n[i];
  d.ovh_ticks = n.ovh_ticks;
  return d;
}

// ---- the scopes (each costs one relaxed load when the switch is off) ---------------------------------------------------------------------
inline thread_local int t_span_depth = 0;  // inside a sub_82C04DF0 span on this thread

// sub_82C04DF0 (r3 = the model object): the outermost span of a thread counts; a nested one does not (no double count).
class RecordSpan {
 public:
  explicit RecordSpan(uint32_t object) : object_(object), on_(On()) {
    if (!on_) return;
    outer_ = t_span_depth++ == 0;
    t0_ = Ticks();
  }
  ~RecordSpan() {
    if (!on_) return;
    const uint64_t dt = Ticks() - t0_;
    --t_span_depth;
    if (!outer_) return;
    const Cls c = ClassOf(object_, 0);
    Add(g_live.record[c], dt), Add(g_live.record_n[c], 1);
  }
  RecordSpan(const RecordSpan&) = delete;
  RecordSpan& operator=(const RecordSpan&) = delete;

 private:
  uint32_t object_;
  bool on_, outer_ = false;
  uint64_t t0_ = 0;
};

// sub_82BDED40 (the part draw; r3 = the model object, pass_site = its caller): always counted in "part"; also in "record" when no span covers it.
class PartSpan {
 public:
  PartSpan(uint32_t object, uint32_t pass_site) : object_(object), site_(pass_site), on_(On()) {
    if (on_) t0_ = Ticks();
  }
  ~PartSpan() {
    if (!on_) return;
    const uint64_t dt = Ticks() - t0_;
    const Cls c = ClassOf(object_, site_);
    Add(g_live.part[c], dt), Add(g_live.part_n[c], 1);
    if (t_span_depth == 0) Add(g_live.record[c], dt), Add(g_live.record_n[c], 1);
  }
  PartSpan(const PartSpan&) = delete;
  PartSpan& operator=(const PartSpan&) = delete;

 private:
  uint32_t object_, site_;
  bool on_;
  uint64_t t0_ = 0;
};

// The crowd's block / card record loops (scene_crowd.h): the whole call is the crowd's recording.
class CrowdSpan {
 public:
  CrowdSpan() : on_(On()) {
    if (on_) t0_ = Ticks();
  }
  ~CrowdSpan() {
    if (!on_) return;
    Add(g_live.record[kCrowd], Ticks() - t0_), Add(g_live.record_n[kCrowd], 1);
  }
  CrowdSpan(const CrowdSpan&) = delete;
  CrowdSpan& operator=(const CrowdSpan&) = delete;

 private:
  bool on_;
  uint64_t t0_ = 0;
};

// One replayed record (sub_82BB53D0 dispatch): the outermost call of the thread counts. `op` is read by the caller from the record.
// Every record is counted (thread-local, no atomic); 1 in 2^g_sample_shift is timed (a per-thread xorshift picks it: unbiased, no aliasing with
// a periodic stream). The window's ticks per op = sampled ticks * n / sampled n (Snapshot).
inline thread_local int t_replay_depth = 0;
class ReplaySpan {
 public:
  explicit ReplaySpan(uint32_t op) : on_(On()) {
    if (!on_) return;
    outer_ = t_replay_depth++ == 0;
    if (!outer_) return;
    slot_ = OpIndex(op);
    if (g_fault == Fault::WrongReplayOp) slot_ = (slot_ + 1) % kAllOps;  // plant
    if (g_fault == Fault::SkipOp && slot_ == kSkippedOp) {                // plant: a sampler that never sees one opcode
      outer_ = false;
      return;
    }
    ReplayTls& t = t_replay;
    ++t.n[slot_], ++t.pending;
    uint32_t x = t.rng;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    t.rng = x;
    if ((x & ((1u << g_sample_shift.load(std::memory_order_relaxed)) - 1u)) == 0) {
      sampled_ = true;
      t0_ = Ticks();
    }
  }
  ~ReplaySpan() {
    if (!on_) return;
    --t_replay_depth;
    if (!outer_) return;
    ReplayTls& t = t_replay;
    if (sampled_) {
      const uint64_t dt = Ticks() - t0_;
      ++t.sn[slot_], t.ticks[slot_] += dt;
    }
    if (t.pending >= kFlushEvery) FlushReplayTls();
  }
  ReplaySpan(const ReplaySpan&) = delete;
  ReplaySpan& operator=(const ReplaySpan&) = delete;

 private:
  bool on_, outer_ = false, sampled_ = false;
  int slot_ = 0;
  uint64_t t0_ = 0;
};

// One draw read by our reader: the class is known only after the draw (the origin tag is resolved inside), so the caller sets it.
class ReaderSpan {
 public:
  ReaderSpan() : on_(On()) {
    if (on_) t0_ = Ticks();
  }
  void SetClass(Cls c) { cls_ = c; }
  ~ReaderSpan() {
    if (!on_) return;
    const uint64_t dt = Ticks() - t0_;
    if (g_fault == Fault::ReaderToRecord) Add(g_live.record[cls_], dt);  // plant
    else Add(g_live.reader[cls_], dt);
    Add(g_live.reader_n[cls_], 1);
  }
  ReaderSpan(const ReaderSpan&) = delete;
  ReaderSpan& operator=(const ReaderSpan&) = delete;

 private:
  bool on_;
  Cls cls_ = kOther;
  uint64_t t0_ = 0;
};

// The live thread publishes the binder's cumulative replace-check ms for a hook name ("arena", "wrestlers", "ring", "flash"; anything else -> other).
inline Cls ReplaceClassOf(const char* name) {
  const std::string n = name ? name : "";
  return n == "arena" ? kArena : n == "wrestlers" ? kWrestlers : n == "ring" ? kRing : n == "flash" ? kFlash : kOther;
}
inline void PublishReplaceMs(Cls c, double cumulative_ms) { g_live.replace_ms[c].store(cumulative_ms, std::memory_order_relaxed); }

// ---- the log line ------------------------------------------------------------------------------------------------------------------------
// `d` = one window (or the whole run for the final line), `frames` = guest frames in it. Ticks -> time with `ticks_per_ns`.
inline std::string FormatLine(const char* tag, uint64_t frames, const Totals& d, double ticks_per_ns) {
  const double f = frames ? double(frames) : 1.0;
  const double to_ms = 1.0 / (ticks_per_ns * 1e6);  // ticks -> ms
  const auto per_class = [&](const uint64_t* v, double scale) {
    std::string s = "{";
    char b[64];
    for (int i = 0; i < kClasses; ++i) {
      std::snprintf(b, sizeof b, "%s%s %.3f", i ? " " : "", kClsName[i], double(v[i]) * scale);
      s += b;
    }
    return s + "}";
  };
  char buf[1400];  // one piece at a time
  std::string out = "[cost] ";
  out += tag;
  std::snprintf(buf, sizeof buf, "frames %llu, record ms/f ", static_cast<unsigned long long>(frames));
  out += buf;
  out += per_class(d.record, to_ms / f);
  out += ", part ms/f ";
  out += per_class(d.part, to_ms / f);
  uint64_t reader_all = 0;
  for (int i = 0; i < kClasses; ++i) reader_all += d.reader[i];
  const double replay_ms = double(d.replay) * to_ms / f, reader_ms = double(reader_all) * to_ms / f;
  std::snprintf(buf, sizeof buf, ", replay ms/f %.3f (%.1f rec/f, excl reader %.3f; ", replay_ms, double(d.replay_n) / f, replay_ms - reader_ms);
  out += buf;
  for (int i = 0; i < kOps; ++i) {
    std::snprintf(buf, sizeof buf, "%s%s %.3f/%.1f", i ? " " : "", OpName(i), double(d.replay_op[i]) * to_ms / f, double(d.replay_op_n[i]) / f);
    out += buf;
  }
  out += "), reader us/draw {";
  for (int i = 0; i < kClasses; ++i) {
    const double us = d.reader_n[i] ? double(d.reader[i]) / double(d.reader_n[i]) / ticks_per_ns / 1e3 : 0.0;
    std::snprintf(buf, sizeof buf, "%s%s %.2f", i ? " " : "", kClsName[i], us);
    out += buf;
  }
  out += "} (draws/f {";
  for (int i = 0; i < kClasses; ++i) {
    std::snprintf(buf, sizeof buf, "%s%s %.1f", i ? " " : "", kClsName[i], double(d.reader_n[i]) / f);
    out += buf;
  }
  out += "}), replace ms/f {";
  for (int i = 0; i < kClasses; ++i) {
    std::snprintf(buf, sizeof buf, "%s%s %.3f", i ? " " : "", kClsName[i], d.replace_ms[i] / f);
    out += buf;
  }
  out += "}";
  // Lane 94, appended after the old fields: the instrument's own cost, the top 12 replay opcodes, the S50 groups.
  std::snprintf(buf, sizeof buf, ", ovh ns/span %.1f (subtracted; replay timed 1/%d), replay top ops ms/f/count/f {", d.ovh_ticks / ticks_per_ns, 1 << g_sample_shift.load());
  out += buf;
  int order[kAllOps];
  for (int i = 0; i < kAllOps; ++i) order[i] = i;
  std::stable_sort(order, order + kAllOps, [&](int a, int b) { return d.op_est[a] > d.op_est[b]; });
  for (int k = 0; k < 12 && d.op_n[order[k]]; ++k) {
    std::snprintf(buf, sizeof buf, "%sop%d %.3f/%.1f", k ? " " : "", order[k], double(d.op_est[order[k]]) * to_ms / f, double(d.op_n[order[k]]) / f);
    out += buf;
  }
  out += "}, replay groups ms/f/count/f {";
  uint64_t gt[kGroups] = {}, gn[kGroups] = {};
  for (int i = 0; i < kAllOps; ++i) gt[GroupOf(i)] += d.op_est[i], gn[GroupOf(i)] += d.op_n[i];
  for (int g = 0; g < kGroups; ++g) {
    std::snprintf(buf, sizeof buf, "%s%s %.3f/%.1f", g ? " " : "", kGrpName[g], double(gt[g]) * to_ms / f, double(gn[g]) / f);
    out += buf;
  }
  out += "}";
  return out;
}

// The periodic + final printer. Update() runs once a guest frame on the render thread (reader.h, where the switch is applied); the final line
// is printed by the live thread's teardown (FinalLine). One line every 600 frames covers the frames since the last one.
inline constexpr uint64_t kWindowFrames = 600;
struct Printer {
  bool started = false;
  Totals last;
  uint64_t start_frame = 0, last_frame = 0;
};
inline Printer g_printer;
inline std::atomic<uint64_t> g_latest_frame{0};
// Returns the line to log ("" = none yet). `on` = the cvar's value this frame.
inline std::string Update(bool on, uint64_t frame, double ticks_per_ns) {
  g_on.store(on, std::memory_order_relaxed);
  if (!on) return "";
  Printer& p = g_printer;
  g_latest_frame.store(frame, std::memory_order_relaxed);
  if (!p.started) {
    p.started = true, p.start_frame = p.last_frame = frame;
    return "";
  }
  if (frame < p.last_frame + kWindowFrames) return "";
  const Totals now = Snapshot();
  std::string line = FormatLine("", frame - p.last_frame, Minus(now, p.last), ticks_per_ns);
  p.last = now, p.last_frame = frame;
  return line;
}
// The whole run since the switch came on ("" = never on).
inline std::string FinalLine(double ticks_per_ns) {
  const Printer& p = g_printer;
  if (!p.started) return "";
  const uint64_t frame = g_latest_frame.load(std::memory_order_relaxed);
  return FormatLine("final: ", frame - p.start_frame, Snapshot(), ticks_per_ns);
}
inline void ResetPrinterForTest() {
  g_printer = Printer{};
  g_latest_frame = 0;
}

}  // namespace wwe2k14::cost
