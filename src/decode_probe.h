// wwe2k14 - data-block decoder probe (Session 8 task 3, "decoder study"). Default OFF. LOG ONLY.
//
// CData::Copy (sub_82C35798, db cdata_copy) picks a decoder by the 4-byte tag at the start of the stored block
// (tools/analysis/list_function.py 0x82C35798: it builds the words 'BPE ', 'ZLIB', 'LZ4 ' on its stack and compares
// them with the block's first word):
//   'BPE '  -> sub_82C41918 (db unknown_block_decoder): byte-pair encoding (Gage-style pair tables; analysis in
//              docs/research/decoder_native.md). Arguments r3 = block (tag at +0, expanded size as a little-endian
//              32-bit word at +12, stream at +16), r4 = destination. No callees.
//   'ZLIB'  -> sub_82C41738 (db zlib_decompress_buffer).
//   'LZ4 '  -> sub_82C50408 (never seen running in coverage).
//   other   -> plain copy.
// This probe overrides the recompiled entry symbols of sub_82C41918 and sub_82C41738 (they are weak aliases of
// __imp__sub_..., generated/default/wwe2k14_pch.h DEFINE_REX_FUNC; rex/hook.h REX_HOOK_RAW) and always calls the
// original body unchanged. With the settings off each override is one setting read and a call.
//
// What the guest leaves in its registers (listing of sub_82C41918, and the generated body, which keeps every guest
// register in the context struct): r5 is the output pointer (0x82C41934 mr r5,r4; written at 0x82C41ADC stb
// r9,0(r5)), r6 the stream pointer, pre-incremented (0x82C41960 addi r6,r10,-1 with r10 = block+16; reads lbzu), and
// the exit (0x82C41B18..0x82C41B24, __restgprlr_26) touches neither. So after the call, r5 - dst = the bytes the
// guest really wrote and r6 + 1 - (block + 16) = the stream bytes it read (both only when the expanded size is not
// 0: with size 0 the guest branches out before r6 is set). The probe uses these for every call, shadow or not.
//
//   --wwe2k14_decode_probe=true    time every call of the two decoders (host clock around the recompiled body) and
//        log:
//          "[wwe2k14][decode] call bpe ..."  one line per BPE call that took >= wwe2k14_decode_probe_call_ms
//              (frame, host tid, guest caller return address lr, src, dst, expanded = the declared size at +12, in =
//              stream bytes the guest read + 16, and with _shadow the block count, native ms, match; lr2 = the
//              caller's own return address, from the guest stack back chain; produced = bytes the guest wrote;
//              shadow_ms = the whole shadow cost of the call: allocation + native decode + compare + free);
//          "[wwe2k14][decode] frame ..."  per host thread and guest frame, when that thread's decode time in that
//              frame was >= wwe2k14_decode_probe_frame_ms (flushed at the thread's next call in a later frame, so a
//              line can appear late; the frame number in it is the one the calls ran in);
//          "[wwe2k14][decode] totals ..."  running totals every 600 guest frames (first call after each boundary;
//              calls after the last boundary of a run are not in any totals line: the usual close hard-exits in
//              ReXApp::OnClosing, which is private, so there is no hook for a final line). Fields: produced = sum
//              of bytes the guest wrote, overrun = calls whose written bytes != the declared size, max_overrun =
//              the largest (written - declared), stream_mismatch = shadow calls whose stream length differs from
//              the native decoder's, shadow_ms = whole shadow cost (native_ms is the decode part of it).
//        "frame" = g_guest_frame (frame_stats.h, VdSwap calls so far) when the call started.
//   --wwe2k14_decode_probe_shadow=true   (needs the probe on) before each BPE call, decode the same block natively
//        into a freshly allocated host buffer (not zeroed, not touched before the timed decode, so large blocks pay
//        their first-touch page faults inside native_ms; the guest destination may or may not be resident, so
//        which way this biases native vs recompiled is NOT established), time it, then after the recompiled call
//        compare: same written length (guest r5 vs native), same stream length (guest r6 vs native), and the bytes
//        up to the written length. Never writes guest memory. Mismatches / native errors are counted and the
//        first 8 logged.
//
// Session 11 additions (the decoder itself is src/native_bpe.h, shared with the offline test tools/bpe/bpe_test.cpp):
//   --wwe2k14_bpe_corpus=true   record every call (input, parameters, output) for the offline test; layout below.
//   --wwe2k14_native_bpe (DEFAULT ON since Session 11, Jarrod's decision 2026-10-10 20:10; =false = the game's own
//        decoder)   replace the guest decoder: decode natively straight into the guest destination
//        (bounded by the declared size), set r3 / r4 / r5 / r6 as the guest would; on any bound (or a destination
//        that could overlap the input) run the guest body instead. "[wwe2k14][native_bpe] active" once, totals every
//        600 guest frames.
//   --wwe2k14_native_bpe_verify=true   run both (native into a host buffer, then the guest), compare written length,
//        stream length and bytes; on a difference log it with the input hash and save the input + the game's output
//        to out/harness/bpe/mismatch/<hash>.bpe (corpus format). The game's output is always kept. Wins over
//        _native_bpe. The corpus recorder always records the game's own decoder (it wins over both); the probe's
//        shadow mode and its guest timing need --wwe2k14_native_bpe=false (with native on, the probe reports the
//        native calls' times instead).
//
// Cost when on: two clock reads per call, one thread_local accumulator update, a few atomic adds; with _shadow a
// host allocation of the expanded size + 64 KiB, a native decode, a compare and a free per BPE call (shadow_ms).

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <chrono>
#include <functional>
#include <thread>
#endif

#include "frame_stats.h"
#include "native_bpe.h"

