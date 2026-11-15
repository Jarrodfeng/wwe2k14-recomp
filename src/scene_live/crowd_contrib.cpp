// The crowd contributor (Session 101, lane 27). See crowd_contrib.h. No game or SDK includes.
#include "crowd_contrib.h"

#include "../renderer_adapter/rdr_switches.h"  // S102 T11: CrowdIdsOnce, read every frame (no game or SDK includes)
#include "../renderer_scene/swap_once.h"        // S102 SW1

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

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


// ---- S102 U1 (docs\renderer\SWAP_TAIL.md): the snapshot every Emit path takes, now in one place so the latch can take it before the release ----
// The block the three paths had (unchanged): no crowd draw since the last record -> the first two such frames repeat the snapshot (a frame the crowd was
// not drawn in), after that the crowd is not on screen (a menu, another scene) and nothing is emitted; counted once per episode.
void Contributor::SnapLocked(bool& skip, bool& count_stale, std::vector<MemberObs>& members, Palettes& palettes, int& arena, uint32_t& other) {
  skip = count_stale = false;
  if (draw_seq_ == emitted_seq_) {
    ++stale_run_;
    if (stale_run_ == 3) count_stale = true;
    if (stale_run_ >= 3) { skip = true; return; }
  } else {
    stale_run_ = 0;
  }
  members = published_;  // (a copy: the hooks go on; into kept storage where the caller keeps it)
  for (size_t i = 0; i < palettes.size(); ++i) palettes[i] = published_palettes_[i];
  arena = arena_;
  other = published_other_;
  emitted_seq_ = draw_seq_;
}

bool Contributor::Latch(const FrameInfo& f) {
  std::lock_guard<std::mutex> lock(mu_);
  SnapLocked(latch_.skip, latch_.count_stale, latch_.members, latch_.palettes, latch_.arena, latch_.other);
  latch_.mark.Set(f.guest_frame);
  return true;
}

bool Contributor::TakeInputs(const FrameInfo& f, Counters& c, std::vector<MemberObs>& members, Palettes& palettes, int& arena, uint32_t& other) {
  bool skip = false, count_stale = false;
  if (latch_.mark.Use(f.guest_frame, Name())) {
    skip = latch_.skip, count_stale = latch_.count_stale;
    if (!skip) {
      members.swap(latch_.members);   // (capacity cycles between the latch and the caller's kept storage)
      for (size_t i = 0; i < palettes.size(); ++i) palettes[i].swap(latch_.palettes[i]);
      arena = latch_.arena;
      other = latch_.other;
    }
  } else {
    std::lock_guard<std::mutex> lock(mu_);
    SnapLocked(skip, count_stale, members, palettes, arena, other);
  }
  if (count_stale) ++c.stale;
  return !skip;
}

