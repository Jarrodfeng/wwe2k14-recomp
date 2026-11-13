// wwe2k14 - the D3D layer, STAGE 2 "no ring" (Session 87; docs\native\d3d\F4_S2_CONTRACTS.md "Stage 2", docs\native\d3d\
// F_direct_and_noring.md sections 2-10, docs\native\d3d\STAGE2_NORING.md). NOT included anywhere yet (parallel mode: the integrator
// owns d3d_layer.h / half2_hooks.h / kernel_overrides.h; the glue is the four lines listed in STAGE2_NORING.md).
//
// Switch --wwe2k14_d3d_noring=N (default 0 = off, nothing here acts; every hook below runs its original at once):
//   1 = tier 2a: the library's ring submit (0x82CB5EF0) writes nothing to the ring and no WPTR; each segment reaches the command
//       thread through a host queue (an INDIRECT_BUFFER item the consumer runs). Same packets, same order, no ring.
//   2 = tier 2b (includes 2a): the sync packets are typed items produced here, in stream order, and run by the consumer:
//       Fence (the segment-end pair: 0x82CB63D0), FlipHandshake (0x82CB3300), SceneTiming / Interrupt (the emitter 0x82CB6510),
//       Present (VdSwap's words, taken after VdSwap_entry has formed them). The library writes none of those packets.
//   3 = tier 2c (includes 2b): the record tokens of F1 / F2 / F3 (d3d_layer::WriteRing) go to the queue by value, not into the
//       segment (TakeRecord). The consumer copies each into an arena of guest physical memory and runs it as a ring-level packet;
//       its draws take their library kind from the record (0x7C = indexed draw, 0x7E = inline draw), no Half 2 address matching
//       (STAGE2_NORING.md "Tier 2c").
// The mechanism is fixed (no ring for the scene): there is no "ring path with a detour". The library's OWN device bookkeeping stays
// true (the fence counter, the heap marker, the segment start) so the switch back to the ring (level 0) at a frame boundary is a
// normal kick. The library's no-GPU mode (device byte 11069 bit 0x02, Session 51) is never set here: fences are written by the
// command thread, in stream order, after the items before them - never by a CPU store (Session 51 lesson 1).
//
// Where each observable of docs\native\d3d\C_sync.md comes from in stage 2:
//   a/b fence + heap marker -> kNrFence (consumer);  c read-pointer write-back -> not needed (the ring is never written, the
//   library's ring-space wait is never reached);  d scratch block -> the consumer's WriteRegister of SCRATCH_REG0/1/4/5;
//   e interrupt callbacks -> the consumer's DispatchInterruptCallback;  f vblank -> the GPU VSync thread, unchanged;
//   g MMIO -> nothing to publish;  h VdSwap -> VdSwap_entry still forms the swap's words, AfterVdSwap() enqueues them.
//
// How a typed item keeps stream order with the packets still written into the segment (the F1/F2/F3 tokens in tiers 2a/2b, and
// any fallback writer): before every typed push the open piece of the segment [device+15332, device+48 + 4) is published as an
// INDIRECT_BUFFER item and the library's piece start (device+15332) moved up to the write position - the library's own "kick
// without a segment switch" does the same (0x82CB6B20). A fallback writer's packets therefore reach the command thread in order
// through the same queue ("ring packets in the scene" are counted as segment dwords: 0 in tier 2c).
#pragma once

#include "guest_gpu_only.h"  // lane 241 (3b)
#include <algorithm>
#include <array>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/det.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "d3d_layer.h"  // F1: Ld32 / St32 / Ld64 / St64, the layer switch, g_guest_frame
#include "guest_view.h"  // S100 strip-360 tier C: the 0xE0000000 view rule, one place

REXCVAR_DEFINE_INT32(wwe2k14_d3d_noring, 0, "wwe2k14",
                     "Session 87 (D3D layer stage 2, experimental): 0 = off; 1 = segments through a host queue instead of the "
                     "ring; 2 = also the fences, flip handshake, scene timing and the present as typed items; 3 = also the F1-F3 "
                     "record tokens (needs the half2 attribution decision). Needs an SDK build with gpu_d3d_noring_abi")
    .range(0, 3);
REXCVAR_DEFINE_INT32(wwe2k14_d3d_noring_from_frame, 0, "wwe2k14",
                     "Session 87 (D3D layer stage 2): the first guest frame the no-ring mode may start at (the hand-over from the "
                     "ring happens at a frame boundary: the open segment is kicked and the GPU idled first)");
REXCVAR_DEFINE_INT32(wwe2k14_d3d_noring_verify, 0, "wwe2k14",
                     "Session 87 (D3D layer stage 2, diagnostic): for the first N flip handshakes run the ORIGINAL 0x82CB3300 and "
                     "compare the argument word our transcription computes with the one it passes to the interrupt emitter (logged)");

REXCVAR_DEFINE_INT32(wwe2k14_d3d_noring_defer, 1, "wwe2k14",
                     "Session 87 (D3D layer stage 2): 1 = the queue releases entries at the moment the library kicks a segment (the ring's own visibility to the command thread; keeps the replay's time-driven content equal to the ring path), 0 = at once");

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_drop_coher, false, "wwe2k14",
                    "Session 92 (M5b): at noring level 3 the kick's coherency handshake 0x82CB61C0 (COHER_STATUS_HOST / SIZE / BASE + WAIT_REG_MEM on 0xA31, "
                    "a segment piece every frame, no guest code reads 0xA31) is not emitted; counted in the block's coher_dropped. Default off = unchanged");

REXCVAR_DEFINE_INT32(wwe2k14_d3d_noring_scissor, 1, "wwe2k14",
                     "Session 87 (D3D layer stage 2): 1 = the library's window scissor setter 0x82CA8CD0 (~160 calls a frame) is a typed register item, 0 = the original writer (its packets travel in the segment pieces)");

REX_EXTERN(__imp__sub_82CB3300);
REX_EXTERN(__imp__sub_82CB6510);
REX_EXTERN(__imp__sub_82CB63D0);
REX_EXTERN(__imp__sub_82CB5EF0);
REX_EXTERN(__imp__sub_82CB34E0);
REX_EXTERN(__imp__sub_82CB21F8);
REX_EXTERN(__imp__sub_82CA8CD0);
REX_EXTERN(__imp__sub_82CAB868);
REX_EXTERN(__imp__sub_82CB7770);
REX_EXTERN(__imp__sub_82CBAEE0);
REX_EXTERN(__imp__sub_82CB61C0);
REX_EXTERN(sub_82CB3300);
REX_EXTERN(sub_82CC4338);
REX_EXTERN(sub_82CB7770);
REX_EXTERN(sub_82CB6EA8);

// Session 100 (strip-360 B4/B5): owner mode answers fences, interrupts and flips on its own thread (src\owner_timing.h, included after the
// adapter reader in half2_hooks.h); the three hooks below ask it first.
namespace wwe2k14::owner_timing {
inline bool Fence(PPCContext& ctx, uint8_t* base);
inline bool Interrupt(PPCContext& ctx, uint8_t* base);
inline bool Flip(PPCContext& ctx, uint8_t* base);
inline bool Submit(PPCContext& ctx, uint8_t* base);  // B6: the ring submit 0x82CB5EF0 a no-op in owner mode
}  // namespace wwe2k14::owner_timing

