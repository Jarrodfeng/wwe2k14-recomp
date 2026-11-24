// S105 PLATE: the championship belt's nameplate (the champion's name) as the game's own picture (docs\research\s105\PLATE.md, verdict 1-5 [C]). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: std + our pure headers only (no game or SDK includes); the unit tools\scene\belt_plate_unit.cpp runs THIS code.
//
// The plate is an ordinary registered texture wrapper (vtable 0x8206ADB8, kind 0, BC3 256 x 128, linear, endian 8-in-16) that MakeWeapon's sub_8279B7A8 makes and registers under the lowercased
// name `nameplate` BEFORE any model names it [C PLATE.md 3]. The model's key table is [model+256] = count, [model+260] = keys (8 bytes each, word 0 = the registry handle); the key whose wrapper is
// named `nameplate` (wrapper +16..+31, a zero-padded lowercase name [C PLATE.md 4]) is the plate. OnEventCall (the weapon rows' raw hook, game thread, both builds) reads its 32 KB of BC3 once and keeps it
// by model; the drawer (rf_props.cpp) binds it to the material whose diffuse sampler is named `nameplate` and puts its hash into the pool key (two holders never share a plate).
// Every guest read goes through the resolver that already reads the game's other textures (guest_texture.h ResolveHandle / ShapeOfFetch / ReadMip0); guest_tex_dirty watches a rewrite in place.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../assets/asset_model.h"
#include "../assets/jboy.h"   // assets::AlphaClearShare (the loader's own rule for Material::diffuse_alpha_clear)
#include "guest_tex_dirty.h"
#include "guest_texture.h"
#include "plate_ink.h"

namespace rdr_scene::live::beltplate {

// Test only (tools\scene\belt_plate_unit.cpp): 1 = the name test is dropped (the first key is taken); 2 = the wrong t (the key after the named one); 3 = a belt with no plate falls back to white without a
// counter; 4 = the plate hash is left out of the pool key (two holders share a pool); 5 = OnMake does not Drop (make_no_drop); 6 = the free / prune / scene-clear paths do not Drop (free_no_drop);
// 7 = the plate's alpha share is not set on the bound material (share_unset: the S110 BELTPLATE black bar).
inline int g_plate_plant = 0;

inline constexpr uint32_t kModelKeyCount = 256, kModelKeys = 260, kWrapperName = 16, kMaxKeys = 256, kMaxPlateBytes = 64 * 1024;
inline constexpr char kPlateName[] = "nameplate";

struct Plate {
  uint32_t handle = 0, header = 0, gen = 0, fetch1 = 0, t = 0, width = 0, height = 0;
  uint64_t hash = 0;
  std::shared_ptr<const std::vector<uint8_t>> blocks;   // BC3, linear, tightly packed, mip 0 only (assets::Texture::data)
};

struct Counters {
  uint64_t read = 0, reread = 0, no_plate = 0, bad_shape = 0, unreadable = 0, dropped = 0;
  uint64_t bound = 0, missing = 0;   // missing: a material named `nameplate` met no plate (it would draw white)
};

template <class RW>
struct RwMem {   // guest_texture.h's memory contract over a word reader (a refused word sets `fail`)
  RW& rw;
  mutable bool fail = false;
  uint32_t u32(uint32_t a) const {
    uint32_t v = 0;
    if (!rw(a, v)) fail = true;
    return v;
  }
};

inline uint64_t HashPlate(uint32_t w, uint32_t h, const std::vector<uint8_t>& b) {
  uint64_t x = 1469598103934665603ull ^ (uint64_t(w) << 32 | h);
  for (uint8_t c : b) x = (x ^ c) * 1099511628211ull;
  return x ? x : 1;
}

// Is the wrapper's name (16 bytes at +16) `nameplate`, case-insensitive (the game lowercases both sides)? Under plant 1 every name matches.
template <class RW>
inline bool NameIsPlate(RW&& rw, uint32_t wrapper) {
  if (g_plate_plant == 1) return true;
  char n[16];
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t v = 0;
    if (!rw(wrapper + kWrapperName + 4 * i, v)) return false;
    for (uint32_t b = 0; b < 4; ++b) n[4 * i + b] = char(v >> (24 - 8 * b));
  }
  const size_t L = sizeof(kPlateName) - 1;
  for (size_t i = 0; i < L; ++i) {
    char c = n[i];
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
    if (c != kPlateName[i]) return false;
  }
  for (size_t i = L; i < 16; ++i)
    if (n[i] != 0) return false;
  return true;
}

enum class Find { NoKeys, NotFound, Found };

