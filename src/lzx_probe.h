// wwe2k14 - XCTD / LZX decoder probe and corpus recorder (Session 12). Default OFF. LOG ONLY.
//
// 334 of the 367 pacs are XCTD-compressed (XCompress LZX, docs/systems/file_loading.md "XCTD decode path"). Every
// decode goes through one function (q callers: sub_82CA0120 read job -> sub_82CA2DC0 -> sub_82CA3E98 ->
// sub_82CA4088), the "chunk loop" (listing of sub_82CA4088, Session 12):
//   r3 = decompression context (tag 0x76C3F251 at +0; the LZX state starts at +20, window size at +24)
//   r4 = destination, r5 = pointer to a big-endian u32: in = bytes wanted, out = bytes written
//   r6 = compressed block (a run of chunks: big-endian u16 length, then that many bytes; length 0 as the first chunk
//        = stored block, the data follows the 2-byte zero), r7 = bytes available at r6
//   r8 = the block's uncompressed size, r9 = offset inside the block where the wanted bytes start.
// Each chunk is one LZX frame of min(32768, r8 - decoded so far) output bytes. The LZX state is reset (sub_82CA4310)
// whenever the call does not continue the previous one (cache fields at +20+12268..12300: same r6, r7, r8 and an
// offset at or past the cached one), so a block's output depends only on its bytes, r8 and the window size.
// sub_82CA3E98 returns 0 whatever happens, so decode errors never reach the read job.
//
//   --wwe2k14_lzx_probe=true   time every call (host clock around the recompiled body) and log
//        "[wwe2k14][lzx] call ..." per call at least wwe2k14_lzx_probe_call_ms long (default 0 = every call) and
//        "[wwe2k14][lzx] totals ..." every 600 guest frames.
//   --wwe2k14_lzx_corpus=true   record every call for the offline test (tools/lzx): one file per distinct
//        (input, size, window, offset, written) in out/harness/lzx/corpus/<hash>.lzx and one CSV row per call in
//        out/harness/lzx/calls/calls_NNN.csv. Game data: stays in out/ (gitignored).
//
//   --wwe2k14_native_lzx (DEFAULT ON since Session 12, Jarrod 2026-10-10; =false = the game's decoder)   replace
//        the game's decoder with src/native_lzx.h: decode the frames up to offset + wanted of the block into a
//        per-thread host buffer (kept for the next call: the read job asks for a block in pieces, and a
//        call whose input bytes, size and window equal the kept block's reuses it), copy the wanted slice to the
//        destination and write the length; the game's resume cache (+20+12268) is cleared so a later game call
//        starts from a reset. Any decoder error, an offset past the block or a destination overlapping the input ->
//        the game's own body for that call ("fallback").
//   --wwe2k14_native_lzx_verify=true   both: native into the host buffer, then the game; compare written length and
//        bytes; on a difference log MISMATCH and save the call to out/harness/lzx/mismatch/<hash>.lzx (corpus
//        format). The game's output is kept. Wins over _native_lzx.
//   Totals "[wwe2k14][native_lzx] at frame ..." every 600 guest frames; with the probe on, the native calls' own
//   lines (decoder=native / verify; ms = the whole hook).
// Order in the hook: corpus (always the game's decoder) > verify > native > probe > the original.
//
// Cost when all are off: four setting reads and the original call.

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
#include <string>
#include <unordered_set>
#include <vector>

#include "decode_probe.h"  // Ticks, TicksMs, HostTid, GuestPtr, Fnv1a64, HashName
#include "frame_stats.h"
#include "native_lzx.h"

REXCVAR_DEFINE_BOOL(wwe2k14_lzx_probe, false, "wwe2k14",
                    "Time every call of the game's XCTD/LZX chunk decoder (sub_82CA4088) and log [wwe2k14][lzx] "
                    "lines (Session 12; log only)");
REXCVAR_DEFINE_DOUBLE(wwe2k14_lzx_probe_call_ms, 0.0, "wwe2k14",
                      "wwe2k14_lzx_probe: log one line per call at least this long (ms; 0 = every call)");
REXCVAR_DEFINE_BOOL(wwe2k14_lzx_corpus, false, "wwe2k14",
                    "Record every call of the LZX chunk decoder (sub_82CA4088): input block, parameters and output to "
                    "out/harness/lzx/corpus plus one CSV row per call in out/harness/lzx/calls/calls_NNN.csv "
                    "(Session 12; game data, stays in out/)");

