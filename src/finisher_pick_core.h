// wwe2k14 - S110 FINPICK (docs\research\s110\FINPICK.md): a seat's wish for the CPU's finisher pair, and the rule the hook applies to the pair
// the game offers. Pure: no SDK, no game (host unit tools\bus\finisher_pick_test.cpp, run by tools\bus\finisher_pick_selftest.py). The hook of
// sub_825FDE00 and the host state `finisher_fit_S` are src\finisher_pick.h; the verb `finisher SLOT hold|nohold|any` is the situation director's.
//
// What the game does (FINPICK.md sections 1-2, [C] from the listings): sub_825FDE00 (only caller: the move chooser sub_826056E0 +340) writes the
// finisher pair [r4+0] / [r4+4] = (17, 18) or (18, 17) (18 first when char+2648 != -1) and [r4+8] = 262; the chooser makes a candidate of every
// entry that is not 262 (sub_826056E0 +46C..+474), tests each against the situation (sub_82605418) and plays the first that fits (+6C8..+704).
// The wish: put 262 over the slot whose LIVE move id (sub_8253AD80(char, slot), the read sub_825FDE00 itself makes at +D4) does not fit; the
// game still decides WHEN (a hold is played only when it fits the situation). When neither slot fits, the pair stays as the game wrote it.
// "hold" is the game's own class test sub_82564D40: 58000 <= id < 58399 (+28 cmpwi 58000, +3C cmpwi 58399; sub_82564FD8 makes the same test on
// the playing act). That a hold finisher ends the match by submission (and a non-hold one by pinfall) is [L]: FINPICK.md 4, the first check run.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace wwe2k14::finisher_pick {

constexpr uint32_t kHoldFirst = 58000;        // sub_82564D40 +28: in when id >= 58000
constexpr uint32_t kHoldEnd = 58399;          // sub_82564D40 +3C: in when id < 58399 (the range is 58000..58398)
constexpr uint32_t kNone = 262;               // "no candidate": sub_825FDE00 +188 writes it at [r4+8]; the chooser skips an entry equal to it (+474)
constexpr uint32_t kSlotA = 17, kSlotB = 18;  // the finisher pair (sub_825FDE00 r27 / r7 at +78)
constexpr int kSeats = 9;                     // the character table's slots (char+1220)

enum Wish : int { kAny = 0, kHold = 1, kNoHold = 2 };  // the director's Cmd::value for `finisher SLOT any|hold|nohold`
inline const char* WishName(int w) { return w == kHold ? "hold" : (w == kNoHold ? "nohold" : "any"); }

inline bool IsHold(uint32_t id) { return id >= kHoldFirst && id < kHoldEnd; }
inline bool Fits(int wish, uint32_t id) { return wish == kHold ? IsHold(id) : (wish == kNoHold ? !IsHold(id) : true); }

// S110 TOOLFIT (docs\research\s110\TOOLFIT.md 2): the posture families. sub_82602DE0 tests a candidate's move id ([cand+92]) against six ranges,
// each a predicate with `id >= first` at +30 and `id < end` at +44. Each also returns 1 for a tag at r4+2 = cand+18, and the other predicates of
// the chain +8CC..+A28 for tags at cand+18 / cand+20: cand+16 is the move's own record, filled from the id by sub_822580D8 at sub_82605418 +1F0.
// Not read here, so a hold we call family-free may still be sent to the family branch by its record; an id in a range never escapes it.
//   58100..58148 sub_82564D88 (+94C, +1C54: r21 3)   58150..58198 sub_82564EC8 (+974, +1BFC: r21 4)   58200..58248 sub_82564E28 (+960, +1CAC: r21 5)
//   58250..58298 sub_82564DD8 (+988, +1C68: r21 3)   58300..58348 sub_82564F18 (+9B0, +1C10: r21 4)   58350..58398 sub_82564E78 (+99C, +1CC0: r21 5)
// A hold in none (58000..58099; 58149, 58199, 58249, 58299, 58349) is family-free. That a family is the opponent's posture is [L] (run 289: the
// STF 58350 never played in 72000 frames; runs 296 / 297: 58021 / 58010, both family-free, ended by submission).
struct Family {
  uint32_t first, end;  // first <= id < end
  const char* test;     // the sub_82602DE0 call that sets r21 (+1CD8)
  int r21;
};
inline constexpr Family kFamilies[] = {{58100, 58149, "+1C54", 3}, {58150, 58199, "+1BFC", 4}, {58200, 58249, "+1CAC", 5},
                                       {58250, 58299, "+1C68", 3}, {58300, 58349, "+1C10", 4}, {58350, 58399, "+1CC0", 5}};
inline const Family* FamilyOf(uint32_t id) {
  for (const Family& f : kFamilies)
    if (id >= f.first && id < f.end) return &f;
  return nullptr;
}
inline bool FreeHold(uint32_t id) { return IsHold(id) && FamilyOf(id) == nullptr; }
constexpr int kFitFamily = 2;  // finisher_fit_S: a hold wish met only by holds in a posture family
// 1 a slot fits the wish; 2 (hold) a slot fits, but every fitting hold is in a family; 0 none fits
inline int FitOf(int wish, uint32_t a, uint32_t b) {
  if (!Fits(wish, a) && !Fits(wish, b)) return 0;
  return wish == kHold && !FreeHold(a) && !FreeHold(b) ? kFitFamily : 1;
}
// one slot's class for the verb's answer: "58350 a hold in posture family 58350..58398 (sub_82602DE0 +1CC0, r21 5)" / "58021 a family-free hold"
inline std::string ClassText(uint32_t id) {
  const std::string n = std::to_string(id);
  if (!IsHold(id)) return n + " not a hold";
  const Family* f = FamilyOf(id);
  if (f == nullptr) return n + " a family-free hold";
  return n + " a hold in posture family " + std::to_string(f->first) + ".." + std::to_string(f->end - 1) + " (sub_82602DE0 " + f->test + ", r21 " +
         std::to_string(f->r21) + ")";
}
inline bool IsFinisherPair(uint32_t a, uint32_t b) { return (a == kSlotA && b == kSlotB) || (a == kSlotB && b == kSlotA); }
// Whose moveset the chooser tests an offered slot against (sub_82605418 +70..+98): the character's own when the chooser context's word +80 is 0,
// else the character at [char+2672] (sub_826056E0 +FC copies +80 from the FRA component; every candidate is a copy of that context, +3E0..+44C).
// sub_825FDE00's own read (+D4) always uses the character: the hook judges by the move the chooser will actually test (S110 FINPICK review).
inline uint32_t MoveOwner(uint32_t ctx80, uint32_t ch, uint32_t other) { return ctx80 != 0 ? other : ch; }

