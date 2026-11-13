// The scene record as a file (Session 101, lane 14). See record_io.h and docs\renderer\SCENE_RECORD.md "Files".
#include "record_io.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <type_traits>

namespace rdr_scene {
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
    default: return "unknown";
  }
}

SceneTables::SceneTables() {
  camera.view = IdentityMat4();
  camera.proj = IdentityMat4();
}

uint32_t SceneTables::AddString(std::string_view s) {
  if (s.empty()) return 0;
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

std::vector<uint8_t> SerializeRecord(const SceneTables& t) {
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

  SceneHeader h = t.header;
  h.magic = kSceneMagic;
  h.version = kSceneVersion;
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
  if (h.version != kSceneVersion) {
    error = Fmt("record version %u, this reader reads version %u", h.version, kSceneVersion);
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
  return true;
}

}  // namespace rdr_scene