namespace wwe2k14::d3d_noring {

namespace dl = wwe2k14::d3d_layer;

// ------------------------------------------------------------------------------------------------------------------------------
// The exe's copy of the queue layout (include\rex\graphics\d3d_noring.h in the SDK is the other copy; tied by the int capability
// cvar gpu_d3d_noring_abi). Change a field or an item layout -> bump kAbi here AND kD3dNoringAbi there.
// ------------------------------------------------------------------------------------------------------------------------------
namespace lay {
inline constexpr int32_t kAbi = 2;
inline constexpr uint32_t kMagic = 0xD3D40001u;
inline constexpr uint32_t kHeaderDwords = 7;
enum Kind : uint32_t {
  kPad = 0, kSegment = 1, kPackets = 2, kFence = 3, kFlipHandshake = 4, kSceneTiming = 5, kInterrupt = 6, kPresent = 7,
  kBarrier = 8, kRegs = 9, kKindCount
};
// field indexes (entry dwords 1-6)
inline constexpr uint32_t kSegAddress = 0, kSegLength = 1, kSegFlags = 2;
inline constexpr uint32_t kPktTag = 0;
inline constexpr uint32_t kFenceValue = 0, kFenceMarker = 1, kFenceMarkerAt = 2, kFenceValueAt = 3, kFenceFlags = 4;
inline constexpr uint32_t kIntFlags = 0, kIntCallback = 1, kIntArgument = 2, kIntScratch = 3, kIntOptions = 4;
inline constexpr uint32_t kPresentFront = 0, kPresentWidth = 1, kPresentHeight = 2;
inline constexpr uint32_t kBarrierTicket = 0;
inline constexpr uint32_t kRegsOpWrite = 1, kRegsOpRmw = 2;
inline constexpr uint32_t kRegsOpBinMaskLo = 3, kRegsOpBinMaskHi = 4, kRegsOpBinSelectLo = 5, kRegsOpBinSelectHi = 6;  // kRegs payload: quads {op, register, a, b}; write: reg := a; rmw: reg := (reg & a) | b
struct Shared {
  uint32_t magic = kMagic;
  uint32_t abi = kAbi;
  uint32_t* storage = nullptr;
  uint32_t capacity = 0;
  alignas(64) std::atomic<uint32_t> tail{0};
  alignas(64) std::atomic<uint32_t> head{0};
  alignas(64) std::atomic<uint32_t> consumed_ticket{0};
  std::atomic<uint64_t> items[kKindCount]{};
  std::atomic<uint64_t> segment_dwords{0};
  std::atomic<uint64_t> packet_dwords{0};
  std::atomic<uint64_t> fences{0};
  std::atomic<uint64_t> flip_waits{0};
  std::atomic<uint64_t> flip_wait_us{0};
  std::atomic<uint64_t> bad{0};
  // ---- ABI 3 (Session 92, M5b; same layout as the SDK's D3dNoringShared; the SDK reads these only when `abi` >= 3) ----
  alignas(64) std::atomic<uint32_t> consumer_idle{0};
  void (*wake)(void*) = nullptr;
  void* wake_ctx = nullptr;
  std::atomic<uint64_t> coher_dropped{0};
};
}  // namespace lay

// The guest callbacks the emitter 0x82CB6510 is given (docs\native\d3d\C_sync.md section 2).
inline constexpr uint32_t kFlipScheduler = 0x82CB2A28;  // the present's flip scheduler
inline constexpr uint32_t kSceneBegin = 0x82C16978;     // scene-timing slots
inline constexpr uint32_t kSceneEnd = 0x82C16980;

// Device offsets (map B / the listings of 0x82CB63D0, 0x82CB6B20, 0x82CB6510, 0x82CB3300).
inline constexpr uint32_t kDevWritePtr = 48, kDevRingEnd = 52, kDevSoftEnd = 56;
inline constexpr uint32_t kDevMiscMask = 32;       // 64-bit dirty mask; bit 63 is cleared by the segment-end / interrupt 0x5C8 write
inline constexpr uint32_t kDevFenceBlock = 11024;  // the fence block (guest virtual)
inline constexpr uint32_t kDevFenceNext = 11036;   // the next fence number (advances by 2 a segment)
inline constexpr uint32_t kDevScratch = 11028;     // the scratch block (guest virtual)
inline constexpr uint32_t kDevByte11068 = 11068;   // bit 0: the tiling extents wrapper
inline constexpr uint32_t kDevByte11069 = 11069;   // bit 0x02: the library's no-GPU mode (never set here), bit 0x10: per-frame
inline constexpr uint32_t kDevByte11071 = 11071;
inline constexpr uint32_t kDevByte11072 = 11072;   // bit 0x80: the pending 0x5C8 write
inline constexpr uint32_t kDevFrameRateMode = 14000;
inline constexpr uint32_t kDevLateThreshold = 12284;
inline constexpr uint32_t kDevPredicate = 13092;   // tiling predication bits of the packet headers (0 outside tiling)
inline constexpr uint32_t kDevPieceStart = 15332;  // the first word of the not yet submitted piece of the open segment
inline constexpr uint32_t kDevHeapBits = 15324;    // (& 3) is part of the heap marker
inline constexpr uint32_t kDevDebugObject = 21940;     // a non-null object makes the ring submit call it: not taken
inline constexpr uint32_t kDevSubmitCallback = 23768;  // likewise
inline constexpr uint32_t kDevProfiling = 22056;       // 2 = the profiling packets of 0x82CC4338 (they are ring packets: delivered as a piece)
inline constexpr uint32_t kDevNoWaitFlags = 24584;     // non-zero: the handshake has no pre-arm / post-wait packets

// The GPU-form address the library puts into its packets for a guest virtual address (listing 0x82CB3300: rlwinm 12,20,31 / addi
// 512 / rlwinm 19,19 / clrlwi 3 / add).
inline constexpr uint32_t GpuForm(uint32_t a) { return wwe2k14::guest_view::ToPhysical(a); }

// ------------------------------------------------------------------------------------------------------------------------------
// State
// ------------------------------------------------------------------------------------------------------------------------------
enum Counter : uint32_t {
  kPushSegment, kPushPackets, kPushFence, kPushFlip, kPushScene, kPushInterrupt, kPushPresent, kPushBarrier,
  kStrayDwords,       // dwords of segment pieces published while the layer took the draws (tier 2c: fallback packets only)
  kPieceDwords,       // dwords of all pieces published
  kQueueFullWaits,
  kFbNoCp, kFbTiling, kFbPredicate, kFbDebugObject, kFbNoDevice, kFbPresentWindow, kFbNoRoom,
  kEnter, kLeave, kRefusedNoGpuBit, kRefusedAbi,
  kVerifyOk, kVerifyBad,
  kPushRegs, kFbDisplay,
  kCounterCount
};
inline constexpr const char* kCounterNames[kCounterCount] = {
    "segments",   "packets",   "fences",    "flips",       "scene_timings", "interrupts", "presents",
    "barriers",   "stray_dwords", "piece_dwords", "queue_full_waits",
    "fb_no_cp",   "fb_tiling", "fb_predicate", "fb_debug_object", "fb_no_device", "fb_present_window", "fb_no_room",
    "enter",      "leave",     "refused_no_gpu_bit", "refused_abi", "verify_ok", "verify_bad",
    "regs",       "fb_display"};

struct State {
  lay::Shared shared;
  std::vector<uint32_t> storage;
  std::mutex push_mutex;
  std::atomic<int> level{0};            // the effective level: 0 = the ring path
  std::atomic<int> abi{-1};             // -1 unknown, 0 absent / different, 1 matches
  std::atomic<bool> attached{false};    // the SDK has been given the block
  std::atomic<bool> defer{false};       // entries wait in `pending` for the kick (see FlushPending)
  std::vector<uint32_t> pending;
  std::mutex pending_mutex;
  std::mutex flush_mutex;  // held across taking the pending entries and pushing them (review A2: another thread's push must not pass them)
  std::atomic<uint32_t> ticket{0};
  std::atomic<uint64_t> census_frame{0};
  std::array<std::atomic<uint64_t>, kCounterCount> n{};
};
inline State g_state;
inline thread_local uint32_t t_original_depth = 0;  // >0: inside an original library function we called as a fallback
inline thread_local uint32_t t_expect_flip_arg = 0xFFFFFFFFu;  // --wwe2k14_d3d_noring_verify: the transcription's word
// The piece the 0x82CB63D0 hook published for the kick that called it (GPU-form start, length in dwords). The kick loaded the piece
// start into a register BEFORE the hook moved device+15332, so its own submit names the same range again: the 0x82CB5EF0 hook drops
// that one descriptor (STAGE2_NORING.md "Tier 2b failure").
struct JustPublished {
  uint32_t gpu = 0, length = 0;
  bool valid = false;
};
inline thread_local JustPublished t_just_published;

inline void Count(Counter c, uint64_t k = 1) { g_state.n[c].fetch_add(k, std::memory_order_relaxed); }
inline int Level() { return g_state.level.load(std::memory_order_relaxed); }

inline bool AbiOk() {
  int v = g_state.abi.load(std::memory_order_relaxed);
  if (v < 0) {
    const auto s = rex::cvar::GetFlagByName("gpu_d3d_noring_abi");
    v = (!s.empty() && s == std::to_string(lay::kAbi)) ? 1 : 0;
    g_state.abi.store(v, std::memory_order_relaxed);
    if (!v) {
      REXLOG_WARN("[d3d][noring] off: the SDK reports gpu_d3d_noring_abi '{}', this exe was written for {}", s, lay::kAbi);
    }
  }
  return v == 1;
}

// A wait that works under det_serial too (Session 51 lesson 2: a guest thread that waits on the host holds the baton, the guest
// callback the consumer needs would never run). pred must not change anything.
template <class P>
inline bool WaitFor(P pred, uint32_t timeout_ms) {
  if (pred()) return true;
  if (rex::det::SerialEnabled() && rex::det::SerialIsGuestThread()) {
    rex::det::SerialBlock(std::function<bool()>(pred), rex::det::Vblanks() + 1000000);
    return pred();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------------
// The producer side of the queue
// ------------------------------------------------------------------------------------------------------------------------------
inline constexpr uint32_t kStorageDwords = 1u << 23;  // 32 MB

inline void EnsureStorage() {
  State& s = g_state;
  if (s.shared.storage) return;
  s.storage.assign(kStorageDwords, 0);
  s.shared.storage = s.storage.data();
  s.shared.capacity = kStorageDwords;
}

// One entry. Waits (counted) while the storage is full: the consumer frees entries as it runs them. The wait is the queue depth
// bound; it never holds the push lock (a second producer must not block on it under det_serial).
inline void PushNow(lay::Kind kind, const uint32_t (&f)[6], const uint32_t* words, uint32_t n) {
  State& s = g_state;
  lay::Shared& sh = s.shared;
  const uint32_t total = lay::kHeaderDwords + n;
  const uint32_t mask = sh.capacity - 1;
  bool counted_wait = false;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(s.push_mutex);
      const uint32_t tail = sh.tail.load(std::memory_order_relaxed);
      const uint32_t head = sh.head.load(std::memory_order_acquire);
      const uint32_t pos = tail & mask;
      const uint32_t pad = (pos + total > sh.capacity) ? sh.capacity - pos : 0;
      if (sh.capacity - (tail - head) >= pad + total) {
        uint32_t at = tail;
        if (pad) {
          sh.storage[pos] = uint32_t(lay::kPad) | (pad << 8);
          at += pad;
        }
        uint32_t* e = sh.storage + (at & mask);
        e[0] = uint32_t(kind) | (total << 8);
        for (uint32_t i = 0; i < 6; ++i) e[1 + i] = f[i];
        if (n) std::memcpy(e + lay::kHeaderDwords, words, size_t(n) * sizeof(uint32_t));
        sh.tail.store(at + total, std::memory_order_seq_cst);  // (Session 92 M5b: seq_cst, the Dekker pair with consumer_idle)
        if (sh.abi >= 3 && sh.consumer_idle.load(std::memory_order_seq_cst) && sh.wake) sh.wake(sh.wake_ctx);  // the idle consumer sleeps: wake it
        return;
      }
    }
    if (!counted_wait) {
      counted_wait = true;
      Count(kQueueFullWaits);
    }
    // (the padding that keeps an entry contiguous is always shorter than the entry, so twice its size is enough)
    WaitFor(
        [&] {
          const uint32_t t = sh.tail.load(std::memory_order_relaxed), h = sh.head.load(std::memory_order_acquire);
          return sh.capacity - (t - h) >= 2 * total;
        },
        2000);
  }
}

// Deferred release (--wwe2k14_d3d_noring_defer, default 1). The ring path makes nothing visible to the command thread before the library
// KICKS a segment (0x82CB5EF0): the flip handshake, the scene-timing interrupts, the fences, the draws of a segment all run when the
// kick has submitted it. Pushing typed items the moment they are produced runs them earlier relative to the guest threads (the flip
// scheduler samples the vblank counter earlier, the scene-timing callbacks bracket different wall time, under det_serial the scheduler
// turns that run the CP's external interrupt requests fall elsewhere) - the replay's time-driven content (titantron frame, crowd phase)
// then differs from the ring's although every draw is the same (integrator runs 38-40, level 1 clean). With deferral every entry
// (pieces and typed items, still in program order) waits in `pending` and is released in one go by the 0x82CB5EF0 hook, i.e. at the
// moment the ring would have received the segment: same visibility, no new ordering.
inline bool Deferring() { return g_state.defer.load(std::memory_order_relaxed); }

inline void FlushPending() {
  State& s = g_state;
  std::lock_guard<std::mutex> flush_lock(s.flush_mutex);
  std::vector<uint32_t> take;
  {
    std::lock_guard<std::mutex> lock(s.pending_mutex);
    take.swap(s.pending);
  }
  for (size_t at = 0; at < take.size();) {
    const uint32_t w0 = take[at], total = w0 >> 8;
    if (total < lay::kHeaderDwords || at + total > take.size()) break;  // (cannot happen: entries are appended whole)
    uint32_t f[6];
    for (uint32_t i = 0; i < 6; ++i) f[i] = take[at + 1 + i];
    PushNow(lay::Kind(w0 & 0xFF), f, take.data() + at + lay::kHeaderDwords, total - lay::kHeaderDwords);
    at += total;
  }
}

inline void Push(lay::Kind kind, const uint32_t (&f)[6], const uint32_t* words, uint32_t n) {
  State& s = g_state;
  if (!Deferring()) {
    std::lock_guard<std::mutex> flush_lock(s.flush_mutex);
    PushNow(kind, f, words, n);
    return;
  }
  bool too_big = false;
  {
    std::lock_guard<std::mutex> lock(s.pending_mutex);
    s.pending.push_back(uint32_t(kind) | ((lay::kHeaderDwords + n) << 8));
    for (uint32_t i = 0; i < 6; ++i) s.pending.push_back(f[i]);
    if (n) s.pending.insert(s.pending.end(), words, words + n);
    too_big = s.pending.size() > (4u << 20);  // 16 MB: a segment is far smaller; bound the memory, not the ordering
  }
  if (too_big) FlushPending();
}

inline void PushSegment(uint32_t gpu_address, uint32_t length, uint32_t flags) {
  // (Review D1: the indirect-buffer packet carries 20 bits of length: a longer piece goes as several, in order.)
  while (length) {
    const uint32_t part = length > 0xFFFFFu ? 0xFFFFFu : length;
    const uint32_t f[6] = {gpu_address, part, flags, 0, 0, 0};
    Push(lay::kSegment, f, nullptr, 0);
    Count(kPushSegment);
    gpu_address += 4 * part;
    length -= part;
  }
}

// ------------------------------------------------------------------------------------------------------------------------------
// The census of what is left in the segments (tier 2c: every dword published as a piece is a "ring packet in the scene").
// Per piece the packets are walked (headers only) and counted by what they are: a type-0 write by its first register, a type-3
// packet by its opcode; the packets that lie inside a range a hook of ours noted (NoteWriter: the original function it had to run as
// a fallback) are counted under that writer's name instead. Printed with the census line every 600 frames, then cleared.
// ------------------------------------------------------------------------------------------------------------------------------
struct NotedRange {
  uint32_t begin, end;  // guest virtual, [begin, end)
  const char* name;
};
inline thread_local std::vector<NotedRange> t_noted;
inline std::mutex g_stray_mutex;
inline std::map<std::string, uint64_t> g_stray_by;

// before / after: device+48 (the last dword written) around an original function a hook of ours ran as a fallback.
inline void NoteWriter(const char* name, uint32_t before, uint32_t after) {
  if (Level() >= 3 && after > before) t_noted.push_back({before + 4, after + 4, name});
}

inline const char* Type3Name(uint32_t op) {
  switch (op) {
    case 0x10: return "NOP";
    case 0x21: return "REG_RMW";
    case 0x22: return "DRAW_INDX";
    case 0x24: return "DRAW_INDX_2";
    case 0x2D: return "VIZ_QUERY";
    case 0x2E: return "SET_CONSTANT";
    case 0x2F: return "IM_LOAD";
    case 0x37: return "INDIRECT_BUFFER_PFD";
    case 0x3B: return "INVALIDATE_STATE";
    case 0x3C: return "WAIT_REG_MEM";
    case 0x3F: return "INDIRECT_BUFFER";
    case 0x43: return "EVENT_WRITE";
    case 0x46: return "XE_SWAP";
    case 0x4A: return "SET_BIN_MASK";
    case 0x54: return "INTERRUPT";
    case 0x58: return "EVENT_WRITE_SHD";
    case 0x5A: return "EVENT_WRITE_EXT";
    case 0x60: return "SET_BIN_MASK_LO";
    case 0x61: return "SET_BIN_MASK_HI";
    case 0x62: return "SET_BIN_SELECT_LO";
    case 0x63: return "SET_BIN_SELECT_HI";
    case 0x7C: return "D3D_TOKEN";
    case 0x7D: return "D3D_COPY";
    case 0x7E: return "D3D_INLINE";
    default: return "";
  }
}

inline void AccountStray(uint8_t* base, uint32_t start, uint32_t end) {
  std::lock_guard<std::mutex> lock(g_stray_mutex);
  uint32_t p = start, walked = 0;
  while (p < end && walked++ < 4096) {
    const uint32_t h = dl::Ld32(base, p);
    const uint32_t type = h >> 30;
    uint32_t len = 1;
    char key[64];
    if (type == 0) {
      len = ((h >> 16) & 0x3FFF) + 2;
      std::snprintf(key, sizeof key, "type0 reg 0x%04X", h & 0x7FFF);
    } else if (type == 1) {
      len = 3;
      std::snprintf(key, sizeof key, "type1");
    } else if (type == 2) {
      std::snprintf(key, sizeof key, "type2 nop");
    } else {
      len = ((h >> 16) & 0x3FFF) + 2;
      const uint32_t op = (h >> 8) & 0x7F;
      if (op >= 0x60 && op <= 0x63 && p + 4 < end) {  // the bin state ops: the value names the writer (a few distinct ones)
        std::snprintf(key, sizeof key, "type3 op 0x%02X bin state = 0x%08X", op, dl::Ld32(base, p + 4));
      } else {
        std::snprintf(key, sizeof key, "type3 op 0x%02X %s", op, Type3Name(op));
      }
    }
    const char* name = key;
    for (const NotedRange& r : t_noted) {
      if (p >= r.begin && p < r.end) {
        name = r.name;
        break;
      }
    }
    g_stray_by[name] += len;
    p += 4 * len;
  }
  if (p < end) g_stray_by["(not walked: piece too long)"] += (end - p) / 4;
  t_noted.erase(std::remove_if(t_noted.begin(), t_noted.end(), [&](const NotedRange& r) { return r.end <= end; }), t_noted.end());
}

// The open piece of the library's segment as an item, so that everything the library wrote before this point runs before the
// typed item pushed next. Moves the library's piece start up like its own kick does. Returns the dwords published.
inline uint32_t PublishPiece(uint8_t* base, uint32_t dev, bool stray_counts = true, JustPublished* out = nullptr) {
  const uint32_t start = dl::Ld32(base, dev + kDevPieceStart);
  const uint32_t end = dl::Ld32(base, dev + kDevWritePtr) + 4;
  if (!start || end <= start) return 0;
  const uint32_t length = (end - start) / 4;
  PushSegment(GpuForm(start), length, 0);
  if (out) *out = JustPublished{GpuForm(start), length, true};
  dl::St32(base, dev + kDevPieceStart, end);
  Count(kPieceDwords, length);
  if (stray_counts && Level() >= 3) {
    Count(kStrayDwords, length);
    AccountStray(base, start, end);
  }
  return length;
}

// The device updates of the 0x5C8 write that the segment end / the interrupt emitter make when the pending flag is set
// (listings 0x82CB63D0 0x82CB6414-0x82CB644C and 0x82CB6590-0x82CB65C0): the misc dirty mask loses bit 63, byte 11072 bit 0x80
// and byte 11071 bits 0-1 clear. The write itself (register 0x5C8 = 0x20000) is the consumer's.
inline void ApplyMiscFlush(uint8_t* base, uint32_t dev) {
  dl::St64(base, dev + kDevMiscMask, dl::Ld64(base, dev + kDevMiscMask) & 0x7FFFFFFFFFFFFFFFull);
  base[dev + kDevByte11072] = uint8_t(base[dev + kDevByte11072] & 0x7F);
  base[dev + kDevByte11071] = uint8_t(base[dev + kDevByte11071] & 0xFC);
}

// The library's recording modes (tiling: the strip recording, the D3D worker's replays): device+13600 (a recording in progress), byte 11068 bits
// 0xB1 (extents wrapper, tile-size variants, ...), byte 11069 bit 0x20 (no segment: the scratch-area mode). Inside them the packets are the
// worker's to replay per strip: every typed hook runs its original (the packets travel as pieces) - the no-ring levels are for the one-tile
// scene (review A2).
inline bool Recording(const uint8_t* base, uint32_t dev) {
  return dl::Ld32(base, dev + 13600) != 0 || (base[dev + kDevByte11068] & 0xB1) != 0 || (base[dev + kDevByte11069] & 0x20) != 0;
}

// ------------------------------------------------------------------------------------------------------------------------------
// The hand-over: ring <-> queue, at a frame boundary on the render thread (the start of the present's flip handshake)
// ------------------------------------------------------------------------------------------------------------------------------
inline void AttachSdk() {
  State& s = g_state;
  if (s.attached.load()) return;
  EnsureStorage();
  // Session 92 M5b: an SDK that understands the ABI-3 wake fields says so through a capability cvar; else the block stays ABI 2
  // (lay::kAbi stays 2: it is the value AbiOk() compares with the SDK's gpu_d3d_noring_abi).
  if (rex::cvar::GetFlagByName("gpu_d3d_noring_abi_max") == "3") s.shared.abi = 3;
  rex::cvar::SetFlagByName("gpu_d3d_noring_shared", std::to_string(uint64_t(reinterpret_cast<uintptr_t>(&s.shared))));
  rex::cvar::SetFlagByName("gpu_d3d_noring", "1");
  s.attached.store(true);
  REXLOG_INFO("[d3d][noring] host queue handed to the SDK: {} dwords at {:p}", s.shared.capacity, static_cast<void*>(s.shared.storage));
}

// Waits until everything the library has issued has completed: the completed fence (fence block +0) reaches the newest issued
// fence number (device+11036 - 2), unsigned modular as the library's own test (0x82CB5A20).
inline bool WaitFenceIdle(uint8_t* base, uint32_t dev, uint32_t timeout_ms) {
  const uint32_t block = dl::Ld32(base, dev + kDevFenceBlock);
  const uint32_t target = dl::Ld32(base, dev + kDevFenceNext) - 2;
  return WaitFor([&] { return int32_t(dl::Ld32(base, block) - target) >= 0; }, timeout_ms);
}

inline void Enter(PPCContext& ctx, uint8_t* base, uint32_t dev, int level) {
  if (wwe2k14::GuestGpuOnly()) return;  // lane 241 (3b): never attach a queue nobody consumes (TickTransition already wants 0)
  if (base[dev + kDevByte11069] & 0x02) {  // the library's no-GPU mode: it writes the fences itself (Session 51); never combine
    Count(kRefusedNoGpuBit);
    return;
  }
  AttachSdk();
  // The ring is the way until now: kick the open piece (fence pair + submit, the library's normal path, the 5EF0 hook below is not
  // active yet) and wait for the command thread to run it - the ring is then idle and the fence counters are true.
  ctx.r3.u64 = dev;
  ctx.lr = 0x82CB3300;
  sub_82CB6EA8(ctx, base);
  if (!WaitFenceIdle(base, dev, 5000)) {
    REXLOG_ERROR("[d3d][noring] the ring did not go idle within 5 s; the no-ring mode is not started");
    return;
  }
  g_state.defer.store(REXCVAR_GET(wwe2k14_d3d_noring_defer) != 0);
  g_state.level.store(level);
  Count(kEnter);
  REXLOG_INFO("[d3d][noring] on at guest frame {}: level {} ({})", wwe2k14::g_guest_frame.load(), level,
              level >= 3 ? "records by value" : level == 2 ? "typed sync items" : "segments through the queue");
}

inline void Leave(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  // Everything queued runs first (the consumer publishes the barrier ticket), then the open piece goes the library's way: the
  // kick writes its fence pair and submits to the ring, the library's counters being true all along.
  const uint32_t ticket = g_state.ticket.fetch_add(1) + 1;
  PublishPiece(base, dev, false);
  const bool was_deferring = g_state.defer.load();
  g_state.defer.store(false);
  FlushPending();
  const uint32_t f[6] = {ticket, 0, 0, 0, 0, 0};
  Push(lay::kBarrier, f, nullptr, 0);
  Count(kPushBarrier);
  if (!WaitFor([&] { return int32_t(g_state.shared.consumed_ticket.load(std::memory_order_acquire) - ticket) >= 0; }, 5000)) {
    REXLOG_ERROR("[d3d][noring] the queue did not drain within 5 s at the switch back; staying on");
    g_state.defer.store(was_deferring);  // (review C3: the mode stays on as it was)
    return;
  }
  g_state.level.store(0);
  Count(kLeave);
  ctx.r3.u64 = dev;
  ctx.lr = 0x82CB3300;
  sub_82CB6EA8(ctx, base);
  REXLOG_INFO("[d3d][noring] off at guest frame {}", wwe2k14::g_guest_frame.load());
}

// Called at the start of each flip handshake (once a frame on the render thread). The level wanted changes only here.
inline void TickTransition(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  if (t_original_depth) return;
  int want = REXCVAR_GET(wwe2k14_d3d_noring);
  // Lane 241 (3b): guest_gpu_only has no command processor to consume the queue (Enter would kick the ring and wait 5 s for a fence nobody writes, every frame).
  if (wwe2k14::GuestGpuOnly()) want = 0;
  if (want > 0 && (!AbiOk() || int64_t(wwe2k14::g_guest_frame.load()) < REXCVAR_GET(wwe2k14_d3d_noring_from_frame))) want = 0;
  // Review A2: the no-ring levels are for the one-tile scene. Without force_no_msaa the game tiles (4 strips, the D3D worker replays the
  // recorded segments per strip): stay on the ring.
  if (want > 0 && rex::cvar::GetFlagByName("wwe2k14_force_no_msaa") != "true") {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_WARN("[d3d][noring] off: wwe2k14_force_no_msaa is not on - the scene tiles, the no-ring levels need one tile");
    want = 0;
  }
  // Review A4 / A5: the library's no-GPU mode (byte 11069 bit 0x02: it writes the fences itself, Session 51), the SDK's detached mode
  // (gpu_native_detached 2 sets that bit from the present hook), or a debug object / submit callback on the device (the ring submit would
  // call into them): never combine with the queue - leave the mode if it is on.
  if (want > 0) {
    rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
    const bool detached = cp && cp->half2_detached_wanted();
    if ((base[dev + kDevByte11069] & 0x02) || detached || dl::Ld32(base, dev + kDevDebugObject) || dl::Ld32(base, dev + kDevSubmitCallback)) {
      Count(kRefusedNoGpuBit);
      want = 0;
    }
  }
  const int cur = Level();
  if (want == cur) return;
  if (cur != 0) Leave(ctx, base, dev);
  if (want != 0 && Level() == 0) Enter(ctx, base, dev, want);
}

// ------------------------------------------------------------------------------------------------------------------------------
// The census (every 600 guest frames, on the render thread at the flip handshake)
// ------------------------------------------------------------------------------------------------------------------------------
inline void CensusMaybe() {
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  uint64_t last = g_state.census_frame.load(std::memory_order_relaxed);
  if (frame < last + 600 || !g_state.census_frame.compare_exchange_strong(last, frame)) return;
  if (!Level() && !g_state.n[kEnter].load(std::memory_order_relaxed)) return;
  std::string out;
  for (uint32_t c = 0; c < kCounterCount; ++c) {
    const uint64_t v = g_state.n[c].exchange(0, std::memory_order_relaxed);
    if (v) out += (out.empty() ? "" : ", ") + std::string(kCounterNames[c]) + " " + std::to_string(v);
  }
  std::string stray;
  {
    std::lock_guard<std::mutex> lock(g_stray_mutex);
    std::vector<std::pair<uint64_t, std::string>> rows;
    for (auto& [k, v] : g_stray_by) rows.push_back({v, k});
    g_stray_by.clear();
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < rows.size() && i < 12; ++i) stray += (i ? "; " : "") + rows[i].second + " " + std::to_string(rows[i].first);
  }
  if (!stray.empty()) REXLOG_INFO("[d3d][noring] stray dwords by what they are (tier 2c: the ring packets left in the scene): {}", stray);
  lay::Shared& sh = g_state.shared;
  REXLOG_INFO("[d3d][noring] frames {}-{}: level {}; producer: {}; queue now {} dwords deep; consumer so far: segment dwords {}, "
              "packet dwords {}, fences {}, flip waits {} ({:.1f} ms), errors {}",
              last, frame, Level(), out.empty() ? "nothing" : out,
              uint32_t(sh.tail.load(std::memory_order_relaxed) - sh.head.load(std::memory_order_relaxed)),
              sh.segment_dwords.load(std::memory_order_relaxed), sh.packet_dwords.load(std::memory_order_relaxed),
              sh.fences.load(std::memory_order_relaxed), sh.flip_waits.load(std::memory_order_relaxed),
              double(sh.flip_wait_us.load(std::memory_order_relaxed)) / 1000.0, sh.bad.load(std::memory_order_relaxed));
}