// The key whose wrapper is named `nameplate` in the model's key table. `t` = its index in the model's texture-name table [C PLATE.md 4].
template <class RW>
inline Find FindPlate(RW&& rw, uint32_t model, guest_tex::Resolved& r, uint32_t& t) {
  uint32_t n = 0, keys = 0;
  if (!rw(model + kModelKeyCount, n) || !rw(model + kModelKeys, keys) || n == 0 || n > kMaxKeys || keys == 0) return Find::NoKeys;
  const RwMem<std::remove_reference_t<RW>> mem{rw};
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t handle = 0;
    if (!rw(keys + 8 * i, handle) || handle == 0) continue;
    guest_tex::Resolved cand;
    if (guest_tex::ResolveHandle(mem, handle, cand) != guest_tex::Why::Ok) continue;
    if (!NameIsPlate(rw, cand.wrapper)) continue;
    t = i;
    if (g_plate_plant == 2) {   // the planted wrong t: the key after the named one
      uint32_t h2 = 0;
      guest_tex::Resolved c2;
      if (i + 1 < n && rw(keys + 8 * (i + 1), h2) && guest_tex::ResolveHandle(mem, h2, c2) == guest_tex::Why::Ok) cand = c2, t = i + 1;
    }
    r = cand;
    return Find::Found;
  }
  return Find::NotFound;
}

class Store {
 public:
  // Game thread, once per event-prop model call. `rw(a, v)` = the guest word reader, `phys(addr, n, dst)` = the physical pixel reader. Cheap after the first read: a lookup, and one generation compare for a found plate.
  template <class RW, class PH>
  void OnEventCall(uint32_t model, RW&& rw, PH&& phys) {
    if (model == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    State& s = map_[model];
    if (s.searched && !s.found) return;   // not a belt (a mic / cap / stage prop): decided once per model address (a freed address is Dropped)
    if (s.found && guest_tex_dirty::Gen(s.plate.header) == s.plate.gen) return;
    guest_tex::Resolved r;
    uint32_t t = 0;
    if (!s.found) {
      const Find f = FindPlate(rw, model, r, t);
      if (f == Find::NoKeys) return last_ = "keys not built yet", void();   // the keys are not built yet: ask again at the next call
      s.searched = true;
      if (f == Find::NotFound) return last_ = "no nameplate key in the model", ++c_.no_plate, void();
    } else {
      const RwMem<std::remove_reference_t<RW>> mem{rw};
      if (guest_tex::ResolveHandle(mem, s.plate.handle, r) != guest_tex::Why::Ok) return Retry(s, "plate handle no longer resolves"), void();   // (the old picture stands; counted once per state)
      t = s.plate.t;
    }
    guest_tex::Shape sh;
    guest_tex_dirty::Watch(r.header);   // (the order of guest_model_tex.h: watch, take the generation BEFORE the read, store it with the result)
    const uint32_t gen = guest_tex_dirty::Gen(r.header);
    s.plate.header = r.header;   // (held from here: Drop / Clear Unwatch it, whatever the read answers)
    if (guest_tex::ShapeOfFetch(r.fetch, sh) != guest_tex::Why::Ok || sh.game_format != 20) return s.found = false, s.retry_counted = false, last_ = "not a BC3 2D picture", ++c_.bad_shape, void();
    std::vector<uint8_t> blocks;
    const guest_tex::Why rd_why = guest_tex::ReadMip0(sh, phys, blocks, kMaxPlateBytes);
    if (rd_why == guest_tex::Why::Memory) return s.found = false, s.searched = false, Retry(s, "plate pixels unreadable"), void();   // retried at the next call (counted once)
    if (rd_why != guest_tex::Why::Ok) return s.found = false, s.retry_counted = false, last_ = "plate size / format refused", ++c_.bad_shape, void();   // decided: Size / Format never change by asking again
    s.retry_counted = false;
    s.plate.gen = gen;
    s.plate.handle = r.handle, s.plate.header = r.header, s.plate.fetch1 = r.fetch[1], s.plate.t = t, s.plate.width = sh.width, s.plate.height = sh.height;
    s.plate.hash = HashPlate(sh.width, sh.height, blocks);
    s.plate.blocks = std::make_shared<const std::vector<uint8_t>>(std::move(blocks));
    s.pub = std::make_shared<const Plate>(s.plate);   // review nit 7: built once, Get hands it out
    s.found = true;
    s.found_once ? ++c_.reread : ++c_.read;
    s.found_once = true;
  }
  // Render thread: the plate of a model (null = none).
  std::shared_ptr<const Plate> Get(uint32_t model) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(model);
    return it != map_.end() && it->second.found ? it->second.pub : nullptr;
  }
  void Drop(uint32_t model) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(model);
    if (it == map_.end()) return;
    const uint32_t hd = it->second.plate.header;
    map_.erase(it), ++c_.dropped;
    if (hd) {   // review 4: free the dirty-table slot unless another entry still reads this header (guest_model_tex.h Trim)
      bool held = false;
      for (const auto& kv : map_) held = held || kv.second.plate.header == hd;
      if (!held) guest_tex_dirty::Unwatch(hd);
    }
  }
  void Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    c_.dropped += map_.size();
    for (const auto& kv : map_)
      if (kv.second.plate.header) guest_tex_dirty::Unwatch(kv.second.plate.header);
    map_.clear();
  }
  std::string LastRefusal() const {   // the store's last refusal (the "NO plate" log line names it)
    std::lock_guard<std::mutex> lk(mu_);
    return last_;
  }
  void CountBound(uint32_t n, uint32_t named_without_plate) {
    std::lock_guard<std::mutex> lk(mu_);
    c_.bound += n;
    if (g_plate_plant != 3) c_.missing += named_without_plate;   // plant 3: a belt that would draw white says nothing
  }
  Counters Counts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return c_;
  }

 private:
  struct State {
    bool searched = false, found = false, found_once = false, retry_counted = false;
    Plate plate;
    std::shared_ptr<const Plate> pub;
  };
  void Retry(State& s, const char* why) {   // a retrying state counts once, not every frame
    last_ = why;
    if (!s.retry_counted) s.retry_counted = true, ++c_.unreadable;
  }
  std::string last_;
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, State> map_;
  Counters c_;
};

