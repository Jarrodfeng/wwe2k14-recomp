// S105 PLATE -> S111 CACMATCHTEX: an event prop's RUN-TIME textures, read from the live model's texture KEY table by index (docs\research\s111\CACMATCHTEX.md; PLATE.md 4). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: std + our pure headers only (no game or SDK includes); the unit tools\scene\belt_plate_unit.cpp runs THIS code.
//
// The game's model keeps one 8-byte key per entry of its texture-name table: [model+256] = count, [model+260] = keys, key t = the texture of name-table entry t (word 0 = the registry handle). The model's
// vslot +60 (sub_82BD4F50) stores key t [C]. Two writers put pictures there that no file holds: MakeWeapon's nameplate (sub_8279B7A8: a texture registered as `nameplate` before the model names it, PLATE.md 3)
// and the Create a Championship belt, whose file holds no textures at all: its CPU-composited DXT5 plate T0 and normal map T1 go to keys 3, 4, 0, 2, 5 = belt, belt_n, plate, plate_n, nameplate of BOBJ
// 2926-2930 entry 0 (T0, T1, T0, T1, T0: the match sub_833250A8, the editor sub_83335270 -> sub_83343FD0 on the preview actor's model). So ONE rule serves both: a diffuse / normal / specular sampler the
// file cannot fill (assets::Material slot -1) is bound to the live key at its name-table index (Material::key_diffuse / key_normal / key_specular). No name test: the nameplate is key t of its own name.
// The drawer (rf_props.cpp, render thread) asks for the indices its bound model needs (Want); the weapon rows' raw hook (game thread, both builds) reads them at the model's next call (OnEventCall); the
// drawer binds what was read (BindKeys). Every guest read goes through the resolver that reads the game's other textures (guest_texture.h ResolveHandle / ShapeOfFetch / ReadMip0); guest_tex_dirty watches a
// rewrite in place (the CPU writers sub_83324AE8 / sub_8332DCE8 end in the flush sub_82CA6BE0).
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
#include "guest_texture_mips.h"   // S111 CAATABLE review: ReadChain (a `chain` model's keys: every level the header names)
#include "plate_ink.h"
#include "desk_keys.h"   // S111 CAATABLE: KeysOverFile (the announce table's weapon ids)