// ------------------------------------------------------------------------------------------------------------------------------
// The typed emitters
// ------------------------------------------------------------------------------------------------------------------------------

// What 0x82CB6510 (the interrupt emitter) does to the stream, as an item. r5 flags, r6 callback, r7 argument. `options`: bit 0 pre-arm,
// bit 1 post-wait (both only from the flip handshake). Returns false = cannot (nothing was changed): the caller runs the original.
inline bool EmitInterrupt(uint8_t* base, uint32_t dev, uint32_t r5, uint32_t callback, uint32_t argument, uint32_t options) {
  if (dl::Ld32(base, dev + kDevPredicate) != 0 || (base[dev + kDevByte11068] & 1)) {
    Count(dl::Ld32(base, dev + kDevPredicate) != 0 ? kFbPredicate : kFbTiling);
    return false;
  }
  uint32_t flags = r5;
  if (!(flags & 6)) flags |= 6;  // (the emitter's default)
  const bool flush = (flags & 2) && (!(flags & 1) || (base[dev + kDevByte11072] & 0x80));
  if (flush) {
    ApplyMiscFlush(base, dev);
    options |= 4;
  }
  PublishPiece(base, dev);
  const lay::Kind kind = callback == kFlipScheduler ? lay::kFlipHandshake
                         : (callback == kSceneBegin || callback == kSceneEnd) ? lay::kSceneTiming : lay::kInterrupt;
  const uint32_t f[6] = {r5, callback, argument, GpuForm(dl::Ld32(base, dev + kDevScratch)), options, 0};
  Push(kind, f, nullptr, 0);
  Count(kind == lay::kFlipHandshake ? kPushFlip : kind == lay::kSceneTiming ? kPushScene : kPushInterrupt);
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------------
// Hooks
// ------------------------------------------------------------------------------------------------------------------------------

// Ring submit 0x82CB5EF0 (r3 device, r4 an array of {size | flags << 24, address} pairs, r5 the count): with the queue on, the pieces
// go to the queue; nothing is written to the ring, no WPTR (so the library's ring-space wait, reason 1, is never reached).
REX_HOOK_RAW(sub_82CB5EF0) {
  if (wwe2k14::owner_timing::Submit(ctx, base)) return;
  const uint32_t dev = ctx.r3.u32;
  // (Review A1: at Level >= 1 a kick NEVER reaches the real ring - not from the fallbacks that run an original under t_original_depth, not in
  // the recording modes: queued work is pending and an early fence would free segment memory (Session 51, failure 1). The debug objects
  // make TickTransition leave the mode instead, below.)
  if (Level() < 1 || dl::Ld32(base, dev + kDevDebugObject) || dl::Ld32(base, dev + kDevSubmitCallback)) {
    if (Level() >= 1) Count(kFbDebugObject);
    __imp__sub_82CB5EF0(ctx, base);
    return;
  }
  const uint32_t desc = ctx.r4.u32, count = ctx.r5.u32;
  const bool pfd = count && (dl::Ld32(base, desc) >> 24) == 0x8F;  // r21 of the listing: the first byte 143 = INDIRECT_BUFFER_PFD
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t header = dl::Ld32(base, desc + 8 * i), address = dl::Ld32(base, desc + 8 * i + 4);
    const uint32_t length = header & 0x00FFFFFFu;
    JustPublished& jp = t_just_published;
    if (jp.valid && length == jp.length && address == jp.gpu) {  // the kick's second submit of the piece the fence hook published
      jp.valid = false;
      continue;
    }
    if (length) PushSegment(address, length, pfd ? 1u : 0u);
    Count(kPieceDwords, length);
  }
  if (Deferring()) FlushPending();  // the kick: everything produced since the last one is the ring's segment now
}

