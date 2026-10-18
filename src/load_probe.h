// wwe2k14 - pac load request probe (Session 12, load-time profiling). Default OFF. LOG ONLY.
//
// Why: in the launch loading screen the loader opens one pac every 4 frames exactly (run 843, f2032, 2036, ...),
// whatever the file's size, so the load looks paced rather than busy. This probe times each request from the
// reader's side (docs/systems/file_loading.md "Request" / "Done"):
//   sub_82C37E48 nonblock_reader_request_impl: r3 = reader; copies the path (16 chars) to reader+20 and enqueues it
//       with the completion trampoline sub_82C37C08.
//   sub_82C37C08 nonblock_reader_read_end_trampoline: r3 = data, r4 = size, r5 = reader; calls the reader's
//       ReadEndProcess (vtable slot 2).
//   --wwe2k14_load_probe=true   one line per call of each:
//       "[wwe2k14][load] enq us=T frame=F tid=H reader=0x.. path=P lr=0x.. lr2=0x.."
//       "[wwe2k14][load] done us=T frame=F tid=H reader=0x.. size=N cb_us=D"   (cb_us = time spent in ReadEndProcess)
//   us = host microseconds since the first probe line. lr = the enqueue's guest caller, lr2 = its caller.
// Cost when off: one setting read per call.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "decode_probe.h"  // Ticks, TicksPerMs, HostTid, GuestPtr
#include "frame_stats.h"
#include "load_turbo.h"  // g_load_turbo_ingame_window (fast loader scope, Session 15)

REXCVAR_DEFINE_BOOL(wwe2k14_load_probe, false, "wwe2k14",
                    "Log every pac load request (sub_82C37E48) and its completion (sub_82C37C08) with host time, frame "
                    "and thread (Session 12 load-time profiling; log only)");

REX_EXTERN(__imp__sub_82C37E48);
REX_EXTERN(__imp__sub_82C37C08);
REX_EXTERN(__imp__sub_82C37070);

namespace wwe2k14::load_probe {

using decode_probe::GuestPtr;
using decode_probe::HostTid;
using decode_probe::Ticks;
using decode_probe::TicksPerMs;

inline int64_t Us() {
  static const int64_t t0 = Ticks();
  return int64_t(double(Ticks() - t0) * 1000.0 / TicksPerMs());
}

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// The caller's own return address, one frame up (same walk as decode_probe.h BpeCall).
inline uint32_t Lr2(PPCContext& ctx, uint8_t* base) {
  const uint32_t sp = ctx.r1.u32;
  if (sp == 0 || (sp & 3) != 0) return 0;
  const uint32_t chain = Be32(GuestPtr(base, sp));
  if (chain <= sp || chain - sp >= (1u << 20) || (chain & 3) != 0) return 0;
  return Be32(GuestPtr(base, chain - 8));
}

inline std::string Path(uint8_t* base, uint32_t reader) {
  std::string s;
  const uint8_t* p = GuestPtr(base, reader + 20);
  for (int i = 0; i < 16 && p[i]; ++i) s.push_back((p[i] >= 32 && p[i] < 127) ? char(p[i]) : '?');
  return s;
}

// State machine (sub_82C37070): current request = [0x839400A4], state = request+104 (1..21, jump table at
// 0x82C37190). Logged when the (request, state) pair changes after a step: how long the previous pair lasted and in
// how many steps (one step per pass of the worker loop, which sleeps 2 ms alertable between passes).
struct StepTrack {
  uint32_t req = 0, state = 0, steps = 0;
  int64_t since_us = 0;
  uint64_t since_frame = 0;
};
inline StepTrack g_step;  // only the CGameWorkerThread steps (under the loader lock)
inline std::atomic<uint32_t> g_worker_tid{0};  // host tid of the thread that steps the state machine

inline uint32_t ReqState(uint8_t* base, uint32_t* req) {
  *req = Be32(GuestPtr(base, 0x839400A4u));
  return *req ? Be32(GuestPtr(base, *req + 104)) : 0;
}

}  // namespace wwe2k14::load_probe

