// Class C4, the wrestlers (Session 101, lane 29). See wrestler_contrib.h.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "wrestler_contrib.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <set>

namespace rdr_scene::live {

Mat4 SkinFromWorld(const Mat4& world, const Mat4& inv_bind) {
  // out row i = sum_k inv_bind[i][k] * world.row(k): sub_82296630 called as (out = skeleton+200, a = world, b = inv bind).
  Mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += inv_bind.m[i * 4 + k] * world.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}

static std::atomic<bool> g_registered{false};

WrestlerContributor& Wrestlers() {
  static WrestlerContributor w;
  return w;
}

void RegisterWrestlers() {
  if (!g_registered.exchange(true)) Register(&Wrestlers());
}

bool WrestlersRegistered() { return g_registered.load(std::memory_order_relaxed); }

WrestlerContributor::Known* WrestlerContributor::KnownFor(uint32_t skel) {
  auto it = known_.find(skel);
  if (it != known_.end()) { it->second.touch = ++touch_seq_; return &it->second; }
  if (known_.size() >= 2 * kMaxSkeletons) {
    // Full (Emit may not be running to age entries out: models are built at match load, before any sink attaches): evict the least recently used
    // entry that is not in the record. None to evict = refuse (bounded memory).
    auto victim = known_.end();
    for (auto k = known_.begin(); k != known_.end(); ++k)
      if (!cur_.count(k->first) && !last_.count(k->first) && (victim == known_.end() || k->second.touch < victim->second.touch)) victim = k;
    if (victim == known_.end()) return nullptr;
    DropPiecesLocked(victim->first);
    known_.erase(victim);
  }
  Known& n = known_.emplace(skel, Known{}).first->second;
  n.touch = ++touch_seq_;
  return &n;
}

void WrestlerContributor::ListAddLocked(uint32_t model) {
  if (model <= 1) return;
  int32_t free_at = -1;
  uint32_t at = ListHash(model);
  for (uint32_t i = 0; i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
    const uint32_t v = listed_[at].load(std::memory_order_relaxed);
    if (v == model) return;
    if (v == 1 && free_at < 0) free_at = int32_t(at);
    if (v == 0) { if (free_at < 0) free_at = int32_t(at); break; }
  }
  if (free_at < 0) { listed_full_.fetch_add(1, std::memory_order_relaxed); return; }
  listed_[uint32_t(free_at)].store(model, std::memory_order_relaxed);
}

void WrestlerContributor::ListRemoveLocked(uint32_t model) {
  uint32_t at = ListHash(model);
  for (uint32_t i = 0; i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
    const uint32_t v = listed_[at].load(std::memory_order_relaxed);
    if (v == 0) return;
    if (v == model) { listed_[at].store(1, std::memory_order_relaxed); return; }
  }
}

void WrestlerContributor::DropPiecesLocked(uint32_t body) {
  auto it = known_.find(body);
  if (it == known_.end()) return;
  for (const PieceRef& p : it->second.model.pieces) {
    auto o = piece_owner_.find(p.model);
    if (o != piece_owner_.end() && o->second == body) piece_owner_.erase(o), ListRemoveLocked(p.model);
  }
  it->second.model.pieces.clear();
  if (piece_owner_.empty())   // lane 206 (note): tombstones never clear by themselves; an empty owner table empties the lock-free one
    for (auto& a : listed_) a.store(0, std::memory_order_relaxed);
}

void WrestlerContributor::Put(uint32_t skel, int32_t slot, uint64_t frame, std::vector<Mat4>&& skin, bool consumer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (Known* k = KnownFor(skel)) {
    if (consumer) {
      if (slot < 0) slot = k->slot;  // the consumer hook cannot match a slot; the update hook did
    } else {
      if (slot >= 0) k->slot = slot;
      k->max_count = uint32_t(skin.size());  // the latest count, so a reused address does not keep an old one
      // The two hooks tag frames from different threads, so source priority must not depend on equal tags: while a consumer read for this skeleton
      // is fresh, a derived palette is bookkeeping only (slot and count above).
      if (k->has_consumer && frame <= k->consumer_frame + kConsumerFresh) return;
    }
    if (consumer) {
      k->has_consumer = true;
      k->consumer_frame = std::max(k->consumer_frame, frame);
      // derived updates that raced ahead of this read (higher tag, same skeleton) must not outrank it
      auto pc = cur_.find(skel);
      if (pc != cur_.end()) {
        auto& pu = pc->second.upds;
        pu.erase(std::remove_if(pu.begin(), pu.end(), [&](const Upd& x) { return !x.consumer && x.frame <= frame + kConsumerFresh; }), pu.end());
      }
    }
  }
  auto it = cur_.find(skel);
  if (it == cur_.end()) {
    const size_t held = last_.size() + cur_.size();
    // lane 206 (S1): a skin under the body size (a piece's own skeleton) leaves kBodyHeadroom places free for bodies
    const bool small_skin = skin.size() < MinNodes();
    if (!last_.count(skel) && (held >= kMaxSkeletons || (small_skin && held >= kMaxSkeletons - kBodyHeadroom))) { cap_refused_.fetch_add(1, std::memory_order_relaxed); NoteUnreadable(); return; }  // bounded memory
    it = cur_.emplace(skel, Pending{}).first;
  }
  auto& u = it->second.upds;
  for (Upd& x : u)
    if (x.frame == frame) {
      if (first_wins_) return;  // the planted fault: keep the first update of the frame
      if (x.consumer && !consumer) {  // the draw's own read beats a derived palette of the same frame, whichever hook ran last
        if (slot >= 0) x.slot = slot;
        return;
      }
      x.slot = slot >= 0 || !x.consumer ? slot : x.slot;
      x.consumer = consumer;
      x.skin = std::move(skin);  // the last update of a frame wins
      return;
    }
  Upd n;
  n.frame = frame, n.slot = slot, n.consumer = consumer, n.skin = std::move(skin);
  u.push_back(std::move(n));
  if (u.size() > kMaxPending) u.erase(u.begin());
}

void WrestlerContributor::SubmitSkin(uint32_t skel, int32_t slot, uint32_t count, const Mat4* skin, uint64_t frame) {
  if (!skin || count == 0) { NoteUnreadable(); return; }
  Put(skel, slot, frame, std::vector<Mat4>(skin, skin + count));
}

void WrestlerContributor::SubmitWorld(uint32_t skel, int32_t slot, uint32_t count, const Mat4* world, const Mat4* inv_bind, uint64_t frame) {
  if (!world || !inv_bind || count == 0) { NoteUnreadable(); return; }
  std::vector<Mat4> skin(count);
  for (uint32_t i = 0; i < count; ++i) skin[i] = SkinFromWorld(world[i], inv_bind[i]);
  Put(skel, slot, frame, std::move(skin));
}

void WrestlerContributor::SubmitConsumer(uint32_t skel, uint32_t count, const Mat4* skin, uint64_t frame) {
  if (!skin || count == 0) { NoteUnreadable(); return; }
  consumer_submits_.fetch_add(1, std::memory_order_relaxed);
  Put(skel, -1, frame, std::vector<Mat4>(skin, skin + count), true);
}

uint32_t WrestlerContributor::WantCount(uint32_t skel) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = known_.find(skel);
  return it == known_.end() ? 0u : it->second.max_count;
}