// The segment-end fence pair 0x82CB63D0 (r3 device, r4 the write pointer; returns the pointer after what it wrote): a Fence item,
// written by the consumer after every item before it. The library's own bookkeeping is kept as the original keeps it.
REX_HOOK_RAW(sub_82CB63D0) {
  if (wwe2k14::owner_timing::Fence(ctx, base)) return;
  const uint32_t dev = ctx.r3.u32, ptr = ctx.r4.u32;
  if (Level() < 2 || t_original_depth || Recording(base, dev)) {
    __imp__sub_82CB63D0(ctx, base);
    return;
  }
  const uint32_t heap_bits = dl::Ld32(base, dev + kDevHeapBits);
  const uint32_t write_ptr = dl::Ld32(base, dev + kDevWritePtr);
  const uint32_t fence = dl::Ld32(base, dev + kDevFenceNext);
  const uint32_t block = dl::Ld32(base, dev + kDevFenceBlock);
  const uint32_t marker = (heap_bits & 3u) | write_ptr;
  dl::St32(base, dev + 13608, write_ptr);  // (the original's two bookkeeping stores)
  dl::St32(base, dev + 13612, heap_bits);
  const bool flush = (base[dev + kDevByte11072] & 0x80) != 0;
  if (flush) ApplyMiscFlush(base, dev);
  t_just_published = JustPublished{};
  PublishPiece(base, dev, true, &t_just_published);
  const uint32_t f[6] = {fence, marker, GpuForm(block + 4) | 2u, GpuForm(block) | 2u, flush ? 1u : 0u, 0};
  Push(lay::kFence, f, nullptr, 0);
  Count(kPushFence);
  dl::St32(base, dev + kDevFenceNext, fence + 2);  // (the original's last store)
  ctx.r3.u64 = ptr;                                // nothing was written: the pointer is where it was
}

