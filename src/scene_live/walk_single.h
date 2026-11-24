// S110 WALKSINGLE (docs\research\s110\WALKRACE.md "The fix", "Sites the build will touch" 2): the game's own single-worker mode for the stream-free build. The shared setter sub_82BB4128 (r3 = object, r4 = value:
// stores [r3 + 140] = r4, folded and shared with other classes' +140 setters) is asked by sub_827B2068 +80 (and the tail call in sub_827B2050 at 0x827B2060) to store the match range table 0x83625190 for the
// draw manager G = [0x8392D9B8]. We store 0:
// then the main thread's walk records every category in ascending order and CParallelDraw1 records none, so no two 2D-record brackets can interleave (the 360 kept them apart only by the time of categories 0-12,
// which our OURS-frame skips remove). The final replay order is unchanged (WALKRACE W7). The cvar wwe2k14_rdr_walk_single (defined in start_settings.cpp, stored into walk1::g_on once at start): false = the setter's
// arguments pass through unchanged (today's behaviour).
// Two detectors count the symptom (docs\research\s110\CAPDET.md): the caption draw slot sub_82345BB8 (render category 57) sets a thread flag; the text run's record read sub_82BCEC58 (only caller
// sub_82BD06B8 +110; r3 = text object, r4 = the run's layout block, r8 != 0 = the run ignores the record) counts a caption build per run, and an UNSCALED one when the block's scale [blk + 16] came out 1.0
// (the function stores 1.0 there exactly when [[0x8392DD80] + 16] != 1, +114..+11C). It also samples [G + 136] (the live table) at that build. (sub_82BCA8F8, the Xfm rebuild, never runs for caption text: CAPDET 3-5.)
// Census line every 600 guest frames, printed from the walk's end hook (draw_origin.h, sub_82C3B218: every frame of every scene, captions or not); it reads the live [G + 136] itself when it prints.
#pragma once

#include "../keep_stream.h"
#include "walk_single_core.h"

#if !WWE2K14_STREAM()

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../frame_stats.h"  // g_guest_frame
#include "../guest_read.h"   // GuestWordReadable
#include "../keep_stream.h"

namespace wwe2k14::walk1 {

inline constexpr uint32_t kRecordWord = 0x8392DD80u;   // the shared 2D layout record pointer (ROWBOUNCE G1 / WALKRACE lead a)
inline constexpr uint32_t kTableOff = 136;             // [G + 136] = the live range table (G + 140 = the one the setter stores)
inline constexpr uint32_t kBlockScaleX = 16;           // [layout block + 16] (f32): the record's x scale when [[0x8392DD80] + 16] == 1, else 1.0 (sub_82BCEC58 +FC / +11C; CAPDET 6)

inline thread_local bool t_caption = false;

inline bool Readable(const uint8_t* base, uint32_t a) {
  a &= ~3u;
  return a >= 0x10000 && wwe2k14::GuestWordReadable(a, base + a);
}
inline uint32_t Rd32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return std::byteswap(v);
}
inline uint32_t RdSafe(const uint8_t* base, uint32_t a) { return Readable(base, a) ? Rd32(base, a) : 0; }

// Called from the walk's end hook (both walk threads, every category): one relaxed load per call; the one thread that wins the exchange prints.
inline void Census(const uint8_t* base) {
  static std::atomic<uint64_t> last_log{0};
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  uint64_t last = last_log.load(std::memory_order_relaxed);
  if (frame < last + 600) return;
  if (!last_log.compare_exchange_strong(last, frame, std::memory_order_relaxed)) return;
  const uint32_t g = RdSafe(base, kManagerWord);
  const uint32_t live = g != 0 ? RdSafe(base, g + kTableOff) : 0;
  REXLOG_INFO("[walk1] single {}, refused {}, table {:#x}, table at caption {:#x}, caption builds {}, unscaled {}", g_on.load(std::memory_order_relaxed) ? "on" : "off",
              (unsigned long long)g_refused.load(), live, g_table_seen.load(), (unsigned long long)g_caption_builds.load(), (unsigned long long)g_unscaled.load());
}

}  // namespace wwe2k14::walk1

REX_EXTERN(__imp__sub_82BB4128);
REX_EXTERN(__imp__sub_82345BB8);
REX_EXTERN(__imp__sub_82BCEC58);

REX_HOOK_RAW(sub_82BB4128) {
  namespace w1 = wwe2k14::walk1;
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t now = w1::Decide(w1::g_on.load(std::memory_order_relaxed), ctx.r3.u32, r4, w1::RdSafe(base, w1::kManagerWord));
  if (now != r4) {
    ctx.r4.u64 = now;
    w1::g_refused.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82BB4128(ctx, base);
}

REX_HOOK_RAW(sub_82345BB8) {   // detector only: the caption draw slot (category 57)
  namespace w1 = wwe2k14::walk1;
  const bool was = w1::t_caption;
  w1::t_caption = true;
  __imp__sub_82345BB8(ctx, base);
  w1::t_caption = was;
}

REX_HOOK_RAW(sub_82BCEC58) {   // detector only: the text run's read of the 2D record; r4 / r8 are read before the call (the original reuses them)
  namespace w1 = wwe2k14::walk1;
  const bool watch = w1::t_caption && ctx.r8.u32 == 0;   // r8 != 0: this run ignores the record's offset / scale
  const uint32_t blk = ctx.r4.u32;                       // the caller's layout block (sub_82BD06B8 sp+112), filled by the original
  __imp__sub_82BCEC58(ctx, base);
  if (!watch || !w1::Readable(base, blk + w1::kBlockScaleX)) return;
  w1::g_caption_builds.fetch_add(1, std::memory_order_relaxed);
  if (std::bit_cast<float>(w1::Rd32(base, blk + w1::kBlockScaleX)) == 1.0f) w1::g_unscaled.fetch_add(1, std::memory_order_relaxed);
  const uint32_t g = w1::RdSafe(base, w1::kManagerWord);
  if (g != 0) w1::g_table_seen.store(w1::RdSafe(base, g + w1::kTableOff), std::memory_order_relaxed);
}

#endif  // !WWE2K14_STREAM()
