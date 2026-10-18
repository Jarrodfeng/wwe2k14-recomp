// wwe2k14 - fixed random seed (Session 8, task 8a). --wwe2k14_fixed_seed=N pins the game's clock-seeded random
// sources so two boots with the same N start every generator from the same state. Default -1 = off; 0 is a valid
// seed. Hooks: config/determinism_hooks.toml. System overview and what is live-checked: docs/systems/rng.md.
//
// What it pins (sources and addresses from db/rng.json, each re-checked against the generated code with
// tools/analysis/list_function.py in Session 8):
//   1. Boot seeds, sub_827E1C30 right after the local-time call (0x827E1C40 bl sub_82C8EF80) returns: the hour,
//      minute, second and milliseconds halfwords of the SYSTEMTIME at r1+104/+106/+108/+110 are overwritten before
//      the first load at 0x827E1C44. They feed MT #1 and MT #2 (packed, via sub_82C3F338 at 0x827E1C7C), MT #1 again
//      and random-table cursor A (low byte of the milliseconds, 0x827E1C80-0x827E1C90) and the main thread's CRT
//      srand (milliseconds, 0x827E1C94-0x827E1C98).
//      Mapping: milliseconds = N % 1000, second = (N / 1000) % 60, minute = (N / 60000) % 60,
//      hour = (N / 3600000) % 24. What each source sees: MT #1 / cursor A (N % 1000) & 0xFF; srand N % 1000;
//      MT #2 the OR-packed word ((h << 8 | m) << 8 | s) << 8 | ms built at 0x827E1C44-0x827E1C68, which is lossy
//      (ms needs 10 bits and overlaps the low 2 bits of the seconds byte: 1234 and 1490 both pack to 0x1EA; about
//      22.1 million distinct values per 86.4 million N). All boot seeds repeat with period 86,400,000.
//   2. Audio LCG (word 0x8395C860), sub_82EF8350 at 0x82EF8368: r3 holds the time() result from the call at
//      0x82EF8364 and is stored at 0x82EF836C. r3 is replaced with N (all 32 bits).
//   3. Script reseeds, sub_82C644F8 (math.randomseed-shaped native) at its srand call 0x82C6450C: r3 is the
//      script's value. It is replaced with N ONLY when it looks clock-derived (within kClockWindowSeconds of the
//      current Unix time: the guest's time() is host UTC Unix seconds under ReXGlue, run 211's audio seed
//      1791480114 = the boot time 17:21:54 UTC). Any other value is a seed the script chose on purpose and is
//      kept, so repeated deliberate reseeds still give their own sequences. Both decisions are logged. Scripts may
//      seed from os.time()-like natives (sub_82C671D0 and sub_82C67450 call time(); unconfirmed which natives
//      they are); a clock value the script transforms first (os.time() % 1000, os.clock()) is not recognised.
// Log-only (no change): "event 1" reseed of MT #1 / cursor A in sub_826D0AB0 at 0x826D0AEC (r3 = seed it uses;
// 0 offline, so already fixed). Shows when event 1 runs relative to menus and matches.
// Not touched (fixed by the executable, so already the same every boot): cursor B (0x839456D5), LCG #2 / #3 / #4
// start values, LCG #5 (zeroed by its user), CRT rand on non-main threads (starts at 1). The boot hook logs the
// current values of these state words once so runs can be compared.
//
// Clock reads that are NOT pinned (not known to feed a seed; see docs/systems/rng.md for the list): KeQuerySystemTime
// users other than the two seeds above, time() callers other than the audio thread, mftb (1547 sites) and
// KeQueryPerformanceFrequency. Unconfirmed: the time() wrappers reached only through function pointers
// (sub_82EF8210, sub_83191888, sub_82C671D0, sub_82C67450) are untraced. Run-to-run drift from per-frame draws (frame timing) is task 8b.
//
// Every hook returns at once when the setting is off (one cvar read). Each hook logs once the first time it changes
// something ([wwe2k14][seed] lines); the script and event-1 hooks log their first kMaxLogLines calls.
//
// Value checks: the SDK parses the setting as a base-10 int64 and accepts a partial match, so "0x1234" is read as 0
// (decimal only). The boot hook warns, once, about values it cannot use as given: below -1 (treated as off) and
// above 4294967295 (only the low 32 bits are used). The "[seed] fixed seed N" line always shows the value in use.
// The setting is read each time a hook runs, but the boot seeds are taken once per boot, so change it on the
// command line, not at run time.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include "frame_stats.h"