REX_HOOK_RAW(sub_82C37070) {
  if (!REXCVAR_GET(wwe2k14_load_probe)) {
    __imp__sub_82C37070(ctx, base);
    return;
  }
  using namespace wwe2k14::load_probe;
  __imp__sub_82C37070(ctx, base);
  StepTrack& s = g_step;
  g_worker_tid.store(HostTid(), std::memory_order_relaxed);
  uint32_t req = 0;
  const uint32_t state = ReqState(base, &req);
  ++s.steps;
  if (req != s.req || state != s.state) {
    const int64_t us = Us();
    const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    REXLOG_INFO("[wwe2k14][load] state us={} frame={} req=0x{:08X} {} -> req=0x{:08X} {} (prev lasted {} us, {} "
                "frames, {} steps)",
                us, frame, s.req, s.state, req, state, us - s.since_us, frame - s.since_frame, s.steps);
    s.req = req;
    s.state = state;
    s.steps = 0;
    s.since_us = us;
    s.since_frame = frame;
  }
}

REX_HOOK_RAW(sub_82C37E48) {
  if (!REXCVAR_GET(wwe2k14_load_probe)) {
    __imp__sub_82C37E48(ctx, base);
    return;
  }
  using namespace wwe2k14::load_probe;
  const uint32_t reader = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t lr2 = Lr2(ctx, base);
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  const int64_t us = Us();
  __imp__sub_82C37E48(ctx, base);
  REXLOG_INFO("[wwe2k14][load] enq us={} frame={} tid={} reader=0x{:08X} path={} lr=0x{:08X} lr2=0x{:08X}", us, frame,
              HostTid(), reader, Path(base, reader), lr, lr2);
}

// Session 15 (A4): name the pac loader's host thread (the only caller of sub_82C37B98: the CGameWorkerThread loop
// sub_82260310) "PacLoader", and (sub-second loads) the thread that runs the read-end completions and their callbacks
// (sub_82C37C08; run 1182: a different host thread, e.g. the arena's 191 ms callback) "PacComplete", so
// --wwe2k14_profile_threads=PacLoader,PacComplete can sample them. Default off.
REXCVAR_DEFINE_BOOL(wwe2k14_name_loader, false, "wwe2k14",
                    "Debug (Session 15): name the pac loader thread 'PacLoader' and its completion thread "
                    "'PacComplete' (host thread descriptions) so --wwe2k14_profile_threads can sample them");
REX_HOOK_RAW(sub_82C37C08) {
#ifdef _WIN32
  if (REXCVAR_GET(wwe2k14_name_loader)) {
    static thread_local bool named = false;
    if (!named) {
      named = true;
      SetThreadDescription(GetCurrentThread(), L"PacComplete");
      REXLOG_INFO("[wwe2k14][load] PacComplete = host thread {}", GetCurrentThreadId());
    }
  }
#endif
  if (!REXCVAR_GET(wwe2k14_load_probe)) {
    __imp__sub_82C37C08(ctx, base);
    return;
  }
  using namespace wwe2k14::load_probe;
  const uint32_t reader = ctx.r5.u32;
  const uint32_t size = ctx.r4.u32;
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  const int64_t us = Us();
  __imp__sub_82C37C08(ctx, base);
  REXLOG_INFO("[wwe2k14][load] done us={} frame={} tid={} reader=0x{:08X} size={} path={} cb_us={}", us, frame,
              HostTid(), reader, size, Path(base, reader), Us() - us);
}

