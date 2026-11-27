// S111 FOLLOWUPS3 (docs\research\s111\FOLLOWUPS3.md; READANSWER review F2): which entrance prop models the game built from its RECOLOURED read, noted where the game built them.
// The entrance prop loader sub_8258C590 makes a recoloured prop (request +16 set: a participant with a saved Threads set, sub_82334BB8) through the task sub_83391F18, which reads /SOBJ/<id>
// only, and every other evtobj prop through sub_825AE720, which reads /BOBJ/<id> when that path resolves, else /SOBJ/<id> (the rule of the other evtobj reader request, sub_8233F6E0, too; the loader's stage-weapon
// routes sub_83325540 / sub_825E2DE0 make no evtobj model). So one prop id can be built twice in one scene from two sections; the
// drawer keys each built model by its own read (weapon_draw_core.h EvtEntryOf: the "/s" entry = the recoloured read).
//   NoteFromRecolourCall  the recoloured task's run sub_83391E00 makes its CWeapon W (stored at task + 192), then hands task + 208 to sub_833A1758 (its one caller, no branch between):
//                         the hook of sub_833A1758 (fp_probe.h) passes r3, so W = [r3 - 16] and W's models are the words W + 1228 + 4k (weapon_rows.h kOffEventModels); each non-zero one is noted.
//   Forget                the CWeapon's destructor (weapon_rows_glue.h EventWeaponFreed): its models go, so a later plain prop at a reused address is not taken for recoloured.
//   IsRecoloured          the section-59 writer (weapon_rows_contrib.h WeaponRowsContributor::Emit) asks per event model: flag kSlotFlagRecoloured on its row.
// Header-only, std only. Threads: the game's (note / forget / ask): a mutex, the atomic count read first.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

namespace rdr_scene::live::prop_read_note {

inline constexpr uint32_t kModels = 10;               // W + 1228 + 4k, k = 0..9 (weapon_rows.h kMaxEventModels)
inline constexpr uint32_t kWeaponModelsOff = 1228;    // = weapon_rows.h kOffEventModels (static_assert in weapon_rows_contrib.h)
inline constexpr uint32_t kRecolourObjToWeapon = 16;  // sub_83391E00: r3 of sub_833A1758 = task + 208, the CWeapon word = task + 192
inline constexpr size_t kMaxWeapons = 64;             // a bound: entrance props of one scene are a handful; a stray address stream cannot grow the table

namespace detail {
struct State {
  std::mutex mu;
  std::map<uint32_t, std::array<uint32_t, kModels>> by_weapon;   // CWeapon -> its models, built from the recoloured read
  std::atomic<size_t> count{0};
};
inline State& S() {
  static State s;
  return s;
}
}  // namespace detail

inline void NoteRecolouredBuild(uint32_t weapon, const uint32_t (&models)[kModels]) {
  if (weapon < 0x10000) return;
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  if (s.by_weapon.size() >= kMaxWeapons && s.by_weapon.find(weapon) == s.by_weapon.end()) s.by_weapon.clear();
  auto& m = s.by_weapon[weapon];
  for (uint32_t k = 0; k < kModels; ++k) m[k] = models[k];
  s.count.store(s.by_weapon.size(), std::memory_order_release);
}

// The hook's entry: `recolour_obj` = r3 of sub_833A1758. read_word(addr, v) = the guest read (fault-checked). True when a CWeapon was read and noted.
template <class ReadWordFn>
inline bool NoteFromRecolourCall(ReadWordFn read_word, uint32_t recolour_obj) {
  uint32_t weapon = 0;
  if (recolour_obj < 0x10000 + kRecolourObjToWeapon || !read_word(recolour_obj - kRecolourObjToWeapon, weapon) || weapon < 0x10000) return false;
  uint32_t models[kModels] = {};
  for (uint32_t k = 0; k < kModels; ++k)
    if (!read_word(weapon + kWeaponModelsOff + 4 * k, models[k])) models[k] = 0;
  NoteRecolouredBuild(weapon, models);
  return true;
}

inline void Forget(uint32_t weapon) {
  auto& s = detail::S();
  if (s.count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> l(s.mu);
  s.by_weapon.erase(weapon);
  s.count.store(s.by_weapon.size(), std::memory_order_release);
}

// True when `model` is one of the models a recoloured task built (the usual case, none: one atomic load).
inline bool IsRecoloured(uint32_t model) {
  if (model == 0) return false;
  auto& s = detail::S();
  if (s.count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> l(s.mu);
  for (const auto& kv : s.by_weapon)
    for (uint32_t m : kv.second)
      if (m == model) return true;
  return false;
}

inline void Clear() {   // tests
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  s.by_weapon.clear();
  s.count.store(0, std::memory_order_release);
}

}  // namespace rdr_scene::live::prop_read_note