// The interrupt emitter 0x82CB6510 (r3 device, r4 the write pointer, r5 flags, r6 callback, r7 argument; returns the pointer after what
// it wrote). Users: the flip handshake (replaced as a whole below), the scene-timing pair (0x82CB71D0), the tiling hand-off, profiling.
REX_HOOK_RAW(sub_82CB6510) {
  if (wwe2k14::owner_timing::Interrupt(ctx, base)) return;
  const uint32_t dev = ctx.r3.u32, ptr = ctx.r4.u32;
  if (t_expect_flip_arg != 0xFFFFFFFFu && ctx.r6.u32 == kFlipScheduler) {  // --wwe2k14_d3d_noring_verify: the original 3300's call
    if (ctx.r7.u32 == t_expect_flip_arg) {
      Count(kVerifyOk);
    } else {
      Count(kVerifyBad);
      REXLOG_ERROR("[d3d][noring] verify: 0x82CB3300 passes {:08X} to the emitter, our transcription computed {:08X}", ctx.r7.u32,
                   t_expect_flip_arg);
    }
    t_expect_flip_arg = 0xFFFFFFFFu;
  }
  if (Level() < 2 || t_original_depth || Recording(base, dev) || !EmitInterrupt(base, dev, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0)) {
    const uint32_t before = dl::Ld32(base, dev + kDevWritePtr);
    __imp__sub_82CB6510(ctx, base);
    NoteWriter("fallback: interrupt emitter 0x82CB6510", before, dl::Ld32(base, dev + kDevWritePtr));
    return;
  }
  ctx.r3.u64 = ptr;
}

