// S108 lane THB (docs\research\s108\THB_threads_gear.md): the colours of an entrance PROP (a cap, a hat, a T-shirt, a scarf: the 26 ids with a /SPMT/<id> mask in evtobj.pac), per prop model.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes: the raw hooks (fp_probe.h, weapon_rows_core.h RawHookEntry), the record writer
// (weapon_rows_contrib.h) and the offline unit (tools\scene\thb_unit.cpp) run THIS code.
//
// The game recolours the prop in two places [C, listings; our words]:
//   * THREADS EDITOR (live): the editor's gear object G (E + 1024) installs, on the prop's model every frame, model+604 = 1 (the second effect registry, EDIT/STS: every yProp row is a cc_mask
//     variant), model+324 = 0x8338EC00 (its draw callback) and model+328 = G. At the model's draw the callback feeds PS c16..c19 = the four colours G+128..G+139 (3 bytes each) / 255 and binds the
//     prop's mask [G+108] at sampler 12 (loaded flag [G+100]); G's own state [G+20] is 2 while it does. Nothing is painted into a texture.
//   * SAVED SET in a match entrance: the entrance task copies a 96-byte block into its recolour object through sub_833A1758 (r4 = the block: +0 = the entry-0 model of the made prop, +4 = the prop
//     id, +16 + 16 i / +20 + 16 i / +24 + 16 i = colour i's R / G / B as words), then repaints the prop's texture 0 on the GPU. Ours never runs that GPU pass; the colours are latched at the block.
// Both feed one draw-time row (rows_change_colour.h: ChangeColourRow, kCcProp): the drawer binds the prop's Mask1 at slot 12 and ChangeColor = the 4 colours, on the game's own cc_mask pixel
// variant (the STS table row of the material's technique). Colour i replaces the diffuse where mask channel i (R, G, B, A) is the first non-zero one, scaled by the mask value (T_SSComp's rule).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "change_colour.h"   // Shortfall (the one refusal queue shape)