void WrestlerContributor::NotePalettePtr(uint32_t skel, uint32_t palette_ptr) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (Known* k = KnownFor(skel)) k->palette_ptr = palette_ptr;
}

void WrestlerContributor::NoteEntryUnknownModel(uint32_t model, uint32_t palette_ptr) {
  bool piece = false, by_desc = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto o = piece_owner_.find(model);  // O(1): the descriptor lists it (the game's own owner table)
    if (o != piece_owner_.end() && o->second != model) piece = by_desc = true;
    if (!piece && palette_ptr)  // else the shared +200 pointer
      for (const auto& kv : known_)
        if (kv.first != model && kv.second.palette_ptr == palette_ptr) { piece = true; break; }
  }
  if (by_desc) entry_pieces_desc_.fetch_add(1, std::memory_order_relaxed);
  (piece ? entry_pieces_ : entry_unknown_model_).fetch_add(1, std::memory_order_relaxed);
}

void WrestlerContributor::SubmitModelLink(uint32_t body_model, uint32_t key, uint32_t pach_buf, uint32_t pach_size, uint32_t desc,
                                          std::vector<PieceRef>&& pieces) {
  std::lock_guard<std::mutex> lock(mutex_);
  Known* k = KnownFor(body_model);
  if (!k) { link_unreadable_.fetch_add(1, std::memory_order_relaxed); return; }
  // A model was (re)built at this address: whatever the table held for the old one (node count, palette pointer, slot, links, consumer freshness)
  // belongs to a dead object. The pose hooks refill it within a frame or two.
  DropPiecesLocked(body_model);
  cur_.erase(body_model);   // pending / kept palettes of the dead object (with its slot) must not be emitted for the new one
  last_.erase(body_model);
  *k = Known{};
  k->touch = ++touch_seq_;
  k->model.known = true, k->model.key = key, k->model.pach_buf = pach_buf, k->model.pach_size = pach_size, k->model.desc = desc;
  link_bodies_.fetch_add(1, std::memory_order_relaxed);
  link_pieces_.fetch_add(pieces.size(), std::memory_order_relaxed);
  link_kind_[KindOfKey(key)].fetch_add(1, std::memory_order_relaxed);
  for (const PieceRef& p : pieces) piece_owner_[p.model] = body_model, ListAddLocked(p.model);
  k->model.pieces = std::move(pieces);
}

