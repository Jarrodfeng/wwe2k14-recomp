// wwe2k14 - determinism probes (Session 8, tasks A and 2). Everything here is default off. Settings:
//   --wwe2k14_fp_probe            the pairwise character distance probe (this comment);
//   --wwe2k14_alloc_trace_frames  boot allocation trace (second section below);
//   --wwe2k14_det_boot_alloc      holds one racing boot allocation so it always comes after the 0x836F3AAC object
//                                 (fixes that one word only, not the whole boot layout; third section).
//                                 Findings: docs/systems/determinism.md, Result 5.
//   --wwe2k14_det_boot_alloc_place=N  places that allocation at a fixed point (task 2; fourth section);
//   --wwe2k14_skel_probe (+ _dump_from / _dump_frames)  skeleton update probe (task 2; fifth section);
//   --wwe2k14_det_inline_motion   waits for the motion decompression worker after each post (task 2; last section).
//                                 Findings: docs/systems/determinism.md, Result 6.
//
// Why: two same-seed CPU vs CPU runs (docs/systems/determinism.md) first differ in the float at character object
// +2812, one bit apart. +2812 is a copy of one entry of a 9x9 float table at 0x837683D8 (row = the lower of the two
// characters' slot numbers at +1220, column = the higher; reader sub_82665E38, its callers sub_8259F9B0 and
// sub_824D2B30 store the result at +2812). The table is filled by sub_82666310: for every slot pair i < j of the
// slot table 0x836F38D0 (9 words) whose both slots are set, entry i*9+j = sub_82C327C8(slot_i[+176], slot_j[+176]),
// the distance in the x/z plane, sqrt((ax-bx)^2 + (az-bz)^2) on the floats at +0 and +8 of the two objects.
// (Addresses from the generated code: generated/default/wwe2k14_recomp.171.cpp sub_82666310,
// wwe2k14_recomp.45.cpp sub_82C327C8, wwe2k14_recomp.157.cpp sub_82665E38; `py tools/q/q.py imm 0x837683D8`.)
//
// What it does, with --wwe2k14_fp_probe=true (config/fp_probe_hooks.toml: one hook in sub_82666310 at 0x8266637C,
// right after `bl 0x82c327c8` returns, before the result is stored with stfsx):
//   - reads the host MXCSR of this thread and the guest context's cached copy (ctx.fpscr.csr: what the recompiled
//     code believes the rounding / flush bits are; it only rewrites MXCSR when the cached copy disagrees with what
//     it wants);
//   - re-reads the four input floats (the callee does not touch r3/r4) and recomputes the distance with the same
//     operation sequence as the generated sub_82C327C8, (a) under the thread's current MXCSR and (b) under round to
//     nearest with flush-to-zero / denormals-are-zero off (what PowerPC code outside VMX expects);
//   - writes one TSV row per call to out/harness/determinism/fp_NNN.tsv (NNN = log number): guest frame, host and
//     guest thread, i, j, both object pointers, the four inputs and the result as hex, MXCSR, the cached csr and
//     flags (A = recompute under the current MXCSR differs from the result: the inputs changed between the call
//     and this hook, i.e. another thread wrote them; C = the canonical recompute differs: the floating-point
//     environment changed the result; S = the cached csr disagrees with the real MXCSR in the rounding / flush
//     bits; N = rounding mode not nearest or a flush bit set);
//   - logs the first kMaxAnomalyLines rows with a flag ([wwe2k14][fpprobe] lines) and a summary every
//     kSummaryCalls calls.
// Two runs' TSVs line up by (call order within the pair, frame); tools/determinism/fp_compare.py compares them.
//
// Cost: off = one cvar read per call of the hook (a few calls per frame, only in scenes with characters). On = a
// few microseconds per call plus a buffered fprintf.
//
// Game-derived output (positions and distances of real frames): stays under out/harness/.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>

#include <xmmintrin.h>

#ifdef _WIN32
#include <share.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_fp_probe, false, "wwe2k14",
                    "Determinism probe: log every pairwise character distance the game computes (sub_82666310 / "
                    "sub_82C327C8) with its inputs, the thread, MXCSR and a recompute check, to "
                    "out/harness/determinism/fp_NNN.tsv (default off; see src/fp_probe.h)");

namespace wwe2k14::fpprobe {

constexpr uint32_t kMaxAnomalyLines = 24;
constexpr uint64_t kSummaryCalls = 20000;
constexpr uint32_t kRoundBits = 0x6000;  // MXCSR RC
constexpr uint32_t kFlushBits = 0x8040;  // MXCSR FTZ | DAZ

inline uint32_t GuestU32(uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address));
}
inline float AsFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
inline uint32_t AsBits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// The generated sub_82C327C8, operation for operation (lfs, fsubs, lfs, lfs, fsubs, fmuls, fmadds, fsqrts).
// noinline + volatile inputs so it runs under whatever MXCSR the caller set and is not folded or moved.
__attribute__((noinline)) inline uint32_t Distance(uint32_t ax, uint32_t az, uint32_t bx, uint32_t bz) {
  volatile uint32_t in[4] = {ax, az, bx, bz};
  const double f0 = double(AsFloat(in[0]));
  const double f13 = double(AsFloat(in[2]));
  const double f12 = double(float(f0 - f13));
  const double f11 = double(AsFloat(in[1]));
  const double f10 = double(AsFloat(in[3]));
  const double f9 = double(float(f11 - f10));
  const double f8 = double(float(f12 * f12));
  const double f7 = double(float(std::fma(f9, f9, f8)));
  const double f1 = double(float(std::sqrt(f7)));
  return AsBits(float(f1));
}

struct Probe {
  std::mutex mutex;
  FILE* file = nullptr;
  bool opened = false;
  uint64_t calls = 0, flag_a = 0, flag_c = 0, flag_s = 0, flag_n = 0;
  uint32_t anomaly_lines = 0;

  void OpenLocked() {
    opened = true;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::filesystem::path exe = std::filesystem::path(std::wstring(buf, n));
    const std::string folder = exe.parent_path().filename().string();
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::weakly_canonical(exe.parent_path() / ".." / ".." / "harness" / "determinism", ec);
    std::filesystem::create_directories(dir, ec);
    char num[16];
    std::snprintf(num, sizeof(num), "%03d", std::max(CurrentLogNumber(), 0));
    const std::string stem = folder == "win-amd64-release" ? "fp_" + std::string(num)
                                                           : "fp_" + folder + "_" + std::string(num);
    const std::filesystem::path path = dir / (stem + ".tsv");
    file = _fsopen(path.string().c_str(), "w", _SH_DENYNO);
    if (!file) {
      REXLOG_WARN("[wwe2k14][fpprobe] cannot write {}", path.string());
      return;
    }
    std::fprintf(file,
                 "frame\thtid\tgtid\ti\tj\ta\tb\tax\taz\tbx\tbz\tresult\trecomp_cur\trecomp_canon\tmxcsr\tcached\t"
                 "flags\n");
    REXLOG_INFO("[wwe2k14][fpprobe] writing {}", path.string());
#endif
  }
};

