#pragma once
// S101 lane 120: the Royal Rumble and move events of the event bus (src/event_bus.h calls these from MatchTick).
// Written against a memory interface (m.r8 / m.r16 / m.r32 of a guest address) and an emit callback (fields string), with no SDK
// includes, so tools/bus/rumble_test.cpp compiles this same code against fake guest words. Reads only: no guest writes, no RNG.
// Sources: docs/systems/rumble.md section 5 (lane 116), docs/systems/moves.md section 3 (lane 113). Review fixes: S101 lane 120 round 2.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wwe2k14::events::rumble {

constexpr uint32_t kG = 0x8376AAF4, kElimCount = 0x836F3924, kLastElimSlot = 0x837632B8, kEnteredCount = 0x835E1928,
                   kSpawnSlot = 0x837632C4, kEligFlags = kG + 14648, kElimCredit = kG + 14688;
constexpr uint32_t kSlotsTable = 0x836F38D0, kMatchDescWord = 0x836F2D88;
constexpr int kEnteredIdCapFrames = 600;
constexpr int64_t kMaxStep = 40;  // a counter that moves more than this in one frame is a resync, not eliminations

struct PendingId {
  int64_t n;
  uint32_t slot;
  uint64_t since;
};

struct State {
  bool started = false;
  int64_t elim = -1, entered = -1, entrants = 0;
  std::array<uint8_t, 40> flags{}, credit{};
  std::vector<PendingId> pending;
  void Reset() { *this = State(); }
};

template <class M>
bool Readable(M&, uint32_t a) { return a >= 0x40000000u && a < 0xF0000000u; }

template <class M>
uint32_t Char(M& m, uint32_t slot) {
  const uint32_t c = m.r32(kSlotsTable + 4 * slot);
  if (!Readable(m, c)) return 0;
  return m.r32(c + 1220) == slot ? c : 0;
}

// the character id of participant index e through the match description (inferred, as event_bus.h's CharacterId)
template <class M>
int64_t DescId(M& m, int64_t e) {
  const uint32_t d = m.r32(kMatchDescWord);
  if (!Readable(m, d) || e < 0 || e > 42) return -1;
  return int64_t(m.r32(d + 724 + 3580 * uint32_t(e) + 56));
}

template <class M>
int64_t CharacterId(M& m, uint32_t c) { return DescId(m, int64_t(m.r32(c + 3756))); }

inline bool IsRumbleType(int t) { return t >= 20 && t <= 23; }

inline std::string S(int64_t v) { return std::to_string(v); }

