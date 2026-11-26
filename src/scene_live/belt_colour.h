// S111 STOCKBELT + BELTMASKNAME (docs\research\s111\STOCKBELT.md, docs\research\s111\BELTMASKNAME.md): read-back R5, the game's recolour of a STOCK belt's own texture for a CREATED title, served as the THB draw-time row.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes: the raw hooks (fp_probe.h), the record writer (weapon_rows_contrib.h), the drawer (rf_props.cpp), the loader
// (live_assets.cpp) and the offline unit (tools\scene\stockbelt_unit.cpp) run THIS code.
//
// The game [C, listings; our words]: a title of kind 3 (created, sub_82255398) whose record +8 is NOT one of the created belt rows 86..90 (sub_822553E0) is drawn on a stock belt id, and its four colours are
// painted into that belt model's texture key 0 (sub_83327840 / sub_82BD4F60: key slot 0 of [model+260]) by the R5 composer (sub_83327F00: T_EditCopy, then T_SSComp through ONE mask, per mip; sub_83327A08 reads it
// back and re-encodes DXT5 into the same texture). Every start hands the composer (r3) a 96-byte block (r4) through sub_83327660: +0 the belt model, +4 the id, +8 the mode, colour k = words +16 + 16 k / +20 / +24.
//   mode 0: the entrance belt (CAB prop object state 1, sub_83324840; r3 = the object + 192): model = entry 0 of the CWeapon its own MakeWeapon made, id = the prop id, mask /CBPM/<prop id>.
//   mode 1: the match weapon belt (sub_8258DC70, from the weapon data load sub_825910D8 +C8; r3 = the weapon manager + 3168): model = the main model of the FIRST slot i < 48 with a main model and id 14
//           (+3C..+50: [mgr+848+4i] != 0, [mgr+388+4i] == 14), id = the title's +8 (= the m2 belt sub-key), mask /CBWM/<id> (256 x 256).
//   mode 2: the ladder rig's belt (sub_82576B98, from the member make sub_825771E8 +F4 for holder type 4, member 1; r3 = the holder + 192): model = [H+80] = member 1's model, made by the same call just before
//           (sub_825771E8 +60..+64 writes [H+76+4k]), id = the title's +8, mask /CBLM/<id> (512 x 512).
// Ours: the GPU pass and the read-back are a 360 habit (the LOOK is the recoloured diffuse); T_SSComp's rule is the cc_mask pixel variant at draw time (THB). ONE mechanism for the three modes:
//   1. the hook (OnBeltBlock) reads the block and checks the model IS its owner's belt by the game's own rule for that mode (read at the block), then keeps (mode, id, colours) per model (TheStore);
//   2. the record writer gives every call of that model ONE section-67 row (RowOf: kCcProp | kCcBeltMask; `reserved` = the mask's NAME, mode << 16 | id), for the call kind that mode draws;
//   3. the drawer asks the loader for the named mask (pac\cab\CAB.pac entry "beltmask:<mode>/<id>": a stored model holding that ONE texture, read by blocks, belt_mask.h) and binds it on the belt's bound
//      copy, on the materials whose diffuse is texture key 0 (BindBeltMask). Only the masks this session's created titles name are ever read; no stock belt model carries a mask otherwise.
// Lifetimes = the models': mode 0 leaves with its event prop (weapon_rows_core.h EventProps: make / free / prune; a scene clear drops mode 0 only), mode 1 at the slot free sub_82590ED8 (it frees every slot model;
// sub_825910D8 calls it at +44 before it re-makes the slots and re-sends the block at +C8), mode 2 at the member make sub_825771E8 (it re-makes every member, then re-sends the block for member 1).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../assets/asset_model.h"                  // Model / Material / Texture (BindBeltMask; pure)
#include "../renderer_scene/rows_change_colour.h"   // ChangeColourRow, kCcProp, kCcBeltMask
#include "../renderer_scene/weapon_rows.h"          // WeaponCall flags, slot table offsets, the stage holder's words
#include "prop_colour.h"                            // kBlock* (the THB block layout = R5's, byte for byte), kSkinnedModelVtable, change_colour::Shortfall

