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
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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

#include "wrestler_contrib.h"

namespace wwe2k14::wrestlers {

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

// Big-endian guest bytes -> host.
inline bool ReadGuest(uint32_t guest, void* dst, size_t n) {
  if (guest < 0x10000 || uint64_t(guest) + n > 0x100000000ull) return false;
  if (uint64_t(guest) + n > 0x7F000000ull && guest < 0x80000000u) return false;  // MMIO heap
  auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest);
  return host && SafeCopy(dst, host, n);
}
inline bool ReadWord(uint32_t guest, uint32_t& v) {
  uint32_t raw = 0;
  if (!ReadGuest(guest, &raw, 4)) return false;
  v = __builtin_bswap32(raw);
  return true;
}
inline bool ReadMatrices(uint32_t guest, uint32_t count, std::vector<rdr_scene::Mat4>& out) {
  out.resize(count);
  if (!ReadGuest(guest, out.data(), size_t(count) * 64)) return false;
  auto* w = reinterpret_cast<uint32_t*>(out.data());
  for (size_t i = 0; i < size_t(count) * 16; ++i) w[i] = __builtin_bswap32(w[i]);  // floats, byte-swapped as words
  return true;
}

constexpr uint32_t kSlotTable = 0x836F38D0;  // 9 character pointers (match_state.md, high)
constexpr uint32_t kMaxNodes = 1024;

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
    if (guest_frame >= due && next_log.compare_exchange_strong(due, guest_frame + 600)) {
      REXLOG_INFO("[scene][live] wrestlers frame {}: LinkBodies {} LinkKind {} {} {} {} {} {} {} {} {} {} LinkEmptyKey {} LinkNoBody {} LinkUnreadable {} LinkCountRefused {} LinkedSeenAsSkeleton {} KnownCount {} SkippedSmall {} CapRefused {} ListedFull {} Wanted {} WantedFull {} EntryPiecesByDescriptor {} EntryUnknownModel {} EntryPieces {} EntryPtrMismatch {} EntryUnreadable {} OwnSkelConsumer {} OwnSkelPtrMismatch {}",
                  guest_frame, w.LinkBodies(), w.LinkKind(0), w.LinkKind(1), w.LinkKind(2), w.LinkKind(3), w.LinkKind(4), w.LinkKind(5), w.LinkKind(6), w.LinkKind(7), w.LinkKind(8), w.LinkKind(9),
                  w.LinkEmptyKey(), w.LinkNoBody(), w.LinkUnreadable(), w.LinkCountRefused(), w.LinkedSeenAsSkeleton(), w.KnownCount(), w.SkippedSmall(), w.CapRefused(), w.ListedFull(), w.Wanted(), w.WantedFull(),
                  w.EntryPiecesByDescriptor(), w.EntryUnknownModel(), w.EntryPieces(), w.EntryPtrMismatch(), w.EntryUnreadable(), w.OwnSkelConsumer(), w.OwnSkelPtrMismatch());
    }
  }
  uint32_t world_ptr = 0, inv_ptr = 0;
  if (!ReadWord(self + 192, world_ptr) || !ReadWord(self + 224, inv_ptr) || !world_ptr || !inv_ptr ||
      !ReadMatrices(world_ptr, c.count, t_world) || !ReadMatrices(inv_ptr, c.count, t_inv)) {
    w.NoteUnreadable();
    return;
  }
  int32_t slot = -1;
  for (int s = 0; s < 9 && slot < 0; ++s) {
    uint32_t ch = 0, pos = 0;
    if (ReadWord(kSlotTable + 4 * s, ch) && ch && ReadWord(ch + 176, pos) && pos == world_ptr + 2 * 64 + 48) slot = s;
  }
  w.SubmitWorld(self, slot, c.count, t_world.data(), t_inv.data(), guest_frame);
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

}  // namespace wwe2k14::wrestlers
