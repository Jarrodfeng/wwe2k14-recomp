// D1 thin draws, the BINDER side's store (Session 101 lane 122, KB; docs\renderer\SKIP_GAME_WORK_MAP.md 2.3). No game or SDK includes: the unit test, fview and the game's
// binder all build THIS header.
//
// The binder LEARNS a key from a P2a replacement that passed every check (Learn), draws a lite draw from its template (Find), re-proves it on the key's proof frame
// (Compare), UNPINS it when a check fails, and RELEASES the template only through the handshake (thin_draw.h):
//   * a key leaves the published set at the end of the frame that unpinned it (EndFrame publishes; the new set has the next generation g);
//   * its template is erased only when the frame being bound carries a generation >= g (the reader that made it had read a set without the key, and the reader's frames
//     carry non-decreasing generations, so no later frame can name the key) AND the key's last use is kReleaseFrames old.
// Planted `release_without_ack`: erased at the unpin itself. A lite draw whose template is gone is a hole.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "thin_draw.h"

namespace rdr_adapter::thin {

// The differences between two templates, one bit per Differ.
inline uint32_t Compare(const ThinTemplate& have, const ThinTemplate& now) {
  uint32_t mask = 0;
  const auto set = [&](Differ d) { mask |= 1u << uint32_t(d); };
  if (have.vs != now.vs || have.ps != now.ps) set(Differ::Program);
  if (have.layout != now.layout || have.topology != now.topology || have.index_32bit != now.index_32bit) set(Differ::Layout);
  if (have.vertex.buffer != now.vertex.buffer || have.vertex.offset != now.vertex.offset || have.index.buffer != now.index.buffer || have.index.offset != now.index.offset ||
      have.index_count != now.index_count)
    set(Differ::Geometry);
  for (int s = 0; s < rdr::kMaxTextures; ++s) {
    const ThinSlot& a = have.slots[s];
    const ThinSlot& b = now.slots[s];
    if (a.set != b.set || (a.set && a.image != b.image)) set(Differ::SlotImage);
    if (a.set && b.set && (a.view_format != b.view_format || a.swizzle[0] != b.swizzle[0] || a.swizzle[1] != b.swizzle[1] || a.swizzle[2] != b.swizzle[2] || a.swizzle[3] != b.swizzle[3]))
      set(Differ::SlotView);
    if (a.set && b.set && (a.min_level != b.min_level || a.max_level != b.max_level)) set(Differ::Levels);
  }
  if (have.stream_vertex_count != now.stream_vertex_count || have.stream_index_count != now.stream_index_count) set(Differ::Counts);
  if (have.blend_base != now.blend_base) set(Differ::Blend);
  return mask;
}

class ThinStore {
 public:
  struct Entry {
    ThinKey key;
    ThinTemplate t;
    uint64_t last_use = 0;
    bool pinned = true;
    bool pending = false;   // unpinned this frame; the generation that lacks it is known at EndFrame
    uint32_t unpin_gen = 0; // the first published generation without the key
  };

  explicit ThinStore(PinPublisher* publisher = &g_pins) : pub_(publisher) {}
  void SetFault(Fault f) { fault_ = f; }
  void SetReleaseFrames(uint32_t n) { release_frames_ = n; }  // the unit test turns the 300-frame minimum off to prove the handshake alone is sound
  Fault fault() const { return fault_; }
  // Review F2: ONE class's assets went (its hook destroyed the buffers / images its templates name): that class's templates go at once (a frame in flight finds none: an epoch drop, never a hole), the others stay.
  uint32_t ClearClass(Cls c) {
    uint32_t n = 0;
    for (auto it = map_.begin(); it != map_.end();) {
      if (it->second.t.cls == c) it = map_.erase(it), ++n;
      else ++it;
    }
    if (n) dirty_ = true;
    return n;
  }
  // Review F2: the hook destroyed ONE buffer (a wrestler part swept after 240 idle frames): only the templates naming it go (vertex or index buffer), at once.
  uint32_t EraseByBuffer(uint64_t buffer) {
    uint32_t n = 0;
    for (auto it = map_.begin(); it != map_.end();) {
      if (uint64_t(it->second.t.vertex.buffer) == buffer || uint64_t(it->second.t.index.buffer) == buffer) it = map_.erase(it), ++n;
      else ++it;
    }
    if (n) dirty_ = true;
    return n;
  }
  uint64_t PinnedCount(Cls c) const {  // lane 129 (KC): the census line of one class
    uint64_t n = 0;
    for (const auto& kv : map_) n += kv.second.pinned && kv.second.t.cls == c;
    return n;
  }
  void Clear() {
    dirty_ = true;
    map_.clear();
    tie_slot0_.clear();
    first_ = ThinTemplate{};
    have_first_ = false;
  }

  // A frame is being bound: its number, and the generation it carries (what the reader read; 0 when it read nothing).
  void BeginFrame(uint64_t frame, uint32_t stamp_generation) {
    frame_ = frame;
    stamp_ = stamp_generation;
  }