namespace rdr_scene::live::beltcolour {

constexpr uint32_t kBlockMode = 8;   // sub_833275E8: block +0 = model, +4 = id, +8 = mode
enum class Mode : uint32_t { kEntranceProp = 0, kMatchWeapon = 1, kLadder = 2 };
constexpr uint32_t kModes = 3;
// Where each caller's composer sits in its owner (the r3 of its sub_83327660 call) [C]: sub_83324840 +120 (prop + 192), sub_8258DC70 +1BC (manager + 3168), sub_82576B98 +17C (holder + 192).
constexpr uint32_t kComposerInManager = 3168, kComposerInHolder = 192;
constexpr uint32_t kBeltWeaponId = 14, kManagerSlotScan = 48;   // sub_8258DC70 +3C..+60: the first slot i < 48 whose main model is set and whose id is 14
constexpr uint32_t kHolderOffBelt = 80;                         // sub_825771E8 +F0: r4 = [H+80] (member 1 of the [H+76+4k] models)
static_assert(kHolderOffBelt == rdr_scene::weapons::kHolderOffModel + 4, "the ladder belt is member k 1 of the holder's models");
static_assert(kManagerSlotScan == rdr_scene::weapons::kMaxSlots, "sub_8258DC70 scans the slot table's 48 slots");
inline constexpr const char* kMaskPac = "pac\\cab\\CAB.pac";   // = assets::kCabPac (the unit asserts the two equal)

// The mask's NAME (the row's `reserved` word) and the loader's entry for it.
inline const char* MaskSection(uint32_t mode) { return mode == 0 ? "CBPM" : mode == 1 ? "CBWM" : mode == 2 ? "CBLM" : nullptr; }   // sub_83331E58's three paths (STOCKBELT 2)
inline uint32_t MaskName(uint32_t mode, uint32_t id) { return (mode << 16) | (id & 0xFFFFu); }
inline uint32_t ModeOfName(uint32_t name) { return name >> 16; }
inline uint32_t IdOfName(uint32_t name) { return name & 0xFFFFu; }
inline std::string MaskEntryOf(uint32_t name) { return "beltmask:" + std::to_string(ModeOfName(name)) + "/" + std::to_string(IdOfName(name)); }
inline bool ParseMaskEntry(const std::string& entry, uint32_t& mode, uint32_t& id) {
  unsigned m = 0, i = 0;
  char tail = 0;
  if (std::sscanf(entry.c_str(), "beltmask:%u/%u%c", &m, &i, &tail) != 2 || m >= kModes || i > 0xFFFFu) return false;
  return mode = m, id = i, true;
}

// Planted twins of the old / wrong rules (the unit's --plant). kNoLatch = nothing latched (the tip before STOCKBELT); kEntranceOnly = modes 1 / 2 counted only (the STOCKBELT tip);
// kNoOwnerCheck = the block's model trusted without its owner's check; kNoDrop = the slot free / member make / scene clear drop nothing; kKindAny = a latch gives a row to ANY call of its model.
// S111 BELTMASKNAME review: kAskAllModes = mode 1 asks, loads and binds its mask although no STS row can draw it (the landed head, F5); kNoPendingMark = a row waiting for its mask draws in the plain pool's name (F3).
enum class Fault { kNone, kNoLatch, kEntranceOnly, kNoOwnerCheck, kNoDrop, kKindAny, kAskAllModes, kNoPendingMark };
inline Fault& PlantForTest() {
  static Fault f = Fault::kNone;
  return f;
}

// S111 BELTMASKNAME review (F5): the modes the drawer can draw recoloured today. Mode 1's m2 belts are yProp_Default T0, which EDIT/STS has no row for (stockbelt_unit real.cbwm.known_gap_no_sts_row checks
// this switch against the player's files): its block is latched, rowed and counted (`not drawable`), but no mask is asked, loaded or bound and no masked copy is made. The R4 lane (a host compose of k_color) turns it on.
inline constexpr bool kModeDrawable[kModes] = {true, false, true};
inline bool AsksMask(const rdr_scene::ChangeColourRow* row) {   // the drawer (rf_props.cpp): ask the loader for this row's mask
  if (!row) return false;
  const uint32_t m = ModeOfName(row->reserved);
  return m < kModes && (kModeDrawable[m] || PlantForTest() == Fault::kAskAllModes);
}
// The pool name's belt part: `~cb<mode>.<id>` once the mask is resident (a masked belt never shares a bound copy with its plain one); `~cb-` while the row waits for it (review F3), so the pool it drew in
// meanwhile is never a prefix of the masked pool it moves to and PoolTrack (belt_plate.h) retires it the frame after.
inline std::string PoolSuffix(uint32_t name, bool resident = true) {
  if (!resident) return PlantForTest() == Fault::kNoPendingMark ? std::string() : std::string("~cb-");
  return "~cb" + std::to_string(ModeOfName(name)) + "." + std::to_string(IdOfName(name));
}

enum class Refusal { kNone, kUnreadable, kMode, kNotModel, kNotEventProp, kIdMismatch, kNotOwnersBelt, kColourRange };
constexpr int kRefusals = 8;
inline const char* RefusalName(Refusal r) {
  switch (r) {
    case Refusal::kNone: return "none";
    case Refusal::kUnreadable: return "unreadable";
    case Refusal::kMode: return "mode word above 2";
    case Refusal::kNotModel: return "model word is not a model";
    case Refusal::kNotEventProp: return "mode 0 model is no event prop";
    case Refusal::kIdMismatch: return "mode 0 block id differs from the model's prop id";
    case Refusal::kNotOwnersBelt: return "the model is not its owner's belt (mode 1: the first id-14 slot's main; mode 2: holder member 1)";
    case Refusal::kColourRange: return "a colour word above 255";
  }
  return "?";
}
struct Latched {
  uint32_t mode = 0, id = 0;
  uint8_t colours[propcolour::kColourBytes] = {};
};
struct Block {
  Refusal refusal = Refusal::kNone;
  uint32_t model = 0;
  Latched v;
};

// The block of sub_83327660 (r3 = `composer`, r4 = `block`) -> the checks of its mode. `find(model, id&)` = EventProps::Find (mode 0: the prop id MakeWeapon registered for that model).
template <class ReadWordFn, class FindFn>
inline Block ReadBlock(ReadWordFn rw, FindFn find, uint32_t composer, uint32_t block) {
  namespace w = rdr_scene::weapons;
  Block b;
  uint32_t model = 0, id = 0, mode = 0;
  if (block < 0x10000u || !rw(block + propcolour::kBlockModel, model) || !rw(block + propcolour::kBlockId, id) || !rw(block + kBlockMode, mode)) return b.refusal = Refusal::kUnreadable, b;
  b.model = model, b.v.mode = mode, b.v.id = id;
  if (mode >= kModes) return b.refusal = Refusal::kMode, b;
  const bool check = PlantForTest() != Fault::kNoOwnerCheck;
  if (check) {
    if (model < 0x10000u) return b.refusal = Refusal::kNotModel, b;
    if (mode == uint32_t(Mode::kEntranceProp)) {   // the THB checks (prop_colour.h ReadBlock): a CSkinModel that MakeWeapon registered with this very prop id
      uint32_t vt = 0;
      if (!rw(model, vt) || vt != propcolour::kSkinnedModelVtable) return b.refusal = Refusal::kNotModel, b;
      int32_t found = -1;
      if (!find(model, found)) return b.refusal = Refusal::kNotEventProp, b;
      if (found != int32_t(id)) return b.refusal = Refusal::kIdMismatch, b;
    } else if (mode == uint32_t(Mode::kMatchWeapon)) {   // sub_8258DC70's own pick, re-read: the first slot with a main model and id 14
      if (composer < kComposerInManager + 0x10000u) return b.refusal = Refusal::kNotOwnersBelt, b;
      const uint32_t mgr = composer - kComposerInManager;
      bool is = false;
      for (uint32_t i = 0; i < kManagerSlotScan; ++i) {
        uint32_t main = 0, sid = 0;
        if (!rw(mgr + w::kOffSlotMain + 4u * i, main) || !rw(mgr + w::kOffSlotId + 4u * i, sid)) return b.refusal = Refusal::kUnreadable, b;
        if (main != 0 && sid == kBeltWeaponId) { is = main == model; break; }
      }
      if (!is) return b.refusal = Refusal::kNotOwnersBelt, b;
    } else {   // mode 2: the holder's member 1, as sub_825771E8 passed it
      if (composer < kComposerInHolder + 0x10000u) return b.refusal = Refusal::kNotOwnersBelt, b;
      uint32_t m1 = 0;
      if (!rw(composer - kComposerInHolder + kHolderOffBelt, m1)) return b.refusal = Refusal::kUnreadable, b;
      if (m1 != model) return b.refusal = Refusal::kNotOwnersBelt, b;
    }
  }
  for (int i = 0; i < propcolour::kColours; ++i)
    for (int c = 0; c < 3; ++c) {
      uint32_t wd = 0;
      if (!rw(block + propcolour::kBlockColours + propcolour::kBlockStride * uint32_t(i) + 4u * uint32_t(c), wd)) return b.refusal = Refusal::kUnreadable, b;
      if (check && wd > 255u) return b.refusal = Refusal::kColourRange, b;
      b.v.colours[3 * i + c] = uint8_t(wd);
    }
  return b;
}

struct Census {
  uint64_t blocks[kModes] = {}, other_mode = 0, latched[kModes] = {}, refused[kRefusals] = {};
  uint64_t rows = 0, kind_miss = 0, mask_asks = 0, mask_pending = 0, undrawable = 0, dropped[kModes] = {}, forgotten = 0;
};

// Per belt model. Written by the game threads (the hooks), read by the record writer (RowOf). Entries leave with the model (the lifetimes above).
class Store {
 public:
  void Latch(const Block& b) {
    std::lock_guard<std::mutex> l(mu_);
    if (b.refusal != Refusal::kNone) {
      ++c_.refused[int(b.refusal)];
      char obj[96], det[200];
      std::snprintf(obj, sizeof obj, "belt block mode %u model 0x%08X id %u: %s", b.v.mode, b.model, b.v.id, RefusalName(b.refusal));
      std::snprintf(det, sizeof det, "a created title's colours on this stock belt are not drawn (the R5 block failed a check)");
      Queue(obj, det);
      return;
    }
    map_[b.model] = b.v;
    if (b.v.mode < kModes) ++c_.latched[b.v.mode];
    live_.store(map_.size(), std::memory_order_relaxed);
  }
  bool Empty() const { return live_.load(std::memory_order_relaxed) == 0; }   // the record writer's per-call early out (no lock while no created title is on a stock belt)
  bool Get(uint32_t model, Latched& out) const {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = map_.find(model);
    if (it == map_.end()) return false;
    return out = it->second, true;
  }
  void ForgetModel(uint32_t model) {
    std::lock_guard<std::mutex> l(mu_);
    if (map_.erase(model)) ++c_.forgotten;
    live_.store(map_.size(), std::memory_order_relaxed);
  }
  void DropMode(Mode m) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = map_.begin(); it != map_.end();)
      if (it->second.mode == uint32_t(m)) it = map_.erase(it), ++c_.dropped[uint32_t(m)];
      else ++it;
    live_.store(map_.size(), std::memory_order_relaxed);
  }
  void NoteBlock(uint32_t mode) {
    std::lock_guard<std::mutex> l(mu_);
    if (mode < kModes) ++c_.blocks[mode];
    else ++c_.other_mode;
  }
  void NoteRow() { std::lock_guard<std::mutex> l(mu_); ++c_.rows; }
  void NoteKindMiss() { std::lock_guard<std::mutex> l(mu_); ++c_.kind_miss; }
  void NoteMaskAsk(bool resident) { std::lock_guard<std::mutex> l(mu_); ++c_.mask_asks; c_.mask_pending += resident ? 0u : 1u; }   // the drawer (render thread)
  void NoteUndrawable() { std::lock_guard<std::mutex> l(mu_); ++c_.undrawable; }   // the drawer: a row of a mode kModeDrawable keeps off (drawn plain, no mask asked)
  void TakeShortfalls(std::vector<change_colour::Shortfall>& out) {   // APPENDS (rf_props.cpp drains prop_colour's first)
    std::lock_guard<std::mutex> l(mu_);
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
  }
  size_t Size() const { std::lock_guard<std::mutex> l(mu_); return map_.size(); }
  Census Counts() const { std::lock_guard<std::mutex> l(mu_); return c_; }
  void ResetForTest() {
    std::lock_guard<std::mutex> l(mu_);
    map_.clear(), c_ = Census{}, pending_.clear(), reported_.clear();
    live_.store(0, std::memory_order_relaxed);
  }

 private:
  void Queue(const std::string& object, const std::string& detail) {   // mu_ held; once per (object), bounded
    if (reported_.size() >= 256 || pending_.size() >= 64 || !reported_.insert(object).second) return;
    pending_.push_back(change_colour::Shortfall{"belt_colour", object, detail});
  }
  mutable std::mutex mu_;
  std::map<uint32_t, Latched> map_;
  std::atomic<size_t> live_{0};
  Census c_;
  std::set<std::string> reported_;
  std::vector<change_colour::Shortfall> pending_;
};
inline Store& TheStore() {
  static Store s;
  return s;
}

