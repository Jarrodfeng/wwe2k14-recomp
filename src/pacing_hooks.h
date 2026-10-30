// wwe2k14 - frame pacing, Step 0 of docs/systems/frame_pacing.md "Safe experiment for 60 fps entrances".
// Mostly log only. Two hooks change the game: the entrance-60 skip of the halve call at 0x82331778
// (--wwe2k14_entrance_60fps) and the late-frame threshold (--wwe2k14_late_frame_threshold, Session 73: r4 replaced).
// Hooks: config/s7_pacing_hooks.toml.
//
// Goal: name the exact call that sets the frame-rate word to 30 in entrances, with guest frame numbers to line up
// against logs/timeline_NNN.csv and logs/frames_NNN.csv.
//
// Timeline events (written only when --wwe2k14_timeline=true; values are hex in the CSV like every other event):
//   fps_word     a = frame-rate word (0x8392DC40), b = the end-of-frame code's copy of it (engine +472),
//                c = guest frame. Only when the word differs from the last one seen here or from that copy (the
//                frame on which the game re-sets the present interval), plus the first presented frame.
//   fps_halve    a call to sub_82BB6D50 is about to run. a = call-site address (the site id), b = word before the
//   fps_restore  call, c = guest frame. Same for sub_82BB6DF0. Every call, including no-op ones.
//   fps_thresh   the late-frame threshold setter sub_82CAA838 ran with a new value. a = value passed (low 7 bits are
//                used), b = D3D device, c = guest frame.
// Log lines, prefix [wwe2k14][pacing] (off with --wwe2k14_pacing_log=false): every word change seen at the end of a
// frame; every halve/restore call that will change the word; the first kMaxNoopLines no-op calls per site, then
// only a count (shown as "call #n" in that site's later lines; each call is also a timeline event when the
// timeline is on); every new threshold value.
//
// "Guest frame" = g_guest_frame (frame_stats.h, VdSwap calls so far). The halve/restore calls run wherever the
// game calls them (probably the update thread; the timeline records the host thread id), so their frame number is
// "frames presented so far" at that moment. At the end-of-frame hook the present has just returned; statically,
// both present paths (sub_82CB3F28, sub_82C227E8) go through sub_82CB37C0, the function that calls VdSwap.
//
// Whether a halve/restore call will change the word is predicted from the callees' code (Session 7 listing of
// sub_82BB6D50 and sub_82BB6DF0): halve changes 60 -> 30 and 50 -> 25, nothing else; restore changes 30 -> 60
// and 25 -> 50 unless the lock flag 0x8392DC44 is non-zero. The fps_word line at the end of the frame confirms it.
//
// Cost: the end-of-frame hook runs once per presented frame (one atomic load and two compares when nothing
// changed). The halve/restore/threshold hooks run only when the game calls those routines.

#pragma once

#include <rex/cvar.h>
#include <rex/det.h>  // Session 73: the late-frame threshold stays the game's in replay mode
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/perf/timeline.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_pacing_log, true, "wwe2k14",
                    "Log [wwe2k14][pacing] lines: frame-rate word changes, the game's calls that halve or restore "
                    "the frame rate, and the late-frame threshold (log only; the timeline events need "
                    "--wwe2k14_timeline=true)");

namespace wwe2k14::pacing {

// Timing block (db/frame_pacing.json video_timing_block). Its base is built as a lis/addi pair in sub_82BB6D50,
// sub_82BB6DF0 and sub_82BA84E8 (Session 7 listing: 0x83930000 - 0x23C8 = 0x8392DC38).
constexpr uint32_t kTimingBlock = 0x8392DC38;
constexpr uint32_t kFrameRateWord = kTimingBlock + 8;  // 0x8392DC40
constexpr uint32_t kLockFlag = kTimingBlock + 12;      // 0x8392DC44: the restore routine returns at once if non-zero
constexpr uint32_t kMaxNoopLines = 8;                  // per call site
constexpr uint32_t kNone = 0xFFFFFFFFu;                // "nothing seen yet"

enum class Kind { kHalve, kRestore };

inline uint32_t ReadGuestU32(uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address));
}

