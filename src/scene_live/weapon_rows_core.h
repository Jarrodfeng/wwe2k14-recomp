// Weapons and props: the per-call rows (Session 103, lane PW-2; contract src\renderer_scene\weapon_rows.h, docs\research\s103\PW_adversary.md 0 / 2 / 3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, NO game or SDK includes: the contributor (weapon_rows_contrib.h), the glue (weapon_rows_glue.h) and the offline test (tools\weapons\weapon_rows_test.cpp) all run THIS tracker.
// Read only: nothing here changes what the game does.
//
// At every model call (the REX_HOOK_RAW on sub_82C04DF0 in draw_origin.h, BEFORE its scene_skip::Ours(Model) test: under OURS the original, and so the mid-asm hook at 0x82C04E1C inside it, never runs: PW review
// fix 1; r3 = model object, r5 = the palette the packer reads = [model+200], set by the callers sub_82C13C88 +F0 / sub_82C13A80 +1A4) it
// asks "is r3 a weapon model": an object in this frame's weapon manager slots (main or secondary models), AND is this the weapon's own submit: the call is made from inside the slot-13 dispatcher sub_82C13C88 for this
// very object (a REX_HOOK_RAW on sub_82C13C88 sets the thread's t_slot13_model; r4 = 0 does not single the submit out: slot 19 sub_82C13A80 and slot 32 sub_82C13EC8 pass 0 too: review fix 4). Calls that are not the own submit
// are counted (stats.other_pass) and never recorded. The membership is pulled ONCE a frame (the first model call of the frame) through
// weapons::ReadSlots (weapon_mgr_reader.h: THE one reader of the slot table; PW-1 / PW-2 review fix 4) and held as a small sorted set of {object, id, slot, secondary}; the slots are written at match load / reload only, but a reload frees every model and the freed addresses come back for other ids, so the
// set is never kept across frames. A weapon call becomes one WeaponCall row and N node rows copied from r5 AT THE CALL (node 1..N, ring_rows' rule: the count from sub_82BD3AB8, rows by
// RowsFromMatrix). Never deduped on (object, r5): two chairs share one model AND one r5 pointer (the game memcpys each weapon's matrices into the shared model right before its submit), so
// `instance` = the n-th call of that object in the frame and each call keeps its own rows. The census says how many named objects were never drawn
// (sub_82C13C88's cached command-buffer branch, [model+560] != 0, never reaches sub_82C04DF0 [U]).
// Frames, ring buffer, TakeNext, CloseBefore: as ring_rows_hook.h (a call in a later frame closes the buffer; a closed frame overwritten before Emit took it is `late`).
// Each row carries the frame it was taken in, the weapon id of its object from THAT frame's table (no join across a reload) and slot_flags (slot, secondary bit, vtable bit, secondary index k).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../renderer_scene/scene_record.h"
#include "../renderer_scene/rows_ring.h"   // S106 G4-2
#include "../renderer_scene/weapon_rows.h"
#include "ring_rows_core.h"  // NodeCount (vtable slot 2 / sub_82BD3AB8), kModelAlphaOffset, kRrCount* flags via scene_record.h
#include "arena_world.h"     // RowsFromMatrix
#include "weapon_mgr_reader.h"  // ReadSlots: the one reader of the slot table
#include "lock_stats.h"  // BHF1: the tracker lock cost counters
#include "belt_plate.h"  // S105 PLATE: the belt nameplate store (a model leaving the event list drops its plate)
#include "prop_colour.h"  // S108 THB: the entrance props' colours (a model leaving the event list drops them; the editor's read at the draw ask)
#include "prop_spec.h"  // S111 PROPSPEC: the game's run-time specular / reflection set of an entrance prop model (the row's code; a model leaving the event list drops it)
#include "belt_colour.h"  // S111 BELTMASKNAME: the R5 belt latches (a model leaving the event list drops its own; the scene clear drops the entrance belts only)
#include "../renderer_scene/prop_light_id.h"  // S111 STAGELESS2: the light set an event prop's CWeapon asks for ([cw + 1468])
#include "blood_slot_id.h"  // S111 BLOODMOD2: the blood rows' identity (the permanent blood mod's store keeps by it)
#include "../renderer_scene/floor_blood_switch.h"  // S111 BLOODMOD2: the permanent blood mod's switch and scene count (On / PoolCount)
#include "blood_held.h"  // S111 BLOODMOD3: the rows of a splat the game fades without drawing its own record (made from the last drawn splat)