namespace rdr_scene::live::beltplate {

// Test only (tools\scene\belt_plate_unit.cpp): 1 = the key index is ignored (the first key that resolves is read for every wanted index); 2 = the wrong t (key t + 1); 3 = a diffuse slot with no texture
// is not counted (silent white); 4 = the set's hash is left out of the pool key (two holders share a pool); 5 = OnMake does not Drop (make_no_drop); 6 = the free / prune / scene-clear paths do not Drop
// (free_no_drop); 7 = the diffuse alpha share is not set on the bound material (share_unset: the S110 BELTPLATE black bar); 8 = only diffuse slots are asked and bound (diffuse_only: the normal / specular
// keys stay unread); 9 = no pending answer (no_pending: the drawer binds before the wanted keys were read, a white first frame). S111 CACMATCHTEX review: 10 = an untried normal / specular key holds the
// prop too (pend_any: the old rule, a prop hidden for its first frames for a spec map); 11 = PoolTrack never retires a model's previous pool (no_retire: one pool per recomposite); 12 = a key the game's draw
// binds nothing at is counted `missing` (none_is_miss: the census calls the game's own empty slot a miss). S111 CAATABLE (desk_keys.h): 13 = BindKeys ignores `over_file` (over_file_ignored: the file's
// stock announce table stands over the live, repainted key); 14 = the raw hook reads the keys of event props only (desk_not_read: weaponrows::ReadsKeys' old gate). S111 CAATABLE review: 15 = a `chain`
// model's key is read mip 0 only (chain_ignored: the desk draws without its mips); 16 = a key that holds the file's own picture still replaces the file's texture (equal_replaces: the file's mips are lost).
// S111 DIRTYOWN review: 17 = the key's stored generation is kept when its header changes (gen_kept: a rebind whose first read fails compares the new header with the old header's generation).
// S111 KEYFRESH: 18 = the cached return is keyed on the handle alone (header_unseen: a new header behind the same handle / wrapper is not seen until the old header flushes; the old rule).
inline int g_plate_plant = 0;

inline constexpr uint32_t kModelKeyCount = 256, kModelKeys = 260, kMaxKeys = 256, kMaxWantT = 64, kMaxKeyTexBytes = guest_tex::kMaxRead, kMaxPendingAsks = 8;

struct Plate {   // the texture of ONE key of the model (key t)
  uint32_t handle = 0, header = 0, gen = 0, fetch1 = 0, t = 0, width = 0, height = 0;
  uint32_t mips = 1;   // S111 CAATABLE review: the levels in `blocks` (1 unless the model asked for its chains: Store::Want `chain`)
  assets::TexFormat format = assets::TexFormat::kUnknown;
  uint64_t hash = 0;
  std::shared_ptr<const std::vector<uint8_t>> blocks;   // linear, tightly packed, mip 0 first, then `mips` - 1 more levels (assets::Texture::data)
};

struct KeySet {   // every key of one model read so far (sorted by t); what the drawer binds
  std::vector<Plate> keys;
  uint64_t hash = 0;   // of every key's (t, hash): the pool key
  uint64_t tried = 0, none = 0;   // S111 CACMATCHTEX review: bit t = key t was tried; = key t is one the game's draw binds nothing at (index past [model+256], or a handle sub_82294F20 resolves to 0)
  const Plate* Find(uint32_t t) const {
    for (const Plate& p : keys)
      if (p.t == t) return &p;
    return nullptr;
  }
};

struct Ask {   // the drawer's answer: the set read so far; pending = a wanted key was never tried yet (the drawer waits a frame, as for a model not resident)
  std::shared_ptr<const KeySet> set;
  bool pending = false;
};

struct Counters {
  uint64_t read = 0, reread = 0, no_plate = 0, bad_shape = 0, unreadable = 0, dropped = 0;   // no_plate: a wanted index with no key (beyond the table, handle 0, or a handle that does not resolve)
  uint64_t bound = 0, missing = 0;   // missing: a diffuse slot whose key the game's draw binds (or that was never read) and ours did not: it draws white where the game draws a picture
  uint64_t pending = 0, other_unbound = 0;   // pending: drawer asks answered "not tried yet"; other_unbound: a normal / specular slot whose key the game binds, tried and refused by ours
  uint64_t game_none = 0;   // S111 CACMATCHTEX review: a slot whose key the game's draw binds nothing at (sub_82C04DF0 0x82C056E4 / 0x82C0570C skip it): the game's own empty slot, not a miss
  uint64_t unwatched = 0;   // S111 DIRTYOWN: key reads the dirty table refused to watch (guest_tex_dirty::Watch false): such a key is read again at every call, never left stale
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

inline assets::TexFormat FormatOf(uint32_t game_format) {
  return game_format == 18 ? assets::TexFormat::kBC1 : game_format == 19 ? assets::TexFormat::kBC2 : game_format == 20 ? assets::TexFormat::kBC3 : assets::TexFormat::kUnknown;
}

// The key indices a model needs from the live model: every diffuse / normal / specular slot the file did not fill (slot -1) whose sampler names a table index; with `over_file` (S111 CAATABLE,
// desk_keys.h: the announce table) every such slot, filled or not. 0 = none (the model is the file's alone).
inline uint64_t WantMask(const assets::Model& m, bool over_file = false) {
  uint64_t mask = 0;
  for (const assets::Material& c : m.materials) {
    const int slot[3] = {c.diffuse, c.normal, c.specular}, t[3] = {c.key_diffuse, c.key_normal, c.key_specular};
    for (int i = 0; i < 3; ++i) {
      if (i > 0 && g_plate_plant == 8) continue;
      if ((slot[i] < 0 || over_file) && t[i] >= 0 && t[i] < int(kMaxWantT)) mask |= uint64_t(1) << uint32_t(t[i]);
    }
  }
  return mask;
}
// S111 CACMATCHTEX review: the indices of WantMask that fill a DIFFUSE slot: only these hold the prop back a frame (Want's `hold`); a missing normal / specular is drawn without for that frame and bound once read.
inline uint64_t HoldMask(const assets::Model& m) {
  uint64_t mask = 0;
  for (const assets::Material& c : m.materials)
    if (c.diffuse < 0 && c.key_diffuse >= 0 && c.key_diffuse < int(kMaxWantT)) mask |= uint64_t(1) << uint32_t(c.key_diffuse);
  return mask;
}

class Store {
 public:
  // Render thread, once per event-prop draw: ask for the key indices `mask` of the live model `model`; answers the set read so far and whether a wanted key of `hold` (HoldMask: the diffuse ones) was never
  // tried (the drawer then skips this frame, as it does for a model not yet resident; bounded by kMaxPendingAsks so a model the hook never sees again is still drawn). An untried key outside `hold` never
  // holds the prop (S111 CACMATCHTEX review). The default `hold` = every wanted key. S111 CAATABLE review: `chain` = read each key with its whole mip chain (the announce table, drawn over its file's
  // textures at a distance), not mip 0 alone.
  Ask Want(uint32_t model, uint64_t mask, uint64_t hold = ~uint64_t(0), bool chain = false) {
    Ask a;
    if (model == 0 || mask == 0) return a;
    std::lock_guard<std::mutex> lk(mu_);
    State& s = map_[model];
    s.want |= mask;
    s.chain = s.chain || chain;
    a.set = s.pub;
    bool untried = false;
    const uint64_t held = mask & (g_plate_plant == 10 ? mask : hold);   // (plant 10: every wanted key holds the prop, the old rule)
    for (uint32_t t = 0; t < kMaxWantT; ++t)
      if (((held >> t) & 1) && !s.k[t].tried) untried = true;
    if (untried && g_plate_plant != 9 && s.pending_asks < kMaxPendingAsks) a.pending = true, ++s.pending_asks, ++c_.pending;
    return a;
  }
  // Game thread, once per event-prop model call. `rw(a, v)` = the guest word reader, `phys(addr, n, dst)` = the physical pixel reader. A model nobody asked for costs one lookup; an asked one costs, per
  // wanted index after its first read, the key word, the walk handle -> registry entry -> wrapper -> header (about a dozen words: S111 KEYFRESH, the game's draw resolves the key at every draw) and one
  // generation compare.
  template <class RW, class PH>
  void OnEventCall(uint32_t model, RW&& rw, PH&& phys) {
    if (model == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(model);
    if (it == map_.end() || it->second.want == 0) return;
    State& s = it->second;
    uint32_t n = 0, keys = 0;
    if (!rw(model + kModelKeyCount, n) || !rw(model + kModelKeys, keys) || n == 0 || n > kMaxKeys || keys == 0) return last_ = "keys not built yet", void();   // asked again at the next call (nothing tried)
    bool changed = false;
    for (uint32_t t = 0; t < kMaxWantT; ++t)
      if ((s.want >> t) & 1) changed = ReadKey(s, t, n, keys, rw, phys) || changed;
    if (changed) Publish(s);
  }
  // The set of a model read so far (null = none).
  std::shared_ptr<const KeySet> Get(uint32_t model) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(model);
    return it != map_.end() ? it->second.pub : nullptr;
  }
  void Drop(uint32_t model) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = map_.find(model);
    if (it == map_.end()) return;
    std::vector<uint32_t> hds;
    for (const auto& kv : it->second.k)
      if (kv.second.p.header) hds.push_back(kv.second.p.header);
    map_.erase(it), ++c_.dropped;
    for (uint32_t hd : hds) UnwatchIfFree(hd);   // review 4 (S105): free the dirty-table slot unless another entry still reads this header
  }
  void Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    c_.dropped += map_.size();
    for (const auto& kv : map_)
      for (const auto& k : kv.second.k)
          if (k.second.p.header) guest_tex_dirty::Unwatch(k.second.p.header, guest_tex_dirty::kOwnerBelt);
    map_.clear();
  }
  std::string LastRefusal() const {   // the store's last refusal (the "NO ... read" log line names it)
    std::lock_guard<std::mutex> lk(mu_);
    return last_;
  }
  void CountBound(uint32_t n, uint32_t diffuse_without, uint32_t other_without, uint32_t game_none = 0) {
    std::lock_guard<std::mutex> lk(mu_);
    c_.bound += n;
    if (g_plate_plant != 3) c_.missing += diffuse_without;   // plant 3: a diffuse that would draw white says nothing
    c_.other_unbound += other_without;
    c_.game_none += game_none;
  }
  Counters Counts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return c_;
  }

 private:
  struct KeyState {
    bool tried = false, ok = false, decided = false, retry_counted = false;
    bool none = false;   // S111 CACMATCHTEX review: the game's draw binds nothing at this key (KeySet::none)
    bool watched = false;   // S111 DIRTYOWN: the dirty table took this key's header (guest_tex_dirty::Watch true); false = the key is read again at every call
    uint32_t seen = 0;      // S111 KEYFRESH: the header the handle resolved to at the last try (0 = it did not resolve): with the handle, the cached return's key
    Plate p;   // p.handle = the handle last tried (a different handle in the key = the game rebound it: read again)
  };
  struct State {
    uint64_t want = 0;
    bool chain = false;   // S111 CAATABLE review: Want `chain` (ReadKey takes every level, guest_texture_mips.h ReadChain)
    uint32_t pending_asks = 0;
    std::unordered_map<uint32_t, KeyState> k;
    std::shared_ptr<const KeySet> pub;
  };
  void UnwatchIfFree(uint32_t hd) {
    bool held = false;
    for (const auto& kv : map_)
      for (const auto& k : kv.second.k) held = held || k.second.p.header == hd;
    if (!held) guest_tex_dirty::Unwatch(hd, guest_tex_dirty::kOwnerBelt);   // S111 DIRTYOWN: the belt store's interest only (another reader's watch of the header stays)
  }
  // Key t of the model: true when what the set publishes for t changed.
  template <class RW, class PH>
  bool ReadKey(State& s, uint32_t t, uint32_t n, uint32_t keys, RW& rw, PH& phys) {
    KeyState& k = s.k[t];
    uint32_t kt = g_plate_plant == 2 ? t + 1 : t, handle = 0;
    const RwMem<std::remove_reference_t<RW>> mem{rw};
    if (g_plate_plant == 1) {   // the planted fault: the first key that resolves, whatever t
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t h = 0;
        guest_tex::Resolved c;
        if (rw(keys + 8 * i, h) && h && guest_tex::ResolveHandle(mem, h, c) == guest_tex::Why::Ok) {
          kt = i;
          break;
        }
      }
    }
    const bool past_table = kt >= n;   // the game's draw skips an index >= [model+256] (sub_82C04DF0 0x82C056E4)
    const bool word_ok = !past_table && rw(keys + 8 * kt, handle);
    if (!word_ok) handle = 0;
    // S111 KEYFRESH: the game's draw resolves the key at every draw (sub_82C04DF0 0x82C05700: sub_82294F20, handle -> the registry entry's wrapper, bound at 0x82C05740), so the walk handle -> wrapper ->
    // header runs at every call, BEFORE the cached test, and the cache is keyed on the header resolved NOW: a new header behind the same handle / wrapper is read at this call, not when the old header
    // flushes; a handle refused as unregistered is tried again once it resolves. (g_plate_plant 18 = the old key, the handle alone.)
    guest_tex::Resolved r;
    const guest_tex::Why rv = word_ok ? guest_tex::ResolveHandle(mem, handle, r) : guest_tex::Why::NoHandle;
    const uint32_t now_header = g_plate_plant == 18 ? k.seen : rv == guest_tex::Why::Ok ? r.header : 0;
    if (k.tried && handle == k.p.handle && now_header == k.seen) {
      if (k.decided) return false;   // this handle and header were refused for good (format, size, no key)
      if (k.ok && k.watched && guest_tex_dirty::Gen(k.p.header) == k.p.gen) return false;   // read, watched, and no flush since (S111 DIRTYOWN: an unwatched key is read again)
    }
    k.seen = now_header;
    const bool was_ok = k.ok, was_tried = k.tried, was_none = k.none;
    const uint32_t old_header = k.p.header;
    k.tried = true, k.decided = false;
    if (k.p.handle != handle) k.retry_counted = false;
    k.p.handle = handle, k.p.t = t;
    const auto refuse = [&](const char* why, uint64_t& counter, bool none) {   // true when the published set changes (a picture gone, the first try, the game-binds-nothing fact moved)
      const uint32_t held = k.p.header;
      k.ok = false, k.decided = true, k.none = none, k.watched = false, k.p.header = 0, k.p.blocks.reset();
      last_ = why, ++counter;
      if (held) UnwatchIfFree(held);
      if (old_header && old_header != held) UnwatchIfFree(old_header);
      return was_ok || !was_tried || was_none != none;
    };
    if (past_table) return refuse("the sampler's index is past the model's key table (the game binds nothing)", c_.no_plate, true);
    if (!word_ok) return refuse("the key word is unreadable", c_.no_plate, false);
    // S111 CACMATCHTEX review: the game's draw resolves the key with sub_82294F20 (index = handle & 0xFFFF inside the registry, stored id == handle, else 0) and binds nothing when that is 0
    // (0x82C0570C): exactly our walk's NoHandle (texture_registry.h WalkTextureHandle). Any other refusal is a picture the game binds and ours cannot read: a miss. (`rv` / `r`: the walk above, S111 KEYFRESH.)
    if (rv == guest_tex::Why::NoHandle || rv == guest_tex::Why::NoRegistry) return refuse("no key at the sampler's index (the game binds nothing)", c_.no_plate, true);
    if (rv != guest_tex::Why::Ok) return refuse("the key is not a plain 2D texture wrapper", c_.bad_shape, false);
    k.none = false;
    guest_tex::Shape sh;
    k.watched = guest_tex_dirty::Watch(r.header, guest_tex_dirty::kOwnerBelt);   // (the order of guest_model_tex.h: watch, take the generation BEFORE the read, store it with the result)
    if (!k.watched) ++c_.unwatched;   // S111 DIRTYOWN: a refused watch is counted, and the key is read again at the next call (the cached return above needs k.watched)
    const uint32_t gen = guest_tex_dirty::Gen(r.header);
    // S111 DIRTYOWN review: the stored generation belongs to the header it was taken from. A new header starts at 0 (a watched header's generation is never 0), so a read of it that fails (Memory,
    // below) is retried at the next call instead of matching the old header's generation by chance and being cached for good.
    if (k.p.header != r.header && g_plate_plant != 17) k.p.gen = 0;
    k.p.header = r.header;   // (held from here: Drop / Clear Unwatch it, whatever the read answers)
    if (old_header && old_header != r.header) UnwatchIfFree(old_header);
    if (guest_tex::ShapeOfFetch(r.fetch, sh) != guest_tex::Why::Ok) return refuse("not a BC1 / BC2 / BC3 2D picture", c_.bad_shape, false);
    std::vector<uint8_t> blocks;
    uint32_t mips = 1;
    guest_tex::Why rd;
    if (s.chain && g_plate_plant != 15) {   // S111 CAATABLE review: every level the game's header names (mip 0 = ReadMip0's bytes; the levels from the mip page, car_paint.h's reader); a level that cannot be read is the key's refusal / retry
      guest_tex::Chain c;
      rd = guest_tex::ReadChain(r.fetch, phys, c, kMaxKeyTexBytes);
      if (rd == guest_tex::Why::Ok) blocks = std::move(c.data), mips = c.mips;
    } else {
      rd = guest_tex::ReadMip0(sh, phys, blocks, kMaxKeyTexBytes);
    }
    if (rd == guest_tex::Why::Memory) {   // retried at the next call (counted once per handle); the old picture, if any, stands
      last_ = "key pixels unreadable";
      if (!k.retry_counted) k.retry_counted = true, ++c_.unreadable;
      return !was_tried || was_none;   // (the first try publishes the tried bit)
    }
    if (rd != guest_tex::Why::Ok) return refuse("key texture size / format refused", c_.bad_shape, false);   // decided: Size / Format never change by asking again
    k.retry_counted = false;
    k.p.gen = gen, k.p.fetch1 = r.fetch[1], k.p.width = sh.width, k.p.height = sh.height, k.p.format = FormatOf(sh.game_format), k.p.mips = mips;
    k.p.hash = HashPlate(sh.width, sh.height, blocks) ^ (uint64_t(sh.game_format) << 56);
    k.p.blocks = std::make_shared<const std::vector<uint8_t>>(std::move(blocks));
    k.ok = true;
    was_ok ? ++c_.reread : ++c_.read;
    return true;
  }
  void Publish(State& s) {
    auto set = std::make_shared<KeySet>();
    uint64_t h = 1469598103934665603ull;
    for (uint32_t t = 0; t < kMaxWantT; ++t) {
      const auto it = s.k.find(t);
      if (it == s.k.end()) continue;
      if (it->second.tried) set->tried |= uint64_t(1) << t;
      if (it->second.tried && it->second.none) set->none |= uint64_t(1) << t;
      if (!it->second.ok) continue;
      set->keys.push_back(it->second.p);
      h = (h ^ (uint64_t(t) + 1)) * 1099511628211ull, h = (h ^ it->second.p.hash) * 1099511628211ull;
    }
    set->hash = h ? h : 1;
    if (set->keys.empty() && set->tried == 0) s.pub = nullptr;   // (a set with no picture but tried keys is published: BindKeys tells the game's empty slot from an unread one)
    else s.pub = std::move(set);
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

// The pool key suffix of a model's set: "" for a model that wants no key (or under plant 4), else "#p<16 hex>": the set's hash when it holds a picture, 0 when none is read yet (S111 CACMATCHTEX review: a
// fixed width, so no live pool's name is a prefix of another set's pool and PoolTrack's RetireOther(old pool) takes that pool alone): two holders' belts never share a pool (and so never a bound copy).
inline std::string PoolSuffix(const std::shared_ptr<const KeySet>& p, bool wanted = false) {
  const bool pics = p && !p->keys.empty();
  if ((!pics && !wanted) || g_plate_plant == 4) return std::string();
  char b[24];
  std::snprintf(b, sizeof b, "#p%016llx", static_cast<unsigned long long>(pics ? p->hash : 0ull));
  return b;
}

// S111 CACMATCHTEX review: one live pool per event model. The drawer notes each model's pool every frame (Note); a model whose pool changed (a new set hash: a reread plate, a key read after the first
// frame, an editor recomposite) leaves its old pool to TakeRetire, which hands it back for ModelCache::RetireOther (pool, posed copies, memo; the bound copy by its "weapon:bound:" name) at the start of
// the next frame, unless another model's live pool still starts with it (two holders of one set share a pool). Render thread only (the drawer's per-cache WeaponState holds it).
class PoolTrack {
 public:
  void Note(uint32_t model, const std::string& pool) {
    std::string& cur = of_[model];
    if (!cur.empty() && cur != pool && g_plate_plant != 11) due_.push_back(cur);
    cur = pool;
  }
  std::vector<std::string> TakeRetire() {
    std::vector<std::string> out;
    for (const std::string& d : due_) {
      bool live = false;
      for (const auto& kv : of_) live = live || kv.second.rfind(d, 0) == 0;
      bool dup = false;
      for (const std::string& o : out) dup = dup || o == d;
      if (!live && !dup) out.push_back(d);
    }
    due_.clear();
    return out;
  }
  void Clear() { of_.clear(), due_.clear(); }   // the event props retired whole (the entrance is over)
  size_t Models() const { return of_.size(); }

 private:
  std::unordered_map<uint32_t, std::string> of_;
  std::vector<std::string> due_;
};

// The call sites of weapon_rows_core.h (planted twins 5 / 6 skip them; test only).
inline void DropOnMake(uint32_t model) { if (g_plate_plant != 5) TheStore().Drop(model); }
inline void DropOnFree(uint32_t model) { if (g_plate_plant != 6) TheStore().Drop(model); }
inline void ClearOnScene() { if (g_plate_plant != 6) TheStore().Clear(); }

// S111 CAATABLE review: the key's picture is the file texture `tex` of `m` itself (same format and size, mip 0 byte-equal): a key nothing rewrote. BindKeys `over_file` then keeps the file's texture.
inline bool KeyIsFile(const assets::Model& m, int tex, const Plate& p) {
  if (tex < 0 || size_t(tex) >= m.textures.size() || !p.blocks) return false;
  const assets::Texture& f = m.textures[size_t(tex)];
  if (f.format != p.format || f.width != p.width || f.height != p.height || f.faces != 1) return false;
  const size_t n = size_t((p.width + 3) / 4) * ((p.height + 3) / 4) * (p.format == assets::TexFormat::kBC1 ? 8u : 16u);   // (a Plate is BC1 / BC2 / BC3 only: FormatOf)
  return n && f.data.size() >= n && p.blocks->size() >= n && std::memcmp(f.data.data(), p.blocks->data(), n) == 0;
}

// Bind the set to the bound copy: every diffuse / normal / specular slot the file left empty (-1) whose name-table index the set holds gets that key's texture (one texture per key handle: the CAB's
// T0 sits in keys 0, 3 and 5). Returns the number of slots bound; a diffuse with nothing is counted `missing` (it draws white), a normal / specular with nothing `other_unbound`.
// S110 BELTPLATE: the loader computes Material::diffuse_alpha_clear only for a texture the file holds; a key texture is bound later, so the same fact (assets::AlphaClearShare) is set here for every diffuse
// it fills. `share_out` / `ink_out` / `shown` (optional) receive the first diffuse's share, ink and key.
// S111 CAATABLE (desk_keys.h): `over_file` = a slot the file DID fill is bound to its key too (the announce table's repainted / rebound keys); such a slot whose key is not read keeps the file's texture
// and is counted nowhere (it is neither white nor the game's empty slot). S111 CAATABLE review: so does one whose key still holds the file's own picture (KeyIsFile): the live key wins only where it differs.
// A key texture carries the levels the store read (Plate::mips).
inline uint32_t BindKeys(assets::Model& bound, const std::shared_ptr<const KeySet>& set, Store& st = TheStore(), float* share_out = nullptr, Ink* ink_out = nullptr, const Plate** shown = nullptr, bool over_file = false) {
  struct Made {
    uint32_t handle;
    int tex;
    float share;
  };
  std::vector<Made> made;
  uint32_t n = 0, white = 0, other = 0, game_none = 0;
  float share = 0.f;
  Ink ink;
  const Plate* first = nullptr;
  const bool over = over_file && g_plate_plant != 13;   // (plant 13: the file's texture stands over the key)
  for (assets::Material& m : bound.materials) {
    int* const slot[3] = {&m.diffuse, &m.normal, &m.specular};
    const int t[3] = {m.key_diffuse, m.key_normal, m.key_specular};
    for (int i = 0; i < 3; ++i) {
      const bool filled = *slot[i] >= 0;
      if ((filled && !over) || t[i] < 0 || t[i] >= int(kMaxWantT)) continue;
      if (i > 0 && g_plate_plant == 8) continue;
      const Plate* p = set ? set->Find(uint32_t(t[i])) : nullptr;
      if (!p || !p->blocks) {   // S111 CACMATCHTEX review: the game's empty slot is not a miss; an untried normal / specular is bound once read (a new set hash), not counted now
        if (filled) continue;   // S111 CAATABLE: a file-filled slot (over_file) whose key is not read keeps the file's texture: not white, not counted
        const uint64_t bit = uint64_t(1) << uint32_t(t[i]);
        if (set && (set->none & bit) && g_plate_plant != 12) ++game_none;
        else if (i == 0) ++white;
        else if (set && (set->tried & bit)) ++other;
        continue;
      }
      if (filled && g_plate_plant != 16 && KeyIsFile(bound, *slot[i], *p)) continue;   // S111 CAATABLE review: nothing rewrote this key: the file's texture stands, its mip chain with it
      size_t mi = made.size();
      for (size_t e = 0; e < made.size(); ++e)
        if (made[e].handle == p->handle) mi = e;
      if (mi == made.size()) {
        assets::Texture tx;
        tx.name = i == 0 && !m.diffuse_name.empty() ? m.diffuse_name : "key" + std::to_string(p->t);
        tx.format = p->format, tx.width = p->width, tx.height = p->height, tx.mips = p->mips, tx.data = *p->blocks;
        const float sh = assets::AlphaClearShare(tx);
        made.push_back({p->handle, int(bound.textures.size()), sh});
        bound.textures.push_back(std::move(tx));
      }
      *slot[i] = made[mi].tex, ++n;
      if (i != 0) continue;
      if (g_plate_plant != 7) m.diffuse_alpha_clear = made[mi].share;
      if (!first) first = p, share = made[mi].share, ink = PlateInk(bound.textures[size_t(made[mi].tex)]);
    }
  }
  if (share_out) *share_out = share;
  if (ink_out) *ink_out = ink;
  if (shown) *shown = first;
  st.CountBound(n, white, other, game_none);
  return n;
}

}  // namespace rdr_scene::live::beltplate