inline uint32_t GuestFrame() {
  return static_cast<uint32_t>(g_guest_frame.load(std::memory_order_relaxed));
}

// End-of-frame hook state (render command thread only; atomics so a stray caller can't tear them).
inline std::atomic<uint32_t> g_last_word{kNone};
// Threshold hook state.
inline std::atomic<uint32_t> g_last_threshold{kNone};
inline std::atomic<uint32_t> g_threshold_calls{0};

// What the callee will store, from its code: 0 = no change.
inline uint32_t PredictedWord(Kind kind, uint32_t word, uint32_t lock) {
  if (kind == Kind::kHalve) return word == 60 ? 30 : word == 50 ? 25 : 0;
  if (lock != 0) return 0;
  return word == 30 ? 60 : word == 25 ? 50 : 0;
}

// One instantiation per call site, so each site has its own call counter.
template <uint32_t kSite>
void OnRateCall(Kind kind, uint32_t caller) {
  static std::atomic<uint32_t> calls{0};
  static std::atomic<uint32_t> noop_lines{0};
  const uint32_t word = ReadGuestU32(kFrameRateWord);
  const uint32_t frame = GuestFrame();
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool halve = kind == Kind::kHalve;
  rex::perf::timeline::Record(halve ? "fps_halve" : "fps_restore", kSite, word, frame);
  if (!REXCVAR_GET(wwe2k14_pacing_log)) return;
  const uint32_t lock = halve ? 0 : ReadGuestU32(kLockFlag);
  const uint32_t next = PredictedWord(kind, word, lock);
  if (next != 0) {
    REXLOG_INFO("[wwe2k14][pacing] frame {}: {} call at 0x{:08X} (sub_{:08X}) changes the word {} -> {} "
                "(call #{} from this site)",
                frame, halve ? "halve" : "restore", kSite, caller, word, next, n);
    return;
  }
  const uint32_t line = noop_lines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (line > kMaxNoopLines) return;
  // On the last line, say where the later no-op calls go. The timeline is off by default, and then they are only
  // counted: the count shows up as "call #n" in this site's later lines.
  const char* tail = "";
  if (line == kMaxNoopLines) {
    tail = rex::perf::timeline::Enabled()
               ? "; further no-op calls from this site are not logged (each one is still a timeline event)"
               : "; further no-op calls from this site are not logged, only counted (the count shows as call #n "
                 "in this site's later lines; --wwe2k14_timeline=true records each one)";
  }
  REXLOG_INFO("[wwe2k14][pacing] frame {}: {} call at 0x{:08X} (sub_{:08X}), word {}{}: no change (call #{} from "
              "this site){}",
              frame, halve ? "halve" : "restore", kSite, caller, word, lock ? ", lock flag set" : "", n, tail);
}

template <uint32_t kSite>
void Halve(uint32_t caller) {
  OnRateCall<kSite>(Kind::kHalve, caller);
}

template <uint32_t kSite>
void Restore(uint32_t caller) {
  OnRateCall<kSite>(Kind::kRestore, caller);
}

}  // namespace wwe2k14::pacing

// Mid-asm hooks (config/s7_pacing_hooks.toml). Global, non-inline: the generated code declares them extern, and
// this header is only included from main.cpp.