// The present's flip handshake 0x82CB3300 (r3 device, r4 flag; listing read in Session 87): the pre-arm packets (flip word := 1,
// wait for it), the interrupt emitter with the flip scheduler and the packed interval word, and the wait for the flip word to be
// 0 - the one frame limiter - become ONE FlipHandshake item. The two helper calls (profiling packets only) and the per-frame bit
// stay. This is also where the ring <-> queue hand-over happens (a frame boundary).
REX_HOOK_RAW(sub_82CB3300) {
  if (wwe2k14::owner_timing::Flip(ctx, base)) return;
  const uint32_t dev = ctx.r3.u32, flag = ctx.r4.u32;
  if (!t_original_depth) {
    TickTransition(ctx, base, dev);
    CensusMaybe();
  }
  ctx.r3.u64 = dev;
  ctx.r4.u64 = flag;  // (TickTransition's library calls may have used them)
  // The packed word the original builds (listing 0x82CB33BC-0x82CB3418).
  const uint32_t mode = dl::Ld32(base, dev + kDevFrameRateMode);
  const uint32_t interval = (mode == 0 || mode == 1) ? 1u : mode == 2 ? 2u : mode == 4 ? 3u : 0u;
  const uint32_t packed = (((interval << 8) & 0xFFFFFF00u) | ((dl::Ld32(base, dev + kDevLateThreshold) >> 23) & 0x7Fu)) | flag;
  if (Level() < 2 || t_original_depth || Recording(base, dev)) {
    const int verify = REXCVAR_GET(wwe2k14_d3d_noring_verify);
    if (verify > 0 && !t_original_depth) {
      static std::atomic<int> done{0};
      if (done.fetch_add(1) < verify) t_expect_flip_arg = packed;
    }
    __imp__sub_82CB3300(ctx, base);
    t_expect_flip_arg = 0xFFFFFFFFu;
    return;
  }
  // The pre-arm / post-wait packets are written only when the flag is 0 and device+24584 is 0 (read before and after: unchanged).
  const bool waits = flag == 0 && dl::Ld32(base, dev + kDevNoWaitFlags) == 0;
  ctx.r3.u64 = dev;
  ctx.r4.u64 = 1;
  ctx.lr = 0x82CB331C;
  sub_82CC4338(ctx, base);
  ctx.r3.u64 = dev;
  ctx.r4.u64 = 0;
  ctx.lr = 0x82CB3328;
  sub_82CB7770(ctx, base);
  // (the original kicks here when the write pointer is past the soft end, once before the pre-arm packets and once before the
  // emitter; nothing is written now, one test covers both)
  if (dl::Ld32(base, dev + kDevWritePtr) > dl::Ld32(base, dev + kDevSoftEnd)) {
    ctx.r3.u64 = dev;
    ctx.lr = 0x82CB3354;
    sub_82CB6EA8(ctx, base);
  }
  if (!EmitInterrupt(base, dev, 0, kFlipScheduler, packed, waits ? 3u : 0u)) {
    // Cannot take it (the tiling extents wrapper is on after 0x82CB7770): the original runs whole, its packets are ordinary ones in
    // the segment, and the next typed item publishes them in order (counted by EmitInterrupt; the two helpers just run once more).
    ++t_original_depth;
    const uint32_t before = dl::Ld32(base, dev + kDevWritePtr);
    __imp__sub_82CB3300(ctx, base);
    NoteWriter("fallback: flip handshake 0x82CB3300", before, dl::Ld32(base, dev + kDevWritePtr));
    --t_original_depth;
    return;
  }
  ctx.r3.u64 = dev;
  ctx.r4.u64 = 0;
  ctx.lr = 0x82CB34C8;
  sub_82CC4338(ctx, base);
  base[dev + kDevByte11069] = uint8_t(base[dev + kDevByte11069] | 0x10);
  ctx.r3.u64 = dl::Ld32(base, dev + kDevWritePtr);
}

// The front-buffer address packet of the present, 0x82CB34E0 (r3 device, r4 front buffer, r5 colour-space word; listings 0x82CB34E0 and
// 0x82CB2BD8, read in Session 87). Normally it writes: a type-0 write of D1GRPH_PRIMARY_SURFACE_ADDRESS (0x1844) := the front buffer's GPU
// form; a REG_RMW on D1GRPH_CONTROL (0x1841): AND 0xFFFFF8FF, OR 0x100 when the word is 0x28280136 else 0; and DC_LUTA_CONTROL (0x1930)
// := 3 when the word is 0x28280136 else 0. Nothing in the SDK reads them (they only reach the register file): one Registers item.
// With device+24392 bit 4 or device+24584 > 0 it calls the flip handshake instead (our hook). Front buffer 0 (the gamma path, boot) is
// left to the original.
REX_HOOK_RAW(sub_82CB34E0) {
  const uint32_t dev = ctx.r3.u32, front = ctx.r4.u32, word = ctx.r5.u32;
  if (Level() < 2 || t_original_depth || Recording(base, dev)) {
    __imp__sub_82CB34E0(ctx, base);
    return;
  }
  if ((dl::Ld32(base, dev + 24392) & 4) || dl::Ld32(base, dev + kDevNoWaitFlags) > 0) {
    ctx.r3.u64 = dev;
    sub_82CB3300(ctx, base);  // (r4 is still the front buffer: the handshake's flag)
    return;
  }
  const uint32_t gpu = GpuForm(front);
  if (!gpu) {
    Count(kFbDisplay);
    ++t_original_depth;
    const uint32_t before = dl::Ld32(base, dev + kDevWritePtr);
    __imp__sub_82CB34E0(ctx, base);
    NoteWriter("fallback: front buffer 0x82CB34E0", before, dl::Ld32(base, dev + kDevWritePtr));
    --t_original_depth;
    return;
  }
  const bool special = word == 0x28280136u;
  const uint32_t q[12] = {lay::kRegsOpWrite, 0x1844, gpu, 0, lay::kRegsOpRmw, 0x1841, 0xFFFFF8FFu, special ? 0x100u : 0u,
                          lay::kRegsOpWrite, 0x1930, special ? 3u : 0u, 0};
  PublishPiece(base, dev);
  const uint32_t f[6] = {3, 0, 0, 0, 0, 0};
  Push(lay::kRegs, f, q, 12);
  Count(kPushRegs);
  ctx.r3.u64 = dl::Ld32(base, dev + kDevWritePtr);
}