namespace rdr_scene::live::weaponrows {

namespace w = rdr_scene::weapons;

inline constexpr uint32_t kMaxCalls = 256;        // WeaponCall rows a frame (a match has a handful of weapons)
inline constexpr uint32_t kMaxPoolNodes = 4096;   // node rows a frame (a call that does not fit is refused WHOLE, never cut: stats overflow + CallResult::refused, logged by the glue)
// S108 FAN (docs\research\s108\FAN_plan.md): a call copies the model's OWN node count (sub_82BD3AB8), as the game's draw reads row v - 1 for every palette slot with no cap (sub_82C04DF0 0x82C0A038-0x82C0A04C).
// This bound is only a guard against a garbage count, NOT a size the data must fit: the largest need in the player's files is 731 rows (evtobj PACH 70 entry 0; Fandango's tassels, prop 2591 entry 4, 658; m2 weapons 11,
// bgEtc stage entries 5; scratch census over all 614 evtobj entries + DLC, 87 m2 models, 152 stage entries). A count above it is refused (no rows: the plan skips the call as bad_rows), never truncated: a cut palette
// leaves every vertex past the cut at its file position (ring_pose.h PoseMesh) = the tassel "explosion" the ring's 96 (kRingMaxNodes, rows_ring.h) caused here before S108 FAN.
inline constexpr uint32_t kWeaponMaxNodes = 1024;
inline constexpr uint32_t kMaxMembers = w::kMaxSlots * (1 + w::kMaxSecondary);   // (informational: the membership is a vector)
inline constexpr uint32_t kRing = 4;

// The model whose slot-13 submit (sub_82C13C88) is in flight on THIS thread: set by the REX_HOOK_RAW on sub_82C13C88 (draw_origin.h, Slot13Scope), read by the raw hook on sub_82C04DF0 (same thread, nested). 0 outside.
inline thread_local uint32_t t_slot13_model = 0;
inline thread_local uint32_t t_slot13_caller = 0;   // review 3: the guest LR of that slot-13 dispatch (who submitted the model: the cage's category-26 node, a CWeapon piece, the chamber ...)
struct Slot13Scope {
  uint32_t prev, prev_caller;
  explicit Slot13Scope(uint32_t model, uint32_t caller = 0) : prev(t_slot13_model), prev_caller(t_slot13_caller) { t_slot13_model = model, t_slot13_caller = caller; }
  ~Slot13Scope() { t_slot13_model = prev, t_slot13_caller = prev_caller; }
};
inline bool IsOwnSubmit(uint32_t model) { return model != 0 && t_slot13_model == model; }

// Tests only: a planted fault must fail its NAMED check (the shipped code carries each as one branch).
enum class Plant : uint32_t {
  None = 0,
  DedupeObjectR5,   // the arena's key: one row per (object, r5): the second chair is dropped
  LateRead,         // the rows are read at the NEXT call (or never), not at the call: the rewritten shared buffer is what is recorded
  NodeCountPlus1,   // the node count off by one (one node too many)
  CachedMembers,    // the membership read once and kept across frames
  CountEverySubmit,    // review fix 4: every call of a weapon's model is recorded, not only the slot-13 submit (slot 19 / 32 passes look like a second chair)
  RowsAfterOursCheck,  // review fix 1: the rows are taken after the scene_skip::Ours(Model) test of the raw hook, so an OURS frame records nothing
  NoFrame,             // review fix 2: the row's frame stamp is left 0
  NoIdAtCall,          // review fix 5: the row's id is left -1 (the drawer would have to join across a reload)
  RigidAsUnknown,      // S103 LVF: the old behaviour: a rigid prop (NodeCount answers -1: no skeleton) is count_unknown and gets 0 rows (never drawn)
  StrictForward,       // S103 WC R2: the old behaviour: a forward into the data object (class 0x8206C7A0) is not followed: count_unknown (a weapon with [H+24] = k gets 0 rows, not k)
  EventInstance0,      // S103 BLF review 1: every event call gets instance 0 (two models of one (id, k) would share one posed slot)
  EventNoFree,       // S103 BLF: the free hook (0x8272D678) drops nothing: a freed event model's address, reused by another model, would be drawn as a belt
  StageNoRecheck,    // S103 CGF: the per-call `u32 [H+76+4k] == model` re-check is skipped: a slot freed after the frame's pull, its address reused by another model, would be recorded as a cage piece
  StageNoCap,        // S103 CGF: a cage slot-32 call (LR 0x826F9F08) is not capped at its 2 palette rows: NodeCount's answer reads past the stack palette
  StageInstancePerFamily,   // S111 CAGESIDE review: a stage model's own and slot-32 calls are numbered apart (0.. each, the S103 count): both drawn, they share the drawer's pose slot (PACH, k, instance)
  StageAlpha252,     // S103 CGF: a slot-32 call's alpha is [model+252] (the slot-13 f1), not the call's own f1 (MdlAlpha)
  StageFirstCallOnly,   // S103 CGF: the event branch's rule (the first call of a model in a frame): a cage wall (3 calls of one model) would be drawn once
  StageIgnoreSwitch,    // S103 CGF review 1: --wwe2k14_rdr_stage_structs = off is ignored (the holder is pulled and stage rows are recorded anyway)
  BeltNoSubKey,        // S103 LDR: the planted twin: the weapon manager's belt (id 14) keeps the raw id 14 (no title sub-key): the plan skips it as before
  LadderNoPath,        // S103 LDR: the planted twin: a ladder holder (type 4) is read with PACH 4 whatever path it was asked for (the title-keyed /STGL/%04d rig would draw LADM's models)
  StageNoRigidBound,  // S103 CGF review 5: a rigid stage model's palette offset (64 * (node - 1)) is not bounded to the 2-row stack palette
  StageAlphaAlways,     // S103 CGF review 4: a slot-32 call of an UNKNOWN caller (LR class other) also takes its alpha from f1 (not [model+252])
  GmRescanEmpty,        // S103 GBR review 2: an empty membership is rescanned every frame (the old early-out test `!gm_.empty()`)
  GmHolderAlways,       // S111 GMBORROW: every gm row is stamped kCallGmHolderTex whatever settings byte G+41 holds (the old rule: every non-FR bank borrowed the chamber's bgEtc PACH 6 set)
  GmNoFallthrough,      // S103 GBR review 7: a gm member whose pass / re-check test fails returns at once instead of falling through to the stage / event lookups
  SceneSigLoaded,       // S103 GBR review 3: the scene signature also holds the holder's loaded flag (a holder finishing its read clears the event list)
  FirstWriteClears,     // S103 SMF (BUG2 7): a 0 -> X first write of the holder / manager pointer clears the event list (the old rule: an entrance prop vanished when the manager was made mid-entrance)
  GmAllModels,          // S103 GBR: the gm membership test is dropped: every own-submit model is recorded as a set object (key 1000)
  GmNoRecheck,          // S103 GBR: the per-call `[obj+48] == model && [obj+44] == key` re-check of a gmsk member is skipped (a freed object's model address reused by another model would be recorded as a set piece)
  GmIgnorePass,         // S103 GBR: the thread pass index (sub_82BA77A8 == 0) is not tested: a shadow / other pass of a set object or a splat is recorded as a second copy
  BloodIncludeDead,     // S103 GBR: a decal record with alpha 0 (dead) is given a call like a live one (the game's loop does not draw it)
  EventNoClear,         // S103 GBR (BH4 3): the event list is not cleared when the stage holder changes
  EventNoPrune,         // S103 GBR (BH4 3): a model whose vtable word is no longer a CSkinModel's stays in the event list
  ShadowAsColour,       // S103 STG (PMW): the shadow flag A tag is ignored: a caster submit under flag A is taken as a colour call (a manager weapon drawn 3x, an event row taken from the props-map call)
  RingCap96,            // S108 FAN: the old rule: every call cut at the ring's kRingMaxNodes (96) rows (Fandango's tassels past bone95 stay at their file position)
  EventRowsAppend,      // S108 FAN: an own submit that replaces an earlier non-own row of the same event model appends a second copy of its rows (the old one stays in the pool, unread) instead of rewriting them in place
};

// S103 BLF (docs\research\s103\BELT.md Adversary): the EVENT PROP models, pushed by two mid-asm hooks (draw_origin.h / event_props_hooks.h). MakeWeapon (sub_8272D710) makes up to 10 CSkinModel objects per prop, one per present
// entry k of the prop's evtobj.pac PACH; the hook at 0x8272E030 hands (model, prop id, k) to OnMake; the CWeapon's destructor hook at 0x8272D678 hands the CWeapon to OnFree, which drops every model it holds (they are still alive
// there). The weapon manager never sees these models (MakeWeapon does not call SetWeaponData), so this is a second membership source next to the slot table. The hooks run on the game's loader / task threads [U]: one mutex.
class EventProps {
 public:
  struct Entry {
    int32_t id = -1;     // the prop id (u32 [r1+2348] in MakeWeapon: the entrance's already-rewritten id)
    uint32_t k = 0;      // the PACH entry this model was built from (r29 at the hook = the loop counter [r1+80])
    int32_t light_set = -1;   // S111 STAGELESS2 (prop_light_id.h): the set id its CWeapon asks the light manager for ([cw + 1468]), read once a frame; -1 = not read
  };
  // S111 STAGELESS2: the CWeapon MakeWeapon builds (the raw hook on its ctor sub_8272BCD0, draw_origin.h). Kept until its free (OnFree) or a scene change (Clear); a pointer whose vtable word is no longer
  // the event CWeapon's is dropped by ReadLightSets.
  void OnWeapon(uint32_t weapon) {
    if (weapon == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    weapons_.push_back(weapon);
  }
  // Once a frame (Tracker::NoteScene, after Prune): each live CWeapon's [cw + 1468] onto every model of it the list holds ([cw + 48] and [cw + 1228 + 4i]). Returns the models stamped.
  template <class ReadWordFn>
  uint32_t ReadLightSets(ReadWordFn&& read_word) {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t n = 0;
    for (size_t i = 0; i < weapons_.size();) {
      const uint32_t cw = weapons_[i];
      uint32_t vt = 0, set = 0;
      if (!read_word(cw, vt) || vt != w::kEventWeaponVtable) { weapons_[i] = weapons_.back(), weapons_.pop_back(); continue; }
      const bool have = read_word(cw + w::kOffAskedLightSet, set);
      uint32_t models[w::kMaxEventModels + 1] = {};
      read_word(cw + 48u, models[0]);
      for (uint32_t k = 0; k < w::kMaxEventModels; ++k) read_word(cw + w::kOffEventModels + 4 * k, models[k + 1]);
      for (uint32_t m : models) {
        if (m == 0) continue;
        auto it = map_.find(m);
        if (it == map_.end()) continue;
        it->second.light_set = have ? int32_t(set) : -1;
        ++n;
      }
      ++i;
    }
    return n;
  }
  static constexpr uint32_t kPubSlots = 128;   // S103 GBR (BH4 item 3): the lock-free lookup's published slots (a match holds a handful of props x <= 10 models)
  void OnMake(uint32_t model, uint32_t prop_id, uint32_t k) {
    if (model == 0) return;   // the allocation failed (0x82BD41B0); the game tests it later (0x8272E044), the hook tests it itself
    std::lock_guard<std::mutex> lk(mu_);
    beltplate::DropOnMake(model);   // S105 PLATE review 1: a free the destructor hook missed leaves the old model's plate verdict at this address (lock order EventProps::mu_ then the Store's, as OnFree / Prune / Clear)
    propcolour::DropOnMake(model);  // S108 THB: ... and the old model's prop colours (the same stale-address rule)
    propspec::DropOnMake(model);    // S111 PROPSPEC: ... and the old model's specular set
    beltcolour::DropOnMake(model);  // S111 BELTMASKNAME: ... and the old model's R5 belt latch (any mode: a reused address)
    map_[model] = Entry{int32_t(prop_id), k};
    live_.store(uint32_t(map_.size()), std::memory_order_relaxed);
    ++made_;
    Publish();
  }
  // `read_word(addr, v)`: the guest reader. Returns the number of models dropped; a pointer that is not a CWeapon of this class (vtable word != kEventWeaponVtable) drops nothing.
  template <class ReadWordFn>
  uint32_t OnFree(uint32_t weapon, ReadWordFn&& read_word, bool drop = true) {
    uint32_t vt = 0;
    if (weapon == 0 || !read_word(weapon, vt) || vt != w::kEventWeaponVtable) return 0;
    uint32_t models[w::kMaxEventModels] = {};
    for (uint32_t i = 0; i < w::kMaxEventModels; ++i) read_word(weapon + w::kOffEventModels + 4 * i, models[i]);
    uint32_t n = 0;
    std::lock_guard<std::mutex> lk(mu_);
    ++frees_;
    for (size_t i = 0; i < weapons_.size(); ++i)   // S111 STAGELESS2: the CWeapon is gone
      if (weapons_[i] == weapon) { weapons_[i] = weapons_.back(), weapons_.pop_back(); break; }
    if (!drop) return 0;
    for (uint32_t m : models)
      if (m != 0) n += uint32_t(map_.erase(m)), beltplate::DropOnFree(m), propcolour::DropOnFree(m), propspec::DropOnFree(m), beltcolour::DropOnFree(m);   // S105 PLATE: a freed model address reused by another belt must not keep the old name   // S108 THB: nor the old prop's colours   // S111 PROPSPEC: nor its specular set   // S111 BELTMASKNAME: nor its R5 latch
    dropped_ += n;
    live_.store(uint32_t(map_.size()), std::memory_order_relaxed);
    Publish();
    return n;
  }
  // S103 GBR (BH2 G2 / BH4 item 3): the list never reset except through the CWeapon destructor hook: a failed CWeapon allocation (0x8272E540) leaks the first model, and an event CWeapon freed any other way (a heap reset, the
  // sub_833247B8 / sub_83391E00 owners [U]) leaves its models here; a leaked address reused by a body model would be drawn as a belt. Event props never outlive the arena / match: the tracker clears the list when the stage
  // holder or the weapon manager changes (Tracker::NoteScene), and prunes per frame every entry whose model no longer carries a CSkinModel vtable word (the owner is not known at the MakeWeapon hook: the CWeapon is made later).
  uint32_t Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t n = uint32_t(map_.size());
    map_.clear(), beltplate::ClearOnScene(), propcolour::ClearOnScene(), propspec::ClearOnScene(), beltcolour::ClearOnScene();   // S105 PLATE   // S108 THB   // S111 PROPSPEC   // S111 BELTMASKNAME (entrance belts only)
    weapons_.clear();   // S111 STAGELESS2: their models just left the list
    cleared_ += n;
    live_.store(0, std::memory_order_relaxed);
    Publish();
    return n;
  }
  template <class ReadWordFn>
  uint32_t Prune(ReadWordFn&& read_word) {   // one vtable read per live entry, once a frame (never per model call)
    if (live_.load(std::memory_order_relaxed) == 0) return 0;
    uint32_t n = 0;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = map_.begin(); it != map_.end();) {
      uint32_t vt = 0;
      if (read_word(it->first, vt) && vt == w::kSkinnedModelVtable) { ++it; continue; }
      beltplate::DropOnFree(it->first), propcolour::DropOnFree(it->first), propspec::DropOnFree(it->first), beltcolour::DropOnFree(it->first), it = map_.erase(it), ++n;   // S105 PLATE   // S108 THB   // S111 PROPSPEC
    }
    if (n) {
      pruned_ += n;
      live_.store(uint32_t(map_.size()), std::memory_order_relaxed);
      Publish();
    }
    return n;
  }
  // The hot path (every model call of the draw hook): NO lock. live_ == 0 (no entrance prop alive) is one relaxed load; else a scan of the published slots, each ONE 64-bit word = (model << 32) | (id << 8) | k, 0 = empty
  // (a scan during a publish can miss a model for one frame, never return a half entry). An id above 24 bits (never seen: prop ids are < 4000) is not published.
  bool Find(uint32_t model, Entry& out) const {
    if (live_.load(std::memory_order_relaxed) == 0) return false;
    for (uint32_t i = 0; i < kPubSlots; ++i) {
      const uint64_t v = pub_[i].load(std::memory_order_relaxed);
      if (v != 0 && uint32_t(v >> 32) == model) {
        out.id = int32_t((v >> 8) & 0xFFFFFFu), out.k = uint32_t(v & 0xFFu);
        return true;
      }
    }
    return false;
  }
  void Resets(uint64_t& cleared, uint64_t& pruned, uint64_t& pub_overflow) const {   // GBR census
    std::lock_guard<std::mutex> lk(mu_);
    cleared = cleared_, pruned = pruned_, pub_overflow = pub_overflow_;
  }
  // The live models as (model, entry), ascending by model (the record's section 59 rows: the drawer's prefetch).
  std::vector<std::pair<uint32_t, Entry>> Snapshot() const {
    std::vector<std::pair<uint32_t, Entry>> out;
    if (live_.load(std::memory_order_relaxed) == 0) return out;
    std::lock_guard<std::mutex> lk(mu_);
    out.assign(map_.begin(), map_.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
  }
  size_t Size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
  }
  void Counts(uint64_t& made, uint64_t& frees, uint64_t& dropped) const {
    std::lock_guard<std::mutex> lk(mu_);
    made = made_, frees = frees_, dropped = dropped_;
  }

