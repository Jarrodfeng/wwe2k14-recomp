// S111 CAWTPOSE (docs\research\s111\CAWTPOSE.md): which character the game DRAWS in a slot, when that is not the one the character table names.
// The game draws a character through its own draw nodes, gated by sub_823CBC50 ([char+1208] opaque node on, [char+224] and [char+232] shown words; the node
// sub_825A3908 reads the same three), never because it sits in the character table 0x836F38D0. The Create a Superstar editor names its NEW character in slot 0
// at the start of the worker's build (sub_822E7570 +B0) while it keeps drawing and updating the OLD one until its buffer flip (sub_822E7CA0 turns the old one's
// +1208 / +1192 / shown words off and the new one's on, in one call). So a slot occupant that leaves the table while the game still draws it stays that slot's
// body, for exactly as long as it still owns its skeleton ([char+48]; the base destructor sub_823CCDD0 +108 clears it) and the game's predicate holds. A
// departed occupant whose words READ "not drawn" (at its departure or later) is named once with shown = false (the latch clears its draw stamps at once, so no
// liveness window outlives the game's hide) and forgotten; one whose words cannot be read is forgotten unnamed (never a guessed hide).
// The state is per latch owner (InputsContributor's member); the game's one latch also publishes the kept characters in g_kept_char for the update hook's slot
// match (another thread), and clears them when it goes. Pure: <atomic> / <cstdint> / <mutex> only; the inputs latch (inputs_contrib.h ReadSlotSkeletons), the update
// hook's slot match (wrestler_hook.h OnSkelExit) and tools\scene\drawn_slots_unit.cpp call THIS code. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// S111 FREEHOOK (docs\research\s111\FREEHOOK.md): the game destroying a character (char_release.h, the hook on its destructor body sub_825A1510) calls Forget, so no later Step reads the block
// sub_825A2A30 +34 frees, whether the character was the slot's kept departed body or its last occupant (a match release clears the table word in an update task, the post-barrier tick frees it).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace scene_inputs::drawn_slots {

inline constexpr uint32_t kSlots = 9;                 // the character table's 9 words
inline constexpr uint32_t kCharSkeleton = 48;         // [char+48] = the body model / skeleton
inline constexpr uint32_t kCharDrawOn = 1208;         // the opaque draw node's enable (sub_823CBC50 +10)
inline constexpr uint32_t kCharShownA = 224;          // sub_823CBC50 +1C
inline constexpr uint32_t kCharShownB = 232;          // sub_823CBC50 +28
inline constexpr uint32_t kPlantNoKeep = 1;           // test only: the old rule (the slot's body is the table's occupant, a departed one is never kept)
inline constexpr uint32_t kPlantNoHideName = 2;       // test only: a departure the game does not draw is forgotten unnamed (its draw stamps linger)
inline constexpr uint32_t kPlantReadFreed = 4;        // test only (S111 FREEHOOK): the old Step (nothing forgotten): a destroyed character's freed block is read at the next latch

struct Kept { uint32_t ch = 0, skel = 0; };
struct Out { uint32_t ch = 0, skel = 0; int32_t slot = -1; bool shown = false; };

// sub_823CBC50 on a character we met in a slot, plus its ownership of the skeleton we knew it by. Unread = a word could not be read (never guessed either way).
enum class Draw : uint8_t { Yes, No, Unread };
template <class ReadWordFn>
inline Draw GameDrawState(ReadWordFn&& rd, uint32_t ch, uint32_t skel) {
  uint32_t owner = 0, on = 0, a = 0, b = 0;
  if (ch < 0x10000 || !rd(ch + kCharSkeleton, owner)) return Draw::Unread;
  if (owner != skel) return Draw::No;   // it no longer owns that skeleton (destroyed: sub_823CCDD0 +108, or rebuilt): the game draws nothing of it through this body
  if (!rd(ch + kCharDrawOn, on) || !rd(ch + kCharShownA, a) || !rd(ch + kCharShownB, b)) return Draw::Unread;
  return on != 0 && a != 0 && b != 0 ? Draw::Yes : Draw::No;
}
template <class ReadWordFn>
inline bool GameDraws(ReadWordFn&& rd, uint32_t ch, uint32_t skel) { return GameDrawState(rd, ch, skel) == Draw::Yes; }

// The game's one latch publishes its kept characters here (InputsContributor's member is built with it); the update hook reads them.
inline std::atomic<uint32_t> g_kept_char[kSlots]{};
inline uint32_t KeptCharOfGame(uint32_t s) { return s < kSlots ? g_kept_char[s].load(std::memory_order_relaxed) : 0u; }