// Worker-loop timing (sub_82260310 game_worker_thread_main calls, per pass: sub_82261FD0(2, 1) = 2 ms alertable
// sleep, sub_82C36090 = work check, sub_82C37B98 = one locked state-machine step, sub_82C2F878(r24) = unknown).
// With the probe on, every call of these on the state-machine thread that takes >= MIN_US is logged with its guest
// caller (the sleep: >= 4 ms; the others: >= 1 ms).
#define WWE2K14_LOAD_TIMED_HOOK(NAME, MIN_US)                                                                          \
  REX_EXTERN(__imp__##NAME);                                                                                     \
  REX_HOOK_RAW(NAME) {                                                                                           \
    if (!REXCVAR_GET(wwe2k14_load_probe)) {                                                                      \
      __imp__##NAME(ctx, base);                                                                                  \
      return;                                                                                                    \
    }                                                                                                            \
    using namespace wwe2k14::load_probe;                                                                         \
    const uint32_t lr = uint32_t(ctx.lr);                                                                        \
    const uint32_t a0 = ctx.r3.u32;                                                                              \
    const int64_t us = Us();                                                                                     \
    __imp__##NAME(ctx, base);                                                                                    \
    const int64_t d = Us() - us;                                                                                 \
    if (d >= (MIN_US) && HostTid() == g_worker_tid.load(std::memory_order_relaxed)) {                          \
      REXLOG_INFO("[wwe2k14][load] slow " #NAME " us={} frame={} tid={} dur_us={} r3=0x{:08X} lr=0x{:08X}", us, \
                  wwe2k14::g_guest_frame.load(std::memory_order_relaxed), HostTid(), d, a0, lr);                 \
    }                                                                                                            \
  }

WWE2K14_LOAD_TIMED_HOOK(sub_82261FD0, 4000)
WWE2K14_LOAD_TIMED_HOOK(sub_82C36090, 1000)

// --- Fast loader (--wwe2k14_fast_loader, DEFAULT ON since Session 12) ---------------------------------------------------------------
// The loader thread (sub_82260310) runs one state-machine step (sub_82C37B98, its only caller) per pass and then waits
// twice on its per-frame event (sub_82C2F878: leaves the thread object's lock, waits for the event, takes the lock
// again), so the loader advances one step every two frames: a request (open + read, then done + next) costs 4
// frames, whatever its size (run 847: 288 waits = 4.2 s of the 5.4 s launch loading screen). With the setting on,
// each pass keeps stepping inside the same window the game already gives it (lock held, right after the frame
// event): deliver pending read completions (the game's own 0 ms alertable sleep, sub_82261FD0, which runs the I/O
// APCs), check for work (sub_82C36090, the loop's own check) and step again, until there is no work, the budget
// (wwe2k14_fast_loader_budget_ms, measured from the start of the pass's first step) is used or max_steps is reached.
REXCVAR_DEFINE_BOOL(wwe2k14_fast_loader, true, "wwe2k14",
                    "Let the pac loader thread run several state-machine steps per frame window instead of one every "
                    "two frames (default ON since Session 12, Jarrod 2026-10-11 after the 95/95 roster sweep; =false = the game's pacing)");
REXCVAR_DEFINE_DOUBLE(wwe2k14_fast_loader_budget_ms, 8.0, "wwe2k14",
                      "wwe2k14_fast_loader: time budget per loader pass (ms)");
REXCVAR_DEFINE_INT32(wwe2k14_fast_loader_max_steps, 256, "wwe2k14",
                     "wwe2k14_fast_loader: most state-machine steps per loader pass");

// Session 14 (Track B): the fast loader steps aside in the game sequences listed here, so the game's own pacing
// applies. 76 = Create a Superstar (Create-a-Wrestler): run 992 (defaults on) shows the sequence id 75 -> 76 at
// frame 3216, 14 frames after the NEW press and before its first load (LOADHD 3219, CREATEMODE.PAC 3229); with the
// fast loader its CREATEMODE.PAC read-end callback runs before the object it fills exists and crashes in
// sub_829BF558 (runs 911 / 919 / 922 / 992), with the game's pacing it does not (run 921).
// Create-a-Story (Session 14, run 1039 in the close-out list): the same kind of crash in its scene editor, a story-only
// read-end callback (sub_82A97D10 -> sub_82BAA6F8) on MENUHD.PAC writing through a null object; intermittent (runs
// 912 / 1045 passed). The editor runs in the in-game sequence 9 entered from the story menus 11 (run 1045 peek:
// 75 -> 11 -> 9), hence "9<11".
REXCVAR_DEFINE_STRING(wwe2k14_fast_loader_off_sequences, "76,9<11", "wwe2k14",
                      "wwe2k14_fast_loader: comma-separated game sequence ids (chain 0x839400F8>0>48) in which the "
                      "fast loader is suspended and the game's own pacing applies; N<M = sequence N entered from M. "
                      "Session 14: 76 = Create-a-Wrestler, 9<11 = Create-a-Story's scene editor (both crash with it). "
                      "Empty = never suspended");

// Session 15 (A1): where the fast loader runs. "turbo" = only while the load turbo's in-game windows are open (the
// pre-entrance load "ingame" and the match load "match", load_turbo.h; the loads it was measured on in Sessions 12 /
// 14), so boot, menus and the creation modes keep the game's own pacing; with --wwe2k14_load_turbo=false there are no
// windows, so no fast loader either. "all" = every load (Sessions 12-14). The sequence list above applies in both.
REXCVAR_DEFINE_STRING(wwe2k14_fast_loader_scope, "turbo", "wwe2k14",
                      "wwe2k14_fast_loader: 'turbo' = only inside the load turbo's in-game windows (pre-entrance load, "
                      "match load; Session 15), 'all' = every load (Sessions 12-14)");

namespace wwe2k14::load_probe {
// Fault-safe guest word read (sequence chain read from the loader thread; same checks as load_turbo.h Read).
inline bool SafeRead32(uint32_t guest, uint32_t* out) {
  if (guest == 0 || (guest & 3) != 0) return false;
  auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest);
  if (!host || !wwe2k14::GuestWordReadable(guest, host)) return false;  // guest_read.h (Session 15)
  *out = Be32(host);
  return true;
}
// Current game sequence id (0 if unreadable), read at most once per guest frame, and the sequence it was entered from
// (the previous non-zero id; 0 until a change has been seen). Called on every loader pass (each frame).
inline std::atomic<uint32_t> g_seq_from{0};
inline uint32_t CurrentSequence() {
  static std::atomic<uint64_t> last_frame{~0ull};
  static std::atomic<uint32_t> last_seq{0};
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (last_frame.load(std::memory_order_relaxed) == frame) return last_seq.load(std::memory_order_relaxed);
  uint32_t a = 0, b = 0, seq = 0;
  if (!(SafeRead32(0x839400F8u, &a) && SafeRead32(a, &b) && SafeRead32(b + 48, &seq))) seq = 0;
  const uint32_t before = last_seq.load(std::memory_order_relaxed);
  if (seq != 0 && before != 0 && seq != before) g_seq_from.store(before, std::memory_order_relaxed);
  if (seq != 0) last_seq.store(seq, std::memory_order_relaxed);
  last_frame.store(frame, std::memory_order_relaxed);
  return seq ? seq : before;
}
// Tokens: "N" = sequence N; "N<M" = sequence N entered from sequence M (Create-a-Story's scene editor runs in the
// in-game sequence 9 entered from the story menus 11; matches enter 9 from the main menu 75).
inline bool SequenceListed(uint32_t seq, uint32_t from, const std::string& list) {
  size_t pos = 0;
  while (pos < list.size()) {
    size_t comma = list.find(',', pos);
    if (comma == std::string::npos) comma = list.size();
    const std::string token = list.substr(pos, comma - pos);
    pos = comma + 1;
    char* end = nullptr;
    const unsigned long v = std::strtoul(token.c_str(), &end, 0);
    if (end == token.c_str() || v != seq) continue;
    if (*end == '<') {
      char* end2 = nullptr;
      const unsigned long f = std::strtoul(end + 1, &end2, 0);
      if (end2 != end + 1 && f == from) return true;
      continue;
    }
    return true;
  }
  return false;
}
// The fast loader's on/off for this call: the setting and its scope, unless the current sequence is listed (logged on
// change).
inline bool FastLoaderActive() {
  if (!REXCVAR_GET(wwe2k14_fast_loader)) return false;
  const std::string list = REXCVAR_GET(wwe2k14_fast_loader_off_sequences);
  const uint32_t seq = list.empty() ? 0 : CurrentSequence();
  const uint32_t from = g_seq_from.load(std::memory_order_relaxed);
  const bool suspended = !list.empty() && seq != 0 && SequenceListed(seq, from, list);
  const bool out_of_scope = REXCVAR_GET(wwe2k14_fast_loader_scope) != "all" &&
                            !wwe2k14::g_load_turbo_ingame_window().load(std::memory_order_relaxed);
  // 0 = active, 1 = suspended by the sequence list, 2 = outside the load turbo's in-game windows (scope "turbo").
  static std::atomic<int> last{-1};
  const int now = suspended ? 1 : out_of_scope ? 2 : 0, prev = last.exchange(now);
  if (prev != now && !(prev == -1 && now == 0)) {  // first call while active: no line
    REXLOG_INFO("[wwe2k14][fast_loader] {} (sequence {}, entered from {}) frame={}",
                now == 1   ? "suspended: the game's own pacing"
                : now == 2 ? "idle: outside the load turbo's in-game windows"
                           : "active",
                seq, from, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
  }
  return now == 0;
}

struct FastStats {
  std::atomic<uint64_t> passes{0}, extra_steps{0}, budget_hits{0}, idle_spins{0};
  std::atomic<uint64_t> next_report_frame{600};
};
inline FastStats g_fast;
inline std::atomic<uint64_t> g_short_waits{0};  // frame waits cut short (sub_82C2F878 hook below)
inline std::atomic<uint64_t> g_wait_cut_pac{0}, g_wait_cut_jobs{0};  // ... because pac work / a job was queued
}  // namespace wwe2k14::load_probe

REX_EXTERN(__imp__sub_82C37B98);
REX_HOOK_RAW(sub_82C37B98) {
  using namespace wwe2k14::load_probe;
#ifdef _WIN32
  static thread_local bool named = false;
  if (!named && REXCVAR_GET(wwe2k14_name_loader)) {
    named = true;
    SetThreadDescription(GetCurrentThread(), L"PacLoader");
    REXLOG_INFO("[wwe2k14][load] PacLoader = host thread {}", GetCurrentThreadId());
  }
#endif
  const bool probe = REXCVAR_GET(wwe2k14_load_probe);
  const bool fast = FastLoaderActive();
  if (!probe && !fast) {
    __imp__sub_82C37B98(ctx, base);
    return;
  }
  const int64_t t0 = Ticks();
  __imp__sub_82C37B98(ctx, base);
  uint32_t steps = 1, spins = 0;
  bool budget_hit = false;
  if (fast) {
    const double budget_ms = REXCVAR_GET(wwe2k14_fast_loader_budget_ms);
    const int32_t max_steps = REXCVAR_GET(wwe2k14_fast_loader_max_steps);
    while (int32_t(steps) < max_steps) {
      if (double(Ticks() - t0) / TicksPerMs() >= budget_ms) {
        budget_hit = true;
        break;
      }
      uint32_t req_before = 0;
      const uint32_t state_before = ReqState(base, &req_before);
      ctx.r3.u64 = 0;  // 0 ms
      ctx.r4.u64 = 1;  // alertable: runs the read-completion APCs
      __imp__sub_82261FD0(ctx, base);
      __imp__sub_82C36090(ctx, base);
      if (ctx.r3.u32 == 0) break;  // nothing queued, nothing in flight
      __imp__sub_82C37B98(ctx, base);
      uint32_t req_after = 0;
      const uint32_t state_after = ReqState(base, &req_after);
      if (req_after == req_before && state_after == state_before) ++spins;  // waiting for a read to finish
      ++steps;
    }
    g_fast.passes.fetch_add(1, std::memory_order_relaxed);
    g_fast.extra_steps.fetch_add(steps - 1, std::memory_order_relaxed);
    g_fast.idle_spins.fetch_add(spins, std::memory_order_relaxed);
    if (budget_hit) g_fast.budget_hits.fetch_add(1, std::memory_order_relaxed);
    const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    uint64_t next = g_fast.next_report_frame.load(std::memory_order_relaxed);
    if (frame >= next && g_fast.next_report_frame.compare_exchange_strong(next, (frame / 600 + 1) * 600)) {
      REXLOG_INFO("[wwe2k14][fast_loader] at frame {}: passes={} extra_steps={} idle_spins={} budget_hits={} "
                  "short_waits={} (pac {} jobs {})",
                  frame, g_fast.passes.load(), g_fast.extra_steps.load(), g_fast.idle_spins.load(),
                  g_fast.budget_hits.load(), g_short_waits.load(), g_wait_cut_pac.load(), g_wait_cut_jobs.load());
    }
  }
  const int64_t d = int64_t(double(Ticks() - t0) * 1000.0 / TicksPerMs());
  if (probe && (d >= 1000 || steps > 1)) {
    REXLOG_INFO("[wwe2k14][load] pass us={} frame={} tid={} dur_us={} steps={} idle_spins={} budget_hit={}", Us() - d,
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed), HostTid(), d, steps, spins,
                budget_hit ? 1 : 0);
  }
}

