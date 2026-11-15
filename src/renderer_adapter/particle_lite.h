// S102 lane T18 (wwe2k14_rdr_particle_lite, PerFrame, default off): a lite record path for the game's particle draws, on the swap thread (the game's render-command thread).
// Docs: docs\renderer\PARTICLE_LITE.md. Source of the problem: docs\research\part2\R15_brock_block.md (Brock's pyro burst: up to 2,750 particle draws a frame, each its own op-19
// draw with its own constant bank; the burst adds +5 ms to the swap interval on the Surface and +0.2 ms of GPU) and R13 section 3.2 (the reader's per-draw record).
//
// What a RUN is here. Two particle draws are one run when the second draw is recorded right after the first (no other op, no other draw between them in the frame tables), at
// the same recorder call site, with the same program pair, vertex layout, primitive, strides, shader objects and declaration, and nothing the copied records are derived
// from moved. With the C7 marks clear (a.unmarked) that is taken from the marks; with them SET (live: the game's recorder raises one on nearly every particle draw, PC run 7245: 29,174 of 29,704
// candidates) the state words are gathered and compared with the state cache's directly (the by-marks path's own gather) and the slots by the snapshot. Marks decide nothing alone. Between two such draws the only things that differ are the per-particle
// constants (the bank delta: c228-c231, c200 yzw) and the inline vertices (the draw's own geometry). Everything else the reader derives in DrawCommon (the fixed and dynamic
// state records, the texture slots, the vertex layout) is a function of facts that did not move, so it is COPIED from the run's previous draw instead of derived again:
//   fixed, dynamic            the previous draw's table indices (what the state cache's by-marks path hands the draw today)
//   texture slots             the previous draw's slot records re-appended to the slot table (the table stays byte-equal to today's: one record per slot per draw)
//   layout                    the previous draw's layout index (same vertex shader)
//   the thin-lite query       skipped when the previous draw of the run returned before its Query (a function of the origin's key, which is in the run's key)
//   the origin record         the previous draw's (TakeDrawOriginInto is a function of the origin key and the static class tables; its census counters are bumped the same)
// What stays per draw, always: the shader lookups, the target emit, PushBanks (the bank delta), the bool / loop compare, BuildGeometry (the inline vertices).
// The recorded frame is byte-equal to the one the full path records (the proof: tools\particle_lite\, gate lane s102_t18); only the work to get there differs.
//
// Pure logic: no game or SDK includes (so tools\particle_lite\particle_lite_test.cpp compiles it against a model of the reader). The reader's DrawCommon holds the wiring,
// marked "S102 T18".
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace rdr_adapter::particle_lite {

// The measured particle pair (yParticle, hard variant) and the two recorder call sites: the same numbers as particle_runs.h (the gate's wiring check compares them).
inline constexpr uint64_t kVs = 0x5F1EF76E69256C9Cull, kPs = 0x34B29856453FE506ull;
inline constexpr uint32_t kSpriteSite = 0x82A0DA58u, kQuadSite = 0x82A0E338u;

// Read once a frame at the present (rdr_sw::BoolSeen); the hot path reads this atomic.
inline std::atomic<bool> g_on{false};

// F2 / C7 hole: one count per draw ENTRY (each ReadMarks call: OnDrawIndexed and the inline Begin), recorded draw or not. A draw that enters a hook and returns early records nothing
// (DrawCommon's early outs, OnEndVertices' inline_draw_empty) but its setters' marks are cleared by the null draw, so the next draw sees "no marks" over a changed device. The gate and
// the state cache each store the count of the draw they were filled from and need exactly one entry since (this draw's own).
inline std::atomic<uint64_t> g_entries{0};
inline uint64_t Entries() { return g_entries.load(std::memory_order_relaxed); }
inline void NoteEntry() { g_entries.fetch_add(1, std::memory_order_relaxed); }
// S102 T18 (C7 hole): the state cache's by_marks needs exactly one draw entry since the draw it was filled from (this draw's own). A dropped draw in between (marks cleared, nothing recorded) fails it.
#ifdef T18_PLANTS
inline bool g_plant_c7_no_entry = false;  // plant 10: the counter is not consulted
#endif
inline bool StateEntryOk(uint64_t cache_entry, uint64_t entries_now) {
#ifdef T18_PLANTS
  if (g_plant_c7_no_entry) return true;
#endif
  return cache_entry + 1 == entries_now;
}

// What the gate decided for a candidate draw (a hard-yParticle draw at a particle site, inline).
enum class Reason : uint8_t {
  Taken = 0,
  FirstOfRun,  // no previous candidate this frame part, or a new frame part: the full path, remembered
  Marked,      // the C7 marks are set AND the state words the gather reads differ from the state cache's (the live recorder raises a mark on nearly every particle draw: the marks alone decide nothing)
  KeyMoved,    // shader, declaration, strides, primitive, call site, pass site, object or the shader pointers differ from the previous draw's
  Gap,         // another op or draw was recorded since the previous candidate (a pass, a copy, a clear, another draw)
  Excluded,    // a mode that changes the draw's records after the state is read: recording off, wide, jitter, trace
  SlotsMoved,  // the slot snapshot differs (F1: the texture slot dirty bits are in the LOW word of dev+24, which the marks do not read)
  Count
};
inline const char* ReasonName(Reason r) {
  static const char* n[] = {"taken", "first of a run", "marked", "key moved", "gap", "excluded", "slots moved"};
  return uint8_t(r) < uint8_t(Reason::Count) ? n[uint8_t(r)] : "?";
}

// What a draw must have in common with the previous one of its run (everything the lite path copies depends on exactly these).
struct Key {
  uint32_t op = 0, site = 0, pass_site = 0, object = 0, part = 0, widget = 0, text = 0, run = 0;  // (op: S102 SW2 review F3, DrawOrigin.op = k.op) the draw's whole origin key (the thin-lite query and the draw's origin record are functions of it)
  uint32_t vs_object = 0, ps_object = 0, decl = 0, prim = 0;
  bool origin_ok = true;  // S102 SW2 (general gate): the draw's origin tag resolved; a draw without one never shares an origin record
  const void* vs = nullptr;  // the reader's shader infos (stable pointers; the layout and the texture facts hang off them)
  const void* ps = nullptr;
  uint8_t strides[24] = {};
  bool Equal(const Key& o) const {
    return site == o.site && pass_site == o.pass_site && object == o.object && part == o.part && widget == o.widget && text == o.text && run == o.run && vs_object == o.vs_object && ps_object == o.ps_object && decl == o.decl && prim == o.prim && vs == o.vs &&
           ps == o.ps && std::memcmp(strides, o.strides, sizeof(strides)) == 0;
  }
};

// F1: what the lite path's copied slot records are a function of, per slot the previous draw used (the same inputs AddTextureSlot's SlotKey keys on, plus the image's flags).
// Built at Remember from the draw's own slot list and again at Decide from that list with the live state: any difference declines.
struct SlotSnap {  // (no member initialisers: a snapshot is built on the candidate path only, and only the first n entries are read)
  uint8_t slot;
  uint8_t flags;  // image: packed, copy_dest, swap, target16
  uint32_t object;
  uint32_t image;
  uint32_t w[6];
  uint8_t header[24];  // the texture object's header bytes (object + 28), when object != 0
  uint8_t pack_host[4];
  bool Equal(const SlotSnap& o) const {
    return slot == o.slot && flags == o.flags && object == o.object && image == o.image && std::memcmp(w, o.w, sizeof(w)) == 0 && (!object || std::memcmp(header, o.header, sizeof(header)) == 0) &&
           std::memcmp(pack_host, o.pack_host, sizeof(pack_host)) == 0;
  }
};
struct SlotsSnap {
  uint64_t epoch;  // g_reg.epoch
  uint32_t n;
  SlotSnap s[32];
  bool Equal(const SlotsSnap& o) const {
    if (epoch != o.epoch || n != o.n) return false;
    for (uint32_t i = 0; i < n; ++i)
      if (!s[i].Equal(o.s[i])) return false;
    return true;
  }
};

struct Census {
  uint64_t frames = 0, candidates = 0, taken = 0, runs = 0, window_start = 0, misses = 0, marked_equal = 0, origin_copied = 0;  // origin_copied (general gate): a taken draw whose origin record was copied too  // marked_equal: a taken draw whose marks were set and whose state words compared equal  // misses: marks-verify only (F5): a taken draw whose full record differs from what the lite path would have copied
  uint64_t by[uint8_t(Reason::Count)] = {};
  bool in_run = false;  // the last decision was Taken (a run is counted at its first Taken)
};
inline Census g_census;
inline Census g_census_gen;  // S102 SW2: the general gate's census
// S102 SW2 (wwe2k14_rdr_draw_lite bit 1, docs\renderer\DRAW_LITE.md): the same gate for EVERY draw family. The fixed / dynamic state records, the texture slot records and the layout are functions of the
// shader pair, the primitive and the device state, not of the draw's origin: a general draw is the next of a run when its shader objects / infos, declaration, primitive and strides equal the previous
// recorded draw's and nothing moved (the rules below, unchanged). The origin key is a separate question (origin_same): the origin record and the thin query's skip are copied only when it is equal too.
inline std::atomic<bool> g_on_gen{false};

// The previous candidate draw of this frame part. Lives on the reader's recording thread under the reader's lock (every draw hook runs under it).
struct Gate {
  bool valid = false;
  uint64_t generation = 0;
  uint64_t draw_index = 0;  // t.draws index of the previous candidate
  uint64_t ops_after = 0;   // t.ops.size() right after it was recorded
  Key key;
  bool thin_noquery = false;  // its thin-lite query returned before Query (nothing was counted)
  uint64_t entries_after = 0;  // g_entries right after it (its own entry included)
  bool last_excluded = false;  // Decide's last answer was Excluded: Remember then leaves the gate invalid (F4: an excluded draw ends the run)
  SlotsSnap snap;              // the slots its record was made from
  bool general = false;        // S102 SW2: the general gate: the key does not compare the origin fields (origin_same says whether they are equal)
  bool origin_same = false;    // S102 SW2: set by Decide when the key is equal (general gate): the draw's origin key equals the previous draw's
  Census* census = &g_census;  // S102 SW2: the gate's own census (the general gate counts apart from the particle gate)

  // Planted twins (T18_PLANTS builds of the test only): each must fail its check. Compiled out of the game.
#ifdef T18_PLANTS
  int plant = 0;  // 1 marks ignored, 2 key ignores the shader, 3 gap ignored, 6 the slot snapshot ignored, 7 thin skipped after a draw that DID query, 8 draw entries ignored, 9 snapshot ignores the header part
#endif

  // slots_same: called only when every cheaper test passed; builds the live snapshot of the previous draw's slots and compares (the reader's PlSlotsSame).
  // state_same: called only when the marks are set (not `unmarked`) and every cheaper test passed: the state words the full path would gather, against the state cache's.
  template <class S, class F>
  Reason Decide(const Key& k, bool unmarked, uint64_t generation_now, uint64_t draws_size, uint64_t ops_size, bool excluded, uint64_t entries_now, S&& state_same, F&& slots_same) {
    Census& c = *census;
    ++c.candidates;
    origin_same = false;
    Reason r;
    last_excluded = excluded;
    if (excluded) r = Reason::Excluded;
    else if (!valid || generation != generation_now) r = Reason::FirstOfRun;
    else if ((ops_size != ops_after || draws_size != draw_index + 1)
#ifdef T18_PLANTS
             && plant != 3
#endif
    ) r = Reason::Gap;
    else if (entries_now != entries_after + 1
#ifdef T18_PLANTS
             && plant != 8
#endif
    ) r = Reason::Gap;  // a draw entered a hook and recorded nothing in between
    else if (!KeyEqual(k)) r = Reason::KeyMoved;
    else if (general && (origin_same = OriginEqual(key, k), false)) r = Reason::KeyMoved;  // (never taken: records whether the origin key is the previous draw's)
    else if (!unmarked
#ifdef T18_PLANTS
             && plant != 1
#endif
             && !state_same()) r = Reason::Marked;
    else if (
#ifdef T18_PLANTS
        plant != 6 &&
#endif
        !slots_same()) r = Reason::SlotsMoved;
    else r = Reason::Taken;
    ++c.by[uint8_t(r)];
    if (r == Reason::Taken) {
      ++c.taken;
      if (!unmarked) ++c.marked_equal;
      if (!c.in_run) ++c.runs;
      c.in_run = true;
    } else {
      c.in_run = false;
    }
    return r;
  }
  // The thin-lite query may be skipped only when the previous draw of the run returned before Query (its answer is a function of the key).
  bool SkipThin() const {
#ifdef T18_PLANTS
    if (plant == 7) return true;
#endif
    return thin_noquery;
  }
  static bool OriginEqual(const Key& a, const Key& b) {
    return a.origin_ok && b.origin_ok && a.op == b.op && a.site == b.site && a.pass_site == b.pass_site && a.object == b.object && a.part == b.part && a.widget == b.widget && a.text == b.text && a.run == b.run;
  }
  // The general gate's key: everything the copied state records are a function of, minus the origin.
  static bool StateKeyEqual(const Key& a, const Key& b) {
    return a.vs_object == b.vs_object && a.ps_object == b.ps_object && a.decl == b.decl && a.prim == b.prim && a.vs == b.vs && a.ps == b.ps && std::memcmp(a.strides, b.strides, sizeof(a.strides)) == 0;
  }
  bool KeyEqual(const Key& k) const {
    if (general) {
#ifdef T18_PLANTS
      if (plant == 2) return key.prim == k.prim;  // the shader pair is not compared
#endif
      return StateKeyEqual(key, k);
    }
#ifdef T18_PLANTS
    if (plant == 2) {  // the shader pair is not compared
      Key a = key, b = k;
      a.vs = b.vs = a.ps = b.ps = nullptr;
      a.vs_object = b.vs_object = a.ps_object = b.ps_object = 0;
      return a.Equal(b);
    }
#endif
    return key.Equal(k);
  }
  // After a TAKEN draw: Decide just proved the slot snapshot equal to the live state, which is what this draw's record was made from, so the snapshot is kept as it is (nothing rebuilt, nothing copied).
  // k is this draw's key: the general gate keeps it when the origin moved (the next draw compares against this draw's origin).
  void RememberTaken(const Key& k, uint64_t draw_index_now, uint64_t ops_size, bool thin_noquery_now, uint64_t entries_now) {
    valid = !last_excluded;
    last_excluded = false;
    entries_after = entries_now;
    draw_index = draw_index_now;
    ops_after = ops_size;
    if (general && !origin_same) key = k;
    thin_noquery = thin_noquery_now;
  }
  // After the draw is in the frame tables (a candidate, taken or not).
  void Remember(const Key& k, uint64_t generation_now, uint64_t draw_index_now, uint64_t ops_size, bool thin_noquery_now, uint64_t entries_now, const SlotsSnap& snap_now) {
    valid = !last_excluded;  // F4
    last_excluded = false;
    entries_after = entries_now;
    snap.epoch = snap_now.epoch, snap.n = snap_now.n;
    for (uint32_t i = 0; i < snap_now.n; ++i) snap.s[i] = snap_now.s[i];
    generation = generation_now;
    draw_index = draw_index_now;
    ops_after = ops_size;
    key = k;
    thin_noquery = thin_noquery_now;
  }
  void Reset() { valid = false; }
};
inline Gate g_gate;
inline Gate g_gate_gen;  // S102 SW2: the general gate (general = true, census = &g_census_gen are set at the present, reader.h)

// Once per guest frame at the present: counts the frames the lever was on and every 600 frames logs
//   [rdr][particle_lite] frames N, taken T draws, runs R, declined D (first of a run F, marked M, key moved K, gap G, excluded E, slots moved S), marked but equal Q, particle_lite_miss X
// A Surface "no gain" is told apart from "took 0 draws" by this line (S102 rule: every lever logs a live census line).
using LogFn = void (*)(const char*);
inline void LogCensusOf(Census& c, const std::atomic<bool>& on, const char* name, uint64_t frame, LogFn log) {  // S102 SW2: one body for both gates (name = particle_lite | draw_lite)
  if (on.load(std::memory_order_relaxed)) ++c.frames;
  if (!c.window_start) c.window_start = frame;
  if (frame < c.window_start + 600) return;
  c.window_start = frame;
  if (c.frames && log) {
    char line[520];
    const uint64_t declined = c.candidates - c.taken;
    std::snprintf(line, sizeof(line), "[rdr][%s] frames %llu, taken %llu draws, runs %llu, declined %llu (first of a run %llu, marked %llu, key moved %llu, gap %llu, excluded %llu, slots moved %llu), marked but equal %llu, origin copied %llu, %s_miss %llu",
                  name, (unsigned long long)c.frames, (unsigned long long)c.taken, (unsigned long long)c.runs, (unsigned long long)declined,
                  (unsigned long long)c.by[uint8_t(Reason::FirstOfRun)], (unsigned long long)c.by[uint8_t(Reason::Marked)], (unsigned long long)c.by[uint8_t(Reason::KeyMoved)],
                  (unsigned long long)c.by[uint8_t(Reason::Gap)], (unsigned long long)c.by[uint8_t(Reason::Excluded)], (unsigned long long)c.by[uint8_t(Reason::SlotsMoved)],
                  (unsigned long long)c.marked_equal, (unsigned long long)c.origin_copied, name, (unsigned long long)c.misses);
    log(line);
  }
  const bool in_run = c.in_run;
  c = Census{};
  c.window_start = frame;
  c.in_run = in_run;
}
inline void LogCensus(uint64_t frame, LogFn log) { LogCensusOf(g_census, g_on, "particle_lite", frame, log); }
inline void LogCensusGeneral(uint64_t frame, LogFn log) { LogCensusOf(g_census_gen, g_on_gen, "draw_lite", frame, log); }

}  // namespace rdr_adapter::particle_lite
