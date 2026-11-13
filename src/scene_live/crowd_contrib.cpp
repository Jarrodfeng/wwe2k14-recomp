// The crowd contributor (Session 101, lane 27). See crowd_contrib.h. No game or SDK includes.
#include "crowd_contrib.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace rdr_scene::live::crowd {

void Contributor::BeginDraw(uint32_t read_ring) {
  building_.clear();  // (the draw thread's own vector: no lock)
  building_other_ = 0;
  draw_ring_ = read_ring > 1 ? 0 : read_ring;
}

void Contributor::NoteMember(const MemberObs& m) { building_.push_back(m); }

void Contributor::NoteUnmodelled() { ++building_other_; }

void Contributor::EndDraw() {
  std::lock_guard<std::mutex> lock(mu_);
  published_.swap(building_);
  published_other_ = building_other_;
  building_.clear();
  published_palettes_ = palettes_[draw_ring_];  // the set the draw's read ring names
  ++draw_seq_;
}

void Contributor::SetPalette(uint32_t write_ring, uint32_t type, uint32_t lod, const float* matrices, uint32_t nodes) {
  if (write_ring > 1 || type >= kTypes || lod >= kLods || !matrices || nodes == 0 || nodes > kPaletteNodes) return;
  std::lock_guard<std::mutex> lock(mu_);
  palettes_[write_ring][lod * kTypes + type].assign(matrices, matrices + size_t(nodes) * 16);
}

void Contributor::SetArena(int number) {
  std::lock_guard<std::mutex> lock(mu_);
  if (number != arena_) {  // another arena: the old one's members and poses are not this one's
    published_.clear(), building_.clear();
    published_other_ = 0;
    for (auto& set : palettes_) for (auto& p : set) p.clear();
    for (auto& p : published_palettes_) p.clear();
    stale_run_ = 0;
  }
  arena_ = number;
}

void Contributor::Reset() {
  std::lock_guard<std::mutex> lock(mu_);
  building_.clear(), published_.clear();
  building_other_ = published_other_ = 0;
  for (auto& set : palettes_) for (auto& p : set) p.clear();
  for (auto& p : published_palettes_) p.clear();
  draw_seq_ = emitted_seq_ = 0;
  stale_run_ = 0;
  draw_ring_ = 0;
  arena_ = -1;
}

