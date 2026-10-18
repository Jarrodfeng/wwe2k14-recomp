// wwe2k14 - frame-barrier probe (Session 8 task B, "what do the 4K entrance drops wait on"). Default OFF.
//
// The render thread (CRenderCmdManager loop sub_82BB5CC0) releases the per-frame barrier at the top of each
// iteration (sub_82C3BB38, called at 0x82BB5DE4), runs the last frame's command list, then waits for every client
// to arrive (sub_82C3BAE0, called at 0x82BB5F5C) before it presents. Clients (db/frame_pacing.json: CGameMainThread,
// CRenderParticleManager, CSkinModelUtil, CParallelDraw0/1, OnlineUpdateOtherThread) arrive in sub_82C3BC60: they
// signal their slot's arrival event (bl at 0x82C3BCA0, r31 = slot) and block on the release event (bl at
// 0x82C3BCAC); 0x82C3BCB0 is the first instruction after that wait returns. Addresses: tools/analysis/
// list_function.py listings of sub_82C3BC60, sub_82C3BAE0 and sub_82BB5CC0 (Session 8 task B), matching the db.
//
//   --wwe2k14_barrier_probe=true   write logs/barrier_NNN.csv: one row per render-loop iteration (at the return of
//        the wait), times in ms after that iteration's release call:
//          frame, t_s (QPC seconds since the first row), wait_begin, wait_end,
//          n (client slots seen so far), then per slot (first-seen order): lr (guest return address of the
//          sub_82C3BC60 call: which thread loop the client is), tid (host thread id), wake (its release wait
//          returned), arrive (it signalled arrival). A slot that did not run this cycle (no release seen after
//          this iteration's release call) gets wake = arrive = -1.
//        "frame" = g_guest_frame (frame_stats.h, VdSwap calls so far) at the end of the wait; the present follows,
//        so this row's wait belongs to the frame that frames_NNN.csv lists as frame+1 (checked in the report).
//   --wwe2k14_barrier_names=true   name each client's host thread "bclient_<lr>_<tid>" (SetThreadDescription) on
//        its first arrival, so --wwe2k14_profile_threads="bclient_" can sample them (the sampler looks up names it
//        has not found yet at every report). Changes only the host thread description (which is also what
//        thread_placement.h's NAME rules and the flight recorder's thread labels read, so they see the new name).
//
// Hooks: config/s8_barrier_probe.toml (log only: none returns a value, no register or guest memory is changed).
// Cost when off, per frame (5 client slots seen at run time, runs 327-358): 5 clients x 3 hooks (client_enter reads
// two settings, the others one), 5 render-thread hooks (release, wait_begin, wait_end, post_call_begin/_end), and in
// sub_82C3BA08's 6-slot callback loop one cb_end per slot plus one cb_begin per non-null slot (up to 12; the same
// loop also runs once before the render loop, call at 0x82BB5D88; tools/q callers 0x82C3BA08 = 2 call sites).
// When on: a QueryPerformanceCounter and a few atomic stores per hook; one ~200-byte fprintf per frame on the render
// thread (buffered, flushed every 120 rows). The usual close hard-exits, so the last <= 120 rows can be lost.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_barrier_probe, false, "wwe2k14",
                    "Write logs/barrier_NNN.csv: per frame, when each frame-barrier client thread was released and "
                    "arrived, and how long the render thread waited for them (Session 8 task B; log only)");
REXCVAR_DEFINE_BOOL(wwe2k14_barrier_names, false, "wwe2k14",
                    "Name the frame-barrier client host threads bclient_<lr>_<tid> so wwe2k14_profile_threads can "
                    "sample them (host thread description only)");

namespace wwe2k14::barrier {

constexpr int kMaxSlots = 24;  // 24 client slots by the db's reading of frame_barrier_ctor (unconfirmed); 5 seen used

struct Slot {
  std::atomic<uint32_t> addr{0};  // guest slot address (r31 in sub_82C3BC60)
  std::atomic<uint32_t> lr{0};
  std::atomic<uint32_t> tid{0};
  std::atomic<int64_t> wake{0};    // QPC
  std::atomic<int64_t> arrive{0};  // QPC
};

inline Slot g_slots[kMaxSlots];
inline std::atomic<int> g_nslots{0};
inline std::atomic<int64_t> g_release{0};     // QPC of this iteration's release call (render thread)
inline std::atomic<int64_t> g_wait_begin{0};  // QPC
inline thread_local uint32_t t_lr = 0;
inline thread_local bool t_named = false;
inline thread_local int t_slot = -1;

inline int64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

inline double QpcMs(int64_t ticks) {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart);
  }();
  return double(ticks) * 1000.0 / freq;
}

