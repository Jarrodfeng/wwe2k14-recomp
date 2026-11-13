// wwe2k14 - the D3D layer's shader bind, read from the device object instead of run (Session 87; shared by F1 and F3).
// NOT included anywhere yet (parallel mode). Docs: docs\native\d3d\F3_CONTRACT.md, the coordinator's decision (b).
//
// The library's flush calls the shader bind 0x82CBE558 when main-mask bits 17-20 are set. The bind does not read plain
// shadows: it builds ring packets - IM_LOAD of the pixel shader, a literal-constant table load (0x82CBE368), and the vertex
// shader either IM_LOADed from the shader object's own memory after an IN-PLACE patch of its fetch instructions (slot cache
// 0x82CBE418) or as an IM_LOAD_IMMEDIATE copy made inside the command stream (0x82CBE210) - and it updates device words
// (+10528 / +10532 / +10536 SQ_PROGRAM_CNTL / CONTEXT_MISC / INTERPOLATOR_CNTL, +10372, +10580, +10560, the stride mirrors
// +12264 / +12272, flag bytes +11069 / +11070 / +11071) that the register-run blocks after it then flush.
//
// This header does the same work as host code, without writing a packet and without running 0x82CBE558 / 0x82CBE418 /
// 0x82CBE210 / 0x82CBCEA0 / 0x82CBE368:
//   Plan()   reads the device (pure: nothing of the device is written) and returns a BindPlan: the register runs (the literal
//            constants), the pixel / vertex shader to load, the main-mask bits the bind sets, and the device words to store.
//   Commit() stores the plan's device words (call it only when the draw is taken).
//   ToPackets() renders a plan as the packets the bind would have written (type-0 runs, IM_LOAD, EVENT_WRITE), for a consumer
//            that still carries PM4 (F1's token payload); the F3 record takes the runs / shaders directly.
// The patched vertex shader: the library's patch routine 0x82CBD910(vs, buffer, declaration, strides, variant) is called on
// OUR private copy of the microcode (a guest buffer in a physical-heap region of ours) as a pure data transform - verified in
// the listing (docs in the Session 87 F3 hand-back): its only stores are to its own stack frame and, by memcpy 0x82C79000, to
// buffer + 12 * (fetch instruction index) for each fetch instruction named in the shader's list; it reads the VS object (the
// variant record, and the pristine fetch words kept at vs + 416 * variant + 68), the declaration (count at +24, 12-byte
// records from +52) and the stride bytes. No ring reservation, no device write. One exception: it reads one uninitialised
// stack halfword (lhz r10,80(r1) at 0x82CBD978, a don't-care of the original compiler that can reach the first mask's bit
// 0); we zero that halfword before the call. Each (shader object, microcode, declaration contents, strides) is patched once and
// the copy reused (the key is the raw bytes of those inputs).
//
// What is supported (everything else is a counted Fail, the caller hands the draw back untouched):
//   - a bound pixel shader, variant 0 of the vertex shader, device+11068 bit 0x40 clear (the library's predicated "binned"
//     recording form writes extra bin-select packets), no vertex-shader "variant 2" patch (0x82CBE0C0 ran in 0 of 23
//     coverage scenarios; the bind takes it when the pixel shader's input mask differs from the vertex shader's).
// Not mirrored on purpose: the ring-space bookkeeping, the slot cache's stamps (vs+416v+40 / +64, declaration +48) and the
// two assert calls (0x83597724) - none is read by anything but the bind itself.
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include "d3d_inline_types.h"
#include "guest_view.h"  // S100 strip-360 tier C: the 0xE0000000 view rule, one place
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_bind_verify, false, "wwe2k14",
                    "Session 87 (D3D layer shader bind): compare each patched vertex-shader copy with the shader object's own "
                    "microcode when the library's slot cache says it holds the same patch (declaration id and strides equal), "
                    "and count / log differences ([d3d] bind verify)");

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_record_lean, false, "wwe2k14",
                    "Session 94 (idea 5, game-thread cost of TRUE NATIVE's record building): per-call trims in the D3D layer that "
                    "cannot change a record's bytes - the texture slot walk visits only the dirty slots (no shadow reads of the "
                    "clean ones) and the patched vertex-shader copy is found by a per-thread cache validated by comparing the "
                    "source bytes instead of hashing them ([d3d][rec] lines)");
REXCVAR_DEFINE_BOOL(wwe2k14_d3d_rec_timer, false, "wwe2k14",
                    "Session 94: time the game thread's D3D record building on 1 guest frame in 8 and print [d3d][rec] every 600 "
                    "frames (diagnostic; off = no clock reads, no line; a bench pair passes it in both arms)");

REX_EXTERN(sub_82CBD910);