REXCVAR_DEFINE_INT64(wwe2k14_fixed_seed, -1, "wwe2k14",
                     "Pin the game's clock-seeded random sources to this seed (decimal, 0..4294967295): boot time "
                     "of day (MT #1, MT #2, random-table cursor A, main-thread srand), the audio LCG's time() seed "
                     "and clock-derived script randomseed values. -1 = off (normal behaviour). See "
                     "docs/systems/rng.md");

namespace wwe2k14::determinism {

constexpr uint32_t kMaxLogLines = 16;
// A script seed within this many seconds of the current Unix time is treated as clock-derived (os.time()-shaped).
// One day covers clock skew and long runs; a deliberate seed this close to the current Unix time is unlikely.
constexpr int64_t kClockWindowSeconds = 86400;

inline bool Enabled() { return REXCVAR_GET(wwe2k14_fixed_seed) >= 0; }
inline uint32_t Seed() { return static_cast<uint32_t>(REXCVAR_GET(wwe2k14_fixed_seed)); }

// Host Unix seconds; the guest's time() returns the same value under ReXGlue (see the header comment).
inline int64_t UnixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline uint8_t* Guest(uint32_t address) { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address); }
inline uint32_t GuestU32(uint32_t address) { return rex::memory::load_and_swap<uint32_t>(Guest(address)); }
inline uint16_t GuestU16(uint32_t address) { return rex::memory::load_and_swap<uint16_t>(Guest(address)); }
inline uint8_t GuestU8(uint32_t address) { return *Guest(address); }
inline void SetGuestU16(uint32_t address, uint16_t value) { rex::memory::store_and_swap<uint16_t>(Guest(address), value); }

inline uint64_t Frame() { return g_guest_frame.load(std::memory_order_relaxed); }

inline std::atomic<bool> g_boot_logged{false};
inline std::atomic<bool> g_audio_logged{false};
inline std::atomic<uint32_t> g_script_calls{0};
inline std::atomic<uint32_t> g_event1_calls{0};

}  // namespace wwe2k14::determinism

