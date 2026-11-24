// S110 UNIAVAIL (docs\research\s110\UNIAVAIL.md): the PURE half of "the game's own card rebuild treats a handed-over seat as unavailable".
// std only (no rex / Windows / game headers), PC + Android; the game side is src\universe_rebuild.h, the unit is tools\seats\handover_unit.cpp
// (TestUniverseRebuild; planted twins: tools\seats\handover_selftest.py).
//
// The rule: the seats the Universe roster check answered gone are held, for ONE Universe day (the date word), as "row-inactive" for the card builder's
// async jobs only. The hold is process-wide (the check runs on the main thread, the job on the game's worker thread), keyed by seat AND day, and READ
// only inside a job's own frame (a thread-local set, filled by JobFrame and emptied when the job returns). The first job that starts on another day
// ends the hold for good. The hold is written into the side record of the save (`held <day> <seat> ...`, see slot_handover_core.h).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wwe2k14::universe_rebuild {

struct Held {
  uint32_t day = 0;       // the Universe date word of the check that answered the seats gone
  std::set<int> seats;    // seats 100..499
};

// A check that answered `gone` seats gone on `day`. No seats: no change. A hold of another day (or none) is replaced; the same day merges.
inline void NoteCheck(Held& h, uint32_t day, const std::set<int>& gone) {
  if (gone.empty()) return;
  if (h.seats.empty() || h.day != day) {
    h.day = day;
    h.seats = {};
  }
  h.seats.insert(gone.begin(), gone.end());
}

// The seats a builder job that starts on `day` sees as inactive. A job on another day ends the hold for good.
inline std::set<int> SeatsForJob(Held& h, uint32_t day) {
  if (h.day != day) h.seats.clear();
  return h.seats;
}

// What the process holds after a load: this save's own hold (its entry's), or none. Never what the last save left behind.
inline Held HeldAfterLoad(const Held& before, bool have_entry, const Held& entry_held) {
  (void)before;
  return have_entry ? entry_held : Held{};
}

// The job's own frame: set by JobFrame on the worker thread, read by the row-active test on that thread only.
inline thread_local const std::set<int>* t_job = nullptr;  // a POINTER: no per-thread construction or destructor on the threads that only ask
struct JobFrame {
  std::set<int> seats_;  // the frame owns the set; t_job points at it for the frame's life
  explicit JobFrame(std::set<int> seats) : seats_(std::move(seats)) { t_job = &seats_; }
  ~JobFrame() { t_job = nullptr; }
  JobFrame(const JobFrame&) = delete;
  JobFrame& operator=(const JobFrame&) = delete;
};

// What the row-active test answers inside a job's frame: inactive for a held seat.
inline bool RowInactiveInJob(uint32_t id) { return t_job != nullptr && t_job->count(int(id)) != 0; }

// ---- the record line:  held <day> <seat> <seat> ...   (day = decimal u32, seats 100..499)
inline std::string HeldLine(const Held& h) {
  std::string t = "held " + std::to_string(h.day);
  for (int s : h.seats) t += " " + std::to_string(s);
  return t;
}
// `words` = the words after the key. False (and `out` untouched) for no seat, a bad day, or a seat outside 100..499.
inline bool ParseHeld(const std::vector<std::string>& words, Held& out) {
  if (words.size() < 2) return false;
  auto num = [](const std::string& w, unsigned long long lo, unsigned long long hi, unsigned long long& v) {
    if (w.empty() || w.size() > 10) return false;
    v = 0;
    for (char c : w) {
      if (c < '0' || c > '9') return false;
      v = v * 10 + uint64_t(c - '0');
    }
    return v >= lo && v <= hi;
  };
  Held h;
  unsigned long long v = 0;
  if (!num(words[0], 0, 0xFFFFFFFFull, v)) return false;
  h.day = uint32_t(v);
  for (size_t i = 1; i < words.size(); ++i) {
    if (!num(words[i], 100, 499, v)) return false;
    h.seats.insert(int(v));
  }
  out = std::move(h);
  return true;
}

}  // namespace wwe2k14::universe_rebuild