REXCVAR_DEFINE_BOOL(wwe2k14_decode_probe, false, "wwe2k14",
                    "Time every call of the game's BPE (sub_82C41918) and zlib (sub_82C41738) block decoders and log "
                    "[wwe2k14][decode] lines (Session 8 task 3; log only)");
REXCVAR_DEFINE_BOOL(wwe2k14_decode_probe_shadow, false, "wwe2k14",
                    "With wwe2k14_decode_probe: also decode each BPE block natively into a host buffer, time it and "
                    "compare with the game's output (never writes guest memory)");
REXCVAR_DEFINE_DOUBLE(wwe2k14_decode_probe_call_ms, 0.25, "wwe2k14",
                      "wwe2k14_decode_probe: log one line per BPE call at least this long (ms)");
REXCVAR_DEFINE_DOUBLE(wwe2k14_decode_probe_frame_ms, 1.0, "wwe2k14",
                      "wwe2k14_decode_probe: log a per-thread per-frame total when its decode time is at least this "
                      "(ms)");
REXCVAR_DEFINE_BOOL(wwe2k14_bpe_corpus, false, "wwe2k14",
                    "Record every call of the BPE decoder (sub_82C41918): input, parameters and output to "
                    "out/harness/bpe/corpus/<hash>.bpe (one file per distinct input) plus one CSV row per call in "
                    "out/harness/bpe/calls/calls_NNN.csv (Session 11; game data, stays in out/)");
REXCVAR_DEFINE_BOOL(wwe2k14_native_bpe, true, "wwe2k14",
                    "Native BPE decoder (default on since Session 11, Jarrod 2026-10-10): replace the game's decoder "
                    "(sub_82C41918) with src/native_bpe.h, falling back to the game's own whenever a bound trips; "
                    "=false restores the game's decoder");
REXCVAR_DEFINE_BOOL(wwe2k14_native_bpe_verify, false, "wwe2k14",
                    "Run both BPE decoders, compare, keep the game's output; log [wwe2k14][native_bpe] mismatches and "
                    "save their inputs to out/harness/bpe/mismatch (Session 11; default off)");
REXCVAR_DEFINE_BOOL(wwe2k14_native_bpe_menus, false, "wwe2k14",
                    "Session 58: the native BPE decoder also outside the in-game sequence (menus, select screens, 30 "
                    "Years of WrestleMania). Off (default): the game's own decoder there, at the speed the game "
                    "expects (a menu preview built with the fast one left Bobby Heenan red in the match)");
REXCVAR_DEFINE_BOOL(wwe2k14_native_bpe_debug, false, "wwe2k14",
                    "Session 58 (diagnostic): with _verify, log [s58][bpe_tail] when the game's decoder changes bytes "
                    "in the 4 KB after what it produced");
REXCVAR_DEFINE_INT32(wwe2k14_native_bpe_debug_slow, 0, "wwe2k14",
                     "Session 58 (diagnostic): native BPE calls spin until they took N times their decode time (the "
                     "game's decoder takes ~5.5x); 0 / 1 = off")
    .range(0, 100);
REXCVAR_DEFINE_BOOL(wwe2k14_native_bpe_debug_regs, false, "wwe2k14",
                    "Session 58 (diagnostic): with _verify, the registers after the game's decoder are put back as the "
                    "native path leaves them (r3-r6 set, the rest as before the call)");

REX_EXTERN(__imp__sub_82C41918);
REX_EXTERN(__imp__sub_82C41738);

namespace wwe2k14::decode_probe {

// Host clock in ticks (QueryPerformanceCounter on Windows, steady_clock nanoseconds elsewhere).
#ifdef _WIN32
inline int64_t Ticks() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
inline double TicksPerMs() {
  static const double per_ms = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart) / 1000.0;
  }();
  return per_ms;
}
inline uint32_t HostTid() { return uint32_t(GetCurrentThreadId()); }
#else
inline int64_t Ticks() {
  return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count());
}
inline double TicksPerMs() { return 1e6; }
inline uint32_t HostTid() { return uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id())); }
#endif

inline double TicksMs(int64_t ticks) { return double(ticks) / TicksPerMs(); }

// Same address translation as the generated code (REX_RAW_ADDR in generated/default/wwe2k14_pch.h on Windows).
inline uint8_t* GuestPtr(uint8_t* base, uint32_t addr) {
  return base + addr + ((addr >= 0xE0000000u) ? 0x1000u : 0u);
}

// The native decoder lives in native_bpe.h since Session 11 (shared with the offline corpus test).
using BpeResult = native_bpe::Result;
inline BpeResult DecodeBpe(const uint8_t* block, uint8_t* out, uint32_t out_cap) {
  return native_bpe::Decode(block, out, out_cap);
}

// --- Corpus recorder (Session 11, --wwe2k14_bpe_corpus) ------------------------------------------------------------
// After each guest call: one file per distinct input (content-addressed by a 64-bit FNV-1a hash of the input bytes,
// out/harness/bpe/corpus/<hash>.bpe; written once, also across concurrent runs: temp file + rename) and one CSV row
// per call (out/harness/bpe/calls/calls_NNN.csv). File layout (host little-endian): "BPEC", version 1, src, dst,
// expanded, produced, consumed, in_len, out_len, reserved (u32 each), in_hash, out_hash (u64), then in_len input bytes (the
// 16-byte header + the stream bytes the guest read, from r6) and out_len output bytes (what the guest wrote, from
// r5). Game data: stays in out/ (gitignored).
inline uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

struct BpeFileHeader {
  char magic[4];
  uint32_t version, src, dst, expanded, produced, consumed, in_len, out_len, reserved;
  uint64_t in_hash, out_hash;
};
static_assert(sizeof(BpeFileHeader) == 56, "corpus header layout");