// The Rumble branch of MatchTick. `emit(fields)` is called with the JSON fragment of a "match" line. `clock_running` = the match clock is
// running. The state is NOT reset when the clock reads 0 again (events_6541 shows the clock restarting inside a match); the caller resets it
// when the sequence leaves 9, and a counter that decreases starts a new match here. Returns true when the type is a Rumble (the caller then
// skips the per-slot elimination path).
template <class M, class E>
bool Tick(State& st, M& m, uint64_t frame, bool clock_running, E&& emit) {
  const int type = m.r8(kG);
  if (!IsRumbleType(type)) {
    if (st.started) st.Reset();
    return false;
  }
  if (st.started && int64_t(m.r32(kElimCount)) < st.elim) st.Reset();  // the counter dropped: a new match
  if (!st.started) {
    if (!clock_running) return true;
    // the bell: sample the counters once
    st.started = true;
    st.elim = int64_t(m.r32(kElimCount));
    const int64_t starters = m.r8(kG + 81);
    st.entrants = int64_t(m.r8(kG + 80)) + starters;
    st.entered = starters + int64_t(m.r32(kEnteredCount));
    for (uint32_t i = 0; i < 40; ++i) {
      st.flags[i] = m.r8(kEligFlags + i);
      st.credit[i] = m.r8(kElimCredit + i);
    }
    // The starters: the ring slots 0..5 whose character is active (state 0). Not re-emitted when the counters are already past their seed
    // (a restart in a running match).
    if (st.elim <= int64_t(m.r8(kG + 82)) && int64_t(m.r32(kEnteredCount)) == 0) {
      int64_t n = 0;
      for (uint32_t slot = 0; slot < 6; ++slot) {
        const uint32_t c = Char(m, slot);
        if (!c || m.r8(c + 1020) != 0) continue;
        emit("\"what\": \"entered\", \"n\": " + S(++n) + ", \"slot\": " + S(slot) + ", \"id\": " + S(CharacterId(m, c)) + ", \"starter\": true");
      }
    }
    return true;
  }
  // eliminated: the counter 0x836F3924
  const int64_t n = int64_t(m.r32(kElimCount));
  if (n > st.elim && n - st.elim > kMaxStep) {
    emit("\"what\": \"resync\", \"counter\": \"eliminated\", \"from\": " + S(st.elim) + ", \"to\": " + S(n));
    for (uint32_t i = 0; i < 40; ++i) {
      st.flags[i] = m.r8(kEligFlags + i);
      st.credit[i] = m.r8(kElimCredit + i);
    }
    st.elim = n;
  } else if (n > st.elim) {
    const int64_t p = st.elim, count = n - p;
    std::vector<int> victims, bys;
    for (uint32_t i = 0; i < 40; ++i) {
      const uint8_t f = m.r8(kEligFlags + i), c = m.r8(kElimCredit + i);
      if (st.flags[i] == 0 && f != 0) victims.push_back(int(i));
      if (c > st.credit[i])
        for (int k = 0; k < c - st.credit[i]; ++k) bys.push_back(int(i));
      st.flags[i] = f;
      st.credit[i] = c;
    }
    // The last line's slot comes from 0x837632B8 (exact for the last of a group only), and so does its victim: the participant index of
    // the character in that slot. The other victims of the group are unordered; one left over is assigned, more than one stay -1.
    const int64_t slot = int32_t(m.r32(kLastElimSlot));
    const uint32_t lc = slot >= 0 && slot < 9 ? Char(m, uint32_t(slot)) : 0;
    const int64_t last_e = lc ? int64_t(m.r32(lc + 3756)) : -1;
    int64_t last_v = -1;
    std::vector<int> rest = victims;
    if (last_e >= 0) {
      for (size_t i = 0; i < rest.size(); ++i)
        if (rest[i] == last_e) {
          last_v = last_e;
          rest.erase(rest.begin() + long(i));
          break;
        }
    }
    if (last_v < 0 && count == 1 && victims.size() == 1) {
      last_v = victims[0];
      rest.clear();
    } else if (last_v < 0 && count == 1 && last_e >= 0) {
      last_v = last_e;
    }
    for (int64_t k = p + 1; k <= n; ++k) {
      const bool last = k == n;
      int64_t v = -1;
      if (last) v = last_v;
      else if (count == 2 && rest.size() == 1) v = rest[0];
      const int64_t by = (count == 1 && !bys.empty()) ? bys[0] : -1;  // a group's credits cannot be paired to its victims
      emit("\"what\": \"eliminated\", \"n\": " + S(k) + ", \"slot\": " + S(last ? slot : -1) + ", \"id\": " + S(DescId(m, v)) +
           ", \"over_the_top\": true, \"by_idx\": " + S(by) + ", \"victim_idx\": " + S(v) +
           ", \"remaining\": " + S(st.entrants ? st.entrants - k : -1) + (count > 1 ? ", \"same_frame\": true" : ""));
    }
    st.elim = n;
  }
  // entered: G+81 + [0x835E1928]
  const int64_t e = int64_t(m.r8(kG + 81)) + int64_t(m.r32(kEnteredCount));
  if (e > st.entered && e - st.entered > kMaxStep) {
    emit("\"what\": \"resync\", \"counter\": \"entered\", \"from\": " + S(st.entered) + ", \"to\": " + S(e));
    st.entered = e;
  } else if (e > st.entered) {
    const int64_t slot = int32_t(m.r32(kSpawnSlot));
    for (int64_t k = st.entered + 1; k <= e; ++k) {
      const bool last = k == e;
      emit("\"what\": \"entered\", \"n\": " + S(k) + ", \"slot\": " + S(last ? slot : -1) + ", \"id\": -1" +
           (e - st.entered > 1 ? ", \"same_frame\": true" : ""));
      if (last && slot >= 0 && slot < 9) st.pending.push_back({k, uint32_t(slot), frame});
    }
    st.entered = e;
  } else if (e < st.entered) {
    emit("\"what\": \"resync\", \"counter\": \"entered\", \"from\": " + S(st.entered) + ", \"to\": " + S(e));
    st.entered = e;
  }
  // entered_id follow-ups: the character is built a few frames after the spawn
  for (size_t i = 0; i < st.pending.size();) {
    const PendingId pd = st.pending[i];
    const uint32_t c = Char(m, pd.slot);
    const bool built = c && m.r8(c + 1020) == 0;
    if (built || frame >= pd.since + kEnteredIdCapFrames) {
      emit("\"what\": \"entered_id\", \"n\": " + S(pd.n) + ", \"slot\": " + S(pd.slot) + ", \"id\": " + S(built ? CharacterId(m, c) : -1));
      st.pending.erase(st.pending.begin() + long(i));
    } else {
      ++i;
    }
  }
  return true;
}

