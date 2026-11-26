// S111 PROPSPEC (docs\research\s111\PROPSPEC.md): the game's run-time specular / reflection override of an entrance's props, per prop model. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no game or SDK includes: the raw hooks' glue (weapon_rows_glue.h PropSpecBind / PropSpecRelease), the tracker (weapon_rows_core.h: the row flags of an event prop / slot weapon model), the drawer (rf_props.cpp: the
// bound copy's materials) and the offline unit (tools\weapons\prop_spec_unit.cpp) run THIS code.
//
// What the game does [C, listings; our words]:
//   The entrance prop command sub_82335720 requests up to 8 props (sub_82334BB8), then installs the wait callback sub_823356D8: once every requested slot of the 72-word prop table 0x836F4B40 holds its
//   object it runs sub_82335268 ONCE. That walks the table. A prop whose id (CWeapon +44, set by the base ctor sub_83422628 +10C from the descriptor's id) is 32 (the ring steps, a stage weapon that
//   sub_8258C590 makes through sub_825E2DE0) gets g_fSpecularLev 0.3 / g_iSpecularPow 40 / g_fReflectAlpha 0.3 (the floats at 0x82004DDC / 0x82003E30), or 0 / 0 / 0 (0x8218B8CC) when the scene's
//   slot-0 entrance bank (sub_82310080, is_equal_104 sub_82310F38) is 104; with bank 104 every other prop that has a /BOBJ/ row (a title belt, sub_82310000) gets 0 / 0 / 0 too.
//   Each value goes through sub_82335070(override, CWeapon, name, kind 10, &value, 1): it keeps the CWeapon at +4, copies the name and the value into its record at +8 ({name[16], u16 kind, ..., floats at
//   +20}) and binds the record (sub_823349B0 -> sub_82BD6378) into every material slot of the CWeapon's model [CWeapon + 48] whose parameter of that name and kind exists, so the draw reads the override in
//   place of the file value. The command's destructor releases them (sub_82335208 -> sub_82334AB8: each bound slot set back to 0, the file value again).
// Ours: the two raw hooks hand (model, name, value) and the release to this store; every event prop / slot weapon row carries a 2-bit code (kCallSpecShift) of the set its model is under now; the drawer builds the
// bound copy of a coded call from the stored model with the set's values (its own pool, so the plain copy is untouched). Read only: nothing is written to the guest.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../assets/asset_model.h"

namespace rdr_scene::live::propspec {

inline constexpr uint32_t kWeaponOffModel = 48;   // [C] sub_82335070 +170..+178: r4 of the binder = [[override + 4] + 48]; sub_83422628 +F8 / +FC: [W + 48] = [W + 1228] = the descriptor's model
inline constexpr uint32_t kWeaponOffId = 44;      // [C] sub_83422628 +10C: [W + 44] = the descriptor's prop id; sub_82335268 +F0 tests it against 32
inline constexpr uint32_t kOvrOffWeapon = 4;      // [C] sub_82335070 +10: [override + 4] = r4 (the CWeapon)
inline constexpr uint32_t kOvrOffRecord = 8;      // [C] sub_82335070 +C0 / +D8: [override + 8] = the record, its first 16 bytes the name (strcpy_bounded 16)
inline constexpr uint32_t kNameBytes = 16;

// The three names sub_82335268 binds (the string pointers at 0x835DFBB4 / 0x835DFBB8 / 0x835DFBBC -> 0x8200A5A8 / 0x8200A598 / 0x8200A588).
enum NameIx : int { kLev = 0, kPow = 1, kRefl = 2, kNames = 3 };
inline int NameIndex(const char* s) {   // s: kNameBytes bytes, NUL-terminated within them or not
  char b[kNameBytes + 1] = {};
  std::memcpy(b, s, kNameBytes);
  if (std::strcmp(b, "g_fSpecularLev") == 0) return kLev;
  if (std::strcmp(b, "g_iSpecularPow") == 0) return kPow;
  if (std::strcmp(b, "g_fReflectAlpha") == 0) return kRefl;
  return -1;
}

// The sets: 1 = the steps set (0.3 / 40 / 0.3), 2 = the off set (0 / 0 / 0, entrance bank 104), 3 = three values the game's caller never passes (drawn as the file, counted); 0 = none (or not all three bound yet).
inline constexpr uint32_t kSpecNone = 0, kSpecSteps = 1, kSpecOff = 2, kSpecOther = 3;
inline constexpr float kStepsLev = 0.3f, kStepsPow = 40.f, kStepsRefl = 0.3f;   // [C] 0x82004DDC = 0.3, 0x82003E30 = 40 (sub_82335268 +114..+120)

// The row layout: bits 30-31 of an event prop's or a slot weapon's WeaponCall::slot_flags (weapon_rows.h: bits 0-29 are taken; 30-31 were reserved 0, so a reader that does not know them sees a plain row).
inline constexpr uint32_t kCallSpecShift = 30, kCallSpecMask = 0x3;
inline constexpr uint32_t PackSpec(uint32_t code) { return (code & kCallSpecMask) << kCallSpecShift; }
inline constexpr uint32_t SpecCodeOf(uint32_t slot_flags) { return (slot_flags >> kCallSpecShift) & kCallSpecMask; }

struct Values {
  uint8_t have = 0;   // bit n = name n bound
  float v[kNames] = {};
};
inline bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f; }
inline uint32_t CodeOf(const Values& x) {
  if (x.have != 7u) return kSpecNone;   // the three binds run back to back in one call of sub_82335268: a partial set is that call in flight (at most one frame)
  if (Near(x.v[kLev], 0.f) && Near(x.v[kPow], 0.f) && Near(x.v[kRefl], 0.f)) return kSpecOff;
  if (Near(x.v[kLev], kStepsLev) && Near(x.v[kPow], kStepsPow) && Near(x.v[kRefl], kStepsRefl)) return kSpecSteps;
  return kSpecOther;
}