void Contributor::Emit(const FrameInfo&, SceneTables& t, Counters& c) {
  std::vector<MemberObs> members;
  std::array<std::vector<float>, kTypes * kLods> palettes;
  int arena;
  uint32_t other;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (draw_seq_ == emitted_seq_) {
      // No crowd draw since the last record: the first two such frames repeat the snapshot (a frame the crowd was not drawn in), after that the
      // crowd is not on screen (a menu, another scene) and nothing is emitted; counted once per episode.
      ++stale_run_;
      if (stale_run_ == 3) ++c.stale;
      if (stale_run_ >= 3) return;
    } else {
      stale_run_ = 0;
    }
    members = published_;  // (a copy: the hooks go on)
    palettes = published_palettes_;
    arena = arena_;
    other = published_other_;
    emitted_seq_ = draw_seq_;
  }
  c.unknown += other;  // members of the populations not modelled (signs / foam hands, the other op-21 sites): seen, not recorded
  uint32_t pac_field = 0;
  if (arena >= 0) {
    char b[64];
    std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", arena);
    pac_field = t.AddString(b);
  }
  // Assets: one per (card?, type, lod), made on first use.
  std::map<uint32_t, uint32_t> asset_of;  // key = (card << 16) | (lod << 8) | type
  auto asset = [&](bool card, uint32_t type, uint32_t lod) {
    const uint32_t key = (card ? 1u << 16 : 0u) | (lod << 8) | type;
    if (auto it = asset_of.find(key); it != asset_of.end()) return it->second;
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_field;
    char e[32];
    if (card) std::snprintf(e, sizeof e, "card:%02u", type);
    else std::snprintf(e, sizeof e, "%u", 20000u + 40u * lod + type);
    a.entry = t.AddString(e);
    a.variant = 0;  // the texture variant (the game's table index, aud_NNa): unknown, 'a' assumed [U]
    if (!pac_field) ++c.unknown;
    ++c.unknown;  // (the variant)
    t.assets.push_back(a);
    return asset_of[key] = uint32_t(t.assets.size() - 1);
  };
  // Palettes: one range of kScBones per (type, lod) actually used.
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> range_of;
  auto palette = [&](uint32_t type, uint32_t lod, uint32_t& first, uint32_t& count) {
    const uint32_t slot = lod * kTypes + type;
    if (auto it = range_of.find(slot); it != range_of.end()) {
      first = it->second.first, count = it->second.second;
      return count != 0;
    }
    const std::vector<float>& p = palettes[slot];
    first = uint32_t(t.bones.size()), count = uint32_t(p.size() / 16);
    for (uint32_t i = 0; i < count; ++i) {
      Mat4 m;
      std::memcpy(m.m, p.data() + size_t(i) * 16, sizeof m.m);
      t.bones.push_back(m);
    }
    range_of[slot] = {first, count};
    return count != 0;
  };
  uint32_t unknown_ordinal = 0;  // members with no readable index: unique within this record (not stable across frames)
  for (const MemberObs& m : members) {
    if (m.block >= kTypes) {
      ++c.unknown;  // a member whose block was not read: nothing to name it by
      continue;
    }
    const bool card = m.card;
    const bool floor = m.floor && !card;  // S101 lane 82: a floor person is always the type's LOD 0 body (crowd_floor.h refuses any other group)
    const uint32_t lod = card ? kCardLayer : floor ? 0u : (m.lod < kLods ? m.lod : 0);
    if (!card && !floor && m.lod >= kLods) ++c.unknown;
    Object o;
    o.asset = asset(card, m.block, card ? 0 : lod);
    o.klass = rdr_adapter::DrawClass::Crowd;
    o.layer = uint8_t(lod);
    o.flags = kObjVisible;
    o.world = WorldFromConstants(m.c);
    char id[64];
    const char* kind = card ? "card" : floor ? "floor" : "block";
    if (m.index != kUnknown) std::snprintf(id, sizeof id, "crowd:%s:%u:%s%u", kind, m.block, card ? "" : "member:", m.index);
    else std::snprintf(id, sizeof id, "crowd:%s:%u:%su%u", kind, m.block, card ? "" : "member:", unknown_ordinal++), ++c.unknown;
    o.id = t.AddString(id);
    {
      // (a card is the impostor of its type's posed person: it takes the type's LOD 0 palette, so a reader drawing a body for it poses it like the type)
      uint32_t first = 0, count = 0;
      if (palette(m.block, card ? 0 : lod, first, count)) {
        o.flags |= kObjSkinned;
        o.bones_first = first, o.bones_count = count;
      } else {
        ++c.unknown;  // no palette read for this (type, group): the body stays at its stored pose
      }
    }
    t.objects.push_back(o);
    ++c.objects;
  }
  if (pose_objects_) {  // S101 lane 35: the LOD 0 palette of every used type, by an invisible object (not counted in c.objects)
    std::array<bool, kTypes> used{};
    for (const MemberObs& m : members)
      if (m.block < kTypes) used[m.block] = true;
    for (uint32_t type = 0; type < kTypes; ++type) {
      if (!used[type]) continue;
      uint32_t first = 0, count = 0;
      if (!palette(type, 0, first, count)) continue;
      Object o;
      o.world = IdentityMat4();  // (Mat4 has no initialiser: an unset world would write stack bytes into every record)
      o.asset = asset(false, type, 0);
      o.klass = rdr_adapter::DrawClass::Crowd;
      o.layer = 0;
      o.flags = kObjSkinned;  // (not kObjVisible)
      char id[32];
      std::snprintf(id, sizeof id, "crowd:pose:%02u", type);
      o.id = t.AddString(id);
      o.bones_first = first, o.bones_count = count;
      t.objects.push_back(o);
    }
  }
}

Contributor& The() {
  static Contributor c;
  return c;
}

}  // namespace rdr_scene::live::crowd