REXCVAR_DEFINE_BOOL(wwe2k14_native_lzx, true, "wwe2k14",
                    "Native XCTD/LZX decoder (default on since Session 12, Jarrod 2026-10-10): replace the game's "
                    "chunk decoder (sub_82CA4088) with src/native_lzx.h, falling back to the game's own on any error; "
                    "=false restores the game's decoder");
REXCVAR_DEFINE_BOOL(wwe2k14_native_lzx_verify, false, "wwe2k14",
                    "Run both LZX decoders, compare, keep the game's output; log [wwe2k14][native_lzx] mismatches and "
                    "save them to out/harness/lzx/mismatch (Session 12; default off)");

REX_EXTERN(__imp__sub_82CA4088);

namespace wwe2k14::lzx_probe {

using decode_probe::Fnv1a64;
using decode_probe::GuestPtr;
using decode_probe::HashName;
using decode_probe::HostTid;
using decode_probe::Ticks;
using decode_probe::TicksMs;

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

// The call's arguments, read before the guest body runs (r5 / r7 etc. are volatile).
struct Args {
  uint32_t ctx, dst, len_ptr, src, src_len, total, offset, wanted, window;
};

inline Args ReadArgs(PPCContext& __restrict ctx, uint8_t* base) {
  Args a;
  a.ctx = ctx.r3.u32;
  a.dst = ctx.r4.u32;
  a.len_ptr = ctx.r5.u32;
  a.src = ctx.r6.u32;
  a.src_len = ctx.r7.u32;
  a.total = ctx.r8.u32;
  a.offset = ctx.r9.u32;
  a.wanted = Be32(GuestPtr(base, a.len_ptr));
  a.window = Be32(GuestPtr(base, a.ctx + 24));
  return a;
}

// Bytes of the block the frames use: chunks until `total` output bytes are covered (or the input / a zero length
// ends them). Stored block (first length 0): 2 + total. Never more than src_len.
inline uint32_t UsedLength(const uint8_t* src, uint32_t src_len, uint32_t total, uint32_t* frames_out) {
  uint32_t frames = 0;
  if (src_len < 2) {
    *frames_out = 0;
    return src_len;
  }
  if (Be16(src) == 0) {
    *frames_out = 0;
    const uint64_t n = 2ull + total;
    return n < src_len ? uint32_t(n) : src_len;
  }
  uint32_t pos = 0, produced = 0;
  while (produced < total && pos + 2 <= src_len) {
    const uint32_t len = Be16(src + pos);
    if (len == 0) break;
    pos += 2 + len;
    const uint32_t frame = total - produced < 32768u ? total - produced : 32768u;
    produced += frame;
    ++frames;
  }
  *frames_out = frames;
  return pos < src_len ? pos : src_len;
}

// --- Corpus (--wwe2k14_lzx_corpus) --------------------------------------------------------------------------------
// File layout (host little-endian): "LZXC", version 1, window, total, src_len (the call's r7), offset, wanted,
// written, in_len, out_len, frames (u32 each), reserved u32, in_hash, out_hash (u64), then in_len input bytes (the
// used part of the block + up to 8 bytes after it, as far as r7 allows: the guest's bit reader may look 4 bytes past
// a chunk) and out_len = written output bytes (from the destination).
struct LzxFileHeader {
  char magic[4];
  uint32_t version, window, total, src_len, offset, wanted, written, in_len, out_len, frames, reserved;
  uint64_t in_hash, out_hash;
};
static_assert(sizeof(LzxFileHeader) == 64, "lzx corpus header layout");

inline std::filesystem::path LzxDir(const char* sub) {
  std::error_code ec;
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  const std::filesystem::path exe = std::filesystem::path(std::wstring(buf, n));
  std::filesystem::path dir =
      std::filesystem::weakly_canonical(exe.parent_path() / ".." / ".." / "harness" / "lzx" / sub, ec);
#else
  std::filesystem::path dir = std::filesystem::current_path() / sub;
#endif
  std::filesystem::create_directories(dir, ec);
  return dir;
}

inline bool WriteLzxFile(const std::filesystem::path& path, const LzxFileHeader& h, const uint8_t* in,
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

struct Corpus {
  std::mutex mutex;
  bool opened = false;
  FILE* csv = nullptr;
  std::filesystem::path dir;
  std::unordered_set<uint64_t> seen_keys, seen_blocks;
  uint64_t calls = 0, unique = 0, files = 0, blocks = 0, in_bytes = 0, out_bytes = 0;

  void OpenLocked() {
    opened = true;
    dir = LzxDir("corpus");
    char num[16];
    std::snprintf(num, sizeof(num), "%03d", CurrentLogNumber() > 0 ? CurrentLogNumber() : 0);
    const std::filesystem::path path = LzxDir("calls") / ("calls_" + std::string(num) + ".csv");
    csv = std::fopen(path.string().c_str(), "w");
    if (csv) {
      std::fprintf(csv, "frame,htid,ctx,dst,src,src_len,used,frames,total,offset,wanted,written,window,in_hash,"
                        "out_hash,guest_us,new_block,new_entry\n");
    }
    REXLOG_INFO("[wwe2k14][lzx_corpus] recording to {} (calls: {})", dir.string(), path.string());
  }

  void Record(uint64_t frame, const Args& a, uint32_t written, const uint8_t* src, const uint8_t* out,
              double guest_ms) {
    uint32_t frames = 0;
    const uint32_t used = UsedLength(src, a.src_len, a.total, &frames);
    const uint32_t in_len = used + 8u <= a.src_len ? used + 8u : a.src_len;
    const uint64_t in_hash = Fnv1a64(src, in_len) ^ (uint64_t(a.total) << 32) ^ a.window;
    const uint64_t out_hash = Fnv1a64(out, written);
    uint64_t key = in_hash;
    key = (key ^ a.offset) * 1099511628211ull;
    key = (key ^ written) * 1099511628211ull;
    std::lock_guard<std::mutex> lock(mutex);
    if (!opened) OpenLocked();
    ++calls;
    const bool new_block = seen_blocks.insert(in_hash).second;
    const bool new_entry = seen_keys.insert(key).second;
    if (new_block) ++blocks;
    if (new_entry) {
      ++unique;
      LzxFileHeader h{};
      std::memcpy(h.magic, "LZXC", 4);
      h.version = 1;
      h.window = a.window;
      h.total = a.total;
      h.src_len = a.src_len;
      h.offset = a.offset;
      h.wanted = a.wanted;
      h.written = written;
      h.in_len = in_len;
      h.out_len = written;
      h.frames = frames;
      h.in_hash = in_hash;
      h.out_hash = out_hash;
      if (WriteLzxFile(dir / (HashName(key) + ".lzx"), h, src, out)) {
        ++files;
        in_bytes += in_len;
        out_bytes += written;
      }
    }
    if (csv) {
      std::fprintf(csv, "%llu,%u,0x%08X,0x%08X,0x%08X,%u,%u,%u,%u,%u,%u,%u,%u,%s,%s,%.1f,%d,%d\n",
                   static_cast<unsigned long long>(frame), HostTid(), a.ctx, a.dst, a.src, a.src_len, used, frames,
                   a.total, a.offset, a.wanted, written, a.window, HashName(in_hash).c_str(),
                   HashName(out_hash).c_str(), guest_ms * 1000.0, new_block ? 1 : 0, new_entry ? 1 : 0);
      if ((calls & 63) == 0) std::fflush(csv);
    }
  }

  void Report(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (csv) std::fflush(csv);
    REXLOG_INFO("[wwe2k14][lzx_corpus] at frame {}: calls={} distinct_blocks={} entries={} files_written={} "
                "in_bytes={} out_bytes={}",
                frame, calls, blocks, unique, files, in_bytes, out_bytes);
  }
};
inline Corpus g_corpus;

struct Totals {
  std::atomic<uint64_t> calls{0}, out{0}, in{0}, ticks{0}, full_blocks{0}, stored{0}, short_writes{0};
  std::atomic<uint64_t> next_report_frame{600};
};
inline Totals g_totals;

inline void MaybeReport(uint64_t frame) {
  uint64_t next = g_totals.next_report_frame.load(std::memory_order_relaxed);
  if (frame < next) return;
  if (!g_totals.next_report_frame.compare_exchange_strong(next, (frame / 600 + 1) * 600)) return;
  const Totals& t = g_totals;
  if (REXCVAR_GET(wwe2k14_lzx_probe)) {
    REXLOG_INFO("[wwe2k14][lzx] totals at frame {}: calls={} out={} in={} guest_ms={:.1f} full_blocks={} stored={} "
                "short_writes={}",
                frame, t.calls.load(), t.out.load(), t.in.load(), TicksMs(int64_t(t.ticks.load())),
                t.full_blocks.load(), t.stored.load(), t.short_writes.load());
  }
  if (REXCVAR_GET(wwe2k14_lzx_corpus)) g_corpus.Report(frame);
}

inline void GuestCall(PPCContext& __restrict ctx, uint8_t* base) {
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  const Args a = ReadArgs(ctx, base);
  const int64_t t0 = Ticks();
  __imp__sub_82CA4088(ctx, base);
  const int64_t ticks = Ticks() - t0;
  const double ms = TicksMs(ticks);
  const uint32_t written = Be32(GuestPtr(base, a.len_ptr));
  const uint8_t* src = GuestPtr(base, a.src);

  if (REXCVAR_GET(wwe2k14_lzx_corpus)) g_corpus.Record(frame, a, written, src, GuestPtr(base, a.dst), ms);

  if (REXCVAR_GET(wwe2k14_lzx_probe)) {
    uint32_t frames = 0;
    const uint32_t used = UsedLength(src, a.src_len, a.total, &frames);
    const bool stored = a.src_len >= 2 && Be16(src) == 0;
    const uint32_t expect = a.offset < a.total ? (a.total - a.offset < a.wanted ? a.total - a.offset : a.wanted) : 0;
    g_totals.calls.fetch_add(1, std::memory_order_relaxed);
    g_totals.out.fetch_add(written, std::memory_order_relaxed);
    g_totals.in.fetch_add(used, std::memory_order_relaxed);
    g_totals.ticks.fetch_add(uint64_t(ticks), std::memory_order_relaxed);
    if (a.offset == 0 && written == a.total) g_totals.full_blocks.fetch_add(1, std::memory_order_relaxed);
    if (stored) g_totals.stored.fetch_add(1, std::memory_order_relaxed);
    if (written != expect) g_totals.short_writes.fetch_add(1, std::memory_order_relaxed);
    if (ms >= REXCVAR_GET(wwe2k14_lzx_probe_call_ms)) {
      REXLOG_INFO("[wwe2k14][lzx] call frame={} tid={} ctx=0x{:08X} src=0x{:08X} src_len={} used={} total={} "
                  "offset={} wanted={} written={} window={} stored={} ms={:.3f} decoder=guest",
                  frame, HostTid(), a.ctx, a.src, a.src_len, used, a.total, a.offset, a.wanted, written, a.window,
                  stored ? 1 : 0, ms);
    }
  }
  MaybeReport(frame);
}


// --- Native decoder (--wwe2k14_native_lzx / _verify) ---------------------------------------------------------------
struct NativeStats {
  std::atomic<uint64_t> calls{0}, native{0}, hits{0}, decodes{0}, fallbacks{0}, verified{0}, mismatches{0};
  std::atomic<uint64_t> out{0}, decoded{0}, native_ticks{0}, guest_ticks{0};
  std::atomic<uint64_t> fb_error[16] = {};
  std::atomic<uint64_t> fb_offset{0}, fb_overlap{0};
  std::atomic<uint64_t> next_report_frame{600};
  std::atomic<int> logged{0};
  std::atomic<bool> announced{false};
};
inline NativeStats g_native;

// Per-thread decoded block, kept for the next call.
struct BlockCache {
  std::unique_ptr<native_lzx::State> st;
  std::vector<uint8_t> in, out;  // in: in_len bytes + 16 zero bytes; out: total + 64
  uint32_t in_len = 0, total = 0, window = 0, produced = 0;
  bool valid = false;
};
inline thread_local BlockCache t_cache;

inline void NativeReport(uint64_t frame, bool verify) {
  uint64_t next = g_native.next_report_frame.load(std::memory_order_relaxed);
  if (frame < next) return;
  if (!g_native.next_report_frame.compare_exchange_strong(next, (frame / 600 + 1) * 600)) return;
  const NativeStats& n = g_native;
  std::string fb;
  for (int i = 1; i < 16; ++i) {
    const uint64_t v = n.fb_error[i].load();
    if (v) fb += " e" + std::to_string(i) + "=" + std::to_string(v);
  }
  REXLOG_INFO("[wwe2k14][native_lzx] at frame {}: mode={} calls={} native={} cache_hits={} decodes={} fallbacks={} "
              "(offset={} overlap={}{}) verified={} mismatches={} out={} decoded={} native_ms={:.1f} guest_ms={:.1f}",
              frame, verify ? "verify" : "native", n.calls.load(), n.native.load(), n.hits.load(), n.decodes.load(),
              n.fallbacks.load(), n.fb_offset.load(), n.fb_overlap.load(), fb, n.verified.load(),
              n.mismatches.load(), n.out.load(), n.decoded.load(), TicksMs(int64_t(n.native_ticks.load())),
              TicksMs(int64_t(n.guest_ticks.load())));
}

// Decodes (or reuses) the first `needed` bytes of this call's block into t_cache (whole frames, like the game, which
// stops once the wanted bytes are out). Returns the native_lzx error (0 = t_cache.out holds them); *decoded = bytes
// decoded by this call (0 on a cache hit).
inline int NativeBlock(const Args& a, const uint8_t* src, uint32_t needed, bool* hit, uint32_t* decoded) {
  BlockCache& c = t_cache;
  uint32_t frames = 0;
  const uint32_t used = UsedLength(src, a.src_len, a.total, &frames);
  const uint32_t in_len = used + 8u <= a.src_len ? used + 8u : a.src_len;
  *decoded = 0;
  if (c.valid && c.produced >= needed && c.in_len == in_len && c.total == a.total && c.window == a.window &&
      std::memcmp(c.in.data(), src, in_len) == 0) {
    *hit = true;
    return native_lzx::kOk;
  }
  *hit = false;
  if (!c.st) c.st = std::make_unique<native_lzx::State>();
  c.valid = false;
  c.in.assign(src, src + in_len);
  c.in.resize(size_t(in_len) + 16, 0);
  if (c.out.size() < size_t(a.total) + 64) c.out.resize(size_t(a.total) + 64);
  const native_lzx::Result r =
      native_lzx::DecodeBlock(c.in.data(), in_len, a.total, a.window, c.out.data(), *c.st, needed);
  if (r.error) return r.error;
  c.in_len = in_len;
  c.total = a.total;
  c.window = a.window;
  c.produced = r.produced;
  c.valid = true;
  *decoded = r.produced;
  return native_lzx::kOk;
}

inline void ClearGameResumeCache(uint8_t* base, uint32_t ctx_addr) {
  // +20+12268 = the cached input pointer (sub_82CA4088 compares it first); 0 never matches a real block.
  uint8_t* p = GuestPtr(base, ctx_addr + 20 + 12268);
  p[0] = p[1] = p[2] = p[3] = 0;
}

inline void SaveMismatch(const Args& a, const uint8_t* src, uint32_t written, const uint8_t* out) {
  uint32_t frames = 0;
  const uint32_t used = UsedLength(src, a.src_len, a.total, &frames);
  const uint32_t in_len = used + 8u <= a.src_len ? used + 8u : a.src_len;
  LzxFileHeader h{};
  std::memcpy(h.magic, "LZXC", 4);
  h.version = 1;
  h.window = a.window;
  h.total = a.total;
  h.src_len = a.src_len;
  h.offset = a.offset;
  h.wanted = a.wanted;
  h.written = written;
  h.in_len = in_len;
  h.out_len = written;
  h.frames = frames;
  h.in_hash = Fnv1a64(src, in_len) ^ (uint64_t(a.total) << 32) ^ a.window;
  h.out_hash = Fnv1a64(out, written);
  const uint64_t key = (h.in_hash ^ a.offset) * 1099511628211ull;
  WriteLzxFile(LzxDir("mismatch") / (HashName(key) + ".lzx"), h, src, out);
}

inline void NativeCall(PPCContext& __restrict ctx, uint8_t* base, bool verify, bool probe) {
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (!g_native.announced.exchange(true)) {
    REXLOG_INFO("[wwe2k14][native_lzx] active: mode={}", verify ? "verify" : "native");
  }
  const int64_t t0 = Ticks();
  const Args a = ReadArgs(ctx, base);
  const uint8_t* src = GuestPtr(base, a.src);
  g_native.calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t expect = a.offset < a.total ? (a.total - a.offset < a.wanted ? a.total - a.offset : a.wanted) : 0;

  int err = native_lzx::kOk;
  bool hit = false, fallback = false;
  uint32_t decoded = 0;
  if (a.offset > a.total) {
    fallback = true;
    g_native.fb_offset.fetch_add(1, std::memory_order_relaxed);
  } else if (expect && uint64_t(a.dst) < uint64_t(a.src) + a.src_len && uint64_t(a.src) < uint64_t(a.dst) + expect) {
    fallback = true;
    g_native.fb_overlap.fetch_add(1, std::memory_order_relaxed);
  } else if (expect) {
    err = NativeBlock(a, src, a.offset + expect, &hit, &decoded);
    if (err) {
      fallback = true;
      g_native.fb_error[err & 15].fetch_add(1, std::memory_order_relaxed);
    }
  }
  const int64_t t1 = Ticks();
  g_native.native_ticks.fetch_add(uint64_t(t1 - t0), std::memory_order_relaxed);

  if (verify || fallback) {
    // The game's body: in verify mode always (its output is kept), else only for a fallback.
    ClearGameResumeCache(base, a.ctx);
    const int64_t g0 = Ticks();
    __imp__sub_82CA4088(ctx, base);
    g_native.guest_ticks.fetch_add(uint64_t(Ticks() - g0), std::memory_order_relaxed);
    if (fallback) g_native.fallbacks.fetch_add(1, std::memory_order_relaxed);
    if (verify && !fallback) {
      const uint32_t written = Be32(GuestPtr(base, a.len_ptr));
      const uint8_t* g = GuestPtr(base, a.dst);
      const bool same = written == expect &&
                        (expect == 0 || std::memcmp(g, t_cache.out.data() + a.offset, expect) == 0);
      g_native.verified.fetch_add(1, std::memory_order_relaxed);
      if (!same) {
        g_native.mismatches.fetch_add(1, std::memory_order_relaxed);
        uint32_t first = 0;
        const uint32_t n = written < expect ? written : expect;
        while (first < n && g[first] == t_cache.out[size_t(a.offset) + first]) ++first;
        if (g_native.logged.fetch_add(1) < 16) {
          REXLOG_INFO("[wwe2k14][native_lzx] MISMATCH frame={} src=0x{:08X} total={} offset={} wanted={} "
                      "game_written={} native_written={} first_diff={} cache_hit={}",
                      frame, a.src, a.total, a.offset, a.wanted, written, expect, first, hit ? 1 : 0);
        }
        SaveMismatch(a, src, written, g);
        t_cache.valid = false;
      }
    }
  } else {
    if (expect) std::memcpy(GuestPtr(base, a.dst), t_cache.out.data() + a.offset, expect);
    uint8_t* lp = GuestPtr(base, a.len_ptr);
    lp[0] = uint8_t(expect >> 24);
    lp[1] = uint8_t(expect >> 16);
    lp[2] = uint8_t(expect >> 8);
    lp[3] = uint8_t(expect);
    ClearGameResumeCache(base, a.ctx);
    g_native.native.fetch_add(1, std::memory_order_relaxed);
  }
  if (expect && !fallback) {
    if (hit) {
      g_native.hits.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_native.decodes.fetch_add(1, std::memory_order_relaxed);
      g_native.decoded.fetch_add(decoded, std::memory_order_relaxed);
    }
  }
  g_native.out.fetch_add(expect, std::memory_order_relaxed);
  if (probe) {
    const double ms = TicksMs(Ticks() - t0);
    if (ms >= REXCVAR_GET(wwe2k14_lzx_probe_call_ms)) {
      REXLOG_INFO("[wwe2k14][lzx] call frame={} tid={} ctx=0x{:08X} src=0x{:08X} src_len={} total={} offset={} "
                  "wanted={} written={} window={} ms={:.3f} decoder={} hit={} fallback={}",
                  frame, HostTid(), a.ctx, a.src, a.src_len, a.total, a.offset, a.wanted, expect, a.window, ms,
                  verify ? "verify" : "native", hit ? 1 : 0, fallback ? err : -1);
    }
  }
  NativeReport(frame, verify);
}

}  // namespace wwe2k14::lzx_probe

REX_HOOK_RAW(sub_82CA4088) {
  const bool probe = REXCVAR_GET(wwe2k14_lzx_probe);
  if (REXCVAR_GET(wwe2k14_lzx_corpus)) {
    wwe2k14::lzx_probe::GuestCall(ctx, base);
    return;
  }
  const bool verify = REXCVAR_GET(wwe2k14_native_lzx_verify);
  if (verify || REXCVAR_GET(wwe2k14_native_lzx)) {
    wwe2k14::lzx_probe::NativeCall(ctx, base, verify, probe);
    return;
  }
  if (!probe) {
    __imp__sub_82CA4088(ctx, base);
    return;
  }
  wwe2k14::lzx_probe::GuestCall(ctx, base);
}
