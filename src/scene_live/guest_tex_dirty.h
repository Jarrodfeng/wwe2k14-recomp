// S105 A5c -> S111 DIRTYOWN: which guest texture objects the game wrote since each of our readers last read them (docs\research\s105\A5c.md item 4; docs\research\s111\DIRTYOWN.md). Copyright (c) the
// wwe2k14-recomp authors. BSD-3-Clause. Pure: std only (the units include it).
//
// The game says exactly when a texture changed: the unlock flush 0x82CA6BE0 (r3 = the texture object = the readers' `header` word) after a CPU write (the CAW composer's 8888 copy 0x822F8388, the match
// composer's DXT pack 0x822F85D8, the Create a Championship plate copy 0x8332D9B0). The flush hook (d3d_textures.h REX_HOOK_RAW(sub_82CA6BE0)) calls NoteFlush(r3) on a game thread; a reader calls
// Watch(header, its Owner) before it reads a picture, keeps Gen(header) taken just before the read, and is due again when Gen moves. Nothing is consumed: each reader compares its own stored copy.
// S111 DIRTYOWN: each reader's interest is its own (an Owner bit per slot). Unwatch(header, owner) drops that reader's bit only and frees the slot when no bit is left, so one reader's free never silences
// another reader's flushes. The table grows (a chain of segments, each twice the last) instead of refusing: Watch answers false only for header 0, owner 0, or past max_slots (counted: watch_refused).
// A watched slot's generation is never 0 (Gen of an unwatched header is 0), so a reader whose header was freed sees a change, never a match.
// NoteFlush / Gen / Seq are lock-free (one probe walk, no allocation); Watch / Unwatch / Owners take the table's mutex and call nothing that takes another lock.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace rdr_scene::live::guest_tex_dirty {

// Test only (tools\scene\guest_tex_dirty_unit.cpp): 1 = owner_ignored (Unwatch frees the slot whoever else holds it: the old one-slot-no-owner table); 2 = no_growth (a full first segment refuses: the
// old fixed table); 3 = refusal_silent (a refused Watch answers true: the reader believes it is watched).
inline int g_dirty_plant = 0;

// The readers (one of each in the game: beltplate::TheStore, carpaint::TheStore, the guest-model contributor's TexResolver). Unwatch(header, owner) = "no entry of THIS reader reads the header now".
enum Owner : uint32_t { kOwnerBelt = 1u, kOwnerPaint = 2u, kOwnerModelTex = 4u };

inline constexpr uint32_t kFirstSlots = 1024, kMaxSlots = 1u << 24;   // segment sizes are powers of two (Home keeps 24 bits)
struct Slot {
  std::atomic<uint32_t> addr{0};
  std::atomic<uint32_t> gen{0};   // review F6 / F7: bumped by every flush; never 0 while watched (Watch starts it at the slot's last value + 1)
  uint32_t owners = 0;            // the Owner bits (under Table::mu)
};
struct Segment {
  explicit Segment(uint32_t n) : size(n), s(new Slot[n]) {}
  ~Segment() { delete next.load(std::memory_order_relaxed); }
  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;
  const uint32_t size;
  std::unique_ptr<Slot[]> s;
  std::atomic<uint32_t> reach{0};   // the longest probe a Watch in this segment used: Find looks no further (a freed slot never shortens it, so no probe chain breaks)
  uint32_t live = 0;                // watched slots (under Table::mu)
  std::atomic<Segment*> next{nullptr};
};
struct Table {
  Segment first{kFirstSlots};
  uint32_t max_slots = kMaxSlots;   // the largest segment Watch may add (a unit sets kFirstSlots to fill the table)
  std::mutex mu;                    // Watch / Unwatch / Owners only
  std::atomic<uint64_t> seq{0};     // moves on every flush of a watched header: a model whose resolver last ran at an older seq runs it again at once
  std::atomic<uint64_t> flushes_watched{0}, watch_refused{0};
  std::atomic<uint32_t> segments{1};
};
// DIRTYOWN review: the process table is never destroyed. Its segments are heap blocks freed by ~Segment, and the flush hook (a game thread) and the readers may still run while static destructors do
// at exit; the old fixed array had nothing to free. (A unit's own Table on the stack is destroyed as usual.)
inline Table& TheTable() { static Table& t = *new Table; return t; }