inline Store& TheStore() {
  static Store s;
  return s;
}

// The pool key suffix of a model's plate: "" with none (or under plant 4), else "#p<hash>": two holders' belts never share a pool (and so never a bound copy).
inline std::string PoolSuffix(const std::shared_ptr<const Plate>& p) {
  if (!p || g_plate_plant == 4) return std::string();
  char b[24];
  std::snprintf(b, sizeof b, "#p%016llx", static_cast<unsigned long long>(p->hash));
  return b;
}

// The sampler's name against `nameplate`, case-insensitive (the game lowercases both sides; review nit 9).
inline bool NameIs(const std::string& n) {
  const size_t L = sizeof(kPlateName) - 1;
  if (n.size() != L) return false;
  for (size_t i = 0; i < L; ++i) {
    char c = n[i];
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
    if (c != kPlateName[i]) return false;
  }
  return true;
}

// The call sites of weapon_rows_core.h (planted twins 5 / 6 skip them; test only).
inline void DropOnMake(uint32_t model) { if (g_plate_plant != 5) TheStore().Drop(model); }
inline void DropOnFree(uint32_t model) { if (g_plate_plant != 6) TheStore().Drop(model); }
inline void ClearOnScene() { if (g_plate_plant != 6) TheStore().Clear(); }

// Bind the plate to every material whose diffuse sampler is named `nameplate` and has no texture (the model file's name table does not hold it: the plate is the game's, made at run time). Returns the
// number bound; a named material with no plate is counted `missing` (it would draw white).
// S110 BELTPLATE: the loader computes Material::diffuse_alpha_clear only for a texture the file holds; the plate is bound later, so the same fact (assets::AlphaClearShare) is set here, once per plate texture;
// `share_out` (optional) receives it.
inline uint32_t BindPlate(assets::Model& bound, const std::shared_ptr<const Plate>& p, Store& st = TheStore(), float* share_out = nullptr, Ink* ink_out = nullptr) {
  uint32_t named = 0, n = 0;
  int tex = -1;
  float share = 0.f;
  Ink ink;
  for (assets::Material& m : bound.materials) {
    if (m.diffuse >= 0 || !NameIs(m.diffuse_name)) continue;
    ++named;
    if (!p || !p->blocks) continue;
    if (tex < 0) {
      assets::Texture t;
      t.name = kPlateName, t.format = assets::TexFormat::kBC3, t.width = p->width, t.height = p->height, t.mips = 1, t.data = *p->blocks;
      tex = int(bound.textures.size());
      share = assets::AlphaClearShare(t);
      ink = PlateInk(t);
      bound.textures.push_back(std::move(t));
    }
    m.diffuse = tex, ++n;
    if (g_plate_plant != 7) m.diffuse_alpha_clear = share;
  }
  if (share_out) *share_out = share;
  if (ink_out) *ink_out = ink;
  st.CountBound(n, (p && p->blocks) ? 0u : named);
  return n;
}

}  // namespace rdr_scene::live::beltplate
