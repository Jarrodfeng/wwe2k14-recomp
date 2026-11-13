// The crowd card bake's invisible cut, decision logic only (Session 101, lane 155; docs\renderer\SKIP_GAME_WORK_MAP.md section 12, CROWD_CARDS_PROMOTION_MAP.md 1.3 / 9.7).
// No game, SDK or renderer includes: the game-side hook (bake_skip_hook.h) asks it, and tools\crowd\bake_skip_test.cpp runs this very file on a fake guest memory.
//
// What the game does (listings, docs\systems\crowd.md "Card pictures"): sub_827FD698 is the thumbnail container's pass. For each of the container's thumbnails (one per person type) it loads the
// thumbnail's render-state snapshot, sets the type's own 64 x 64 targets, clears, draws the type's LOD-2 body and copies the target into the type's picture. It skips a thumbnail ITSELF when the
// thumbnail struct's model count (word +960) is 0 or its model cell (word at [+964]) is null (0x827FD754-0x827FD76C: `lwz r10,960(r11); cmpwi; beq` / `lwz r10,964; lwz r9,0(r10); cmplwi; beq`).
// Only the far cards read the 20 pictures (CROWD_CARDS_PROMOTION_MAP 9.2). So the cut is: for a type whose cards have not been asked for lately, set +960 to 0 for the span of the pass (the game's
// own skip branch then takes the whole iteration: no snapshot load, no targets, no clear, no draw, no copy), and write the saved value back when the pass returns.
//
// The picture of a skipped type keeps its last bake. The guarantee is therefore an AGE, not "never stale": a type that has had no card for `history` crowd draws is baked only once in `refresh`
// bake passes (staggered by type), so a card that reappears reads a picture at most `refresh` bake passes old (the pose and the light set of at most that many frames ago on a figure a few
// pixels high), and the cards' own promotion (crowd_cards_core.cpp, lane 223) takes a type's picture from the image its own stream cards show (the bake join is only a cross-check), so a type whose bake is absent
// is drawn with the picture the game's cards read (the last bake's bytes, no hole, no wrong picture). Everything is counted; `exposure` below is the age histogram of every read.
#pragma once

#include <atomic>
#include <cstdint>

namespace wwe2k14::bake_skip {

inline constexpr uint32_t kTypes = 20;
inline constexpr uint32_t kMaxCapacity = 256;   // a container claiming more slots than this is not the thumbnail container
// The container (this = r3 of sub_827FD698): +16 is the slot CAPACITY (128 live; review B1), +20 the array of thumbnail struct pointers. Only slots 0-19 (the person types) are registered.
inline constexpr uint32_t kOffCount = 16, kOffStructs = 20;
// A thumbnail struct: the model count the pass tests, the pointer to the cell array whose first word is the model.
inline constexpr uint32_t kOffModelCount = 960, kOffCells = 964;
// A guest address the hook may read or write: the game's heap and static data (heap, image and the 64 KB physical pages; not the 16 MB-page physical aliases at 0xC0000000), above the null page. Anything else is treated as "unreadable" and left alone.
inline constexpr bool Plausible(uint32_t a) { return a >= 0x10000u && a < 0xC0000000u; }
inline constexpr uint32_t kAgeBuckets = 6;  // ages 0 .. 4, and 5 or more

struct Policy {
  uint32_t history = 8;   // crowd draws without a card of the type before its bake may be skipped
  uint32_t refresh = 4;   // a skipped type is baked once in this many bake passes (staggered by type); the age bound of a read
  bool anchor_last = true;   // review S2: the last live thumbnail of a pass is never cut (it leaves the post-pass constant registers the game's own)
  uint32_t warmup = 16;   // bake passes at the start (and after a reset) in which nothing is skipped: every type is baked, so no picture is ever read before its first bake
};

// 0 = off, 1 = census only (the decision is made and counted, the guest is NOT touched: the live check's first step), 2 = skip.
enum class Mode : uint8_t { Off = 0, Census = 1, Skip = 2 };

// Skipped = the cut; Anchor = baked because it is the LAST live thumbnail of the pass (review S2); the others baked.
enum class Why : uint8_t { Cards, Warmup, Refresh, GameSkips, Skipped, Anchor, Count };

struct Counters {
  std::atomic<uint64_t> passes{0};
  std::atomic<uint64_t> why[static_cast<unsigned>(Why::Count)] = {};  // per thumbnail iteration
  std::atomic<uint64_t> unreadable{0};         // a pass whose container could not be read (null / count past kTypes): left alone
  std::atomic<uint64_t> reads{0};              // card loops (per type per crowd draw) seen
  std::atomic<uint64_t> read_age[kAgeBuckets] = {};  // the picture's age at a read, in bake passes since the type's last bake (as the policy baked it)
  std::atomic<uint64_t> age_max{0};
  std::atomic<uint64_t> age_over_bound{0};     // reads with an age above Policy::refresh (must stay 0)
  std::atomic<uint64_t> read_never_baked{0};   // reads of a type the ledger has no bake for (a fresh picture after ForgetType): not an age
  std::atomic<uint64_t> foreign{0};            // container slots past kTypes that hold a thumbnail (left alone)
  std::atomic<uint64_t> overlap{0}, overlap_other_thread{0};  // lane 155 review S1: the other readers of +960 / +964 (pose writers) ran inside a pass
  std::atomic<uint64_t> edits{0}, restores{0}, restore_skipped{0};  // guest writes made / undone / not undone because the word had changed under us
};

class Ledger {
 public:
  explicit Ledger(Policy p = Policy()) : p_(p) {}
  const Policy& policy() const { return p_; }
  Counters& counters() { return c_; }
  const Counters& counters() const { return c_; }