// ---- the move event (lane 113): char+2486 = the waza slot, char+2488 = the move id, ring slots 0..5
struct MoveState {
  std::array<int64_t, 6> seen{-2, -2, -2, -2, -2, -2};
  std::array<uint32_t, 6> ptr{};
};

// [L] the slot-to-class naming is unconfirmed (sub_825FE118 may reverse it): 15-16 signature, 17-19 finisher, else normal.
inline const char* MoveClass(int waza) { return waza >= 15 && waza <= 16 ? "signature" : (waza >= 17 && waza <= 19 ? "finisher" : "normal"); }

// emit(fields) with the kind "move". all = also normal moves.
// LIMIT (documented, pinned by the tests): the game's logger sub_825A14A8 clears and rewrites the halfwords inside one call, so this poll never
// sees the clear: the SAME (waza, move) twice in a row on one character (a repeat after a reversal) is one line, not two. A real fix needs a
// hook on sub_825A14A8 (a [[midasm_hook]], which means regenerating the recompiled code: not offline-verifiable, left out).
// A character first seen (a new pointer) only stores its key: its halfwords may be stale from the recycled object.
template <class M, class E>
void MoveTick(MoveState& st, M& m, bool all, E&& emit) {
  for (uint32_t slot = 0; slot < 6; ++slot) {
    const uint32_t c = Char(m, slot);
    if (!c) {
      st.seen[slot] = -2;
      st.ptr[slot] = 0;
      continue;
    }
    const int64_t idx = m.r16(c + 2486), mv = m.r16(c + 2488);
    const bool cleared = idx == 0xFFFF || mv == 0;
    const int64_t key = cleared ? -2 : ((idx << 16) | mv);
    if (c != st.ptr[slot]) {  // first sight of this character: remember, never emit
      st.ptr[slot] = c;
      st.seen[slot] = key;
      continue;
    }
    if (cleared) {
      st.seen[slot] = -2;
      continue;
    }
    if (key == st.seen[slot]) continue;
    st.seen[slot] = key;
    const char* cls = MoveClass(int(idx));
    if (!all && cls[0] == 'n') continue;
    emit(std::string("\"slot\": ") + S(slot) + ", \"id\": " + S(CharacterId(m, c)) + ", \"waza\": " + S(idx) + ", \"move\": " + S(mv) +
         ", \"class\": \"" + cls + "\"");
  }
}

}  // namespace wwe2k14::events::rumble