// Frame wait (sub_82C2F878, thread object r3): leave the object's lock (+12), flags +40 = 0 / +8 = 1, wait on the
// per-frame event (+104) with no timeout (sub_82261F38 -> sub_82263C00(handle, ms, alertable)), reset it
// (sub_82C8F0F0), take the lock again, flags back, return 1. With --wwe2k14_fast_loader and the call coming from the
// loader loop's two wait sites (return addresses 0x822603D8 / 0x822603F0) while the loader has work (sub_82C36090),
// or a job waits in the loop's second queue ([0x8394012C]+48, sub_82C3BF80: one job per pass, e.g. the memory-pool
// sizing that probes MmAllocatePhysicalMemoryEx, run 849), the same sequence runs with a wait of
// wwe2k14_fast_loader_wait_ms instead of a whole frame. Other callers
// (sub_82C3D490) and an idle loader keep the game's wait.
REXCVAR_DEFINE_INT32(wwe2k14_fast_loader_wait_ms, 1, "wwe2k14",
                     "wwe2k14_fast_loader: while the loader has work, its frame waits time out after this many ms "
                     "(-1 = keep the game's whole-frame wait)");
REX_EXTERN(__imp__sub_82C2F878);
REX_EXTERN(__imp__RtlLeaveCriticalSection);
REX_EXTERN(__imp__RtlEnterCriticalSection);
REX_EXTERN(sub_82263C00);
REX_EXTERN(sub_82C8F0F0);
namespace wwe2k14::load_probe {
inline void StoreBe32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = GuestPtr(base, addr);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
}  // namespace wwe2k14::load_probe
REX_HOOK_RAW(sub_82C2F878) {
  using namespace wwe2k14::load_probe;
  const uint32_t lr = uint32_t(ctx.lr);
  const bool loop_site = lr == 0x822603D8u || lr == 0x822603F0u;
  const int32_t wait_ms = REXCVAR_GET(wwe2k14_fast_loader_wait_ms);
  if (loop_site && wait_ms >= 0 && FastLoaderActive()) {
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_82C36090(ctx, base);  // the loop's own work check (same lock order: thread lock held here)
    bool busy = ctx.r3.u32 != 0;
    if (busy) {
      g_wait_cut_pac.fetch_add(1, std::memory_order_relaxed);
    } else {
      // The loop's second job queue: [0x8394012C] = queue object, +48 = head of its job list (sub_82C3BF80 runs one
      // job per pass). Read without its lock: only a hint for how long to wait.
      const uint32_t q = Be32(GuestPtr(base, 0x8394012Cu));
      busy = q != 0 && Be32(GuestPtr(base, q + 48)) != 0;
      if (busy) g_wait_cut_jobs.fetch_add(1, std::memory_order_relaxed);
    }
    if (busy) {
      ctx.r3.u64 = obj + 12;
      __imp__RtlLeaveCriticalSection(ctx, base);
      StoreBe32(base, obj + 40, 0);
      StoreBe32(base, obj + 8, 1);
      ctx.r3.u64 = Be32(GuestPtr(base, obj + 104));
      ctx.r4.u64 = uint64_t(uint32_t(wait_ms));
      ctx.r5.u64 = 0;
      sub_82263C00(ctx, base);
      ctx.r3.u64 = Be32(GuestPtr(base, obj + 104));
      sub_82C8F0F0(ctx, base);
      ctx.r3.u64 = obj + 12;
      __imp__RtlEnterCriticalSection(ctx, base);
      StoreBe32(base, obj + 40, 1);
      StoreBe32(base, obj + 8, 0);
      ctx.r3.u64 = 1;
      g_short_waits.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ctx.r3.u64 = obj;
  }
  if (!REXCVAR_GET(wwe2k14_load_probe)) {
    __imp__sub_82C2F878(ctx, base);
    return;
  }
  const uint32_t a0 = ctx.r3.u32;
  const int64_t us = Us();
  __imp__sub_82C2F878(ctx, base);
  const int64_t d = Us() - us;
  if (d >= 1000 && HostTid() == g_worker_tid.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[wwe2k14][load] slow sub_82C2F878 us={} frame={} tid={} dur_us={} r3=0x{:08X} lr=0x{:08X}", us,
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed), HostTid(), d, a0, lr);
  }
}

