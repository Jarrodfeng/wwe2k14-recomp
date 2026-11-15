// The scene record as a file (Session 101, lane 14). See record_io.h and docs\renderer\SCENE_RECORD.md "Files".
#include "record_io.h"
#include "../renderer/render_lean.h"   // S102 RT1

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <type_traits>

namespace rdr_scene {

bool g_crowd_run_roundtrip = false;   // S102 T19
#ifdef RDR_CROWD_RUNS_PLANTS
int g_run_plant = 0;   // test only (tools/crowd/crowd_run_markers_test.cpp): 1 a run is not broken where the LOD (palette) changes; 2 a member is dropped from a run; 3 the old format is not read; 4 ids rebuilt off by one; 5 the marker bounds checks are gone; 6 the index-less ordinal restarts in every run; 7 the engine-object zeros are appended at the end, not at `at`; 8 the folder sink writes a marker record as it is
#endif

namespace {

constexpr uint32_t kMaxSections = 64;
constexpr uint64_t kAlign = 8;

uint64_t AlignUp(uint64_t v) { return (v + kAlign - 1) & ~(kAlign - 1); }

std::string Fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

// One section to write: its id, element size and the bytes of the in-memory table.
struct Out { uint32_t id; uint32_t size; uint64_t count; const void* data; };

template <class T> Out Sec(uint32_t id, const std::vector<T>& v) {
  static_assert(std::is_trivially_copyable_v<T>);
  return {id, uint32_t(sizeof(T)), v.size(), v.data()};
}

template <class T>
bool Load(size_t size, const uint8_t* data, const SectionEntry& e, std::vector<T>& out, std::string& error) {
  if (e.element_size != sizeof(T)) {
    error = Fmt("section %s: element_size %u in the file, this reader knows %zu (a different record layout)", SectionName(e.id), e.element_size, sizeof(T));
    return false;
  }
  if (e.offset > size || e.count > (size - e.offset) / sizeof(T)) {
    error = Fmt("section %s: truncated (%llu elements of %u bytes at offset %llu, the file has %zu bytes)", SectionName(e.id), (unsigned long long)e.count,
                e.element_size, (unsigned long long)e.offset, size);
    return false;
  }
  out.resize(size_t(e.count));
  if (e.count) std::memcpy(out.data(), data + e.offset, size_t(e.count) * sizeof(T));
  return true;
}

}  // namespace

const char* SectionName(uint32_t id) {
  switch (id) {
    case kScStrings: return "strings";
    case kScCamera: return "Camera";
    case kScAssets: return "AssetRef";
    case kScObjects: return "Object";
    case kScBones: return "Mat4(bones)";
    case kScParams: return "ParamValue";
    case kScLights: return "Light";
    case kScSprites: return "Sprite";
    case kScTexts: return "Text";
    case kScMovies: return "Movie";
    case kScTargets: return "Target";
    case kScEngineObjects: return "uint32(engine objects)";
    case kScLightStates: return "LightState";
    case kScLightRefs: return "LightRef";
    case kScCircleShadows: return "CircleShadow";  // lane 136
    case kScCircleMesh: return "CircleMesh";  // lane 136
    case kScPlaneFrames: return "PlaneFrame";  // lane 139
    case kScPlaneDraws: return "PlaneDraw";  // lane 139
    case kScArenaWorlds: return "ArenaWorld";  // S101 lane 130
    case kScRingRows: return "RingRows";  // lane 194
    case kScRingNodes: return "RingNodeRows";  // lane 194
    case kScCasterCalls: return "CasterCall";  // lane 202
    case kScCasterStates: return "CasterState";  // lane 202
    case kScCasterNodes: return "CasterNode";  // lane 217
    case kScCasterExtras: return "CasterExtra";  // lane 221
    case kScCasterMats: return "CasterMat";  // lane 226
    case kScSphereOcc: return "SphereOcc";   // lane 242
    case kScCrowdRuns: return "CrowdRun";  // S102 T19
    case kScCrowdMembers: return "CrowdMember";  // S102 T19
    default: return "unknown";
  }
}

#ifdef RDR_CROWD_IDS_PLANTS
bool g_expand_keeps_prefix = false;
#endif

SceneTables::SceneTables() {
  camera.view = IdentityMat4();
  camera.proj = IdentityMat4();
}

// S102 SW1: a fresh table's state with the allocations kept. Mirrors the member list of record_io.h in declaration order.
void SceneTables::Recycle() {
  if (rdr::g_lean_plant.load(std::memory_order_relaxed) != 6) arena_memo.v.store(-2, std::memory_order_relaxed);   // S102 RT1 review: a refilled table is a new record (plant 6, tools only: the reset is left out)
  header = SceneHeader{};
  has_camera = false;
  camera = Camera{};
  camera.view = IdentityMat4();
  camera.proj = IdentityMat4();
  strings.clear();
  assets.clear(), objects.clear(), bones.clear(), params.clear(), lights.clear(), sprites.clear(), texts.clear(), movies.clear(), targets.clear();
  engine_objects.clear(), light_states.clear(), light_refs.clear();
  plane_errors = 0, circle_errors = 0;
  circle_shadows.clear(), circle_mesh.clear(), plane_frames.clear(), plane_draws.clear();
  arena_world_errors = 0;
  arena_worlds.clear();
  ring_errors = 0;
  ring_rows.clear(), ring_nodes.clear();
  caster_errors = 0;
  caster_calls.clear(), caster_mats.clear(), caster_states.clear(), caster_extras.clear();
#ifdef RDR_SWAP_PLANTS
  if (g_recycle_plant != 2) caster_nodes.clear(), sphere_errors = 0;
#else
  caster_nodes.clear();
  sphere_errors = 0;
#endif
  sphere_occ.clear();
  crowd_runs.clear(), crowd_members.clear();
  if (!string_index_.empty()) string_index_.clear();
#ifdef RDR_SWAP_PLANTS
  if (g_recycle_plant == 3) return;
#endif
  indexed_size_ = 0;
  flat_ = false;
  seed_ = SeedLink{};
  flat_slots_.clear();
  flat_count_ = 0;
#ifdef RDR_CROWD_IDS_PLANTS
  seed_plant = 0, flat_plant = 0;
#endif
}

// S102 T11: the flat index. The hash is FNV-1a over the bytes; a slot holds a field (offset + 1), so a lookup compares against the blob itself.
#ifdef RDR_CROWD_IDS_PLANTS
#define RDR_NUL_OK(c) (flat_plant == 2 || (c) == '\0')
#else
#define RDR_NUL_OK(c) ((c) == '\0')
#endif
static uint64_t FlatHash(const char* p, size_t n) { return FlatStringHash(p, n); }   // S102 T11b: one definition (string_pool.h), shared with the pool

void SceneTables::UseFlatStringIndex(size_t strings_hint, size_t bytes_hint) {
  bool keep = PoolSeeded() && flat_ && indexed_size_ == strings.size() && !flat_slots_.empty();   // S102 T11b: only a seeded table keeps its index (any other caller may have replaced the blob with one of the same size: R20 review F3)
#ifdef RDR_CROWD_IDS_PLANTS
  if (seed_plant == 3) keep = flat_ && indexed_size_ == strings.size() && !flat_slots_.empty();
#endif
  if (keep) {   // already flat and in step (a seeded table): keep the index, only make room
    size_t want_now = flat_slots_.size();
    while (want_now < (strings_hint + flat_count_) * 2 + 2) want_now <<= 1;
    if (want_now > flat_slots_.size()) { flat_slots_.assign(want_now, 0u); indexed_size_ = size_t(-1); }   // (a bigger table: rebuilt from the blob on the next AddString)
    if (bytes_hint && strings.capacity() < strings.size() + bytes_hint) strings.reserve(strings.size() + bytes_hint);
    return;
  }
  if (bytes_hint && strings.capacity() < strings.size() + bytes_hint) strings.reserve(strings.size() + bytes_hint);
  string_index_ = {};  // (the node map is not used from here on)
  flat_ = true;
  size_t want = 16;
  while (want < (strings_hint + flat_count_) * 2 + 2) want <<= 1;
  if (flat_slots_.size() < want) flat_slots_.assign(want, 0u);
  indexed_size_ = size_t(-1);  // rebuild from the blob on the next AddString
}

bool SceneTables::SeedStrings(StringPool& pool) {
  if (!strings.empty() || flat_ || indexed_size_ != 0) return false;   // only a fresh table: the pool's strings are the blob's start
  const std::string& blob = pool.Blob();
  strings.reserve(blob.size() + 8192);   // (room for the frame's own strings: no regrow of a copied blob)
  strings.assign(blob);
  flat_ = true;
  string_index_ = {};
  seed_.pool = &pool;
  seed_.bytes = strings.size();
#ifdef RDR_CROWD_IDS_PLANTS
  if (seed_plant == 1) {   // planted: the blob without its index (FlatAdd does not look in the pool either)
    flat_slots_.assign(16, 0u);
    flat_count_ = 0;
    indexed_size_ = strings.size();
    return true;
  }
#endif
  flat_slots_.assign(256, 0u);   // the table's OWN index: the strings added after the seed. The pool's strings are found in the pool's index (FlatAdd), not copied
  flat_count_ = 0;
  indexed_size_ = strings.size();
  return true;
}

template <class F>
static void ForEachStringField(SceneTables& t, F&& f) {   // every name field of the tables (scene_record.h: the nine "string:" fields)
  f(t.header.address);
  for (AssetRef& a : t.assets) f(a.pac), f(a.entry);
  for (Object& o : t.objects) f(o.id);
  for (ParamValue& p : t.params) f(p.name);
  for (Target& g : t.targets) f(g.name);
  for (Sprite& s : t.sprites) f(s.widget);
  for (Text& x : t.texts) f(x.utf8), f(x.widget);
}

std::vector<uint32_t> SceneTables::StringFields() const {
  SceneTables copy_of_fields_only;   // (the visitor takes a mutable table: only the field vectors are copied)
  copy_of_fields_only.header = header;
  copy_of_fields_only.assets = assets, copy_of_fields_only.objects = objects, copy_of_fields_only.params = params, copy_of_fields_only.targets = targets;
  copy_of_fields_only.sprites = sprites, copy_of_fields_only.texts = texts;
  std::vector<uint32_t> out;
  ForEachStringField(copy_of_fields_only, [&](uint32_t& f) { out.push_back(f); });
  return out;
}

void SceneTables::CompactStrings() {
  std::string old;
  old.swap(strings);
  std::unordered_map<std::string_view, uint32_t> made;
  auto str_of = [&](uint32_t field) -> std::string_view {
    if (field == 0 || field > old.size()) return {};
    const size_t pos = field - 1, end = old.find('\0', pos);
    return std::string_view(old.data() + pos, (end == std::string::npos ? old.size() : end) - pos);
  };
  ForEachStringField(*this, [&](uint32_t& field) {
    const std::string_view s = str_of(field);
    if (s.empty()) { field = field > old.size() ? field : 0; return; }   // (a field past the blob stays as it was: Validate reports it, as before)
    auto it = made.find(s);
    if (it == made.end()) {
      it = made.emplace(s, uint32_t(strings.size() + 1)).first;
      strings.append(s.data(), s.size());
      strings.push_back('\0');
    }
    field = it->second;
  });
  string_index_ = {};
  flat_ = false;
  flat_slots_.clear();
  flat_count_ = 0;
  indexed_size_ = size_t(-1);   // the next AddString rebuilds the node map from the compact blob
  seed_ = SeedLink();
}

// A table seeded from a pool whose blob is still the table's prefix (not a moved-from shell; not the planted index-less table).
bool SceneTables::PoolSeeded() const {
#ifdef RDR_CROWD_IDS_PLANTS
  if (seed_plant == 1) return false;
#endif
  return seed_.pool && seed_.bytes && strings.size() >= seed_.bytes;
}
bool SceneTables::PoolHas(std::string_view s, uint64_t h) const {
  const uint32_t f = seed_.pool->Find(s, h);
  return f && size_t(f) - 1 + s.size() < seed_.bytes;
}

void SceneTables::FlatInsert(uint32_t field, uint64_t hash) {
  const size_t mask = flat_slots_.size() - 1;
  for (size_t i = size_t(hash) & mask;; i = (i + 1) & mask)
    if (!flat_slots_[i]) { flat_slots_[i] = field; ++flat_count_; return; }
}

void SceneTables::FlatGrow() {  // twice the slots (at least 16), every field reinserted by hashing the blob's own bytes
  std::vector<uint32_t> old;
  old.swap(flat_slots_);
  flat_slots_.assign(old.size() * 2 > 16 ? old.size() * 2 : 16, 0u);
  flat_count_ = 0;
  for (uint32_t f : old)
    if (f) { const char* q = strings.data() + (f - 1); FlatInsert(f, FlatHash(q, std::strlen(q))); }
}

void SceneTables::FlatRebuild() {
#ifdef RDR_CROWD_IDS_PLANTS
  if (flat_plant != 3)
#endif
  if (flat_slots_.empty()) flat_slots_.assign(16, 0u);   // S102 T11 F1: a moved-from table keeps flat_ but has no slots
  std::fill(flat_slots_.begin(), flat_slots_.end(), 0u);
  flat_count_ = 0;
  size_t pos = 0;
  if (PoolSeeded()) pos = seed_.bytes;   // S102 T11b: the seeded prefix is the pool's (found in the pool's index); only what came after is indexed here
  while (pos < strings.size()) {
    const char* base = strings.data();
    const void* nul = std::memchr(base + pos, 0, strings.size() - pos);
    const size_t len = nul ? size_t(static_cast<const char*>(nul) - (base + pos)) : strings.size() - pos;
    if ((flat_count_ + 1) * 2 > flat_slots_.size()) FlatGrow();
    const uint64_t h = FlatHash(base + pos, len);
    const size_t mask = flat_slots_.size() - 1;
    bool found = PoolSeeded() && PoolHas(std::string_view(base + pos, len), h);   // (a tail string equal to a pool string is not a new one)
    for (size_t i = size_t(h) & mask; !found && flat_slots_[i]; i = (i + 1) & mask) {
      const size_t at = flat_slots_[i] - 1;
      if (at + len < strings.size() && std::memcmp(base + at, base + pos, len) == 0 && RDR_NUL_OK(base[at + len])) { found = true; break; }  // (the first of equal strings keeps its field)
    }
    if (!found) FlatInsert(uint32_t(pos + 1), h);
    pos += len + 1;
  }
  indexed_size_ = strings.size();
}

uint32_t SceneTables::FlatAdd(std::string_view s) {
#ifdef RDR_CROWD_IDS_PLANTS
  if (flat_plant != 3)
#endif
    if (flat_slots_.empty()) flat_slots_.assign(16, 0u);   // S102 T11 F1: the guard (a moved-from table has no slots; the mask below would be SIZE_MAX)
#ifdef RDR_CROWD_IDS_PLANTS
  if (indexed_size_ != strings.size() && !(flat_plant == 1 && flat_count_ != 0)) FlatRebuild();
#else
  if (indexed_size_ != strings.size()) FlatRebuild();
#endif
  const uint64_t h = FlatHash(s.data(), s.size());
  const size_t mask = flat_slots_.size() - 1;
  for (size_t i = size_t(h) & mask; flat_slots_[i]; i = (i + 1) & mask) {
    const size_t at = flat_slots_[i] - 1;
    if (at + s.size() < strings.size() && std::memcmp(strings.data() + at, s.data(), s.size()) == 0 && RDR_NUL_OK(strings[at + s.size()])) return flat_slots_[i];
  }
  if (PoolSeeded()) {   // S102 T11b: not one of this frame's own strings: is it one of the pool's (the seeded prefix)? The pool's field is the answer, as the copied index would have given
    const uint32_t f = seed_.pool->Find(s, h);
    if (f && size_t(f) - 1 + s.size() < seed_.bytes) return f;
  }
  if ((flat_count_ + 1) * 2 > flat_slots_.size()) FlatGrow();
  const uint32_t field = uint32_t(strings.size() + 1);
  strings.append(s.data(), s.size());
  strings.push_back('\0');
  FlatInsert(field, h);
  indexed_size_ = strings.size();
  return field;
}

uint32_t SceneTables::AddString(std::string_view s) {
  if (s.empty()) return 0;
  if (flat_) return FlatAdd(s);
  // Equal strings share a field (the first one's): a hash index over the NUL-separated blob, rebuilt when the blob was not changed by us.
  if (indexed_size_ != strings.size()) {
    string_index_.clear();
    size_t pos = 0;
    while (pos < strings.size()) {
      const size_t end = strings.find('\0', pos);
      const size_t len = (end == std::string::npos ? strings.size() : end) - pos;
      string_index_.emplace(std::string(strings.data() + pos, len), uint32_t(pos + 1));  // (emplace keeps the first of equal strings)
      pos += len + 1;
    }
    indexed_size_ = strings.size();
  }
  if (auto it = string_index_.find(std::string(s)); it != string_index_.end()) return it->second;
  const uint32_t field = uint32_t(strings.size() + 1);
  strings.append(s.data(), s.size());
  strings.push_back('\0');
  string_index_.emplace(std::string(s), field);
  indexed_size_ = strings.size();
  return field;
}

std::string_view SceneTables::Str(uint32_t field) const {
  if (field == 0 || field > strings.size()) return {};
  const size_t pos = field - 1;
  const size_t end = strings.find('\0', pos);
  return std::string_view(strings.data() + pos, (end == std::string::npos ? strings.size() : end) - pos);
}

std::vector<uint8_t> SerializeRecord(const SceneTables& seeded_or_not) {
  if (seeded_or_not.SeedPrefixBytes()) {   // S102 T11b: a table seeded from the string pool is written with only the strings it names
    SceneTables compact = seeded_or_not;
    compact.CompactStrings();
    return SerializeRecord(compact);
  }
  const SceneTables& t = seeded_or_not;
  std::vector<Out> secs;
  if (!t.strings.empty()) secs.push_back({kScStrings, 1, t.strings.size(), t.strings.data()});
  if (t.has_camera) secs.push_back({kScCamera, uint32_t(sizeof(Camera)), 1, &t.camera});
  if (!t.assets.empty()) secs.push_back(Sec(kScAssets, t.assets));
  if (!t.objects.empty()) secs.push_back(Sec(kScObjects, t.objects));
  if (!t.bones.empty()) secs.push_back(Sec(kScBones, t.bones));
  if (!t.params.empty()) secs.push_back(Sec(kScParams, t.params));
  if (!t.lights.empty()) secs.push_back(Sec(kScLights, t.lights));
  if (!t.sprites.empty()) secs.push_back(Sec(kScSprites, t.sprites));
  if (!t.texts.empty()) secs.push_back(Sec(kScTexts, t.texts));
  if (!t.movies.empty()) secs.push_back(Sec(kScMovies, t.movies));
  if (!t.targets.empty()) secs.push_back(Sec(kScTargets, t.targets));
  if (!t.engine_objects.empty()) secs.push_back(Sec(kScEngineObjects, t.engine_objects));
  if (!t.light_states.empty()) secs.push_back(Sec(kScLightStates, t.light_states));
  if (!t.light_refs.empty()) secs.push_back(Sec(kScLightRefs, t.light_refs));
  if (!t.arena_worlds.empty()) secs.push_back(Sec(kScArenaWorlds, t.arena_worlds));  // S101 lane 130
  if (!t.circle_shadows.empty()) secs.push_back(Sec(kScCircleShadows, t.circle_shadows));  // lane 136
  if (!t.circle_mesh.empty()) secs.push_back(Sec(kScCircleMesh, t.circle_mesh));  // lane 136
  if (!t.plane_frames.empty()) secs.push_back(Sec(kScPlaneFrames, t.plane_frames));  // lane 139
  if (!t.plane_draws.empty()) secs.push_back(Sec(kScPlaneDraws, t.plane_draws));  // lane 139
  if (!t.ring_rows.empty()) secs.push_back(Sec(kScRingRows, t.ring_rows));  // lane 194
  if (!t.ring_nodes.empty()) secs.push_back(Sec(kScRingNodes, t.ring_nodes));  // lane 194
  if (!t.caster_calls.empty()) secs.push_back(Sec(kScCasterCalls, t.caster_calls));  // lane 202
  if (!t.caster_states.empty()) secs.push_back(Sec(kScCasterStates, t.caster_states));  // lane 202
  if (!t.caster_nodes.empty()) secs.push_back(Sec(kScCasterNodes, t.caster_nodes));  // lane 217
  if (!t.caster_extras.empty()) secs.push_back(Sec(kScCasterExtras, t.caster_extras));  // lane 221
  if (!t.caster_mats.empty()) secs.push_back(Sec(kScCasterMats, t.caster_mats));  // lane 226
  if (!t.sphere_occ.empty()) secs.push_back(Sec(kScSphereOcc, t.sphere_occ));  // lane 242
  const bool has_runs = !t.crowd_runs.empty();  // S102 T19: only a record with markers is version 2; every other record is written exactly as before
  if (has_runs) {
    secs.push_back(Sec(kScCrowdRuns, t.crowd_runs));
    secs.push_back(Sec(kScCrowdMembers, t.crowd_members));
  }

  SceneHeader h = t.header;
  h.magic = kSceneMagic;
  h.version = has_runs ? kSceneVersionRuns : kSceneVersion;
  h.section_count = uint32_t(secs.size());
  std::memset(h.pad, 0, sizeof h.pad);

  uint64_t at = AlignUp(sizeof(SceneHeader) + secs.size() * sizeof(SectionEntry));
  std::vector<SectionEntry> dir(secs.size());
  for (size_t i = 0; i < secs.size(); ++i) {
    dir[i].id = secs[i].id, dir[i].element_size = secs[i].size, dir[i].count = secs[i].count, dir[i].offset = at;
    at = AlignUp(at + secs[i].count * secs[i].size);
  }
  std::vector<uint8_t> bytes(size_t(at), 0);
  std::memcpy(bytes.data(), &h, sizeof h);
  if (!dir.empty()) std::memcpy(bytes.data() + sizeof h, dir.data(), dir.size() * sizeof(SectionEntry));
  for (size_t i = 0; i < secs.size(); ++i)
    if (secs[i].count) std::memcpy(bytes.data() + dir[i].offset, secs[i].data, size_t(secs[i].count * secs[i].size));
  return bytes;
}

bool ParseRecord(const uint8_t* data, size_t size, SceneTables& out, std::string& error) {
  out = SceneTables();
  if (size < sizeof(SceneHeader)) {
    error = Fmt("truncated: %zu bytes, the header alone is %zu", size, sizeof(SceneHeader));
    return false;
  }
  SceneHeader h;
  std::memcpy(&h, data, sizeof h);
  if (h.magic != kSceneMagic) {
    error = Fmt("not a scene record (magic 0x%08X, expected 0x%08X \"RSCN\")", h.magic, kSceneMagic);
    return false;
  }
#ifdef RDR_CROWD_RUNS_PLANTS
  if ((h.version != kSceneVersion || g_run_plant == 3) && h.version != kSceneVersionRuns) {   // planted twin 3: the old format is no longer read
#else
  if (h.version != kSceneVersion && h.version != kSceneVersionRuns) {   // S102 T19: version 2 = a record with crowd run markers; version 1 is read as before
#endif
    error = Fmt("record version %u, this reader reads versions %u and %u", h.version, kSceneVersion, kSceneVersionRuns);
    return false;
  }
  if (h.section_count > kMaxSections) {
    error = Fmt("section_count %u is not plausible (limit %u)", h.section_count, kMaxSections);
    return false;
  }
  const uint64_t dir_end = sizeof(SceneHeader) + uint64_t(h.section_count) * sizeof(SectionEntry);
  if (dir_end > size) {
    error = Fmt("truncated: the section directory (%u entries) runs past the file's %zu bytes", h.section_count, size);
    return false;
  }
  out.header = h;
  for (uint32_t i = 0; i < h.section_count; ++i) {
    SectionEntry e;
    std::memcpy(&e, data + sizeof(SceneHeader) + size_t(i) * sizeof(SectionEntry), sizeof e);
    bool ok = true;
    switch (e.id) {
      case kScStrings: {
        if (e.element_size != 1) { error = Fmt("section strings: element_size %u, expected 1", e.element_size); return false; }
        if (e.offset > size || e.count > size - e.offset) {
          error = Fmt("section strings: truncated (%llu bytes at %llu, the file has %zu)", (unsigned long long)e.count, (unsigned long long)e.offset, size);
          return false;
        }
        out.strings.assign(reinterpret_cast<const char*>(data) + e.offset, size_t(e.count));
        break;
      }
      case kScCamera: {
        std::vector<Camera> c;
        ok = Load(size, data, e, c, error);
        if (ok && c.size() != 1) { error = Fmt("section Camera: count %zu, expected 1", c.size()); return false; }
        if (ok) out.camera = c[0], out.has_camera = true;
        break;
      }
      case kScAssets: ok = Load(size, data, e, out.assets, error); break;
      case kScObjects: ok = Load(size, data, e, out.objects, error); break;
      case kScBones: ok = Load(size, data, e, out.bones, error); break;
      case kScParams: ok = Load(size, data, e, out.params, error); break;
      case kScLights: ok = Load(size, data, e, out.lights, error); break;
      case kScSprites: ok = Load(size, data, e, out.sprites, error); break;
      case kScTexts: ok = Load(size, data, e, out.texts, error); break;
      case kScMovies: ok = Load(size, data, e, out.movies, error); break;
      case kScTargets: ok = Load(size, data, e, out.targets, error); break;
      case kScEngineObjects: ok = Load(size, data, e, out.engine_objects, error); break;
      case kScLightStates: ok = Load(size, data, e, out.light_states, error); break;
      case kScLightRefs: ok = Load(size, data, e, out.light_refs, error); break;
      case kScCircleShadows: {  // lane 136: a bad circle section drops the circle tables only (counted), never the whole record (as section 15)
        std::string werr;
        if (!Load(size, data, e, out.circle_shadows, werr)) out.circle_shadows.clear(), ++out.circle_errors;
        ok = true;
        break;
      }
      case kScCircleMesh: {
        std::string werr;
        if (!Load(size, data, e, out.circle_mesh, werr)) out.circle_mesh.clear(), ++out.circle_errors;
        ok = true;
        break;
      }
      case kScPlaneFrames: {  // lane 139: a bad plane section drops the plane tables only (counted), never the whole record
        std::string werr;
        if (!Load(size, data, e, out.plane_frames, werr)) out.plane_frames.clear(), ++out.plane_errors;
        ok = true;
        break;
      }
      case kScPlaneDraws: {
        std::string werr;
        if (!Load(size, data, e, out.plane_draws, werr)) out.plane_draws.clear(), ++out.plane_errors;
        ok = true;
        break;
      }
      case kScRingRows: {  // lane 194: a bad ring section drops the ring tables only (counted), never the whole record
        std::string werr;
        if (!Load(size, data, e, out.ring_rows, werr)) out.ring_rows.clear(), ++out.ring_errors;
        ok = true;
        break;
      }
      case kScRingNodes: {
        std::string werr;
        if (!Load(size, data, e, out.ring_nodes, werr)) out.ring_nodes.clear(), ++out.ring_errors;
        ok = true;
        break;
      }
      case kScCasterCalls: {  // lane 202: a bad caster section drops the caster tables only (counted), never the whole record
        std::string werr;
        if (!Load(size, data, e, out.caster_calls, werr)) out.caster_calls.clear(), ++out.caster_errors;
        ok = true;
        break;
      }
      case kScCasterStates: {
        std::string werr;
        if (!Load(size, data, e, out.caster_states, werr)) out.caster_states.clear(), ++out.caster_errors;
        ok = true;
        break;
      }
      case kScCasterNodes: {  // lane 217
        std::string werr;
        if (!Load(size, data, e, out.caster_nodes, werr)) out.caster_nodes.clear(), ++out.caster_errors;
        ok = true;
        break;
      }
      case kScCasterMats: {  // lane 226
        std::string werr;
        if (!Load(size, data, e, out.caster_mats, werr)) out.caster_mats.clear(), ++out.caster_errors;
        ok = true;
        break;
      }
      case kScSphereOcc: {  // lane 242: a layout / count this reader does not know drops THIS section only (counted), never the record
        std::string werr;
        if (!Load(size, data, e, out.sphere_occ, werr)) out.sphere_occ.clear(), ++out.sphere_errors;
        else if (const std::string sp = SphereTablesProblem(out); !sp.empty()) out.sphere_occ.clear(), ++out.sphere_errors;
        ok = true;
        break;
      }
      case kScCasterExtras: {  // lane 221
        std::string werr;
        if (!Load(size, data, e, out.caster_extras, werr)) out.caster_extras.clear(), ++out.caster_errors;
        ok = true;
        break;
      }
      case kScCrowdRuns: if (h.version >= kSceneVersionRuns) ok = Load(size, data, e, out.crowd_runs, error); break;   // S102 T19 (a version 1 record cannot carry them)
      case kScCrowdMembers: if (h.version >= kSceneVersionRuns) ok = Load(size, data, e, out.crowd_members, error); break;
      case kScArenaWorlds: {  // S101 lane 130: a layout / kind this reader does not know drops THIS section only (counted), never the whole record
        std::string werr;
        std::vector<ArenaWorld> tmp;
        bool good = Load(size, data, e, tmp, werr);
        for (const ArenaWorld& w : tmp) good = good && w.layout == 1 && (w.kind == kAwObject || w.kind == kAwClock);
        if (good) out.arena_worlds = std::move(tmp);
        else ++out.arena_world_errors;
        ok = true;
        break;
      }
      default: break;  // a section this reader does not know: skipped (a newer writer's addition)
    }
    if (!ok) return false;
  }
  if (!out.engine_objects.empty() && out.engine_objects.size() != out.objects.size()) {
    error = Fmt("section engine objects: %zu entries for %zu objects (must be parallel)", out.engine_objects.size(), out.objects.size());
    return false;
  }
  for (size_t i = 0; i < out.light_refs.size(); ++i)
    if (out.light_refs[i].state >= out.light_states.size()) {
      error = Fmt("LightRef %zu: state %u, but there are %zu light states", i, out.light_refs[i].state, out.light_states.size());
      return false;
    }
  if (!out.crowd_runs.empty() || !out.crowd_members.empty()) {   // S102 T19: the markers become the plain member list here, so every reader of a file sees what it always saw
    if (const std::string rp0 = CrowdRunsProblem(out); !rp0.empty()) { error = rp0; return false; }
    SceneTables plain = ExpandCrowdRuns(out);
    out = std::move(plain);
  }
  if (const std::string cp = CircleTablesProblem(out); !cp.empty()) out.circle_shadows.clear(), out.circle_mesh.clear(), ++out.circle_errors;  // lane 136: only the circle tables go
  if (const std::string pp = PlaneTablesProblem(out); !pp.empty()) out.plane_frames.clear(), out.plane_draws.clear(), ++out.plane_errors;  // lane 139: only the plane tables go
  if (const std::string rp = RingTablesProblem(out); !rp.empty()) out.ring_rows.clear(), out.ring_nodes.clear(), ++out.ring_errors;  // lane 194: only the ring tables go
  if (const std::string cp2 = CasterTablesProblem(out); !cp2.empty()) out.caster_calls.clear(), out.caster_states.clear(), out.caster_nodes.clear(), out.caster_extras.clear(), out.caster_mats.clear(), ++out.caster_errors;  // lane 202: only the caster tables go
  // The indices must point inside the tables (a record that draws from a bad index would draw garbage, silently).
  const auto bad_str = [&](uint32_t f) { return f > out.strings.size(); };
  for (size_t i = 0; i < out.assets.size(); ++i)
    if (bad_str(out.assets[i].pac) || bad_str(out.assets[i].entry)) { error = Fmt("AssetRef %zu: a string field points outside the strings section", i); return false; }
  for (size_t i = 0; i < out.objects.size(); ++i) {
    const Object& o = out.objects[i];
    if (o.asset >= out.assets.size()) { error = Fmt("Object %zu: asset %u, but there are %zu assets", i, o.asset, out.assets.size()); return false; }
    if (uint64_t(o.bones_first) + o.bones_count > out.bones.size()) { error = Fmt("Object %zu: bones %u+%u past the %zu palette matrices", i, o.bones_first, o.bones_count, out.bones.size()); return false; }
    if (uint64_t(o.params_first) + o.params_count > out.params.size()) { error = Fmt("Object %zu: params %u+%u past the %zu values", i, o.params_first, o.params_count, out.params.size()); return false; }
    if (bad_str(o.id)) { error = Fmt("Object %zu: id string points outside the strings section", i); return false; }
  }
  for (size_t i = 0; i < out.params.size(); ++i)
    if (bad_str(out.params[i].name)) { error = Fmt("ParamValue %zu: name string points outside the strings section", i); return false; }
  return true;
}

bool WriteRecord(const std::string& path, const SceneTables& t, std::string& error) {
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  // S101 lane 26 review: written beside, renamed on success, removed on failure: a failed write never leaves a partial .rsc under the real name.
  const std::filesystem::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot create " + tmp.string(); return false; }
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    f.close();
    if (!f) { error = "write failed: " + path; std::filesystem::remove(tmp, ec); return false; }
  }
  std::error_code ec2;
  std::filesystem::rename(tmp, p, ec);
  if (ec) {

    std::filesystem::remove(p, ec2);  // rename onto an existing file can fail on some systems: replace it
    std::filesystem::rename(tmp, p, ec);
  }
  if (ec) { error = "cannot rename into place: " + path + ": " + ec.message(); std::filesystem::remove(tmp, ec); return false; }
  return true;
}

bool ReadRecord(const std::string& path, SceneTables& out, std::string& error) {
  std::ifstream f(std::filesystem::path(path), std::ios::binary | std::ios::ate);
  if (!f) { error = "cannot open " + path; return false; }
  const std::streamoff n = f.tellg();
  if (n < 0) { error = "cannot size " + path; return false; }
  std::vector<uint8_t> bytes(static_cast<size_t>(n));
  f.seekg(0);
  if (n) f.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!f) { error = "read failed: " + path; return false; }
  if (!ParseRecord(bytes.data(), bytes.size(), out, error)) { error = path + ": " + error; return false; }
  if (g_crowd_run_roundtrip) {   // S102 T19 (fview --crowd-run-markers): the record goes through the marker form and back, so a picture proves the expansion
    if (CompactCrowdRuns(out)) {
      const std::vector<uint8_t> again = SerializeRecord(out);
      SceneTables back;
      if (!ParseRecord(again.data(), again.size(), back, error)) { error = path + " (after the crowd run markers): " + error; return false; }
      out = std::move(back);
    }
  }
  return true;
}

