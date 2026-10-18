// wwe2k14 - Session 27.5 item A: Cena's title "flying" at 60 fps entrances (docs/research/cena_title_s275.md).
//
// What it is (live traces, runs 1993-2008): the title in Cena's hand is its own skeleton (36 nodes) driven by its own
// motion clip. Node 0 (the plate's pivot, relative to his hand) has a translation track with a key on every 1/60 s
// frame (16-bit deltas, sampler sub_823D9608). Its even keys are smooth; its odd keys jump around by up to ~15 units
// (cm) early in the walk-out. The console's 30 fps entrance advances the motion frame by 2 per update, so it only
// ever samples even frames and never shows the odd keys; the 60 fps entrance (and any step rate above 30) samples
// them, so the plate swings out of his hand on every other frame. Probably a period-2 jitter baked into the data
// (unconfirmed why); the decoder reads the stored keys correctly.
//
// --wwe2k14_fix_secondary_motion=true wraps the two translation track samplers, sub_823D91C8 (byte deltas) and
// sub_823D9608 (16-bit deltas; the title's node 0 uses this one), both called from sub_823D9A40 with r3 = motion
// player, r4 = out vec4, r5 = key cursor {u16 index, u16 time}, r6 = track, r7 = node, f1 = frame, and a cache of the
// position at the cursor at [player+140] + node*16. For a frame that is not an even integer it also samples the two
// even frames around it (a private copy of the cursor, the cache slot put back after each call, as the game's own
// root-motion path in sub_823D9A40 does), and if the stored value is more than --wwe2k14_fix_secondary_motion_min
// units from the segment between them, and farther from it than --wwe2k14_fix_secondary_motion_ratio (0.25) of the
// segment's length, it returns the point of that segment for its time instead (a cut between two even frames leaves
// the odd sample at one end of the segment, and real motion stays close to it: both are kept). Only where the
// console shows even frames only
// (ConsoleShowsEvenFramesOnly: our 60 fps entrances, or a 30 fps scene rescaled by --wwe2k14_step_hz); matches,
// which the console runs at 60, are never touched. Even frames are never changed (the console's 30 fps entrance is
// bit-identical with the fix on). DEFAULT ON since 2026-10-18 (Jarrod, after the slow-motion walk-out comparison);
// =false restores the game's own odd-frame keys.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "decode_probe.h"  // GuestPtr
#include "frame_stats.h"   // g_guest_frame
#include "pacing_hooks.h"  // wwe2k14_entrance_60fps
#include "step_rate.h"     // g_entrances, g_game_word, g_applied_hz

REXCVAR_DEFINE_BOOL(wwe2k14_fix_secondary_motion, true, "wwe2k14",
                    "Session 27.5: odd-frame keys of motion translation tracks (Cena's title at 60 fps) that stray "
                    "from their even neighbours are replaced by the line between those (even frames, i.e. the 30 fps "
                    "entrances, are never changed). Default on (Jarrod, 2026-10-18). See src/motion_fix.h");
REXCVAR_DEFINE_DOUBLE(wwe2k14_fix_secondary_motion_min, 0.5, "wwe2k14",
                      "Session 27.5: with wwe2k14_fix_secondary_motion, replace a sample only if it is more than this "
                      "many units (cm) from the line between the even frames around it");
REXCVAR_DEFINE_DOUBLE(wwe2k14_fix_secondary_motion_ratio, 0.25, "wwe2k14",
                      "Session 27.5: with wwe2k14_fix_secondary_motion, replace a sample only if it is also farther from "
                      "the segment than this fraction of the segment's length (real motion turning up to ~110 degrees "
                      "in 1/30 s stays below 0.25)");
REXCVAR_DEFINE_BOOL(wwe2k14_fix_secondary_motion_log, false, "wwe2k14",
                    "Session 27.5: log what wwe2k14_fix_secondary_motion did every 300 guest frames (samples checked, "
                    "replaced, the five largest strays and where; a period is logged by the first check after it ends)");

REX_EXTERN(__imp__sub_823D91C8);
REX_EXTERN(__imp__sub_823D9608);

namespace wwe2k14::motion_fix {

inline uint32_t U32(uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, decode_probe::GuestPtr(base, a), 4);
  return std::byteswap(be);
}
inline float F32(uint8_t* base, uint32_t a) { return std::bit_cast<float>(U32(base, a)); }
inline void SetF32(uint8_t* base, uint32_t a, float v) {
  const uint32_t be = std::byteswap(std::bit_cast<uint32_t>(v));
  std::memcpy(decode_probe::GuestPtr(base, a), &be, 4);
}

// The log's top list: the largest stray per (player, node) this period. A player is fingerprinted by its node count
// (+156) and motion length (max frame, +64), since the address alone says little (the title: 36 nodes, 562).
struct Top {
  uint32_t player = 0, node = 0, nodes = 0;
  float dev = 0.0f, frame = 0.0f, length = 0.0f;
  float sample[3] = {}, e0[3] = {}, e1[3] = {};  // the stored value and the two even samples around it
};
struct Stats {
  std::atomic<uint64_t> checked{0}, replaced{0};
  std::atomic<uint32_t> floor_bits{0};  // float bits of the smallest stray in a full top list (cheap pre-check)
  std::mutex mutex;
  Top top[5];
  std::atomic<uint64_t> last_log_frame{0};
};
inline Stats g_stats;