WrestlerContributor::ModelKeyInfo WrestlerContributor::ModelKeyFor(uint32_t body_model) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = known_.find(body_model);
  return it == known_.end() ? ModelKeyInfo{} : it->second.model;
}

uint32_t WrestlerContributor::OwnerOfPiece(uint32_t piece_model) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto o = piece_owner_.find(piece_model);
  return o == piece_owner_.end() ? 0u : o->second;
}

void WrestlerContributor::SubmitAssetLink(uint32_t skel, const std::string& pac, const std::string& entry, uint32_t variant) {
  std::lock_guard<std::mutex> lock(mutex_);
  Known* k = KnownFor(skel);
  if (!k || pac.empty()) { NoteUnreadable(); return; }
  k->declared.pac = pac, k->declared.entry = entry, k->declared.variant = variant, k->declared.known = true;
}

void WrestlerContributor::SubmitAssetGuess(uint32_t skel, const std::string& pac, const std::string& entry, uint32_t variant) {
  std::lock_guard<std::mutex> lock(mutex_);
  Known* k = KnownFor(skel);
  if (!k || pac.empty()) { NoteUnreadable(); return; }
  k->guessed.pac = pac, k->guessed.entry = entry, k->guessed.variant = variant, k->guessed.known = true;
  guessed_links_.fetch_add(1, std::memory_order_relaxed);
}

WrestlerContributor::Link WrestlerContributor::GuessFor(uint32_t skel) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = known_.find(skel);
  return it == known_.end() ? Link{} : it->second.guessed;
}

void WrestlerContributor::ApplyDelta(uint32_t skel, float dx, float dy, float dz) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Mat4>* skin = nullptr;
  auto p = cur_.find(skel);
  if (p != cur_.end() && !p->second.upds.empty()) {
    if (!p->second.upds.back().consumer) skin = &p->second.upds.back().skin;  // a consumer palette is what +200 held at the draw: the delta is the game's to have applied (or not) by then
  } else if (auto l = last_.find(skel); l != last_.end()) {
    if (!l->second.consumer) skin = &l->second.skin;
  }
  if (!skin) return;
  for (Mat4& m : *skin) m.m[12] += dx, m.m[13] += dy, m.m[14] += dz;
}