// The body of the raw hook on sub_83327660, before the original runs. `rw` = a guest word read, `find` = EventProps::Find (as the THB hook).
template <class ReadWordFn, class FindFn>
inline void OnBeltBlock(ReadWordFn rw, FindFn find, uint32_t composer, uint32_t block) {
  const Block b = ReadBlock(rw, find, composer, block);
  TheStore().NoteBlock(b.refusal == Refusal::kUnreadable && b.model == 0 ? kModes : b.v.mode);
  const Fault f = PlantForTest();
  if (f == Fault::kNoLatch) return;
  if (f == Fault::kEntranceOnly && b.v.mode != uint32_t(Mode::kEntranceProp)) return;
  TheStore().Latch(b);
}

// The lifetimes (call sites: weapon_rows_core.h EventProps for mode 0; fp_probe.h's raw hooks on sub_82590ED8 / sub_825771E8 for modes 1 / 2).
inline void DropOnMake(uint32_t model) { if (PlantForTest() != Fault::kNoDrop) TheStore().ForgetModel(model); }   // a new event model at this address: the old latch (any mode) is not its
inline void DropOnFree(uint32_t model) { if (PlantForTest() != Fault::kNoDrop) TheStore().ForgetModel(model); }
inline void ClearOnScene() { if (PlantForTest() != Fault::kNoDrop) TheStore().DropMode(Mode::kEntranceProp); }   // the event list's scene clear: entrance belts only (modes 1 / 2 keep their own lifetimes)
inline void DropOnSlotFree() { if (PlantForTest() != Fault::kNoDrop) TheStore().DropMode(Mode::kMatchWeapon); }   // sub_82590ED8: every slot model is freed
inline void DropOnMemberMake() { if (PlantForTest() != Fault::kNoDrop) TheStore().DropMode(Mode::kLadder); }     // sub_825771E8: every holder member is made again

