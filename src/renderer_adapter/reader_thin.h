// D1 thin draws, the READER side (Session 101 lane 122, KR; docs\renderer\SKIP_GAME_WORK_MAP.md 2.3). No game or SDK includes: the unit test (tools\thin\), fview's
// reader emulation and the game's reader all build THIS header, so what is tested is what ships.
//
// Per frame (a frame part): Begin() takes the newest pin set; for each draw Query(site, object, part) answers "go lite?" and counts the ordinal of the draw's
// (site, object, part). A draw goes lite when its key is in the set AND this is not the key's proof frame. The generation of the set it read is stamped on the frame.
//   * ordinals are counted for EVERY draw whose triple is in the set, lite or not, in draw order: the binder counts the same way over the frame's draw table;
//   * a key of an older set is never used: the decision is made from the one snapshot taken at Begin (the plant `stale_generation` decides from an older snapshot but
//     stamps the newest, which releases templates under a frame that still needs them).
#pragma once
#include <cstdint>
#include <memory>

#include "thin_draw.h"

namespace rdr_adapter::thin {

class FrameLite {
 public:
  // snapshot: the set to decide from (may be null: nothing is lite). stamp_generation: what the frame will carry. Normally the snapshot's own generation.
  void SetNoOrdinal(bool on) { no_ordinal_ = on; }  // the planted fault `no_ordinal`
  // lane 138 (D3 skip the object, d3_arena.h): with D3 on, the proof frame of a pinned key is its OBJECT's (every K-th frame by the object's hash), so all the parts of an object are full together and the
  // binder compares the whole object in one frame. lane 152 (13): it applies to the arena's keys only (the pin set carries the class: PinSet::arena_triples). Off: the key's own hash, as D1 always did.
  void SetProofByObject(bool on) { proof_by_object_ = on; }
  void SetWrestlerProofByObject(bool on) { wrestler_proof_by_object_ = on; }   // lane 227 (Wrestlers D3 H2): the same for the WRESTLERS' keys, behind their own switch
  void Begin(std::shared_ptr<const PinSet> snapshot, uint64_t frame, uint32_t proof_k = kProofK, int64_t stamp_override = -1) {
    snap_ = std::move(snapshot);
    frame_ = frame;
    k_ = proof_k;
    ++epoch_;
    lite_ = 0;
    stamp_ = stamp_override >= 0 ? uint32_t(stamp_override) : snap_ ? snap_->generation : 0;
    active_ = snap_ && !snap_->keys.empty();
  }
  // One draw of the frame, in draw order. True: build nothing but the anchor (flag kDrawThin, blank the rest).
  // eligible = false (an inline-vertex draw: its vertex bytes are new every frame and cannot be skipped) still counts its ordinal.
  bool Query(uint32_t pass_site, uint32_t object, uint32_t part, bool eligible = true, uint64_t program = 0) {
    if (!active_) return false;
    const uint64_t triple = TripleHash(pass_site, object, part);
    if (!snap_->HasTriple(triple)) return false;
    uint32_t ordinal = Next(triple);
    if (ordinal == ~0u) return false;  // more distinct pinned triples in one frame than the counter holds: never lite (the binder's ordinals would not match)
    if (no_ordinal_) ordinal = 0;
    last_ordinal_ = ordinal;
    const uint64_t key = KeyHash(pass_site, object, part, ordinal, program);
    if (!eligible || !snap_->Has(key) || IsProofFrame(((proof_by_object_ && snap_->IsArenaTriple(triple)) || (wrestler_proof_by_object_ && snap_->IsWrestlerTriple(triple))) ? ObjectProofHash(object) : key, frame_, k_)) return false;  // lane 138: the object's hash with D3 on; lane 152 (13): for the ARENA's keys only (the ring's and wrestlers' parts keep their own proof frames)
    ++lite_;
    return true;
  }
  // lane 289 (the ring SKIP arm): a SKIPPED ring call's draws never reach the stream, so its part triples are counted here at the marker's position (the binder's ComputeKeys counts the same list at the same place):
  // the instances after a cut keep their own ordinals. Only a pinned triple has a counter (Query counts the same way).
  void Reserve(const uint64_t* triples, size_t n) {
    if (!active_) return;
    for (size_t i = 0; i < n; ++i)
      if (snap_->HasTriple(triples[i])) (void)Next(triples[i]);
  }
  uint32_t last_ordinal() const { return last_ordinal_; }   // the ordinal Query last counted (the offline reader check compares it with the binder's key)
  uint32_t stamp() const { return stamp_; }
  uint32_t lite_draws() const { return lite_; }
  bool active() const { return active_; }

 private:
  // A small open-addressing counter table, cleared lazily by epoch (no per-frame memset, no allocation).
  uint32_t Next(uint64_t triple) {
    uint32_t i = uint32_t(triple >> 7) & (kSlots - 1);
    for (uint32_t probe = 0; probe < kSlots; ++probe, i = (i + 1) & (kSlots - 1)) {
      Slot& s = slots_[i];
      if (s.epoch != epoch_) {
        s.epoch = epoch_, s.triple = triple, s.n = 1;
        return 0;
      }
      if (s.triple == triple) return s.n++;
    }
    return ~0u;  // the table is full: the caller answers "not lite" (never wrong, only slower)
  }
  static constexpr uint32_t kSlots = 1024;
  struct Slot {
    uint64_t triple = 0;
    uint32_t n = 0, epoch = 0;
  };
  Slot slots_[kSlots];
  std::shared_ptr<const PinSet> snap_;
  uint64_t frame_ = 0;
  uint32_t k_ = kProofK, epoch_ = 0, lite_ = 0, stamp_ = 0, last_ordinal_ = 0;
  bool active_ = false, no_ordinal_ = false, proof_by_object_ = false, wrestler_proof_by_object_ = false;
};

// The blank a lite draw carries (the reader built none of it). Shared by the game's reader and fview's emulation, so "what the binder must not read" is one list.
struct LiteBlank {
  static constexpr uint64_t kHash = kBlankHash;
  static constexpr uint32_t kCount = kBlankCount;
};

}  // namespace rdr_adapter::thin