void WrestlerContributor::Emit(const FrameInfo& f, SceneTables& t, Counters& c) {
  struct Item { uint32_t skel; int32_t slot; bool consumer; Link link; std::vector<Mat4> skin; };
  std::vector<Item> items;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<uint32_t> refreshed;
    for (auto it = cur_.begin(); it != cur_.end();) {
      auto& u = it->second.upds;
      int pick = -1;  // the newest update at or before this record's frame
      for (size_t i = 0; i < u.size(); ++i)
        if (f.guest_frame == 0 || u[i].frame <= f.guest_frame) pick = (pick < 0 || u[i].frame >= u[size_t(pick)].frame) ? int(i) : pick;
      if (pick >= 0) {
        Kept& k = last_[it->first];
        k.slot = u[size_t(pick)].slot;
        k.consumer = u[size_t(pick)].consumer;
        k.skin = std::move(u[size_t(pick)].skin);
        k.age = 0;
        refreshed.insert(it->first);
        const uint64_t picked_frame = u[size_t(pick)].frame;
        u.erase(std::remove_if(u.begin(), u.end(), [&](const Upd& x) { return x.frame <= picked_frame; }), u.end());
      }
      it = u.empty() ? cur_.erase(it) : std::next(it);
    }
    for (auto it = last_.begin(); it != last_.end();) {
      if (!refreshed.count(it->first)) {
        ++it->second.age;
        if (it->second.age > kPruneAfter) { it = last_.erase(it); continue; }
        ++c.stale;  // per skeleton not refreshed since the last record
      }
      Link link;
      int32_t slot = it->second.slot;
      if (auto kn = known_.find(it->first); kn != known_.end()) {
        kn->second.age = 0;
        if (kn->second.slot >= 0) slot = kn->second.slot;  // the slot the update hook matched wins (a consumer palette carries none)
        if (kn->second.model.known && !kn->second.seen_as_skeleton) {
          kn->second.seen_as_skeleton = true;  // desc+4 (the body model) turned up as a skeleton the pose hooks update: the identity holds for this body
          linked_seen_as_skel_.fetch_add(1, std::memory_order_relaxed);
        }
        link = kn->second.declared;  // a guess never enters the record
        if (!link.known && kn->second.model.known) {
          // The game's model key is [C]; the pac NAME is [U] (R1): a pac-less AssetRef (pac 0 = unknown) whose entry names the key and whose variant is the attire.
          link.known = true;
          link.entry = "model_key:" + std::to_string(kn->second.model.key);
          link.variant = AttireOfKey(kn->second.model.key);
        }
      }
      items.push_back(Item{it->first, slot, it->second.consumer, link, it->second.skin});
      ++it;
    }
  }
  {  // bookkeeping of skeletons that are in neither cur_ nor last_ ages out (a reused address must not inherit a stale count / link)
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = known_.begin(); it != known_.end();) {
      if (cur_.count(it->first) || last_.count(it->first)) { ++it; continue; }
      if (++it->second.age > kKnownAge) {
        DropPiecesLocked(it->first);
        it = known_.erase(it);
      } else {
        ++it;
      }
    }
  }
  c.unknown += uint32_t(unreadable_.exchange(0, std::memory_order_relaxed));
  if (items.empty()) return;

  // Slot-matched skeletons first (slot order), then the rest by address: a deterministic order.
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    const bool sa = a.slot >= 0, sb = b.slot >= 0;
    if (sa != sb) return sa;
    if (sa && a.slot != b.slot) return a.slot < b.slot;
    return a.skel < b.skel;
  });

  // One placeholder AssetRef for "asset unknown" (pac 0 = none): the record's reader requires every Object::asset to be in range.
  // A drawer skips an AssetRef with pac 0 (rsc_draw does).
  // S101 lane 43: the engine-object column is parallel to objects; pad it for the classes before this one (live_scene.cpp pads after us too).
  t.engine_objects.resize(t.objects.size(), 0u);
  const uint32_t unknown_asset = uint32_t(t.assets.size());
  AssetRef none;
  none.kind = uint8_t(AssetKind::Model);
  none.variant = 0xFFFFFFFFu;
  t.assets.push_back(none);
  const uint32_t light = light_set_.load(std::memory_order_relaxed);
  std::set<int32_t> used_slots;
  std::map<std::string, uint32_t> linked;
  for (const Item& s : items) {
    // A slot claimed twice in one record: the first claim keeps "actor:slot:N", a second becomes a skel: id (and invisible).
    const bool slot_ok = s.slot >= 0 && used_slots.insert(s.slot).second;
    char id[40];
    if (slot_ok) std::snprintf(id, sizeof(id), "actor:slot:%d", s.slot);
    else std::snprintf(id, sizeof(id), "skel:%08X", s.skel);
    Object o;
    o.asset = unknown_asset;
    if (s.link.known) {  // declared link (the char model reader): one AssetRef per distinct (pac, entry, variant) in this record
      const std::string key = s.link.pac + '\n' + s.link.entry + '\n' + std::to_string(s.link.variant);
      auto f2 = linked.find(key);
      if (f2 == linked.end()) {
        AssetRef a;
        a.kind = uint8_t(AssetKind::Model);
        a.pac = t.AddString(s.link.pac);
        a.entry = t.AddString(s.link.entry);
        a.variant = s.link.variant;
        f2 = linked.emplace(key, uint32_t(t.assets.size())).first;
        t.assets.push_back(a);
      }
      o.asset = f2->second;
    }
    o.klass = rdr_adapter::DrawClass::ModelPart;
    // A skeleton that matches no character slot (managers, waiting entrance models, extras; size filter only) is not shown: visible only with a slot.
    o.flags = kObjSkinned | (slot_ok ? uint32_t(kObjVisible) : 0u);
    o.id = t.AddString(id);
    o.world = IdentityMat4();  // the palette is already in world space (the root matrix is folded in by sub_82BD30E8); unconfirmed against the front-end
    o.bones_first = uint32_t(t.bones.size());
    o.bones_count = uint32_t(s.skin.size());
    t.bones.insert(t.bones.end(), s.skin.begin(), s.skin.end());
    o.light_set = light;
    // mesh mask stays all-ones (every mesh shown): the attire's piece list is unknown, so this is "unknown", not a claim.
    t.objects.push_back(o);
    t.engine_objects.push_back(s.skel);  // the pairing key with DrawOrigin::object (the model object IS the skeleton; map section 1)
    ++c.objects;
    c.unknown += (s.link.known && !s.link.pac.empty()) ? 1 : 2;   // (asset: a key without a pac name is still unknown,) mesh mask
    if (!s.consumer) {                   // the palette came from the exit of sub_82BD30E8: not where the draw reads it (Brock's nodes 59-108)
      ++c.unknown;
      derived_fallbacks_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!slot_ok) ++c.unknown;           // the skeleton -> wrestler link
    if (light == kUnknown) ++c.unknown;  // light set
  }
}

}  // namespace rdr_scene::live