// Which call of the latched model is the belt the game recoloured: mode 0 an event prop call, mode 1 the weapon manager's belt main model (a belt id, not a secondary), mode 2 the stage holder's member k 1.
inline bool KindFits(uint32_t mode, const rdr_scene::weapons::WeaponCall& r) {
  namespace w = rdr_scene::weapons;
  const uint32_t f = r.slot_flags;
  if (mode == uint32_t(Mode::kEntranceProp)) return w::IsEventCall(f);
  if (mode == uint32_t(Mode::kMatchWeapon)) return !w::IsEventCall(f) && !w::IsStageCall(f) && !w::IsGmCall(f) && !w::IsBloodCall(f) && (f & w::kCallSecondary) == 0 && w::BeltSubOfId(r.id) >= 0;
  if (mode == uint32_t(Mode::kLadder)) return w::IsStageCall(f) && w::StageEntry(f) == 1;
  return false;
}
// The record writer (weapon_rows_contrib.h Emit): the section-67 row of this call's model, false = none (no latch, or the latch is not for this kind of call: counted).
inline bool RowOf(const rdr_scene::weapons::WeaponCall& r, rdr_scene::ChangeColourRow& row) {
  Latched v;
  if (TheStore().Empty() || !TheStore().Get(r.object, v)) return false;
  if (!KindFits(v.mode, r) && PlantForTest() != Fault::kKindAny) return TheStore().NoteKindMiss(), false;
  row = rdr_scene::ChangeColourRow();
  row.object = r.object;
  row.flags = rdr_scene::kCcProp | rdr_scene::kCcBeltMask;
  std::memcpy(row.colours, v.colours, propcolour::kColourBytes);
  row.reserved = MaskName(v.mode, v.id);
  TheStore().NoteRow();
  return true;
}
// The drawer (rf_props.cpp): the belt row of a call's object, null = none.
inline const rdr_scene::ChangeColourRow* BeltRowOf(const std::vector<rdr_scene::ChangeColourRow>& rows, uint32_t object) {
  for (const rdr_scene::ChangeColourRow& r : rows)
    if (r.object == object && (r.flags & rdr_scene::kCcBeltMask) != 0) return &r;
  return nullptr;
}
// R5 recolours texture key 0 of the model and nothing else (sub_83327840 / sub_82BD4F60): the materials whose diffuse is the loader's texture 0 (the name table's first: `color` on every evtobj belt and every
// /STGL ladder belt, `k_color` on the m2 belts; stockbelt_unit real.*) name the mask as change_mask[0]; the others keep none. The mask is one more texture of the BOUND copy. Returns the materials named; 0 = nothing added.
inline int BindBeltMask(assets::Model& m, const assets::Texture& mask) {
  int named = 0;
  for (const assets::Material& mat : m.materials) named += mat.diffuse == 0;
  if (named == 0 || m.textures.empty()) return 0;
  const int index = int(m.textures.size());
  for (assets::Material& mat : m.materials)
    if (mat.diffuse == 0) mat.change_mask[0] = index;
  m.textures.push_back(mask);
  return named;
}