 private:
  void Publish() {   // mu_ held: the whole table is republished (set-up / free rates: a few a match)
    uint32_t i = 0;
    for (const auto& kv : map_) {
      if (kv.second.id < 0 || kv.second.id > 0xFFFFFF || i >= kPubSlots) { ++pub_overflow_; continue; }
      pub_[i++].store((uint64_t(kv.first) << 32) | (uint64_t(uint32_t(kv.second.id)) << 8) | (kv.second.k & 0xFFu), std::memory_order_relaxed);
    }
    for (; i < kPubSlots; ++i) pub_[i].store(0, std::memory_order_relaxed);
  }
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, Entry> map_;
  std::vector<uint32_t> weapons_;   // S111 STAGELESS2: the live event CWeapons (OnWeapon), mu_ held
  std::atomic<uint32_t> live_{0};
  std::atomic<uint64_t> pub_[kPubSlots] = {};
  uint64_t made_ = 0, frees_ = 0, dropped_ = 0, cleared_ = 0, pruned_ = 0, pub_overflow_ = 0;
};

// capped (S108 FAN): calls whose node count is above kWeaponMaxNodes, refused (no rows); overflow: calls refused because the frame's pool (kMaxPoolNodes) is full. Neither is ever cut short.
struct Stats {
  uint64_t other_pass = 0, calls = 0, weapon_calls = 0, recorded = 0, rows_copied = 0, unreadable = 0, count_unknown = 0, capped = 0, forwarded = 0, overflow = 0, old_calls = 0, late = 0, frames = 0,
           member_reads = 0, mgr_missing = 0, slots_refused = 0, alpha_unread = 0;
  uint64_t slots_unreadable = 0, bad_vtable_calls = 0;
  uint64_t event_replaced = 0;   // S103 BLF: an own-submit call replaced the row of an earlier non-own pass of the same model
  uint64_t event_rows_in_place = 0;   // S108 FAN: of those, the rows rewritten in place (the replaced row's pool rows reused: one copy a frame)
  uint32_t max_rows = 0, max_rows_k = 0;   // S108 FAN review: the largest row count one call was given so far, and whose (id / entry k; the census names it: `largest call N rows (event prop I k K)`)
  int32_t max_rows_id = -1;
  bool max_rows_event = false;
  uint64_t event_calls = 0;  // S103 BLF: calls whose r3 is an event prop model (every pass; the first of a frame becomes a row, the rest count in other_pass)
  uint64_t rigid = 0, rigid_node_assumed = 0;   // S103 LVF: rigid props (no skeleton: ONE row, the node of part 0's bone list); rigid_node_assumed = part 0 unreadable, node 1 taken
  uint64_t data_leaf = 0, zero_nodes = 0, foreign_fwd = 0, bad_count = 0;   // S103 WC R1 / R2: counts that came from the data object (+220 -> class 0x8206C7A0, [H+24]); n == 0 (no rows, the game copies nothing); forwards into an unknown class; counts below -1
  uint64_t objects = 0;        // (object, frame) pairs that were called
  uint64_t named = 0;          // (object, frame) pairs the slots named (frames that made at least one model call)
  uint64_t never_drawn = 0;    // named, no call
  uint64_t last_named = 0, last_never = 0;   // the last closed frame
  uint64_t stage_pulls = 0, stage_frames = 0, stage_calls = 0, stage_slot32 = 0, stage_own = 0, stage_recheck_fail = 0, stage_capped = 0;   // S103 CGF: holder pulls; frames with members; calls of a member model (by family); address-reuse re-check failures; cage rows capped at 2
  uint64_t stage_own_cage = 0, stage_own_other = 0, stage_rigid_bounded = 0;   // review 3 / 5: own calls by caller class (cage category-26 node / any other caller); rigid-node palette offsets refused (past the 2-row stack palette)
  uint32_t stage_type = 9, stage_models = 0;   // S103 CGF: the last pulled holder's type and member count
  // S103 GBR census: gm set objects / blood splats / the event list's resets
  uint64_t gm_pulls = 0, gm_calls = 0, gm_other_pass = 0, gm_pass_skip = 0, gm_recheck_fail = 0;   // membership rebuilds; recorded set-object calls; member calls that were not the slot-13 submit; calls of a non-zero pass index; address-reuse re-check failures
  uint32_t gm_models = 0;                                                                          // the last membership's size
  uint64_t blood_calls = 0, blood_other_pass = 0, blood_pass_skip = 0, blood_dead_skipped = 0, blood_unmatched = 0, blood_unreadable = 0, blood_frames = 0;   // blood splat calls recorded; non-submit calls; non-zero pass; dead records not given a call; calls with no live record left; pool unreadable
  uint32_t blood_live = 0;                                                                         // the last snapshot's live record count
  uint64_t blood_held = 0, blood_held_noref = 0, blood_synth_checks = 0;   // S111 BLOODMOD3 (the permanent blood mod only): held rows made (a particle fading undrawn: an instant replay); such splats with no reference to make them from; drawn splats checked against the rows made from the one before
  float blood_synth_err = 0.f;                                             // the largest row difference of those checks (game units: near 0 = the composition rule holds on the game's draws)
  uint64_t shadow_calls = 0, shadow_props_calls = 0;   // S103 STG: calls made under the game's flag A (tagged kCallShadowPass, never a colour row); of them, the props map's (camera = module + 1312)
  uint64_t ev_cleared = 0, ev_pruned = 0, ev_scene_changes = 0;                                    // event list entries dropped by a scene change / a stale vtable; scene changes seen
};

// S103 CGF (CAGE.md section 3 + Adversary 1 / 4): the membership of the ONE stage-structure holder H = [0x837631C4], pulled every frame (never cached across frames: the free zeroes the slots, a reload reuses addresses).
struct StageMember {
  uint32_t model = 0, k = 0;
};
struct StageView {
  uint32_t holder = 0, type = 9;
  int32_t pach = -1;   // the bgEtc PACH of the type's path (weapons::StagePachOfType); -1 = nothing to draw
  std::vector<StageMember> members;
  const StageMember* Find(uint32_t model) const {
    for (const StageMember& m : members)
      if (m.model == model) return &m;
    return nullptr;
  }
};
// Accepts H >= 0x10000, [H+172] == 1 (loaded / idle), type in {0, 1, 2, 3, 5, 7} or 4 with a ladder path latched (S103 LDR: StagePathPach of the last request), n = min([H+164], 18), members = the nonzero [H+76+4k] whose vtable word is CSkinModel's. An unreadable word = no members (never half a list).
template <class ReadWordFn>
inline bool ReadStageHolder(ReadWordFn& read_word, StageView& out, int32_t ladder_path_pach = -1) {
  out = StageView();
  uint32_t h = 0, loaded = 0, type = 0, count = 0;
  if (!read_word(w::kHolderGlobal, h) || h < 0x10000) return false;
  if (!read_word(h + w::kHolderOffLoaded, loaded) || loaded != 1) return false;
  if (!read_word(h + w::kHolderOffType, type)) return false;
  const int32_t pach = type == w::kHolderTypeLadder ? ladder_path_pach : w::StagePachOfType(type);   // S103 LDR: a ladder holder (type 4) maps by the path its request was given (the latch below), never by the type
  if (pach < 0 || !read_word(h + w::kHolderOffCount, count)) return false;
  const uint32_t n = count < w::kHolderMaxKeys ? count : w::kHolderMaxKeys;
  StageView v;
  v.holder = h, v.type = type, v.pach = pach;
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t m = 0, vt = 0;
    if (!read_word(h + w::kHolderOffModel + 4 * k, m)) return false;
    if (m == 0) continue;
    if (!read_word(m, vt)) return false;
    if (vt == w::kSkinnedModelVtable) v.members.push_back({m, k});
  }
  if (v.members.empty()) return false;
  out = std::move(v);
  return true;
}

// One named model object of this frame's slot table (main or secondary), what a call of it is stamped with.
struct Named {
  uint32_t object = 0;
  int32_t id = -1;
  uint32_t slot_flags = 0;   // PackSlotFlags(slot, secondary, vtable_ok, k)
};
// A word reader (the test's fake guest / the game's ReadWord) as the Mem type weapons::ReadSlots wants.
template <class ReadWordFn>
struct WordMem {
  ReadWordFn& fn;
  bool read32(uint32_t a, uint32_t& v) const { return fn(a, v); }
};
using live::weapons::BeltSubKey;   // S103 LDR: the belt sub-key reader lives with the slot reader (weapon_mgr_reader.h)
// The membership of the frame: the objects of the live slots, from the ONE slot reader. Sorted by object (a model object shared by two slots keeps its first); `why` says what the table was.
template <class ReadWordFn>
inline live::weapons::Why ReadNamed(ReadWordFn& read_word, std::vector<Named>& out, std::vector<w::WeaponSlot>& slots_scratch, live::weapons::BeltKeyCache& belt_keys, bool belt_subkey = true) {
  out.clear();
  WordMem<ReadWordFn> mem{read_word};
  const live::weapons::Why why = live::weapons::ReadSlots(mem, slots_scratch);
  if (why != live::weapons::Why::None) return why;
  if (belt_subkey) live::weapons::ApplyBeltIds(read_word, belt_keys, slots_scratch);   // S103 LDR (B2): the belt's id carries its sub-key (the title's belt), read once per belt object
  for (const w::WeaponSlot& sl : slots_scratch) {
    const bool vt = (sl.flags & 1u) != 0;
    const int32_t id = sl.id;   // (the belt's id already carries its sub-key: ApplyBeltIds above)
    out.push_back({sl.main, id, w::PackSlotFlags(sl.slot, false, vt)});
    for (uint32_t k = 0; k < w::kMaxSecondary; ++k)
      if (sl.secondary[k]) out.push_back({sl.secondary[k], id, w::PackSlotFlags(sl.slot, true, vt, k)});
  }
  std::stable_sort(out.begin(), out.end(), [](const Named& x, const Named& y) { return x.object < y.object; });
  out.erase(std::unique(out.begin(), out.end(), [](const Named& x, const Named& y) { return x.object == y.object; }), out.end());
  return why;
}