inline Probe& Instance() {
  static Probe p;
  return p;
}

}  // namespace wwe2k14::fpprobe

// sub_82666310 at 0x8266637C (after `bl 0x82c327c8`): r3 / r4 = the two objects (slot_i[+176], slot_j[+176]),
// r30 = i * 9, r31 = j, f1 = the distance about to be stored at 0x837683D8 + (r30 + r31) * 4. Log only.
void wwe2k14_fp_probe_dist(PPCRegister& r3, PPCRegister& r4, PPCRegister& r30, PPCRegister& r31, PPCRegister& f1,
                           PPCFPSCRRegister& fpscr) {
  if (!REXCVAR_GET(wwe2k14_fp_probe)) return;
  using namespace wwe2k14::fpprobe;
  const uint32_t mx = _mm_getcsr();
  const uint32_t cached = fpscr.csr;
  const uint32_t a = r3.u32, b = r4.u32;
  const uint32_t ax = GuestU32(a), az = GuestU32(a + 8), bx = GuestU32(b), bz = GuestU32(b + 8);
  const uint32_t result = AsBits(float(f1.f64));
  const uint32_t recomp_cur = Distance(ax, az, bx, bz);
  _mm_setcsr(mx & ~(kRoundBits | kFlushBits));
  const uint32_t recomp_canon = Distance(ax, az, bx, bz);
  _mm_setcsr(mx);
  char flags[8];
  int nf = 0;
  const bool fa = recomp_cur != result, fc = recomp_canon != result,
             fs = (mx & (kRoundBits | kFlushBits)) != (cached & (kRoundBits | kFlushBits)),
             fn = (mx & (kRoundBits | kFlushBits)) != 0;
  if (fa) flags[nf++] = 'A';
  if (fc) flags[nf++] = 'C';
  if (fs) flags[nf++] = 'S';
  if (fn) flags[nf++] = 'N';
  if (!nf) flags[nf++] = '-';
  flags[nf] = 0;
  const uint32_t gtid = rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread()->thread_id() : 0;
#ifdef _WIN32
  const uint32_t htid = GetCurrentThreadId();
#else
  const uint32_t htid = 0;
#endif
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  const uint32_t i = r30.u32 / 9, j = r31.u32;
  Probe& p = Instance();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (!p.opened) p.OpenLocked();
  ++p.calls;
  p.flag_a += fa;
  p.flag_c += fc;
  p.flag_s += fs;
  p.flag_n += fn;
  if (p.file) {
    std::fprintf(p.file, "%llu\t%u\t%u\t%u\t%u\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%s\n",
                 (unsigned long long)frame, htid, gtid, i, j, a, b, ax, az, bx, bz, result, recomp_cur, recomp_canon,
                 mx, cached, flags);
    if ((p.calls & 63) == 0) std::fflush(p.file);
  }
  if (flags[0] != '-' && p.anomaly_lines < kMaxAnomalyLines) {
    ++p.anomaly_lines;
    REXLOG_WARN("[wwe2k14][fpprobe] flags {} frame={} host thread {} guest thread {} pair {}-{} inputs {:08X} {:08X} "
                "{:08X} {:08X} result {:08X} recompute current {:08X} canonical {:08X} mxcsr {:08X} cached {:08X}",
                flags, frame, htid, gtid, i, j, ax, az, bx, bz, result, recomp_cur, recomp_canon, mx, cached);
  }
  if (p.calls % kSummaryCalls == 0) {
    REXLOG_INFO("[wwe2k14][fpprobe] {} calls (frame={}): A {} C {} S {} N {}", p.calls, frame, p.flag_a, p.flag_c,
                p.flag_s, p.flag_n);
    if (p.file) std::fflush(p.file);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Boot allocation trace (--wwe2k14_alloc_trace_frames=N, default 0 = off). Task A found that which of the two
// values the slot-1 position takes at the divergence follows the boot-time heap layout (the pointer at 0x836F3AAC
// is 0xAF4A0400 or 0xAF4A0450 in different boots, docs/systems/determinism.md). This logs every request to the
// game's two allocator entry points while the guest frame is below N, so two boots with different layouts can be
// compared request by request (tools/determinism/alloc_compare.py).
// Hooks (config/fp_probe_hooks.toml), each on the second instruction (after `mflr r12`, so r12 = the caller's
// return address; r3 = size in bytes, r4 = second argument, alignment or flags, unconfirmed):
//   sub_82C2DCD8 at 0x82C2DCDC and sub_82C2F5A8 at 0x82C2F5AC (sub_827E0F90, the function that stores
//   0x836F3AAC, calls one or the other depending on sub_82C2E1E0). Both pick a heap with sub_82C2D8C0 and round the
//   size up to 16 bytes (0x82C2DD20-0x82C2DD28). Other allocation paths are not traced.
// Output: out/harness/determinism/alloc_NNN.tsv (frame, host thread, guest thread, path, size, r4, caller), and
// at the first traced call at or after frame N one [wwe2k14][alloctrace] line with the words at 0x836F3A54,
// 0x836F3A98 and 0x836F3AAC (the boot heap pointers that vary), after which tracing stops.
REXCVAR_DEFINE_INT32(wwe2k14_alloc_trace_frames, 0, "wwe2k14",
                     "Determinism probe: log every allocation request (sub_82C2DCD8 / sub_82C2F5A8) during the first "
                     "N guest frames to out/harness/determinism/alloc_NNN.tsv (0 = off; see src/fp_probe.h)");

namespace wwe2k14::fpprobe {

struct AllocTrace {
  std::mutex mutex;
  FILE* file = nullptr;
  bool opened = false;
  std::atomic<bool> done{false};
  uint64_t rows = 0;
};

inline AllocTrace& Allocs() {
  static AllocTrace t;
  return t;
}

inline void AllocTraceHit(int path, uint32_t size, uint32_t arg, uint32_t caller) {
  const int32_t limit = REXCVAR_GET(wwe2k14_alloc_trace_frames);
  if (limit <= 0) return;
  AllocTrace& t = Allocs();
  if (t.done.load(std::memory_order_relaxed)) return;  // checked again under the lock
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  const uint32_t gtid = rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread()->thread_id() : 0;
#ifdef _WIN32
  const uint32_t htid = GetCurrentThreadId();
#else
  const uint32_t htid = 0;
#endif
  std::lock_guard<std::mutex> lock(t.mutex);
  if (t.done.load(std::memory_order_relaxed)) return;
  if (!t.opened) {
    t.opened = true;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::filesystem::path exe = std::filesystem::path(std::wstring(buf, n));
    const std::string folder = exe.parent_path().filename().string();
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::weakly_canonical(exe.parent_path() / ".." / ".." / "harness" / "determinism", ec);
    std::filesystem::create_directories(dir, ec);
    char num[16];
    std::snprintf(num, sizeof(num), "%03d", std::max(CurrentLogNumber(), 0));
    const std::string stem = folder == "win-amd64-release" ? "alloc_" + std::string(num)
                                                           : "alloc_" + folder + "_" + std::string(num);
    const std::filesystem::path path = dir / (stem + ".tsv");
    t.file = _fsopen(path.string().c_str(), "w", _SH_DENYNO);
    if (t.file) {
      std::fprintf(t.file, "frame\thtid\tgtid\tpath\tsize\targ\tcaller\n");
      REXLOG_INFO("[wwe2k14][alloctrace] writing {} (first {} frames)", path.string(), limit);
    } else {
      REXLOG_WARN("[wwe2k14][alloctrace] cannot write {}", path.string());
    }
#endif
  }
  if (frame >= uint64_t(limit)) {
    t.done.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[wwe2k14][alloctrace] stopped at frame {} after {} rows; boot heap pointers [0x836F3A54]={:08X} "
                "[0x836F3A98]={:08X} [0x836F3AAC]={:08X}",
                frame, t.rows, GuestU32(0x836F3A54), GuestU32(0x836F3A98), GuestU32(0x836F3AAC));
    if (t.file) {
      std::fclose(t.file);
      t.file = nullptr;
    }
    return;
  }
  ++t.rows;
  if (t.file) {
    std::fprintf(t.file, "%llu\t%u\t%u\t%d\t%u\t%u\t%08X\n", (unsigned long long)frame, htid, gtid, path, size, arg,
                 caller);
    if ((t.rows & 255) == 0) std::fflush(t.file);
  }
}

}  // namespace wwe2k14::fpprobe

// ---------------------------------------------------------------------------------------------------------------
// Boot allocation order (--wwe2k14_det_boot_alloc=true, default off): pins one word of the boot heap layout, the
// pointer at 0x836F3AAC. Evidence (task A, boots 335-339 with --wwe2k14_alloc_trace_frames=120): before the 1-byte
// object whose address is stored at 0x836F3AAC (sub_827E0F90, request from return address 0x827E0FB8, path 1) the
// boot thread always makes the same 1,449 requests, and guest thread 10 makes one 52-byte path-1 request through
// sub_82250050 (return address 0x82250080) that lands before the marker in some boots (335, 338) and after it in
// the others. 52 bytes = four 16-byte blocks plus, probably, one header block = 0x50, the distance between the two
// marker values. With the setting on, a path-1 request of 52 bytes from 0x82250080 on any thread other than the boot
// thread (the thread of the first allocation request) waits, before the allocator runs, until the boot thread's
// next request after the marker request enters this hook (so the marker allocation has returned), at most
// kBootAllocWaitMs. It does NOT give the whole boot a fixed layout: once released, the held request still races
// the boot thread's later requests (32, 4, 30384, 344 ... bytes), so it lands at a varying point after the marker
// (held boots 342 / 345: 3 boot-thread requests after it, a placement no natural boot showed; natural boots: 8, 19,
// 24 or 32 after it, or before it). The fixed insertion point is --wwe2k14_det_boot_alloc_place below (task 2,
// which also found that the match divergence does not follow this layout). One [wwe2k14][detalloc] line per held
// request (at most 8). The hold is given up for the rest of the run (one warning line) on the first timeout, or
// if the marker has not been seen by guest frame kBootAllocMaxFrame. Other boot-time races (other threads, path 2)
// are not changed.
REXCVAR_DEFINE_BOOL(wwe2k14_det_boot_alloc, false, "wwe2k14",
                    "Determinism experiment: hold one known racing boot allocation (guest thread 10's 52-byte request) "
                    "until the boot thread's 0x836F3AAC object is allocated, so that one pointer word is the same "
                    "every boot (only that word: the request still interleaves at varying points after it; default "
                    "off; see src/fp_probe.h)");

namespace wwe2k14::fpprobe {

constexpr uint32_t kMarkerCallerA = 0x827E0FB8;  // sub_827E0F90 after `bl 0x82c2dcd8` (path 1)
constexpr uint32_t kMarkerCallerB = 0x827E0FC0;  // sub_827E0F90 after `bl 0x82c2f5a8` (path 2)
constexpr uint32_t kRacingCaller = 0x82250080;   // sub_82250050 after `bl 0x82c2dcd8`
constexpr uint32_t kRacingSize = 52;
constexpr int kBootAllocWaitMs = 1000;
constexpr uint64_t kBootAllocMaxFrame = 30;  // the marker is set before frame 30 in every traced boot

inline std::atomic<uint32_t> g_boot_alloc_thread{0};  // host thread id of the first allocation request
// 0 before the marker request, 1 requested, 2 returned, 3 hold given up (timeout or no marker by kBootAllocMaxFrame)
inline std::atomic<int> g_marker_stage{0};
inline std::atomic<uint32_t> g_held_lines{0};
inline std::atomic<bool> g_heap_marker_logged{false};

// Stops all further holds this run; logs once, only if it was this call that gave up.
inline void AbandonBootAllocHold(const char* why, uint64_t frame) {
  int s = g_marker_stage.load(std::memory_order_acquire);
  while (s < 2 && !g_marker_stage.compare_exchange_weak(s, 3, std::memory_order_acq_rel)) {
  }
  if (s < 2) {
    REXLOG_WARN("[wwe2k14][detalloc] hold given up at frame {} ({}); no further requests are held this run", frame,
                why);
  }
}

inline void BootAllocOrder(int path, uint32_t size, uint32_t caller, uint32_t htid, uint64_t frame) {
  const int stage = g_marker_stage.load(std::memory_order_acquire);
  if (stage >= 2) return;  // plain load first: no locked read-modify-write once the marker is done
  if (frame >= kBootAllocMaxFrame) {
    AbandonBootAllocHold("the boot thread's 0x836F3AAC request was not seen", frame);
    return;
  }
  uint32_t boot = g_boot_alloc_thread.load(std::memory_order_acquire);
  if (boot == 0) {
    uint32_t none = 0;
    boot = g_boot_alloc_thread.compare_exchange_strong(none, htid, std::memory_order_acq_rel) ? htid : none;
  }
  if (htid == boot) {
    // CAS (boot thread only, at most twice per run) so a concurrent give-up (stage 3) is never overwritten.
    int expected = stage;
    if (caller == kMarkerCallerA || caller == kMarkerCallerB) {
      if (stage == 0) g_marker_stage.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
    } else if (stage == 1) {
      g_marker_stage.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
    }
    return;
  }
  if (path != 1 || size != kRacingSize || caller != kRacingCaller) return;
  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  const int before = stage;
  while (g_marker_stage.load(std::memory_order_acquire) < 2) {
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(kBootAllocWaitMs)) {
      timed_out = true;
      break;
    }
#ifdef _WIN32
    Sleep(0);
#endif
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
  if (timed_out) AbandonBootAllocHold("a held request timed out", frame);
  if (g_held_lines.fetch_add(1) < 8) {
    const uint32_t gtid =
        rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread()->thread_id() : 0;
    REXLOG_INFO("[wwe2k14][detalloc] {}-byte request from 0x{:08X} on guest thread {} {} the boot thread's 0x836F3AAC "
                "allocation (stage {} on entry, waited {} us{})",
                size, caller, gtid, timed_out ? "did NOT wait for" : "held until after", before, us.count(),
                timed_out ? ", timed out" : "");
  }
}

// Once per boot when either allocation setting is on: the boot heap pointers that vary between boots.
inline void LogHeapMarkers(uint64_t frame) {
  // Relaxed load first so allocations after the one log line do no locked read-modify-write.
  if (frame < 30 || g_heap_marker_logged.load(std::memory_order_relaxed) || g_heap_marker_logged.exchange(true)) {
    return;
  }
  REXLOG_INFO("[wwe2k14][alloctrace] heap markers at frame {}: [0x836F3A54]={:08X} [0x836F3A98]={:08X} "
              "[0x836F3AAC]={:08X}",
              frame, GuestU32(0x836F3A54), GuestU32(0x836F3A98), GuestU32(0x836F3AAC));
}

}  // namespace wwe2k14::fpprobe

// ---------------------------------------------------------------------------------------------------------------
// Fixed insertion point for the racing boot allocation (--wwe2k14_det_boot_alloc_place=N, default -1 = off; Session 8
// task 2). --wwe2k14_det_boot_alloc only keeps guest thread 10's 52-byte request (path 1, return address 0x82250080)
// after the 0x836F3AAC marker request; once released it still lands at a varying point. This setting places it at
// one chosen point instead: just before the boot thread's request number N after the marker request (the marker
// request itself is number 0, so N = 0 places it just before the marker: the layout of the boots that took the
// 0xAF4A0450 marker; N = 33 places it after 32 more boot-thread requests, the most common natural placement in the
// task A traces). Both threads wait for each other, each at most kBootAllocWaitMs:
//   - the racing request waits (before the allocator runs) until the boot thread has reached request N;
//   - the boot thread waits at the entry of request N until the racing request has returned (midasm hook at
//     0x82250080 in sub_82250050, the instruction after its `bl 0x82c2dcd8`; tools/analysis/list_function.py
//     0x82250050), so the two allocations cannot overlap in the allocator.
// If the boot thread passes request N before the racing request is seen, or either wait times out, or nothing is
// placed by guest frame kBootAllocMaxFrame, the placement is given up for the run (one warning line). One
// [wwe2k14][detplace] line per wait. Other threads' boot requests (and path 2) are not ordered by this.
// Takes precedence over --wwe2k14_det_boot_alloc when both are set.
REXCVAR_DEFINE_INT32(wwe2k14_det_boot_alloc_place, -1, "wwe2k14",
                     "Determinism experiment: place guest thread 10's racing 52-byte boot allocation just before the "
                     "boot thread's Nth allocation request after its 0x836F3AAC request (0 = just before that "
                     "request); each thread waits for the other at most 1 s (-1 = off, default; see src/fp_probe.h)");

namespace wwe2k14::fpprobe {

// 0 idle; 1 the racing request waits for the boot thread; 2 the boot thread waits at request N; 3 the racing request
// is allocating; 4 placed (it returned); 5 given up.
inline std::atomic<int> g_place_state{0};
inline std::atomic<int64_t> g_place_boot_count{-1};  // boot-thread requests since the marker request (= 0); -1 before
inline thread_local bool t_place_inflight = false;
inline std::atomic<uint32_t> g_place_lines{0};

inline uint32_t GuestThreadId() {
  return rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread()->thread_id() : 0;
}

inline void PlaceAbandon(const char* why, uint64_t frame) {
  int s = g_place_state.load(std::memory_order_acquire);
  while (s < 4 && !g_place_state.compare_exchange_weak(s, 5, std::memory_order_acq_rel)) {
  }
  if (s < 4) {
    REXLOG_WARN("[wwe2k14][detplace] placement given up at frame {} ({}); boot-thread requests since the marker: {}",
                frame, why, g_place_boot_count.load(std::memory_order_relaxed));
  }
}

inline void BootAllocPlace(int path, uint32_t size, uint32_t caller, uint32_t htid, uint64_t frame, int32_t place) {
  if (g_place_state.load(std::memory_order_acquire) >= 4) return;  // plain load once placed or given up
  if (frame >= kBootAllocMaxFrame) {
    PlaceAbandon("nothing placed by guest frame 30", frame);
    return;
  }
  uint32_t boot = g_boot_alloc_thread.load(std::memory_order_acquire);
  if (boot == 0) {
    uint32_t none = 0;
    boot = g_boot_alloc_thread.compare_exchange_strong(none, htid, std::memory_order_acq_rel) ? htid : none;
  }
  if (htid == boot) {
    int64_t n = g_place_boot_count.load(std::memory_order_relaxed);  // only the boot thread writes it
    if (n < 0) {
      if (caller != kMarkerCallerA && caller != kMarkerCallerB) return;
      n = 0;
    } else {
      ++n;
    }
    g_place_boot_count.store(n, std::memory_order_release);
    if (n != place) return;
    int s = g_place_state.load(std::memory_order_acquire);
    while (s < 2 && !g_place_state.compare_exchange_weak(s, 2, std::memory_order_acq_rel)) {
    }
    if (s >= 2) return;  // given up already
    const auto start = std::chrono::steady_clock::now();
    bool timed_out = false;
    while (g_place_state.load(std::memory_order_acquire) < 4) {
      if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(kBootAllocWaitMs)) {
        timed_out = true;
        break;
      }
#ifdef _WIN32
      Sleep(0);
#endif
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    if (timed_out) PlaceAbandon("the boot thread timed out waiting for the racing request", frame);
    const bool placed = g_place_state.load(std::memory_order_acquire) == 4;
    if (g_place_lines.fetch_add(1) < 8) {
      REXLOG_INFO("[wwe2k14][detplace] boot thread (guest thread {}) waited {} us at its request {} after the marker "
                  "(caller 0x{:08X}, {} bytes) for the racing request: {}",
                  GuestThreadId(), us.count(), n, caller, size,
                  placed ? "placed" : (timed_out ? "NOT placed (timed out)" : "NOT placed (given up)"));
    }
    return;
  }
  if (path != 1 || size != kRacingSize || caller != kRacingCaller) return;
  int s = 0;
  g_place_state.compare_exchange_strong(s, 1, std::memory_order_acq_rel);  // 0 -> 1: waiting
  if (s >= 3) return;  // another request is being placed, or given up
  if (g_place_boot_count.load(std::memory_order_acquire) > place) {
    PlaceAbandon("missed: the boot thread had passed the target request before the racing request came", frame);
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  for (;;) {
    int cur = g_place_state.load(std::memory_order_acquire);
    if (cur == 2) {
      if (g_place_state.compare_exchange_strong(cur, 3, std::memory_order_acq_rel)) break;
      continue;
    }
    if (cur >= 3) return;  // given up meanwhile
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(kBootAllocWaitMs)) {
      timed_out = true;
      break;
    }
#ifdef _WIN32
    Sleep(0);
#endif
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
  if (timed_out) {
    PlaceAbandon("the racing request timed out waiting for the boot thread", frame);
  } else {
    t_place_inflight = true;  // the return hook (0x82250080) on this thread releases the boot thread
  }
  if (g_place_lines.fetch_add(1) < 8) {
    REXLOG_INFO("[wwe2k14][detplace] {}-byte request from 0x{:08X} on guest thread {} waited {} us for the boot "
                "thread's request {} after the marker: {}",
                size, caller, GuestThreadId(), us.count(), place, timed_out ? "NOT placed (timed out)" : "placing");
  }
}

inline void AllocHook(int path, uint32_t size, uint32_t arg, uint32_t caller) {
  const bool order = REXCVAR_GET(wwe2k14_det_boot_alloc);
  const int32_t place = REXCVAR_GET(wwe2k14_det_boot_alloc_place);
  if (!order && place < 0 && REXCVAR_GET(wwe2k14_alloc_trace_frames) <= 0) return;
#ifdef _WIN32
  const uint32_t htid = GetCurrentThreadId();
#else
  const uint32_t htid = 0;
#endif
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (place >= 0) {
    BootAllocPlace(path, size, caller, htid, frame, place);
  } else if (order) {
    BootAllocOrder(path, size, caller, htid, frame);
  }
  LogHeapMarkers(frame);
  AllocTraceHit(path, size, arg, caller);
}

}  // namespace wwe2k14::fpprobe

// sub_82250050 at 0x82250080 (its `bl 0x82c2dcd8` returned; r3 = the new block). Releases the boot thread when the
// placed request returns. Never changes a register.
void wwe2k14_alloc_place_ret(PPCRegister& r3) {
  if (REXCVAR_GET(wwe2k14_det_boot_alloc_place) < 0) return;
  using namespace wwe2k14::fpprobe;
  if (!t_place_inflight) return;
  t_place_inflight = false;
  g_place_state.store(4, std::memory_order_release);
  REXLOG_INFO("[wwe2k14][detplace] racing request placed: block 0x{:08X} on guest thread {} at frame {}", r3.u32,
              GuestThreadId(), wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
}

// sub_82C2DCD8 at 0x82C2DCDC / sub_82C2F5A8 at 0x82C2F5AC: r3 = size, r4 = second argument, r12 = caller's return
// address (mflr r12 just ran). Never changes a register; with --wwe2k14_det_boot_alloc it may delay one request.
void wwe2k14_alloc_trace_a(PPCRegister& r3, PPCRegister& r4, PPCRegister& r12) {
  wwe2k14::fpprobe::AllocHook(1, r3.u32, r4.u32, r12.u32);
}
void wwe2k14_alloc_trace_b(PPCRegister& r3, PPCRegister& r4, PPCRegister& r12) {
  wwe2k14::fpprobe::AllocHook(2, r3.u32, r4.u32, r12.u32);
}

// ---------------------------------------------------------------------------------------------------------------
// Skeleton update probe (--wwe2k14_skel_probe=true, default off; Session 8 task 2). Task A found that the first
// one-bit difference of a same-seed match pair is in slot 1's position x, written only by guest thread 7 in
// sub_82BD30E8 (about 2.5 calls per frame) and sub_82BD3620 (rare). Read from the listing
// (py tools/analysis/list_function.py 0x82BD30E8): this = r31; r28 = bone count; for each bone k it multiplies the
// 4x4 matrix at [this+180] + 64k (local) by the root matrix at this+96 (parent index -1) or by the already computed
// matrix of its parent (parent index = word at P+48+80k, P = r3 at 0x82BD31C8), with VMX vmulfp/vmaddfp, and stores
// the result at [this+192] + 64k; then (r27 != 0) it subtracts an offset (x at r1+80, z at r1+88, computed by
// sub_82280008 / sub_82C32BC0) from the translation x / z (+48 / +56) of every bone. Meaning of the matrices:
// interpretation from the code shape (bone transforms), unconfirmed.
// The recompiled vmaddfp is a separate multiply and add (no fma), so for equal inputs and equal MXCSR the result
// is the same in every run. This probe records, per call, a hash of every input and output and the MXCSR, so the
// first call where two runs differ shows which input differs first (or none: then the computation or the
// floating-point state):
//   0x82BD31C8 (bone loop start)  r3 = parent table, r28 = count, r31 = this, r27: hashes of the root matrix, the
//                                 parent indices and the local matrices; MXCSR and the cached csr;
//   0x82BD339C (loop done)         hash of the local matrices again (another thread writing them mid-call would show
//                                 here) and of the output matrices; MXCSR (VMX code: flush mode on);
//   0x82BD344C (offset computed)   the offset vector at r1+80 (16 bytes);
//   0x82BD35A4 (exit)              hash of the output matrices; MXCSR; writes the row.
//   0x82BD362C (sub_82BD3620 entry, after its prologue) r3 = this, r4 = a delta vector it adds to this+80..88 and to
//                                 every bone translation: one row with the delta and the old this+80..88.
// Output: out/harness/determinism/skel_NNN.tsv. With --wwe2k14_skel_probe_dump_from=F (and _dump_frames, default
// 300) the raw bytes of every call in frames F..F+frames-1 also go to skel_NNN.bin (record layout: see
// SkelWriteRecordLocked). Compare two runs with tools/determinism/skel_compare.py. Cost when off: one cvar read per
// hook call. sub_82BD30E8 runs for every skeleton object, not only the wrestlers: run 403 logged 969,642 calls over
// the 7,252 frames that had any (about 134 per frame; about 137 per frame in the match), 3 or 4 hooks each (the
// offset hook only runs when r27 != 0), so at most about 550 cvar reads per frame (each a call returning a static
// bool), plus the rare sub_82BD3620 hook.
REXCVAR_DEFINE_BOOL(wwe2k14_skel_probe, false, "wwe2k14",
                    "Determinism probe: hash the inputs and outputs of every skeleton update call (sub_82BD30E8, "
                    "sub_82BD3620) with MXCSR to out/harness/determinism/skel_NNN.tsv (default off; see "
                    "src/fp_probe.h)");
REXCVAR_DEFINE_INT32(wwe2k14_skel_probe_dump_from, -1, "wwe2k14",
                     "With --wwe2k14_skel_probe: also dump the raw input/output bytes of every call from this guest "
                     "frame on to skel_NNN.bin (-1 = off)");
REXCVAR_DEFINE_INT32(wwe2k14_skel_probe_dump_frames, 300, "wwe2k14",
                     "With --wwe2k14_skel_probe_dump_from: number of frames to dump");

namespace wwe2k14::fpprobe {

constexpr uint32_t kSkelMaxBones = 1024;

// memcpy from guest memory that returns false instead of crashing (as SafeCopy in src/frame_hash.h).
inline bool SkelSafeCopy(void* dst, const void* src, size_t n) {
  static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
#ifdef _WIN32
  __try {
    copy(dst, src, n);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
  return true;
#else
  copy(dst, src, n);
  return true;
#endif
}

inline bool SkelCopy(uint32_t guest, uint32_t length, std::string& out) {
  out.assign(length, '\0');
  if (length == 0) return true;
  if (guest < 0x10000 || uint64_t(guest) + length > 0x100000000ull) return false;
  if (uint64_t(guest) + length > 0x7F000000ull && guest < 0x80000000u) return false;  // MMIO heap
  auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest);
  if (!host) return false;
  return SkelSafeCopy(out.data(), host, length);
}

inline uint64_t Fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

inline uint32_t BeWord(const std::string& s, size_t off) {
  uint32_t v = 0;
  if (off + 4 <= s.size()) std::memcpy(&v, s.data() + off, 4);
  return _byteswap_ulong(v);
}

struct SkelCall {
  bool active = false, mid = false, off = false, ok_root = false, ok_par = false, ok_loc = false;
  uint64_t frame = 0;
  uint32_t self = 0, count = 0, flag27 = 0, parents = 0, local = 0, out = 0;
  uint64_t h_root = 0, h_par = 0, h_loc_in = 0, h_loc_mid = 0, h_out_mid = 0, h_out_exit = 0;
  uint32_t offset[4] = {0, 0, 0, 0};
  uint32_t mx_in = 0, mx_mid = 0, mx_exit = 0, csr_in = 0, csr_exit = 0;
  std::string root, par, loc;  // raw copies (big-endian guest bytes), kept for the dump
};
inline thread_local SkelCall t_skel;

struct SkelLog {
  std::mutex mutex;
  FILE* tsv = nullptr;
  FILE* bin = nullptr;
  bool opened = false;
  uint64_t rows = 0;
};
inline SkelLog& Skel() {
  static SkelLog s;
  return s;
}

inline void SkelOpenLocked(SkelLog& s) {
  s.opened = true;
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  const std::filesystem::path exe = std::filesystem::path(std::wstring(buf, n));
  const std::string folder = exe.parent_path().filename().string();
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::weakly_canonical(exe.parent_path() / ".." / ".." / "harness" / "determinism", ec);
  std::filesystem::create_directories(dir, ec);
  char num[16];
  std::snprintf(num, sizeof(num), "%03d", std::max(CurrentLogNumber(), 0));
  const std::string stem =
      folder == "win-amd64-release" ? "skel_" + std::string(num) : "skel_" + folder + "_" + std::string(num);
  const std::filesystem::path path = dir / (stem + ".tsv");
  s.tsv = _fsopen(path.string().c_str(), "w", _SH_DENYNO);
  if (!s.tsv) {
    REXLOG_WARN("[wwe2k14][skelprobe] cannot write {}", path.string());
  } else {
    std::fprintf(s.tsv,
                 "kind\tframe\tgtid\tself\tcount\tr27\tparents\tlocal\tout\th_root\th_par\th_loc_in\th_loc_mid\t"
                 "h_out_mid\toff_x\toff_y\toff_z\toff_w\th_out_exit\tmx_in\tmx_mid\tmx_exit\tcsr_in\tcsr_exit\n");
    REXLOG_INFO("[wwe2k14][skelprobe] writing {}", path.string());
  }
  if (REXCVAR_GET(wwe2k14_skel_probe_dump_from) >= 0) {
    const std::filesystem::path bpath = dir / (stem + ".bin");
    s.bin = _fsopen(bpath.string().c_str(), "wb", _SH_DENYNO);
    if (s.bin) REXLOG_INFO("[wwe2k14][skelprobe] raw dump {}", bpath.string());
  }
#endif
}

inline bool SkelDumpFrame(uint64_t frame) {
  const int32_t from = REXCVAR_GET(wwe2k14_skel_probe_dump_from);
  const int32_t frames = std::max(0, REXCVAR_GET(wwe2k14_skel_probe_dump_frames));
  return from >= 0 && frame >= uint64_t(from) && frame < uint64_t(from) + uint64_t(frames);
}

// skel_NNN.bin record: "SKL1", u32 kind (1 = sub_82BD30E8 call, 2 = sub_82BD3620 call), u64 frame, u32 gtid,
// u32 self, u32 count (little-endian), then for kind 1: root matrix (64), parent table span (count * 80 from P),
// local matrices (count * 64, at the loop start), output matrices (count * 64, at exit), offset (16); for kind 2:
// delta (16), old this+80..+95 (16). Guest bytes as in memory (big-endian).
inline void SkelWriteRecordLocked(SkelLog& s, uint32_t kind, uint64_t frame, uint32_t gtid, uint32_t self,
                                  uint32_t count, std::initializer_list<const std::string*> parts) {
  if (!s.bin) return;
  std::fwrite("SKL1", 1, 4, s.bin);
  std::fwrite(&kind, 4, 1, s.bin);
  std::fwrite(&frame, 8, 1, s.bin);
  std::fwrite(&gtid, 4, 1, s.bin);
  std::fwrite(&self, 4, 1, s.bin);
  std::fwrite(&count, 4, 1, s.bin);
  for (const std::string* p : parts) std::fwrite(p->data(), 1, p->size(), s.bin);
}

}  // namespace wwe2k14::fpprobe

// sub_82BD30E8 at 0x82BD31C8 (bone loop start): r3 = parent table, r27 = second argument, r28 = bone count,
// r31 = this. Log only.
void wwe2k14_skel_in(PPCRegister& r3, PPCRegister& r27, PPCRegister& r28, PPCRegister& r31, PPCFPSCRRegister& fpscr) {
  if (!REXCVAR_GET(wwe2k14_skel_probe)) return;
  using namespace wwe2k14::fpprobe;
  SkelCall& c = t_skel;
  c.active = true;
  c.mid = c.off = false;
  c.frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  c.mx_in = _mm_getcsr();
  c.csr_in = fpscr.csr;
  c.self = r31.u32;
  c.count = int32_t(r28.u32) < 0 ? 0 : std::min<uint32_t>(r28.u32, kSkelMaxBones);
  c.flag27 = r27.u32;
  c.parents = r3.u32;
  std::string w;
  c.local = SkelCopy(c.self + 180, 4, w) ? BeWord(w, 0) : 0;
  c.out = SkelCopy(c.self + 192, 4, w) ? BeWord(w, 0) : 0;
  c.ok_root = SkelCopy(c.self + 96, 64, c.root);
  c.ok_par = SkelCopy(c.parents, c.count * 80, c.par);
  c.ok_loc = SkelCopy(c.local, c.count * 64, c.loc);
  c.h_root = c.ok_root ? Fnv(c.root) : 0;
  std::string idx;
  idx.reserve(c.count * 4);
  for (uint32_t k = 0; c.ok_par && k < c.count; ++k) idx.append(c.par, 48 + 80 * k, 4);
  c.h_par = c.ok_par ? Fnv(idx) : 0;
  c.h_loc_in = c.ok_loc ? Fnv(c.loc) : 0;
}

// 0x82BD339C (bone loop done). Log only.
void wwe2k14_skel_mid(PPCRegister& r31) {
  if (!REXCVAR_GET(wwe2k14_skel_probe)) return;
  using namespace wwe2k14::fpprobe;
  SkelCall& c = t_skel;
  if (!c.active || c.self != r31.u32) return;
  c.mid = true;
  c.mx_mid = _mm_getcsr();
  std::string tmp;
  c.h_loc_mid = SkelCopy(c.local, c.count * 64, tmp) ? Fnv(tmp) : 0;
  if (c.h_loc_mid != 0 && c.ok_loc && c.h_loc_mid != c.h_loc_in && tmp.size() == c.loc.size()) {
    // Another thread wrote the local matrices while this call read them: log which words changed (first lines).
    static std::atomic<uint32_t> lines{0};
    if (lines.fetch_add(1) < 60) {
      std::string what;
      uint32_t words = 0;
      for (size_t off = 0; off + 4 <= tmp.size(); off += 4) {
        if (std::memcmp(tmp.data() + off, c.loc.data() + off, 4) == 0) continue;
        if (++words <= 6) {
          char b[96];
          std::snprintf(b, sizeof(b), " bone %zu +%zu %08X->%08X", off / 64, off % 64, BeWord(c.loc, off),
                        BeWord(tmp, off));
          what += b;
        }
      }
      REXLOG_INFO("[wwe2k14][skelprobe] local matrices of {:08X} changed during the call at frame {} (guest thread "
                  "{}): {} words:{}",
                  c.self, c.frame, GuestThreadId(), words, what);
    }
  }
  c.h_out_mid = SkelCopy(c.out, c.count * 64, tmp) ? Fnv(tmp) : 0;
}

// 0x82BD344C (after the second `bl 0x82c32bc0`): the offset vector at r1+80. Log only.
void wwe2k14_skel_off(PPCRegister& r1) {
  if (!REXCVAR_GET(wwe2k14_skel_probe)) return;
  using namespace wwe2k14::fpprobe;
  SkelCall& c = t_skel;
  if (!c.active) return;
  c.off = true;
  std::string v;
  SkelCopy(r1.u32 + 80, 16, v);
  for (int k = 0; k < 4; ++k) c.offset[k] = BeWord(v, 4 * k);
}

// 0x82BD35A4 (exit; also reached by the early returns, which never passed the loop-start hook). Log only.
void wwe2k14_skel_exit(PPCRegister& r31, PPCFPSCRRegister& fpscr) {
  if (!REXCVAR_GET(wwe2k14_skel_probe)) return;
  using namespace wwe2k14::fpprobe;
  SkelCall& c = t_skel;
  if (!c.active || c.self != r31.u32) return;
  c.active = false;
  c.mx_exit = _mm_getcsr();
  c.csr_exit = fpscr.csr;
  std::string outb;
  const bool ok_out = SkelCopy(c.out, c.count * 64, outb);
  c.h_out_exit = ok_out ? Fnv(outb) : 0;
  const uint32_t gtid = GuestThreadId();
  SkelLog& s = Skel();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.opened) SkelOpenLocked(s);
  ++s.rows;
  if (s.tsv) {
    std::fprintf(s.tsv,
                 "S\t%llu\t%u\t%08X\t%u\t%u\t%08X\t%08X\t%08X\t%016llX\t%016llX\t%016llX\t%016llX\t%016llX\t%08X\t"
                 "%08X\t%08X\t%08X\t%016llX\t%08X\t%08X\t%08X\t%08X\t%08X\n",
                 (unsigned long long)c.frame, gtid, c.self, c.count, c.flag27, c.parents, c.local, c.out,
                 (unsigned long long)c.h_root, (unsigned long long)c.h_par, (unsigned long long)c.h_loc_in,
                 c.mid ? (unsigned long long)c.h_loc_mid : 0ull, c.mid ? (unsigned long long)c.h_out_mid : 0ull,
                 c.off ? c.offset[0] : 0u, c.off ? c.offset[1] : 0u, c.off ? c.offset[2] : 0u,
                 c.off ? c.offset[3] : 0u, (unsigned long long)c.h_out_exit, c.mx_in, c.mid ? c.mx_mid : 0u,
                 c.mx_exit, c.csr_in, c.csr_exit);
    if ((s.rows & 63) == 0) std::fflush(s.tsv);
  }
  if (s.bin && SkelDumpFrame(c.frame)) {
    std::string offb(16, '\0');
    for (int k = 0; k < 4; ++k) {
      const uint32_t be = _byteswap_ulong(c.offset[k]);
      std::memcpy(offb.data() + 4 * k, &be, 4);
    }
    SkelWriteRecordLocked(s, 1, c.frame, gtid, c.self, c.count, {&c.root, &c.par, &c.loc, &outb, &offb});
    std::fflush(s.bin);
  }
}

// sub_82BD3620 at 0x82BD362C (after its prologue, before `lfs f0,0(r4)`): r3 = this, r4 = the delta vector. Log only.
void wwe2k14_skel_shift(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(wwe2k14_skel_probe)) return;
  using namespace wwe2k14::fpprobe;
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  std::string delta, old;
  SkelCopy(r4.u32, 16, delta);
  SkelCopy(r3.u32 + 80, 16, old);
  const uint32_t gtid = GuestThreadId();
  SkelLog& s = Skel();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.opened) SkelOpenLocked(s);
  if (s.tsv) {
    // Same columns as an S row: the old this+80..+88 in parents / local / out, the delta in off_x..off_w.
    std::fprintf(s.tsv,
                 "D\t%llu\t%u\t%08X\t0\t0\t%08X\t%08X\t%08X\t0\t0\t0\t0\t0\t%08X\t%08X\t%08X\t%08X\t0\t%08X\t0\t0\t0\t"
                 "0\n",
                 (unsigned long long)frame, gtid, r3.u32, BeWord(old, 0), BeWord(old, 4), BeWord(old, 8),
                 BeWord(delta, 0), BeWord(delta, 4), BeWord(delta, 8), BeWord(delta, 12), _mm_getcsr());
  }
  if (s.bin && SkelDumpFrame(frame)) {
    SkelWriteRecordLocked(s, 2, frame, gtid, r3.u32, 0, {&delta, &old});
    std::fflush(s.bin);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Make the motion decompression synchronous (--wwe2k14_det_inline_motion=true, default off; Session 8 task 2
// experiment; the hook function is wwe2k14_det_sync_motion). The skeleton probe and a memory watch (run 392) found
// that the local bone matrices the update thread (guest thread 7) reads in sub_82BD30E8 are also written by the motion decompression worker thread (sub_823B7AE0 ->
// sub_823FA818 -> sub_823DB1C0 -> sub_82296630; guest thread 42 in run 392) while thread 7 is reading them: a race
// that causes transient skeleton differences between runs; whether it causes the one-bit match divergence is
// unconfirmed (docs/systems/determinism.md, Result 6). The update thread posts a
// character slot to the worker in sub_823FABB8 (bl sub_823B7B90 at 0x823FACB4: clears the slot's event, sets its
// in-flight word worker+64+4*slot, sets its pending word worker+100+4*slot to 0, wakes the worker); the worker
// decompresses, signals the slot's event and sets the pending word to 1 (sub_823B7AE0 0x823B7B50-0x823B7B60); the
// update thread later waits for the event and finishes in sub_823B7C38. With this setting on, the hook right after
// the post (0x823FACB8) waits until the slot's pending word is non-zero, so the worker's writes always land at the
// same point of the update thread's frame (as if the worker were infinitely fast); everything else runs as before.
// Worker pointer: the word at 0x836F3A98 (db/cutscene_motion.json); slot: owner +1220 (the value the post used).
// At most kSyncMotionWaitMs per post; the first 4 timeouts are logged and the setting keeps trying on later posts.
// Log: one line at the first post, then a summary every kSyncMotionLogFrames guest frames while posts happen
// (posts seen, posts that had to wait, spins, total and longest wait, timeouts; counts are for the whole run).
REXCVAR_DEFINE_BOOL(wwe2k14_det_inline_motion, false, "wwe2k14",
                    "Determinism experiment: make motion decompression synchronous: after the update thread posts a "
                    "character to the decompression worker, wait until the worker has finished it (default off; see "
                    "src/fp_probe.h)");

namespace wwe2k14::fpprobe {
constexpr int kSyncMotionWaitMs = 100;
constexpr uint64_t kSyncMotionLogFrames = 600;  // summary cadence (guest frames, about 10 s at 60 fps)
inline std::atomic<uint64_t> g_sync_waits{0}, g_sync_spins{0}, g_sync_timeouts{0};
inline std::atomic<uint64_t> g_sync_waited{0}, g_sync_wait_us{0}, g_sync_max_us{0};
inline std::atomic<uint64_t> g_sync_next_log_frame{0};
}  // namespace wwe2k14::fpprobe

// sub_823FABB8 at 0x823FACB8 (after `bl 0x823b7b90` returned): r30 = owner (character) object. Never changes a
// register or guest memory; with the setting on it may wait.
void wwe2k14_det_sync_motion(PPCRegister& r30) {
  if (!REXCVAR_GET(wwe2k14_det_inline_motion)) return;
  using namespace wwe2k14::fpprobe;
  const uint32_t worker = GuestU32(0x836F3A98);
  const uint32_t slot = GuestU32(r30.u32 + 1220);
  if (worker == 0 || slot >= 9) return;
  const uint32_t pending = worker + 100 + 4 * slot;
  const auto start = std::chrono::steady_clock::now();
  uint64_t spins = 0;
  bool timed_out = false;
  while (GuestU32(pending) == 0) {
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(kSyncMotionWaitMs)) {
      timed_out = true;
      break;
    }
    ++spins;
#ifdef _WIN32
    Sleep(0);
#endif
  }
  const uint64_t n = g_sync_waits.fetch_add(1) + 1;
  if (spins != 0) {
    const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count());
    g_sync_spins.fetch_add(spins);
    g_sync_waited.fetch_add(1);
    g_sync_wait_us.fetch_add(us);
    uint64_t prev = g_sync_max_us.load(std::memory_order_relaxed);
    while (us > prev && !g_sync_max_us.compare_exchange_weak(prev, us)) {
    }
  }
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (timed_out && g_sync_timeouts.fetch_add(1) < 4) {
    REXLOG_WARN("[wwe2k14][detmotion] worker did not finish slot {} within {} ms (frame {})", slot,
                kSyncMotionWaitMs, frame);
  }
  uint64_t next = g_sync_next_log_frame.load(std::memory_order_relaxed);
  if ((n == 1 || frame >= next) &&
      g_sync_next_log_frame.compare_exchange_strong(next, frame + kSyncMotionLogFrames)) {
    REXLOG_INFO("[wwe2k14][detmotion] frame {}: {} posts seen ({} had to wait; worker 0x{:08X}, this slot {}); "
                "{} spins, wait total {} us, longest {} us, {} timeouts",
                frame, n, g_sync_waited.load(), worker, slot, g_sync_spins.load(), g_sync_wait_us.load(),
                g_sync_max_us.load(), g_sync_timeouts.load());
  }
}