// ---- S102 T19: the crowd run markers -----------------------------------------------------------------------------------------------------------
std::string CrowdRunsProblem(const SceneTables& t) {
  uint64_t next = 0;
  uint32_t last_at = 0;
  for (size_t i = 0; i < t.crowd_runs.size(); ++i) {
    const CrowdRun& r = t.crowd_runs[i];
    if (r.count == 0) return Fmt("CrowdRun %zu: no members", i);
    if (r.kind > kRunCard) return Fmt("CrowdRun %zu: kind %u", i, unsigned(r.kind));
    if (r.first_member != next) return Fmt("CrowdRun %zu: first member %u, expected %llu (runs must index the members in order)", i, r.first_member, (unsigned long long)next);
    next += r.count;
#ifdef RDR_CROWD_RUNS_PLANTS
    if (g_run_plant == 5) { if (r.kind > kRunCard) return Fmt("CrowdRun %zu: kind %u", i, unsigned(r.kind)); continue; }   // planted 5: the count / bounds checks are gone (only the kind is still refused)
#endif
    if (next > t.crowd_members.size()) return Fmt("CrowdRun %zu: members up to %llu, but there are %zu", i, (unsigned long long)next, t.crowd_members.size());
    if (r.asset >= t.assets.size()) return Fmt("CrowdRun %zu: asset %u, but there are %zu assets", i, r.asset, t.assets.size());
    if (r.skinned && uint64_t(r.bones_first) + r.bones_count > t.bones.size()) return Fmt("CrowdRun %zu: bones %u+%u past the %zu palette matrices", i, r.bones_first, r.bones_count, t.bones.size());
    if (r.at < last_at || r.at > t.objects.size()) return Fmt("CrowdRun %zu: insertion index %u (previous %u, %zu objects)", i, r.at, last_at, t.objects.size());
    last_at = r.at;
  }
  if (next != t.crowd_members.size()) return Fmt("CrowdMember: %zu entries, the runs cover %llu", t.crowd_members.size(), (unsigned long long)next);
  return {};
}