inline bool On() { return REXCVAR_GET(wwe2k14_barrier_probe) || REXCVAR_GET(wwe2k14_barrier_names); }

inline int FindOrAddSlot(uint32_t addr) {
  const int n = g_nslots.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (g_slots[i].addr.load(std::memory_order_relaxed) == addr) return i;
  // New slot: claim an index. Clients each own one slot, so races only happen at the first arrival of two threads.
  const int i = g_nslots.load(std::memory_order_relaxed);
  if (i >= kMaxSlots) return -1;
  int expected = i;
  if (!g_nslots.compare_exchange_strong(expected, i + 1)) return FindOrAddSlot(addr);
  g_slots[i].addr.store(addr, std::memory_order_relaxed);
  return i;
}

struct Writer {
  FILE* f = nullptr;
  bool failed = false;
  int64_t t0 = 0;
  uint32_t rows = 0;
};
inline Writer g_writer;  // used only by the render thread (the thread that reaches 0x82BB5F60)

}  // namespace wwe2k14::barrier

// Mid-asm hooks (config/s8_barrier_probe.toml). Global, non-inline; this header is only included from main.cpp.

// sub_82C3BC60 at 0x82C3BC64 (after mflr r12): r12 = the guest return address of this barrier call.
void wwe2k14_barrier_client_enter(PPCRegister& r12) {
  using namespace wwe2k14::barrier;
  if (!On()) return;
  t_lr = r12.u32;
#ifdef _WIN32
  if (!t_named && REXCVAR_GET(wwe2k14_barrier_names)) {
    t_named = true;
    wchar_t name[64];
    swprintf(name, 64, L"bclient_%08X_%u", t_lr, unsigned(GetCurrentThreadId()));
    SetThreadDescription(GetCurrentThread(), name);
    REXLOG_INFO("[wwe2k14][barrier] client thread {} (barrier call returns to 0x{:08X}) named bclient_{:08X}_{}",
                GetCurrentThreadId(), t_lr, t_lr, GetCurrentThreadId());
  }
#endif
}

// sub_82C3BC60 at 0x82C3BCA0 (the arrival-event signal call): r31 = this thread's slot.
void wwe2k14_barrier_client_arrive(PPCRegister& r31) {
  using namespace wwe2k14::barrier;
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  const int64_t now = Qpc();
  const int i = FindOrAddSlot(r31.u32);
  t_slot = i;
  if (i < 0) return;
  g_slots[i].lr.store(t_lr, std::memory_order_relaxed);
  g_slots[i].tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  g_slots[i].arrive.store(now, std::memory_order_release);
}

// sub_82C3BC60 at 0x82C3BCB0 (first instruction after the release wait returned): r31 = the slot.
void wwe2k14_barrier_client_released(PPCRegister& r31) {
  using namespace wwe2k14::barrier;
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  const int64_t now = Qpc();
  int i = t_slot;
  if (i < 0 || g_slots[i].addr.load(std::memory_order_relaxed) != r31.u32) i = FindOrAddSlot(r31.u32);
  if (i < 0) return;
  g_slots[i].wake.store(now, std::memory_order_release);
}

// sub_82BB5CC0 at 0x82BB5DE4: the render thread is about to release the clients (sub_82C3BB38).
void wwe2k14_barrier_render_release() {
  using namespace wwe2k14::barrier;
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  g_release.store(Qpc(), std::memory_order_release);
}

// sub_82BB5CC0 at 0x82BB5F5C: the render thread is about to wait for the clients (sub_82C3BAE0).
void wwe2k14_barrier_render_wait_begin() {
  using namespace wwe2k14::barrier;
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  g_wait_begin.store(Qpc(), std::memory_order_release);
}

// After the wait the render thread calls sub_82BA78F8 (0x82BB5F64) and sub_82C3BA08 (0x82BB5F6C), which calls up
// to 6 callbacks from barrier+3880 (bctrl at 0x82C3BA38, r11 = callback, r3 = its context from slot+24; the loop
// continues at 0x82C3BA3C, also the target of the null-slot branch at 0x82C3BA2C). With the probe on, any of these
// calls that takes at least wwe2k14_barrier_slow_ms logs one "[wwe2k14][barrier] slow" line (frame, what, ms).
REXCVAR_DEFINE_DOUBLE(wwe2k14_barrier_slow_ms, 5.0, "wwe2k14",
                      "Barrier probe: log render-thread post-barrier calls taking at least this many ms");

