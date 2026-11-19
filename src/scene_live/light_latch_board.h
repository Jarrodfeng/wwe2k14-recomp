// The comparison half of lane A6 (bits 4 / 7 from memory; docs/renderer/INPUTS_A6.md): the predicted-install record, the counters, the two compares and the board the lights contributor's Emit reads. Copyright (c) the
// wwe2k14-recomp authors. BSD-3-Clause. Header only; needs only scene_record.h and light_index_core.h (no guest reads), so light_state_contrib.h can include it. The predictors are light_latch.h.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer_scene/scene_record.h"
#include "light_index_core.h"

namespace rdr_scene::live::lightlatch {

inline constexpr uint32_t kFreshFrames = 1;   // a table entry confirmed within this many hook frames of the latch's hook frame counts as "walked this frame"

enum Class : uint32_t { kWrestler = 0, kRope, kFixture, kCanvas, kSkirt, kClassCount };
inline const char* ClassName(uint32_t c) {
  static const char* n[kClassCount] = {"wrestler", "rope", "fixture", "canvas", "skirt"};
  return c < kClassCount ? n[c] : "?";
}

// Test-only knobs: a planted fault is a flag here (the same pattern as InstallFaults). Shipped code passes nullptr.
struct LatchFaults {
  bool ignore_gate = false;      // the wrestler's gate / override rule is not applied (always the override path)
  bool swap_ring_words = false;  // the ropes and the fixtures read each other's index word
  bool count_stale = false;      // an entry the walk did not confirm this frame is compared as if it had been
  uint32_t compare_words = rdr_scene::kLsLightFloats;   // how many words of the 896-byte block are compared
  bool skeleton_52 = false;      // the model is [char + 52] instead of [char + 48]
  bool count_unchanged = false;  // the state / block compare also counts predictions whose block did not change since the previous latch
  bool ignore_block_flag = false; // the manager row's kLoBlockRead flag is not required
};

struct Pred {
  uint32_t cls = 0, model = 0;
  uint32_t source = 0;          // the SOURCE block address the install copies (object + 48); exact for the embedded object, the first row's object for a manager object (identical blocks share a row)
  bool source_exact = false;
  uint32_t index = 0, mode = 0; // raw, as sub_826203E8 would be asked (kNoIndex / kNoIndex for the embedded path)
  bool have_block = false;
  bool changed = false;         // the (block, index, mode, source) differs from this model's previous latch (a static light cannot tell a stale read from a fresh one); set by Board::Mark
  uint32_t block[rdr_scene::kLsLightFloats] = {};
};

struct Stats {
  uint64_t frames = 0;                                   // latches that produced predictions
  uint64_t preds[kClassCount] = {}, no_install[kClassCount] = {}, unreadable[kClassCount] = {}, unresolved[kClassCount] = {};
  uint64_t idx_eq[kClassCount] = {}, idx_diff[kClassCount] = {}, idx_no_entry[kClassCount] = {}, idx_not_walked[kClassCount] = {};
  uint64_t blk_eq[kClassCount] = {}, blk_diff[kClassCount] = {};   // the block at the TABLE's object address against the predicted block
  uint64_t st_eq[kClassCount][3] = {}, st_diff[kClassCount][3] = {};   // walk LightState vs predicted block, delta -1 / 0 / +1 (walk frame - latch hook frame)
  uint64_t ring_unnoted = 0, no_manager = 0;
  uint64_t changed_preds[kClassCount] = {}, unchanged_skipped[kClassCount] = {};   // predictions whose block changed since the previous latch (the only ones the block / state compares count) / those skipped
  uint64_t changed_frames = 0;                           // latches with at least one changed prediction
  uint64_t emitted_no_pred = 0, dup_hooks = 0;           // kept wrestler skeletons with no prediction (class present, no prediction); latches seen twice for one hook frame (not counted twice)
  char first_no_pred[48] = {};
  uint64_t latch_entries = 0;
  char first_idx[160] = {}, first_state[160] = {};       // the first difference of each kind, for the census
};

inline void NoteFirst(char* dst, const char* text) {
  if (dst[0] == 0) std::snprintf(dst, 160, "%s", text);
}

// Compare the predictions of a latch (hook frame `hook`) with the table as the walk learned it. `find(model, entry)` = Table::Find; `block_at(addr, words)` reads the block at the TABLE's object address
// (nullptr = not compared). Only entries confirmed within kFreshFrames of `hook` were walked this frame; the others are counted apart, never as equal.
template <class FindFn, class BlockFn>
inline void CompareIndex(const std::vector<Pred>& preds, uint32_t hook, FindFn find, BlockFn block_at, Stats& st, const LatchFaults* ft = nullptr) {
  using rdr_scene::live::lightidx::NodeLight;
  for (const Pred& p : preds) {
    NodeLight e;
    if (!find(p.model, e)) {
      ++st.idx_no_entry[p.cls];
      continue;
    }
    const bool fresh = e.frame + kFreshFrames >= hook && e.frame <= hook + kFreshFrames;
    if (!fresh && !(ft && ft->count_stale)) {
      ++st.idx_not_walked[p.cls];
      continue;
    }
    const bool same = e.index == p.index && e.mode == p.mode && (!p.source_exact || e.object == p.source);
    if (same) {
      ++st.idx_eq[p.cls];
    } else {
      ++st.idx_diff[p.cls];
      char b[160];
      std::snprintf(b, sizeof b, "%s model %08X: latch idx %08X mode %08X src %08X, walk idx %08X mode %08X obj %08X", ClassName(p.cls), p.model, p.index, p.mode, p.source, e.index, e.mode, e.object);
      NoteFirst(st.first_idx, b);
    }
    uint32_t w[rdr_scene::kLsLightFloats];
    if (p.have_block && (p.changed || (ft && ft->count_unchanged)) && block_at(e.object, w)) {   // only a CHANGED block can tell a stale read from a fresh one
      if (std::memcmp(w, p.block, sizeof w) == 0) ++st.blk_eq[p.cls];
      else ++st.blk_diff[p.cls];
    }
  }
}

// Compare the predicted blocks with the walk's LightStates of the same model: `states` / `refs` are one closed walk frame (LightState::light = R+0..895 as floats; a LightRef carries the model and the hook frame).
// For every prediction and every delta (walk frame - latch hook frame) in -1..+1 the FIRST ref of the model is compared, bit for bit over `compare_words` words.
template <class StateT, class RefT>
inline void CompareStates(const std::vector<Pred>& preds, uint32_t hook, const StateT* states, uint32_t n_states, const RefT* refs, uint32_t n_refs, Stats& st, const LatchFaults* ft = nullptr) {
  const uint32_t words = ft ? std::min<uint32_t>(ft->compare_words, rdr_scene::kLsLightFloats) : rdr_scene::kLsLightFloats;
  for (const Pred& p : preds) {
    if (!p.have_block) continue;
    if (!p.changed && !(ft && ft->count_unchanged)) continue;
    bool done[3] = {false, false, false};
    for (uint32_t i = 0; i < n_refs; ++i) {
      const RefT& r = refs[i];
      if (r.object != p.model || r.state >= n_states) continue;
      const int64_t d = int64_t(r.frame) - int64_t(hook);
      if (d < -1 || d > 1 || done[d + 1]) continue;
      done[d + 1] = true;
      const bool same = std::memcmp(states[r.state].light, p.block, size_t(words) * 4) == 0;
      if (same) {
        ++st.st_eq[p.cls][d + 1];
      } else {
        ++st.st_diff[p.cls][d + 1];
        uint32_t k = 0;
        while (k < words && std::memcmp(&states[r.state].light[k], &p.block[k], 4) == 0) ++k;
        char b[160];
        uint32_t a = 0;
        std::memcpy(&a, &states[r.state].light[k], 4);
        std::snprintf(b, sizeof b, "%s model %08X delta %+d: first word %u walk %08X latch %08X", ClassName(p.cls), p.model, int(d), k, a, k < words ? p.block[k] : 0u);
        NoteFirst(st.first_state, b);
      }
    }
  }
}

// The latch's predictions, kept for the last few hook frames so the writer thread's compare (the lights contributor's Emit) can pair them with a closed walk frame.
class Board {
 public:
  static constexpr uint32_t kKeep = 6;
  // Was this hook frame's latch already stored? (Emit's own Take after a missed latch must not store or count the same hook twice.) A repeat is counted in dup_hooks.
  bool HasHook(uint32_t hook) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Ent& e : ring_)
      if (e.valid && e.hook == hook) {
        ++st_.dup_hooks;
        return true;
      }
    return false;
  }
  // Sets Pred::changed against the previous latch's prediction of the same model (no previous = not changed: nothing to compare with). Read only; Put records the new baseline.
  void Mark(std::vector<Pred>& preds) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Pred& p : preds) {
      auto it = prev_.find(p.model);
      p.changed = it != prev_.end() && (it->second.index != p.index || it->second.mode != p.mode || it->second.source != p.source || std::memcmp(it->second.block, p.block, sizeof p.block) != 0);
    }
  }
  void Put(uint32_t hook, std::vector<Pred>&& preds, const Stats& add) {
    std::lock_guard<std::mutex> lock(mu_);
    bool any = false;
    for (const Pred& p : preds) {
      if (p.changed) ++st_.changed_preds[p.cls], any = true;
      else ++st_.unchanged_skipped[p.cls];
      prev_[p.model] = p;
    }
    if (any) ++st_.changed_frames;
    Ent& e = ring_[next_++ % kKeep];
    e.hook = hook, e.valid = true, e.preds = std::move(preds);
    st_.frames += 1;
    MergeCounts(add);
  }
  // `models` = the kept wrestler skeletons the record emits; one with no prediction is "class present, no prediction" (the referee when it is not in the slot table, a skeleton the slot table does not hold).
  void NoteNoPred(const std::vector<Pred>& preds, const std::vector<uint32_t>& models) {
    std::lock_guard<std::mutex> lock(mu_);
    for (uint32_t m : models) {
      bool have = false;
      for (const Pred& p : preds) have = have || p.model == m;
      if (have) continue;
      ++st_.emitted_no_pred;
      char b[48];
      std::snprintf(b, sizeof b, "%08X", m);
      NoteFirst(st_.first_no_pred, b);
    }
  }
  // Called by the lights contributor's Emit for one closed walk frame (`walk_frame` = the hook frame its refs carry).
  template <class StateT, class RefT>
  void OnWalkFrame(uint32_t walk_frame, const StateT* states, uint32_t n_states, const RefT* refs, uint32_t n_refs) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Ent& e : ring_)
      if (e.valid && int64_t(walk_frame) - int64_t(e.hook) >= -1 && int64_t(walk_frame) - int64_t(e.hook) <= 1) CompareStates(e.preds, e.hook, states, n_states, refs, n_refs, st_);
  }
  Stats Get() const {
    std::lock_guard<std::mutex> lock(mu_);
    return st_;
  }
  void AddIndexCompare(const Stats& d) {
    std::lock_guard<std::mutex> lock(mu_);
    for (uint32_t c = 0; c < kClassCount; ++c) {
      st_.idx_eq[c] += d.idx_eq[c], st_.idx_diff[c] += d.idx_diff[c], st_.idx_no_entry[c] += d.idx_no_entry[c], st_.idx_not_walked[c] += d.idx_not_walked[c], st_.blk_eq[c] += d.blk_eq[c], st_.blk_diff[c] += d.blk_diff[c];
    }
    if (d.first_idx[0]) NoteFirst(st_.first_idx, d.first_idx);
  }
  void NoteLatchEntries(uint64_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    st_.latch_entries += n;
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    st_ = Stats{};
    prev_.clear();
    for (Ent& e : ring_) e = Ent{};
    next_ = 0;
  }
  // The census text: `[light_latch frames N, light_index latch==walk E, differ D, no_entry .., not_walked .., first ..., light_state latch==walk E, differ D (delta ...), first ...]`.
  std::string Census() const {
    const Stats s = Get();
    uint64_t ie = 0, id = 0, in = 0, iw = 0, be = 0, bd = 0;
    for (uint32_t c = 0; c < kClassCount; ++c) ie += s.idx_eq[c], id += s.idx_diff[c], in += s.idx_no_entry[c], iw += s.idx_not_walked[c], be += s.blk_eq[c], bd += s.blk_diff[c];
    std::string o = "[light_latch frames " + std::to_string((unsigned long long)s.frames) + ", entries_fed " + std::to_string((unsigned long long)s.latch_entries) + ", no_manager " + std::to_string((unsigned long long)s.no_manager) + ", ring_unnoted " +
                    std::to_string((unsigned long long)s.ring_unnoted);
    o += ", light_index latch==walk " + std::to_string((unsigned long long)ie) + ", differ " + std::to_string((unsigned long long)id) + ", no_entry " + std::to_string((unsigned long long)in) + ", not_walked " + std::to_string((unsigned long long)iw) +
         ", first [" + s.first_idx + "]";
    uint64_t cp = 0;
    for (uint32_t c = 0; c < kClassCount; ++c) cp += s.changed_preds[c];
    o += ", changed_frames " + std::to_string((unsigned long long)s.changed_frames) + " (changed preds " + std::to_string((unsigned long long)cp) + "; block and state are counted on changed preds only: a static light cannot show a stale read), block latch==walk " +
         std::to_string((unsigned long long)be) + " differ " + std::to_string((unsigned long long)bd) + ", emitted_no_pred " + std::to_string((unsigned long long)s.emitted_no_pred) + " [" + s.first_no_pred + "], dup_hooks " + std::to_string((unsigned long long)s.dup_hooks);
    uint64_t se[3] = {}, sd[3] = {};
    for (uint32_t c = 0; c < kClassCount; ++c)
      for (int d = 0; d < 3; ++d) se[d] += s.st_eq[c][d], sd[d] += s.st_diff[c][d];
    o += ", light_state latch==walk " + std::to_string((unsigned long long)(se[0] + se[1] + se[2])) + ", differ " + std::to_string((unsigned long long)(sd[0] + sd[1] + sd[2])) + " (delta -1/0/+1(+1 = the same frame: a LightRef carries the walk's hook frame f, the latch of that frame is hook f-1: J1 kHookToStreamOffset = 1) eq " + std::to_string((unsigned long long)se[0]) + "/" +
         std::to_string((unsigned long long)se[1]) + "/" + std::to_string((unsigned long long)se[2]) + " differ " + std::to_string((unsigned long long)sd[0]) + "/" + std::to_string((unsigned long long)sd[1]) + "/" + std::to_string((unsigned long long)sd[2]) + "), first [" +
         s.first_state + "]";
    for (uint32_t c = 0; c < kClassCount; ++c)
      o += std::string(c ? "; " : ", per class ") + ClassName(c) + " preds " + std::to_string((unsigned long long)s.preds[c]) + " idx " + std::to_string((unsigned long long)s.idx_eq[c]) + "/" + std::to_string((unsigned long long)s.idx_diff[c]) + " no_install " +
           std::to_string((unsigned long long)s.no_install[c]) + " unresolved " + std::to_string((unsigned long long)s.unresolved[c]) + " unreadable " + std::to_string((unsigned long long)s.unreadable[c]) + " changed " + std::to_string((unsigned long long)s.changed_preds[c]) + " block " + std::to_string((unsigned long long)s.blk_eq[c]) + "/" + std::to_string((unsigned long long)s.blk_diff[c]) + " state eq " + std::to_string((unsigned long long)(s.st_eq[c][0] + s.st_eq[c][1] + s.st_eq[c][2])) + " differ " + std::to_string((unsigned long long)(s.st_diff[c][0] + s.st_diff[c][1] + s.st_diff[c][2]));
    return o + "]";
  }

 private:
  struct Ent {
    uint32_t hook = 0;
    bool valid = false;
    std::vector<Pred> preds;
  };
  void MergeCounts(const Stats& a) {
    for (uint32_t c = 0; c < kClassCount; ++c) st_.preds[c] += a.preds[c], st_.no_install[c] += a.no_install[c], st_.unreadable[c] += a.unreadable[c], st_.unresolved[c] += a.unresolved[c];
    st_.ring_unnoted += a.ring_unnoted, st_.no_manager += a.no_manager;
  }
  mutable std::mutex mu_;
  Ent ring_[kKeep];
  std::unordered_map<uint32_t, Pred> prev_;   // the last latch's prediction per model (the baseline of Pred::changed)
  uint32_t next_ = 0;
  Stats st_;
};

inline Board& The() {
  static Board b;
  return b;
}

}  // namespace rdr_scene::live::lightlatch
