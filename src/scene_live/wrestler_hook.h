// Class C4, the wrestlers: the game-side half (Session 101, lane 29; docs\systems\wrestler_pose.md). Header-only, included by fp_probe.h
// (the existing mid-asm hooks of sub_82BD30E8; no new hook, no codegen). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// Read only: it never writes guest memory or changes a register. One relaxed load when no sink wants records.
//   OnSkelIn(self, count)  at 0x82BD31C8 (bone loop start; r31 = skeleton, r28 = node count): remember this call on this thread.
//   OnSkelExit(self)       at 0x82BD35A4 (the common exit): if the call passed the loop start and the skeleton is wrestler-sized, copy the
//                          world matrices (skeleton+192, count x 64 bytes) and the inverse bind (skeleton+224), match the character slot,
//                          and Submit. The early returns of the function never pass the loop-start hook, so they submit nothing.
// Slot match: character slot table 0x836F38D0 (9 pointers); char+176 is the position vector the game keeps for the character, and for slots
// 0 and 1 it equals (skeleton+192 world array) + 2*64 + 48, i.e. bone 2's translation (docs\systems\determinism.md Result 6, traces of two
// slots in one run). The hook uses exactly that pointer equality and nothing else; a skeleton that matches no slot stays "skel:...".
// The equality is evidenced for slots 0 and 1 only; for slots 2..8 it is the same rule, unconfirmed.
#pragma once
#include <rex/logging.h>
#include "../renderer_adapter/census_flush.h"   // S102 CEN1
#include "../renderer_scene/bone_tag_rows.h"   // S106 G4-C: SpringHairCensusText
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <functional>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "../renderer_adapter/upd_lean.h"   // S102 U3 (bit 16)
#include "wrestler_contrib.h"
#include "slot_key.h"   // S103 SKY
#include "inputs_core.h"   // S108 GAVF: g_track_act_unreadable (census)
#include "../feed_seen.h"   // S110 FEEDREC
#include "piece_link.h"   // S103 ARM2: DescriptorOfBody (the slot manager's descriptor of the body)
#include "../assets/attire_build.h"   // S103 ARM2: SlotOwner / the build-step latch
#include "safe_guest_pages.h"
#include "../frame_stats.h"   // g_guest_frame: the cache epoch
#include "../guest_read.h"   // GuestWordReadableFast (the off-Windows page check)
#include "../renderer_scene/body_scale.h"   // S103 W2 (census only)
#include "../renderer_game/scene_frame.h"   // S103 S: the slot-key read only while the scene switch is on