namespace wwe2k14::d3d_bind {

namespace d3d = wwe2k14::d3d_types;

enum class Fail : uint32_t {
  kNone = 0,
  kNoPs,        // no pixel shader bound (z-only draws): the bind's no-PS branch is not mirrored
  kBinned,      // device+11068 bit 0x40: the predicated recording form
  kVariant2,    // the vertex shader would need the second variant patch (0x82CBE0C0)
  kDecl0Patch,  // the copy path with no declaration (the library would crash too)
  kNoRegion,    // the private region could not be allocated / is full
  kBadInput,    // a count outside what the listing allows (declaration records, microcode size)
  kCount
};
inline constexpr const char* kFailNames[uint32_t(Fail::kCount)] = {"none", "no_pixel_shader", "binned_form", "variant2",
                                                                    "decl0_patch", "no_region", "bad_input"};

struct DeviceWord {
  uint32_t offset;
  uint32_t value;
};

struct BindPlan {
  bool nothing = false;          // no vertex shader bound: the bind returns the mask unchanged
  uint64_t mask = 0;             // the main-mask value the bind returns (bits added)
  bool hiz_event = false;        // EVENT_WRITE(0xF) 0x82CBCEA0 emits first
  // Register runs in the order the bind writes them: PS literals [0, ps_literal_runs), then VS literals.
  std::vector<d3d::RegisterRun> runs;
  std::vector<uint32_t> values;
  uint32_t ps_literal_runs = 0;
  bool has_ps = false, has_vs = false;
  d3d::ShaderBind pixel_shader, vertex_shader;
  std::vector<DeviceWord> words;  // 32-bit device stores (+10372 +10528 +10532 +10536 +10560 +10580)
  bool write_strides = false;     // +12264 / +12272 <- strides (16 bytes: streams 0-7, 8-15)
  uint8_t strides[16] = {};
  uint8_t b69 = 0, b70 = 0, b71 = 0;  // the flag bytes' new values
  // Session 94: back to a fresh plan but keeping the vectors' capacity (a per-draw plan reused per thread allocates nothing).
  void Reset() {
    nothing = false;
    mask = 0;
    hiz_event = false;
    runs.clear();
    values.clear();
    ps_literal_runs = 0;
    has_ps = has_vs = false;
    pixel_shader.guest_address = pixel_shader.dword_count = 0;
    pixel_shader.inline_words.clear();
    vertex_shader.guest_address = vertex_shader.dword_count = 0;
    vertex_shader.inline_words.clear();
    words.clear();
    write_strides = false;
    std::memset(strides, 0, sizeof(strides));
    b69 = b70 = b71 = 0;
  }
};

// ---- guest memory helpers (the game's accessors: big-endian, the E-heap 4 KB shift) -----------------------------------
inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, base + a + ::rex::memory::detail::PhysicalHostOffset(a), 4);
  return std::byteswap(be);
}
inline uint64_t Ld64(const uint8_t* base, uint32_t a) {
  uint64_t be;
  std::memcpy(&be, base + a + ::rex::memory::detail::PhysicalHostOffset(a), 8);
  return std::byteswap(be);
}
inline uint32_t Ld16(const uint8_t* base, uint32_t a) {
  uint16_t be;
  std::memcpy(&be, base + a + ::rex::memory::detail::PhysicalHostOffset(a), 2);
  return std::byteswap(be);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = std::byteswap(v);
  std::memcpy(base + a + ::rex::memory::detail::PhysicalHostOffset(a), &be, 4);
}
inline const uint8_t* Host(const uint8_t* base, uint32_t a) { return base + a + ::rex::memory::detail::PhysicalHostOffset(a); }
inline uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + ::rex::memory::detail::PhysicalHostOffset(a); }

// The library's GPU address form of a virtual address (listing 0x82CBE264-0x82CBE2CC): the low 29 bits plus 0x1000 for the
// 4 KB-page physical view.
inline uint32_t PhysForm(uint32_t a) { return wwe2k14::guest_view::ToPhysical(a); }

// ---- the private region (Session 90: GUEST VIRTUAL heap, 64 KB pages, no address shift; none of the game physical pool) ----------------------------------------
inline constexpr uint32_t kRegionBytes = 4u << 20;
inline constexpr uint32_t kArgBytes = 4096;  // [0, 4096): the stride argument; copies follow
// S101 2026-11-13 (run 7221: after 5 match loads the pause menu and much of the crowd vanished: vs_patch_failed_5 = this region
// full). The copies below are append-only and keyed by the shader OBJECT address, so every load adds copies and none is freed. The
// owner renderer's reader only needs the patched bytes once (it parses them into host facts), so it patches in a fixed scratch slot
// of its own (PatchedVertexShaderToHost, EnsureScratch: 256 KB taken the first time the reader patches) and takes nothing from the
// region. (Opus review A: the scratch is NOT carved out of the region, so the bind path keeps its whole 4 MB.)
inline constexpr uint32_t kScratchBytes = 256u << 10;  // = the largest microcode either path accepts
inline constexpr uint32_t kBumpLimit = kRegionBytes;
inline uint32_t g_scratch = 0;  // guest virtual; 0 = not taken yet (guarded by g_region_mutex)
struct Region {
  uint32_t virt = 0, used = kArgBytes;
  bool failed = false;
};
inline Region g_region;
inline std::mutex g_region_mutex;