// Second job queue of the loader loop (sub_82C3BF80(queue): job = [queue+48]; calls [job+8]([job+12]); pops it):
// with the probe on, one line per job: function, argument, duration.
REX_EXTERN(__imp__sub_82C3BF80);
REX_HOOK_RAW(sub_82C3BF80) {
  if (!REXCVAR_GET(wwe2k14_load_probe)) {
    __imp__sub_82C3BF80(ctx, base);
    return;
  }
  using namespace wwe2k14::load_probe;
  const uint32_t q = ctx.r3.u32;
  const uint32_t job = Be32(GuestPtr(base, q + 48));
  const uint32_t fn = job ? Be32(GuestPtr(base, job + 8)) : 0;
  const uint32_t arg = job ? Be32(GuestPtr(base, job + 12)) : 0;
  const int64_t us = Us();
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  __imp__sub_82C3BF80(ctx, base);
  if (job) {
    REXLOG_INFO("[wwe2k14][load] job us={} frame={} tid={} job=0x{:08X} fn=0x{:08X} arg=0x{:08X} dur_us={}", us, frame,
                HostTid(), job, fn, arg, Us() - us);
  }
}

// Entrance-motion poller (sub_8232B868, once per frame on the game logic thread; requests MOVN/MOVL... through
// sub_8232B1C8 when the sequence value is 855 or a flag holds). Its value: X = [0x836F2E58] + 16;
// Y = X + 12 + [X+4] * 332 (sub_82321528); i = [Y+12] (< 8); value = [Y + 140 + 24 * i] (sub_8230AD78). With the
// probe on, logs each change of (i, value) with the frame.
REX_EXTERN(__imp__sub_8232B868);
namespace wwe2k14::load_probe {
inline uint32_t g_seq_last = 0xFFFFFFFFu, g_seq_idx_last = 0xFFFFFFFFu;
}  // namespace wwe2k14::load_probe
REX_HOOK_RAW(sub_8232B868) {
  if (!REXCVAR_GET(wwe2k14_load_probe)) {
    __imp__sub_8232B868(ctx, base);
    return;
  }
  using namespace wwe2k14::load_probe;
  uint32_t i = 0xFFFFFFFFu, v = 0xFFFFFFFFu;
  const uint32_t root = Be32(GuestPtr(base, 0x836F2E58u));
  if (root) {
    const uint32_t x = root + 16;
    const uint32_t y = x + 12 + Be32(GuestPtr(base, x + 4)) * 332;
    i = Be32(GuestPtr(base, y + 12));
    v = i < 8 ? Be32(GuestPtr(base, y + 140 + 24 * i)) : 0;
  }
  if (i != g_seq_idx_last || v != g_seq_last) {
    REXLOG_INFO("[wwe2k14][load] seq us={} frame={} idx={} value={} (was idx={} value={})", Us(),
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed), int32_t(i), v, int32_t(g_seq_idx_last),
                g_seq_last);
    g_seq_idx_last = i;
    g_seq_last = v;
  }
  __imp__sub_8232B868(ctx, base);
}