inline std::filesystem::path HarnessDir(const char* sub) {
  std::error_code ec;
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  const std::filesystem::path exe = std::filesystem::path(std::wstring(buf, n));
  std::filesystem::path dir =
      std::filesystem::weakly_canonical(exe.parent_path() / ".." / ".." / "harness" / "bpe" / sub, ec);
#else
  std::filesystem::path dir = std::filesystem::current_path() / sub;
#endif
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// Writes one corpus-format file (used by the recorder and by verify mode for mismatches). Returns true if written
// (false if it already existed or could not be written).
inline bool WriteBpeFile(const std::filesystem::path& path, const BpeFileHeader& h, const uint8_t* in,
                         const uint8_t* out) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return false;
  std::filesystem::path tmp = path;
  tmp += ".tmp" + std::to_string(HostTid());
  FILE* f = std::fopen(tmp.string().c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
  if (ok && h.in_len) ok = std::fwrite(in, 1, h.in_len, f) == h.in_len;
  if (ok && h.out_len) ok = std::fwrite(out, 1, h.out_len, f) == h.out_len;
  ok = (std::fclose(f) == 0) && ok;
  if (ok) {
    std::filesystem::rename(tmp, path, ec);  // a concurrent run may have won: then keep its copy
    if (ec) ok = false;
  }
  if (!ok) std::filesystem::remove(tmp, ec);
  return ok;
}

inline std::string HashName(uint64_t h) {
  char s[24];
  std::snprintf(s, sizeof(s), "%016llx", static_cast<unsigned long long>(h));
  return s;
}

struct Corpus {
  std::mutex mutex;
  bool opened = false;
  FILE* csv = nullptr;
  std::filesystem::path dir;
  std::unordered_map<uint64_t, uint64_t> seen;  // in_hash -> out_hash
  uint64_t calls = 0, unique = 0, files = 0, repeat_calls = 0, repeat_bytes = 0, all_bytes = 0, out_conflict = 0,
           overlap_skipped = 0;
  uint64_t next_report_frame = 600;

  void OpenLocked() {
    opened = true;
    dir = HarnessDir("corpus");
    char num[16];
    std::snprintf(num, sizeof(num), "%03d", CurrentLogNumber() > 0 ? CurrentLogNumber() : 0);
    const std::filesystem::path path = HarnessDir("calls") / ("calls_" + std::string(num) + ".csv");
    csv = std::fopen(path.string().c_str(), "w");
    if (csv) std::fprintf(csv, "frame,htid,lr,lr2,src,dst,expanded,produced,consumed,in_hash,out_hash,guest_us,new\n");
    REXLOG_INFO("[wwe2k14][bpe_corpus] recording to {} (calls: {})", dir.string(), path.string());
  }

  void ReportLocked(uint64_t frame) {
    REXLOG_INFO("[wwe2k14][bpe_corpus] at frame {}: calls={} unique_inputs={} files_written={} repeat_calls={} "
                "repeat_bytes={} all_bytes={} out_conflict={} overlap_skipped={}",
                frame, calls, unique, files, repeat_calls, repeat_bytes, all_bytes, out_conflict, overlap_skipped);
  }

  void Record(uint64_t frame, uint32_t lr, uint32_t lr2, uint32_t src, uint32_t dst, uint32_t expanded,
              uint32_t produced, uint32_t consumed, bool overlap, const uint8_t* in, const uint8_t* out,
              double guest_ms) {
    const uint32_t in_len = expanded ? 16u + consumed : 16u;
    // Hash outside the lock (pure reads of guest memory the guest just finished with).
    const uint64_t in_hash = Fnv1a64(in, in_len);
    const uint64_t out_hash = Fnv1a64(out, produced);
    std::lock_guard<std::mutex> lock(mutex);
    if (!opened) OpenLocked();
    ++calls;
    all_bytes += expanded;
    bool is_new = false;
    auto it = seen.find(in_hash);
    if (it == seen.end()) {
      seen.emplace(in_hash, out_hash);
      ++unique;
      is_new = true;
      if (overlap) {
        ++overlap_skipped;  // the input was overwritten by the output: not a valid corpus entry
      } else {
        BpeFileHeader h{{'B', 'P', 'E', 'C'}, 1, src, dst, expanded, produced, consumed, in_len, produced, 0, in_hash,
                        out_hash};
        if (WriteBpeFile(dir / (HashName(in_hash) + ".bpe"), h, in, out)) ++files;
      }
    } else {
      ++repeat_calls;
      repeat_bytes += expanded;
      if (it->second != out_hash) ++out_conflict;
    }
    if (csv) {
      std::fprintf(csv, "%llu,%u,0x%08X,0x%08X,0x%08X,0x%08X,%u,%u,%u,%s,%s,%.1f,%d\n",
                   static_cast<unsigned long long>(frame), HostTid(), lr, lr2, src, dst, expanded, produced, consumed,
                   HashName(in_hash).c_str(), HashName(out_hash).c_str(), guest_ms * 1000.0, is_new ? 1 : 0);
      std::fflush(csv);
    }
    if (frame >= next_report_frame) {
      next_report_frame = (frame / 600 + 1) * 600;
      ReportLocked(frame);
    }
  }
};
inline Corpus g_corpus;

struct Totals {
  // Times are host clock ticks (TicksPerMs), so nothing is truncated per call.
  std::atomic<uint64_t> bpe_calls{0}, bpe_out{0}, bpe_produced{0}, bpe_in{0}, bpe_blocks{0};
  std::atomic<uint64_t> bpe_guest_ticks{0}, bpe_native_ticks{0}, bpe_shadow_ticks{0};
  std::atomic<uint64_t> bpe_mismatch{0}, bpe_errors{0}, bpe_overlap{0}, bpe_shadowed{0}, bpe_stream_mismatch{0};
  std::atomic<uint64_t> bpe_overrun{0};     // calls whose guest-written length != the declared expanded size
  std::atomic<int64_t> bpe_max_overrun{0};  // largest (written - declared), bytes
  std::atomic<uint64_t> bpe_hist[5] = {};   // expanded size < 4 KiB, < 64 KiB, < 256 KiB, < 1 MiB, >= 1 MiB
  std::atomic<uint64_t> zlib_calls{0}, zlib_ticks{0};
  std::atomic<uint64_t> next_report_frame{600};
  std::atomic<int> logged_mismatch{0};
};
inline Totals g_totals;

struct ThreadAcc {
  uint64_t frame = ~0ull;
  uint32_t bpe_calls = 0, zlib_calls = 0;
  uint64_t bpe_out = 0, bpe_in = 0;
  double bpe_ms = 0, native_ms = 0, shadow_ms = 0, zlib_ms = 0, max_call_ms = 0;
  int decoder = 0;  // which BPE decoder ran this thread-frame: 0 the game's, 1 native, 2 verify (both)
};
inline thread_local ThreadAcc t_acc;

inline void FlushThread(uint64_t new_frame) {
  ThreadAcc& a = t_acc;
  if (a.frame != ~0ull && a.frame != new_frame) {
    if (a.bpe_ms + a.zlib_ms >= REXCVAR_GET(wwe2k14_decode_probe_frame_ms)) {
      REXLOG_INFO(
          "[wwe2k14][decode] frame {} tid {}: bpe calls={} out={} in={} ms={:.3f} native_ms={:.3f} max_call_ms={:.3f}; "
          "zlib calls={} ms={:.3f}; shadow_ms={:.3f}; decoder={}",
          a.frame, HostTid(), a.bpe_calls, a.bpe_out, a.bpe_in, a.bpe_ms, a.native_ms, a.max_call_ms, a.zlib_calls,
          a.zlib_ms, a.shadow_ms, a.decoder == 1 ? "native" : a.decoder == 2 ? "verify" : "guest");
    }
    a = ThreadAcc{};
  }
  a.frame = new_frame;
}

inline void MaybeReportTotals(uint64_t frame) {
  uint64_t next = g_totals.next_report_frame.load(std::memory_order_relaxed);
  if (frame < next) return;
  if (!g_totals.next_report_frame.compare_exchange_strong(next, (frame / 600 + 1) * 600)) return;
  const Totals& t = g_totals;
  REXLOG_INFO(
      "[wwe2k14][decode] totals at frame {}: bpe calls={} out={} in={} blocks={} guest_ms={:.1f} shadowed={} "
      "native_ms={:.1f} mismatch={} errors={} overlap={} hist(<4K,<64K,<256K,<1M,>=1M)={},{},{},{},{}; zlib calls={} "
      "ms={:.1f}; produced={} overrun={} max_overrun={} stream_mismatch={} shadow_ms={:.1f}",
      frame, t.bpe_calls.load(), t.bpe_out.load(), t.bpe_in.load(), t.bpe_blocks.load(),
      TicksMs(int64_t(t.bpe_guest_ticks.load())), t.bpe_shadowed.load(), TicksMs(int64_t(t.bpe_native_ticks.load())),
      t.bpe_mismatch.load(), t.bpe_errors.load(), t.bpe_overlap.load(), t.bpe_hist[0].load(), t.bpe_hist[1].load(),
      t.bpe_hist[2].load(), t.bpe_hist[3].load(), t.bpe_hist[4].load(), t.zlib_calls.load(),
      TicksMs(int64_t(t.zlib_ticks.load())), t.bpe_produced.load(), t.bpe_overrun.load(), t.bpe_max_overrun.load(),
      t.bpe_stream_mismatch.load(), TicksMs(int64_t(t.bpe_shadow_ticks.load())));
}

inline void BpeCall(PPCContext& __restrict ctx, uint8_t* base) {
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  FlushThread(frame);
  const uint32_t src = ctx.r3.u32;
  const uint32_t dst = ctx.r4.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  // One level further up: the caller's frame starts at the back chain word [r1]; its prologue (__savegprlr_NN or
  // stw r12,-8(r1)) saved its own return address at back_chain - 8 (same walk as guest_watch.h). 0 if implausible.
  uint32_t lr2 = 0;
  {
    const uint32_t sp = ctx.r1.u32;
    if (sp != 0 && (sp & 3) == 0) {
      const uint8_t* s = GuestPtr(base, sp);
      const uint32_t chain = (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | s[3];
      if (chain > sp && chain - sp < (1u << 20) && (chain & 3) == 0) {
        const uint8_t* c = GuestPtr(base, chain - 8);
        lr2 = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
      }
    }
  }
  const uint8_t* block = GuestPtr(base, src);
  const uint32_t expanded = uint32_t(block[12]) | (uint32_t(block[13]) << 8) | (uint32_t(block[14]) << 16) |
                            (uint32_t(block[15]) << 24);

  const bool probe_on = REXCVAR_GET(wwe2k14_decode_probe);
  const bool shadow_on = probe_on && REXCVAR_GET(wwe2k14_decode_probe_shadow);
  std::unique_ptr<uint8_t[]> host;
  BpeResult nr;
  bool shadowed = false;
  int64_t native_ticks = 0;
  int64_t shadow_ticks = 0;  // allocation + native decode + compare + free
  uint32_t cap = 0;
  if (shadow_on) {
    cap = expanded + 65536u;
    const int64_t s0 = Ticks();
    host.reset(cap >= expanded ? new (std::nothrow) uint8_t[cap] : nullptr);  // default-init: not zeroed
    if (host) {
      const int64_t n0 = Ticks();
      nr = DecodeBpe(block, host.get(), cap);
      const int64_t n1 = Ticks();
      native_ticks = n1 - n0;
      shadowed = true;
    }
    shadow_ticks = Ticks() - s0;
  }

  const int64_t t0 = Ticks();
  __imp__sub_82C41918(ctx, base);
  const int64_t guest_ticks = Ticks() - t0;
  const double guest_ms = TicksMs(guest_ticks);

  // What the guest really did (see the header): written bytes from r5, stream bytes from r6.
  const uint32_t produced = ctx.r5.u32 - dst;
  const uint32_t consumed = expanded ? ctx.r6.u32 + 1u - (src + 16u) : 0u;
  const int64_t overrun = int64_t(produced) - int64_t(expanded);
  if (overrun != 0) {
    g_totals.bpe_overrun.fetch_add(1, std::memory_order_relaxed);
    int64_t prev = g_totals.bpe_max_overrun.load(std::memory_order_relaxed);
    while (overrun > prev && !g_totals.bpe_max_overrun.compare_exchange_weak(prev, overrun)) {
    }
  }
  const bool overlap = expanded != 0 && uint64_t(dst) < uint64_t(src) + 16 + consumed &&
                       uint64_t(src) < uint64_t(dst) + produced;

  if (REXCVAR_GET(wwe2k14_bpe_corpus)) {
    g_corpus.Record(frame, lr, lr2, src, dst, expanded, produced, consumed, overlap, block, GuestPtr(base, dst),
                    guest_ms);
  }
  if (!probe_on) return;

  bool match = true;
  bool stream_ok = true;
  if (shadowed) {
    const int64_t c0 = Ticks();
    const uint8_t* g = GuestPtr(base, dst);
    // Read the guest destination only up to what the guest itself wrote (r5), never past it.
    const uint32_t n = nr.produced < produced ? nr.produced : produced;
    match = nr.error == 0 && nr.produced == produced && std::memcmp(g, host.get(), n) == 0;
    stream_ok = expanded == 0 || nr.consumed == consumed;
    uint32_t first = n;
    if (!match) {
      first = 0;
      while (first < n && g[first] == host[first]) ++first;
    }
    host.reset();
    shadow_ticks += Ticks() - c0;
    g_totals.bpe_shadowed.fetch_add(1, std::memory_order_relaxed);
    g_totals.bpe_blocks.fetch_add(nr.blocks, std::memory_order_relaxed);
    g_totals.bpe_native_ticks.fetch_add(uint64_t(native_ticks), std::memory_order_relaxed);
    if (nr.error) g_totals.bpe_errors.fetch_add(1, std::memory_order_relaxed);
    if (!match) g_totals.bpe_mismatch.fetch_add(1, std::memory_order_relaxed);
    if (!stream_ok) g_totals.bpe_stream_mismatch.fetch_add(1, std::memory_order_relaxed);
    if ((!match || !stream_ok) && g_totals.logged_mismatch.fetch_add(1) < 8) {
      REXLOG_INFO("[wwe2k14][decode] MISMATCH bpe frame={} src=0x{:08X} dst=0x{:08X} expanded={} guest_produced={} "
                  "native_produced={} guest_in={} native_in={} error={} first_diff={}",
                  frame, src, dst, expanded, produced, nr.produced, consumed, nr.consumed, nr.error, first);
    }
  }
  if (shadow_on) g_totals.bpe_shadow_ticks.fetch_add(uint64_t(shadow_ticks), std::memory_order_relaxed);
  if (overlap) g_totals.bpe_overlap.fetch_add(1, std::memory_order_relaxed);
  const double native_ms = TicksMs(native_ticks);
  const double shadow_ms = TicksMs(shadow_ticks);

  g_totals.bpe_calls.fetch_add(1, std::memory_order_relaxed);
  g_totals.bpe_out.fetch_add(expanded, std::memory_order_relaxed);
  g_totals.bpe_produced.fetch_add(produced, std::memory_order_relaxed);
  g_totals.bpe_in.fetch_add(expanded ? consumed + 16u : 0u, std::memory_order_relaxed);
  g_totals.bpe_guest_ticks.fetch_add(uint64_t(guest_ticks), std::memory_order_relaxed);
  const int bin = expanded < 4096 ? 0 : expanded < 65536 ? 1 : expanded < 262144 ? 2 : expanded < 1048576 ? 3 : 4;
  g_totals.bpe_hist[bin].fetch_add(1, std::memory_order_relaxed);

  ThreadAcc& a = t_acc;
  ++a.bpe_calls;
  a.bpe_out += expanded;
  a.bpe_in += expanded ? consumed + 16u : 0u;
  a.bpe_ms += guest_ms;
  a.native_ms += native_ms;
  a.shadow_ms += shadow_ms;
  if (guest_ms > a.max_call_ms) a.max_call_ms = guest_ms;

  if (guest_ms >= REXCVAR_GET(wwe2k14_decode_probe_call_ms)) {
    REXLOG_INFO("[wwe2k14][decode] call bpe frame={} tid={} lr=0x{:08X} src=0x{:08X} dst=0x{:08X} out={} in={} "
                "blocks={} guest_ms={:.3f} native_ms={:.3f} match={} overlap={} lr2=0x{:08X} produced={} "
                "shadow_ms={:.3f}",
                frame, HostTid(), lr, src, dst, expanded, expanded ? consumed + 16u : 0u, nr.blocks, guest_ms,
                native_ms, shadowed ? (match && stream_ok ? 1 : 0) : -1, overlap ? 1 : 0, lr2, produced, shadow_ms);
  }
  MaybeReportTotals(frame);
}

inline void ZlibCall(PPCContext& __restrict ctx, uint8_t* base) {
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  FlushThread(frame);
  const int64_t t0 = Ticks();
  __imp__sub_82C41738(ctx, base);
  const int64_t ticks = Ticks() - t0;
  g_totals.zlib_calls.fetch_add(1, std::memory_order_relaxed);
  g_totals.zlib_ticks.fetch_add(uint64_t(ticks), std::memory_order_relaxed);
  ++t_acc.zlib_calls;
  t_acc.zlib_ms += TicksMs(ticks);
  MaybeReportTotals(frame);
}

// --- Native replacement (Session 11, --wwe2k14_native_bpe / --wwe2k14_native_bpe_verify) ---------------------------
// Session 58: true while the game is in its in-game sequence (set by the app: load_probe.h includes this header).
// Null = always.
inline bool (*g_native_bpe_in_game)() = nullptr;
inline std::atomic<uint64_t> g_bpe_menu_guest_calls{0};  // calls left to the game's decoder outside the game

struct NativeStats {
  // guarded: native mode = calls the guest body ran because of a real overlap / straddle; verify mode = calls whose
  // destination is within the generous bound (native mode would stage them). staged: decoded via a scratch buffer.
  std::atomic<uint64_t> calls{0}, bytes{0}, zero{0}, native_ticks{0}, guest_ticks{0}, fallbacks{0}, guarded{0},
      staged{0}, verified{0}, mismatches{0}, native_errors{0};
  std::atomic<uint64_t> next_report_frame{600};
  std::atomic<bool> announced{false};
  std::atomic<int> logged{0};
};
inline NativeStats g_native;

inline void NativeReport(uint64_t frame, bool verify) {
  uint64_t next = g_native.next_report_frame.load(std::memory_order_relaxed);
  if (frame < next) return;
  if (!g_native.next_report_frame.compare_exchange_strong(next, (frame / 600 + 1) * 600)) return;
  const NativeStats& s = g_native;
  REXLOG_INFO("[wwe2k14][native_bpe] at frame {}: mode={} calls={} bytes={} zero={} native_ms={:.1f} guest_ms={:.1f} "
              "fallbacks={} guarded={} staged={} verified={} mismatches={} native_errors={} menu_guest={}",
              frame, verify ? "verify" : "native", s.calls.load(), s.bytes.load(), s.zero.load(),
              TicksMs(int64_t(s.native_ticks.load())), TicksMs(int64_t(s.guest_ticks.load())), s.fallbacks.load(),
              s.guarded.load(), s.staged.load(), s.verified.load(), s.mismatches.load(), s.native_errors.load(),
              g_bpe_menu_guest_calls.load());
}

// Saves the guest's input (header + the stream bytes the guest read) and output as a corpus-format file in
// out/harness/bpe/mismatch/, so it can be added to the corpus and fixed offline. Returns the input hash.
inline uint64_t SaveMismatch(const uint8_t* block, uint32_t src, uint32_t dst, uint32_t expanded, uint32_t produced,
                             uint32_t consumed, const uint8_t* guest_out) {
  const uint32_t in_len = 16u + consumed;
  const uint64_t in_hash = Fnv1a64(block, in_len);
  const BpeFileHeader h{{'B', 'P', 'E', 'C'}, 1, src, dst, expanded, produced, consumed, in_len, produced, 0, in_hash,
                        Fnv1a64(guest_out, produced)};
  WriteBpeFile(HarnessDir("mismatch") / (HashName(in_hash) + ".bpe"), h, block, guest_out);
  return in_hash;
}

inline void NativeCall(PPCContext& __restrict ctx, uint8_t* base, bool verify) {
  NativeStats& s = g_native;
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (!s.announced.exchange(true)) {
    REXLOG_INFO("[wwe2k14][native_bpe] active: mode={} (sub_82C41918 replaced by src/native_bpe.h{})",
                verify ? "verify" : "native", verify ? "; both run, the game's own output is kept" : "");
  }
  const uint32_t src = ctx.r3.u32;
  const uint32_t dst = ctx.r4.u32;
  const uint8_t* block = GuestPtr(base, src);
  const uint32_t expanded = native_bpe::ExpandedSize(block);
  s.calls.fetch_add(1, std::memory_order_relaxed);
  s.bytes.fetch_add(expanded, std::memory_order_relaxed);
  if (expanded == 0) {  // the guest returns at once (r3 = dst); nothing to gain
    s.zero.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82C41918(ctx, base);
    NativeReport(frame, verify);
    return;
  }
  // Writing natively into the destination is only safe when it cannot overlap the input (the guest re-decodes on a
  // fallback, so the input must survive) and the destination does not straddle the 0xE0000000 translation step. The
  // stream length is unknown before decoding: bound it generously (BPE stores at most ~1 + 2 bytes per code plus
  // per-block tables). 0 of ~9,900 calls overlapped in Session 8.
  const uint64_t d0 = dst, d1 = uint64_t(dst) + expanded;
  const uint64_t s0 = src, s1 = uint64_t(src) + 16u + 2ull * expanded + 65536u;
  const bool guarded = (d0 < s1 && s0 < d1) || (d0 < 0xE0000000ull && d1 > 0xE0000000ull) || d1 > 0x100000000ull;

  if (verify) {
    std::unique_ptr<uint8_t[]> host(new (std::nothrow) uint8_t[expanded]);
    native_bpe::Result r;
    if (host) {
      const int64_t n0 = Ticks();
      r = native_bpe::Decode(block, host.get(), expanded);  // same bound as the native mode
      s.native_ticks.fetch_add(uint64_t(Ticks() - n0), std::memory_order_relaxed);
    }
    // Session 58 (diagnostic, Heenan's red attire with native BPE only): the 4 KB after the decoded bytes and the
    // volatile registers, as they were before the guest body, to see whether it writes past what it produces or
    // leaves registers the native path does not set.
    // Only the rest of the page holding the last decoded byte: the page after it may not be mapped (run 3926).
    uint8_t tail_before[4096];
    const uint32_t tail_at = dst + (host ? r.produced : expanded);
    const uint32_t kTail = ((tail_at + 0xFFFu) & ~0xFFFu) - tail_at;
    const bool check_tail = REXCVAR_GET(wwe2k14_native_bpe_debug) && kTail != 0 && tail_at < 0xE0000000u;
    if (check_tail) std::memcpy(tail_before, GuestPtr(base, tail_at), kTail);
    std::unique_ptr<PPCContext> before;
    if (REXCVAR_GET(wwe2k14_native_bpe_debug_regs)) before.reset(new PPCContext(ctx));
    const int64_t g0 = Ticks();
    __imp__sub_82C41918(ctx, base);
    s.guest_ticks.fetch_add(uint64_t(Ticks() - g0), std::memory_order_relaxed);
    if (check_tail && host) {
      const uint8_t* after = GuestPtr(base, dst + r.produced);
      uint32_t first = kTail, last = 0;
      for (uint32_t i = 0; i < kTail; ++i) {
        if (after[i] != tail_before[i]) first = std::min(first, i), last = i;
      }
      if (first != kTail && s.logged.fetch_add(1) < 64) {
        REXLOG_WARN("[s58][bpe_tail] frame={} src=0x{:08X} dst=0x{:08X} expanded={} produced={}: the guest wrote "
                    "bytes {}-{} past what it produced", frame, src, dst, expanded, r.produced, first, last);
      }
    }
    if (before) {
      // As the native path leaves them: r3-r6 from the guest, everything else as before the call.
      const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
      ctx = *before;
      ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6;
    }
    if (guarded) s.guarded.fetch_add(1, std::memory_order_relaxed);
    if (host) {
      s.verified.fetch_add(1, std::memory_order_relaxed);
      const uint32_t produced = ctx.r5.u32 - dst;
      const uint32_t consumed = ctx.r6.u32 + 1u - (src + 16u);
      const uint8_t* g = GuestPtr(base, dst);
      const bool same = r.error == 0 && r.produced == produced && r.consumed == consumed &&
                        std::memcmp(g, host.get(), produced) == 0;
      if (!same) {
        (r.error ? s.native_errors : s.mismatches).fetch_add(1, std::memory_order_relaxed);
        if (s.logged.fetch_add(1) < 32) {
          const uint64_t h = SaveMismatch(block, src, dst, expanded, produced, consumed, g);
          REXLOG_WARN("[wwe2k14][native_bpe] {} frame={} in_hash={} src=0x{:08X} dst=0x{:08X} expanded={} "
                      "guest_produced={} native_produced={} guest_in={} native_in={} error={} (saved to "
                      "out/harness/bpe/mismatch; the game's output is kept)",
                      r.error ? "NATIVE_ERROR (native mode would fall back)" : "MISMATCH", frame, HashName(h), src,
                      dst, expanded, produced, r.produced, consumed, r.consumed, r.error);
        }
      }
    }
    NativeReport(frame, verify);
    return;
  }

  native_bpe::Result r;
  const int64_t n0 = Ticks();
  if (guarded) {
    // The generous bound says the destination may overlap the input. Decode into a scratch buffer, then check the
    // real ranges with the true stream length: no overlap -> the guest would have read exactly these input bytes, so
    // copy the result in ("staged"); overlap or a straddling destination -> run the guest body.
    const bool straddle = (d0 < 0xE0000000ull && d1 > 0xE0000000ull) || d1 > 0x100000000ull;
    std::unique_ptr<uint8_t[]> scratch(straddle ? nullptr : new (std::nothrow) uint8_t[expanded]);
    bool staged = false;
    if (scratch) {
      r = native_bpe::Decode(block, scratch.get(), expanded);
      const uint64_t in_end = uint64_t(src) + 16u + r.consumed;
      if (r.error == 0 && !(d0 < in_end && s0 < d0 + r.produced)) {
        std::memcpy(GuestPtr(base, dst), scratch.get(), r.produced);
        staged = true;
      }
    }
    if (!staged) {
      s.native_ticks.fetch_add(uint64_t(Ticks() - n0), std::memory_order_relaxed);
      s.guarded.fetch_add(1, std::memory_order_relaxed);
      const int64_t g0 = Ticks();
      __imp__sub_82C41918(ctx, base);
      s.guest_ticks.fetch_add(uint64_t(Ticks() - g0), std::memory_order_relaxed);
      NativeReport(frame, verify);
      return;
    }
    s.staged.fetch_add(1, std::memory_order_relaxed);
  } else {
    r = native_bpe::Decode(block, GuestPtr(base, dst), expanded);
  }
  s.native_ticks.fetch_add(uint64_t(Ticks() - n0), std::memory_order_relaxed);
  if (const int32_t slow = REXCVAR_GET(wwe2k14_native_bpe_debug_slow); slow > 1) {
    // Session 58 (diagnostic): spin until the call took `slow` times its native time (the guest decoder is ~5.5x).
    const int64_t took = Ticks() - n0;
    while (Ticks() - n0 < took * slow) {
    }
  }
  if (r.error) {
    // A bound tripped (a stream the guest would decode past its declared size, or a malformed one): run the game's
    // own decoder, which rewrites the destination from the untouched input.
    s.fallbacks.fetch_add(1, std::memory_order_relaxed);
    if (s.logged.fetch_add(1) < 32) {
      REXLOG_WARN("[wwe2k14][native_bpe] FALLBACK frame={} in_hash~={} src=0x{:08X} dst=0x{:08X} expanded={} "
                  "error={} produced={} consumed={}",
                  frame, HashName(Fnv1a64(block, 16u + r.consumed)), src, dst, expanded, r.error, r.produced,
                  r.consumed);
    }
    const int64_t g0 = Ticks();
    __imp__sub_82C41918(ctx, base);
    s.guest_ticks.fetch_add(uint64_t(Ticks() - g0), std::memory_order_relaxed);
    NativeReport(frame, verify);
    return;
  }
  // Registers as the guest leaves them (listing: add r3,r11,r4 / mr r5,r4 + one addi per byte / addi r6,r3+16,-1 +
  // one per stream byte / r4 = the table-step constant). r7-r12, ctr and cr6 are volatile scratch nobody reads.
  const uint64_t r3_in = ctx.r3.u64;
  const uint64_t r4_in = ctx.r4.u64;
  ctx.r3.u64 = r4_in + expanded;
  ctx.r5.u64 = r4_in + r.produced;
  ctx.r6.u64 = r3_in + 16u + r.consumed - 1u;
  ctx.r4.u64 = 0xF7F7F7F7F7F7F7F8ull;
  NativeReport(frame, verify);
}

// Session 11 (step 6): with --wwe2k14_decode_probe on as well, time the whole native call as the calling thread sees
// it and feed the probe's per-thread per-frame lines ("[wwe2k14][decode] frame ... ms=... ; decoder=native"), so a
// timed A/B gets the update thread's decode time per frame in both arms with the same probe (bpe ms = time inside
// the hook; native_ms = the same; the per-call lines and the probe totals stay guest-only).
inline void NativeCallTimed(PPCContext& __restrict ctx, uint8_t* base, bool verify) {
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  FlushThread(frame);
  const uint32_t expanded = native_bpe::ExpandedSize(GuestPtr(base, ctx.r3.u32));
  const int64_t t0 = Ticks();
  NativeCall(ctx, base, verify);
  const double ms = TicksMs(Ticks() - t0);
  ThreadAcc& a = t_acc;
  ++a.bpe_calls;
  a.bpe_out += expanded;
  a.bpe_ms += ms;
  a.native_ms += ms;
  if (ms > a.max_call_ms) a.max_call_ms = ms;
  a.decoder = verify ? 2 : 1;
  // Per-call line like the guest path's: the per-frame line is only flushed at the thread's next call, which on a
  // short route may never come (the update thread's hitch burst is its last decode before the route quits).
  if (ms >= REXCVAR_GET(wwe2k14_decode_probe_call_ms)) {
    REXLOG_INFO("[wwe2k14][decode] call {} frame={} tid={} out={} ms={:.3f}", verify ? "verify" : "native", frame,
                HostTid(), expanded, ms);
  }
}

}  // namespace wwe2k14::decode_probe

// Overrides of the recompiled entry symbols (weak aliases). Order: the corpus recorder records the game's own
// decoder, so it runs the guest body whatever the native settings say; then --wwe2k14_native_bpe (default on) /
// _verify replace the BPE decoder (verify: both run, the game's output is kept; with the probe on, the native path
// feeds its per-frame / per-call lines); then the probe times the game's decoder (native off); else the original.
// Session 58: outside the in-game sequence the game's own decoder runs unless --wwe2k14_native_bpe_menus (30 Years of
// WrestleMania match 1, run 3929 vs 3930: native BPE in the menu, where the match preview builds Bobby Heenan, left
// him red in the match; the same build at the game's decoder speed was right, and so was native BPE in the match load).
REX_HOOK_RAW(sub_82C41918) {
  if (REXCVAR_GET(wwe2k14_bpe_corpus)) {
    wwe2k14::decode_probe::BpeCall(ctx, base);
    return;
  }
  const bool verify = REXCVAR_GET(wwe2k14_native_bpe_verify);
  if (!verify && REXCVAR_GET(wwe2k14_native_bpe) && !REXCVAR_GET(wwe2k14_native_bpe_menus) &&
      wwe2k14::decode_probe::g_native_bpe_in_game && !wwe2k14::decode_probe::g_native_bpe_in_game()) {
    wwe2k14::decode_probe::g_bpe_menu_guest_calls.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82C41918(ctx, base);
    return;
  }
  if (verify || REXCVAR_GET(wwe2k14_native_bpe)) {
    if (REXCVAR_GET(wwe2k14_decode_probe)) {
      wwe2k14::decode_probe::NativeCallTimed(ctx, base, verify);
    } else {
      wwe2k14::decode_probe::NativeCall(ctx, base, verify);
    }
    return;
  }
  if (!REXCVAR_GET(wwe2k14_decode_probe) && !REXCVAR_GET(wwe2k14_bpe_corpus)) {
    __imp__sub_82C41918(ctx, base);
    return;
  }
  wwe2k14::decode_probe::BpeCall(ctx, base);
}

#include "native_zlib.h"  // Session 15: --wwe2k14_native_zlib / _verify, --wwe2k14_zlib_probe

REX_HOOK_RAW(sub_82C41738) {
  if (wwe2k14::native_zlib::Wanted()) {
    wwe2k14::native_zlib::Call(ctx, base);
    return;
  }
  if (!REXCVAR_GET(wwe2k14_decode_probe)) {
    __imp__sub_82C41738(ctx, base);
    return;
  }
  wwe2k14::decode_probe::ZlibCall(ctx, base);
}