static Mat4 WorldOfConstants(const float* c) {   // = rdr_scene::live::crowd::WorldFromConstants (the contributor's own; the proof compares the two)
  Mat4 m{};
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) m.m[k * 4 + r] = c[r * 4 + k];
  m.m[15] = 1.f;
  return m;
}

// Everything but the member tables: assigned field by field into `o` (vector::assign keeps o's capacity, so a reused table allocates nothing; a whole-table copy copied the object
// tables that were cleared at once, and the string index map). tools\gate\t19_lint.py: this names every SceneTables member.
static void CopyOtherTables(const SceneTables& t, SceneTables& o) {
  o.header = t.header;
  o.has_camera = t.has_camera;
  o.camera = t.camera;
  o.strings.assign(t.strings);
  o.assets.assign(t.assets.begin(), t.assets.end());
  o.bones.assign(t.bones.begin(), t.bones.end());
  o.params.assign(t.params.begin(), t.params.end());
  o.lights.assign(t.lights.begin(), t.lights.end());
  o.sprites.assign(t.sprites.begin(), t.sprites.end());
  o.texts.assign(t.texts.begin(), t.texts.end());
  o.movies.assign(t.movies.begin(), t.movies.end());
  o.targets.assign(t.targets.begin(), t.targets.end());
  o.light_states.assign(t.light_states.begin(), t.light_states.end());
  o.light_refs.assign(t.light_refs.begin(), t.light_refs.end());
  o.plane_errors = t.plane_errors;
  o.circle_errors = t.circle_errors;
  o.circle_shadows.assign(t.circle_shadows.begin(), t.circle_shadows.end());
  o.circle_mesh.assign(t.circle_mesh.begin(), t.circle_mesh.end());
  o.plane_frames.assign(t.plane_frames.begin(), t.plane_frames.end());
  o.plane_draws.assign(t.plane_draws.begin(), t.plane_draws.end());
  o.arena_world_errors = t.arena_world_errors;
  o.arena_worlds.assign(t.arena_worlds.begin(), t.arena_worlds.end());
  o.ring_errors = t.ring_errors;
  o.ring_rows.assign(t.ring_rows.begin(), t.ring_rows.end());
  o.ring_nodes.assign(t.ring_nodes.begin(), t.ring_nodes.end());
  o.caster_errors = t.caster_errors;
  o.caster_calls.assign(t.caster_calls.begin(), t.caster_calls.end());
  o.caster_nodes.assign(t.caster_nodes.begin(), t.caster_nodes.end());
  o.caster_mats.assign(t.caster_mats.begin(), t.caster_mats.end());
  o.caster_states.assign(t.caster_states.begin(), t.caster_states.end());
  o.caster_extras.assign(t.caster_extras.begin(), t.caster_extras.end());
  o.sphere_errors = t.sphere_errors;
  o.sphere_occ.assign(t.sphere_occ.begin(), t.sphere_occ.end());
  o.crowd_runs.clear();
  o.crowd_members.clear();
}