namespace rdr_scene::live::propcolour {

constexpr uint32_t kCallbackEditorProp = 0x8338EC00u;   // sub_8338EC00: the Threads editor's draw callback of the entrance prop (installed by its render node sub_8338ED08, every frame)
// S111 CACBUILD (docs\research\s111\CACBUILD.md 2): the Create a Championship belt's draw callback (installed by sub_833449E0 / sub_83344760 on [[0x836F4B40]+1228], arg = the CAB object C).
// For a created belt id ([C+228]) it binds at sampler 12 C's own 1x1 texture, zero-filled by the ctor sub_83344240 and never written, and C+236 (samplers 14 / 4) is never set: the game recolours
// nothing, so the plain row IS its picture. Any other id binds its loaded /CBPM mask, which ours does not load: still a shortfall.
constexpr uint32_t kCallbackCabBelt = 0x833445D0u, kOffCabPropId = 228;
inline bool IsCreatedBeltPropId(int32_t id) { return (id >= 2926 && id <= 2930) || (id >= 3926 && id <= 3930) || (id >= 5059 && id <= 5063); }   // sub_83328A48's 15 ids
constexpr uint32_t kSkinnedModelVtable = 0x8206AF90u;   // the model class of MakeWeapon's models (= weapons::kSkinnedModelVtable; the unit asserts the two equal)
constexpr uint32_t kOffSwitch = 604, kOffCallback = 324, kOffCallbackArg = 328;   // model words (as change_colour.h)
constexpr uint32_t kOffState = 20, kOffMaskLoaded = 100, kOffColours = 128;       // gear object G
constexpr uint32_t kStateShowing = 2;                                             // [G+20] while the callback is installed (the render node runs in state 2)
constexpr int kColours = 4, kColourBytes = 12;
constexpr uint32_t kBlockModel = 0, kBlockId = 4, kBlockColours = 16, kBlockStride = 16;   // the match block (sub_833A1758's r4)

// ---- the editor: one draw's read ------------------------------------------------------------------------------------------------------------------------------------------------------------
enum class Read { kOff, kFresh, kNotReady, kForeignCallback, kNoCallback, kUnreadable, kCabPlain /* S111 CACBUILD: the CAB belt callback on a created id: no recolour (counted, quiet) */ };
struct EditorSample {
  Read read = Read::kOff;
  uint32_t callback = 0, gear = 0, state = 0, mask_loaded = 0;
  uint8_t colours[kColourBytes] = {};
};
enum class Fault { kNone, kIgnoreState /* recolour with the gear object not in state 2 */, kAnyCallback /* any callback read as the editor's */, kIgnoreSwitch, kCabAsForeign /* S111 CACBUILD twin: the CAB callback read as foreign */ };
inline Fault& PlantForTest() {
  static Fault f = Fault::kNone;
  return f;
}
template <class ReadWordFn>
inline EditorSample ReadEditor(ReadWordFn rw, uint32_t model, Fault fault = PlantForTest()) {
  EditorSample s;
  uint32_t sw = 0, cb = 0, g = 0;
  if (!rw(model + kOffSwitch, sw)) return s.read = Read::kUnreadable, s;
  if (sw == 0 && fault != Fault::kIgnoreSwitch) return s.read = Read::kOff, s;
  if (!rw(model + kOffCallback, cb) || !rw(model + kOffCallbackArg, g)) return s.read = Read::kUnreadable, s;
  s.callback = cb;
  if (cb == 0) return s.read = Read::kNoCallback, s;
  if (cb == kCallbackCabBelt && fault != Fault::kCabAsForeign) {   // S111 CACBUILD: decided by the prop id the CAB object holds
    uint32_t id = 0;
    if (g < 0x10000u || !rw(g + kOffCabPropId, id)) return s.read = Read::kUnreadable, s;
    s.gear = g;
    return s.read = IsCreatedBeltPropId(int32_t(id)) ? Read::kCabPlain : Read::kForeignCallback, s;
  }
  if (cb != kCallbackEditorProp && fault != Fault::kAnyCallback) return s.read = Read::kForeignCallback, s;
  if (g < 0x10000u) return s.read = Read::kUnreadable, s;
  s.gear = g;
  if (!rw(g + kOffState, s.state) || !rw(g + kOffMaskLoaded, s.mask_loaded)) return s.read = Read::kUnreadable, s;
  if ((s.state != kStateShowing && fault != Fault::kIgnoreState) || s.mask_loaded == 0) return s.read = Read::kNotReady, s;   // the callback binds no mask: the game draws it undefined, we draw it plain (counted)
  for (int i = 0; i < kColourBytes / 4; ++i) {
    uint32_t w = 0;
    if (!rw(g + kOffColours + 4u * uint32_t(i), w)) return s.read = Read::kUnreadable, s;
    for (int b = 0; b < 4; ++b) s.colours[4 * i + b] = uint8_t(w >> (24 - 8 * b));   // big-endian word -> bytes in memory order
  }
  return s.read = Read::kFresh, s;
}

// ---- the match: the block of sub_833A1758 ------------------------------------------------------------------------------------------------------------------------------------------------------
enum class Refusal { kNone, kUnreadable, kNotModel, kNotEventProp, kIdMismatch, kColourRange };
inline const char* RefusalName(Refusal r) {
  switch (r) {
    case Refusal::kNone: return "none";
    case Refusal::kUnreadable: return "unreadable";
    case Refusal::kNotModel: return "model word is not a model";
    case Refusal::kNotEventProp: return "model is no event prop";
    case Refusal::kIdMismatch: return "block id differs from the model's prop id";
    case Refusal::kColourRange: return "a colour word above 255";
  }
  return "?";
}
struct Block {
  Refusal refusal = Refusal::kNone;
  uint32_t model = 0;
  int32_t id = -1;
  uint8_t colours[kColourBytes] = {};
};
// `find(model, id&)` = EventProps::Find (the prop id MakeWeapon registered for that model; false = not an event prop). A block that fails any check is NOT trusted (counted, ignored).
enum class BlockFault { kNone, kNoChecks /* every check skipped: the block is trusted as it stands */ };
inline BlockFault& BlockPlantForTest() {
  static BlockFault f = BlockFault::kNone;
  return f;
}
template <class ReadWordFn, class FindFn>
inline Block ReadBlock(ReadWordFn rw, FindFn find, uint32_t block) {
  Block b;
  const bool checks = BlockPlantForTest() != BlockFault::kNoChecks;
  uint32_t model = 0, id = 0, vt = 0;
  if (block < 0x10000u || !rw(block + kBlockModel, model) || !rw(block + kBlockId, id)) return b.refusal = Refusal::kUnreadable, b;
  b.model = model, b.id = int32_t(id);
  if (checks) {
    if (model < 0x10000u || !rw(model, vt) || vt != kSkinnedModelVtable) return b.refusal = Refusal::kNotModel, b;
    int32_t found = -1;
    if (!find(model, found)) return b.refusal = Refusal::kNotEventProp, b;
    if (found != int32_t(id)) return b.refusal = Refusal::kIdMismatch, b;
  }
  for (int i = 0; i < kColours; ++i)
    for (int c = 0; c < 3; ++c) {
      uint32_t w = 0;
      if (!rw(block + kBlockColours + kBlockStride * uint32_t(i) + 4u * uint32_t(c), w)) return b.refusal = Refusal::kUnreadable, b;
      if (checks && w > 255u) return b.refusal = Refusal::kColourRange, b;
      b.colours[3 * i + c] = uint8_t(w);
    }
  return b;
}

// The body of the raw hook on sub_833A1758 (fp_probe.h), before the original runs: the block -> the checks -> the latch. The unit drives this very function.
template <class ReadWordFn, class FindFn>
inline void OnRecolourBlock(ReadWordFn rw, FindFn find, uint32_t block);   // (defined after Store, below)

// The 4 ChangeColor constants the shader takes: (R, G, B, 1) / 255 (the callback's rule; the same as change_colour.h ToConstants for the first 4 of 16).
inline void ToConstants(const uint8_t colours[kColourBytes], float out[kColours * 4]) {
  for (int i = 0; i < kColours; ++i) {
    out[4 * i + 0] = float(colours[3 * i + 0]) * (1.0f / 255.0f);
    out[4 * i + 1] = float(colours[3 * i + 1]) * (1.0f / 255.0f);
    out[4 * i + 2] = float(colours[3 * i + 2]) * (1.0f / 255.0f);
    out[4 * i + 3] = 1.0f;
  }
}

struct Census {
  uint64_t latched = 0, editor = 0, editor_off = 0, not_ready = 0, foreign = 0, no_callback = 0, unreadable = 0, cab_plain = 0;
  uint64_t refused[6] = {};          // by Refusal
  uint64_t rows = 0, no_mask = 0, no_row = 0, drawn = 0, forgotten = 0, aged = 0;
};
struct Out {
  uint8_t colours[kColourBytes] = {};
  bool editor = false;   // the Threads editor's draw (MatDifCol 1); false = a saved set (the match)
};

// Per prop model. Written by the game thread (the editor read at the draw ask; the match latch at sub_833A1758), read by the record writer. Entries leave with the model (the hooks of weapon_rows_core.h
// EventProps and the model destroy hook), and an editor entry also when no draw refreshed it for kAgeFrames frames (the editor stopped showing the prop).
class Store {
 public:
  static constexpr uint64_t kAgeFrames = 60;
  void Latch(const Block& b) {
    std::lock_guard<std::mutex> l(mu_);
    if (b.refusal != Refusal::kNone) {
      ++c_.refused[int(b.refusal)];
      char obj[96], det[160];
      std::snprintf(obj, sizeof obj, "prop block model 0x%08X id %d: %s", b.model, int(b.id), RefusalName(b.refusal));
      std::snprintf(det, sizeof det, "the saved set's gear colours are not applied (the block failed a check)");
      QueueLocked("prop_colour", obj, det);
      return;
    }
    Entry& e = map_[b.model];
    std::memcpy(e.match, b.colours, kColourBytes);
    e.id = b.id, e.have_match = true;
    ++c_.latched;
  }
  // One draw ask of an event prop model (game thread).
  void NoteEditor(uint32_t model, uint64_t frame, const EditorSample& s) {
    std::lock_guard<std::mutex> l(mu_);
    switch (s.read) {
      case Read::kOff: ++c_.editor_off; break;
      case Read::kFresh: ++c_.editor; break;
      case Read::kNotReady: ++c_.not_ready; break;
      case Read::kForeignCallback: ++c_.foreign; break;
      case Read::kNoCallback: ++c_.no_callback; break;
      case Read::kUnreadable: ++c_.unreadable; break;
      case Read::kCabPlain: ++c_.cab_plain; break;
    }
    // A foreign callback or an unreadable read: the game recolours this prop and we cannot, said once per (model, reason). The switch set
    // with NO callback is not one: the gear object's update clears +324 / +328 every pass and its render node re-installs them only in
    // state 2 (sub_8338E810, sub_8338ED08), so in the frames of a gear change the game sends no colours either. Counted, never an alarm.
    if (s.read == Read::kForeignCallback || s.read == Read::kUnreadable) {
      char obj[96], det[160];
      std::snprintf(obj, sizeof obj, "prop model 0x%08X: %s", model, s.read == Read::kForeignCallback ? "foreign callback" : "unreadable");
      std::snprintf(det, sizeof det, "switch model+604 set, callback 0x%08X, gear object 0x%08X: drawn with its plain row", s.callback, s.gear);
      QueueLocked("prop_colour", obj, det);
    }
    auto it = map_.find(model);
    if (s.read != Read::kFresh) {   // not the editor's colours this frame: the editor part goes (a saved set's latch, if any, stays)
      if (it != map_.end()) {
        it->second.have_editor = false;
        if (!it->second.have_match) map_.erase(it);
      }
      return;
    }
    Entry& e = map_[model];
    std::memcpy(e.editor, s.colours, kColourBytes);
    e.have_editor = true, e.editor_frame = frame;
  }
  // The record side: the colours the game draws this prop with, or nothing.
  bool Get(uint32_t model, uint64_t now, Out& out) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = map_.find(model);
    if (it == map_.end()) return false;
    Entry& e = it->second;
    if (e.have_editor && now > e.editor_frame + kAgeFrames) {
      e.have_editor = false, ++c_.aged;
      if (!e.have_match) return map_.erase(it), false;
    }
    if (e.have_editor) return std::memcpy(out.colours, e.editor, kColourBytes), out.editor = true, true;
    if (e.have_match) return std::memcpy(out.colours, e.match, kColourBytes), out.editor = false, true;
    return false;
  }
  void ForgetModel(uint32_t model) {   // the game destroyed / re-made / freed this model: its address may be reused by another object
    std::lock_guard<std::mutex> l(mu_);
    if (map_.erase(model)) ++c_.forgotten;
  }
  void Clear() {
    std::lock_guard<std::mutex> l(mu_);
    c_.forgotten += map_.size();
    map_.clear();
  }
  void NoteRow() { std::lock_guard<std::mutex> l(mu_); ++c_.rows; }
  void NoteDrawn() { std::lock_guard<std::mutex> l(mu_); ++c_.drawn; }
  void NoteNoRow() { std::lock_guard<std::mutex> l(mu_); ++c_.no_row; }   // the STS table has no bindable cc_mask row for this material's effect / technique: drawn stock
  void NoteNoMask() { std::lock_guard<std::mutex> l(mu_); ++c_.no_mask; }   // a prop draw the record recolours whose material has no mask loaded (no /SPMT row, or the file lacks it; S111 STOCKBELT: a belt material not on `color`, which R5 leaves alone)
  void TakeShortfalls(std::vector<change_colour::Shortfall>& out) {
    std::lock_guard<std::mutex> l(mu_);
    out.swap(pending_);
    pending_.clear();
  }
  size_t Size() { std::lock_guard<std::mutex> l(mu_); return map_.size(); }
  Census Counts() { std::lock_guard<std::mutex> l(mu_); return c_; }
  std::string CensusLine() {
    const Census c = Counts();
    uint64_t refused = 0;
    for (int i = 1; i < 6; ++i) refused += c.refused[i];
    char b[560];
    std::snprintf(b, sizeof b, "[prop_colour] latched %llu, editor %llu (off %llu, not ready %llu), drawn %llu (rows %llu), no mask %llu, no row %llu, shortfall %llu (foreign %llu, no callback %llu, unreadable %llu, blocks refused %llu), held %zu, forgotten %llu, aged %llu, cab plain %llu",
                  (unsigned long long)c.latched, (unsigned long long)c.editor, (unsigned long long)c.editor_off, (unsigned long long)c.not_ready, (unsigned long long)c.drawn, (unsigned long long)c.rows,
                  (unsigned long long)c.no_mask, (unsigned long long)c.no_row, (unsigned long long)(c.foreign + c.no_callback + c.unreadable + refused), (unsigned long long)c.foreign, (unsigned long long)c.no_callback,
                  (unsigned long long)c.unreadable, (unsigned long long)refused, Size(), (unsigned long long)c.forgotten, (unsigned long long)c.aged, (unsigned long long)c.cab_plain);
    return b;
  }
  void ResetForTest() {
    std::lock_guard<std::mutex> l(mu_);
    map_.clear(), c_ = Census{}, pending_.clear(), reported_.clear();
  }