inline void NoteStray(uint8_t* base, uint32_t player, uint32_t node, float dev, float frame, const float* sample,
                      const float* e0, const float* e1) {
  if (!(dev > std::bit_cast<float>(g_stats.floor_bits.load(std::memory_order_relaxed)))) return;
  std::lock_guard<std::mutex> lock(g_stats.mutex);
  Top* slot = nullptr;
  for (Top& t : g_stats.top) {
    if (t.dev > 0.0f && t.player == player && t.node == node) slot = &t;
  }
  if (!slot) {
    slot = &g_stats.top[0];
    for (Top& t : g_stats.top) {
      if (t.dev < slot->dev) slot = &t;
    }
  }
  if (dev <= slot->dev) return;
  *slot = Top{player, node, U32(base, player + 156), dev, frame, F32(base, player + 64)};
  for (int i = 0; i < 3; ++i) slot->sample[i] = sample[i], slot->e0[i] = e0[i], slot->e1[i] = e1[i];
  float floor = g_stats.top[0].dev;
  for (const Top& t : g_stats.top) floor = std::min(floor, t.dev);
  g_stats.floor_bits.store(std::bit_cast<uint32_t>(floor), std::memory_order_relaxed);
}

inline void MaybeLog() {
  if (!REXCVAR_GET(wwe2k14_fix_secondary_motion_log)) return;
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  uint64_t last = g_stats.last_log_frame.load(std::memory_order_relaxed);
  if (frame < last + 300 || !g_stats.last_log_frame.compare_exchange_strong(last, frame)) return;
  const uint64_t checked = g_stats.checked.exchange(0), replaced = g_stats.replaced.exchange(0);
  std::string top;
  {
    std::lock_guard<std::mutex> lock(g_stats.mutex);
    std::sort(std::begin(g_stats.top), std::end(g_stats.top), [](const Top& a, const Top& b) { return a.dev > b.dev; });
    for (const Top& t : g_stats.top) {
      if (t.dev > 0.0f) {
        top += fmt::format("{}{:.2f} (player 0x{:08X} [{} nodes, length {:.0f}] node {} at motion frame {:.2f}: "
                           "({:.1f} {:.1f} {:.1f}) between ({:.1f} {:.1f} {:.1f}) and ({:.1f} {:.1f} {:.1f}))",
                           top.empty() ? "" : "; ", t.dev, t.player, t.nodes, t.length, t.node, t.frame, t.sample[0],
                           t.sample[1], t.sample[2], t.e0[0], t.e0[1], t.e0[2], t.e1[0], t.e1[1], t.e1[2]);
      }
    }
    for (Top& t : g_stats.top) t = Top{};
    g_stats.floor_bits.store(0, std::memory_order_relaxed);
  }
  REXLOG_INFO("[wwe2k14][motionfix] frames {}-{}: {} odd-frame translation samples checked, {} replaced; largest jitter "
              "strays: {}",
              last, frame, checked, replaced, top.empty() ? "none" : top);
}

// True where the console would step 2 motion frames per update (so it shows only even frames): the entrances with
// our 60 fps entrance (the game's word stays 60 there, scene word 4), or a scene whose own frame-rate word is 30 / 25
// while --wwe2k14_step_hz rescales it. Matches run at 60 on the console too: their odd frames are the original look
// and are never touched.
inline bool ConsoleShowsEvenFramesOnly() {
  if (REXCVAR_GET(wwe2k14_entrance_60fps) && step_rate::g_entrances.load(std::memory_order_relaxed)) return true;
  if (!step_rate::g_applied_hz.load(std::memory_order_relaxed)) return false;  // no rescale: the step is the game's
  const uint32_t g = step_rate::g_game_word.load(std::memory_order_relaxed);
  return g == 30 || g == 25;
}