namespace wwe2k14::wrestlers {

// A guest range is read only after the guest page table says every page is readable (safe_guest_pages.h; the inputs_glue.h ReadChecked pattern), on every platform. On Windows SafeCopy's SEH stays as the guard for the check-then-copy race only (S108 GAVF): a host fault is no longer the validity check, because the SDK's handler logs every caught fault as an error line `Unhandled guest access violation` (xmemory.cpp:909) and takes a lock, and a wild value of ours became a line no one can tell from a game fault.
// Off Windows (no SEH) the virtual check is trusted only on a 4 KB-page kernel (HostPagesOk). The check itself is right on a larger host page too (the SDK grants a host page the union of its guest pages' access); the hazard is the gap between check and copy: a Release that empties every guest page of a host page makes it PROT_NONE and the plain memcpy in SafeCopy takes SIGSEGV (BH3-1). A non-4 KB host still reads (refusing would stop all 3D) and logs one warning at the first check. The proper fix (a SIGSEGV recovery point around SafeCopy, ahead of blackbox's handler) is NOT built.
inline std::atomic<uint64_t> g_page_check_calls{0}, g_page_check_pages{0}, g_page_check_hits{0}, g_page_check_refused{0};   // the check's cost, printed with the wrestlers census (OnSkelExit)
inline bool HostPagesOk() {
#ifdef _WIN32
  return true;
#else
  return wwe2k14::safe_pages::HostPageIs4k(rex::memory::page_size());
#endif
}
// S108 GAVF: a refused WILD address (>= 0x10000) is named once per distinct address, 8 a run, with the caller's exe-relative frames (resolve with the .map beside the exe). Called on every refusal: the mutex is taken until the 8 lines are out, then a relaxed load ends it.
inline void NoteRefusedRead(uint32_t guest, size_t n) {
  static std::mutex mu;
  static wwe2k14::safe_pages::RefusalLog log;
  static std::atomic<bool> full{false};
  if (full.load(std::memory_order_relaxed)) return;
  {
    std::lock_guard<std::mutex> lock(mu);
    const bool say = log.ShouldLog(guest);
    if (log.n >= log.kMax) full.store(true, std::memory_order_relaxed);
    if (!say) return;
  }
  std::string frames;
#ifdef _WIN32
  void* pcs[16];
  const USHORT count = RtlCaptureStackBackTrace(1, 16, pcs, nullptr);
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  for (USHORT i = 0; i < count; ++i) frames += fmt::format(" +0x{:X}", reinterpret_cast<uintptr_t>(pcs[i]) - base);
#endif
  REXLOG_WARN("[wwe2k14][guest_read] refused guest 0x{:08X} size {} (the page table says unreadable; first of at most 8 addresses a run). Caller frames, exe-relative (look up in the .map beside the exe):{}", guest, n, frames.empty() ? " (none)" : frames);
}
// The page check. MUST CHANGE TOGETHER with its hand copy in tools\scene\guest_check_bench.cpp (CheckRds): the bench prices exactly this body against the old Windows path (S108 RDS).
inline bool GuestRangeReadable(uint32_t guest, size_t n) {
#ifndef _WIN32
  static const bool k4k = [] {   // BH3-1 (review): a non-4 KB host still draws (a blanket refusal would stop all 3D); one warning that the reads are check-then-copy with no fault recovery
    const bool ok = wwe2k14::safe_pages::HostPageIs4k(rex::memory::page_size());
    if (!ok) REXLOG_WARN("[wwe2k14][guest_read] host page size {}: guest reads are check-then-copy without fault recovery; a rare crash is possible", rex::memory::page_size());
    return ok;
  }();
  (void)k4k;
#endif
  static thread_local wwe2k14::safe_pages::PageCache<> cache;   // readable pages of this frame (this thread): no lock
  static thread_local wwe2k14::safe_pages::FrameTally<> tally;   // S108 GAVF review: this thread's counts, published once a frame (no shared atomic per call)
  const auto frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  tally.Roll(frame, [](uint64_t c, uint64_t p, uint64_t h, uint64_t r) {
    g_page_check_calls.fetch_add(c, std::memory_order_relaxed);
    g_page_check_pages.fetch_add(p, std::memory_order_relaxed);
    g_page_check_hits.fetch_add(h, std::memory_order_relaxed);
    g_page_check_refused.fetch_add(r, std::memory_order_relaxed);
  });
  ++tally.calls;
  auto raw = [](uint32_t a) { return wwe2k14::GuestWordReadableFast(a); };
  const bool ok = wwe2k14::safe_pages::RangeReadable(guest, n, wwe2k14::safe_pages::Cached(cache, frame, tally.pages, tally.hits, raw));
  if (!ok) {
    ++tally.refused;
    if (guest >= wwe2k14::safe_pages::RefusalLog::kSmall) NoteRefusedRead(guest, n);
  }
  return ok;
}
// The same for PHYSICAL (GPU) memory read through TranslatePhysical: the physical heap's own table, per page, on the raw bytes the copy reads (safe_guest_pages.h PhysReadable).
inline bool PhysRangeReadable(uint32_t addr, size_t n) {
#ifdef _WIN32
  (void)addr; (void)n;
  return true;
#else
  auto* ph = REX_KERNEL_MEMORY()->GetPhysicalHeap();
  static thread_local wwe2k14::safe_pages::PageCache<> cache;   // physical pages of this frame (this thread), apart from the virtual cache
  static thread_local wwe2k14::safe_pages::FrameTally<> tally;   // S108 RDS: GuestRangeReadable's per-thread counts, published once a frame (was four shared atomic adds a call). UNCOMPILED here (no Android build in this lane)
  const auto frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  tally.Roll(frame, [](uint64_t c, uint64_t p, uint64_t h, uint64_t r) {
    g_page_check_calls.fetch_add(c, std::memory_order_relaxed);
    g_page_check_pages.fetch_add(p, std::memory_order_relaxed);
    g_page_check_hits.fetch_add(h, std::memory_order_relaxed);
    g_page_check_refused.fetch_add(r, std::memory_order_relaxed);
  });
  ++tally.calls;
  auto raw = [ph](uint32_t p) {
    uint32_t pr = 0;
    return ph->QueryProtect(p, &pr) && (pr & rex::memory::kMemoryProtectRead) != 0;
  };
  const bool ok = ph && wwe2k14::safe_pages::PhysReadable(addr, n, wwe2k14::safe_pages::Cached(cache, frame, tally.pages, tally.hits, raw));
  if (!ok) ++tally.refused;
  return ok;
#endif
}

inline bool SafeCopy(void* dst, const void* src, size_t n) {
  static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
#ifdef _WIN32
  __try {
    copy(dst, src, n);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
  return true;
#else
  copy(dst, src, n);
  return true;
#endif
}

// S102 U3 (R26 lane A (b), wwe2k14_rdr_upd_lean bit 16): the guest memory object, resolved once. Every guest read of the model-entry contributors (about 10 a call, a few hundred calls a frame) went through
// REX_KERNEL_MEMORY() = kernel_state()->memory(), a call and a load each time (kernel_state.cpp:74 0.05 ms in R26's profile). The Memory object lives as long as the kernel state, which lives as long as the process.
inline rex::memory::Memory* GuestMem() {
  if (wwe2k14::upd_lean::On(wwe2k14::upd_lean::kEntry2)) {
    static std::atomic<rex::memory::Memory*> cached{nullptr};
    rex::memory::Memory* m = cached.load(std::memory_order_relaxed);
    if (!m) cached.store(m = REX_KERNEL_MEMORY(), std::memory_order_relaxed);
    return m;
  }
  return REX_KERNEL_MEMORY();
}
// Big-endian guest bytes -> host, one checked read.
inline bool ReadGuestDirect(uint32_t guest, void* dst, size_t n) {
  if (guest < 0x10000 || uint64_t(guest) + n > 0x100000000ull) return false;
  if (uint64_t(guest) + n > 0x7F000000ull && guest < 0x80000000u) return false;  // MMIO heap
  if (wwe2k14::safe_pages::CrossesE0View(guest, n)) return false;   // BH3-5: only the start is translated
  auto* host = GuestMem()->TranslateVirtual<const uint8_t*>(guest);
  return host && GuestRangeReadable(guest, n) && SafeCopy(dst, host, n);
}
// S108 RDS phase 2: inside a pass scope (safe_guest_pages.h PassLines, this thread) a read of up to one line is served from the pass's line copy (the 128-byte aligned line, read once per pass by
// ReadGuestDirect, lies in the field's own page; every address rule above gives a line the field's verdict: the boundaries it tests are 128-byte aligned and a line never ends past 4 GB). Outside one: direct.
inline bool ReadGuestNoNote(uint32_t guest, void* dst, size_t n) {   // S110 FEEDREC: the non-recording twin (inner helpers call this one)
  if (n <= wwe2k14::safe_pages::LineCopy<>::kLine)
    if (wwe2k14::safe_pages::PassLines* pl = wwe2k14::safe_pages::PassLines::Current())
      return pl->lines.Get(guest, uint32_t(n), dst, [](uint32_t a, uint32_t k, void* d) { return ReadGuestDirect(a, d, k); });
  return ReadGuestDirect(guest, dst, n);
}
inline bool ReadGuest(uint32_t guest, void* dst, size_t n WWE_FEED_LOC_PARAM) {
  WWE_FEED_NOTE(guest, (uint32_t)n);
  return ReadGuestNoNote(guest, dst, n);
}
// ReadGuest's address rules for one range, without the read (the rows read applies them to its hull; a hull that passes them has every row passing them).
inline bool GuestRangeBoundsOk(uint32_t guest, uint64_t n) {
  if (guest < 0x10000 || uint64_t(guest) + n > 0x100000000ull) return false;
  if (uint64_t(guest) + n > 0x7F000000ull && guest < 0x80000000u) return false;  // MMIO heap
  return !wwe2k14::safe_pages::CrossesE0View(guest, n);
}
// S108 RDS phase 2: a picture plane's rows (`rows` x `width` bytes, `pitch` apart) into dst, packed. One page check of the rows' hull when safe_pages::RowsHullOk (the hull then asks exactly the pages the rows
// ask), each row then copied under SafeCopy's SEH; any other layout is one ReadGuest a row (the old path). Was one check a row: ~1,400 a movie flush (S108 RDS tally, CopyDisplay <- OnFlush).
inline bool ReadGuestRows(uint32_t guest, uint32_t pitch, uint32_t rows, uint32_t width, uint8_t* dst) {
  const uint8_t* host = nullptr;
  return wwe2k14::safe_pages::ReadRows(
      guest, pitch, rows, width, dst, [](uint32_t g, uint64_t n) { return GuestRangeBoundsOk(g, n); },
      [&host](uint32_t g, uint64_t n) { return (host = GuestMem()->TranslateVirtual<const uint8_t*>(g)) != nullptr && GuestRangeReadable(g, size_t(n)); },
      [&host, guest](uint32_t g, uint8_t* d, uint32_t n) { return SafeCopy(d, host + (g - guest), n); },   // the hull is one contiguous host range: the 0xE0000000 view is never crossed (GuestRangeBoundsOk)
      [](uint32_t g, uint8_t* d, uint32_t n) { return ReadGuest(g, d, n); });
}
// S102 U2 (R22 cut 3): compare n guest bytes in place with a host buffer: 0 equal, 1 differs, -1 unreadable (same bounds and fault rules as ReadGuest, no copy).
inline int CompareGuest(uint32_t guest, const void* ref, size_t n) {
  if (guest < 0x10000 || uint64_t(guest) + n > 0x100000000ull) return -1;
  if (uint64_t(guest) + n > 0x7F000000ull && guest < 0x80000000u) return -1;  // MMIO heap
  if (wwe2k14::safe_pages::CrossesE0View(guest, n)) return -1;   // BH3-5
  auto* host = GuestMem()->TranslateVirtual<const uint8_t*>(guest);
  if (!host || !GuestRangeReadable(guest, n)) return -1;
#ifdef _WIN32
  static int (*volatile cmp)(const void*, const void*, size_t) = std::memcmp;   // S102 U3 review: a call through a volatile pointer, so the fault is in a callee of the __try body (SafeCopy's pattern; tools\upd_lean lint)
  __try {
    return cmp(host, ref, n) != 0 ? 1 : 0;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return -1;
  }
#else
  return std::memcmp(host, ref, n) != 0 ? 1 : 0;
#endif
}
inline bool ReadWordNoNote(uint32_t guest, uint32_t& v) {   // S110 FEEDREC: the twin whose ADDRESS is taken (ReadWordFn users): a plain function in every build
  uint32_t raw = 0;
  if (!ReadGuestNoNote(guest, &raw, 4)) return false;
  v = __builtin_bswap32(raw);
  return true;
}
inline bool ReadWord(uint32_t guest, uint32_t& v WWE_FEED_LOC_PARAM) {
  WWE_FEED_NOTE(guest, 4);
  return ReadWordNoNote(guest, v);
}
inline bool ReadMatrices(uint32_t guest, uint32_t count, std::vector<rdr_scene::Mat4>& out WWE_FEED_LOC_PARAM) {
  out.resize(count);
  WWE_FEED_NOTE(guest, count * 64u);
  if (!ReadGuestNoNote(guest, out.data(), size_t(count) * 64)) return false;
  auto* w = reinterpret_cast<uint32_t*>(out.data());
  for (size_t i = 0; i < size_t(count) * 16; ++i) w[i] = __builtin_bswap32(w[i]);  // floats, byte-swapped as words
  return true;
}

constexpr uint32_t kSlotTable = 0x836F38D0;  // 9 character pointers (match_state.md, high)
constexpr uint32_t kMaxNodes = 1024;

// S103 SKY: name every slot body from the game's character descriptor (slot_key.h), not the participant row. Off = the old row rule (slot < 6, rows 6..39 empty).
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_slot_key_desc, true, "wwe2k14",
                    "Stream-free wrestlers: the slot model key comes from the game's character descriptor [[0x83924734]+864+80*[char+1220]] (right in a Royal Rumble and after a slot swap); false = the participant-row guess (slot < 6, rows 6..39 empty)");
namespace sky_census {   // desc key vs the old row key, slots 0..5 (the first live sighting of a swap / remap / Rumble is differ / row_unusable)
inline std::atomic<uint64_t> agree{0}, differ{0}, row_unusable{0};
inline std::atomic<uint32_t> occupant_char[9] = {}, occupant_key[9] = {};
inline std::atomic<uint32_t> occupant_lines{0};
inline std::atomic<uint64_t> remap_active{0};                                   // slots seen while the slot remap is active (descriptor indexed by char+1224)
inline std::atomic<uint64_t> scale_moved{0};                                    // S110 MGRFINGERS: latches whose scale record index != the slot position (each was read from another body's record before the fix)
inline std::atomic<uint64_t> ctor_agree{0}, ctor_differ{0};                      // [ch+44] vs the key used (census only, not a gate)
inline std::atomic<uint64_t> hi_link_agree{0}, hi_link_differ{0}, hi_link_verified{0}, hi_named_by_key{0}, hi_nokey{0};   // slots 6..8 (referee, role-4 people): key vs the declared link
}  // namespace sky_census

struct CallState {
  uint32_t self = 0, count = 0;
  bool active = false;
};
inline thread_local CallState t_call;
inline thread_local std::vector<rdr_scene::Mat4> t_world, t_inv;

inline void OnSkelIn(uint32_t self, uint32_t count_raw) {
  CallState& c = t_call;
  c.active = false;
  // Not registered = nobody calls Emit: collecting would fill memory for nothing (S101 lane 29 review 2).
  if (!rdr_scene::live::WrestlersRegistered() || !rdr_scene::live::Enabled()) return;
  const uint32_t count = int32_t(count_raw) < 0 ? 0 : std::min<uint32_t>(count_raw, kMaxNodes);
  auto& w = rdr_scene::live::Wrestlers();
  // S101 lane 206: a skeleton under MinNodes() that the game's own model descriptor lists as a wrestler piece (hair 48 nodes, caps, glasses: 55 file entries) is kept: its rows are its own.
  if (const uint32_t wanted = w.WantedCount(self)) {   // lane 209: a wanted own-skeleton address reached the update hook (with which count?)
    if (w.NoteWantedHookSaw(self, count, wanted)) REXLOG_INFO("[scene][live] own-skeleton want 0x{:08X}: update hook saw count {} (wanted {})", self, count, wanted);
  }
  if (!w.KeepSkeleton(self, count)) {
    if (count > 0) w.NoteSkippedSmall();
    return;
  }
  c.self = self;
  c.count = count;
  c.active = true;
}

inline void OnSkelExit(uint32_t self, uint64_t guest_frame) {  // guest_frame: wwe2k14::g_guest_frame read on this (the updating) thread
  CallState& c = t_call;
  if (!c.active || c.self != self) return;
  c.active = false;
  auto& w = rdr_scene::live::Wrestlers();
  {   // S101 lane 207: the wrestler pipeline's counters, one line per 600 guest frames (why the descriptor route links no wrestler; how many own-skeleton pieces are wanted)
    static std::atomic<uint64_t> next_log{600};
    uint64_t due = next_log.load(std::memory_order_relaxed);
    static census_flush::Site cf_site;   // S102 CEN1: a flip phase end prints the census now
    const bool cf_due = census_flush::Due(cf_site);
    if ((guest_frame >= due || cf_due) && next_log.compare_exchange_strong(due, guest_frame + 600)) {
      REXLOG_INFO("[scene][live] wrestlers frame {}: LinkBodies {} LinkKind {} {} {} {} {} {} {} {} {} {} LinkEmptyKey {} LinkNoBody {} LinkUnreadable {} LinkCountRefused {} LinkedSeenAsSkeleton {} KnownCount {} SkippedSmall {} CapRefused {} ListedFull {} Wanted {} WantedFull {} EntryPiecesByDescriptor {} EntryUnknownModel {} EntryPieces {} EntryPtrMismatch {} EntryUnreadable {} OwnSkelConsumer {} OwnSkelPtrMismatch {} PieceLinks {} PieceWhy [{}] RingLeftOut {} BodyLinks {} [{}] ModelsDestroyed {}",
                  guest_frame, w.LinkBodies(), w.LinkKind(0), w.LinkKind(1), w.LinkKind(2), w.LinkKind(3), w.LinkKind(4), w.LinkKind(5), w.LinkKind(6), w.LinkKind(7), w.LinkKind(8), w.LinkKind(9),
                  w.LinkEmptyKey(), w.LinkNoBody(), w.LinkUnreadable(), w.LinkCountRefused(), w.LinkedSeenAsSkeleton(), w.KnownCount(), w.SkippedSmall(), w.CapRefused(), w.ListedFull(), w.Wanted(), w.WantedFull(),
                  w.EntryPiecesByDescriptor(), w.EntryUnknownModel(), w.EntryPieces(), w.EntryPtrMismatch(), w.EntryUnreadable(), w.OwnSkelConsumer(), w.OwnSkelPtrMismatch(), w.PieceLinksMade(), w.PieceWhyText(), w.RingLeftOut(), w.BodyLinkCount(), w.BodyLinkText(), w.Released());   // S103 NRB / PZ F5; S108 RDS: ModelsDestroyed = the sub_82BD3B40 hook's ForgetModel calls
      REXLOG_INFO("[scene][live] frame {}: slot key (desc rule {}): agree {} differ {} row_unusable {} occupant changes {} remap_active {} scale index moved {} ctor44 agree {} differ {} slots6-8: link_agree {} link_differ {} link_verified {} named_by_key {} nokey {}", guest_frame, REXCVAR_GET(wwe2k14_rdr_slot_key_desc) ? "on" : "off", sky_census::agree.load(), sky_census::differ.load(), sky_census::row_unusable.load(), sky_census::occupant_lines.load(), sky_census::remap_active.load(), sky_census::scale_moved.load(), sky_census::ctor_agree.load(), sky_census::ctor_differ.load(), sky_census::hi_link_agree.load(), sky_census::hi_link_differ.load(), sky_census::hi_link_verified.load(), sky_census::hi_named_by_key.load(), sky_census::hi_nokey.load());   // S103 SKY
      REXLOG_INFO("[scene][live] frame {}: guest page checks calls {} page lookups {} of which cache hits {} (per-frame cache) refused {}; unreadable: track act reads {} node-count delegate reads {}", guest_frame, g_page_check_calls.load(std::memory_order_relaxed), g_page_check_pages.load(std::memory_order_relaxed), g_page_check_hits.load(std::memory_order_relaxed), g_page_check_refused.load(std::memory_order_relaxed), scene_inputs::g_track_act_unreadable.load(std::memory_order_relaxed), scene_inputs::g_delegate_unreadable.load(std::memory_order_relaxed));   // S103 PC1 / S108 GAVF: the phone's cost of the page-checked readers
      REXLOG_INFO("[scene][live] frame {}: {}", guest_frame, rdr_scene::BodyScaleTable::CensusText(rdr_scene::BodyScaleTable::Get().Census()));   // S103 W2: latch vs the stream-learned scale (census only)
      REXLOG_INFO("{}", rdr_scene::SpringHairCensusText());   // S103 W2B: [scene][wrestlers] hair rows N, bodies with extras drawn N / refused N (tags / count / unreadable)
    }
  }
  uint32_t world_ptr = 0, inv_ptr = 0;
  if (!ReadWord(self + 192, world_ptr) || !ReadWord(self + 224, inv_ptr) || !world_ptr || !inv_ptr ||
      !ReadMatrices(world_ptr, c.count, t_world) || !ReadMatrices(inv_ptr, c.count, t_inv)) {
    w.NoteUnreadable();
    return;
  }
  int32_t slot = -1;
  uint32_t slot_ch = 0;   // S103 SKY: the matched character
  for (int s = 0; s < 9 && slot < 0; ++s) {
    uint32_t ch = 0, pos = 0;
    if (ReadWord(kSlotTable + 4 * s, ch) && ch && ReadWord(ch + 176, pos) && pos == world_ptr + 2 * 64 + 48) { slot = s; slot_ch = ch; }
  }
  w.SubmitWorld(self, slot, c.count, t_world.data(), t_inv.data(), guest_frame);
  const auto rd = [](uint32_t a, uint32_t& v) { return ReadWord(a, v); };
  if (slot >= 0 && scene_frame::CurrentMode() != scene_frame::Mode::Off) {   // S103 W3: the body scale of this character slot (the source of the scale, body_scale.h)
    // S110 MGRFINGERS: [[0x83924734] + 864 + 80 x DescIndex(char)] + 12 (slot_key.h DescScale), the SAME record the model key comes from. NOT the slot-table position: under the game's slot
    // remap that position holds another character's record (docs\research\s110\MGRFINGERS.md). 0x83924734 = the match request context word (sub_827E4538 0x827E45A4); sub_8271ED10 0x8271ED48
    // `lfs f0,12(r9)` reads the float at +12 of the 80-byte record (R1: rec80); sub_823BB0C8 writes it.
    int si = -1;
    float v = 0.f;
    const bool ok = slotkey::DescScale(rd, slot_ch, v, &si);
    rdr_scene::BodyScaleTable::Get().SetLatch(self, slot, v, ok);
    if (ok && si != slot) sky_census::scale_moved.fetch_add(1, std::memory_order_relaxed);
  } else if (slot < 0) {
    rdr_scene::BodyScaleTable::Get().ClearLatch(self);   // S103 W3: not a slot wrestler now (the address may have been reused): no stale latch value
  }
  {  // S103 SKY (was S103 B): the slot's model key from the game's own character descriptor (slot_key.h, TMR.md section 3): [[0x83924734] + 864 + 80 x [char+1220]] + 0, slots 0..8.
     // The participant row (entrant_slots A = [0x8376AAF0] + 148 + 56 x row, +16) is a census only: it is wrong in a Royal Rumble (rows rotate) and after a slot swap (sub_8251F290).
     // Only while the scene switch is on (lane A review 1): off = no reads, key cleared.
    const bool scene_on = scene_frame::CurrentMode() != scene_frame::Mode::Off;
    uint32_t key = 0;
    if (scene_on && slot >= 0) {
      const bool desc_rule = REXCVAR_GET(wwe2k14_rdr_slot_key_desc);
      key = slotkey::SlotKeyOf(rd, slot, slot_ch, desc_rule);
      {   // census: remap active, and the character constructor's own key [ch+44]
        bool remap = false;
        slotkey::DescIndex(rd, slot_ch, &remap);
        if (remap) sky_census::remap_active.fetch_add(1, std::memory_order_relaxed);
        if (key) { const uint32_t c44 = slotkey::CtorKey(rd, slot_ch); if (c44 == key) sky_census::ctor_agree.fetch_add(1, std::memory_order_relaxed); else sky_census::ctor_differ.fetch_add(1, std::memory_order_relaxed); }
      }
      if (slot >= 6) {   // census: slots 6..8 now get a key too; how it sits beside the declared link (read before SubmitSlotKey, which drops a link of another character)
        const auto lk = w.BodyKeyFor(self);
        if (!key) sky_census::hi_nokey.fetch_add(1, std::memory_order_relaxed);
        else if (!lk.known) sky_census::hi_named_by_key.fetch_add(1, std::memory_order_relaxed);
        else {
          (lk.key == key ? sky_census::hi_link_agree : sky_census::hi_link_differ).fetch_add(1, std::memory_order_relaxed);
          if (lk.verified) sky_census::hi_link_verified.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (slot < 6) {   // census: the desc key against the old row rule
        bool usable = false;
        const uint32_t row = slotkey::RowKey(rd, slot, usable);
        const uint32_t desc = desc_rule ? key : slotkey::DescKey(rd, slot_ch);
        if (!usable) sky_census::row_unusable.fetch_add(1, std::memory_order_relaxed);
        else if (row == desc) sky_census::agree.fetch_add(1, std::memory_order_relaxed);
        else sky_census::differ.fetch_add(1, std::memory_order_relaxed);
        if (sky_census::occupant_char[slot].load(std::memory_order_relaxed) != slot_ch || sky_census::occupant_key[slot].load(std::memory_order_relaxed) != desc) {
          sky_census::occupant_char[slot].store(slot_ch, std::memory_order_relaxed);
          sky_census::occupant_key[slot].store(desc, std::memory_order_relaxed);
          if (sky_census::occupant_lines.fetch_add(1, std::memory_order_relaxed) < 64) {
            uint32_t s1220 = 0xFFFFFFFFu;
            ReadWord(slot_ch + 1220, s1220);
            REXLOG_INFO("[scene][live] slot {} occupant char 0x{:08X} skel 0x{:08X} +1220 {} desc key {} row key {}{} frame {}", slot, slot_ch, self, int32_t(s1220), desc, row, usable ? "" : " (row rule unusable)", guest_frame);
          }
        }
      } else if (slot_ch && sky_census::occupant_char[slot].load(std::memory_order_relaxed) != slot_ch) {
        sky_census::occupant_char[slot].store(slot_ch, std::memory_order_relaxed);
        if (sky_census::occupant_lines.fetch_add(1, std::memory_order_relaxed) < 64)
          REXLOG_INFO("[scene][live] slot {} occupant char 0x{:08X} skel 0x{:08X} desc key {} frame {}", slot, slot_ch, self, key, guest_frame);
      }
    }
    // S103 ARM2 F2 / HPF: the slot's own descriptor by the game's index rule (D+7, else the slot index; slotkey::SlotDescriptor: a character slot never writes desc+4, HPR.md), asked only while the build-step latch
    // holds this key. A descriptor whose load does not build (mode 0 / 2) forgets the capture; the colours are used only for the descriptor that made them (attire_build.h SlotOwner / TagForSlot).
    uint32_t slot_desc = 0;
    if (key && attire_build::Latch::Instance().HasChosen(key)) {
      slotkey::SlotDescInfo si;
      attire_build::SlotWhy why = attire_build::SlotWhy::kNoDesc;
      slot_desc = attire_build::SlotDescForChar(rd, slot_ch, key, attire_build::Latch::Instance(), why, &si);
      // HPF log: one line per (slot, key, verdict) change, first 64: settles HPR's [L] (reader+96 == element D+7) and the cross-check against the old desc+4 rule.
      static std::atomic<uint64_t> last[9];
      static std::atomic<uint32_t> lines{0};
      const uint64_t sig = (uint64_t(key) << 8) | (uint64_t(why) << 4) | 1u;
      if (slot >= 0 && slot < 9 && last[slot].load(std::memory_order_relaxed) != sig && lines.fetch_add(1, std::memory_order_relaxed) < 64) {
        last[slot].store(sig, std::memory_order_relaxed);
        const uint32_t d = slotkey::SlotDescriptor(rd, slot_ch);
        uint32_t c92 = 0, m44 = 0, ob = 0, ok = 0;
        if (d) ReadWord(d + 92, c92), ReadWord(d + 44, m44);
        const bool ob_found = scene_inputs::DescriptorOfBody([](uint32_t a, void* dst, size_t n) { return ReadGuest(a, dst, n); }, self, ob, ok);
        char obs[48] = "none";
        if (ob_found) std::snprintf(obs, sizeof obs, "0x%08X%s", ob, ob == d ? " (agrees)" : " (DIFFERS)");
        const char* const names[] = {"tag", "no_desc", "key_mismatch", "mode_base"};
        REXLOG_INFO("[attire_build] slot {} key {}: D index {}, D+7 {}, desc 0x{:08X} (+92 {}, +44 {}), DescriptorOfBody {}, verdict {}", slot, key, si.s, si.b7, d, c92, m44,
                    obs, names[int(why)]);
      }
    }
    // S110 SEATREL: the slot's descriptor for EVERY keyed slot (not only under the attire latch above): the file note is per descriptor (model_file_note.h)
    w.SubmitSlotKey(self, key, slot_desc, key ? slotkey::SlotDescriptor(rd, slot_ch) : 0u);
  }
}

// sub_82BD3620 at 0x82BD362C (the existing wwe2k14_skel_shift hook; r3 = skeleton, r4 = pointer to the 3-float delta): the game adds the delta to the
// translation of every node of +192 and +200 right after (listing: lfs from r4+0/4/8, fadds into the +48/+56/+64... stores of both arrays).
inline void OnSkelShift(uint32_t self, uint32_t delta_ptr) {
  if (!rdr_scene::live::WrestlersRegistered() || !rdr_scene::live::Enabled()) return;
  uint32_t d[3];
  if (!ReadGuest(delta_ptr, d, sizeof d)) return;
  float f[3];
  for (int i = 0; i < 3; ++i) { d[i] = __builtin_bswap32(d[i]); std::memcpy(&f[i], &d[i], 4); }
  rdr_scene::live::Wrestlers().ApplyDelta(self, f[0], f[1], f[2]);
}

// lane 209: the one-time line for a wanted own-skeleton piece that reached the palette hook (sub_82C04DF0): which r5, which [model+200], how many nodes, did the update hook ever see the address.
inline const bool g_own_skel_log_installed = (rdr_scene::live::Wrestlers().SetOwnSkelLog([](uint32_t model, uint32_t r5, uint32_t field, uint32_t count, bool hook_saw) {
  REXLOG_INFO("[scene][live] own-skeleton entry 0x{:08X}: r5 0x{:08X} [model+200] 0x{:08X}{} count {} update hook saw address {}", model, r5, field, field == r5 ? "" : " (differs)", count, hook_saw ? "yes" : "no");
}), true);
// S103 NRB: the one-time line per SUBM piece instance the latch links (piece_link.h): wanted 0 = the scene core never recorded that model as its own skeleton (a borrower, or instance+0 is not the draw object).
inline const bool g_piece_link_log_installed = (rdr_scene::live::Wrestlers().SetPieceLinkLog([](uint32_t model, int32_t slot, uint32_t id, uint32_t owner, uint32_t wanted, scene_inputs::PieceWhy why, scene_inputs::PieceBuild build, uint32_t kind) {
  const char* const builds[] = {"unknown", "deferred", "B1", "B2"};   // S103 PZF2 B-3: how sub_8259E720 built the model (the first B2 seen in a run is the evidence the refusal waits for)
  REXLOG_INFO("[scene][live] piece link 0x{:08X} = slot {} piece {} (owner 0x{:08X}): recorded own skeleton {} (wanted {}), reader proof {} {} (0 = drawn by its owner's body; PieceWhy in slot_piece.h), build {}, model kind {}", model, slot, id, owner, wanted ? "yes" : "no", wanted, int(why), scene_inputs::kPieceWhyNames[size_t(why)], builds[size_t(build) < 4 ? size_t(build) : 0], kind == 0xFFFFFFFFu ? -1 : int(kind));
}), true);
// S103 DTP: one line per change of a piece's owner-copy flag (SUBM instance +24): copy 0 = the game took the piece (the People's Elbow ELBW object holds the pad: PEP.md), 1 = it joined its owner again
inline const bool g_piece_detach_log_installed = (rdr_scene::live::Wrestlers().SetPieceDetachLog([](uint32_t model, int32_t slot, uint32_t id, uint32_t owner, uint32_t copy, uint64_t frame) {
  REXLOG_INFO("[scene][live] piece detach 0x{:08X} slot {} piece {} (owner 0x{:08X}): owner copy flag now {} ({}) frame {}", model, slot, id, owner, copy, copy ? "joined its owner again" : "detached: the game poses it from its own palette", frame);
}), true);
// S103 PZ F5: the one-time line (first 32) when a Known record that carried a declared model link ages out: the link itself stays with the body (body_links_). Settles PZ's open [U] (why two slot-6 characters read the same [char+48]).
inline const bool g_known_erase_log_installed = (rdr_scene::live::Wrestlers().SetKnownEraseLog([](uint32_t body, uint32_t key, uint32_t age, uint64_t since_link) {
  REXLOG_INFO("[scene][live] known erased, link kept: body 0x{:08X} model_key {} (character {} kind {}) age {} collects, {} collects since the link", body, key, key / 100, key % 10, age, since_link);
}), true);

// S103 AUSF (AUS2 log lines 1-3): the stream-free build names a body it leaves out of the record: kind 0 stale drop, 1 latch read refused (once per run of refusals), 2 quiet hide; at most 64 lines each a run
inline const bool g_wrestler_event_log_installed = (rdr_scene::live::Wrestlers().SetWrestlerEventLog([](int kind, uint64_t frame, uint32_t skel, int32_t slot, uint64_t a, uint64_t b, uint64_t c) {
  static std::atomic<int> n[3];
  if (kind < 0 || kind > 2 || n[kind].fetch_add(1, std::memory_order_relaxed) >= 64) return;
  if (kind == 0) REXLOG_INFO("[scene][wrestlers] stale drop frame {} skel 0x{:08X} slot {} age {} last tag {} (hold allowed up to {} records; the body left the table or is older)", frame, skel, slot, a, b, c);
  else if (kind == 1) REXLOG_INFO("[scene][wrestlers] latch read refused frame {} skel 0x{:08X} slot {} (mismatch, no walk evidence)", frame, skel, slot);
  else REXLOG_INFO("[scene][wrestlers] quiet hide frame {} skel 0x{:08X} slot {} upd {} draw {}", frame, skel, slot, a, b);
}), true);
}  // namespace wwe2k14::wrestlers