namespace wwe2k14::barrier {
inline thread_local int64_t t_post_start = 0;
inline thread_local uint32_t t_post_what = 0;
inline void PostBegin(uint32_t what) {
  t_post_what = what;
  t_post_start = Qpc();
}
inline void PostEnd(uint32_t ctx_r3) {
  if (!t_post_start) return;
  const double ms = QpcMs(Qpc() - t_post_start);
  t_post_start = 0;
  if (ms >= REXCVAR_GET(wwe2k14_barrier_slow_ms))
    REXLOG_INFO("[wwe2k14][barrier] slow frame {}: {} 0x{:08X} took {:.2f} ms (context 0x{:08X})",
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed),
                t_post_what == 0x82BA78F8 ? "call" : "post-barrier callback", t_post_what, ms, ctx_r3);
}
inline thread_local uint32_t t_cb_ctx = 0;
}  // namespace wwe2k14::barrier

// sub_82BB5CC0 at 0x82BB5F64 (bl sub_82BA78F8) and 0x82BB5F68 (after it).
void wwe2k14_barrier_post_call_begin() {
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  wwe2k14::barrier::PostBegin(0x82BA78F8);
}
void wwe2k14_barrier_post_call_end() {
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  wwe2k14::barrier::PostEnd(0);
}
// sub_82C3BA08 at 0x82C3BA38 (bctrl): r11 = callback, r3 = context.
void wwe2k14_barrier_cb_begin(PPCRegister& r11, PPCRegister& r3) {
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  wwe2k14::barrier::t_cb_ctx = r3.u32;
  wwe2k14::barrier::PostBegin(r11.u32);
}
// sub_82C3BA08 at 0x82C3BA3C (after a callback, or a null slot skipped: then nothing was started).
void wwe2k14_barrier_cb_end() {
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  wwe2k14::barrier::PostEnd(wwe2k14::barrier::t_cb_ctx);
}

// sub_82BB5CC0 at 0x82BB5F60: the wait returned; every active client has arrived for this iteration.
void wwe2k14_barrier_render_wait_end() {
  using namespace wwe2k14::barrier;
  if (!REXCVAR_GET(wwe2k14_barrier_probe)) return;
  const int64_t now = Qpc();
  const int64_t rel = g_release.load(std::memory_order_acquire);
  if (rel == 0) return;  // the pre-loop wait at 0x82BB5D90 is not hooked; first loop iteration not released yet
  Writer& w = g_writer;
  if (w.failed) return;
  if (!w.f) {
    const int run = wwe2k14::CurrentLogNumber();
    char name[64];
    std::snprintf(name, sizeof(name), "barrier_%03d.csv", run < 0 ? 0 : run);
    const std::filesystem::path p = std::filesystem::current_path() / "logs" / name;
    w.f = _wfopen(p.wstring().c_str(), L"w");
    if (!w.f) {
      w.failed = true;
      REXLOG_WARN("[wwe2k14][barrier] cannot write {}", p.string());
      return;
    }
    std::setvbuf(w.f, nullptr, _IOFBF, 1 << 20);
    std::fputs("frame,t_s,wait_begin,wait_end,n,slots(lr;tid;wake;arrive)...\n", w.f);
    w.t0 = rel;
    REXLOG_INFO("[wwe2k14][barrier] probe on: writing {}", p.string());
  }
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  const int n = g_nslots.load(std::memory_order_acquire);
  const double base = QpcMs(rel);
  std::fprintf(w.f, "%llu,%.4f,%.3f,%.3f,%d", static_cast<unsigned long long>(frame), (QpcMs(rel - w.t0)) / 1000.0,
               QpcMs(g_wait_begin.load(std::memory_order_acquire)) - base, QpcMs(now) - base, n);
  for (int i = 0; i < n; ++i) {
    const int64_t wk = g_slots[i].wake.load(std::memory_order_acquire);
    const int64_t ar = g_slots[i].arrive.load(std::memory_order_acquire);
    const bool ran = wk >= rel;
    std::fprintf(w.f, ",%08X;%u;%.3f;%.3f", g_slots[i].lr.load(std::memory_order_relaxed),
                 g_slots[i].tid.load(std::memory_order_relaxed), ran ? QpcMs(wk) - base : -1.0,
                 (ran && ar >= wk) ? QpcMs(ar) - base : -1.0);
  }
  std::fputc('\n', w.f);
  if (++w.rows % 120 == 0) std::fflush(w.f);
}