// Loading-screen candidates (Session 12): coverage says these ran in match_load / entrance_to_match_load (and story)
// but never in entrances, in the ring or the menus. With the probe on, each hooked function logs every run of
// consecutive frames it was called on: "[wwe2k14][load] runs NAME frames A-B calls N" (written when the run ends).
namespace wwe2k14::load_probe {
struct FrameRun {
  const char* name;
  uint64_t first = ~0ull, last = ~0ull, calls = 0;
  void Hit(uint64_t frame) {
    if (last != ~0ull && frame > last + 1) {
      REXLOG_INFO("[wwe2k14][load] runs {} frames {}-{} calls {}", name, first, last, calls);
      first = ~0ull;
      calls = 0;
    }
    if (first == ~0ull) first = frame;
    last = frame;
    ++calls;
  }
};
}  // namespace wwe2k14::load_probe
#define WWE2K14_LOAD_RUN_HOOK(NAME)                                                              \
  REX_EXTERN(__imp__##NAME);                                                                     \
  namespace wwe2k14::load_probe {                                                                \
  inline FrameRun g_run_##NAME{#NAME};                                                           \
  }                                                                                              \
  REX_HOOK_RAW(NAME) {                                                                           \
    if (REXCVAR_GET(wwe2k14_load_probe)) {                                                       \
      wwe2k14::load_probe::g_run_##NAME.Hit(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)); \
    }                                                                                            \
    __imp__##NAME(ctx, base);                                                                    \
  }
WWE2K14_LOAD_RUN_HOOK(sub_82808CF0)
WWE2K14_LOAD_RUN_HOOK(sub_82808D48)
WWE2K14_LOAD_RUN_HOOK(sub_82809398)
WWE2K14_LOAD_RUN_HOOK(sub_8281C8C8)
WWE2K14_LOAD_RUN_HOOK(sub_8281CC48)
WWE2K14_LOAD_RUN_HOOK(sub_82329D40)
WWE2K14_LOAD_RUN_HOOK(sub_8232ADC8)
WWE2K14_LOAD_RUN_HOOK(sub_825C2F40)
WWE2K14_LOAD_RUN_HOOK(sub_827E4538)
WWE2K14_LOAD_RUN_HOOK(sub_82EAAB80)
WWE2K14_LOAD_RUN_HOOK(sub_82F0FEA8)