inline bool EnsureRegion() {
  if (g_region.virt) return true;
  if (g_region.failed) return false;
  auto* memory = REX_KERNEL_MEMORY();
  rex::memory::BaseHeap* heap = memory->LookupHeapByType(false, 0x10000);
  uint32_t address = 0;
  if (heap && heap->Alloc(kRegionBytes, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) {
    g_region.virt = address;
    REXLOG_INFO("[d3d][mem] shader-bind region: {} bytes at guest virtual {:08X}; taken from the game's physical pool: 0 bytes "
                "(patched microcode reaches the SDK as an IM_LOAD address with bit 31 set = guest virtual)",
                kRegionBytes, address);
    return true;
  }
  g_region.failed = true;
  REXLOG_ERROR("[d3d] shader-bind region could not be allocated: binds that need a patched vertex shader fall back");
  return false;
}

struct PatchedCopy {
  uint32_t virt = 0;      // guest virtual address (the A view)
  uint32_t physical = 0;  // what an IM_LOAD takes
  uint32_t dwords = 0;
};
inline std::map<std::string, PatchedCopy> g_copies;
inline std::atomic<uint64_t> g_stat_copies{0}, g_stat_reused{0}, g_stat_verify_same{0}, g_stat_verify_diff{0}, g_stat_verify_na{0};

// The vertex-shader variant record (listing 0x82CBE210 / 0x82CBE418: [vs + 8 (variant + 112)] is the record's offset).
inline uint32_t VariantRecord(const uint8_t* base, uint32_t vs, uint32_t variant) { return vs + Ld32(base, vs + 8 * (variant + 112)); }

// A 64-bit hash of a byte range (eight bytes at a time, multiply-xorshift; for cache keys, not security).
inline uint64_t Hash64(const uint8_t* p, size_t n) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(n) * 0xC2B2AE3D27D4EB4Full);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
  }
  uint64_t tail = 0;
  for (size_t k = 0; i + k < n; ++k) tail |= uint64_t(p[i + k]) << (8 * k);
  h = (h ^ tail) * 0xC4CEB9FE1A85EC53ull;
  h ^= h >> 29;
  return h;
}