// The state-dependent flush 0x82CB21F8 (r3 device; listing read in Session 87): register 0x5C8 := 0x20000, and when device byte 11072 has
// bit 0x40 also 0xE00 := 1 and 0xE40 := 1; then byte 11072 &= 0x3F, byte 11071 &= 0xFC, the misc dirty mask loses its top two bits.
// Called by the present and by the draw writers when they run as fallbacks (F1 carries the same writes inside its own record).
REX_HOOK_RAW(sub_82CB21F8) {
  const uint32_t dev = ctx.r3.u32;
  if (Level() < 2 || t_original_depth || Recording(base, dev)) {
    __imp__sub_82CB21F8(ctx, base);
    return;
  }
  const bool extra = (base[dev + kDevByte11072] & 0x40) != 0;
  const uint32_t q[12] = {lay::kRegsOpWrite, 0x5C8, 0x20000u, 0, lay::kRegsOpWrite, 0xE00, 1, 0, lay::kRegsOpWrite, 0xE40, 1, 0};
  PublishPiece(base, dev);
  const uint32_t f[6] = {extra ? 3u : 1u, 0, 0, 0, 0, 0};
  Push(lay::kRegs, f, q, extra ? 12 : 4);
  Count(kPushRegs);
  base[dev + kDevByte11072] = uint8_t(base[dev + kDevByte11072] & 0x3F);
  base[dev + kDevByte11071] = uint8_t(base[dev + kDevByte11071] & 0xFC);
  dl::St64(base, dev + kDevMiscMask, dl::Ld64(base, dev + kDevMiscMask) & 0x3FFFFFFFFFFFFFFFull);
  ctx.r3.u64 = dl::Ld32(base, dev + kDevWritePtr);
}

// The window scissor setter 0x82CA8CD0 (r3 device, r4 = left, r5 = top, r6 = right, r7 = bottom; db d3d_set_scissor_tiled; listing read in
// Session 87). Called ~160 times a frame in the bench: by the GPR split's tail 0x82CB04D0 (F2 runs it), by the viewport / scissor setter
// 0x82CAB280, the bin-state restore 0x82CAB868 and the clear / resolve cores. It writes its packets straight into the segment. In the
// plain case (device byte 11068 neither the tile-size variant 0x10 nor, with 0x20, a surface change) it is ONE type-0 packet:
// PA_SC_WINDOW_OFFSET (0x2080) := 0, PA_SC_WINDOW_SCISSOR_TL (0x2081) := (top << 16 & 0x3FFF0000) | (left & 0x3FFF), _BR (0x2082) :=
// (bottom << 16 & 0x3FFF0000) | (right & 0x3FFF). That case is a Registers item; the tile variants run the original (noted).
REX_HOOK_RAW(sub_82CA8CD0) {
  const uint32_t dev = ctx.r3.u32;
  if (Level() < 2 || t_original_depth || Recording(base, dev) || !REXCVAR_GET(wwe2k14_d3d_noring_scissor)) {
    __imp__sub_82CA8CD0(ctx, base);
    return;
  }
  const uint32_t left = ctx.r4.u32, top = ctx.r5.u32, right = ctx.r6.u32, bottom = ctx.r7.u32;
  const uint8_t b = base[dev + kDevByte11068];
  // The listing's flag (0x82CA8D08-0x82CA8DA8): 1 = the tile variant. Byte 11068 bit 0x10 -> 1. Else bit 0x20 clear -> 0. Else (0x20 set) -> 1 only
  // if every one of the five bound surfaces (device+12816..12832) equals its saved copy (device+13104..13120) or is unbound (0); one that
  // changed to a non-zero value gives 0. Plain = flag 0. (Review A3: the first version had the 0x20 branch inverted.)
  bool plain;
  if (b & 0x10) {
    plain = false;
  } else if (!(b & 0x20)) {
    plain = true;
  } else {
    bool unchanged_or_unbound = true;
    for (uint32_t k = 0; k < 5; ++k) {
      const uint32_t cur = dl::Ld32(base, dev + 12816 + 4 * k), saved = dl::Ld32(base, dev + 13104 + 4 * k);
      if (cur != saved && cur != 0) unchanged_or_unbound = false;
    }
    plain = !unchanged_or_unbound;
  }
  // (the listing's flag is 1 for the tile variants: plain = flag == 0)
  if (!plain) {
    const uint32_t before = dl::Ld32(base, dev + kDevWritePtr);
    ++t_original_depth;
    __imp__sub_82CA8CD0(ctx, base);
    --t_original_depth;
    NoteWriter("fallback: window scissor tile variant 0x82CA8CD0", before, dl::Ld32(base, dev + kDevWritePtr));
    return;
  }
  const uint32_t tl = ((top << 16) & 0x3FFF0000u) | (left & 0x3FFFu);
  const uint32_t br = ((bottom << 16) & 0x3FFF0000u) | (right & 0x3FFFu);
  const uint32_t q[12] = {lay::kRegsOpWrite, 0x2080, 0, 0, lay::kRegsOpWrite, 0x2081, tl, 0, lay::kRegsOpWrite, 0x2082, br, 0};
  PublishPiece(base, dev);
  const uint32_t f[6] = {3, 0, 0, 0, 0, 0};
  Push(lay::kRegs, f, q, 12);
  Count(kPushRegs);
  ctx.r3.u64 = dl::Ld32(base, dev + kDevWritePtr);
}

// The SDK's capability for the bin-state ops of the Registers item (gpu_d3d_noring_ops): an SDK without it would take them for plain writes.
inline bool BinOpsOk() {
  static const bool ok = rex::cvar::GetFlagByName("gpu_d3d_noring_ops") == "1";
  return ok;
}

// The default bin state + scissor restore 0x82CAB868 (r3 device, r4: 0 = also reset the bin state; db d3d_restore_bin_state_and_scissor;
// listing read in Session 87). The present calls it with 0 once a frame. With 0 it writes four SET_BIN_* packets (mask lo := device+13084,
// select lo := all ones, mask hi := device+13088, select hi := 0), then re-issues the stored scissor through 0x82CA8CD0, calls 0x82CBAEE0 and
// marks every dirty mask. What the SDK does with the bin state: bin_mask_ / bin_select_ (and the drawing side's own pair) are read only by
// PREDICATED packets (ExecutePacket: "(bin_select_ & bin_mask_) != 0", header bit 0; command_processor.cpp) and the tiling log / cpf_bin
// counters. The library sets the predicate bit only inside the tiling extents mode (device+13092 / byte 11068 bit 0; EmitInterrupt and the
// records refuse that mode), so with one tile nothing in the bench reads these values - they are still KEPT exactly (a Registers item,
// ops 3-6, run on both sides like the packets: an answer, not a drop). The rest of the function is the original run with r4 = 1, which
// skips the bin packets and nothing else. Needs the SDK capability gpu_d3d_noring_ops (else the original runs and the packets are strays).
REX_HOOK_RAW(sub_82CAB868) {
  const uint32_t dev = ctx.r3.u32;
  if (Level() < 2 || t_original_depth || Recording(base, dev) || ctx.r4.u32 != 0 || !BinOpsOk()) {
    __imp__sub_82CAB868(ctx, base);
    return;
  }
  const uint32_t q[16] = {lay::kRegsOpBinMaskLo, 0, dl::Ld32(base, dev + 13084), 0, lay::kRegsOpBinSelectLo, 0, 0xFFFFFFFFu, 0,
                          lay::kRegsOpBinMaskHi, 0, dl::Ld32(base, dev + 13088), 0, lay::kRegsOpBinSelectHi, 0, 0,           0};
  PublishPiece(base, dev);
  const uint32_t f[6] = {4, 0, 0, 0, 0, 0};
  Push(lay::kRegs, f, q, 16);
  Count(kPushRegs);
  ctx.r4.u64 = 1;
  __imp__sub_82CAB868(ctx, base);
}

// The extents-wrapper decision 0x82CB7770 (r3 device, r4 = 0: decide tiling from the device state and reset the mask; r4 != 0: a predicate
// mask for the wrapper; listing read in Session 87 with the callers 0x82CB3300 (r4 = 0, once a frame), 0x82C22650 (the scene's close:
// three calls, r4 = 0 and the masks) and 0x82CB7C50). BOTH branches end the same way (listing 0x82CB7890-0x82CB78C8): device+13088 := 0,
// +11060 := r4, +11064 := 0, a kick if the write pointer is past the soft end, then ONE packet SET_BIN_MASK_LO := device+13084 (-1 after
// r4 = 0, else r4). That packet is the run-086 stray (value 0xFFFFFFFF: r4 = 0 calls, value 3: the scene close's mask). The state the
// function computes is left to the original; only its last packet is taken back and answered as a Registers item (bin op 3, both sides):
// when the original wrote exactly the two dwords at the old write pointer (no kick, no segment switch) the pointer is put back and the value
// pushed instead; anything else stays as it was (a stray, counted by the census).
REX_HOOK_RAW(sub_82CB7770) {
  const uint32_t dev = ctx.r3.u32;
  if (Level() < 2 || t_original_depth || !BinOpsOk() || dl::Ld32(base, dev + 13600) != 0) {
    __imp__sub_82CB7770(ctx, base);
    return;
  }
  const uint32_t before = dl::Ld32(base, dev + kDevWritePtr);
  __imp__sub_82CB7770(ctx, base);
  const uint32_t after = dl::Ld32(base, dev + kDevWritePtr);
  if (after != before + 8 || dl::Ld32(base, before + 4) != 0xC0006000u) return;  // (a kick moved it, or not the expected packet: left as written)
  const uint32_t value = dl::Ld32(base, before + 8);
  dl::St32(base, dev + kDevWritePtr, before);  // the packet is taken back ...
  PublishPiece(base, dev);                     // ... what precedes it goes first ...
  const uint32_t q[4] = {lay::kRegsOpBinMaskLo, 0, value, 0};
  const uint32_t f[6] = {1, 0, 0, 0, 0, 0};
  Push(lay::kRegs, f, q, 4);                   // ... then the mask, as an item
  Count(kPushRegs);
  ctx.r3.u64 = before;
}

