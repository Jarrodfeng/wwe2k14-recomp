// S105 A5c: which guest texture objects the game wrote since the guest-model resolver last read them (docs\research\s105\A5c.md, "What a build should do" item 4). Copyright (c) the wwe2k14-recomp
// authors. BSD-3-Clause. Pure: std only (the unit includes it).
//
// The game says exactly when a texture changed: the unlock flush 0x82CA6BE0 (r3 = the texture object = the resolver's `header` word) after a copy (the CAW composer's 8888 copy 0x822F8388, the match
// composer's DXT pack 0x822F85D8). The flush hook calls NoteFlush(r3) on a game thread; the resolver calls Watch(header) after it reads a picture and Dirty / Clear around its next read.
// Only WATCHED headers are recorded (the flush runs for every texture of the game: an unwatched one costs one table probe and nothing else). A fixed table, lock-free, no allocation: a full probe
// window means the header is not watched and the resolver keeps its old fallback (the 61-frame guard), counted by the caller.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene::live::guest_tex_dirty {

inline constexpr uint32_t kSlots = 1024, kProbe = 8;   // kSlots a power of two
struct Slot {
  std::atomic<uint32_t> addr{0};
  std::atomic<uint32_t> gen{0};   // review F6 / F7: a per-slot flush generation (bumped by every flush); a reader keeps the generation it read and is dirty when it differs
};
struct Table {
  Slot s[kSlots];
  std::atomic<uint64_t> seq{0};   // moves on every flush of a watched header: a model whose resolver last ran at an older seq runs it again at once
  std::atomic<uint64_t> flushes_watched{0}, watch_full{0};
};
inline Table& TheTable() { static Table t; return t; }

inline uint32_t Home(uint32_t a) { return (((a >> 4) * 2654435761u) >> 8) & (kSlots - 1); }
inline Slot* Find(Table& t, uint32_t a) {
  if (!a) return nullptr;
  const uint32_t h = Home(a);
  for (uint32_t i = 0; i < kProbe; ++i) {
    Slot& s = t.s[(h + i) & (kSlots - 1)];
    if (s.addr.load(std::memory_order_acquire) == a) return &s;
  }
  return nullptr;
}

// The resolver read the picture at `header`: from now on a flush of it is recorded. False = the table has no room (the caller keeps its fallback).
inline bool Watch(uint32_t header, Table& t = TheTable()) {
  if (!header) return false;
  if (Find(t, header)) return true;
  const uint32_t h = Home(header);
  for (uint32_t i = 0; i < kProbe; ++i) {
    Slot& s = t.s[(h + i) & (kSlots - 1)];
    uint32_t e = 0;
    if (s.addr.load(std::memory_order_acquire) == 0 && s.addr.compare_exchange_strong(e, header, std::memory_order_acq_rel)) return true;
    if (e == header) return true;   // another thread watched it first
  }
  t.watch_full.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// The flush hook (a game thread, both builds): `header` = r3 of sub_82CA6BE0. Cheap for an unwatched object.
inline void NoteFlush(uint32_t header, Table& t = TheTable()) {
  Slot* s = Find(t, header);
  if (!s) return;
  s->gen.fetch_add(1, std::memory_order_acq_rel);
  t.flushes_watched.fetch_add(1, std::memory_order_relaxed);
  t.seq.fetch_add(1, std::memory_order_acq_rel);
}
// The flush generation of a watched header (0 = not watched). A reader takes it JUST BEFORE its read and stores it with the result: a flush that lands during the read leaves the stored value behind, so the next call
// re-reads (review F6: any number of readers of one header each see it; F7: a failed read does not store it, so it is retried).
inline uint32_t Gen(uint32_t header, Table& t = TheTable()) {
  const Slot* s = Find(t, header);
  return s ? s->gen.load(std::memory_order_acquire) : 0;
}
// Review F5: free the slot of a header no reader holds any more (the table is insert-only otherwise). Find has no early exit on an empty slot, so a freed slot breaks no probe chain. A racing NoteFlush at worst bumps the slot's
// next owner once (one extra re-read).
inline void Unwatch(uint32_t header, Table& t = TheTable()) {
  if (Slot* s = Find(t, header)) s->gen.store(0, std::memory_order_relaxed), s->addr.store(0, std::memory_order_release);
}
inline uint64_t Seq(Table& t = TheTable()) { return t.seq.load(std::memory_order_acquire); }

}  // namespace rdr_scene::live::guest_tex_dirty