// sub_827E1C30 at 0x827E1C44 (first load after the local-time call): r1 = the frame holding the SYSTEMTIME at +96.
void wwe2k14_seed_boot_time(PPCRegister& r1) {
  using namespace wwe2k14::determinism;
  const int64_t value = REXCVAR_GET(wwe2k14_fixed_seed);
  if (value < 0) {
    // Off. Runs once per boot, so the range check costs nothing; -1 (the default) stays silent.
    if (value != -1) {
      REXLOG_WARN("[wwe2k14][seed] --wwe2k14_fixed_seed={} is negative: setting off (use -1 for off, "
                  "0..4294967295 for a seed)", value);
    }
    return;
  }
  if (value > 0xFFFFFFFFll) {
    REXLOG_WARN("[wwe2k14][seed] --wwe2k14_fixed_seed={} is above 4294967295: using its low 32 bits, {}", value,
                Seed());
  }
  const uint32_t n = Seed();
  const uint32_t base = r1.u32 + 104;  // hour +104, minute +106, second +108, milliseconds +110
  const uint16_t old_h = GuestU16(base), old_m = GuestU16(base + 2), old_s = GuestU16(base + 4),
                 old_ms = GuestU16(base + 6);
  const uint16_t ms = static_cast<uint16_t>(n % 1000), s = static_cast<uint16_t>((n / 1000) % 60),
                 m = static_cast<uint16_t>((n / 60000) % 60), h = static_cast<uint16_t>((n / 3600000) % 24);
  SetGuestU16(base, h);
  SetGuestU16(base + 2, m);
  SetGuestU16(base + 4, s);
  SetGuestU16(base + 6, ms);
  if (!g_boot_logged.exchange(true)) {
    const uint32_t mt2 = ((((static_cast<uint32_t>(h) << 8) | m) << 8 | s) << 8) | ms;  // as 0x827E1C44-68 packs it
    REXLOG_INFO("[wwe2k14][seed] fixed seed {}: boot time of day {:02}:{:02}:{:02}.{:03} -> {:02}:{:02}:{:02}.{:03} "
                "(MT #1 / cursor A seed byte {}, srand {}, MT #2 packed seed 0x{:X}) frame={}",
                n, old_h, old_m, old_s, old_ms, h, m, s, ms, ms & 0xFF, ms, mt2, Frame());
    // Sources this setting does not write; logged so two runs can be compared (addresses: db/rng.json).
    REXLOG_INFO("[wwe2k14][seed] state at boot (not pinned): cursorA=0x{:02X} cursorB=0x{:02X} lcg2=0x{:08X} "
                "lcg3=0x{:08X} lcg4=0x{:08X} lcg5=0x{:08X} audio_lcg=0x{:08X}",
                GuestU8(0x839456D4), GuestU8(0x839456D5), GuestU32(0x836229F8), GuestU32(0x83622AC8),
                GuestU32(0x835E1984), GuestU32(0x835E4CD0), GuestU32(0x8395C860));
  }
}

// sub_82EF8350 (audio worker thread entry) at 0x82EF8368: r3 = time() result, stored to 0x8395C860 at 0x82EF836C.
void wwe2k14_seed_audio_time(PPCRegister& r3) {
  using namespace wwe2k14::determinism;
  if (!Enabled()) return;
  const uint32_t old = r3.u32;
  r3.u64 = Seed();
  if (!g_audio_logged.exchange(true)) {
    REXLOG_INFO("[wwe2k14][seed] audio LCG seed time() {} -> {} frame={}", old, Seed(), Frame());
  }
}

// sub_82C644F8 (script math.randomseed-shaped native) at 0x82C6450C, its srand call: r3 = the script's value.
// Replaced only when it looks clock-derived (see the header comment); deliberate seeds are kept.
void wwe2k14_seed_script_srand(PPCRegister& r3) {
  using namespace wwe2k14::determinism;
  if (!Enabled()) return;
  const uint32_t old = r3.u32;
  const int64_t now = UnixNow();
  const int64_t delta = static_cast<int64_t>(old) - now;
  const bool clock_like = delta >= -kClockWindowSeconds && delta <= kClockWindowSeconds;
  if (clock_like) r3.u64 = Seed();
  const uint32_t call = ++g_script_calls;
  if (call <= kMaxLogLines) {
    if (clock_like) {
      REXLOG_INFO("[wwe2k14][seed] script randomseed #{}: {} is clock-like ({:+} s from now) -> {} frame={}", call,
                  old, delta, Seed(), Frame());
    } else {
      REXLOG_INFO("[wwe2k14][seed] script randomseed #{}: {} kept (not clock-like) frame={}", call, old, Frame());
    }
  }
}

// sub_826D0AB0 ("event 1" reset) at 0x826D0AEC, its MT #1 reseed call: r3 = seed (0 offline). Log only.
void wwe2k14_seed_event1(PPCRegister& r3) {
  using namespace wwe2k14::determinism;
  if (!Enabled()) return;
  const uint32_t call = ++g_event1_calls;
  if (call <= kMaxLogLines) {
    REXLOG_INFO("[wwe2k14][seed] event 1 reseed #{}: MT #1 / cursor A seed {} (not changed) frame={}", call,
                r3.u32, Frame());
  }
}