// sub_82BA84E8 at 0x82BA85E8, once per presented frame: r9 = the copy of the word from the last change
// (engine +472), r10 = the frame-rate word.
void wwe2k14_pacing_fps_word(PPCRegister& r9, PPCRegister& r10) {
  using namespace wwe2k14::pacing;
  const uint32_t copy = r9.u32;
  const uint32_t word = r10.u32;
  const uint32_t last = g_last_word.load(std::memory_order_relaxed);
  if (word == last && copy == word) return;  // the per-frame path
  g_last_word.store(word, std::memory_order_relaxed);
  const uint32_t frame = GuestFrame();
  rex::perf::timeline::Record("fps_word", word, copy, frame);
  if (!REXCVAR_GET(wwe2k14_pacing_log)) return;
  if (copy != word) {
    // The game now calls sub_82CAA828 at 0x82BA8638 with 2 for 30/25, else 1 (static reading of 0x82BA85F8-0x82BA8630).
    REXLOG_INFO("[wwe2k14][pacing] frame {}: frame-rate word {} -> {} seen at end of frame; present interval "
                "becomes {} from the next present",
                frame, copy, word, (word == 30 || word == 25) ? 2 : 1);
  } else {
    // First presented frame, or (not expected: only 0x82BA85F0 and the display constructor write the copy) the word
    // changed and the copy already matched it.
    REXLOG_INFO("[wwe2k14][pacing] frame {}: frame-rate word {}{}", frame, word,
                last == kNone ? " (first presented frame)" : " (copy already matched; unexpected)");
  }
}

// Session 73 (Jarrod: "the first second or two of everything, entrances, matches starting, runs in slow motion"):
// the game sets its D3D late-frame threshold to 0 at boot, so every frame that misses its vblank waits a whole extra
// one (docs/systems/frame_pacing.md finding 3): a scene start's slightly late frames become 33.3 ms each, half speed.
// >= 0 replaces the value the game passes (percent of a vblank period a frame may be late and still flip at once).
// DEFAULT 100 (Session 73, PC A/B runs 4685-4689, normal speed, one lane): a late frame flips when it is ready (20-25
// ms) instead of at the next vblank (33.3 ms); the entrance start's first 0.5 s 0.82-0.86 -> 0.95 of real time, frames
// of 30-40 ms over a run 34-36 -> 9. The presenter shows whole frames, so no tearing. Replay mode (det) keeps the
// game's 0: its flips are on the det vblank grid, and the replays' picture references were taken that way.
REXCVAR_DEFINE_INT32(wwe2k14_late_frame_threshold, 100, "wwe2k14",
                     "Session 73: the D3D late-frame threshold (percent, 0-127) instead of the game's own 0, so a "
                     "slightly late frame flips at once instead of a whole vblank later; -1 = the game's "
                     "(src/pacing_hooks.h)");

// sub_82CAA838 at its first instruction: r3 = D3D device, r4 = new late-frame threshold (percent, low 7 bits).
void wwe2k14_pacing_threshold(PPCRegister& r3, PPCRegister& r4) {
  using namespace wwe2k14::pacing;
  if (const int32_t forced = REXCVAR_GET(wwe2k14_late_frame_threshold); forced >= 0 && !rex::det::Enabled()) {
    r4.u32 = (r4.u32 & ~0x7Fu) | (uint32_t(std::min(forced, 127)) & 0x7Fu);
  }
  const uint32_t value = r4.u32;
  const uint32_t n = g_threshold_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t previous = g_last_threshold.exchange(value, std::memory_order_relaxed);
  if (previous == value) return;
  const uint32_t device = r3.u32;
  const uint32_t frame = GuestFrame();
  rex::perf::timeline::Record("fps_thresh", value, device, frame);
  if (!REXCVAR_GET(wwe2k14_pacing_log)) return;
  // The bits the setter is about to overwrite: bits 23-29 of device+12284 (the getter sub_82CAA848 reads the same).
  const uint32_t current = device ? (ReadGuestU32(device + 12284) >> 23) & 0x7F : 0;
  REXLOG_INFO("[wwe2k14][pacing] frame {}: late-frame threshold set to {} (passed 0x{:X}; was {}) on device "
              "0x{:08X}, call #{}",
              frame, value & 0x7F, value, current, device, n);
}