// Session 92 M5b: the kick's coherency handshake 0x82CB61C0 (r3 device, r4 -> out GPU-form block address, r5 -> out dword count; callers
// 0x82CB6EA8 = the kick and 0x82CCA850 = the tiling end, both of which read only the count and skip the submit on 0). On the ordinary path
// (device+12248 qword == 0x00000000FFFFFFFF - the game's "empty range", set at device setup and by 0x82CB5700 - and device+12256 == 0) it
// emits 11 dwords when device byte 11070 bit 0x04 is set (else a count of 0, its own normal path): COHER_SIZE / BASE / STATUS_HOST
// := 0x01000000 + a WAIT_REG_MEM on 0xA31 for bit 0x80000000, which therefore always passes at once (confirmed as the per-frame segment
// piece by M5a's run 14; no guest code reads 0xA31). Dropped: *r5 := 0, nothing allocated or written (the same as the original's
// bit-clear path). Counted in the block (coher_dropped). Every other path runs the original. Review: S92 M5b (the empty-range value).
REX_HOOK_RAW(sub_82CB61C0) {
  const uint32_t dev = ctx.r3.u32, out_count = ctx.r5.u32;
  if (Level() < 3 || t_original_depth || !REXCVAR_GET(wwe2k14_d3d_drop_coher) || Recording(base, dev) ||
      dl::Ld64(base, dev + 12248) != 0x00000000FFFFFFFFull || dl::Ld32(base, dev + 12256) != 0) {
    __imp__sub_82CB61C0(ctx, base);
    return;
  }
  if (base[dev + 11070] & 0x04) g_state.shared.coher_dropped.fetch_add(1, std::memory_order_relaxed);  // (the original would have emitted 11 dwords)
  dl::St32(base, out_count, 0);
}

// 0x82CBAEE0 (r3 device, r4 value; called by 0x82CAB868 and, at boot, 0x82CC0150): device+11044 bits 12-13 := r4 (rlwimi 12,18,19 = the
// mask 0x3000), then register 0x5C8 := 0x20000 and register 0xF01 := the new device+11044. The SDK reads neither register (0x5C8 is the
// coherency-flush marker the other segment-end writes use; 0xF01 is named nowhere in it): plain register images, so the register file is
// exactly what the packets would have left.
REX_HOOK_RAW(sub_82CBAEE0) {
  const uint32_t dev = ctx.r3.u32;
  if (Level() < 2 || t_original_depth || Recording(base, dev)) {
    __imp__sub_82CBAEE0(ctx, base);
    return;
  }
  const uint32_t value = (dl::Ld32(base, dev + 11044) & ~0x3000u) | ((ctx.r4.u32 << 12) & 0x3000u);
  dl::St32(base, dev + 11044, value);
  const uint32_t q[8] = {lay::kRegsOpWrite, 0x5C8, 0x20000u, 0, lay::kRegsOpWrite, 0xF01, value, 0};
  PublishPiece(base, dev);
  const uint32_t f[6] = {2, 0, 0, 0, 0, 0};
  Push(lay::kRegs, f, q, 8);
  Count(kPushRegs);
  ctx.r3.u64 = dl::Ld32(base, dev + kDevWritePtr);
}

// ------------------------------------------------------------------------------------------------------------------------------
// The present (glue called by VdSwapOverride, src\kernel_overrides.h, AFTER rex::kernel::xboxkrnl::VdSwap_entry has formed its words)
// ------------------------------------------------------------------------------------------------------------------------------

// The device the library itself uses for its scratch block (listing 0x82CB6510: *(*(0x82000000 + 2132 or 2136))); one device in this game.
inline uint32_t GlobalDevice(uint8_t* base) {
  for (const uint32_t slot : {2132u, 2136u}) {  // (the library picks by the process type; both hold the one device)
    const uint32_t holder = dl::Ld32(base, 0x82000000u + slot);
    if (const uint32_t dev = holder ? dl::Ld32(base, holder) : 0) return dev;
  }
  return 0;
}

// buffer_guest: the 64-dword area the present reserved in the open segment (VdSwap's first argument); VdSwap_entry has written its
// fetch-constant packet, its XE_SWAP packet and type-2 NOPs into it. They become one Present item (run as ring-level packets by
// the consumer); the area is skipped by moving the library's piece start behind it. Returns false = not taken (the words stay in
// the segment and run with it, as on the ring).
inline bool AfterVdSwap(uint8_t* base, uint32_t buffer_guest) {
  if (Level() < 2 || t_original_depth || !buffer_guest) return false;
  const uint32_t dev = GlobalDevice(base);
  if (!dev) {
    Count(kFbNoDevice);
    return false;
  }
  if (Recording(base, dev)) return false;
  // (The reservation 0x82CB70E0 does NOT move the write pointer: the present sets device+48 to the area's last dword itself after
  // VdSwap, 0x82CB3B4C. So at this point the area lies BEYOND device+48 + 4 and the open piece ends where the area starts.)
  const uint32_t start = dl::Ld32(base, dev + kDevPieceStart);
  if (!(start && start <= buffer_guest && buffer_guest - start < (4u << 20))) {
    Count(kFbPresentWindow);  // the area is not behind the open piece (unexpected): leave it to the segment
    return false;
  }
  uint32_t words[64];
  uint32_t used = 0;
  for (uint32_t i = 0; i < 64; ++i) {
    words[i] = dl::Ld32(base, buffer_guest + 4 * i);
    if (words[i] != 0x80000000u) used = i + 1;  // (the trailing type-2 NOPs are dropped)
  }
  if (!used) return false;
  // Everything before the reservation first, in order; then the swap; then the library's piece starts behind the area.
  if (buffer_guest > start) {
    PushSegment(GpuForm(start), (buffer_guest - start) / 4, 0);
    Count(kPieceDwords, (buffer_guest - start) / 4);
    if (Level() >= 3) {
      Count(kStrayDwords, (buffer_guest - start) / 4);
      AccountStray(base, start, buffer_guest);
    }
  }
  // XE_SWAP's words: [7] header, [8] signature, [9] front buffer (physical), [10] width, [11] height (src\kernel\xboxkrnl\xboxkrnl_video.cpp).
  const uint32_t f[6] = {used > 9 ? words[9] : 0, used > 10 ? words[10] : 0, used > 11 ? words[11] : 0, 0, 0, 0};
  Push(lay::kPresent, f, words, used);
  Count(kPushPresent);
  dl::St32(base, dev + kDevPieceStart, buffer_guest + 256);
  for (uint32_t i = 0; i < 64; ++i) dl::St32(base, buffer_guest + 4 * i, 0x80000000u);  // (type-2 NOPs, as the SDK leaves the spare words)
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------------
// Tier 2c: the F1 / F2 / F3 record tokens by value. Called by d3d_layer::WriteRing instead of writing the ring (one line of glue).
// ------------------------------------------------------------------------------------------------------------------------------
// w / n: the packet exactly as WriteRing would write it (header word first, host order; ONE packet per call: F1's token, F2's copy
// record, F3's inline record). The item's tag is the packet's opcode, which is how the consumer knows what a draw in it is. The
// library's half2 entry for the call (AroundWriter reserved it before the writer ran) is left with an empty packet range - nothing
// was written to the segment - and the consumer drops such entries from the queue head (Half2DropDeadLibraryDraws).
// Returns false = not taken (level < 3): the caller writes it into the segment as before.
inline bool TakeRecord(uint8_t* base, uint32_t dev, const uint32_t* w, uint32_t n) {
  if (Level() < 3 || t_original_depth || Recording(base, dev)) return false;
  PublishPiece(base, dev);  // (anything a fallback writer left in the segment runs first)
  const uint32_t f[6] = {(w[0] >> 8) & 0x7Fu, 0, 0, 0, 0, 0};
  Push(lay::kPackets, f, w, n);
  Count(kPushPackets);
  return true;
}

}  // namespace wwe2k14::d3d_noring