struct FrameBuffer {
  uint64_t frame = 0;
  bool valid = false;
  std::vector<w::WeaponCall> calls;
  std::vector<RingNodeRows> nodes;
  uint32_t named = 0, never_drawn = 0, objects = 0;   // filled when the frame closes
  void Clear() {
    frame = 0, valid = false;
    calls.clear(), nodes.clear();
    named = never_drawn = objects = 0;
  }
};

struct CallResult {
  bool weapon = false, recorded = false;
  uint32_t instance = 0, n_nodes = 0;
  bool counted = false, vtable_ok = false, rigid = false;   // S103 LVF: for the per-call log line (NodeCount's answer, the slot's vtable flag, the rigid path taken)
  int32_t n = 0;
  bool data_leaf = false;                  // S103 WC: the count came from the data object (the forward's slot 2 = sub_82C27620)
  uint32_t fwd_vtable = 0, fwd_fn = 0;     // S103 WC: the forward target's vtable and slot 2 word when it was neither a CSkinModel nor the data object (0 = none); for the per-call log line
  bool own_submit = false;                 // S103 BLF: the call was the slot-13 own submit
  bool event = false;                      // S103 BLF: the model is an event prop (MakeWeapon), id = its prop id, k = its PACH entry
  bool stage = false;                      // S103 CGF: the model is a stage structure piece (the holder's [H+76+4k]): id = the bgEtc PACH, k = the key
  bool gm = false, blood = false;          // S103 GBR: a gmsk set object (id = kGmIdBase + key) / a blood splat (id = kBloodIdBase + bfloat16 colour, k = texture index)
  uint32_t lr = 0;                         // S103 CGF: the guest LR of the call (a stage call; the family discriminator)
  float f1 = 1.0f;                         // S103 CGF: the call's f1 (MdlAlpha)
  int32_t id = -1;
  uint32_t k = 0;
  uint8_t refused = 0;                     // S108 FAN: kRefusedOverBound / kRefusedPoolFull (the call got no rows; the glue logs it with the model and the count `n`), 0 = not refused
  uint32_t pool_used = 0;                  // S108 FAN: the frame's pool rows at the refusal
};
inline constexpr uint8_t kRefusedOverBound = 1, kRefusedPoolFull = 2;

class Tracker {
 public:
  Plant plant = Plant::None;
  bool stage_enabled = true;   // review 1: --wwe2k14_rdr_stage_structs (default true): off = no holder pull and no stage branch
  bool gm_enabled = true, blood_enabled = true;   // S103 GBR: --wwe2k14_rdr_gm_sets (default true): off = no gm membership pull, no blood read; the call branches are skipped
  // S103 LDR (docs\research\s103\COV10_match_objects.md B1): the bgEtc PACH of the LAST path the stage holder's request sub_82576768 was given (the raw hook in draw_origin.h calls NoteStagePath with r4's string); -1 = that path has no
  // PACH (LADU / LADC, a non-ladder path, nothing yet). A ladder holder (type 4, five paths) is read only through it (ReadStageHolder); every request overwrites it, so a stale PACH never outlives the next request.
  std::atomic<int32_t> stage_path_pach{-1};
  std::atomic<uint64_t> stage_paths{0}, stage_paths_ladder{0}, stage_paths_unmapped{0};   // requests seen; of those a ladder PACH; a ladder-looking path with no PACH (LADU / LADC / a title row the directory lacks)
  void NoteStagePath(const char* s, uint32_t n) {
    int32_t p = w::StagePathPach(s, n);
    if (plant == Plant::LadderNoPath && p >= 11) p = 4;   // twin: the title-keyed rig is read as the no-title one (the PACH of the path is ignored)
    stage_path_pach.store(p, std::memory_order_relaxed);
    stage_paths.fetch_add(1, std::memory_order_relaxed);
    if (p >= 0) stage_paths_ladder.fetch_add(1, std::memory_order_relaxed);
    else if (n >= 5 && s[0] == '/' && s[1] == 'S' && s[2] == 'T' && s[3] == 'G' && (s[4] == 'L' || (n >= 8 && s[4] == '/' && s[5] == 'L' && s[6] == 'A' && s[7] == 'D'))) stage_paths_unmapped.fetch_add(1, std::memory_order_relaxed);
  }
  EventProps events;   // S103 BLF: the event prop models (pushed by the MakeWeapon hooks); consulted when r3 is not in the slot table
  void EventMake(uint32_t model, uint32_t prop_id, uint32_t k) { events.OnMake(model, prop_id, k); }
  void EventWeapon(uint32_t weapon) { events.OnWeapon(weapon); }   // S111 STAGELESS2: the CWeapon ctor's hook (weapon_rows_glue.h EventWeaponMade)
  uint64_t BeltKeyReads() const { return belt_keys_.reads; }   // S103 LDR: sub-key reads so far (one per belt object)
  template <class ReadWordFn>
  uint32_t EventFree(uint32_t weapon, ReadWordFn&& read_word) { return events.OnFree(weapon, read_word, plant != Plant::EventNoFree); }