  // The template for a lite draw (null = a hole). A key that was unpinned but not yet released is still usable by frames that predate the unpin: that is the point.
  const ThinTemplate* Find(uint64_t key_hash) {
    auto it = map_.find(key_hash);
    if (it == map_.end()) return nullptr;
    it->second.last_use = frame_;
    return &it->second.t;
  }
  // The entry only when it is pinned (the proof asks this: an unpinned key is re-learned, not compared).
  const Entry* Pinned(uint64_t key_hash) const {
    const auto it = map_.find(key_hash);
    return it != map_.end() && it->second.pinned ? &it->second : nullptr;
  }
  bool Known(uint64_t key_hash) const { return map_.find(key_hash) != map_.end(); }

  // A P2a replacement passed every check: this is the key's template (a new key, or an unpinned one coming back). Counted by the caller via the return: true = learned.
  bool Learn(const ThinKey& key, ThinTemplate t) {
    ApplyLearnFault(key, t);
    const uint64_t h = key.Hash();
    auto it = map_.find(h);
    if (it != map_.end() && it->second.pinned) return false;
    t.learned_frame = uint32_t(frame_);
    Entry e;
    e.key = key, e.t = t, e.last_use = frame_;
    map_[h] = e;
    dirty_ = true;
    return true;
  }
  // The proof found a different bind (or a check failed): the template is replaced by the new P2a bind, in place (a re-template). The key stays pinned.
  void Retemplate(const ThinKey& key, ThinTemplate t) {
    ApplyLearnFault(key, t);
    auto it = map_.find(key.Hash());
    if (it == map_.end()) return;
    t.learned_frame = uint32_t(frame_);
    it->second.t = t;
    it->second.last_use = frame_;
  }
  // A failed P2a check on a pinned key (or a test's churn): out of the published set at the end of this frame.
  bool Unpin(uint64_t key_hash) {
    auto it = map_.find(key_hash);
    if (it == map_.end() || !it->second.pinned) return false;
    it->second.pinned = false;
    it->second.pending = true;
    dirty_ = true;
    if (fault_ == Fault::ReleaseWithoutAck) {  // the planted hole: gone at once, whatever frames the reader has in flight
      map_.erase(it);
      ++released_;
    }
    return true;
  }

  struct EndResult {
    uint32_t generation = 0;
    uint64_t released = 0, pinned = 0;
  };
  // After a bound frame: publish the pinned set (a new generation only when it changed) and release what the handshake allows.
  EndResult EndFrame() {
    EndResult r;
    if (dirty_) {  // the set changed (learn / unpin / clear): rebuild and publish; otherwise the last answer stands (no per-frame sort)
      std::vector<uint64_t> keys, triples, arena_triples, wrestler_triples;
      keys.reserve(map_.size());
      for (const auto& [h, e] : map_) {
        if (!e.pinned) continue;
        keys.push_back(h);
        triples.push_back(e.key.Triple());
        if (e.t.cls == Cls::Arena) arena_triples.push_back(e.key.Triple());   // lane 152 (13)
        if (e.t.cls == Cls::Wrestlers) wrestler_triples.push_back(e.key.Triple());   // lane 227 (H2)
      }
      last_pinned_ = keys.size();
      last_gen_ = pub_ ? pub_->Publish(std::move(keys), std::move(triples), false, std::move(arena_triples), std::move(wrestler_triples)) : 0;
      waiting_ = true;
    }
    r.pinned = last_pinned_;
    r.generation = last_gen_;
    if (waiting_) {  // entries out of the set wait for the handshake
      bool any = false;
      for (auto it = map_.begin(); it != map_.end();) {
        Entry& e = it->second;
        if (e.pending && !e.pinned) e.pending = false, e.unpin_gen = r.generation;
        const bool acked = !e.pinned && !e.pending && stamp_ >= e.unpin_gen && frame_ >= e.last_use + release_frames_;
        if (acked) {
          it = map_.erase(it);
          ++r.released;
          ++released_;
        } else {
          any = any || !e.pinned;
          ++it;
        }
      }
      waiting_ = any;
    }
    dirty_ = false;
    return r;
  }
  // Every pinned key of one object leaves the set (the arena dropped the object's join: its templates were proven against a join that is gone). Returns how many.
  uint32_t UnpinObject(uint32_t object) {
    uint32_t n = 0;
    for (auto& [h, e] : map_) {
      if (e.pinned && e.key.object == object) {
        e.pinned = false, e.pending = true, ++n;
        if (fault_ == Fault::ObjectSibling) break;  // planted: only the first key of the object leaves, its siblings keep drawing a stale mesh
      }
    }
    if (n) dirty_ = true;
    if (n && fault_ == Fault::ReleaseWithoutAck)
      for (auto it = map_.begin(); it != map_.end();) it = (!it->second.pinned && it->second.key.object == object) ? map_.erase(it) : std::next(it);
    return n;
  }
  uint64_t released() const { return released_; }
  size_t size() const { return map_.size(); }
  // For tests / the census: the keys currently pinned, deterministic order not guaranteed.
  std::vector<uint64_t> PinnedKeys() const {
    std::vector<uint64_t> v;
    for (const auto& [h, e] : map_)
      if (e.pinned) v.push_back(h);
    return v;
  }