// The expansion into a table the caller owns (a reused one keeps its capacity). `o` ends equal to ExpandCrowdRuns(t) (its string index is the flat one).
static void ExpandInto(const SceneTables& t, SceneTables& o) {
  o.arena_memo.v.store(-2, std::memory_order_relaxed);   // S102 RT1 review: `o` is refilled in place (thread-local reuse): its memo belongs to the last record
  CopyOtherTables(t, o);
  // S102 T11b (R20 review F1): a seeded source's blob starts with the pool. The expansion keeps only the strings its fields name (no file carries the pool, no per-frame re-hash of it in the render thread's
  // reused table). The plain objects' ids are remapped with the rest (they are copied into `o` and compacted together), then taken from there.
  bool seeded_src = t.SeedPrefixBytes() != 0;
#ifdef RDR_CROWD_IDS_PLANTS
  if (g_expand_keeps_prefix) seeded_src = false;
#endif
  thread_local std::vector<Object> remapped;   // (the render thread's: kept capacity)
  if (seeded_src) {
    o.objects.assign(t.objects.begin(), t.objects.end());
    o.CompactStrings();
    remapped.swap(o.objects);
  }
  const std::vector<Object>& tobjs = seeded_src ? remapped : t.objects;
  const bool bad = t.crowd_runs.empty() || !CrowdRunsProblem(t).empty();   // (a table with bad markers expands to nothing: callers check CrowdRunsProblem first)
  if (bad) {
    o.objects.assign(tobjs.begin(), tobjs.end());
    o.engine_objects.assign(t.engine_objects.begin(), t.engine_objects.end());
    return;
  }
  uint64_t total = 0;
  for (const CrowdRun& r : t.crowd_runs) total += r.count;
  o.objects.clear();
  o.objects.reserve(tobjs.size() + size_t(total));
  const bool eng = !t.engine_objects.empty();
  o.engine_objects.clear();
  if (eng) o.engine_objects.reserve(t.engine_objects.size() + size_t(total));
  o.UseFlatStringIndex(size_t(total) + 64, size_t(total) * 28 + 1024);
  size_t src = 0;
  uint32_t unknown_ordinal = 0;
  static const char* const kKinds[3] = {"block", "floor", "card"};
  auto copy_to = [&](size_t upto) {   // the plain objects before the next run (and their engine objects)
    o.objects.insert(o.objects.end(), tobjs.begin() + long(src), tobjs.begin() + long(upto));
#ifdef RDR_CROWD_RUNS_PLANTS
    if (g_run_plant == 7) { src = upto; return; }   // planted: the others' engine objects are put at the end, the members' zeros in step (not at `at`)
#endif
    if (eng)
      for (size_t i = src; i < upto; ++i) o.engine_objects.push_back(i < t.engine_objects.size() ? t.engine_objects[i] : 0u);
    src = upto;
  };
  for (const CrowdRun& r : t.crowd_runs) {
    copy_to(r.at);
#ifdef RDR_CROWD_RUNS_PLANTS
    if (g_run_plant == 6) unknown_ordinal = 0;   // planted: the index-less ordinal restarts in every run
#endif
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "crowd:%s:%u:%s", kKinds[r.kind], unsigned(r.block), r.kind == kRunCard ? "" : "member:");
    const size_t prefix = size_t(n);
    uint32_t count = r.count;
#ifdef RDR_CROWD_RUNS_PLANTS
    if (g_run_plant == 2 && count > 1) --count;   // planted: the run's last member is dropped
#endif
    Object ob;
    ob.asset = r.asset;
    ob.klass = rdr_adapter::DrawClass::Crowd;
    ob.layer = r.layer;
    ob.flags = kObjVisible;
    if (r.skinned) ob.flags |= kObjSkinned, ob.bones_first = r.bones_first, ob.bones_count = r.bones_count;
    for (uint32_t i = 0; i < count; ++i) {
      const CrowdMember& m = t.crowd_members[size_t(r.first_member) + i];
      ob.world = WorldOfConstants(m.c);
      char* at = buf + prefix;
      uint32_t v = m.index;
      if (m.index == kCrowdNoIndex) *at++ = 'u', v = unknown_ordinal++;
#ifdef RDR_CROWD_RUNS_PLANTS
      else if (g_run_plant == 4) v = m.index + 1;   // planted: ids rebuilt off by one
#endif
      char digits[12];
      int nd = 0;
      do { digits[nd++] = char('0' + v % 10); v /= 10; } while (v);
      while (nd) *at++ = digits[--nd];
      ob.id = o.AddString(std::string_view(buf, size_t(at - buf)));
      o.objects.push_back(ob);
#ifdef RDR_CROWD_RUNS_PLANTS
      if (g_run_plant == 7) continue;
#endif
      if (eng) o.engine_objects.push_back(0u);
    }
  }
  copy_to(tobjs.size());