// `[belt_colour] blocks: entrance E, match weapon W, ladder L (other O); latched E' W' L'; refused R (...); rows N, kind misses K; masks asked A (not resident P), not drawable U; held H; dropped: entrance D0, slot free D1, member make D2, freed F`
inline std::string CensusLine() {
  const Census c = TheStore().Counts();
  uint64_t refused = 0;
  for (int i = 1; i < kRefusals; ++i) refused += c.refused[i];
  char b[640];
  std::snprintf(b, sizeof b,
                "[belt_colour] blocks: entrance %llu, match weapon %llu, ladder %llu (other %llu); latched %llu %llu %llu; refused %llu (unreadable %llu, not owner's belt %llu, not event prop %llu, id %llu); rows %llu, kind misses %llu; masks asked %llu (not resident %llu), not drawable %llu; held %zu; dropped: entrance %llu, slot free %llu, member make %llu, freed %llu",
                (unsigned long long)c.blocks[0], (unsigned long long)c.blocks[1], (unsigned long long)c.blocks[2], (unsigned long long)c.other_mode, (unsigned long long)c.latched[0], (unsigned long long)c.latched[1],
                (unsigned long long)c.latched[2], (unsigned long long)refused, (unsigned long long)c.refused[int(Refusal::kUnreadable)], (unsigned long long)c.refused[int(Refusal::kNotOwnersBelt)],
                (unsigned long long)c.refused[int(Refusal::kNotEventProp)], (unsigned long long)c.refused[int(Refusal::kIdMismatch)], (unsigned long long)c.rows, (unsigned long long)c.kind_miss,
                (unsigned long long)c.mask_asks, (unsigned long long)c.mask_pending, (unsigned long long)c.undrawable, TheStore().Size(), (unsigned long long)c.dropped[0], (unsigned long long)c.dropped[1], (unsigned long long)c.dropped[2],
                (unsigned long long)c.forgotten);
  return b;
}
inline void ResetForTest() { TheStore().ResetForTest(); }

}  // namespace rdr_scene::live::beltcolour
