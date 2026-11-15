// S102 lane T12: constant-bank change detection on the game's render-command thread without the generic memcmp (switch wwe2k14_rdr_bank_dirty,
// PerFrame, default off). The pieces live here so the unit test (bank_dirty_test.cpp) runs the very code the reader runs; reader.h keeps the glue.
//
// What the reader did per draw (PushBanks): (1) compare the 64-byte blocks the library marked dirty against our raw copy of the device floats
// (memcmp per block, reader.h ChangedBankBlocks: R13's 5149 / 5150); (2) for every block of touch = dirty | lit_mask | lit_last, build the block
// (device floats + the shader pair's literals) and memcmp it against the last bank sent (R13's 5246); equal = no patch.
//
// This lane keeps the source of truth the reader already has (the marked-block compare (1): the SDK's dirty masks say where to look, the bytes say
// whether it changed) and removes the work that follows from it:
//   A. (1) compares through four inline SSE2 loads (Equal64) instead of the C runtime's memcmp call (its call, length dispatch and byte
//      tail were the cost of a 64-byte compare).
//   B. (2) skips blocks that cannot differ from the last bank sent and does not compare blocks that are known to differ:
//      Invariant I: after every emit, `last` == device floats + that draw's literals, for all 64 blocks of the stage.
//      Facts the reader has at the next emit: `dirty` = blocks whose device bytes (1) found different from our raw copy since the last emit (an
//      emit follows every refresh that changed anything, so it is exactly the blocks whose floats changed since `last` was built);
//      `lit_now` / `lit_last` = blocks the current / last emit's literal table covers; `lits_same` = the literal table is the very one of the last emit.
//        - not dirty and the literals are the same        => block == last: not visited at all (was: built and compared);
//        - dirty and in no literal block (now or last)    => the floats changed and no literal overlays them or overlaid them: block != last: a patch
//                                                            with no compare;
//        - every other block of touch                     => built and compared as before.
//      Exactness needs "dirty means different from last", which fails for one emit: after the device pointer changed (the raw copy was refilled with a
//      pattern, so every block reads as changed). `dirty_exact` is false for that emit and the plan falls back to the old touch set and compares.
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
#include "adapter_frame.h"

namespace rdr_adapter::bank_dirty {

// The census (the 600-frame line). Written by the game's render-command thread only (every hook body runs under the reader's mutex).
struct Counters {
  uint64_t frames = 0;             // frames in which at least one emit ran with the switch on
  uint64_t frames_declined = 0;    // frames whose first emit(s) had to take the old plan (device change)
  uint64_t emits = 0;              // stage emits that ran with the switch on
  uint64_t emits_old_plan = 0;     // ... of which fell back to the old touch set (declined: dirty not exact)
  uint64_t blocks_touch = 0;       // blocks the old plan would have built and compared (touch)
  uint64_t blocks_built = 0;       // blocks built (device floats + literals)
  uint64_t blocks_compared = 0;    // blocks compared with the last bank
  uint64_t blocks_skipped_same = 0;   // not visited: not dirty and the literals unchanged
  uint64_t blocks_skipped_known = 0;  // built but not compared: dirty and no literal covers them
  uint64_t refresh_blocks = 0;     // marked blocks compared against the device bytes (inline SSE2 compares)
  uint64_t patches = 0;            // patches written
};

// 64 bytes equal? Four 16-byte loads, no call. (Non-x64: the plain compare.)
inline bool Equal64(const uint8_t* p, const uint8_t* q) {
#if defined(_M_X64) || defined(__x86_64__)
  const __m128i x0 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q)));
  const __m128i x1 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 16)));
  const __m128i x2 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 32)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 32)));
  const __m128i x3 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 48)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 48)));
  const __m128i x = _mm_or_si128(_mm_or_si128(x0, x1), _mm_or_si128(x2, x3));
  return _mm_movemask_epi8(_mm_cmpeq_epi8(x, _mm_setzero_si128())) == 0xFFFF;
#else
  return std::memcmp(p, q, 64) == 0;
#endif
}

// The blocks of `candidates` whose device bytes differ from our raw copy. fast = Equal64, else the old per-block memcmp (the switch-off path).
inline uint64_t ChangedBlocks(const uint8_t* device_raw, const uint8_t* raw, uint64_t candidates, bool fast, Counters* c = nullptr) {
  uint64_t mask = 0;
  for (uint64_t m = candidates; m; m &= m - 1) {
    const uint32_t b = uint32_t(std::countr_zero(m));
    const bool equal = fast ? Equal64(device_raw + 64 * b, raw + 64 * b) : std::memcmp(device_raw + 64 * b, raw + 64 * b, 64) == 0;
    if (!equal) mask |= 1ull << b;
    if (c && fast) ++c->refresh_blocks;
  }
  return mask;
}