  // `read_word(addr, v)`; `read_mats(addr, n, std::vector<Mat4>&)` (big-endian guest bytes -> host floats, false on any fault): the fake in the test, wrestlers::ReadWord / ReadMatrices in the game.
  template <class ReadWordFn, class ReadMatsFn>
  CallResult OnModelCall(uint32_t model, uint32_t palette_ptr, uint64_t frame, bool own_submit, ReadWordFn read_word, ReadMatsFn read_mats, float f1 = 1.0f, uint32_t lr = 0, uint32_t own_caller = 0, bool (*pass0)(void*) = nullptr, void* pass_ctx = nullptr, uint8_t shadow = 0) {
    CallResult res;
    if (plant == Plant::ShadowAsColour) shadow = 0;
    TimedLock<std::mutex> lk(mu_, lock_stats_);   // BHF1 B2: counted, not cut (the cost is fitted first; census: model_lock)
    ++stats_.calls;
    if (!Advance(frame)) { ++stats_.old_calls; return res; }
    if (!scene_frame_valid_ || scene_frame_ != cur_.frame) {   // S103 GBR: the once-a-frame pulls of the gm set / blood model / scene signature (next to the stage holder's and the weapon slots' below)
      scene_frame_ = cur_.frame, scene_frame_valid_ = true;
      NoteScene(read_word);
      if (gm_enabled) PullGm(read_word, cur_.frame);
      blood_model_ = 0;
      if (blood_enabled) {
        uint32_t p = 0, m = 0;
        if (read_word(w::kBloodHolderGlobal, p) && p >= 0x10000 && read_word(p + w::kBloodHolderOffModel, m) && m >= 0x10000) blood_model_ = m;
      }
      blood_recs_frame_valid_ = false, blood_next_ = 0;
      if (blood_enabled && ::rdr_scene::floor_blood::On()) blood_ids_.Scan(read_word, ::rdr_scene::floor_blood::PoolCount());   // S111 BLOODMOD2 (the mod only): the particles' states, once a frame (a new splat = a new serial)
      if (blood_enabled && ::rdr_scene::floor_blood::On()) EmitHeldSplats(read_word);   // S111 BLOODMOD3 (the mod only): a splat the game fades without drawing its own record (an instant replay) gets a keep-only row
    }
    const bool stage_on = stage_enabled || plant == Plant::StageIgnoreSwitch;
    if (stage_on && (!stage_frame_valid_ || stage_frame_ != cur_.frame)) {   // S103 CGF: the stage holder's membership, one pull a frame (the first model call), next to the weapon slots' below
      ++stats_.stage_pulls;
      stage_frame_ = cur_.frame, stage_frame_valid_ = true;
      if (!ReadStageHolder(read_word, stage_, stage_path_pach.load(std::memory_order_relaxed))) stage_ = StageView();
      stats_.stage_type = stage_.type, stats_.stage_models = uint32_t(stage_.members.size());
      if (!stage_.members.empty()) ++stats_.stage_frames;
    }
    if (!members_frame_valid_ || (members_frame_ != cur_.frame && plant != Plant::CachedMembers)) {   // one membership read a frame (the first model call), not a read per call
      ++stats_.member_reads;
      const live::weapons::Why why = ReadNamed(read_word, members_, slots_, belt_keys_, plant != Plant::BeltNoSubKey);
      if (why == live::weapons::Why::NoManager) ++stats_.mgr_missing;
      if (why == live::weapons::Why::Refused) ++stats_.slots_refused;
      if (why == live::weapons::Why::Unreadable) ++stats_.slots_unreadable;
      members_frame_ = cur_.frame, members_frame_valid_ = true;
      named_ = members_;
      called_.assign(named_.size(), 0);
    }
    if (plant == Plant::LateRead && !pending_.empty()) {   // the planted bug: the rows of the PREVIOUS weapon call are read now, after the game rewrote the shared buffer
      for (const Pending& p : pending_) {
        mats_.clear();
        if (p.frame == cur_.frame && read_mats(p.r5, p.count, mats_) && mats_.size() >= p.count)
          for (uint32_t i = 0; i < p.count; ++i) arenaworld::RowsFromMatrix(mats_[i].m, cur_.nodes[p.first + i].rows);
      }
      pending_.clear();
    }
    auto it = std::lower_bound(named_.begin(), named_.end(), model, [](const Named& n, uint32_t m) { return n.object < m; });
    if (model == 0) return res;
    Named ev;   // S103 BLF: an event prop model (pushed by the MakeWeapon hooks, not in the slot table): the same row, stamped with the prop id and its entry k
    bool is_event = false, is_stage = false, is_gm = false, is_blood = false;
    uint32_t blood_id = 0;   // S111 BLOODMOD2: the splat's identity (blood_slot_id.h; 0 while the permanent blood mod is off)
    BloodRec blood_br;       // S111 BLOODMOD3: this blood call's record (its place / scale / angle: the held rows' next reference)
    if (it == named_.end() || it->object != model) {
      if (gm_enabled && !gm_.empty()) {   // S103 GBR (GMS.md (A)): a gmsk set object's model; only the slot-13 submit in the colour pass (pass index 0)
        const auto git = std::lower_bound(gm_.begin(), gm_.end(), model, [](const GmMember& g, uint32_t m) { return g.model < m; });
        const GmMember any_model{model, 0, 1000};   // planted twin GmAllModels: every model is a member (the membership test dropped)
        const GmMember* gmp = (git != gm_.end() && git->model == model) ? &*git : nullptr;
        const bool take_any = plant == Plant::GmAllModels && !gmp;
        if (take_any) gmp = &any_model;
        if (gmp) {
          // review 7: a member whose submit / pass / re-check test fails is NOT recorded as a set object, but falls through to the blood / stage / event lookups below (a stale membership entry must not hide a real stage or event model that reuses the address)
          bool take = true;
          uint32_t cur_model = 0, cur_key = 0;
          if (!own_submit) ++stats_.gm_other_pass, take = false;
          else if (plant != Plant::GmIgnorePass && pass0 && !pass0(pass_ctx)) ++stats_.gm_pass_skip, take = false;
          else if (plant != Plant::GmNoRecheck && !take_any && !(read_word(gmp->obj + w::kGmObjOffModel, cur_model) && cur_model == model && read_word(gmp->obj + w::kGmObjOffKey, cur_key) && cur_key == gmp->key)) ++stats_.gm_recheck_fail, take = false;
          if (!take && plant == Plant::GmNoFallthrough) return res;
          if (take) {
            ev.object = model, ev.id = w::GmId(gmp->key), ev.slot_flags = w::PackGmFlags(gm_holder_tex_);   // S111 GMBORROW: + the game's texture-set choice (PullGm)
            is_gm = true;
            res.gm = true;
          }
        }
      }
      if (!is_gm && blood_enabled && blood_model_ != 0 && model == blood_model_) {   // S103 GBR (GMS.md (B)): one splat per call; the n-th call of the frame takes the n-th live decal record
        if (!own_submit) { ++stats_.blood_other_pass; return res; }
        if (plant != Plant::GmIgnorePass && pass0 && !pass0(pass_ctx)) { ++stats_.blood_pass_skip; return res; }
        if (!blood_recs_frame_valid_) PullBloodRecs(read_word);
        if (!blood_recs_ok_) { ++stats_.blood_unreadable; return res; }
        if (blood_next_ >= blood_recs_.size()) { ++stats_.blood_unmatched; return res; }
        const BloodRec br = blood_recs_[blood_next_++];
        ev.object = model, ev.id = w::BloodIdOf(br.colour), ev.slot_flags = w::PackBloodFlags(br.tex) | br.slot_bits;   // S111 BLOODMOD2: + the particle slot k (bits 0-7; 0 while the mod is off)
        blood_id = br.id;
        blood_br = br;
        is_blood = true;
        res.blood = true;
      }
    }
    if (!is_gm && !is_blood && (it == named_.end() || it->object != model)) {
      const StageMember* sm = stage_on ? stage_.Find(model) : nullptr;   // S103 CGF: a stage structure piece (the holder's model): every call is its own instance
      if (sm) {
        // the game's getter reads [H+76+4k] at its own call: re-read that one word here (an address freed after this frame's pull and reused by another model must not become a cage piece)
        uint32_t cur_word = 0;
        if (plant != Plant::StageNoRecheck && !(read_word(stage_.holder + w::kHolderOffModel + 4 * sm->k, cur_word) && cur_word == model)) { ++stats_.stage_recheck_fail; return res; }
        // an own call is tagged by who called the slot-13 dispatcher (review 3); a slot-32 / other call by its own LR. The alpha comes from f1 only for the cage / cell LR classes (review 4): an unknown caller's f1 is not MdlAlpha
        const uint32_t lr_class = own_submit ? w::OwnCallerClass(own_caller) : w::LrClassOf(lr);
        const bool alpha_f1 = !own_submit && (lr_class != w::kStageLrOther || plant == Plant::StageAlphaAlways);
        ev.object = model, ev.id = w::StageId(stage_.pach), ev.slot_flags = w::PackStageFlags(sm->k, own_submit, alpha_f1, lr_class) | w::stg_nodes::CasterBit(own_submit, own_caller);   // S111 CHAMBERSHADOW: + bit 31 when the submitting node is one the game's shadow passes draw (a CWeapon piece, stage_struct_nodes.h)
        is_stage = true;
        res.stage = true, res.lr = lr, res.f1 = f1;
      } else {
        EventProps::Entry e;
        if (!events.Find(model, e)) return res;
        ev.object = model, ev.id = e.id, ev.slot_flags = w::PackEventFlags(e.k);
        is_event = true;
      }
    }
    const Named& hit = (is_event || is_stage || is_gm || is_blood) ? ev : *it;
    res.weapon = true, res.own_submit = own_submit;
    uint32_t ev_instance = 0;
    uint32_t reuse_first = 0, reuse_count = 0;   // S108 FAN: the pool rows of the event row this own submit replaces (rewritten in place when the count matches: one copy of the model's rows a frame, not two)
    // S103 STG (docs\research\s103\PMW.md): a call made under the game's shadow flag A (the props pass and map B submit every caster once each, in a frame the game walks) is a caster row, never a colour row: it skips the event first-call
    // logic, the called_ mark and the stage counts, takes its own instance key, and carries kCallShadowPass (+ kCallShadowProps for the props map). gm sets and blood splats already return on flag A (pass0 above).
    const bool shadow_call = shadow != 0 && !is_gm && !is_blood;
    if (shadow_call) {
      ++stats_.shadow_calls;
      if (shadow == 1) ++stats_.shadow_props_calls;
    } else if (is_gm || is_blood) {
      is_gm ? ++stats_.gm_calls : ++stats_.blood_calls;   // S103 GBR: one row per call, each its own instance (a set object once a frame; the n-th splat)
    } else if (is_stage) {
      ++stats_.stage_calls, own_submit ? ++stats_.stage_own : ++stats_.stage_slot32;
      if (own_submit) w::StageLrClass(ev.slot_flags) == w::kStageLrCage ? ++stats_.stage_own_cage : w::OwnOtherCaller(w::StageLrClass(ev.slot_flags)) ? ++stats_.stage_own_other : 0u;   // S111 PODGLASS: own-other = class 3 or the own-node class 2
    } else if (is_event) {
      // Which of the model's submits (slot 13 / 19 / 32: all reach sub_82C04DF0 with r3 = the model, all of vtable 0x8206AF90) an event prop uses is [U], so the first call of a model in a frame is its row and later passes of the SAME
      // model in that frame are counted, never a second copy (a prop is drawn once; a shadow / extra pass is not a second belt).
      // Review fixes 1 / 5: the row of a model is its OWN-submit call when one comes in the frame (it replaces an earlier non-own row), else its first call. The instance is the n-th MODEL of this (prop id, entry k) in the
      // frame (two tag-team belts / two identical mics = 0 and 1: each its own posed slot in the drawer), not the model's call count.
      ++stats_.event_calls;
      EvSeen* seen = nullptr;
      for (EvSeen& s : ev_seen_)
        if (s.model == model) seen = &s;
      if (seen) {
        if (seen->own || !own_submit) { ++stats_.other_pass; return res; }
        for (size_t i = 0; i < cur_.calls.size(); ++i)
          if (cur_.calls[i].object == model) { ev_instance = cur_.calls[i].instance, reuse_first = cur_.calls[i].node_first, reuse_count = cur_.calls[i].node_count, cur_.calls.erase(cur_.calls.begin() + std::ptrdiff_t(i)); break; }
        seen->own = true;
        ++stats_.event_replaced;
      } else {
        ev_seen_.push_back({model, own_submit});
        ev_instance = plant == Plant::EventInstance0 ? 0u : NextInstanceKey((1ull << 63) | (uint64_t(uint32_t(hit.id)) << 8) | w::EventEntry(hit.slot_flags));
      }
    } else {
      if (!own_submit && plant != Plant::CountEverySubmit) { ++stats_.other_pass; return res; }   // another pass of the weapon's model (slot 19 / 32): not a second chair, not recorded
      called_[size_t(it - named_.begin())] = 1;
    }
    if (!shadow_call) ++stats_.weapon_calls;
    // S111 CAGESIDE review: a stage piece's instance counts its calls of BOTH families in one number (the plan draws both, and the drawer's pose slot is (PACH, k, instance): rf_props.cpp ApplyWeaponRows, pool
    // weapon:stg<PACH>/<k>, slot #instance). S103 CGF counted per family, so an own and a slot-32 call of one model took the same slot: the second pose overwrote the first. Plant::StageInstancePerFamily = that count.
    const uint64_t stage_key = (uint64_t(model) << 32) | ((plant == Plant::StageInstancePerFamily && !own_submit) ? 0u : 1u);
    const uint32_t instance = shadow_call ? NextInstanceKey((uint64_t(model) << 32) | (shadow == 1 ? 4u : 5u)) : (is_gm || is_blood) ? NextInstanceKey((uint64_t(model) << 32) | (is_blood ? 2u : 3u)) : is_stage ? NextInstanceKey(stage_key) : is_event ? ev_instance : NextInstance(model);
    if (is_stage && !shadow_call && plant == Plant::StageFirstCallOnly && NextInstanceKey((uint64_t(model) << 32) | (own_submit ? 6u : 7u)) > 0) return res;   // (the twin: a later call of the model in its family)
    res.instance = instance;
    res.event = is_event, res.id = hit.id, res.k = (is_stage || is_blood) ? w::StageEntry(hit.slot_flags) : is_event ? w::EventEntry(hit.slot_flags) : 0u;   // (StageEntry = bits 16-23: a blood splat's texture index too)
    res.vtable_ok = (hit.slot_flags & w::kCallVtableOk) != 0;
    if (!res.vtable_ok) ++stats_.bad_vtable_calls;
    if (plant == Plant::DedupeObjectR5) {
      const uint64_t key = (uint64_t(model) << 32) | palette_ptr;
      for (uint64_t k : dedupe_)
        if (k == key) return res;
      dedupe_.push_back(key);
    }
    if (cur_.calls.size() >= kMaxCalls) { ++stats_.overflow; return res; }
    w::WeaponCall e;
    e.object = model;
    e.instance = (is_blood && ::rdr_scene::floor_blood::On()) ? blood_id : instance;   // S111 BLOODMOD2: a blood row's instance is its splat's identity while the permanent blood mod is on (the drawer poses blood by its own pool counter, rf_props.cpp ApplyWeaponRows)
    e.frame = plant == Plant::NoFrame ? 0u : uint32_t(cur_.frame);   // the frame of the call (a record can hold more than one: the drawer takes the newest)
    e.id = plant == Plant::NoIdAtCall ? -1 : hit.id;                 // the id of this object in THIS frame's table (an event prop: its prop id)
    e.slot_flags = (is_event || is_stage || is_gm || is_blood) ? hit.slot_flags : w::PackSlotFlags(hit.slot_flags & 0xFFu, (hit.slot_flags & w::kCallSecondary) != 0, (hit.slot_flags & w::kCallVtableOk) != 0, w::CallSecondaryIndex(hit.slot_flags));
    if (shadow_call) e.slot_flags |= w::kCallShadowPass | (shadow == 1 ? w::kCallShadowProps : 0u);
    if (!is_stage && !is_gm && !is_blood) e.slot_flags |= propspec::PackSpec(propspec::TheStore().CodeFor(model));   // S111 PROPSPEC: the game's run-time set on this MODEL (0 = none), on an event row AND a weapon-manager slot row: the steps' entrance object keeps the slot's main model (sub_825E2DE0 -> sub_825E0F60 -> sub_83422628 +F8)
    e.node_first = uint32_t(cur_.nodes.size());
    e.node_count = 0;
    uint32_t aw = 0;
    if (is_gm || is_blood) e.alpha = f1;   // S103 GBR: f1 = [model+252] at the call (the set object's fade / the splat's record alpha)
    else if (is_stage && (hit.slot_flags & w::kCallAlphaFromF1) && plant != Plant::StageAlpha252) e.alpha = f1;   // S103 CGF (Adversary 3 A): a slot-32 submit's f1 = MdlAlpha is the call's own (0.16 / [0x835E3038] * 0.01 for the cage, table * f31 for the cell), NOT [model+252] (the slot-13 f1)
    else if (read_word(model + ringrows::kModelAlphaOffset, aw)) std::memcpy(&e.alpha, &aw, 4);
    else ++stats_.alpha_unread;
    int32_t n = 0;
    uint32_t flags = 0;
    ringrows::ForeignForward foreign;
    bool data_leaf = false;
    bool counted = ringrows::NodeCount(read_word, model, n, flags, &foreign, &data_leaf);
    if (counted && data_leaf && plant == Plant::StrictForward) counted = false;   // planted: the data object is not followed
    res.counted = counted, res.n = n, res.data_leaf = counted && data_leaf;
    if (foreign.seen) ++stats_.foreign_fwd, res.fwd_vtable = foreign.vtable, res.fwd_fn = foreign.fn;
    if (flags & kRrCountForwarded) ++stats_.forwarded;
    if (res.data_leaf) ++stats_.data_leaf, res.fwd_fn = skin::kSkinDataCountFn, res.fwd_vtable = skin::kSkinDataVtable;
    if (counted && n < -1) ++stats_.bad_count;   // a count below -1 is not a number the game's code returns: unknown, not a row count
    if (!counted || n < -1 || (n <= 0 && plant == Plant::RigidAsUnknown)) {
      ++stats_.count_unknown;
    } else if (n == 0) {
      // S103 WC R2: n == 0 = the game copies NOTHING (the weapon draw memcpy's n << 6 bytes, sub_825DFA78 +440): a counted call with no rows, never a guessed row.
      ++stats_.zero_nodes;
    } else {
      // S103 LVF: NodeCount answers -1 (counted) for a model with no skeleton ([+296] == 0 and [+220] == 0): a RIGID prop (chair, ladder, table). It has ONE node: the one its part 0 names
      // (the bone list at [model+824] -> +8 count, +12 first node: the arena_world_hook.h rule; S103 WC R2: when [m+824] is 0 the game's own fallback descriptor [[D+12]+20] with D = [m+220]), read from the palette at 64 * (node - 1);
      // node 1 when part 0 cannot be read (counted, flagged). Every OTHER n (S103 WC R2) is the game's count: that many rows.
      const bool rigid = n == -1;
      uint32_t count = rigid ? 1u : uint32_t(n);
      uint32_t palette = palette_ptr;
      // review 5: the slot-32 stack palette of the cage and of the cell's three submit sites is 2 rows [C for the cage; the cell's length is [U], capped alike by decision]
      const bool cap2 = is_stage && !own_submit && (lr == w::kLrCage36 || lr == w::kLrCell1 || lr == w::kLrCell2 || lr == w::kLrCell3);
      if (rigid) {
        ++stats_.rigid;
        res.rigid = true;
        uint32_t desc = 0, bones = 0, node = 0, dobj = 0, hdr = 0;
        if (!(read_word(model + 824, desc) && desc >= 0x10000)) {
          const bool via_d = read_word(model + 220, dobj) && dobj >= 0x10000 && read_word(dobj + skin::kSkinDataBlockOff, hdr) && hdr >= 0x10000 && read_word(hdr + skin::kSkinDataPartsOff, desc) && desc >= 0x10000;
          if (!via_d) desc = 0;
        }
        if (palette_ptr != 0 && desc >= 0x10000 && read_word(desc + 8, bones) && bones > 0 && read_word(desc + 12, node) && node >= 1 && node < 1024) {
          if (cap2 && plant != Plant::StageNoRigidBound && node > w::kStageCagePaletteRows) ++stats_.stage_rigid_bounded, ++stats_.rigid_node_assumed;   // review 5: past the 2-row stack palette: node 1 is assumed, never the caller's stack
          else palette = palette_ptr + 64u * (node - 1u);
        } else ++stats_.rigid_node_assumed;
      }
      if (plant == Plant::RingCap96 && count > kRingMaxNodes) count = kRingMaxNodes, ++stats_.capped;   // the planted twin: the old cut at the ring's 96
      const bool over_bound = count > kWeaponMaxNodes;   // S108 FAN: a garbage count (no file model needs more than 731): refused, no rows, never cut
      if (over_bound) {
        ++stats_.capped;
        res.refused = kRefusedOverBound, res.pool_used = uint32_t(cur_.nodes.size());
      } else {
        if (cap2 && plant != Plant::StageNoCap && count > w::kStageCagePaletteRows) count = w::kStageCagePaletteRows, ++stats_.stage_capped;   // S103 CGF: the cage's stack palette holds exactly 2 rows (sub_826F9C40's loop); a larger NodeCount would read the caller's own stack
        if (plant == Plant::NodeCountPlus1) ++count;
        // S108 FAN: an own submit replacing an earlier non-own row of the same event model rewrites that row's pool rows in place (same model, same palette, same frame: the same count), so the frame holds ONE copy
        const bool in_place = reuse_count != 0 && reuse_count == count && plant != Plant::EventRowsAppend;
        if (!in_place && cur_.nodes.size() + count > kMaxPoolNodes) {   // the frame's pool is full: the call is refused WHOLE (never cut short), counted and returned for the glue's log line
          ++stats_.overflow;
          res.refused = kRefusedPoolFull, res.pool_used = uint32_t(cur_.nodes.size());
          return res;
        }
        mats_.clear();
        if (palette_ptr == 0 || !read_mats(palette, count, mats_) || mats_.size() < count) {
          ++stats_.unreadable;
        } else {
          const uint32_t first = in_place ? reuse_first : uint32_t(cur_.nodes.size());
          if (!in_place) cur_.nodes.resize(cur_.nodes.size() + count);
          if (plant == Plant::LateRead) pending_.push_back(Pending{cur_.frame, palette, first, count});   // zero rows now, filled at the next call
          for (uint32_t i = 0; i < count; ++i) {
            RingNodeRows r{};
            if (plant != Plant::LateRead) arenaworld::RowsFromMatrix(mats_[i].m, r.rows);
            cur_.nodes[first + i] = r;
          }
          e.node_first = first, e.node_count = count;
          stats_.rows_copied += count;
          if (in_place) ++stats_.event_rows_in_place;
          if (count > stats_.max_rows) stats_.max_rows = count, stats_.max_rows_id = hit.id, stats_.max_rows_k = res.k, stats_.max_rows_event = is_event;   // S108 FAN review
        }
      }
    }
    if (is_blood && e.node_count == 1 && blood_br.f_ok && ::rdr_scene::floor_blood::On()) NoteBloodRef(blood_br, cur_.nodes[e.node_first].rows);   // S111 BLOODMOD3 (the mod only): the drawn splat is the held rows' reference
    cur_.calls.push_back(e);
    ++stats_.recorded;
    res.recorded = true, res.n_nodes = e.node_count;
    return res;
  }