#ifdef RDR_CROWD_RUNS_PLANTS
  if (g_run_plant == 7 && eng)   // planted: the others' engine objects at the end, in their own order
    for (size_t i = 0; i < tobjs.size(); ++i) o.engine_objects.push_back(i < t.engine_objects.size() ? t.engine_objects[i] : 0u);
#endif
}

SceneTables ExpandCrowdRuns(const SceneTables& t) {
  SceneTables o;
  ExpandInto(t, o);
  return o;
}

// The render thread's pool: three tables, each reused once nothing else holds it (use count 1) and keyed by the record it expanded (a weak reference: an expired record is never
// matched, and its control block cannot be reused while the key lives). The present's record comes back as the next message's record: it is found, not expanded twice.
std::shared_ptr<const SceneTables> ExpandedForReading(const std::shared_ptr<const SceneTables>& t) {
  if (!t || t->crowd_runs.empty()) return t;
  struct Slot { std::weak_ptr<const SceneTables> key; std::shared_ptr<SceneTables> out; };
  thread_local Slot slots[3];
  thread_local unsigned next = 0;
  for (Slot& s : slots)
    if (s.out && !s.key.owner_before(t) && !t.owner_before(s.key)) return s.out;
  Slot* use = nullptr;
  for (unsigned k = 0; k < 3 && !use; ++k) {
    Slot& s = slots[(next + k) % 3];
    if (!s.out || s.out.use_count() == 1) use = &s;
  }
  if (!use) {   // all three still held by messages in flight: a fresh table replaces the oldest slot (its holders keep theirs)
    use = &slots[next % 3];
    use->out.reset();
  }
  if (!use->out) use->out = std::make_shared<SceneTables>();
  next = unsigned(use - slots) + 1;
  ExpandInto(*t, *use->out);
  use->key = t;
  return use->out;
}