 private:
  struct Entry {
    bool have_match = false, have_editor = false;
    int32_t id = -1;
    uint8_t match[kColourBytes] = {}, editor[kColourBytes] = {};
    uint64_t editor_frame = 0;
  };
  static constexpr size_t kMaxKeys = 4096, kMaxPending = 64;   // (memory only: the draw_shortfall sink keeps its own bound)
  void QueueLocked(const char* kind, const std::string& object, const std::string& detail) {
    if (reported_.size() >= kMaxKeys || pending_.size() >= kMaxPending) return;
    if (!reported_.insert(std::string(kind) + '\n' + object).second) return;
    pending_.push_back(change_colour::Shortfall{kind, object, detail});
  }
  std::mutex mu_;
  std::map<uint32_t, Entry> map_;
  Census c_;
  std::set<std::string> reported_;
  std::vector<change_colour::Shortfall> pending_;
};

inline Store& TheStore() {
  static Store s;
  return s;
}

template <class ReadWordFn, class FindFn>
inline void OnRecolourBlock(ReadWordFn rw, FindFn find, uint32_t block) { TheStore().Latch(ReadBlock(rw, find, block)); }

// The call sites of weapon_rows_core.h EventProps (beside beltplate::DropOnMake / DropOnFree / ClearOnScene) and of the model destroy hook. Planted twins of the unit skip them.
enum class Plant { kNone, kMakeNoDrop, kFreeNoDrop };
inline Plant& DropPlantForTest() {
  static Plant p = Plant::kNone;
  return p;
}
inline void DropOnMake(uint32_t model) { if (DropPlantForTest() != Plant::kMakeNoDrop) TheStore().ForgetModel(model); }
inline void DropOnFree(uint32_t model) { if (DropPlantForTest() != Plant::kFreeNoDrop) TheStore().ForgetModel(model); }
inline void ClearOnScene() { TheStore().Clear(); }

}  // namespace rdr_scene::live::propcolour