  // lane 152 (review B1) rule: the record writer closes every open frame older than the present's guest frame.
  void CloseBefore(uint64_t frame) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_.valid && cur_.frame < frame) Advance(frame);
  }
  // The oldest closed frame not yet taken; false when none. Swaps the buffer out (no copy).
  bool TakeNext(FrameBuffer& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (taken_ >= closed_count_) return false;
    FrameBuffer& b = ring_[taken_ % kRing];
    std::swap(out, b);
    b.Clear();
    ++taken_;
    return true;
  }
  std::string LockText() const { return lock_stats_.Text(); }   // BHF1 B2: the model-call lock cost (census: model_lock)
  Stats GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
  }
  // S103 CGF: the stage holder's members as of this frame's pull (the contributor's section-59 prefetch rows).
  StageView StageSnapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stage_;
  }

 private:
  struct Pending { uint64_t frame; uint32_t r5, first, count; };

  // S103 GBR (BH2 G2 / BH4 item 3): once a frame. The scene signature = the stage holder's pointer and the weapon manager's pointer (review 3: never the holder's loaded / type / count words): a change (a stage freed / made, an arena or match change) clears the event-prop list
  // (event props never outlive the arena); then every entry whose model's vtable word is no longer a CSkinModel's is pruned. [L] that the holder's words change at a stage change (the free zeroes the slots, the
  // loaded flag drops); the step check reads `event list cleared N` in the weapon_rows census.
  template <class ReadWordFn>
  void NoteScene(ReadWordFn& read_word) {
    uint32_t h = 0, mgr = 0;   // review 3: ONLY the holder pointer and the weapon manager pointer: the holder finishing its non-blocking read mid-entrance ([H+172] / type / count move) must not clear live props
    read_word(w::kHolderGlobal, h), read_word(w::kMgrGlobal, mgr);
    uint32_t loaded = 0;
    if (plant == Plant::SceneSigLoaded && h >= 0x10000) read_word(h + w::kHolderOffLoaded, loaded);   // (planted twin only)
    const uint64_t sig = (uint64_t(h) << 32) ^ uint64_t(mgr) ^ (uint64_t(loaded) << 24) ^ 0x8000000000000000ull;
    // S103 SMF (BUG2 7): a 0 -> X FIRST write of either pointer (the weapon manager made after the entrance began, a cage holder made mid-entrance) is not a scene change: live props must not leave the list. A change from a non-zero
    // value (another scene, or freed to 0) still clears. (Plant::FirstWriteClears = the old rule, the planted twin.)
    const bool first_write = (h != scene_h_ && scene_h_ == 0) || (mgr != scene_mgr_ && scene_mgr_ == 0);
    const bool moved = (h != scene_h_ && scene_h_ != 0) || (mgr != scene_mgr_ && scene_mgr_ != 0);
    const bool only_first = first_write && !moved && plant != Plant::FirstWriteClears;
    if (scene_sig_valid_ && sig != scene_sig_ && !only_first) {
      ++stats_.ev_scene_changes;
      if (plant != Plant::EventNoClear) stats_.ev_cleared += events.Clear();
    }
    scene_sig_ = sig, scene_sig_valid_ = true, scene_h_ = h, scene_mgr_ = mgr;
    if (plant != Plant::EventNoPrune) stats_.ev_pruned += events.Prune(read_word);
    events.ReadLightSets(read_word);   // S111 STAGELESS2 (prop_light_id.h): the set each live event CWeapon asks for, onto its models (the section-59 rows carry it)
  }

  // The gm set objects' model set (GMS.md (A)): L = [0x8376A9A0] is a plain array of 12 controller words; each controller holds a 512-word object table at +2120; an object's model is [obj+48] and its key [obj+44].
  // The 12 controller words are read every frame (the signature); the tables are rebuilt when the signature changes and every 30 frames (a controller can refill its table in place), sorted by model.
  template <class ReadWordFn>
  void PullGm(ReadWordFn& read_word, uint64_t frame) {
    {   // S111 GMBORROW: settings byte G+41, once a frame (sub_827C9190 hands the stage holder's texture set when it is non-zero, read once per controller at the bank load). The game writes
        // G+41 at match set-up only (the apply sub_822CA008 copies the type's rule row over G+0..63; Create a Finisher's set-up sub_82270AB8 clears it), so the frame read equals the game's load read.
      uint32_t gw = 0;
      gm_holder_tex_ = plant == Plant::GmHolderAlways || (read_word(w::kGmHolderTexWord, gw) && ((gw >> w::kGmHolderTexShift) & 0xFFu) != 0);
    }
    uint32_t l = 0;
    if (!read_word(w::kGmListGlobal, l) || l < 0x10000) { gm_.clear(), gm_sig_ = 0, stats_.gm_models = 0; return; }
    uint32_t ctl[w::kGmCtlCount] = {};
    uint64_t sig = 1469598103934665603ull ^ l;
    bool any = false;
    for (uint32_t i = 0; i < w::kGmCtlCount; ++i) {
      if (!read_word(l + 4 * i, ctl[i])) ctl[i] = 0;
      sig = (sig ^ ctl[i]) * 1099511628211ull;
      any = any || ctl[i] >= 0x10000;
    }
    if (!any) { gm_.clear(), gm_sig_ = sig, stats_.gm_models = 0; return; }
    if (sig == gm_sig_ && frame - gm_pull_frame_ < 30 && (plant != Plant::GmRescanEmpty || !gm_.empty())) return;   // review 2: an empty set is not rescanned every frame either
    gm_sig_ = sig, gm_pull_frame_ = frame;
    ++stats_.gm_pulls;
    gm_.clear();
    for (uint32_t i = 0; i < w::kGmCtlCount; ++i) {
      if (ctl[i] < 0x10000) continue;
      for (uint32_t j = 0; j < w::kGmTableLen; ++j) {
        uint32_t obj = 0, m = 0, key = 0;
        if (!read_word(ctl[i] + w::kGmTableOff + 4 * j, obj) || obj < 0x10000) continue;
        if (!read_word(obj + w::kGmObjOffModel, m) || m < 0x10000 || !read_word(obj + w::kGmObjOffKey, key)) continue;
        if (!((key >= 1000 && key <= 1255) || (key >= 1500 && key <= 1755))) continue;   // review 7: only the two key ranges the game builds (sub_827C8EB0: T[0..255] = 1000.., T[256..511] = 1500..)
        gm_.push_back({m, obj, key});
      }
    }
    std::sort(gm_.begin(), gm_.end(), [](const GmMember& a, const GmMember& b) { return a.model < b.model; });
    gm_.erase(std::unique(gm_.begin(), gm_.end(), [](const GmMember& a, const GmMember& b) { return a.model == b.model; }), gm_.end());
    stats_.gm_models = uint32_t(gm_.size());
  }

  // The blood pool's decal records, snapshotted at the first splat call of the frame (GMS.md (B)): Q = [0x837689D4], 30 records of 48 bytes at Q+2448: +20 alpha (0.0 = dead [L]), +24 colour, +32 texture index.
  // The game's loop (0x82697A40..0x82697C18) walks the records in order and draws the live ones, one model call each: the n-th splat call takes the n-th live record.
  template <class ReadWordFn>
  void PullBloodRecs(ReadWordFn& read_word) {
    blood_recs_frame_valid_ = true, blood_recs_ok_ = false;
    blood_recs_.clear();
    uint32_t q = 0;
    if (!read_word(w::kBloodPoolGlobal, q) || q < 0x10000) return;
    uint32_t dead = 0;
    const bool ids_on = blood_enabled && ::rdr_scene::floor_blood::On();   // S111 BLOODMOD2 (the permanent blood mod only): each row carries its particle slot and its splat's identity
    const bool replay = ids_on && blood_ids_.ReplayOn(read_word);         // [Q + 3888]: the replay track writes the records this frame (no record is its particle's own)
    for (uint32_t k = 0; k < w::kBloodRecCount; ++k) {
      const uint32_t at = q + w::kBloodRecOff + w::kBloodRecStride * k;
      uint32_t aw = 0, cw = 0, tw = 0;
      if (!read_word(at + w::kBloodRecOffAlpha, aw) || !read_word(at + w::kBloodRecOffColour, cw) || !read_word(at + w::kBloodRecOffTex, tw)) return;
      BloodRec r;
      std::memcpy(&r.alpha, &aw, 4), std::memcpy(&r.colour, &cw, 4);
      r.tex = tw < 16u ? tw : 0u;
      if (ids_on) r.slot_bits = k, r.id = blood_ids_.IdOf(read_word, k, aw, replay);   // S111 BLOODMOD2 (0 = a played-back record, or not the particle's own copy)
      if (ids_on) r.f_ok = bloodheld::ReadRecordFields(read_word, at, r.f);   // S111 BLOODMOD3: the fields the draw builds this record's rows from
      if (r.alpha == 0.0f) {   // dead: the game's loop skips a record whose alpha EQUALS the float at 0x8218B8CC = 0.0 [C: the image word is 0x00000000; fcmpu, so -0.0 is skipped too and a NaN alpha is NOT skipped (the plan still refuses an alpha not > 0)]
        ++dead;
        if (plant != Plant::BloodIncludeDead) continue;
      }
      blood_recs_.push_back(r);
    }
    blood_recs_ok_ = true;
    stats_.blood_dead_skipped += dead, stats_.blood_live = uint32_t(blood_recs_.size());
    if (!blood_recs_.empty()) ++stats_.blood_frames;
  }

  struct GmMember { uint32_t model, obj, key; };
  struct BloodRec { float alpha = 0.f, colour = 1.f; uint32_t tex = 0, slot_bits = 0, id = 0; bloodheld::Fields f; bool f_ok = false; };   // S111 BLOODMOD2: + the particle slot k and the splat's identity; BLOODMOD3: + the record's place / scale / angle (mod on only)

  // S111 BLOODMOD3 (docs\research\s111\BLOODMOD3.md; the permanent blood mod only). The pool update sub_826983B8 steps every live particle's life BEFORE it tests the replay word [Q + 3888]; only the copy into
  // the records waits on it. So during an instant replay a splat can fade and free while its record shows the playback, and the game never draws it fading. Once a frame (after Scan) each particle in its fade
  // (state 3, 0 < alpha < 1) whose record is not its own copy (the replay word, or the alpha bits differ) gets a keep-only row (kCallBloodHeld): its identity, colour and texture, rows made from the last drawn
  // splat (blood_held.h SynthRows). A particle whose record is its own is the game's drawn row (PullBloodRecs), never a held one.
  template <class ReadWordFn>
  void EmitHeldSplats(ReadWordFn& read_word) {
    uint32_t q = 0;
    if (blood_model_ == 0 || !read_word(w::kBloodPoolGlobal, q) || q < 0x10000) return;
    const bool replay = blood_ids_.ReplayOn(read_word);
    for (uint32_t k = 0; k < w::kBloodRecCount; ++k) {
      const uint32_t id = blood_ids_.LiveId(k);
      if (id == 0) continue;
      bloodheld::Live p;
      if (!bloodheld::ReadParticle(read_word, q, k, p) || p.state != bloodheld::kStateFade || !(p.alpha > 0.f && p.alpha < 1.f)) continue;
      uint32_t rec_alpha = 0;
      if (!replay && read_word(q + w::kBloodRecOff + w::kBloodRecStride * k + w::kBloodRecOffAlpha, rec_alpha) && rec_alpha == p.alpha_bits) continue;   // its own record: the game draws it
      RingNodeRows rows{};
      if (!blood_ref_.ok || blood_ref_.model != blood_model_ || blood_ref_.pools != blood_ids_.Pools() || !bloodheld::SynthRows(blood_ref_.rows, blood_ref_.f, p.f, rows.rows)) {
        ++stats_.blood_held_noref;
        continue;
      }
      if (cur_.calls.size() >= kMaxCalls || cur_.nodes.size() + 1 > kMaxPoolNodes) { ++stats_.overflow; return; }
      w::WeaponCall e;
      e.object = blood_model_, e.instance = id, e.frame = uint32_t(cur_.frame), e.id = w::BloodIdOf(p.colour), e.alpha = p.alpha;
      e.slot_flags = w::PackBloodFlags(p.tex) | k | w::kCallBloodHeld;
      e.node_first = uint32_t(cur_.nodes.size()), e.node_count = 1;
      cur_.nodes.push_back(rows);
      cur_.calls.push_back(e);
      ++stats_.blood_held;
    }
  }
  // A drawn splat's rows and the record fields they were drawn from: the held rows' reference (the same blood model and scene count). Before it is taken, the rows made from the previous reference for THIS
  // record are compared with the game's own (census synth_checks / synth_err: the composition rule checked on the game's draws).
  void NoteBloodRef(const BloodRec& br, const float rows[12]) {
    const uint32_t pools = blood_ids_.Pools();
    float want[12];
    if (blood_ref_.ok && blood_ref_.model == blood_model_ && blood_ref_.pools == pools && bloodheld::SynthRows(blood_ref_.rows, blood_ref_.f, br.f, want)) {
      float err = 0.f;
      for (int i = 0; i < 12; ++i) err = std::max(err, std::fabs(want[i] - rows[i]));
      ++stats_.blood_synth_checks;
      if (err > stats_.blood_synth_err) stats_.blood_synth_err = err;
    }
    if (!(br.f.scale > 0.f)) return;
    blood_ref_.ok = true, blood_ref_.model = blood_model_, blood_ref_.pools = pools, blood_ref_.f = br.f;
    std::memcpy(blood_ref_.rows, rows, sizeof blood_ref_.rows);
  }
  std::vector<GmMember> gm_;
  bool gm_holder_tex_ = false;   // S111 GMBORROW: settings byte G+41 at the last pull (PullGm)
  uint64_t gm_sig_ = 0, gm_pull_frame_ = 0, scene_sig_ = 0, scene_frame_ = 0;
  uint32_t scene_h_ = 0, scene_mgr_ = 0;   // S103 SMF (BUG2 7): the last holder / manager pointers, to tell a first write (0 -> X) from a change
  bool scene_sig_valid_ = false, scene_frame_valid_ = false;
  uint32_t blood_model_ = 0, blood_next_ = 0;
  bool blood_recs_frame_valid_ = false, blood_recs_ok_ = false;
  std::vector<BloodRec> blood_recs_;
  bloodid::Slots blood_ids_;   // S111 BLOODMOD2: the particles' serials (scanned once a frame while the permanent blood mod is on)
  struct BloodRef { bool ok = false; uint32_t model = 0, pools = 0; bloodheld::Fields f; float rows[12] = {}; };
  BloodRef blood_ref_;         // S111 BLOODMOD3: the last drawn splat's rows and record fields (the held rows' reference; mod on only)

  // The n-th recorded call of this object in this frame: two chairs = 0 and 1.
  uint32_t NextInstance(uint32_t model) { return NextInstanceKey(uint64_t(model) << 32); }
  uint32_t NextInstanceKey(uint64_t key) {
    for (auto& c : instances_)
      if (c.first == key) return c.second++;
    instances_.push_back({key, 1});
    return 0;
  }
  // Closes the frame's census: which named objects were called.
  void CloseCensus() {
    uint32_t never = 0, objs = 0;
    for (size_t i = 0; i < named_.size() && i < called_.size(); ++i) {
      if (called_[i]) ++objs;
      else ++never;
    }
    cur_.named = uint32_t(named_.size()), cur_.never_drawn = never, cur_.objects = objs;
    if (members_frame_valid_ && members_frame_ == cur_.frame) {
      stats_.named += named_.size(), stats_.never_drawn += never, stats_.objects += objs;
      stats_.last_named = named_.size(), stats_.last_never = never;
    }
  }
  // Returns false for a call of an already closed frame.
  bool Advance(uint64_t frame) {
    if (!cur_.valid) {
      cur_.valid = true, cur_.frame = frame;
      return true;
    }
    if (frame < cur_.frame) return false;
    if (frame > cur_.frame) {
      if (closed_count_ - taken_ >= kRing) {   // the oldest closed frame is overwritten before Emit took it
        ++taken_;
        ++stats_.late;
      }
      CloseCensus();
      std::swap(ring_[closed_count_ % kRing], cur_);   // keeps every buffer's capacity
      ++closed_count_;
      cur_.Clear();
      cur_.valid = true, cur_.frame = frame;
      instances_.clear(), dedupe_.clear(), pending_.clear(), ev_seen_.clear();
      if (plant != Plant::CachedMembers) named_.clear(), called_.clear();
      ++stats_.frames;
    }
    return true;
  }

  mutable std::mutex mu_;
  LockStats lock_stats_;   // BHF1 B2: OnModelCall lock cost counters
  FrameBuffer cur_, ring_[kRing];
  std::vector<Named> members_, named_;
  live::weapons::BeltKeyCache belt_keys_;   // S103 LDR: the belt sub-key per belt object address
  StageView stage_;                    // S103 CGF: the holder's members, pulled once a frame
  uint64_t stage_frame_ = 0;
  bool stage_frame_valid_ = false;
  std::vector<w::WeaponSlot> slots_;   // scratch for ReadNamed
  std::vector<uint8_t> called_;
  uint64_t members_frame_ = 0;
  bool members_frame_valid_ = false;
  std::vector<std::pair<uint64_t, uint32_t>> instances_;   // (object << 32) -> calls so far; event props: (1<<63 | id << 8 | k) -> models so far
  struct EvSeen { uint32_t model; bool own; };
  std::vector<EvSeen> ev_seen_;   // event models already given a row this frame (own = that row came from the own submit)
  std::vector<uint64_t> dedupe_;
  std::vector<Pending> pending_;
  std::vector<Mat4> mats_;
  uint64_t closed_count_ = 0, taken_ = 0;
  Stats stats_;
};