inline uint32_t Home(uint32_t a, uint32_t size) { return (((a >> 4) * 2654435761u) >> 8) & (size - 1); }
inline Slot* Find(Table& t, uint32_t a, Segment** in = nullptr) {
  if (!a) return nullptr;
  for (Segment* g = &t.first; g; g = g->next.load(std::memory_order_acquire)) {
    const uint32_t h = Home(a, g->size), r = g->reach.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < r; ++i) {
      Slot& s = g->s[(h + i) & (g->size - 1)];
      if (s.addr.load(std::memory_order_acquire) == a) {
        if (in) *in = g;
        return &s;
      }
    }
  }
  return nullptr;
}

// Reader `owner` is about to read the picture at `header`: from now on a flush of it is recorded. Idempotent (a reader may call it at every read). False = refused and counted: the caller does not
// trust Gen for this header (beltplate re-reads the key every call; carpaint / the model resolver keep their fallbacks and count it).
inline bool Watch(uint32_t header, uint32_t owner, Table& t = TheTable()) {
  if (!header || !owner) return t.watch_refused.fetch_add(1, std::memory_order_relaxed), false;
  std::lock_guard<std::mutex> lk(t.mu);
  if (Slot* s = Find(t, header)) return s->owners |= owner, true;
  for (Segment* g = &t.first; g;) {
    if (g->live * 4u < g->size * 3u) {   // under 3/4 full: a free slot exists, and probes stay short
      const uint32_t h = Home(header, g->size);
      for (uint32_t i = 0; i < g->size; ++i) {
        Slot& s = g->s[(h + i) & (g->size - 1)];
        if (s.addr.load(std::memory_order_relaxed)) continue;
        if (i + 1 > g->reach.load(std::memory_order_relaxed)) g->reach.store(i + 1, std::memory_order_release);   // before the address: a Find that sees the address looks this far
        const uint32_t gen = s.gen.load(std::memory_order_relaxed) + 1;
        s.gen.store(gen ? gen : 1u, std::memory_order_relaxed);
        s.owners = owner;
        s.addr.store(header, std::memory_order_release);
        ++g->live;
        return true;
      }
    }
    Segment* n = g->next.load(std::memory_order_relaxed);
    if (!n && g_dirty_plant != 2 && g->size * 2u <= t.max_slots) {
      n = new Segment(g->size * 2u);
      g->next.store(n, std::memory_order_release);
      t.segments.fetch_add(1, std::memory_order_relaxed);
    }
    g = n;
  }
  t.watch_refused.fetch_add(1, std::memory_order_relaxed);
  return g_dirty_plant == 3;
}

// The flush hook (a game thread, both builds): `header` = r3 of sub_82CA6BE0. Cheap for an unwatched object (one probe walk).
inline void NoteFlush(uint32_t header, Table& t = TheTable()) {
  Slot* s = Find(t, header);
  if (!s) return;
  if (s->gen.fetch_add(1, std::memory_order_acq_rel) == 0xFFFFFFFFu) s->gen.fetch_add(1, std::memory_order_acq_rel);   // (0 = unwatched: the wrap skips it)
  t.flushes_watched.fetch_add(1, std::memory_order_relaxed);
  t.seq.fetch_add(1, std::memory_order_acq_rel);
}
// The flush generation of a watched header (0 = not watched). A reader takes it JUST BEFORE its read and stores it with the result: a flush that lands during the read leaves the stored value behind, so the next call
// re-reads (review F6: any number of readers of one header each see it; F7: a failed read does not store it, so it is retried).
inline uint32_t Gen(uint32_t header, Table& t = TheTable()) {
  const Slot* s = Find(t, header);
  return s ? s->gen.load(std::memory_order_acquire) : 0;
}
// Reader `owner` holds `header` nowhere any more: its bit goes, and the slot is freed when no reader is left. A reader that does not hold it changes nothing (a stale holder of a reused address frees
// nobody's watch). A racing NoteFlush at worst bumps the slot's next owner once (one extra re-read).
inline void Unwatch(uint32_t header, uint32_t owner, Table& t = TheTable()) {
  std::lock_guard<std::mutex> lk(t.mu);
  Segment* g = nullptr;
  Slot* s = Find(t, header, &g);
  if (!s || (g_dirty_plant != 1 && !(s->owners & owner))) return;
  s->owners = g_dirty_plant == 1 ? 0u : (s->owners & ~owner);
  if (s->owners) return;
  s->addr.store(0, std::memory_order_release);
  --g->live;
}
// The readers holding `header` (0 = none): the units' view of the owner bits.
inline uint32_t Owners(uint32_t header, Table& t = TheTable()) {
  std::lock_guard<std::mutex> lk(t.mu);
  const Slot* s = Find(t, header);
  return s ? s->owners : 0u;
}
inline uint64_t Seq(Table& t = TheTable()) { return t.seq.load(std::memory_order_acquire); }

}  // namespace rdr_scene::live::guest_tex_dirty