// model -> the values bound to it now. Bind / Release run on the game thread of the scene command (the raw hooks); CodeFor on the draw hook's threads (lock free, as EventProps::Find).
class Store {
 public:
  static constexpr uint32_t kPubSlots = 32;   // a scene binds at most 72 props x 3 names; the coded ones are a handful
  void Bind(uint32_t model, int name, float v, int32_t prop_id = -1) {
    if (model == 0 || name < 0 || name >= kNames) return;
    std::lock_guard<std::mutex> lk(mu_);
    Values& x = map_[model];
    x.have = uint8_t(x.have | (1u << name)), x.v[name] = v;
    ids_[model] = prop_id;
    ++binds_;
    Publish();
  }
  // name -1 = every value of the model (the record's name was not readable).
  void Release(uint32_t model, int name) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(model);
    if (it == map_.end()) return;
    it->second.have = (name >= 0 && name < kNames) ? uint8_t(it->second.have & ~(1u << name)) : uint8_t(0);
    if (it->second.have == 0) map_.erase(it), ids_.erase(model);
    ++releases_;
    Publish();
  }
  void Drop(uint32_t model) {   // the model left the event list (EventProps OnMake / OnFree / Prune): a reused address must not keep the old prop's set
    std::lock_guard<std::mutex> lk(mu_);
    if (map_.erase(model) == 0) return;
    ids_.erase(model);
    ++dropped_;
    Publish();
  }
  void Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    if (map_.empty()) return;
    dropped_ += map_.size();
    map_.clear(), ids_.clear();
    Publish();
  }
  uint32_t CodeFor(uint32_t model) const {
    if (live_.load(std::memory_order_relaxed) == 0) return kSpecNone;
    for (uint32_t i = 0; i < kPubSlots; ++i) {
      const uint64_t p = pub_[i].load(std::memory_order_relaxed);
      if (p != 0 && uint32_t(p >> 32) == model) return uint32_t(p & kCallSpecMask);
    }
    return kSpecNone;
  }
  int32_t PropIdOf(uint32_t model) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = ids_.find(model);
    return it == ids_.end() ? -1 : it->second;
  }
  struct Counts {
    uint64_t binds = 0, releases = 0, dropped = 0, overflow = 0, other = 0;
    uint32_t live = 0;
  };
  Counts Count() const {
    std::lock_guard<std::mutex> lk(mu_);
    Counts c;
    c.binds = binds_, c.releases = releases_, c.dropped = dropped_, c.overflow = overflow_, c.other = other_, c.live = uint32_t(map_.size());
    return c;
  }

 private:
  void Publish() {   // mu_ held
    uint32_t i = 0;
    for (const auto& kv : map_) {
      const uint32_t code = CodeOf(kv.second);
      if (code == kSpecNone) continue;
      if (code == kSpecOther) ++other_;
      if (i >= kPubSlots) { ++overflow_; continue; }
      pub_[i++].store((uint64_t(kv.first) << 32) | code, std::memory_order_relaxed);
    }
    live_.store(i, std::memory_order_relaxed);
    for (; i < kPubSlots; ++i) pub_[i].store(0, std::memory_order_relaxed);
  }
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, Values> map_;
  std::unordered_map<uint32_t, int32_t> ids_;
  std::atomic<uint32_t> live_{0};
  std::atomic<uint64_t> pub_[kPubSlots] = {};
  uint64_t binds_ = 0, releases_ = 0, dropped_ = 0, overflow_ = 0, other_ = 0;
};
inline Store& TheStore() {
  static Store s;
  return s;
}
inline void DropOnMake(uint32_t model) { TheStore().Drop(model); }
inline void DropOnFree(uint32_t model) { TheStore().Drop(model); }
inline void ClearOnScene() { TheStore().Clear(); }