// Calls to sub_82BB6D50 (halve), one hook per call instruction; the template argument is the call site.
void wwe2k14_pacing_halve_8339E7D0() { wwe2k14::pacing::Halve<0x8339E7D0>(0x8339E720); }
void wwe2k14_pacing_halve_8235BA64() { wwe2k14::pacing::Halve<0x8235BA64>(0x8235B948); }
void wwe2k14_pacing_halve_825BF048() { wwe2k14::pacing::Halve<0x825BF048>(0x825BEE80); }
// Session 7 prep experiment: 0x82331778 is the one call that switches every entrance to 30 fps (run 126). With
// --wwe2k14_entrance_60fps=true the hook returns true and the generated code jumps over the call
// (jump_address_on_true = 0x8233177C in config/s7_pacing_hooks.toml), so the word stays 60: the game's own 60 fps
// mode (step 1/60 s, present interval 1). Read when the call is about to run (entrance start). Default off.
REXCVAR_DEFINE_BOOL(wwe2k14_entrance_60fps, false, "wwe2k14",
                    "Experiment: skip the entrance's switch to 30 fps (call site 0x82331778), so entrances run at 60");
bool wwe2k14_pacing_halve_82331778() {
  wwe2k14::pacing::Halve<0x82331778>(0x82331628);
  if (!REXCVAR_GET(wwe2k14_entrance_60fps)) return false;
  REXLOG_INFO("[wwe2k14][pacing] entrance 60 fps experiment: skipping the halve call at 0x82331778 (frame {})",
              wwe2k14::g_guest_frame.load());
  return true;
}
void wwe2k14_pacing_halve_82330BC0() { wwe2k14::pacing::Halve<0x82330BC0>(0x82330B50); }
void wwe2k14_pacing_halve_833B5A1C() { wwe2k14::pacing::Halve<0x833B5A1C>(0x833B59E0); }
void wwe2k14_pacing_halve_825C0FA8() { wwe2k14::pacing::Halve<0x825C0FA8>(0x825C0EC8); }
void wwe2k14_pacing_halve_82A4BD1C() { wwe2k14::pacing::Halve<0x82A4BD1C>(0x82A4BCE0); }
void wwe2k14_pacing_halve_82704D24() { wwe2k14::pacing::Halve<0x82704D24>(0x82704CF8); }

// Calls to sub_82BB6DF0 (restore).
void wwe2k14_pacing_restore_8339E83C() { wwe2k14::pacing::Restore<0x8339E83C>(0x8339E720); }
void wwe2k14_pacing_restore_825BBE24() { wwe2k14::pacing::Restore<0x825BBE24>(0x825BBC00); }
void wwe2k14_pacing_restore_825BF818() { wwe2k14::pacing::Restore<0x825BF818>(0x825BEE80); }
void wwe2k14_pacing_restore_825BF89C() { wwe2k14::pacing::Restore<0x825BF89C>(0x825BEE80); }
void wwe2k14_pacing_restore_8233178C() { wwe2k14::pacing::Restore<0x8233178C>(0x82331628); }
void wwe2k14_pacing_restore_82330BC8() { wwe2k14::pacing::Restore<0x82330BC8>(0x82330B50); }
void wwe2k14_pacing_restore_83392A28() { wwe2k14::pacing::Restore<0x83392A28>(0x833929E8); }
void wwe2k14_pacing_restore_82A44A4C() { wwe2k14::pacing::Restore<0x82A44A4C>(0x82A44A38); }
void wwe2k14_pacing_restore_833B51F8() { wwe2k14::pacing::Restore<0x833B51F8>(0x833B5198); }
void wwe2k14_pacing_restore_82705CA0() { wwe2k14::pacing::Restore<0x82705CA0>(0x82705C80); }
void wwe2k14_pacing_restore_82A49C58() { wwe2k14::pacing::Restore<0x82A49C58>(0x82A49C48); }
void wwe2k14_pacing_restore_82A4C100() { wwe2k14::pacing::Restore<0x82A4C100>(0x82A4C0F0); }
void wwe2k14_pacing_restore_8232F80C() { wwe2k14::pacing::Restore<0x8232F80C>(0x8232F7D8); }
void wwe2k14_pacing_restore_827ABECC() { wwe2k14::pacing::Restore<0x827ABECC>(0x827ABDD8); }
void wwe2k14_pacing_restore_8235B92C() { wwe2k14::pacing::Restore<0x8235B92C>(0x8235B688); }