struct Edit {
  bool applies = false;               // a hold / nohold wish and the (17, 18) pair: (19, 19) and the signature pair are never touched
  int fit = -1;                       // FitOf: 1 = a slot fits the wish, 2 = only family holds do, 0 = none does (the pair is left as the game wrote it)
  bool drop0 = false, drop1 = false;  // write kNone over [r4+0] / [r4+4]
};

// slot0 / slot1 = [r4+0] / [r4+4] as sub_825FDE00 wrote them; id0 / id1 = their live move ids.
inline Edit Decide(int wish, uint32_t slot0, uint32_t slot1, uint32_t id0, uint32_t id1) {
  Edit e;
  if ((wish != kHold && wish != kNoHold) || !IsFinisherPair(slot0, slot1)) return e;
  e.applies = true;
  const bool f0 = Fits(wish, id0), f1 = Fits(wish, id1);
  e.fit = FitOf(wish, id0, id1);
  e.drop0 = f1 && !f0;
  e.drop1 = f0 && !f1;
  return e;
}

// The per-seat store. Written on the game thread (the director's verb, the hook), read by the host state on the render thread.
inline std::atomic<int> g_wishes{0};     // seats with a wish: 0 = the hook runs the original and nothing else (one load)
inline std::atomic<int> g_wish[kSeats];  // Wish (static storage: 0 = any)
inline std::atomic<int> g_fit[kSeats];   // 1 / 0, read only while the seat has a wish
inline std::atomic<bool> g_told[kSeats];  // the "no fitting finisher" line was logged for this wish (once per match: the wish is cleared outside a match)

inline void Recount() {
  int n = 0;
  for (int s = 0; s < kSeats; ++s) n += g_wish[s].load(std::memory_order_relaxed) != kAny ? 1 : 0;
  g_wishes.store(n, std::memory_order_release);
}

// The host state: -1 no wish; 1 a slot of the pair fits; 2 (hold) only a hold in a posture family fits (S110 TOOLFIT); 0 none fits. Valid from
// the verb's `ok` (the slots' moves as they read then); every offer of the pair (the hook) sets it again from the live ids.
inline int FitState(int seat) {
  if (seat < 0 || seat >= kSeats || g_wish[seat].load(std::memory_order_acquire) == kAny) return -1;
  return g_fit[seat].load(std::memory_order_acquire);
}

inline std::string NoFitText(int seat, int wish, uint32_t id17, uint32_t id18) {
  return "seat " + std::to_string(seat) + ": no " + WishName(wish) + " finisher in slots 17/18 (ids " + std::to_string(id17) + ", " + std::to_string(id18) +
         "): the game's choice stands";
}

// The verb: the wish and the fit of slots 17 / 18 as their moves read now. -> the text of the director's answer.
inline std::string SetWish(int seat, int wish, uint32_t id17, uint32_t id18) {
  if (seat < 0 || seat >= kSeats) return "no such seat";
  g_told[seat].store(false, std::memory_order_relaxed);
  if (wish != kHold && wish != kNoHold) {
    g_wish[seat].store(kAny, std::memory_order_release);
    g_fit[seat].store(-1, std::memory_order_release);
    Recount();
    return "no wish: the game's own choice";
  }
  const int fit = FitOf(wish, id17, id18);
  g_fit[seat].store(fit, std::memory_order_release);
  g_wish[seat].store(wish, std::memory_order_release);
  Recount();
  const std::string head = "slots 17 / 18 = " + std::to_string(id17) + " / " + std::to_string(id18);
  if (fit == 1) return head + ": a " + std::string(WishName(wish)) + " finisher fits";
  if (fit == kFitFamily)
    return head + ": a hold finisher fits only in a posture family (" + ClassText(id17) + "; " + ClassText(id18) +
           "): no family-free hold (ids 58000..58099) to keep";
  return head + ": " + NoFitText(seat, wish, id17, id18);
}

// Outside a match: every wish goes (the next match's seats are other wrestlers).
inline void Clear() {
  if (g_wishes.load(std::memory_order_relaxed) == 0) return;
  for (int s = 0; s < kSeats; ++s) {
    g_wish[s].store(kAny, std::memory_order_release);
    g_fit[s].store(-1, std::memory_order_release);
    g_told[s].store(false, std::memory_order_relaxed);
  }
  g_wishes.store(0, std::memory_order_release);
}

// `finisher_fit_S` (S = 0..8) -> S, else -1.
inline int StateSeat(const std::string& name) {
  static const std::string kPrefix = "finisher_fit_";
  if (name.size() != kPrefix.size() + 1 || name.compare(0, kPrefix.size(), kPrefix) != 0) return -1;
  const char c = name.back();
  return (c >= '0' && c < char('0' + kSeats)) ? c - '0' : -1;
}

}  // namespace wwe2k14::finisher_pick