// The raw hook on sub_82335070's entry (before the original): r4 = the CWeapon, r5 = the name, r7 = &value. The game binds nothing when r4, r5 or r7 is 0 (+60 / +68 / +70): neither does this.
// `read_word(addr, v)` = a big-endian guest word as a host value; `read_bytes(addr, dst, n)` = raw guest bytes. True = a value was stored.
struct BindSeen {
  uint32_t model = 0;
  int32_t prop_id = -1;
  int name = -1;
  float value = 0.f;
  uint32_t code = kSpecNone;
};
template <class ReadWordFn, class ReadBytesFn>
bool OnBind(Store& s, ReadWordFn&& read_word, ReadBytesFn&& read_bytes, uint32_t weapon, uint32_t name_ptr, uint32_t value_ptr, BindSeen* seen = nullptr) {
  if (weapon == 0 || name_ptr == 0 || value_ptr == 0) return false;
  uint32_t model = 0, id = 0, bits = 0;
  char nm[kNameBytes] = {};
  if (!read_word(weapon + kWeaponOffModel, model) || model == 0) return false;
  if (!read_bytes(name_ptr, nm, kNameBytes) || !read_word(value_ptr, bits)) return false;
  const int ix = NameIndex(nm);
  if (ix < 0) return false;   // a name sub_82335268 never passes
  float v = 0.f;
  std::memcpy(&v, &bits, 4);
  const int32_t pid = read_word(weapon + kWeaponOffId, id) ? int32_t(id) : -1;
  s.Bind(model, ix, v, pid);
  if (seen) seen->model = model, seen->prop_id = pid, seen->name = ix, seen->value = v, seen->code = s.CodeFor(model);
  return true;
}
// The raw hook on sub_82334AB8's entry (before the original): r3 = the override (its CWeapon at +4, its record at +8).
template <class ReadWordFn, class ReadBytesFn>
bool OnRelease(Store& s, ReadWordFn&& read_word, ReadBytesFn&& read_bytes, uint32_t ovr) {
  uint32_t weapon = 0, model = 0, rec = 0;
  if (ovr == 0 || !read_word(ovr + kOvrOffWeapon, weapon) || weapon == 0 || !read_word(weapon + kWeaponOffModel, model) || model == 0) return false;
  int ix = -1;
  char nm[kNameBytes] = {};
  if (read_word(ovr + kOvrOffRecord, rec) && rec != 0 && read_bytes(rec, nm, kNameBytes)) ix = NameIndex(nm);
  s.Release(model, ix);
  return true;
}

// The drawer: the set's values on every material of the bound copy (the game binds each name only into a material that names it: the steps' three meshes and the stock belts' meshes name all three
// [C asset index: bgEtc p9/32 meshes 0-2, evtobj BOBJ 2004 meshes 0-2], so every material takes them; a material that names none keeps level 0 / reflection 0 under the off set either way).
inline bool SetOf(uint32_t code, float& lev, float& pw, float& refl) {
  if (code == kSpecSteps) return lev = kStepsLev, pw = kStepsPow, refl = kStepsRefl, true;
  if (code == kSpecOff) return lev = 0.f, pw = 0.f, refl = 0.f, true;
  return false;
}
inline uint32_t ApplyToModel(uint32_t code, assets::Model& m) {
  float lev = 0.f, pw = 0.f, refl = 0.f;
  if (!SetOf(code, lev, pw, refl)) return 0;
  for (assets::Material& mat : m.materials) mat.spec_lev = lev, mat.spec_pow = pw, mat.reflect_alpha = refl;
  return uint32_t(m.materials.size());
}
// The pool suffix of a coded call: the overridden copy never shares a pool (or a bound copy) with the file-valued one.
inline std::string PoolSuffix(uint32_t code) { return code == kSpecSteps ? std::string("~spec1") : code == kSpecOff ? std::string("~spec0") : std::string(); }

}  // namespace rdr_scene::live::propspec