// S102 T11 F7: Emit (this classic body) and EmitCached below make the same record. A change to what one writes (an object field, a string, the order of the
// AddString calls) goes into BOTH; tools\crowd\crowd_ids_check.py (the byte-equality arms) proves they still agree.
void Contributor::Emit(const FrameInfo& finfo, SceneTables& t, Counters& c) {
  // S102 T19: the marker path (a record with CrowdRun markers, not the same bytes: the expanded member list is the same); read every frame at the point of use.
  if (rdr_sw::BoolSeen(rdr_sw::kCrowdRunMarkers, g_crowd_run_markers.load(std::memory_order_relaxed), finfo.guest_frame)) {
    EmitRuns(finfo, t, c);
    return;
  }
  // S102 T11: the cached path (same record bytes); read every frame at the point of use.
  if (rdr_sw::BoolSeen(rdr_sw::kCrowdIdsOnce, g_crowd_ids_once.load(std::memory_order_relaxed), finfo.guest_frame)) {
    EmitCached(finfo, t, c);
    return;
  }
  std::vector<MemberObs> members;
  std::array<std::vector<float>, kTypes * kLods> palettes;
  int arena = -1;
  uint32_t other = 0;
  if (!TakeInputs(finfo, c, members, palettes, arena, other)) return;   // S102 U1: the block moved to SnapLocked (unchanged), the latch's copy or one now
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

// ---- S102 T11: the cached path -------------------------------------------------------------------------------------------------------------
// Same objects, same strings in the same order, so the record's bytes are the classic path's (the offline test serialises both and compares). What
// changes: a member's id string is formatted once per (kind, type, index) and kept; the strings are interned through the flat index (no heap string
// per call); the member and palette copies, the asset / palette lookups and the object vector reuse their storage across frames.
const std::string* Contributor::CachedId(uint32_t variant, uint32_t block, uint32_t n, PooledField** pooled) {
#ifdef RDR_CROWD_IDS_PLANTS
  const uint32_t pool_variant = plant == 5 ? (variant >= 3 ? 3u : 0u) : variant;   // planted 5: the pool field is kept without the kind
  if (plant == 1) variant = variant >= 3 ? 3u : 0u;   // the kind is ignored: block, floor and card share the entries of kind "block"
  if (plant == 2) block = 0;               // the type is ignored
#else
  const uint32_t pool_variant = variant;
#endif
  if (n >= kIdCacheMax) return nullptr;
  std::vector<std::string>& v = id_cache_[variant][block];
  if (v.size() <= n) v.resize(size_t(n) + 1);
  if (pooled) {   // S102 T11b
    std::vector<PooledField>& pv = id_pool_[pool_variant][block];
    if (pv.size() <= n) pv.resize(size_t(n) + 1);
    *pooled = &pv[n];
  }
  std::string& id = v[n];
  if (id.empty()) {
    char b[64];
    const bool card = variant % 3 == 2, floor = variant % 3 == 1;
    const char* kind = card ? "card" : floor ? "floor" : "block";
    if (variant < 3) std::snprintf(b, sizeof b, "crowd:%s:%u:%s%u", kind, block, card ? "" : "member:", n);
    else std::snprintf(b, sizeof b, "crowd:%s:%u:%su%u", kind, block, card ? "" : "member:", n);
    id = b;
    ++ids_stats_.id_made;
  } else {
    ++ids_stats_.id_hits;
  }
  return &id;
}

// S102 SW1: the kept pool field of (variant, block, n); the same slot (and the same plant) CachedId hands out. n < kIdCacheMax, block < kTypes (the caller's checks).
PooledField* Contributor::PoolSlot(uint32_t variant, uint32_t block, uint32_t n) {
#ifdef RDR_CROWD_IDS_PLANTS
  const uint32_t pool_variant = plant == 5 ? (variant >= 3 ? 3u : 0u) : variant;
#else
  const uint32_t pool_variant = variant;
#endif
  std::vector<PooledField>& pv = id_pool_[pool_variant][block];
  if (pv.size() <= n) pv.resize(size_t(n) + 1);
  return &pv[n];
}

// S102 T11 F7: the cached twin of Emit above; change both (see there). The entry-string cache (entry_cache_) is keyed (card, lod, type), which is all the text
// depends on; AssetRef::variant is the constant 0 here ([U], see Emit), so it is not part of the key: if variant ever becomes a value read from the game, the key
// must gain it (the byte-equality arms and planted twin 4, a cache keyed without the lod, show such a key mistake is caught).
void Contributor::EmitCached(const FrameInfo& f, SceneTables& t, Counters& c) {
  int arena = -1;
  uint32_t other = 0;
  if (!TakeInputs(f, c, scratch_members_, scratch_palettes_, arena, other)) {   // S102 U1 (kept storage: swapped with the latch's, no allocation once grown)
    ++ids_stats_.declined;
    if (StringPool* sp = t.SeedPool()) ++sp->stats.crowd_stale;   // S102 T11b
    return;
  }
  const std::vector<MemberObs>& members = scratch_members_;
  const auto& palettes = scratch_palettes_;
  c.unknown += other;
  ++ids_stats_.frames;
  ids_stats_.members += members.size();
  // (the ids do not depend on the arena: the cache is kept across arena changes; only the pac string and the asset entries are per arena, and the pac string is keyed by it)
  if (f.guest_frame >= ids_window_start_ + 600 || ids_window_start_ == 0) {
    if (ids_window_start_ != 0) {   // the census line, every 600 frames while the lever is on (R13 / S102 rule)
      char line[320];
      std::snprintf(line, sizeof line,
                    "[rdr][crowd_ids_once] frames %llu-%llu, taken %llu, declined %llu (stale crowd), members %llu, id hits %llu, made %llu, uncached %llu",
                    (unsigned long long)ids_window_start_, (unsigned long long)f.guest_frame, (unsigned long long)ids_stats_.frames,
                    (unsigned long long)ids_stats_.declined, (unsigned long long)ids_stats_.members, (unsigned long long)ids_stats_.id_hits,
                    (unsigned long long)ids_stats_.id_made, (unsigned long long)ids_stats_.id_uncached);
      if (auto log = rdr_sw::g.log.load(std::memory_order_relaxed)) log(line);
    }
    ids_window_start_ = f.guest_frame ? f.guest_frame : 1;
  }

  // S102 T11b: a table seeded from the string pool already has its index (the pool's strings are in it) and needs room for only the strings this frame still adds (the pac string,
  // the asset entries, the pose ids, and any id the pool does not hold yet); the member ids are the pool's fields.
  StringPool* pool = t.SeedPool();
  if (pool) {
    if (pooled_for_ != pool->id()
#ifdef RDR_CROWD_IDS_PLANTS
        && plant != 6
#endif
    ) {   // another pool: the fields kept for the old one are not this one's
      for (auto& per_kind : id_pool_)
        for (auto& v : per_kind) v.clear();
      pooled_for_ = pool->id();
    }
#ifdef RDR_CROWD_IDS_PLANTS
    if (plant == 6) pooled_for_ = pool->id();
#endif
  } else {
    t.UseFlatStringIndex(members.size() + 2 * kTypes + 128, members.size() * 28 + 8192);
  }
  uint64_t pooled_hits = 0, pooled_misses = 0, fast_n = 0;
  const bool fast_ids = pool && swap_once::Has(swap_once::kCrowdIds);   // S102 SW1 (wwe2k14_rdr_swap_once bit 2): a member whose pool field is kept skips the id cache
  t.objects.reserve(t.objects.size() + members.size() + kTypes);
  t.assets.reserve(t.assets.size() + 2 * kLods * kTypes);
  {
    size_t nodes = 0;
    for (const auto& p : palettes) nodes += p.size() / 16;
    t.bones.reserve(t.bones.size() + nodes);
  }

  uint32_t pac_field = 0;
  if (arena >= 0) {
#ifdef RDR_CROWD_IDS_PLANTS
    const bool keep = plant == 3 && pac_arena_ != -2;   // planted: the pac string of the first arena stays
#else
    const bool keep = false;
#endif
    if (pac_arena_ != arena && !keep) {
      char b[64];
      std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", arena);
      pac_cache_ = b;
      pac_arena_ = arena;
    }
    pac_field = t.AddString(pac_cache_);
  }
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  std::array<uint32_t, 2 * kLods * kTypes> asset_of;
  asset_of.fill(kNone);
  auto asset = [&](bool card, uint32_t type, uint32_t lod) {
    uint32_t& slot = asset_of[(size_t(card) * kLods + lod) * kTypes + type];
    if (slot != kNone) return slot;
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_field;
#ifdef RDR_CROWD_IDS_PLANTS
    std::string& e = entry_cache_[card ? 1 : 0][plant == 4 ? 0 : lod][type];   // planted 4: keyed without the lod
#else
    std::string& e = entry_cache_[card ? 1 : 0][lod][type];
#endif
    if (e.empty()) {
      char b[32];
      if (card) std::snprintf(b, sizeof b, "card:%02u", type);
      else std::snprintf(b, sizeof b, "%u", 20000u + 40u * lod + type);
      e = b;
    }
    a.entry = t.AddString(e);
    a.variant = 0;
    if (!pac_field) ++c.unknown;
    ++c.unknown;
    t.assets.push_back(a);
    return slot = uint32_t(t.assets.size() - 1);
  };
  struct Range { uint32_t first = 0, count = 0; bool have = false; };
  std::array<Range, kTypes * kLods> range_of;
  auto palette = [&](uint32_t type, uint32_t lod, uint32_t& first, uint32_t& count) {
    const uint32_t slot = lod * kTypes + type;
    Range& r = range_of[slot];
    if (r.have) {
      first = r.first, count = r.count;
      return count != 0;
    }
    const std::vector<float>& p = palettes[slot];
    first = uint32_t(t.bones.size()), count = uint32_t(p.size() / 16);
    for (uint32_t i = 0; i < count; ++i) {
      Mat4 m;
      std::memcpy(m.m, p.data() + size_t(i) * 16, sizeof m.m);
      t.bones.push_back(m);
    }
    r = {first, count, true};
    return count != 0;
  };
  uint32_t unknown_ordinal = 0;
  for (const MemberObs& m : members) {
    if (m.block >= kTypes) {
      ++c.unknown;
      continue;
    }
    const bool card = m.card;
    const bool floor = m.floor && !card;
    const uint32_t lod = card ? kCardLayer : floor ? 0u : (m.lod < kLods ? m.lod : 0);
    if (!card && !floor && m.lod >= kLods) ++c.unknown;
    Object o;
    o.asset = asset(card, m.block, card ? 0 : lod);
    o.klass = rdr_adapter::DrawClass::Crowd;
    o.layer = uint8_t(lod);
    o.flags = kObjVisible;
    o.world = WorldFromConstants(m.c);
    const uint32_t kind = card ? 2u : floor ? 1u : 0u;
    const bool known = m.index != kUnknown;
    const uint32_t n = known ? m.index : unknown_ordinal++;
    if (!known) ++c.unknown;
    PooledField* pf = nullptr;
    if (fast_ids && n < kIdCacheMax && (pf = PoolSlot(kind + (known ? 0u : 3u), m.block, n))->field) {   // S102 SW1: the field is kept: no id string, no cache entry touched
      o.id = pf->field;
      ++fast_n, ++pooled_hits;
    } else if (const std::string* id = CachedId(kind + (known ? 0u : 3u), m.block, n, pool ? &pf : nullptr)) {
      uint32_t fld = 0;
      if (pf) {   // S102 T11b: the pool field the entry kept: no hashing, no probe, no compare
        fld = pf->field ? pf->field : PooledFieldOf(*pool, *pf, [id]() { return *id; });
        ++(fld ? pooled_hits : pooled_misses);
      }
      o.id = fld ? fld : t.AddString(*id);   // (a string the pool does not hold yet: interned this frame the old way, queued for the pool)
    } else {   // an index past the cache: formatted into the stack, still no heap
      char b[64];
      const char* kn = card ? "card" : floor ? "floor" : "block";
      std::snprintf(b, sizeof b, known ? "crowd:%s:%u:%s%u" : "crowd:%s:%u:%su%u", kn, m.block, card ? "" : "member:", n);
      o.id = t.AddString(b);
      ++ids_stats_.id_uncached;
    }
    {
      uint32_t first = 0, count = 0;
      if (palette(m.block, card ? 0 : lod, first, count)) {
        o.flags |= kObjSkinned;
        o.bones_first = first, o.bones_count = count;
      } else {
        ++c.unknown;
      }
    }
    t.objects.push_back(o);
    ++c.objects;
  }
  if (pose_objects_) {
    std::array<bool, kTypes> used{};
    for (const MemberObs& m : members)
      if (m.block < kTypes) used[m.block] = true;
    for (uint32_t type = 0; type < kTypes; ++type) {
      if (!used[type]) continue;
      uint32_t first = 0, count = 0;
      if (!palette(type, 0, first, count)) continue;
      Object o;
      o.world = IdentityMat4();
      o.asset = asset(false, type, 0);
      o.klass = rdr_adapter::DrawClass::Crowd;
      o.layer = 0;
      o.flags = kObjSkinned;
      std::string& pid = pose_id_[type];
      if (pid.empty()) {
        char b[32];
        std::snprintf(b, sizeof b, "crowd:pose:%02u", type);
        pid = b;
      }
      o.id = t.AddString(pid);
      o.bones_first = first, o.bones_count = count;
      t.objects.push_back(o);
    }
  }
  if (pool) pool->stats.crowd_fields += pooled_hits, pool->stats.crowd_fallback += pooled_misses;   // S102 T11b
  if (fast_ids) {   // S102 SW1: the census (one relaxed add each a frame); the id cache's own hit count keeps counting the members the fast path served
    ids_stats_.id_hits += fast_n;
    swap_once::g_count.crowd_fast.fetch_add(fast_n, std::memory_order_relaxed);
    swap_once::g_count.crowd_slow.fetch_add(uint64_t(members.size()) - fast_n, std::memory_order_relaxed);
  }
}

// ---- S102 T19: the marker path --------------------------------------------------------------------------------------------------------------------
// One CrowdRun per run of consecutive members that share (kind, person type, LOD group), one CrowdMember (12 constants + the index) per member; no Object, no id string. The
// assets, palettes, pac / entry strings and pose objects are made exactly as in Emit / EmitCached (same order of first use, same counters), so rdr_scene::ExpandCrowdRuns of
// the record equals the plain record's member list (crowd_run_markers_test.cpp compares every object). A change to what Emit writes goes into all THREE paths.
void Contributor::EmitRuns(const FrameInfo& f, SceneTables& t, Counters& c) {
  int arena = -1;
  uint32_t other = 0;
  if (!TakeInputs(f, c, scratch_members_, scratch_palettes_, arena, other)) { ++run_stats_.declined_stale; return; }   // S102 U1
  const std::vector<MemberObs>& members = scratch_members_;
  const auto& palettes = scratch_palettes_;
  c.unknown += other;
  ++run_stats_.frames;
  if (f.guest_frame >= run_window_start_ + 600 || run_window_start_ == 0) {
    if (run_window_start_ != 0) {   // the census line, every 600 frames while the lever is on (S102 rule); the counts are the window's
      char line[320];
      std::snprintf(line, sizeof line,
                    "[rdr][crowd_run_markers] frames %llu-%llu, taken %llu, runs %llu, members %llu, declined %llu (stale crowd %llu, no recordable member %llu), single-member runs %llu",
                    (unsigned long long)run_window_start_, (unsigned long long)f.guest_frame, (unsigned long long)run_stats_.frames, (unsigned long long)run_stats_.runs,
                    (unsigned long long)run_stats_.members, (unsigned long long)(run_stats_.declined_stale + run_stats_.declined_empty), (unsigned long long)run_stats_.declined_stale,
                    (unsigned long long)run_stats_.declined_empty, (unsigned long long)run_stats_.singles);
      if (auto log = rdr_sw::g.log.load(std::memory_order_relaxed)) log(line);
      run_stats_ = RunStats{};
      ++run_stats_.frames;   // (this frame is the new window's first)
    }
    run_window_start_ = f.guest_frame ? f.guest_frame : 1;
  }

  t.UseFlatStringIndex(2 * kTypes + 2 * kLods * kTypes + 16, 2048);
  t.assets.reserve(t.assets.size() + 2 * kLods * kTypes);
  t.crowd_members.reserve(t.crowd_members.size() + members.size());
  t.crowd_runs.reserve(t.crowd_runs.size() + last_runs_ + 8);   // S102 T19 F5: from the previous frame's count
  if (pose_objects_) t.objects.reserve(t.objects.size() + kTypes);   // (the pose objects: at most one per person type)
  {
    size_t nodes = 0;
    for (const auto& p : palettes) nodes += p.size() / 16;
    t.bones.reserve(t.bones.size() + nodes);
  }
  uint32_t pac_field = 0;
  if (arena >= 0) {
    if (pac_arena_ != arena) {
      char b[64];
      std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", arena);
      pac_cache_ = b;
      pac_arena_ = arena;
    }
    pac_field = t.AddString(pac_cache_);
  }
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  std::array<uint32_t, 2 * kLods * kTypes> asset_of;
  asset_of.fill(kNone);
  auto asset = [&](bool card, uint32_t type, uint32_t lod) {
    uint32_t& slot = asset_of[(size_t(card) * kLods + lod) * kTypes + type];
    if (slot != kNone) return slot;
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_field;
    std::string& e = entry_cache_[card ? 1 : 0][lod][type];
    if (e.empty()) {
      char b[32];
      if (card) std::snprintf(b, sizeof b, "card:%02u", type);
      else std::snprintf(b, sizeof b, "%u", 20000u + 40u * lod + type);
      e = b;
    }
    a.entry = t.AddString(e);
    a.variant = 0;
    if (!pac_field) ++c.unknown;
    ++c.unknown;
    t.assets.push_back(a);
    return slot = uint32_t(t.assets.size() - 1);
  };
  struct Range { uint32_t first = 0, count = 0; bool have = false; };
  std::array<Range, kTypes * kLods> range_of;
  auto palette = [&](uint32_t type, uint32_t lod, uint32_t& first, uint32_t& count) {
    const uint32_t slot = lod * kTypes + type;
    Range& r = range_of[slot];
    if (r.have) {
      first = r.first, count = r.count;
      return count != 0;
    }
    const std::vector<float>& p = palettes[slot];
    first = uint32_t(t.bones.size()), count = uint32_t(p.size() / 16);
    for (uint32_t i = 0; i < count; ++i) {
      Mat4 m;
      std::memcpy(m.m, p.data() + size_t(i) * 16, sizeof m.m);
      t.bones.push_back(m);
    }
    r = {first, count, true};
    return count != 0;
  };
  const uint32_t at = uint32_t(t.objects.size());   // the members go here when a reader expands the markers (the pose objects stay plain, after them)
  const size_t runs_before = t.crowd_runs.size();
  std::array<bool, kTypes> used{};
  uint32_t run_kind = 9, run_block = 0, run_layer = 0;   // the open run's key (kind 9 = none open)
  bool run_has_palette = false;
  uint32_t recorded = 0;
  for (const MemberObs& m : members) {
    if (m.block >= kTypes) {
      ++c.unknown;
      continue;
    }
    used[m.block] = true;
    const bool card = m.card;
    const bool floor = m.floor && !card;
    const uint32_t lod = card ? kCardLayer : floor ? 0u : (m.lod < kLods ? m.lod : 0);
    if (!card && !floor && m.lod >= kLods) ++c.unknown;
    const uint32_t kind = card ? uint32_t(kRunCard) : floor ? uint32_t(kRunFloor) : uint32_t(kRunBlock);
#ifdef RDR_CROWD_RUNS_PLANTS
    const bool same = run_kind == kind && run_block == m.block && (run_plant == 1 || run_layer == lod);   // planted 1: a run goes on across a LOD (= palette) change
#else
    const bool same = run_kind == kind && run_block == m.block && run_layer == lod;
#endif
    if (!same) {
      CrowdRun r;
      r.at = at;
      r.first_member = uint32_t(t.crowd_members.size());
      r.count = 0;
      r.asset = asset(card, m.block, card ? 0 : lod);
      uint32_t first = 0, count = 0;
      run_has_palette = palette(m.block, card ? 0 : lod, first, count);
      if (run_has_palette) r.skinned = 1, r.bones_first = first, r.bones_count = count;
      r.kind = uint8_t(kind), r.block = uint8_t(m.block), r.layer = uint8_t(lod);
      t.crowd_runs.push_back(r);
      run_kind = kind, run_block = m.block, run_layer = lod;
    }
    ++t.crowd_runs.back().count;
    CrowdMember cm;
    std::memcpy(cm.c, m.c, sizeof cm.c);
    if (m.index != kUnknown) cm.index = m.index;
    else cm.index = kCrowdNoIndex, ++c.unknown;   // (the id's per-record ordinal is the expander's count of such members, in order)
    t.crowd_members.push_back(cm);
    if (!run_has_palette) ++c.unknown;   // no palette read for this (type, group): the body stays at its stored pose (counted per member, as in Emit)
    ++c.objects;
    ++recorded;
  }
  run_stats_.members += recorded;
  run_stats_.runs += t.crowd_runs.size() - runs_before;
  last_runs_ = t.crowd_runs.size() - runs_before;
  if (!recorded) ++run_stats_.declined_empty;
  for (size_t i = runs_before; i < t.crowd_runs.size(); ++i)
    if (t.crowd_runs[i].count == 1) ++run_stats_.singles;
  if (pose_objects_) {
    for (uint32_t type = 0; type < kTypes; ++type) {
      if (!used[type]) continue;
      uint32_t first = 0, count = 0;
      if (!palette(type, 0, first, count)) continue;
      Object o;
      o.world = IdentityMat4();
      o.asset = asset(false, type, 0);
      o.klass = rdr_adapter::DrawClass::Crowd;
      o.layer = 0;
      o.flags = kObjSkinned;
      std::string& pid = pose_id_[type];
      if (pid.empty()) {
        char b[32];
        std::snprintf(b, sizeof b, "crowd:pose:%02u", type);
        pid = b;
      }
      o.id = t.AddString(pid);
      o.bones_first = first, o.bones_count = count;
      t.objects.push_back(o);
    }
  }
#ifdef RDR_CROWD_RUNS_PLANTS
  if (run_plant == 3)   // planted 3: `at` taken after the pose objects (the members would be expanded behind them)
    for (size_t i = runs_before; i < t.crowd_runs.size(); ++i) t.crowd_runs[i].at = uint32_t(t.objects.size());
#endif
}

Contributor& The() {
  static Contributor c;
  return c;
}

}  // namespace rdr_scene::live::crowd
