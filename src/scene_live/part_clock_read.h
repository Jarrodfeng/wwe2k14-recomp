// The arena's UV-scroll clock (VS c186 `Time`), read at the latch (Session 103 lane E5; docs\renderer\LIVE_MAPS.md "Lane E5"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only; NO game, SDK or renderer-adapter includes (it uses stage_fx_core.h's ReadFn / ReadWord / ReadSlotModel): the contributor (stage_fx_contrib.h) and the offline unit (tools\scene\e5_unit.cpp)
// run THESE functions. Marks: [C] read in a listing (cited), [L] a reading, [U] unknown.
//
// Where c186 comes from [C listings]:
//   * The colour pass sub_82C04DF0 records the parameter whose descriptor is [model+772] (the string "g_fTime", 0x82069DDC; sub_82BDB460 0x82BDCCF8 builds the descriptor) with the value
//     `lfs f0,12(r26)` at 0x82C09820, where r26 = [model+484] + 24 * r16 (0x82C07938-0x82C07954: r16 = the part index, `rlwinm r11,r16,1,0,30; add r8,r16,r11; rlwinm r11,r8,3,0,28` = 24 r16).
//     So c186 = the part's TIME word, the float at [model+484] + 24 part + 12. (The cache-replay route reads the same value out of the cached block [[model+832]+1124], refreshed from the same words by
//     sub_82BD7B50; the direct route above is what a latch reads.)
//   * The writer is sub_82BD5A98 (listing 0x82BD5A98-0x82BD5C18; db: a model vtable method; data words 0x8206AFF8 / 0x8206B510 / 0x82021748 / 0x82021850 / 0x82021960 / 0x82210C00 = slot 26 of the
//     model vtables). It is called from the model UPDATE sub_82BD2010 (vtable slot 24, listing 0x82BD203C: `lwz r8,104(r9); mtctr; bctrl` right after slot 25 and before sub_82BD40C8; q: sub_82BD2010 ran in
//     match_load, entrances, in_ring, pause_menu, replay ... and has the vtable data words 0x82021740 / 0x8206AFF0 / 0x8206B508). For every part p < [model+372]: t = [part + 12] + [model+248];
//     if t > [part + 16] then t -= [part + 16]; [part + 12] = t. (The routine unrolls 4 parts a turn, stride 96, and finishes the rest at stride 24; it returns at once when [model+484] == 0 or the
//     scene camera table [0x83929A68] has no camera, 0x82BD5AAC-0x82BD5AC8.) So the clock is STATE that advances once per update call: it is not `frame - setup frame` (J1: d3_record.h, d3_arena.h),
//     it differs per model by [model+248] (the model constructor sets it to 1.0, 0x82BD3A20; the beam cones' 2x is a different step or period), and it wraps at the part's period.
//   * Update-side, not draw-side: the update task is not under any of the 15 skip hooks (scene_skip_nodes.h) [L: the hooks wrap draw submissions, the ring task and the plane / pre-light modules;
//     sub_82BD2010 is none of them]. UNCONFIRMED on a live OURS frame: the trace string for that is in the lane's hand-back (`0x82BD5A98*r3*f1` on a coverage build, mode 2).
// A model whose parts all hold the same (time, period) gets ONE row; a model with several clocks gets one row per run of equal parts (rows ascend by `part`).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "stage_fx_core.h"
#include "../renderer_scene/rows_part_clock.h"   // S106 G4-2