// S103 STG (PMW.md section 3 (a)): is the calling thread inside the game's shadow-map render? `slot` = the thread slot (caster_core.h ReadThreadSlot: [[[r13]+1048]+32]). 0 = colour (flag A, u32 [0x83929A48 + 4 * slot], is 0), 1 = flag A set and the
// thread's current camera [0x83929A40 + 4 * slot] is the props light (shadow module [0x83924748] + 1312), 2 = flag A set, any other camera (map A / B: module + 304). An unreadable flag is 0 (the call is a colour call, as before this lane).
// The addresses are spelled here (this header includes no game header); weapon_rows_glue.h static_asserts them equal to draw_camera_core.h's (kFlagA, kShadowModulePtr, kLightRecordB) and inputs_core.h's kCamTable.
inline constexpr uint32_t kShadowFlagA = 0x83929A48, kShadowCamTable = 0x83929A40, kShadowModulePtr = 0x83924748, kShadowLightRecordB = 1312;
template <class ReadWordFn>
inline uint8_t ShadowKindAt(ReadWordFn read_word, uint32_t slot) {
  uint32_t flag = 0, cam = 0, mod = 0;
  if (!read_word(kShadowFlagA + 4u * slot, flag) || flag == 0) return 0;
  if (read_word(kShadowCamTable + 4u * slot, cam) && read_word(kShadowModulePtr, mod) && mod != 0 && cam == mod + kShadowLightRecordB) return 1;
  return 2;
}