// Which blocks an emit visits and which of them are known to differ from the last bank.
struct Plan {
  uint64_t visit = 0;       // blocks to build
  uint64_t known_diff = 0;  // subset of visit: a patch without a compare
  bool old_plan = false;    // the switch-off set, or the fallback (dirty not exact)
};
inline Plan PlanEmit(bool on, uint64_t dirty, uint64_t lit_now, uint64_t lit_last, bool lits_same, bool dirty_exact) {
  Plan p;
  const uint64_t lit = lit_now | lit_last;
  if (!on || !dirty_exact) {
    p.visit = dirty | lit;
    p.old_plan = true;
    return p;
  }
  const uint64_t pure = dirty & ~lit;                        // floats changed, no literal overlays them now or overlaid them at the last emit
  const uint64_t lit_check = lits_same ? (dirty & lit) : lit;  // literal blocks that can differ: the changed ones, or all of them when the table changed
  p.visit = pure | lit_check;
  p.known_diff = pure;
  return p;
}

// The bookkeeping the plan rests on. It runs on EVERY draw whether the switch is on or not (so a per-frame / per-draw flip of the switch finds it exact):
// reader.h calls these and nothing else touches the fields.
struct State {
  bool dirty_exact = false;        // "a dirty block differs from the last bank sent" holds for the next emit
  uint64_t lit_serial = 0;         // +1 at every rebuild of the literal table (reader.h LitCache)
  uint64_t lit_serial_last = ~0ull;  // the serial at the last emit
  void OnDeviceChange() { dirty_exact = false; }  // the raw copy was refilled with a pattern: every block reads as changed
  void OnLitRebuild() { ++lit_serial; }
  bool LitsSame() const { return lit_serial_last == lit_serial; }
  Plan PlanFor(bool on, uint64_t dirty, uint64_t lit_now, uint64_t lit_last) const { return PlanEmit(on, dirty, lit_now, lit_last, LitsSame(), dirty_exact); }
  void AfterEmit() {  // both stages' last banks now equal the device floats + this draw's literals
    dirty_exact = true;
    lit_serial_last = lit_serial;
  }
};

// Writes the patches of one stage's emit into `bank_patches` (appends) and returns the new def's index, or `last_def` when nothing changed.
// `block_of(b, out[16])` builds block b (device floats + literals). `last` is updated for every patched block. Same loop as reader.h's
// emit lambda had (ascending block order, patch = the whole 64 bytes), with the plan deciding what is built and compared.
template <class BlockOf>
inline uint32_t EmitPatches(std::vector<rdr_adapter::BankDef>& bank_defs, std::vector<rdr_adapter::BankPatch>& bank_patches, const Plan& plan, uint64_t touch_old_count,
                            BlockOf&& block_of, rdr_adapter::ConstantBank& last, uint32_t last_def, Counters* c) {
  rdr_adapter::BankDef d;
  d.base = last_def;
  d.patch_first = uint32_t(bank_patches.size());
  for (uint64_t m = plan.visit; m; m &= m - 1) {
    const uint32_t b = uint32_t(std::countr_zero(m));
    uint32_t block[16];
    block_of(b, block);
    if (c) ++c->blocks_built;
    if (plan.known_diff >> b & 1) {
      if (c) ++c->blocks_skipped_known;
    } else {
      if (c) ++c->blocks_compared;
      if (Equal64(reinterpret_cast<const uint8_t*>(block), reinterpret_cast<const uint8_t*>(last.dwords + 16 * b))) continue;  // (F2: R13's 5246 site, inline)
    }
    rdr_adapter::BankPatch& patch = bank_patches.emplace_back();
    patch.block = b;
    std::memcpy(patch.dwords, block, 64);
    std::memcpy(last.dwords + 16 * b, block, 64);
    if (c) ++c->patches;
  }
  if (c) {
    c->blocks_touch += touch_old_count;
    c->blocks_skipped_same += touch_old_count - uint64_t(std::popcount(plan.visit));
  }
  d.patch_count = uint32_t(bank_patches.size()) - d.patch_first;
  if (!d.patch_count) return last_def;  // the same bank as before
  const uint32_t index = uint32_t(bank_defs.size());
  bank_defs.push_back(d);
  return index;
}

}  // namespace rdr_adapter::bank_dirty