// The private copy of variant 0's microcode: PATCHED for (declaration, strides) with the library's routine (patch = true), or
// a plain copy (patch = false: the shaders that need no fetch patch, or no declaration). Layer draws always load a copy of ours
// and never the object's own memory: a fallback draw running the original bind can patch that in place, and the library's
// fence stamp on it (vs + 416 variant + 64) is not mirrored. Found or made. Fail: no region / bad input.
inline Fail PatchedVertexShaderSlow(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t vs, uint32_t decl,
                                    const uint8_t strides[24], bool patch, PatchedCopy& out) {
  const uint32_t rec = VariantRecord(base, vs, 0);
  const uint32_t ucode_offset = Ld32(base, rec + 872), bytes = Ld32(base, rec + 876);
  const uint32_t count = patch ? Ld32(base, decl + 24) : 0;
  if (!bytes || (bytes & 3) || bytes > (256u << 10) || count > 64) return Fail::kBadInput;
  const uint8_t* source = Host(base, Ld32(base, vs + 32) + ucode_offset);
  // The key: every input the patch routine reads, and the source microcode and the pristine fetch words it patches from
  // (vs + 68 .. vs + 416: a reused object address with new microcode must not find the old copy).
  std::string key;
  key.reserve(80 + 12 * count);
  auto put = [&](const void* p, size_t n) { key.append(static_cast<const char*>(p), n); };
  const uint8_t tag = patch ? 'P' : 'C';
  const uint64_t source_hash = Hash64(source, bytes);
  const uint64_t pristine_hash = patch ? Hash64(Host(base, vs + 68), 416 - 68) : 0;
  put(&tag, 1);
  put(&vs, 4);
  put(&ucode_offset, 4);
  put(&bytes, 4);
  put(&source_hash, 8);
  put(&pristine_hash, 8);
  if (patch) {
    put(&count, 4);
    put(Host(base, decl + 52), 12 * count);  // the declaration's records, guest byte order
    put(strides, 24);
  }
  std::lock_guard lock(g_region_mutex);
  if (auto it = g_copies.find(key); it != g_copies.end()) {
    g_stat_reused.fetch_add(1, std::memory_order_relaxed);
    out = it->second;
    return Fail::kNone;
  }
  if (!EnsureRegion()) return Fail::kNoRegion;
  const uint32_t at = (g_region.used + 15u) & ~15u;
  if (at + bytes > kBumpLimit) {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
      REXLOG_ERROR("[d3d][mem] shader-bind region FULL ({} of {} bytes, {} copies): new patched vertex shaders fail from now on",
                   g_region.used, kBumpLimit, g_copies.size());
    }
    return Fail::kNoRegion;
  }
  g_region.used = at + bytes;
  const uint32_t copy = g_region.virt + at;
  // Our private copy of the microcode as the object holds it; the patch overwrites the fetch instructions it names.
  std::memcpy(Host(base, copy), source, bytes);
  PatchedCopy c;
  c.virt = copy;
  c.physical = 0x80000000u | copy;  // (Session 90: guest virtual tag, bit 31; SDK d3d_shader_tag.h)
  c.dwords = bytes >> 2;
  if (!patch) {
    g_copies.emplace(std::move(key), c);
    g_stat_copies.fetch_add(1, std::memory_order_relaxed);
    out = c;
    return Fail::kNone;
  }
  // The stride argument (24 bytes: streams 0-23 as the device's +12904 array) and the uninitialised stack halfword.
  std::memcpy(Host(base, g_region.virt), strides, 24);
  const uint32_t frame80 = ctx.r1.u32 - 592u + 80u;  // lhz r10,80(r1) of 0x82CBD910's own frame
  std::memset(Host(base, frame80), 0, 2);
  // Call the library's patch routine as a data transform: r3 vs, r4 buffer, r5 declaration, r6 strides, r7 variant.
  const uint64_t r1 = ctx.r1.u64, r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64,
                 lr = ctx.lr;
  ctx.r3.u64 = vs;
  ctx.r4.u64 = copy;
  ctx.r5.u64 = decl;
  ctx.r6.u64 = g_region.virt;
  ctx.r7.u64 = 0;
  ctx.lr = 0x82CBE4E0;  // (a plausible return address; the routine only saves and restores it)
  sub_82CBD910(ctx, base);
  ctx.r1.u64 = r1;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.lr = lr;
  g_copies.emplace(std::move(key), c);
  g_stat_copies.fetch_add(1, std::memory_order_relaxed);
  out = c;
  return Fail::kNone;
}

// Session 94 (wwe2k14_d3d_record_lean): the slow path above hashes the shader's whole microcode (and the 348 pristine bytes) on
// every vertex-shader change to build its string key. This front cache is per thread (no lock), keyed by a hash of the SMALL inputs
// only, and a hit is accepted only when the skey inputs are byte-equal and the source / pristine bytes compare equal to the
// snapshot taken when the entry was stored - so a hit finds exactly the copy the slow path's key (which hashes the same bytes)
// would find. The copies are never removed from g_copies, so a stored entry stays valid.
struct FastEntry {
  std::vector<uint8_t> skey, source, pristine;
  PatchedCopy copy;
};
inline thread_local std::unordered_map<uint64_t, FastEntry> t_fast;
inline thread_local std::vector<uint8_t> t_skey;
inline std::atomic<uint64_t> g_stat_fast_hits{0}, g_stat_fast_misses{0};