  // The crowd draw entry began (sub_82519508; a tiled frame runs it once per tile).
  void BeginCrowdDraw() { draws_.fetch_add(1, std::memory_order_relaxed); }
  // The card loop of `type` ran (sub_82512F70, r3 = type): the type's cards are asked for NOW, and the picture they read is as old as its last bake.
  void NoteCards(uint32_t type) {
    if (type >= kTypes) return;
    const uint64_t d = draws_.load(std::memory_order_relaxed);
    last_cards_[type].store(d + 1, std::memory_order_relaxed);   // +1: 0 means never
    const uint64_t passes = c_.passes.load(std::memory_order_relaxed);        // passes begun so far; the last one has index passes - 1
    const uint64_t baked = last_bake_[type].load(std::memory_order_relaxed);   // index + 1 of the pass that last baked the type (0 = never)
    c_.reads.fetch_add(1, std::memory_order_relaxed);
    if (baked == 0) {   // a picture the policy never baked (a re-registered type before its first pass): counted apart, never an age
      c_.read_never_baked.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint64_t a = passes >= baked ? passes - baked : 0;                   // 0 = baked by the pass that ran just before this card loop (guarded: Reset can zero passes under a reader)
    c_.read_age[a < kAgeBuckets ? a : kAgeBuckets - 1].fetch_add(1, std::memory_order_relaxed);
    uint64_t m = c_.age_max.load(std::memory_order_relaxed);
    while (a > m && !c_.age_max.compare_exchange_weak(m, a, std::memory_order_relaxed)) {}
    if (a > p_.refresh) c_.age_over_bound.fetch_add(1, std::memory_order_relaxed);
  }
  // The pass began: the decision for each type is asked with Decide (in the pass's own order), the pass ordinal is fixed here.
  void BeginPass() {
    if (warm_req_.exchange(false, std::memory_order_relaxed)) Reset();   // review (3): a re-registration asks, the pass's own thread resets, never the loader thread
    cur_pass_ = c_.passes.fetch_add(1, std::memory_order_relaxed);   // the index of THIS pass (0 based)
  }
  // `game_skips` = the game itself will skip this thumbnail (model count 0 or no model): nothing to decide, and nothing baked.
  Why Decide(uint32_t type, bool game_skips, bool anchor = false) {
    Why w;
    if (game_skips) {
      w = Why::GameSkips;
    } else {
      const uint64_t d = draws_.load(std::memory_order_relaxed);
      const uint64_t lc = last_cards_[type].load(std::memory_order_relaxed);   // crowd draw index + 1 of the last card loop (0 = never)
      if (cur_pass_ < p_.warmup || last_bake_[type].load(std::memory_order_relaxed) == 0) w = Why::Warmup;   // review B2: a type with no bake on the ledger is never cut
      else if (lc != 0 && d - (lc - 1) <= p_.history) w = Why::Cards;
      else if (p_.refresh == 0 || (cur_pass_ + type) % p_.refresh == 0) w = Why::Refresh;
      else w = (anchor && p_.anchor_last) ? Why::Anchor : Why::Skipped;
    }
    c_.why[static_cast<unsigned>(w)].fetch_add(1, std::memory_order_relaxed);
    if (w != Why::Skipped && w != Why::GameSkips) last_bake_[type].store(cur_pass_ + 1, std::memory_order_relaxed);   // the policy bakes it (virtual in census mode)
    return w;
  }
  // Review B2 / (3): the crowd is rebuilt per match; sub_827FD4C8 registers a type's NEW picture (the container can come back at the same address). That asks for a full warm-up: the flag is consumed by the
  // pass's own thread at its next BeginPass (Reset there; the loader thread never touches the ledger's arrays).
  void ForgetType(uint32_t type) {
    if (type >= kTypes) return;
    warm_req_.store(true, std::memory_order_relaxed);
  }
  // Review S1 / (4): the pass is running (the flag opens BEFORE PlanPass and closes AFTER EndPass; each opening is a generation). The other readers of +960 / +964 bracket their call with ReaderEnter /
  // ReaderExit: an overlap is counted when the pass was open at entry, is open at exit, or a pass opened and closed in between (the generation changed).
  void EnterPass(uint64_t thread) {
    gen_.fetch_add(1, std::memory_order_relaxed);
    pass_thread_.store(thread + 1, std::memory_order_relaxed);
    in_pass_.store(true, std::memory_order_relaxed);
  }
  void LeavePass() { in_pass_.store(false, std::memory_order_relaxed); }
  uint64_t ReaderEnter(uint64_t thread) {
    const bool open = in_pass_.load(std::memory_order_relaxed);
    if (open) NoteOverlap(thread);
    return gen_.load(std::memory_order_relaxed) * 2 + (open ? 1 : 0);
  }
  void ReaderExit(uint64_t token, uint64_t thread) {
    const bool was_open = token & 1;
    if (was_open) return;   // already counted at entry
    if (in_pass_.load(std::memory_order_relaxed) || gen_.load(std::memory_order_relaxed) != token / 2) NoteOverlap(thread);
  }
  void Reset() {   // a new arena / manager: everything is baked again for `warmup` passes; the census restarts with it
    c_.passes.store(0), c_.unreadable.store(0), c_.reads.store(0), c_.age_max.store(0), c_.age_over_bound.store(0), c_.read_never_baked.store(0), c_.foreign.store(0);
    c_.edits.store(0), c_.restores.store(0), c_.restore_skipped.store(0), c_.overlap.store(0), c_.overlap_other_thread.store(0);
    for (auto& w : c_.why) w.store(0);
    for (auto& a : c_.read_age) a.store(0);
    for (uint32_t t = 0; t < kTypes; ++t) last_cards_[t].store(0), last_bake_[t].store(0);
    draws_.store(0);
    cur_pass_ = 0;
  }

 private:
  Policy p_;
  Counters c_;
  std::atomic<uint64_t> draws_{0};
  std::atomic<uint64_t> last_cards_[kTypes] = {};
  std::atomic<uint64_t> last_bake_[kTypes] = {};
  uint64_t cur_pass_ = 0;
  void NoteOverlap(uint64_t thread) {
    c_.overlap.fetch_add(1, std::memory_order_relaxed);
    if (pass_thread_.load(std::memory_order_relaxed) != thread + 1) c_.overlap_other_thread.fetch_add(1, std::memory_order_relaxed);
  }
  std::atomic<bool> in_pass_{false};
  std::atomic<bool> warm_req_{false};
  std::atomic<uint64_t> gen_{0};
  std::atomic<uint64_t> pass_thread_{0};
};

struct Edit { uint32_t addr, saved; };
struct Plan {
  Edit edits[kTypes];
  uint32_t n = 0;
  uint32_t skipped_mask = 0;   // bit t = the policy cuts type t's bake in this pass (set in census mode too, where nothing is written)
  bool readable = false;
};

// Reads the container the way sub_827FD698 does and decides every thumbnail. read32(guest address) -> big-endian word value (as the guest sees it); write32(address, value) stores one.
// mode Skip writes +960 := 0 for each skipped type (saved in the plan); Census writes nothing. Nothing is written for a thumbnail the game skips itself or whose pointers are null.
template <class Read32, class Write32>
Plan PlanPass(Ledger& L, Mode mode, uint32_t container, Read32 read32, Write32 write32) {
  Plan plan;
  Counters& c = L.counters();
  const uint32_t count = Plausible(container) ? read32(container + kOffCount) : 0;
  const uint32_t arr = Plausible(container) ? read32(container + kOffStructs) : 0;
  // Review B1: container +16 is the CAPACITY (128 live, sub_824E55B0 -> sub_827FD268), not the number of types; only the first 20 slots are ever registered.
  if (!Plausible(container) || !Plausible(arr) || count == 0 || count > kMaxCapacity) {
    c.unreadable.fetch_add(1, std::memory_order_relaxed);
    return plan;
  }
  plan.readable = true;
  L.BeginPass();
  for (uint32_t t = count > kTypes ? kTypes : count; t < count; ++t)   // slots past the 20 person types: counted if filled, never touched
    if (Plausible(read32(arr + 4 * t))) c.foreign.fetch_add(1, std::memory_order_relaxed);
  // The FIRST live thumbnail is kept too (match15, 15-type arena: the capture's patches show lights, camera and others written ONLY by the first iteration; whether the game writes them once per pass or once
  // per 'lights changed' flag, a cut first iteration could leave them unwritten). The pass leaves the constant registers its LAST executed iteration wrote (the recorder appends every write; the draws after the bakes inherit the blocks nothing rewrites: review S2, bake_skip_regs.py).
  // Every iteration writes the same registers (one program), so keeping the last live thumbnail keeps the post-pass register state the game's own. A live thumbnail past the 20 types runs after ours: then
  // nothing of ours is last.
  struct Slot { uint32_t st = 0, n = 0; bool game_skips = true; } slots[kTypes];
  int anchor = -1, first_anchor = -1;
  for (uint32_t t = 0; t < count && t < kTypes; ++t) {
    Slot& sl = slots[t];
    sl.st = read32(arr + 4 * t);
    sl.game_skips = !Plausible(sl.st);   // null or garbage: the game's own check (or its crash) is not ours to second-guess; nothing is decided or written
    if (!sl.game_skips) {
      sl.n = read32(sl.st + kOffModelCount);
      const uint32_t cells = read32(sl.st + kOffCells);
      sl.game_skips = sl.n == 0 || !Plausible(cells) || read32(cells) == 0;
    }
    if (!sl.game_skips) {
      anchor = int(t);
      if (first_anchor < 0) first_anchor = int(t);
    }
  }
  bool foreign_live = false;
  for (uint32_t t = kTypes; t < count; ++t) {   // a live thumbnail past the person types runs after every one of ours: review (5): cut NOTHING that pass
    const uint32_t fst = read32(arr + 4 * t);
    if (Plausible(fst) && read32(fst + kOffModelCount) != 0) foreign_live = true;
  }
  for (uint32_t t = 0; t < count && t < kTypes; ++t) {
    const uint32_t st = slots[t].st, n = slots[t].n;
    const bool game_skips = slots[t].game_skips;
    const Why w = L.Decide(t, game_skips, foreign_live || int(t) == anchor || int(t) == first_anchor);
    if (w == Why::Skipped) plan.skipped_mask |= 1u << t;
    if (w == Why::Skipped && mode == Mode::Skip) {
      plan.edits[plan.n++] = {st + kOffModelCount, n};
      write32(st + kOffModelCount, 0);
      c.edits.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return plan;
}

// Puts the saved model counts back, only where the word still holds our 0 (a word somebody else changed in between is theirs).
template <class Read32, class Write32>
void EndPass(Ledger& L, const Plan& plan, Read32 read32, Write32 write32) {
  Counters& c = L.counters();
  for (uint32_t i = 0; i < plan.n; ++i) {
    if (read32(plan.edits[i].addr) == 0) {
      write32(plan.edits[i].addr, plan.edits[i].saved);
      c.restores.fetch_add(1, std::memory_order_relaxed);
    } else {
      c.restore_skipped.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace wwe2k14::bake_skip