class Departed {
 public:
  Departed() = default;
  explicit Departed(std::atomic<uint32_t>* publish) : publish_(publish) {}
  Departed(const Departed&) = delete;
  Departed& operator=(const Departed&) = delete;
  ~Departed() {   // the update hook never matches a slot through a latch that is gone
    if (publish_)
      for (uint32_t s = 0; s < kSlots; ++s) publish_[s].store(0, std::memory_order_relaxed);
  }
  // One latch: `occ[s]` = the table's occupant of slot s at this latch ({0, 0} = empty or unreadable). Writes the kept departed bodies (shown) and the departures
  // named hidden once (shown = false) to `out` (room for kSlots) and returns how many. A second call in the same latch with the same table gives the same kept list.
  template <class ReadWordFn>
  uint32_t Step(ReadWordFn&& rd, const Kept (&occ)[kSlots], Out* out, uint32_t faults = 0) {
    uint32_t n = 0;
    if (!(faults & kPlantReadFreed)) DrainForgotten();   // S111 FREEHOOK: a character the game destroyed since the last Step is no candidate (its block is freed)
    for (uint32_t s = 0; s < kSlots; ++s) {
      Kept next{};
      const Kept cand = kept_[s].ch ? kept_[s] : last_occ_[s];   // the slot's last drawn body: its kept departed one, else its last occupant
      bool in_table = false;
      for (uint32_t k = 0; k < kSlots; ++k) in_table = in_table || (cand.ch && occ[k].ch == cand.ch) || (cand.skel && occ[k].skel == cand.skel);
      if (!(faults & kPlantNoKeep) && cand.ch && !in_table) {
        const Draw d = GameDrawState(rd, cand.ch, cand.skel);
        if (d == Draw::Yes) next = cand, out[n++] = Out{cand.ch, cand.skel, int32_t(s), true};
        else if (d == Draw::No && (kept_[s].ch || !(faults & kPlantNoHideName))) out[n++] = Out{cand.ch, cand.skel, int32_t(s), false};   // named once hidden, then forgotten
      }
      kept_[s] = next;
      last_occ_[s] = occ[s];
      published_[s].store(next.ch, std::memory_order_relaxed);
      if (publish_) publish_[s].store(next.ch, std::memory_order_relaxed);
    }
    return n;
  }
  // The kept departed character of slot s (0 = none) as this owner's last Step left it.
  uint32_t KeptChar(uint32_t s) const { return s < kSlots ? published_[s].load(std::memory_order_relaxed) : 0u; }
  // S111 FREEHOOK: the game is destroying character `ch` (any thread). The published words drop it at once (the update hook's slot match and the light latch stop naming it); kept_ / last_occ_
  // (the latch thread's) drop it at the start of the next Step. More than kForgetMax frees between two Steps: that Step forgets every remembered character (never a read of a freed block).
  void Forget(uint32_t ch) {
    if (ch < 0x10000) return;
    {
      std::lock_guard<std::mutex> lock(forget_mu_);
      if (forget_n_ < kForgetMax) forget_[forget_n_++] = ch;
      else forget_all_ = true;
    }
    for (uint32_t s = 0; s < kSlots; ++s) {
      uint32_t want = ch;
      published_[s].compare_exchange_strong(want, 0u, std::memory_order_relaxed);
      want = ch;
      if (publish_) publish_[s].compare_exchange_strong(want, 0u, std::memory_order_relaxed);
    }
  }

 private:
  static constexpr uint32_t kForgetMax = 64;
  void DrainForgotten() {   // the latch thread, at the start of Step
    uint32_t gone[kForgetMax];
    uint32_t n = 0;
    bool all = false;
    {
      std::lock_guard<std::mutex> lock(forget_mu_);
      n = forget_n_, all = forget_all_;
      for (uint32_t i = 0; i < n; ++i) gone[i] = forget_[i];
      forget_n_ = 0, forget_all_ = false;
    }
    if (!n && !all) return;
    for (uint32_t s = 0; s < kSlots; ++s) {
      bool k = all && kept_[s].ch, o = all && last_occ_[s].ch;
      for (uint32_t i = 0; i < n; ++i) k = k || (kept_[s].ch && kept_[s].ch == gone[i]), o = o || (last_occ_[s].ch && last_occ_[s].ch == gone[i]);
      if (k) kept_[s] = Kept{};
      if (o) last_occ_[s] = Kept{};
    }
  }
  std::atomic<uint32_t>* publish_ = nullptr;
  Kept kept_[kSlots]{}, last_occ_[kSlots]{};
  std::atomic<uint32_t> published_[kSlots]{};
  std::mutex forget_mu_;   // S111 FREEHOOK: guards forget_ / forget_n_ / forget_all_ only (Forget's callers are game threads)
  uint32_t forget_[kForgetMax]{};
  uint32_t forget_n_ = 0;
  bool forget_all_ = false;
};

}  // namespace scene_inputs::drawn_slots