inline Fail PatchedVertexShader(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t vs, uint32_t decl,
                                const uint8_t strides[24], bool patch, PatchedCopy& out) {
  if (!REXCVAR_GET(wwe2k14_d3d_record_lean)) return PatchedVertexShaderSlow(ctx, base, dev, vs, decl, strides, patch, out);
  const uint32_t rec = VariantRecord(base, vs, 0);
  const uint32_t ucode_offset = Ld32(base, rec + 872), bytes = Ld32(base, rec + 876);
  const uint32_t count = patch ? Ld32(base, decl + 24) : 0;
  if (!bytes || (bytes & 3) || bytes > (256u << 10) || count > 64) return PatchedVertexShaderSlow(ctx, base, dev, vs, decl, strides, patch, out);
  const uint8_t* source = Host(base, Ld32(base, vs + 32) + ucode_offset);
  const uint8_t* pristine = Host(base, vs + 68);
  constexpr size_t kPristine = 416 - 68;
  std::vector<uint8_t>& skey = t_skey;
  skey.clear();
  auto put = [&](const void* p, size_t n) { skey.insert(skey.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
  const uint8_t tag = patch ? 'P' : 'C';
  put(&tag, 1);
  put(&vs, 4);
  put(&ucode_offset, 4);
  put(&bytes, 4);
  if (patch) {
    put(&count, 4);
    put(Host(base, decl + 52), 12 * count);
    put(strides, 24);
  }
  const uint64_t fk = Hash64(skey.data(), skey.size());
  if (auto it = t_fast.find(fk); it != t_fast.end()) {
    const FastEntry& e = it->second;
    if (e.skey == skey && e.source.size() == bytes && std::memcmp(e.source.data(), source, bytes) == 0 &&
        (!patch || std::memcmp(e.pristine.data(), pristine, kPristine) == 0)) {
      g_stat_reused.fetch_add(1, std::memory_order_relaxed);
      g_stat_fast_hits.fetch_add(1, std::memory_order_relaxed);
      out = e.copy;
      return Fail::kNone;
    }
  }
  g_stat_fast_misses.fetch_add(1, std::memory_order_relaxed);
  // Snapshot BEFORE the slow path: a patch routine writes the private copy, never the object's own bytes, but take it first anyway.
  FastEntry e;
  e.skey = skey;
  e.source.assign(source, source + bytes);
  if (patch) e.pristine.assign(pristine, pristine + kPristine);
  const Fail f = PatchedVertexShaderSlow(ctx, base, dev, vs, decl, strides, patch, out);
  if (f == Fail::kNone) {
    e.copy = out;
    t_fast[fk] = std::move(e);
  }
  return f;
}

// S101 2026-11-13: the owner renderer's reader (renderer_adapter\reader.h GetVertexShader) needs the patched microcode only to parse
// it into host facts once per shader key, never as guest memory. So the copy is made in the fixed scratch slot [kBumpLimit,
// kRegionBytes) and handed back as host bytes: nothing is kept in the region, however many loads run (run 7221: the bump area
// filled after 5 match loads). Same inputs and the same library patch call as PatchedVertexShaderSlow, under the same mutex (the
// stride argument at the region's start is shared).
inline Fail PatchedVertexShaderToHost(PPCContext& ctx, uint8_t* base, uint32_t vs, uint32_t decl, const uint8_t strides[24], bool patch,
                                      std::vector<uint8_t>& out) {
  const uint32_t rec = VariantRecord(base, vs, 0);
  const uint32_t ucode_offset = Ld32(base, rec + 872), bytes = Ld32(base, rec + 876);
  const uint32_t count = patch ? Ld32(base, decl + 24) : 0;
  if (!bytes || (bytes & 3) || bytes > kScratchBytes || count > 64) return Fail::kBadInput;
  const uint8_t* source = Host(base, Ld32(base, vs + 32) + ucode_offset);
  if (!patch) {  // a plain copy: host bytes, no guest memory at all
    out.assign(source, source + bytes);
    return Fail::kNone;
  }
  std::lock_guard lock(g_region_mutex);
  if (!EnsureRegion()) return Fail::kNoRegion;  // the stride argument lives at the region's start
  if (!g_scratch) {
    rex::memory::BaseHeap* heap = REX_KERNEL_MEMORY()->LookupHeapByType(false, 0x10000);
    uint32_t address = 0;
    if (!heap || !heap->Alloc(kScratchBytes, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) {
      return Fail::kNoRegion;
    }
    g_scratch = address;
    REXLOG_INFO("[d3d][mem] reader shader scratch: {} bytes at guest virtual {:08X} (reused for every patch)", kScratchBytes, address);
  }
  const uint32_t copy = g_scratch;
  std::memcpy(Host(base, copy), source, bytes);
  std::memcpy(Host(base, g_region.virt), strides, 24);
  const uint32_t frame80 = ctx.r1.u32 - 592u + 80u;  // lhz r10,80(r1) of 0x82CBD910's own frame (as the slow path)
  std::memset(Host(base, frame80), 0, 2);
  const uint64_t r1 = ctx.r1.u64, r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64,
                 lr = ctx.lr;
  ctx.r3.u64 = vs;
  ctx.r4.u64 = copy;
  ctx.r5.u64 = decl;
  ctx.r6.u64 = g_region.virt;
  ctx.r7.u64 = 0;
  ctx.lr = 0x82CBE4E0;
  sub_82CBD910(ctx, base);
  ctx.r1.u64 = r1;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.lr = lr;
  const uint8_t* p = Host(base, copy);
  out.assign(p, p + bytes);
  return Fail::kNone;
}

// The runtime check of "our copy has the content the library's patched copy has": when the library's slot cache holds the same
// patch in the object's own microcode (declaration id +48 equal to the slot's +40 stamp, and the slot's stored strides equal
// ours - the cache-hit condition of 0x82CBE418), the object's words must equal our copy's. Only possible after a draw that took
// the original path (the layer never runs the in-place patch).
inline void VerifyCopy(const uint8_t* base, uint32_t vs, uint32_t decl, const uint8_t strides[16], const PatchedCopy& c) {
  const uint32_t id = Ld32(base, decl + 48);
  const uint32_t slot = vs;  // variant 0
  const uint32_t rec = VariantRecord(base, vs, 0);
  if (!id || Ld32(base, slot + 40) != id || std::memcmp(Host(base, slot + 48), strides, 16) != 0) {
    g_stat_verify_na.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint8_t* object = Host(base, Ld32(base, vs + 32) + Ld32(base, rec + 872));
  const uint8_t* ours = Host(base, c.virt);
  if (std::memcmp(object, ours, c.dwords * 4) == 0) {
    g_stat_verify_same.fetch_add(1, std::memory_order_relaxed);
  } else {
    if (g_stat_verify_diff.fetch_add(1, std::memory_order_relaxed) < 4) {
      uint32_t first = 0;
      while (first < c.dwords && std::memcmp(object + 4 * first, ours + 4 * first, 4) == 0) ++first;
      REXLOG_ERROR("[d3d] bind verify: our patched copy of vertex shader {:08X} differs from the library's in-place patch at "
                   "dword {} (ours {:08X}, library {:08X})",
                   vs, first, Ld32(base, c.virt + 4 * first), Ld32(base, Ld32(base, vs + 32) + Ld32(base, rec + 872) + 4 * first));
    }
  }
}

// The literal-constant table load (listing 0x82CBE368): rec[20] is the table's offset; the table (8-byte entries: u16
// float4 index, u16 dword count, u32 offset) starts 20 bytes after rec + rec[20], its byte length is at +16; each entry
// loads `count` dwords from (offset + data_base) into the ALU constant registers from 0x4000 + 4 index.
inline void AddLiterals(BindPlan& p, const uint8_t* base, uint32_t rec, uint32_t data_base) {
  const uint32_t off = Ld32(base, rec + 20);
  if (!off) return;
  const uint32_t table = rec + off;
  uint32_t at = table + 20;
  const uint32_t end = at + Ld32(base, table + 16);
  while (at < end) {
    const uint32_t index = Ld16(base, at), count = Ld16(base, at + 2);
    if (!count) break;
    const uint32_t src = Ld32(base, at + 4) + data_base;
    at += 8;
    d3d::RegisterRun run;
    run.first_register = 0x4000 + index * 4;
    run.count = count;
    run.value_offset = uint32_t(p.values.size());
    for (uint32_t i = 0; i < count; ++i) p.values.push_back(Ld32(base, src + 4 * i));
    p.runs.push_back(run);
  }
}

// 0x82CBCEA0 (main-mask bit 17): the HiZ control word +10560 and flag byte +11069 from the depth / blend state; returns the
// mask with bit 8. May emit EVENT_WRITE(0xF) (p.hiz_event).
inline uint64_t HizPrepass(const uint8_t* base, uint32_t dev, uint64_t m, uint32_t& d10560, uint8_t& b69, uint8_t b70, BindPlan& p) {
  const uint32_t ps = Ld32(base, dev + 13068);
  uint32_t r8 = 0;
  if (ps) r8 = (Ld32(base, ps + Ld32(base, ps + 64) + 48) >> 27) & 1;  // rlwinm r8,r11,5,31,31 (0x82CBCED8): bit 27
  const uint32_t r10 = Ld32(base, dev + 10548);
  const uint32_t r7 = r10 >> 1;
  uint32_t r11 = uint32_t(b70) >> 5;
  uint32_t r9 = (Ld32(base, dev + 12284) >> 20) & 0xFFF;
  r11 &= r7;
  r11 = (d10560 & ~1u) | (r11 & 1);
  d10560 = r11;  // 0x82CBCF04 stores it: bit 0 of +10560 is the new value, the bit-1 insert below builds on it
  if (!(r9 & 2)) {
    r11 = r9 & 1;
  } else {
    const uint32_t zf = (r10 >> 4) & 7;
    const bool bit4 = (r11 & 0x10) != 0;
    const uint32_t pass = !bit4 ? (zf == 1 || zf == 3 || zf == 2) : (zf == 4 || zf == 6 || zf == 2);
    r11 = r11 & ~r8;
    r11 &= r7;
    r11 &= pass;
    r11 &= 1;
    if (r10 & 1) {
      const uint32_t a = (r10 & 0x3800u) == 0, b = (r10 & 0xE0000u) == 0;
      r11 = (a & b) & r11;
      if (r10 & 0x80) {
        const uint32_t c = (r10 & 0xE0000000u) == 0, d = (r10 & 0x03800000u) == 0;
        r11 = (c & d) & r11;
      }
    }
  }
  d10560 = (d10560 & ~2u) | ((r11 << 1) & 2u);
  if (d10560 & 2) {
    if (b69 & 4) {
      if (Ld32(base, dev + 13904) == 0 || Ld32(base, dev + 13912) == 1) {
        b69 &= 0xFB;
        p.hiz_event = true;
      }
    }
  } else if ((r7 & 1) && (r10 & 4)) {
    const uint32_t zf = (r10 >> 4) & 7;
    bool set = zf == 7 || zf == 5;
    if (!set) set = !(d10560 & 0x10) ? (zf == 4 || zf == 6) : (zf == 1 || zf == 3);
    if (set) b69 |= 4;
  }
  return m | 0x100;
}

// The bind (listing 0x82CBE558, non-binned form), as a plan. `mask` = the main dirty mask the flush holds (bit 19 already
// raised by the stride rule where it applies); stride0_dword = the stream-0 stride byte for this draw (stride / 4) for an inline
// draw, or ~0u to use the device's +12904 as it is.
// device_only (Session 100, owner mode's bookkeeping-only draws, d3d_layer.h OwnerNullDraw): the device words, flag bytes and strides
// only; no literal runs and no patched vertex-shader copy (they feed the old command processor's record, which is not written).
inline Fail Plan(PPCContext& ctx, uint8_t* base, uint32_t dev, uint64_t mask, uint32_t stride0_dword, BindPlan& p, bool device_only = false) {
  p.Reset();
  uint64_t m = mask;
  uint8_t b69 = base[dev + 11069], b70 = base[dev + 11070], b71 = base[dev + 11071];
  uint32_t d10560 = Ld32(base, dev + 10560), d10580 = Ld32(base, dev + 10580), d10372 = Ld32(base, dev + 10372),
           d10528 = Ld32(base, dev + 10528), d10532 = Ld32(base, dev + 10532), d10536 = Ld32(base, dev + 10536);
  bool w10560 = false, w10580 = false, w10372 = false, w10528 = false, w10532 = false, w10536 = false;
  const uint32_t vs = Ld32(base, dev + 13072), ps = Ld32(base, dev + 13068), decl = Ld32(base, dev + 12240);

  const uint32_t hiz_before = d10560;
  if (m & 0x20000ull) m = HizPrepass(base, dev, m, d10560, b69, b70, p);
  w10560 = d10560 != hiz_before;
  auto finish = [&]() {
    p.mask = m;
    p.b69 = b69;
    p.b70 = b70;
    p.b71 = b71;
    if (w10560) p.words.push_back({10560, d10560});
    if (w10580) p.words.push_back({10580, d10580});
    if (w10372) p.words.push_back({10372, d10372});
    if (w10528) p.words.push_back({10528, d10528});
    if (w10532) p.words.push_back({10532, d10532});
    if (w10536) p.words.push_back({10536, d10536});
  };
  if (!vs) {
    p.nothing = true;
    finish();
    return Fail::kNone;
  }
  if (!ps) return Fail::kNoPs;
  if (base[dev + 11068] & 0x40) return Fail::kBinned;

  const uint32_t rec0 = VariantRecord(base, vs, 0);
  const uint32_t r25 = rec0 + 872;  // the variant's record (listing 0x82CBE5D8)

  // The pixel shader's state block.
  const uint32_t blk = ps + Ld32(base, ps + 64);
  const uint32_t r27 = blk + 40, r18 = Ld32(base, blk + 48), r16 = Ld32(base, blk + 52);
  const bool flag = (r18 & 0x70000000u) > 0x10000000u;
  if (flag) {  // OR, not replace (0x82CBE700-0x82CBE714: the new bit is inserted from (flag | the old byte)): the bits are pending
    b70 |= 0x08;  // flags the next resolve's 3 x EVENT_WRITE(6); cleared by whoever consumes it
    b71 |= 0x02;  // flags 0x82CB21F8 / the clear pass / the no-PS bind path
  }
  if (m & 0x100000ull) {  // the pixel shader changed
    if (!device_only) AddLiterals(p, base, ps + 40, Ld32(base, ps + 24));
    p.ps_literal_runs = uint32_t(p.runs.size());
    p.has_ps = true;
    p.pixel_shader.guest_address = PhysForm(Ld32(base, blk + 40) + Ld32(base, ps + 24));
    p.pixel_shader.dword_count = Ld32(base, blk + 44) >> 2;
    m |= 0x4000ull;
    d10536 = Ld32(base, r27 + 24);
    w10536 = true;
    if (b71 & 0x08) {
      m |= 1ull << 56;
      d10372 = Ld32(base, Ld32(base, dev + 12816) + 28);
      w10372 = true;
    }
    if ((d10580 & 7) != 4) {
      d10580 = (d10580 & ~7u) | 4;
      w10580 = true;
      m |= 0x80008ull;
    }
  }
  // The vertex shader's interpolator mask against the pixel shader's (listing 0x82CBE7E8-0x82CBE838).
  constexpr uint32_t kInterpMask = 0x603FF;
  if ((Ld32(base, r27 + 20) & kInterpMask) != (Ld32(base, r25 + 20) & kInterpMask)) return Fail::kVariant2;

  const uint32_t sp80 = Ld32(base, r25 + 8), r24 = Ld32(base, r25 + 12);
  b71 = uint8_t((b71 & 0xFE) | ((b71 & 1) | (flag ? 1 : 0)));  // listing 0x82CBE920-0x82CBE948

  // The vertex shader (0x82CBE950-0x82CBEA7C): not at all unless bit 19 (the "7C" form: bit 7 of +11070 set, bits 19 / 20).
  uint8_t strides[24];
  std::memcpy(strides, Host(base, dev + 12904), 24);
  if (stride0_dword != ~0u) strides[0] = uint8_t(stride0_dword);
  const bool seven_c = (b70 & 0x80) != 0;
  const bool vs_changed = seven_c ? (m & 0x180000ull) != 0 : (m & 0x80000ull) != 0;
  if (vs_changed) {
    if (!device_only) AddLiterals(p, base, vs + 872, Ld32(base, vs + 32));
    b70 &= 0xBF;
    p.has_vs = true;
    const bool no_patch = !seven_c && (!decl || (Ld32(base, vs + 872) & 0x40));
    if (no_patch) {  // 0x82CBEA40: the object's own microcode, the strides from the slot
      PatchedCopy c;
      if (!device_only) {
        if (Fail f = PatchedVertexShader(ctx, base, dev, vs, decl, strides, false, c); f != Fail::kNone) return f;
      }
      p.vertex_shader.guest_address = c.physical;
      p.vertex_shader.dword_count = c.dwords;
      std::memcpy(p.strides, Host(base, vs + 48), 8);  // (raw 8-byte copies, as ld / std)
      std::memcpy(p.strides + 8, Host(base, vs + 56), 8);
    } else {
      if (!decl) return Fail::kDecl0Patch;
      PatchedCopy c;
      if (!device_only) {
        if (Fail f = PatchedVertexShader(ctx, base, dev, vs, decl, strides, true, c); f != Fail::kNone) return f;
      }
      p.vertex_shader.guest_address = c.physical;
      p.vertex_shader.dword_count = c.dwords;
      std::memcpy(p.strides, strides, 16);
      if (seven_c) b70 &= 0x7F;  // 0x82CBE210 with the variant flag 0 clears bit 7
      if (!device_only && REXCVAR_GET(wwe2k14_d3d_bind_verify)) VerifyCopy(base, vs, decl, strides, c);
    }
    p.write_strides = true;
  }
  if (m & 0x180000ull) {  // listing 0x82CBEAC4
    m |= 0x18000ull;
    d10532 = r24 | r16;
    d10528 = sp80 | r18;
    w10532 = w10528 = true;
  }
  finish();
  return Fail::kNone;
}

// The device stores the bind makes. Only for a draw that is taken.
inline void Commit(uint8_t* base, uint32_t dev, const BindPlan& p) {
  for (const DeviceWord& w : p.words) St32(base, dev + w.offset, w.value);
  if (p.write_strides) std::memcpy(Host(base, dev + 12264), p.strides, 16);
  base[dev + 11069] = p.b69;
  base[dev + 11070] = p.b70;
  base[dev + 11071] = p.b71;
}

// The plan as the packets the bind writes, in its order (for a consumer that carries PM4): EVENT_WRITE, PS literals, PS
// IM_LOAD, VS literals, VS IM_LOAD. Host-order dwords.
inline void ToPackets(const BindPlan& p, std::vector<uint32_t>& out) {
  if (p.hiz_event) {
    out.push_back(0xC0004600u);
    out.push_back(0xF);
  }
  auto runs = [&](uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; ++i) {
      const d3d::RegisterRun& r = p.runs[i];
      // The library writes literal constants as LOAD_ALU_CONSTANT from memory; as register values this is the equivalent
      // type-0 run (same registers, same values).
      out.push_back(((r.count - 1) << 16) | r.first_register);
      for (uint32_t k = 0; k < r.count; ++k) out.push_back(p.values[r.value_offset + k]);
    }
  };
  runs(0, p.ps_literal_runs);
  if (p.has_ps) {
    out.push_back(0xC0012700u);
    out.push_back(p.pixel_shader.guest_address | 1u);
    out.push_back(p.pixel_shader.dword_count);
  }
  runs(p.ps_literal_runs, uint32_t(p.runs.size()));
  if (p.has_vs) {
    out.push_back(0xC0012700u);
    out.push_back(p.vertex_shader.guest_address & ~3u);
    out.push_back(p.vertex_shader.dword_count);
  }
}

}  // namespace wwe2k14::d3d_bind