 private:
  void ApplyLearnFault(const ThinKey& key, ThinTemplate& t) {
    switch (fault_) {
      case Fault::FirstTemplate:  // lane 35's class: one material for every member (the first key ever learned decides every slot)
        if (!have_first_) first_ = t, have_first_ = true;
        else
          for (int s = 0; s < rdr::kMaxTextures; ++s) t.slots[s] = first_.slots[s];
        break;
      case Fault::WrongSiteTemplate:  // Session 97's half-arena class: the ORIGINAL's template used on the mirrored copy. In Check B both sites draw the same meshes with the same template (measured: a
                                      // pure site swap changes nothing), so the plant takes the colour site's template of the object's PART 0 for the mirror-site key of any part
        if (key.pass_site == 0x82C0B94Cu) {
          auto it = map_.begin();
          for (; it != map_.end(); ++it)
            if (it->second.key.pass_site == 0x82C0A950u && it->second.key.object == key.object && it->second.key.part == 0 && it->second.key.ordinal == 0) break;
          if (it != map_.end()) {
            ThinTemplate original = it->second.t;
            original.cls = t.cls, original.stream_vertex_count = t.stream_vertex_count, original.stream_index_count = t.stream_index_count, original.blend_base = t.blend_base;
            t = original;
          }
        }
        break;
      case Fault::SkirtTie: {  // lane 129 (KC) R: RING_OURS.md 23, the skirts 973 / 974 have identical geometry and different textures: the first template with these stream counts decides slot 0 for every key with them
        if (t.cls != Cls::Ring || !t.slots[0].set) break;
        const uint64_t group = Mix(t.stream_vertex_count, t.stream_index_count);
        const auto it = tie_slot0_.find(group);
        if (it == tie_slot0_.end()) tie_slot0_[group] = t.slots[0];
        else t.slots[0] = it->second;
        break;
      }
      case Fault::AllMips: // lanes 68 / 70: the template's image view takes the file's whole chain, not the stream slot's level window
        for (int s = 0; s < rdr::kMaxTextures; ++s)
          if (t.slots[s].set) t.slots[s].min_level = 0, t.slots[s].max_level = 15;
        break;
      default: break;
    }
  }

  PinPublisher* pub_;
  Fault fault_ = Fault::None;
  std::unordered_map<uint64_t, Entry> map_;
  ThinTemplate first_;
  std::unordered_map<uint64_t, ThinSlot> tie_slot0_;  // lane 129 (KC): the SkirtTie plant
  bool have_first_ = false, dirty_ = false, waiting_ = false;
  uint64_t last_pinned_ = 0;
  uint32_t last_gen_ = 0;
  uint64_t frame_ = 0, released_ = 0;
  uint32_t stamp_ = 0, release_frames_ = kReleaseFrames;
};

// The keys of a frame, the binder's way (the reader counts the same ordinals only for pinned triples, reader_thin.h): over ALL the frame's candidate draws in draw order.
// get(i, site, object, part) returns true when draw i is a candidate (a model part at one of the two colour sites with an object). keys[i] = 0: not a candidate.
template <class Get>
inline void ComputeKeys(uint32_t n, Get get, std::vector<uint64_t>& keys, std::vector<ThinKey>* full = nullptr, bool no_ordinal = false,
                        const std::unordered_map<uint32_t, ThinKey>* reserved = nullptr,
                        const std::unordered_map<uint32_t, const std::vector<uint64_t>*>* fill = nullptr) {  // lane 289 (the LIVE form of `reserved`): fill[i] = the part triples of the skipped call whose MARKER is draw i; they count at the marker's position (the reader counts the same list, reader_thin.h Reserve).  // get(i, site, object, part, program); lane 258: `reserved` = draws cut from the frame (a D3 skip): not keyed, but they still count in the ordinals of their triple
  keys.assign(n, 0);
  if (full) full->assign(n, ThinKey{});
  std::unordered_map<uint64_t, uint32_t> count;
  count.reserve(256);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t site = 0, object = 0, part = 0;
    uint64_t program = 0;
    if (!get(i, site, object, part, program)) {
      if (reserved) {
        const auto rv = reserved->find(i);
        if (rv != reserved->end()) ++count[TripleHash(rv->second.pass_site, rv->second.object, rv->second.part)];
      }
      if (fill) {
        const auto fv = fill->find(i);
        if (fv != fill->end())
          for (const uint64_t triple : *fv->second) ++count[triple];
      }
      continue;
    }
    const uint32_t ordinal = no_ordinal ? (count[TripleHash(site, object, part)]++, 0u) : count[TripleHash(site, object, part)]++;  // (no_ordinal: the planted fault)
    keys[i] = KeyHash(site, object, part, ordinal, program);
    if (full) (*full)[i] = ThinKey{site, object, part, ordinal, program};
  }
}

}  // namespace rdr_adapter::thin