// "crowd:<kind>:<block>:[member:][u]<number>" -> its parts; false for any other string.
static bool ParseCrowdId(std::string_view s, uint8_t& kind, uint32_t& block, bool& unknown, uint32_t& number) {
  auto eat = [&](std::string_view p) { if (s.substr(0, p.size()) != p) return false; s.remove_prefix(p.size()); return true; };
  auto num = [&](uint32_t& v) {
    size_t i = 0;
    uint64_t x = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i < 10) x = x * 10 + uint64_t(s[i++] - '0');
    if (!i || x > 0xFFFFFFFEull || (i > 1 && s[0] == '0')) return false;   // (a leading zero would not round trip through "%u")
    v = uint32_t(x);
    s.remove_prefix(i);
    return true;
  };
  if (!eat("crowd:")) return false;
  if (eat("block:")) kind = kRunBlock;
  else if (eat("floor:")) kind = kRunFloor;
  else if (eat("card:")) kind = kRunCard;
  else return false;
  if (!num(block) || !eat(":")) return false;
  if (kind != kRunCard && !eat("member:")) return false;
  unknown = eat("u");
  return num(number) && s.empty();
}

// S102 T19 F6: the blob without the strings no field names (what the compacted members' ids were), the fields remapped: the layout the live marker path writes. Every table with a
// string field is named here (objects' id, assets' pac / entry, params' name, sprites' widget, texts' utf8 / widget, targets' name); a field that is not the start of a string
// leaves the blob alone.
static void DropUnreferencedStrings(SceneTables& t, bool compacted) {
  if (!compacted || t.strings.empty()) return;
  const size_t n = t.strings.size();
  std::vector<uint32_t> fields;
  auto note = [&](uint32_t f) { if (f) fields.push_back(f); };
  for (const AssetRef& a : t.assets) note(a.pac), note(a.entry);
  for (const Object& o : t.objects) note(o.id);
  for (const ParamValue& p : t.params) note(p.name);
  for (const Sprite& sp : t.sprites) note(sp.widget);
  for (const Text& tx : t.texts) note(tx.utf8), note(tx.widget);
  for (const Target& tg : t.targets) note(tg.name);
  std::vector<uint8_t> starts(n + 1, 0), keep(n + 1, 0);   // a segment starts at 0 or after a NUL
  for (size_t i = 0; i < n; ++i) starts[i] = (i == 0 || t.strings[i - 1] == '\0');
  for (uint32_t f : fields) {
    if (f > n || !starts[f - 1]) return;   // (a field that is not a string start: leave the blob as it is)
    keep[f - 1] = 1;
  }
  std::string out;
  out.reserve(n);
  std::vector<uint32_t> remap(n + 1, 0);
  for (size_t pos = 0; pos < n;) {
    const char* base = t.strings.data();
    const void* nul = std::memchr(base + pos, 0, n - pos);
    const size_t len = nul ? size_t(static_cast<const char*>(nul) - (base + pos)) : n - pos;
    if (keep[pos]) {
      remap[pos] = uint32_t(out.size() + 1);
      out.append(base + pos, len + (nul ? 1 : 0));
    }
    pos += len + 1;
  }
  auto fix = [&](uint32_t& f) { if (f) f = remap[f - 1]; };
  for (AssetRef& a : t.assets) fix(a.pac), fix(a.entry);
  for (Object& o : t.objects) fix(o.id);
  for (ParamValue& p : t.params) fix(p.name);
  for (Sprite& sp : t.sprites) fix(sp.widget);
  for (Text& tx : t.texts) fix(tx.utf8), fix(tx.widget);
  for (Target& tg : t.targets) fix(tg.name);
  t.strings = std::move(out);
  t.UseFlatStringIndex(0, 0);   // (its index is rebuilt from the new blob on the next AddString)
}