// S111 CAATABLE: the model calls whose live texture keys the key store reads (belt_plate.h Store::OnEventCall: only the indices the drawer asked for): an event prop's, and an announce-table
// weapon row's (desk_keys.h KeysOverFile: the game repaints / rebinds those keys). Plant 14 (desk_not_read) restores the old gate, event props only.
inline bool ReadsKeys(const CallResult& r) {
  if (r.event) return true;
  if (beltplate::g_plate_plant == 14) return false;
  return r.weapon && !r.stage && !r.gm && !r.blood && beltplate::KeysOverFile(r.id);
}

// The first lines of the raw hook on sub_82C04DF0 (draw_origin.h), kept pure so the offline unit runs the REAL order: the rows are taken BEFORE the scene_skip::Ours(Model) test (under OURS the original never runs,
// and neither does the mid-asm hook inside it: the stream-free build recorded nothing: PW review fix 1). Returns true when the original must run (not OURS). `registered` = WeaponRowsRegistered() && Enabled().
template <class ReadWordFn, class ReadMatsFn>
inline bool RawHookEntry(Tracker& tr, bool registered, bool ours, uint32_t r3, uint32_t r5, uint64_t frame, ReadWordFn read_word, ReadMatsFn read_mats, CallResult* out, float f1, uint32_t lr, bool (*pass0)(void*), void* pass_ctx, uint8_t shadow, bool (*plate_phys)(uint32_t, uint32_t, uint8_t*)) {   // S105 PLATE review 6: no defaults, so a glue edit that drops plate_phys cannot compile
  if (tr.plant == Plant::RowsAfterOursCheck && ours) return false;   // the planted bug: the rows are a feature of the stream's path only
  if (registered) {
    const CallResult r = tr.OnModelCall(r3, r5, frame, IsOwnSubmit(r3), read_word, read_mats, f1, lr, IsOwnSubmit(r3) ? t_slot13_caller : 0u, pass0, pass_ctx, shadow);   // S103 GBR: pass0 = "the thread's pass index (sub_82BA77A8) is 0", asked only for a gm set object / a splat   // S103 CGF (review 3: the own submit's caller): f1 (MdlAlpha) and the guest LR ride along for the stage structures' family tag
    if (out) *out = r;
    if (ReadsKeys(r) && plate_phys) beltplate::TheStore().OnEventCall(r3, read_word, plate_phys);   // S105 PLATE: the belt's nameplate, read once per model on the game thread (both builds: the rows are taken before the OURS test)   // S111 CAATABLE: and the announce table's keys
    if (r.event) {   // S108 THB: the Threads editor recolours its entrance prop at this very draw (callback 0x8338EC00): its 4 colours, read where the game hands them to the GPU (one word when the switch is off)
      const propcolour::EditorSample es = propcolour::ReadEditor(read_word, r3);
      propcolour::TheStore().NoteEditor(r3, frame, es);
      beltcolour::OnEditorDraw(read_word, r3, r.id, es);   // S111 CACSTRAP: the Create a Championship preview of a STOCK belt (callback 0x833445D0): C's colours now, as the entrance belt's mode-0 row
    }
  }
  return !ours;
}

}  // namespace rdr_scene::live::weaponrows