// Original: sub_823D91C8 (byte keys; returns at once when [player+88] is 0) or sub_823D9608 (16-bit keys, the same
// arguments and cursor / cache, no +88 test).
template <void (*Original)(PPCContext&, uint8_t*), bool kCheck88>
inline void Sample(PPCContext& ctx, uint8_t* base) {
  const PPCContext entry = ctx;
  Original(ctx, base);
  const PPCContext after = ctx;
  const uint32_t player = entry.r3.u32, out = entry.r4.u32, cursor = entry.r5.u32, track = entry.r6.u32;
  const uint32_t node = entry.r7.u32;
  double t = entry.f1.f64;
  if (t > 65534.0) t = F32(base, player + 56);  // the sampler's own rule (constant 0x8200FFCC = 65534)
  if (!(t >= 0.0) || t > 65532.0) return;
  const double t0 = 2.0 * std::floor(t * 0.5);
  if (t == t0) return;                                  // an even frame: always the game's own sample
  if (kCheck88 && !U32(base, player + 88)) return;      // no data: the byte sampler returned at once
  // Only players that play a motion (a length at +64; the title's: 562). Temporary players on the stack (length 0)
  // sample fixed frames the game asks for (motion frames 1 / 5 / 35 / 41 / 47 in every entrance, runs 2058-2060):
  // the console asks for the same frames and sees the same keys, so they are left alone.
  if (!(F32(base, player + 64) > 0.0f)) return;
  if (!(F32(base, track + 8) >= 1.0e-7f)) return;       // a constant track (constant 0x82010030)
  const uint32_t cache = U32(base, player + 140) + node * 16;
  uint8_t cache_saved[16], cursor_saved[4];
  std::memcpy(cache_saved, decode_probe::GuestPtr(base, cache), 16);
  std::memcpy(cursor_saved, decode_probe::GuestPtr(base, cursor), 4);
  // Scratch on the guest stack below the caller's frame: two vec4 results and a cursor copy. The extra calls run
  // with r1 = the scratch base, so their own saves and frames land below it.
  const uint32_t scratch = (entry.r1.u32 - 128) & ~15u;
  float ends[2][3];
  for (int k = 0; k < 2; ++k) {
    std::memcpy(decode_probe::GuestPtr(base, scratch + 32), cursor_saved, 4);
    ctx = entry;
    ctx.r1.u64 = scratch;
    ctx.r4.u64 = scratch + 16 * k;
    ctx.r5.u64 = scratch + 32;
    ctx.f1.f64 = t0 + 2.0 * k;
    Original(ctx, base);
    std::memcpy(decode_probe::GuestPtr(base, cache), cache_saved, 16);
    for (int i = 0; i < 3; ++i) ends[k][i] = F32(base, scratch + 16 * k + 4 * i);
  }
  ctx = after;
  // The stray is the distance from the stored sample to the segment between the two even samples: jitter sits off
  // it (the title: ~15-20 units off a 2-3 unit segment), while a cut or jump between the even frames puts the odd
  // sample at one end (distance ~0: kept, so a cut never gets an in-between frame).
  const float w = float((t - t0) * 0.5);
  float line[3], sample[3], seg[3], rel[3], seg2 = 0.0f, proj = 0.0f;
  for (int i = 0; i < 3; ++i) {
    line[i] = ends[0][i] + (ends[1][i] - ends[0][i]) * w;
    sample[i] = F32(base, out + 4 * i);
    seg[i] = ends[1][i] - ends[0][i];
    rel[i] = sample[i] - ends[0][i];
    seg2 += seg[i] * seg[i];
    proj += rel[i] * seg[i];
  }
  const float u = seg2 > 0.0f ? std::clamp(proj / seg2, 0.0f, 1.0f) : 0.0f;
  float dev2 = 0.0f;
  for (int i = 0; i < 3; ++i) dev2 += (rel[i] - seg[i] * u) * (rel[i] - seg[i] * u);
  const float dev = std::sqrt(dev2);
  // Real motion stays near the segment even when it turns fast: a point on an arc is (1 - cos a/2) / (2 sin a/2) of
  // the chord from it for a turn a in the two frames (0.21 at 90 degrees, 0.29 at 120). Jitter is far from a short
  // segment (the title: 0.3-18x its length, runs 2008; single-frame spikes: a zero-length segment). Fast-moving
  // 2-node objects (probably lights sweeping ~4 m a frame, runs 2050 / 2051) are 0.06-0.13x their segment: kept.
  const double ratio = REXCVAR_GET(wwe2k14_fix_secondary_motion_ratio);
  const bool jitter = double(dev2) > ratio * ratio * double(seg2);
  g_stats.checked.fetch_add(1, std::memory_order_relaxed);
  if (jitter && std::isfinite(dev) && REXCVAR_GET(wwe2k14_fix_secondary_motion_log)) {
    NoteStray(base, player, node, dev, float(t), sample, ends[0], ends[1]);
  }
  if (jitter && dev > float(REXCVAR_GET(wwe2k14_fix_secondary_motion_min))) {
    for (int i = 0; i < 3; ++i) SetF32(base, out + 4 * i, line[i]);
    g_stats.replaced.fetch_add(1, std::memory_order_relaxed);
  }
  MaybeLog();
}

}  // namespace wwe2k14::motion_fix

REX_HOOK_RAW(sub_823D91C8) {
  if (!REXCVAR_GET(wwe2k14_fix_secondary_motion) || !wwe2k14::motion_fix::ConsoleShowsEvenFramesOnly()) {
    __imp__sub_823D91C8(ctx, base);
    return;
  }
  wwe2k14::motion_fix::Sample<__imp__sub_823D91C8, true>(ctx, base);
}
REX_HOOK_RAW(sub_823D9608) {
  if (!REXCVAR_GET(wwe2k14_fix_secondary_motion) || !wwe2k14::motion_fix::ConsoleShowsEvenFramesOnly()) {
    __imp__sub_823D9608(ctx, base);
    return;
  }
  wwe2k14::motion_fix::Sample<__imp__sub_823D9608, false>(ctx, base);
}