uint32_t CompactCrowdRuns(SceneTables& t) {
  if (!t.crowd_runs.empty() || t.objects.empty()) return 0;
  const bool eng = !t.engine_objects.empty() && t.engine_objects.size() == t.objects.size();
  if (!t.engine_objects.empty() && !eng) return 0;
  const Object def{};
  std::vector<Object> kept;
  std::vector<uint32_t> kept_eng;
  kept.reserve(t.objects.size());
  std::vector<CrowdRun> runs;
  std::vector<CrowdMember> members;
  uint32_t unknown_ordinal = 0;
  for (size_t i = 0; i < t.objects.size(); ++i) {
    const Object& o = t.objects[i];
    uint8_t kind = 0;
    uint32_t block = 0, number = 0;
    bool unknown = false;
    bool ok = o.klass == rdr_adapter::DrawClass::Crowd && (o.flags == kObjVisible || o.flags == (kObjVisible | kObjSkinned)) && o.pad == 0 && o.light_set == def.light_set &&
              o.params_first == 0 && o.params_count == 0 && o.mesh_mask_lo == def.mesh_mask_lo && o.mesh_mask_hi == def.mesh_mask_hi && (!eng || t.engine_objects[i] == 0);
    for (int k = 0; ok && k < 4; ++k) ok = o.tint[k] == 1.f;
    ok = ok && o.world.m[3] == 0.f && o.world.m[7] == 0.f && o.world.m[11] == 0.f && o.world.m[15] == 1.f;
    ok = ok && ParseCrowdId(t.Str(o.id), kind, block, unknown, number) && block <= 255 && !(unknown && number != unknown_ordinal);
    const bool skinned = (o.flags & kObjSkinned) != 0;
    ok = ok && (skinned || (o.bones_first == 0 && o.bones_count == 0));
    if (!ok) {
      kept.push_back(o);
      if (eng) kept_eng.push_back(t.engine_objects[i]);
      continue;
    }
    if (unknown) ++unknown_ordinal;
    if (runs.empty() || runs.back().at != kept.size() || runs.back().kind != kind || runs.back().block != block || runs.back().layer != o.layer || runs.back().asset != o.asset ||
        runs.back().skinned != (skinned ? 1 : 0) || runs.back().bones_first != o.bones_first || runs.back().bones_count != o.bones_count) {
      CrowdRun r;
      r.at = uint32_t(kept.size()), r.first_member = uint32_t(members.size()), r.count = 0, r.asset = o.asset;
      r.bones_first = o.bones_first, r.bones_count = o.bones_count;
      r.kind = kind, r.block = uint8_t(block), r.layer = o.layer, r.skinned = skinned ? 1 : 0;
      runs.push_back(r);
    }
    ++runs.back().count;
    CrowdMember m;
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 4; ++k) m.c[r * 4 + k] = o.world.m[k * 4 + r];
    m.index = unknown ? kCrowdNoIndex : number;
    members.push_back(m);
  }
  if (runs.empty()) return 0;
  t.objects = std::move(kept);
  if (eng) t.engine_objects = std::move(kept_eng);
  t.crowd_runs = std::move(runs);
  t.crowd_members = std::move(members);
  DropUnreferencedStrings(t, true);   // (F6: the live marker record never had the members' id strings)
  return uint32_t(t.crowd_runs.size());
}

}  // namespace rdr_scene