namespace rdr_scene::live::stagefx {

inline constexpr uint32_t kModelPartCount = 372, kModelPartData = 484, kModelClockStep = 248;
inline constexpr uint32_t kPartStride = 24, kPartTimeOff = 12, kPartPeriodOff = 16;
inline constexpr uint32_t kMaxClockParts = 1024;   // a model with more parts than this is refused (the arena's models have a handful)

// Test-only planted faults (e5_unit): 0 none, 1 = period_as_time (the word at +16 read), 2 = stride16 (parts 16 bytes apart), 3 = one_run (every part of a model folded into the first row).
inline int g_clock_plant = 0;

enum class ClockWhy : uint32_t { Ok = 0, NoModel, Unreadable, NoParts, Absurd, NonFinite };

struct ClockCensus {
  uint64_t models = 0, rows = 0, no_model = 0, unreadable = 0, no_parts = 0, absurd = 0, non_finite = 0, duplicates = 0, overflow = 0;
};

// The reader's reusable buffers (the contributor owns one: nothing is allocated per take once they have grown).
struct ClockScratch {
  std::vector<uint32_t> part_words;   // one model's part array, raw big-endian words
  std::vector<uint32_t> slot_words;   // the stage's 399 model words, raw
  struct Seen { uint32_t model; size_t first, count; bool ok; };
  std::vector<Seen> seen;             // a model already read this take: its rows, to copy for another slot
  ClockScratch() { part_words.reserve(kMaxClockParts * (kPartStride / 4)), slot_words.reserve(kSlotCount), seen.reserve(kSlotCount); }
};

// Appends the clock rows of one model (nothing on a refusal). `frame` = the hook's guest frame.
inline ClockWhy ReadModelClocks(ReadFn read, uint32_t model, uint32_t frame, std::vector<PartClock>& rows, uint32_t slot, ClockScratch& sc) {
  if (model < 0x10000) return ClockWhy::NoModel;
  uint32_t count = 0, base = 0, step_raw = 0;
  if (!ReadWord(read, model + kModelPartCount, count) || !ReadWord(read, model + kModelPartData, base) || !read(model + kModelClockStep, &step_raw, 4)) return ClockWhy::Unreadable;
  if (base < 0x10000 || count == 0) return ClockWhy::NoParts;   // the update returns at once on a null array: nothing advances, nothing to read
  if (count > kMaxClockParts) return ClockWhy::Absurd;
  sc.part_words.resize(size_t(count) * (kPartStride / 4));
  if (!read(base, sc.part_words.data(), size_t(count) * kPartStride)) return ClockWhy::Unreadable;
  const uint32_t* raw = sc.part_words.data();
  auto fl = [](uint32_t be) { const uint32_t w = Swap32(be); float v; std::memcpy(&v, &w, 4); return v; };
  const float step = fl(step_raw);
  if (!std::isfinite(step)) return ClockWhy::NonFinite;
  const size_t first = rows.size();
  float last_t = 0.f, last_p = 0.f;
  for (uint32_t p = 0; p < count; ++p) {
    const uint32_t* w = raw + p * (kPartStride / 4);
    const uint32_t* tw = g_clock_plant == 2 ? raw + p * 4 : w;   // plant: parts 16 bytes apart
    const float t = fl(tw[(g_clock_plant == 1 ? kPartPeriodOff : kPartTimeOff) / 4]), per = fl(tw[kPartPeriodOff / 4]);
    if (!std::isfinite(t) || !std::isfinite(per)) {
      rows.resize(first);
      return ClockWhy::NonFinite;
    }
    if (p != 0 && (g_clock_plant == 3 || (std::memcmp(&t, &last_t, 4) == 0 && std::memcmp(&per, &last_p, 4) == 0))) continue;   // the same clock as the part before: one row covers the run
    PartClock r;
    r.frame = frame, r.object = model, r.slot = slot, r.part = p, r.time = t, r.period = per, r.step = step;
    rows.push_back(r);
    last_t = t, last_p = per;
  }
  return ClockWhy::Ok;
}

// The rows of a whole stage: every model of the stage array (slots 0..398), ONE ROW SET PER SLOT (a model in two slots is read once and its rows are copied with each slot number: the front end joins by
// slot). `cap` bounds the rows of the frame; a slot past it is counted `overflow`, never a partial set. One bulk read of the 399 slot words.
inline void ReadStageClocks(ReadFn read, uint32_t stage, uint32_t frame, std::vector<PartClock>& rows, ClockCensus& c, ClockScratch& sc, size_t cap = 1024) {
  if (stage == 0) return;
  sc.slot_words.resize(kSlotCount);
  if (!read || !read(stage + kStageModels, sc.slot_words.data(), kSlotCount * 4)) {
    ++c.unreadable;
    return;
  }
  sc.seen.clear();
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    const uint32_t model = Swap32(sc.slot_words[slot]);
    if (model < 0x10000) continue;   // an empty slot
    const size_t before = rows.size();
    const ClockScratch::Seen* known = nullptr;
    for (const ClockScratch::Seen& s : sc.seen)
      if (s.model == model) known = &s;
    ClockWhy why = ClockWhy::Ok;
    if (known) {
      ++c.duplicates;
      if (!known->ok) continue;   // its refusal was counted when it was first read
      for (size_t i = 0; i < known->count; ++i) {
        PartClock r = rows[known->first + i];
        r.slot = slot;
        rows.push_back(r);
      }
    } else {
      ++c.models;
      why = ReadModelClocks(read, model, frame, rows, slot, sc);
      sc.seen.push_back({model, before, rows.size() - before, why == ClockWhy::Ok});
      switch (why) {
        case ClockWhy::Ok: break;
        case ClockWhy::NoModel: ++c.no_model; break;
        case ClockWhy::Unreadable: ++c.unreadable; break;
        case ClockWhy::NoParts: ++c.no_parts; break;
        case ClockWhy::Absurd: ++c.absurd; break;
        case ClockWhy::NonFinite: ++c.non_finite; break;
      }
    }
    if (why == ClockWhy::Ok && rows.size() > cap) {
      rows.resize(before);
      ++c.overflow;
      if (!sc.seen.empty() && sc.seen.back().model == model) sc.seen.back().ok = false;
    } else {
      c.rows += rows.size() - before;
    }
  }
}

// The clock for (object, part): the row of that object with the largest `part` <= the asked one; false when the object has no row.
inline bool ClockOf(const PartClock* rows, size_t n, uint32_t object, uint32_t part, float& out) {
  bool found = false;
  uint32_t best = 0;
  for (size_t i = 0; i < n; ++i) {
    if (rows[i].object != object || rows[i].part > part) continue;
    if (!found || rows[i].part >= best) found = true, best = rows[i].part, out = rows[i].time;
  }
  return found;
}

}  // namespace rdr_scene::live::stagefx
