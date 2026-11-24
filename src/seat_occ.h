// S110 SEAT3 lane G-B (docs\research\s110\SEAT3.md section 1, "the occupant table"): WHO sits in each roster seat right now, as every reader of the game-side
// hooks (names, table 102, availability, DSBD, ENEX BIN / LSD, music) asks it. A seat's occupant is a resident (published once at boot, src\superstars.h Load)
// or a guest (published by a bind, src\seat_bind.h): his card's rows already re-targeted to THAT seat (seat_bind_core.h RetargetCard), names included.
// std only (+ seats_core.h): no game memory here. An occupant is immutable once published and never freed (a reader on another thread may still hold the
// pointer; one occupant is ~3 KB and a bind makes one), so a read is one acquire load of g_occ[seat].
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "seats_core.h"

namespace wwe2k14::seats {

struct Occupant {
  std::string key;
  int seat = 0, bank = 0;  // the seat he sits in and the frame's bank for it
  int music = -1;          // his `inst ... music M`
  bool guest = false;
  sc::SeatRows rows;       // re-targeted to `seat` (a resident's are re-targeted at load from the reference identity, seats_core.h LoadCharacter; a guest's at his bind)
};

inline std::atomic<const Occupant*> g_occ[kLastId + 1];  // [seat]; null = nobody (static storage: zero-initialised)
inline std::mutex g_occ_mx;
inline std::deque<std::unique_ptr<Occupant>> g_occ_keep;  // every occupant ever published (see the top note)

inline const Occupant* OccOf(int seat) { return (seat >= kFirstId && seat <= kLastId) ? g_occ[seat].load(std::memory_order_acquire) : nullptr; }

inline const Occupant* PublishOcc(std::unique_ptr<Occupant> o) {
  const Occupant* p = o.get();
  if (!p || p->seat < kFirstId || p->seat > kLastId) return nullptr;
  std::lock_guard lock(g_occ_mx);
  g_occ_keep.push_back(std::move(o));
  g_occ[p->seat].store(p, std::memory_order_release);
  return p;
}
inline void ClearOcc(int seat) {
  if (seat >= kFirstId && seat <= kLastId) g_occ[seat].store(nullptr, std::memory_order_release);
}
// The residents of the roster, published (boot: superstars::Load). Guests are published by seat_bind.h.
inline void PublishResidents(const Roster& r) {
  for (const Resident& x : r.res) {
    auto o = std::make_unique<Occupant>();
    o->key = x.key, o->seat = x.seat, o->bank = x.bank, o->music = x.card.inst.music, o->guest = false, o->rows = x.rows;
    PublishOcc(std::move(o));
  }
}
// Everyone seated now, ascending seat.
inline std::vector<const Occupant*> Occupants() {
  std::vector<const Occupant*> v;
  for (int s = kFirstId; s <= kLastId; ++s)
    if (const Occupant* o = g_occ[s].load(std::memory_order_acquire)) v.push_back(o);
  return v;
}

// The page the DOWNLOADED tab shows (0-based) of how many (0 = no library paging): the title text (seats.h OnTextFind) reads it.
inline std::atomic<int> g_page_now{0}, g_page_total{0};
inline std::string PageTitle() {
  return "DOWNLOADED SUPERSTARS  (LB) " + std::to_string(g_page_now.load() + 1) + "/" + std::to_string(g_page_total.load()) + " (RB)";
}

// ---- the readers' views (what seats_core.h answers from a Roster, answered from the occupants) ----
inline std::vector<std::pair<int, const uint8_t*>> Bin5RowsOcc() {
  std::vector<std::pair<int, const uint8_t*>> v;
  for (const Occupant* o : Occupants()) v.push_back({o->seat, o->rows.bin5.data()});
  return v;
}
inline std::vector<const uint8_t*> BinpRowsOcc(int k) {
  std::vector<const uint8_t*> v;
  for (const Occupant* o : Occupants())
    for (const auto& b : o->rows.binp)
      if (b.k == k) v.push_back(b.row.data());
  return v;
}
inline std::vector<int> LsdListsOcc() {
  std::set<int> s;
  for (const Occupant* o : Occupants())
    for (const auto& l : o->rows.lsd) s.insert(l.list);
  return {s.begin(), s.end()};
}
inline std::vector<std::array<uint8_t, sc::kLsdRow>> LsdRowsOcc(int list) {
  std::vector<std::array<uint8_t, sc::kLsdRow>> v;
  for (const Occupant* o : Occupants())
    for (const auto& l : o->rows.lsd)
      if (l.list == list) v.push_back(LsdRowToMemory(l.row.data()));
  return v;
}
// B9: the key a table-102 row is asked by is the occupant's bank; the row is his entrance wrestler's. -1 = not an occupant's bank (or he has no T102).
inline int64_t SrcBankOcc(uint32_t key16) {
  for (int s = kFirstId; s <= kLastId; ++s) {
    const Occupant* o = g_occ[s].load(std::memory_order_acquire);
    if (o && uint32_t(o->bank) == (key16 & 0xFFFF) && o->rows.has_t102) return int64_t(o->rows.t102_src_bank & 0xFFFF);
  }
  return -1;
}
// B5: the text id asked of the text manager -> the occupant's name (0 name, 1 short, 2 long). nullptr = not ours.
inline const std::string* NameForOcc(int64_t id) {
  for (int k = 0; k < 3; ++k) {
    const int64_t seat = id - int64_t(kTextBases[k]);
    if (seat < kFirstId || seat > kLastId) continue;
    if (const Occupant* o = OccOf(int(seat))) return &o->rows.names[k];
  }
  return nullptr;
}

}  // namespace wwe2k14::seats
