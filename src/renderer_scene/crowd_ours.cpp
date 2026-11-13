// The crowd drawn by us inside the game's frame. See crowd_ours.h and docs\renderer\CROWD_PROMOTION.md.
#include "crowd_ours.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "../assets/crowd_body.h"
#include "../scene_live/crowd_skip_core.h"  // S101 lane 50: SkippingNow (no game or SDK includes)
#include "../scene_live/member_thin_core.h"  // lane 272: the members thin record (CheckTally; no game or SDK includes)
#include "../renderer/problem.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "crowd_fast.h"  // lane 270
#include "fx_tables.h"

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;

constexpr uint32_t kTypes = 20;
constexpr uint32_t kClassCount = kCrowdClassCount;
constexpr uint32_t kMaxMeshes = 4;
constexpr uint32_t kCardLod = 3;
constexpr float kBodyScale = 10.0f;  // S101 lane 36: blob position = stored vertex x 10 (file space) x palette

// The effect table of a variant by name (the field names and offsets of its vertex block, its input names), cached.
const fxt::VariantTable* TableOf(int variant) {
  static std::vector<const fxt::VariantTable*> by_index;
  if (by_index.empty()) {
    by_index.assign(std::size(kVariants), nullptr);
    for (size_t i = 0; i < std::size(kVariants); ++i) {
      if (!kVariants[i].name) continue;
      for (const fxt::VariantTable& t : fxt::kTables)
        if (std::strcmp(t.variant, kVariants[i].name) == 0) {
          by_index[i] = &t;
          break;
        }
    }
  }
  return variant >= 0 && size_t(variant) < by_index.size() ? by_index[size_t(variant)] : nullptr;
}

const fxt::Field* FieldOf(const fxt::VariantTable* t, const char* name) {
  if (!t) return nullptr;
  for (uint32_t k = 0; k < t->field_count; ++k)
    if (std::strcmp(t->fields[k].name, name) == 0) return &t->fields[k];
  return nullptr;
}

size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

struct AssetInfo {
  bool valid = false, card = false;
  uint32_t type = 0, lod = 0;
  int arena = -1;
};

AssetInfo ParseAssetRef(const SceneTables& t, const AssetRef& a) {  // the original sscanf reading, kept as the reference of the fast one (lane 270)
  AssetInfo info;
  const std::string_view entry = t.Str(a.entry), pac = t.Str(a.pac);
  int arena = -1;
  const std::string pac_s(pac);
  if (std::sscanf(pac_s.c_str(), "pac\\bg\\bg%d.pac", &arena) == 1) info.arena = arena;
  const std::string e(entry);
  unsigned type = 0, number = 0;
  if (std::sscanf(e.c_str(), "card:%u", &type) == 1 && e.rfind("card:", 0) == 0) {
    info.card = true, info.type = type, info.lod = kCardLod, info.valid = type < kTypes;
  } else if (std::sscanf(e.c_str(), "%u", &number) == 1 && number >= 20000 && number < 20000 + 3 * 40) {
    info.lod = (number - 20000) / 40, info.type = (number - 20000) % 40, info.valid = info.type < kTypes;
  }
  return info;
}

// lane 270: the same reading without the two std::string copies and the four sscanf calls (a record holds hundreds of assets and this ran for every one, every frame).
AssetInfo ParseAsset(const SceneTables& t, const AssetRef& a) {
  AssetInfo info;
  const std::string_view entry = t.Str(a.entry), pac = t.Str(a.pac);
  constexpr std::string_view kPac = "pac\\bg\\bg";
  int arena = -1;
  if (pac.size() > kPac.size() && pac.compare(0, kPac.size(), kPac) == 0 && fast::ParseInt(pac, kPac.size(), arena)) info.arena = arena;
  uint32_t v = 0;
  if (entry.size() > 5 && entry.compare(0, 5, "card:") == 0 && fast::ParseUInt(entry, 5, v)) {
    info.card = true, info.type = v, info.lod = kCardLod, info.valid = v < kTypes;
  } else if (fast::ParseUInt(entry, 0, v) && v >= 20000 && v < 20000 + 3 * 40) {
    info.lod = (v - 20000) / 40, info.type = (v - 20000) % 40, info.valid = info.type < kTypes;
  }
  return info;
}

// S101 lane 82: a seated floor person is a record object named "crowd:floor:TT:member:I" (live: crowd_hooks.h / crowd_floor.h; fview: the floor synth).
bool IsFloorObject(const SceneTables& t, const Object& o) {
  const std::string_view id = t.Str(o.id);
  return id.size() > 12 && id.compare(0, 12, "crowd:floor:") == 0;
}

}  // namespace

CrowdOurs::CrowdOurs(rdr::Renderer& renderer, const CrowdOursOptions& options) : renderer_(renderer), options_(options), root_(options.game_root) {
  rdr::ImageDesc d;
  d.width = d.height = 1;
  d.format = rdr::Format::RGBA8;
  d.usage = rdr::kUsageSampled;
  white_ = renderer_.CreateImage(d);
  const uint8_t px[4] = {255, 255, 255, 255};
  renderer_.UpdateImage(white_, 0, 0, px, 4);
  ph_.Init(kPhaseNames, kPhaseCount), cph_.Init(kCardPhaseNames, kCardPhaseCount);  // lane 270 (env RDR_CROWD_PHASES=1)
  if (const char* e = std::getenv("RDR_CROWD_PARSE_CHECK")) parse_check_ = e[0] == '1';
  if (const char* e = std::getenv("RDR_CROWD_POSE_CHECK")) pose_check_ = e[0] == '1';
  if (const char* e = std::getenv("RDR_CROWD_NO_CACHE")) options_.caches = !(e[0] == '1');  // lane 270 (A/B: the same frames without the cross-frame / in-frame caches)
}

CrowdOurs::~CrowdOurs() {
  if (worker_.joinable()) worker_.join();
  renderer_.WaitIdle();
  DropBodies();
  if (white_ != rdr::kNoImage) renderer_.DestroyImage(white_);
}

void CrowdOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  std::lock_guard<std::mutex> lock(mu_);
  record_ = std::move(record);
}

void CrowdOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  root_ = root;
}

void CrowdOurs::DropBodies() {
  for (TypeBody& b : bodies_) {
    for (MeshGpu& m : b.meshes) {
      if (m.index != rdr::kNoBuffer) renderer_.DestroyBuffer(m.index);
    }
    for (rdr::ImageId id : b.images) renderer_.DestroyImage(id);
    b = TypeBody{};
  }
  uploaded_ = false;
}

rdr::ImageId CrowdOurs::MakeTexture(const assets::Texture& t) {
  if (t.width == 0 || t.height == 0 || t.format == assets::TexFormat::kUnknown) return rdr::kNoImage;
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if ((bc && ((t.width % 4) || (t.height % 4))) || t.width > 16384 || t.height > 16384) return rdr::kNoImage;
  uint32_t mips = std::max(1u, t.mips), mx = std::max(t.width, t.height), maxmips = 1;
  while (mx > 1) mx >>= 1, ++maxmips;
  mips = std::min(mips, maxmips);
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < mips; ++i) {
    const size_t sz = MipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (total + sz > t.data.size()) break;
    total += sz, ++fit;
  }
  if (fit == 0) return rdr::kNoImage;
  rdr::ImageDesc d;
  d.width = uint16_t(t.width);
  d.height = uint16_t(t.height);
  d.mips = uint8_t(fit);
  d.usage = rdr::kUsageSampled;
  switch (t.format) {
    case assets::TexFormat::kBC1: d.format = rdr::Format::BC1; break;
    case assets::TexFormat::kBC2: d.format = rdr::Format::BC2; break;
    case assets::TexFormat::kBC3: d.format = rdr::Format::BC3; break;
    case assets::TexFormat::kB8G8R8A8: d.format = rdr::Format::BGRA8; break;
    default: d.format = rdr::Format::RGBA8; break;
  }
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id == rdr::kNoImage) return id;
  size_t src = 0;
  std::vector<uint8_t> wide;
  for (uint32_t i = 0; i < fit; ++i) {
    const uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
    const size_t sb = MipBytes(t.format, mw, mh);
    if (t.format == assets::TexFormat::kB4G4R4A4) {
      wide.resize(size_t(mw) * mh * 4);
      for (size_t k = 0; k < size_t(mw) * mh; ++k) {
        const uint16_t px = uint16_t(t.data[src + 2 * k] | (t.data[src + 2 * k + 1] << 8));
        wide[4 * k] = uint8_t(((px >> 8) & 15) * 17), wide[4 * k + 1] = uint8_t(((px >> 4) & 15) * 17);
        wide[4 * k + 2] = uint8_t((px & 15) * 17), wide[4 * k + 3] = uint8_t(((px >> 12) & 15) * 17);
      }
      renderer_.UpdateImage(id, i, 0, wide.data(), wide.size());
    } else {
      renderer_.UpdateImage(id, i, 0, t.data.data() + src, sb);
    }
    src += sb;
  }
  return id;
}

// ---- loading ---------------------------------------------------------------------------------------------------------------------

// S101 lane 271 (SKIP_GAME_WORK_MAP 19.8): the in-game sequence's match card names the arena ~1.5 s before the first record that draws it. Starting the load then (on its worker,
// as FrameReady would) has the meshes ready when that record arrives instead of 0.1-8 s after it. A load already running or done, a record that already named an arena, or the
// same arena again does nothing; a wrong arena is a wasted load that FrameReady discards (loaded_.arena != arena_) and replaces.
void CrowdOurs::Announce(int arena) {
  if (load_state_.load() == 2 && loaded_.arena != arena) {   // a finished early load for another arena (the player backed out after the card) must not block this one
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (arena < 0 || uploaded_ || arena_ >= 0 || load_state_.load() != 0 || load_for_ == arena) return;
  StartLoad(arena);
}

void CrowdOurs::StartLoad(int arena) {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  if (root.empty()) {
    static bool said = false;
    if (!said) said = true, rdr::Log("[rdr][crowd] no game folder to load the crowd's bodies from: the stream's crowd stays");
    return;
  }
  if (worker_.joinable()) worker_.join();
  load_state_ = 1;
  load_for_ = arena;
  ++stats_.arena_loads;
  worker_ = std::thread([this, arena, root] {
    Loaded loaded;
    loaded.arena = arena;
    if (!assets::crowd_body::Init(root)) {
      loaded.error = "no game files at " + root;
    } else {
      for (uint32_t c = 0; c < kClassCount; ++c) {
        std::string err;
        loaded.ok[c] = assets::crowd_body::LoadBodyLodGameSpace(arena, int(c / kTypes), int(c % kTypes), loaded.models[c], err);
        if (!loaded.ok[c] && loaded.error.empty()) loaded.error = "LOD " + std::to_string(c / kTypes) + " type " + std::to_string(c % kTypes) + ": " + err;
      }
      assets::crowd_body::Release(arena);
    }
    loaded_ = std::move(loaded);
    load_state_ = 2;
  });
}

bool CrowdOurs::Upload(Loaded& loaded) {
  const auto upload_t0 = std::chrono::steady_clock::now();   // lane 271: the upload runs inside one FrameReady on the live thread; its length is logged
  DropBodies();
  uint32_t types = 0;
  for (uint32_t t = 0; t < kClassCount; ++t) {
    if (!loaded.ok[t]) continue;
    const assets::Model& model = loaded.models[t];
    TypeBody& body = bodies_[t];
    std::unordered_map<int, rdr::ImageId> textures;
    for (const assets::Mesh& me : model.meshes) {
      if (body.meshes.size() >= kMaxMeshes) break;
      if (me.vertices.empty() || me.indices.size() < 3) continue;
      bool bad = false;
      for (uint32_t ix : me.indices) bad = bad || ix >= me.vertices.size();
      if (bad) continue;
      MeshGpu m;
      m.vertices.resize(me.vertices.size());
      m.stored_pose.resize(me.vertices.size());
      for (size_t i = 0; i < me.vertices.size(); ++i) {
        const assets::Vertex& v = me.vertices[i];
        BodyVertex& b = m.vertices[i];
        for (int c = 0; c < 3; ++c) b.pos[c] = v.pos[c] * kBodyScale;  // the loader's vertices are 1/10 of the game's skinned blob (lane 36, bit-exact); normals are not scaled
        std::memcpy(b.normal, v.normal, sizeof b.normal);
        std::memcpy(b.uv, v.uv0, sizeof b.uv);
        std::memcpy(b.bones, v.bones, sizeof b.bones);
        std::memcpy(b.weights, v.weights, sizeof b.weights);
      }
      PoseVertices(m.vertices, nullptr, 0, m.stored_pose);
      m.index = renderer_.CreateBuffer(me.indices.size() * 4, me.indices.data());
      m.index_count = uint32_t(me.indices.size());
      const int mat = me.material;
      if (mat >= 0 && size_t(mat) < model.materials.size()) {
        const int tex = model.materials[size_t(mat)].diffuse;
        if (tex >= 0 && size_t(tex) < model.textures.size()) {
          auto it = textures.find(tex);
          if (t >= kTypes && !bodies_[t % kTypes].meshes.empty()) {  // a LOD 1 / 2 body uses the same aud_NNa picture as its type's LOD 0 body: no second upload
            m.diffuse = bodies_[t % kTypes].meshes[0].diffuse;
          } else {
            if (it == textures.end()) {
              it = textures.emplace(tex, MakeTexture(model.textures[size_t(tex)])).first;
              if (it->second != rdr::kNoImage) body.images.push_back(it->second);
            }
            m.diffuse = it->second;
          }
        }
      }
      body.meshes.push_back(std::move(m));
    }
    body.present = !body.meshes.empty();
    types += body.present;
  }
  if (types == 0) rdr::Log("[rdr][crowd] arena %d: no crowd bodies could be loaded (%s)", loaded.arena, loaded.error.c_str());
  else rdr::Log("[rdr][crowd] arena %d: %u of %u crowd bodies loaded (3 LOD groups x 20 types), uploaded in %.1f ms%s%s", loaded.arena, types, kClassCount,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - upload_t0).count(), loaded.error.empty() ? "" : "; first failure: ", loaded.error.c_str());
  loaded = Loaded{};
  uploaded_ = types > 0;
  return uploaded_;
}

void CrowdOurs::PoseVertices(const std::vector<BodyVertex>& stored, const float* palette, size_t nodes, std::vector<PosedVertex>& out) {
  // lane 270: x / y / z as lanes 0-2 of one 4-wide vector. Every lane runs the scalar loop's operations in the scalar loop's order (((a*b + c*d) + e*f) + g, then w * that, summed bone by bone),
  // so the results are bit-identical to the one-component-at-a-time version it replaces (proved by the byte-equal pictures and by RDR_CROWD_POSE_CHECK=1, which runs both).
  using f4 = float __attribute__((ext_vector_type(4)));
  out.resize(stored.size());
  for (size_t i = 0; i < stored.size(); ++i) {
    const BodyVertex& v = stored[i];
    PosedVertex& o = out[i];
    std::memcpy(o.uv, v.uv, sizeof o.uv);
    f4 p = {0, 0, 0, 0}, n = {0, 0, 0, 0};
    float wsum = 0;
    if (palette) {
      const f4 px = v.pos[0], py = v.pos[1], pz = v.pos[2], nx = v.normal[0], ny = v.normal[1], nz = v.normal[2];
      for (int j = 0; j < 4; ++j) {
        const float w = v.weights[j];
        if (!(w > 0.f) || v.bones[j] >= nodes) continue;
        const float* t = palette + size_t(v.bones[j]) * 16;
        f4 c0, c1, c2, c3;
        std::memcpy(&c0, t, sizeof c0), std::memcpy(&c1, t + 4, sizeof c1), std::memcpy(&c2, t + 8, sizeof c2), std::memcpy(&c3, t + 12, sizeof c3);
        const f4 wv = w;
        p += wv * (px * c0 + py * c1 + pz * c2 + c3);
        n += wv * (nx * c0 + ny * c1 + nz * c2);
        wsum += w;
      }
    }
    if (wsum > 0.f) {
      o.pos[0] = p[0] / wsum, o.pos[1] = p[1] / wsum, o.pos[2] = p[2] / wsum;
      o.normal[0] = n[0] / wsum, o.normal[1] = n[1] / wsum, o.normal[2] = n[2] / wsum;
    } else {
      std::memcpy(o.pos, v.pos, sizeof o.pos);
      std::memcpy(o.normal, v.normal, sizeof o.normal);
    }
  }
}

// The scalar reference (the original loop), run beside the 4-wide one when RDR_CROWD_POSE_CHECK=1.
static void PoseVerticesRef(const std::vector<CrowdOurs::BodyVertex>& stored, const float* palette, size_t nodes, std::vector<CrowdOurs::PosedVertex>& out) {
  out.resize(stored.size());
  for (size_t i = 0; i < stored.size(); ++i) {
    const CrowdOurs::BodyVertex& v = stored[i];
    CrowdOurs::PosedVertex& o = out[i];
    std::memcpy(o.uv, v.uv, sizeof o.uv);
    float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, wsum = 0;
    if (palette) {
      for (int j = 0; j < 4; ++j) {
        const float w = v.weights[j];
        if (!(w > 0.f) || v.bones[j] >= nodes) continue;
        const float* t = palette + size_t(v.bones[j]) * 16;
        for (int c = 0; c < 3; ++c) {
          p[c] += w * (v.pos[0] * t[c] + v.pos[1] * t[4 + c] + v.pos[2] * t[8 + c] + t[12 + c]);
          n[c] += w * (v.normal[0] * t[c] + v.normal[1] * t[4 + c] + v.normal[2] * t[8 + c]);
        }
        wsum += w;
      }
    }
    if (wsum > 0.f) {
      for (int c = 0; c < 3; ++c) o.pos[c] = p[c] / wsum, o.normal[c] = n[c] / wsum;
    } else {
      std::memcpy(o.pos, v.pos, sizeof o.pos);
      std::memcpy(o.normal, v.normal, sizeof o.normal);
    }
  }
}

// ---- the frame -------------------------------------------------------------------------------------------------------------------

void CrowdOurs::NoCrowdFrame() {
  // The load screen: no crowd draw for a while. The bodies, their textures and the loader's cached pac go (the next match loads again).
  constexpr uint32_t kIdleDrop = 300;
  {  // a close-up keeps the crowd in the record (its draws may be culled); a load screen has none: only a record without crowd objects counts as idle
    std::lock_guard<std::mutex> lock(mu_);
    if (record_)
      for (const Object& o : record_->objects)
        if (o.klass == rdr_adapter::DrawClass::Crowd) {
          idle_frames_ = 0;
          return;
        }
  }
  if (idle_frames_ + 1 == 12 * kIdleDrop && load_state_.load() == 2 && !uploaded_ && arena_ < 0) {   // lane 271: an early load nobody claimed (the player left after the card) gives its memory back
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
    load_for_ = -1;
    rdr::Log("[rdr][crowd] an early load was never claimed: released");
  }
  if (++idle_frames_ == kIdleDrop && (uploaded_ || arena_ >= 0)) {
    DropBodies();
    arena_ = -1;
    load_for_ = -1;
    rdr::Log("[rdr][crowd] no crowd for %u frames: the bodies are released", kIdleDrop);
  }
}

bool CrowdOurs::FrameReady() {
  idle_frames_ = 0;
  std::shared_ptr<const SceneTables> record;
  {
    std::lock_guard<std::mutex> lock(mu_);
    record = record_;
  }
  frame_record_ = record;
  if (!record) return ++stats_.not_ready, false;
  int arena = -1;
  bool any = false;
  for (const Object& o : record->objects) {
    if (o.klass != rdr_adapter::DrawClass::Crowd || o.asset >= record->assets.size()) continue;
    any = true;
    const AssetInfo info = ParseAsset(*record, record->assets[o.asset]);
    if (info.arena >= 0) {
      arena = info.arena;
      break;
    }
  }
  if (!any || arena < 0) return ++stats_.not_ready, false;
  if (arena != arena_) {
    DropBodies();
    arena_ = arena;
  }
  if (!uploaded_) {
    const int state = load_state_.load();
    if (state == 2) {
      if (worker_.joinable()) worker_.join();
      if (loaded_.arena == arena_) Upload(loaded_);
      else loaded_ = Loaded{};
      load_state_ = 0;  // (a failed upload is not retried every frame: load_for_ still names this arena)
    } else if (state == 1 && load_for_ != arena_) {
      static int logged_for = -2;
      if (logged_for != arena_) {
        logged_for = arena_;
        rdr::Log("[rdr][crowd] early load for arena %d is being replaced by %d", load_for_, arena_);
      }
    } else if (state == 0 && load_for_ != arena_) {
      StartLoad(arena_);
    }
  }
  if (!uploaded_) return ++stats_.not_ready, false;
  return true;
}

uint16_t CrowdOurs::LayoutFor(int vs) {
  const auto found = layouts_.find(vs);
  if (found != layouts_.end()) return found->second;
  constexpr uint16_t kRGB32F = 106, kRG32F = 103;
  uint16_t id = 0xFFFF;
  const fxt::VariantTable* t = TableOf(vs);
  bool supported = t != nullptr && t->input_count > 0;
  rdr::VertexLayoutDesc desc{};
  desc.strides[0] = uint32_t(sizeof(PosedVertex));
  for (uint32_t k = 0; supported && k < t->input_count && desc.attribute_count < rdr::kMaxVertexAttributes; ++k) {
    const std::string name = t->inputs[k].name;
    rdr::VertexAttribute a{};
    a.location = t->inputs[k].location;
    if (name == "inPosition" && t->inputs[k].components == 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(PosedVertex, pos));
    else if (name == "inNormal" && t->inputs[k].components == 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(PosedVertex, normal));
    else if (name == "inUv" && t->inputs[k].components == 2) a.format = kRG32F, a.offset = uint32_t(offsetof(PosedVertex, uv));
    else supported = false;
    desc.attributes[desc.attribute_count++] = a;
  }
  if (supported) id = renderer_.AddVertexLayout(desc);
  layouts_.emplace(vs, id);
  return id;
}

namespace {
// The packed constant block of a variant from a draw's own bank (the binder's PackConstants layout), for per-member material values.
void PrefetchBank(const rdr::fx::Variant& v, const rdr_adapter::ConstantBank& bank) {  // lane 270: the lines PackFromBank will read
  for (uint32_t r = 0; r < v.constant_count; ++r) {
    const rdr::fx::ConstantRange& range = v.constants[r];
    const char* from = reinterpret_cast<const char*>(bank.dwords + uint32_t(range.reg) * 4);
    for (uint32_t b = 0; b < uint32_t(range.count) * 16; b += 64) __builtin_prefetch(from + b);
    __builtin_prefetch(from + uint32_t(range.count) * 16 - 1);
  }
}
uint32_t PackedBytes(const rdr::fx::Variant& v) { return std::min<uint32_t>(std::max<uint32_t>(v.block_bytes, 16), 8192); }
uint32_t PackFromBank(const rdr::fx::Variant& v, const rdr_adapter::ConstantBank& bank, const rdr_adapter::BoolLoop& bools, uint8_t* block) {
  const uint32_t bytes = PackedBytes(v);
  std::memset(block, 0, bytes);
  for (uint32_t r = 0; r < v.constant_count; ++r) {
    const rdr::fx::ConstantRange& range = v.constants[r];
    std::memcpy(block + range.offset, bank.dwords + uint32_t(range.reg) * 4, uint32_t(range.count) * 16);
  }
  if (v.bools_offset != 0xFFFF) std::memcpy(block + v.bools_offset, bools.words, 32);
  return bytes;
}
uint64_t WorldHashOfBank(const rdr_adapter::ConstantBank& bank) {
  const uint32_t* c = bank.dwords + 76 * 4;
  uint64_t h = 0xcbf29ce484222325ull;
  for (int k = 0; k < 12; ++k) h = (h ^ c[k]) * 0x100000001b3ull;
  return h;
}
// lane 272 (members thin, SKIP_GAME_WORK_MAP 20): do two stream draws bind the same material? Banks: the vertex bank outside c76-c78 (dwords 304-315, the member's world), the whole fragment bank, the bool/loop
// words and both programs; picture: the slot-0 image, sampler, view format and swizzle. This is the claim the thin record rests on (one bank and one picture per class), checked on every full frame.
const rdr_adapter::TextureSlot* Slot0Of(const rdr_adapter::LoadedFrame& fr, const rdr_adapter::Draw& d) {
  const rdr_adapter::TextureSlot* last = nullptr;
  for (uint32_t k = 0; k < d.texture_count; ++k)
    if (fr.texture_slots[d.texture_first + k].slot == 0) last = &fr.texture_slots[d.texture_first + k];
  return last;
}
bool MemberSamePicture(const rdr_adapter::LoadedFrame& fr, const rdr_adapter::Draw& a, const rdr_adapter::Draw& b) {
  const rdr_adapter::TextureSlot* x = Slot0Of(fr, a);
  const rdr_adapter::TextureSlot* y = Slot0Of(fr, b);
  if (!x || !y) return x == y;
  if (x->image != y->image || x->view_format != y->view_format || std::memcmp(x->swizzle, y->swizzle, sizeof x->swizzle) != 0) return false;
  return x->sampler == y->sampler || std::memcmp(&fr.samplers[x->sampler], &fr.samplers[y->sampler], sizeof fr.samplers[0]) == 0;
}
bool MemberSameBank(const rdr_adapter::LoadedFrame& fr, const rdr_adapter::Draw& a, const rdr_adapter::Draw& b) {
  if (a.vs_hash != b.vs_hash || a.ps_hash != b.ps_hash) return false;
  const uint32_t* va = fr.constant_banks[a.vs_bank].dwords;
  const uint32_t* vb = fr.constant_banks[b.vs_bank].dwords;
  if (std::memcmp(va, vb, 304 * 4) != 0 || std::memcmp(va + 316, vb + 316, (1024 - 316) * 4) != 0) return false;
  if (std::memcmp(fr.constant_banks[a.ps_bank].dwords, fr.constant_banks[b.ps_bank].dwords, sizeof(rdr_adapter::ConstantBank)) != 0) return false;
  return std::memcmp(fr.bool_loops[a.bool_loop].words, fr.bool_loops[b.bool_loop].words, sizeof(rdr_adapter::BoolLoop)) == 0;
}
uint64_t WorldHashOfMat(const Mat4& w) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) {
      uint32_t bits;
      std::memcpy(&bits, &w.m[k * 4 + r], 4);
      h = (h ^ bits) * 0x100000001b3ull;
    }
  return h;
}
}  // namespace

// lane 272: the members' thin record around InjectBody. In: the hook's tally of the stream frame (member_thin_core.h CountRing, or the cut tool's sidecar), per site. Out: in.member_result for the gate
// (1 a full frame drawn clean, 2 a full frame refused / not clean, 3 a thin frame drawn, 4 a thin frame ours could not draw = a hole). Both switches off: InjectBody alone, nothing else runs.
uint8_t CrowdOurs::MemberNotReadyResult(const rdr_adapter::LoadedFrame& frame) {
  if (!options_.members_thin || !options_.member_counts) return 0;
  const uint64_t fno = frame.header.guest_frame;
  if (fno < options_.member_offset) return 0;
  for (int site = 0; site < 2; ++site) {
    uint32_t seen = 0, dropped = 0;
    uint64_t world = 0;
    if (options_.member_counts(fno - options_.member_offset, site, seen, dropped, world) && dropped > 0) return ++stats_.member_thin_holes, 4;
  }
  return 0;
}
namespace {
enum Ph : size_t { kSetup, kAssets, kPalette, kStreamMap, kGroup, kAccounted, kPairing, kAlloc, kPose, kPackVs, kPackPs, kMaterial, kFind, kWorld, kClassChecks, kPushGeom, kPushConst, kTexSet, kDraw, kTail };
}
const char* const CrowdOurs::kPhaseNames[] = {"setup", "assets parse", "palettes", "stream map", "group members", "accounted/keep", "pairing proof", "scratch alloc", "pose verts", "pack vs bank", "pack ps bank", "material", "find stream draw", "world write", "class checks", "push geometry", "push constants", "textures+sampler", "draw op", "tail"};
const size_t CrowdOurs::kPhaseCount = sizeof kPhaseNames / sizeof kPhaseNames[0];

uint32_t CrowdOurs::Inject(rdr_adapter::CrowdInject& in) {
  if (!(options_.member_probe || options_.members_thin)) return InjectBody(in);
  mt_ = MemberFrame{};
  for (auto& row : mt_.draw_of_class) for (int& v : row) v = -1;
  if (options_.member_counts && in.frame) {
    const uint64_t fno = in.frame->header.guest_frame;
    if (fno >= options_.member_offset)
      for (int site = 0; site < 2; ++site) {
        MemberFrame::Tally& t = mt_.tally[site];
        t.known = options_.member_counts(fno - options_.member_offset, site, t.seen, t.dropped, t.world);
        mt_.signal = mt_.signal || (t.known && t.dropped > 0);
      }
  }
  if (options_.member_counts && in.frame && options_.members_thin && !mt_.signal) {   // lane 295: the thin stamp at offset 0 / 2 only (the offset-1 tally is missing): a stamp elsewhere
    const uint64_t fno = in.frame->header.guest_frame;
    bool known1 = false, other = false;
    for (int k = 0; k < 3 && fno >= uint64_t(k); ++k)
      for (int site = 0; site < (options_.floor ? 2 : 1); ++site) {
        uint32_t se = 0, dr = 0;
        uint64_t w = 0;
        const bool kn = options_.member_counts(fno - uint64_t(k), site, se, dr, w);
        if (k == 1) known1 = known1 || kn;
        else other = other || (kn && dr > 0);
      }
    mt_.elsewhere = other && !known1;
  }
  const bool signal = mt_.signal;
  const uint32_t drawn = InjectBody(in);
  if (signal) {
    in.member_result = (drawn && mt_.thin_ok) ? 3 : 4;
    if (in.member_result == 4 && !mt_.hole_counted) ++stats_.member_thin_holes;   // (a hole the body did not count itself: an early return before the thin branch)
  } else {
    in.member_result = (drawn && mt_.full_clean) ? 1 : 2;
    if (options_.members_thin && mt_.stream_short) {   // the cards' SF1 backstop: a refused frame whose stream holds fewer member draws than the record has members has probably lost them (an unknown or wrong-offset tally): a hole whatever the tally says, so a wrong offset latches after 3
      in.member_result = 4;
      if (!mt_.hole_counted) ++stats_.member_thin_holes, ++stats_.member_why[mt_.elsewhere ? wwe2k14::member_thin::kWhyElsewhere : "stream holds fewer member draws than the record has members, tally says not thin"];
    } else if (options_.members_thin && drawn && mt_.nonuniform) {   // lane 295 SF2: a class whose members bind different material on a FULL frame (the proof frame included) is the one silently-wrong-picture path (a dropped member would be drawn with the anchor's material): report it as a hole so 3 in 10 s latch the cut
      in.member_result = 4;
      if (!mt_.hole_counted) ++stats_.member_thin_holes, ++stats_.member_why["a full frame's class binds different material (non-uniform): the cut's claim is false"];
    } else {
      ++(in.member_result == 1 ? stats_.member_full_clean : stats_.member_full_refused);
    }
  }
  if (options_.member_counts && in.frame && mt_.counted) {   // lane 295: the frame judged from the tallies at offsets 0 / 1 / 2: the stamp evidence (the hook's proof), the verify arm's calls check, the Judge against what the body did
    namespace mth = wwe2k14::member_thin;
    const uint64_t fno = in.frame->header.guest_frame;
    mth::FrameView fv;
    fv.sites = options_.floor ? 2 : 1;
    for (int k = 0; k < 3 && fno >= uint64_t(k); ++k)
      for (int site = 0; site < fv.sites; ++site) {
        mth::Tally& t = fv.at[k][site];
        t.known = options_.member_counts(fno - uint64_t(k), site, t.seen, t.dropped, t.world);
      }
    for (int site = 0; site < 2; ++site) fv.rec_members[site] = mt_.rec_members[site], fv.rec_world[site] = mt_.probe_world[site], fv.stream_draws[site] = mt_.probe_members[site];
    const mth::Judgement j = mth::Judge(fv);
    stats_.member_judge_disagrees += j.take == mth::Take::Hole && mt_.thin_ok;   // a refusal the Judge makes and the body did not: must stay 0
    ++stats_.member_stamp_evidence[uint8_t(j.ev)];
    mth::g_stamp.Report(j.ev, mth::NowMs());
    if (++stats_.member_judged % 600 == 0) rdr::Log("%s", MemberSkipLine().c_str());   // the 600-frame summary line
    if (j.take == mth::Take::Full && !signal)
      for (int site = 0; site < fv.sites; ++site) stats_.member_va.Calls(fv.at[1][site], fv.stream_draws[site], fv.rec_world[site]);
    if (j.take == mth::Take::Full && !signal) ++stats_.member_va.frames;
  }
  if (options_.member_probe && options_.member_counts && !signal && drawn && in.frame) {   // the stamp probe: which offset's tally equals the full frame the game recorded
    ++stats_.member_probe_frames;
    const uint64_t fno = in.frame->header.guest_frame;
    for (int k = 0; k < 3; ++k) {
      if (fno < uint64_t(k)) continue;
      bool all = true;
      for (int site = 0; site < (options_.floor ? 2 : 1); ++site) {
        uint32_t se = 0, dr = 0;
        uint64_t w = 0;
        if (!options_.member_counts(fno - uint64_t(k), site, se, dr, w) || se != mt_.probe_members[site] || w != mt_.probe_world[site]) all = false;
      }
      stats_.member_probe_match[k] += all;
    }
    if (stats_.member_probe_frames % 600 == 0)
      rdr::Log("[rdr][crowd] member thin stamp: offset 0 / 1 / 2 match %llu / %llu / %llu of %llu full frames", static_cast<unsigned long long>(stats_.member_probe_match[0]), static_cast<unsigned long long>(stats_.member_probe_match[1]),
               static_cast<unsigned long long>(stats_.member_probe_match[2]), static_cast<unsigned long long>(stats_.member_probe_frames));
  }
  return drawn;
}

uint32_t CrowdOurs::InjectBody(rdr_adapter::CrowdInject& in) {
  const std::shared_ptr<const SceneTables> rec = frame_record_;
  if (!rec || !uploaded_ || !in.frame || !in.draw) return 0;
  ph_.Begin();
  struct PhEnd {
    PhaseClock& c;
    uint64_t* members;
    ~PhEnd() { c.Count(*members); c.End(kTail); }
  };
  uint64_t ph_members = 0;
  PhEnd ph_end{ph_, &ph_members};
  const SceneTables& t = *rec;
  const rdr_adapter::LoadedFrame& fr = *in.frame;
  const uint16_t layout = LayoutFor(in.vs);
  const fxt::VariantTable* table = TableOf(in.vs);
  const fxt::Field* world_field = FieldOf(table, "WorldMatrix");
  const fxt::Field* vp_field = FieldOf(table, "ViewProj");
  if (layout == 0xFFFF || !world_field || world_field->vec4s < 3 || !vp_field || world_field->offset + 48u > in.vs_block_bytes) {
    if (!said_variant_) {
      said_variant_ = true;
      rdr::Log("[rdr][crowd] the dropped draw's vertex effect %s cannot be fed by our bodies (inputs or WorldMatrix): the stream's crowd stays", in.vs >= 0 ? kVariants[in.vs].name : "?");
    }
    ++stats_.unsupported_variant;
    return 0;
  }

  ph_.Lap(kSetup);
  // Assets -> (type, lod / card); palettes by type.
  std::vector<AssetInfo> info(t.assets.size());
  for (size_t i = 0; i < t.assets.size(); ++i) info[i] = ParseAsset(t, t.assets[i]);
  ph_.Lap(kAssets);
  if (parse_check_)  // lane 270 (env RDR_CROWD_PARSE_CHECK=1): the fast reading must equal the sscanf one, field for field, on every asset of every frame
    for (size_t i = 0; i < t.assets.size(); ++i) {
      const AssetInfo ref = ParseAssetRef(t, t.assets[i]);
      ++stats_.parse_checked;
      stats_.parse_mismatch += ref.valid != info[i].valid || ref.card != info[i].card || ref.type != info[i].type || ref.lod != info[i].lod || ref.arena != info[i].arena;
    }
  struct PaletteRef { int score = -1; uint32_t first = 0, count = 0; };
  std::array<PaletteRef, kClassCount> palette{};  // per class = lod * 20 + type
  for (const Object& o : t.objects) {
    if (o.klass != rdr_adapter::DrawClass::Crowd || o.asset >= info.size() || !info[o.asset].valid || !(o.flags & rdr_scene::kObjSkinned) || !o.bones_count) continue;
    if (size_t(o.bones_first) + o.bones_count > t.bones.size()) continue;
    const AssetInfo& a = info[o.asset];
    // A member is posed with the palette of ITS OWN (type, LOD group) (the game animates every LOD group with its own palette block): the bones range of any object of that class.
    // A pose object (invisible, id "crowd:pose:TT") and a card name the type's LOD 0 palette (cards take it; used for the LOD 0 class when no LOD 0 member exists).
    const bool pose_object = !(o.flags & kObjVisible);
    const uint32_t lod = (pose_object || a.card) ? 0u : a.lod;
    const int score = pose_object ? 10 : a.card ? 8 : 7;
    PaletteRef& slot = palette[lod * kTypes + a.type];
    if (score > slot.score) slot = {score, o.bones_first, o.bones_count};
  }

  ph_.Lap(kPalette);
  // The stream's own crowd draws of this frame by member world (c76-c78, bit for bit: lane 36): the pairing proof and each member's material (its texture / variant and
  // constants come from ITS stream draw, never from the first one).
  fast::FlatU64Map& stream = stream_map_;  // world hash -> draw index (lane 270: a reused flat table; first insert wins like emplace)
  stream.Reset(t.objects.size() + 64);
  uint64_t stream_draws = 0, floor_stream_draws = 0;
  std::vector<std::pair<uint64_t, uint32_t>> claimed;  // S101 lane 82 follow-up: every claimed stream draw (world hash, draw index)
  const bool member_on = (options_.member_probe || options_.members_thin) && !options_.cards_as_bodies && !options_.fault_no_cards;   // lane 272
  std::vector<uint8_t> in_ops;   // lane 272: the stream is what the op list DRAWS (a thin capture's cut draws keep their table rows; a live frame's table holds only recorded draws)
  if (member_on) {
    in_ops.assign(fr.draws.count, 0);
    for (uint32_t k = 0; k < fr.ops.count; ++k)
      if (fr.ops[k].kind == rdr_adapter::OpKind::Draw && fr.ops[k].index < fr.draws.count) in_ops[fr.ops[k].index] = 1;
  }
  for (uint32_t d = 0; d < fr.draws.count; ++d) {
    if (member_on && !in_ops[d]) continue;
    if (d + 16 < fr.draws.count) {  // lane 270: the c76-c78 words of a crowd draw's bank sit in a cache line nobody has touched this frame (banks are 4 KB apart): ask for the one 16 draws ahead
      const rdr_adapter::DrawOrigin* ahead = fr.OriginOf(d + 16);
      if (ahead && (ahead->call_site == rdr_adapter::kCrowdBlocksSite || ahead->call_site == rdr_adapter::kCrowdCardsSite || ahead->call_site == rdr_adapter::kCrowdFloorSite)) {
        const char* w = reinterpret_cast<const char*>(fr.constant_banks[fr.draws[d + 16].vs_bank].dwords + 76 * 4);
        __builtin_prefetch(w), __builtin_prefetch(w + 47);
      }
    }
    const rdr_adapter::DrawOrigin* og = fr.OriginOf(d);
    const bool floor_site = og && og->call_site == rdr_adapter::kCrowdFloorSite && DrawsFloor();  // S101 lane 82
    if (!og || (og->call_site != rdr_adapter::kCrowdBlocksSite && !(og->call_site == rdr_adapter::kCrowdCardsSite && (options_.cards_as_bodies || options_.fault_no_cards)) && !floor_site)) continue;  // S101 lane CD: cards on (InjectCards) are not members of this seam
    floor_stream_draws += floor_site;
    ++stream_draws;
    claimed.emplace_back(WorldHashOfBank(fr.constant_banks[fr.draws[d].vs_bank]), d);
    stream.emplace(claimed.back().first, d);
  }

  ph_.Lap(kStreamMap);
  // Group the visible members by class = (LOD group, type): each member is drawn with ITS OWN LOD's body and palette, as the stream does. The planted fault `lod0_palette` is the
  // old behaviour: every member as its type's LOD 0 body with the LOD 0 palette.
  for (ClassScratch& sc : scratch_) {
    sc.members.clear();
    sc.real_lod.clear();
    sc.is_floor.clear();
  }
  int undrawable_type = -1;  // the planted fault: the type of the first dropped draw (its member found by world)
  if (options_.fault_undrawable_first) {
    const uint64_t first_world = WorldHashOfBank(fr.constant_banks[in.draw->vs_bank]);
    for (const Object& o : t.objects)
      if (o.klass == rdr_adapter::DrawClass::Crowd && (o.flags & kObjVisible) && o.asset < info.size() && info[o.asset].valid && WorldHashOfMat(o.world) == first_world) {
        undrawable_type = int(info[o.asset].type);
        break;
      }
  }
  uint32_t skipped = 0;
  std::vector<uint32_t> kept;  // members we cannot draw: their stream draws stay (never a hole, never a T-pose)
  keep_draws_.clear();
  fast::FlatU64Map& accounted = accounted_set_;  // worlds of the members drawn / density-skipped / kept; a claimed stream draw outside it has NO member in the record
  bool missing_planted = false;
  accounted.Reset(t.objects.size() + 64);
  for (uint32_t i = 0; i < t.objects.size(); ++i) {
    const Object& o = t.objects[i];
    if (o.klass != rdr_adapter::DrawClass::Crowd || !(o.flags & kObjVisible)) continue;
    if (o.asset >= info.size() || !info[o.asset].valid) { ++skipped; kept.push_back(i); continue; }
    const AssetInfo& a = info[o.asset];
    if (options_.fault_record_missing_member && !missing_planted && !a.card && !IsFloorObject(t, o)) { missing_planted = true; continue; }  // planted
    if (a.card && !options_.cards_as_bodies) continue;  // (the stream keeps its cards, or the planted fault drops them)
    // S101 lane 82: the seated floor people are ordinary LOD 0 members of this seam when the floor switch is on; with it off the record's floor objects are not ours to draw.
    const bool floor_member = !a.card && IsFloorObject(t, o);
    if (floor_member && !options_.floor) continue;
    uint32_t real_lod = a.card ? 0u : a.lod;
    uint32_t member_type = a.type;
    if (floor_member && options_.fault_floor_lod1) real_lod = 1;                                      // planted: the 317-vertex body
    if (floor_member && options_.fault_floor_type_shift) member_type = (member_type + 1) % kTypes;     // planted: the next person type
    const uint32_t cls = (options_.fault_lod0_palette ? 0u : real_lod) * kTypes + member_type;
    if (!bodies_[cls].present || int(a.type) == undrawable_type) { ++skipped; kept.push_back(i); continue; }
    if (in.density < 100 && in.rank) {
      uint32_t w[3];
      std::memcpy(w, &o.world.m[12], sizeof w);  // the member's position bits = c76.w c77.w c78.w of the game's draw
      if (in.rank(w[0], w[1], w[2]) >= in.density) { accounted.insert(WorldHashOfMat(o.world)); continue; }
    }
    if (floor_member && options_.fault_floor_drop_no_draw) continue;  // planted: the floor site is claimed (its stream draws are dropped) and nothing of ours is drawn
    accounted.insert(WorldHashOfMat(o.world));
    for (int copies = (floor_member && options_.fault_floor_dup) ? 2 : 1; copies > 0; --copies) {  // planted: the floor members emitted twice
      scratch_[cls].members.push_back(i);
      scratch_[cls].real_lod.push_back(uint8_t(real_lod));
      scratch_[cls].is_floor.push_back(floor_member ? 1 : 0);
    }
  }
  ph_.Lap(kGroup);
  // A class with no palette in the record (the stored pose is a node-local pile: drawn at the member's place it is a visible clump) is not drawn: its members keep their stream draws.
  uint32_t unposed_kept = 0;
  if (!options_.fault_bind_pose)
    for (uint32_t cls = 0; cls < kClassCount; ++cls)
      if (palette[cls].score < 0 && !scratch_[cls].members.empty()) {
        unposed_kept += uint32_t(scratch_[cls].members.size());
        kept.insert(kept.end(), scratch_[cls].members.begin(), scratch_[cls].members.end());
        scratch_[cls].members.clear();
        scratch_[cls].real_lod.clear();
        scratch_[cls].is_floor.clear();
      }
  for (uint32_t index : kept) {
    const auto it = stream.find(WorldHashOfMat(t.objects[index].world));
    if (it != stream.end()) keep_draws_.insert(it->second);
  }
  // A claimed stream draw whose member is not in the record at all (a record from a stretch without it, a pairing slip on a frame that is not verified) is NOT dropped:
  // it stays the game's (never a hole). Not for the planted no_cards fault, which drops the cards on purpose.
  if (!options_.fault_no_cards) {
    for (const ClassScratch& sc : scratch_)
      for (uint32_t index : sc.members) accounted.insert(WorldHashOfMat(t.objects[index].world));
    for (uint32_t index : kept) accounted.insert(WorldHashOfMat(t.objects[index].world));
    for (const auto& c : claimed)
      if (!accounted.count(c.first)) keep_draws_.insert(c.second);
  }

  // lane 272 (members thin, SKIP_GAME_WORK_MAP 20.3 / 20.4): the class of every claimed stream draw (by its world), the uniformity of each class's draws (every full frame: the claim the cut rests on), and, on a THIN
  // frame, the tally check that replaces the pairing proof below plus the class material from the kept draws. A thin frame ours cannot draw is a HOLE (counted; the gate resets; 3 in 10 s latch the cut).
  const bool thin = member_on && options_.members_thin && mt_.signal;
  if (member_on) {
    // The census (class map + uniformity compare) runs on every thin frame and every full frame of a real skip run (full frames are rare there: the proofs); a census-only run samples every 8th frame.
    const bool map_on = thin || options_.members_thin || (member_sample_++ % 8 == 0);
    fast::FlatU64Map& by_world = member_world_map_;   // world hash -> (floor ? 256 : 0) | class of the record's visible member
    if (map_on) by_world.Reset(t.objects.size() + 64);
    bool ambiguous = false, unmapped = false, wrong_site = false;
    uint64_t rec_members[2] = {0, 0}, rec_world[2] = {0, 0}, sdraws[2] = {0, 0};
    for (const Object& o : t.objects) {
      if (o.klass != rdr_adapter::DrawClass::Crowd || !(o.flags & kObjVisible)) continue;
      const bool valid = o.asset < info.size() && info[o.asset].valid;
      if (valid && info[o.asset].card) continue;   // (the far cards: InjectCards, another site)
      const bool floor_member = IsFloorObject(t, o);
      if (floor_member && !options_.floor) continue;
      const uint64_t h = WorldHashOfMat(o.world);
      ++rec_members[floor_member], rec_world[floor_member] += h;
      if (!valid || !map_on) continue;
      const uint32_t code = (floor_member ? 256u : 0u) | ((options_.fault_lod0_palette ? 0u : info[o.asset].lod) * kTypes + info[o.asset].type);
      const auto prev = by_world.find(h);
      if (prev != by_world.end()) ambiguous = ambiguous || prev->second != code;
      else by_world.emplace(h, code);
    }
    int first_draw = -1;
    for (const auto& c : claimed) {   // (stream order: the lowest draw index of a class is its first)
      const uint32_t d = c.second;
      const rdr_adapter::DrawOrigin* og = fr.OriginOf(d);
      const bool fl = og && og->call_site == rdr_adapter::kCrowdFloorSite;
      ++sdraws[fl];
      if (!map_on) continue;
      const auto it = by_world.find(c.first);
      if (it == by_world.end()) { unmapped = true; stats_.member_va.Unsourced(1); continue; }
      if (((it->second >> 8) != 0) != fl) { wrong_site = true; continue; }
      int& slot = mt_.draw_of_class[fl][it->second & 255];
      if (first_draw < 0) first_draw = int(d);
      if (slot < 0) {
        slot = int(d);
        ++stats_.member_va.members;
        continue;
      }
      const rdr_adapter::Draw& a = fr.draws[uint32_t(slot)];
      const rdr_adapter::Draw& b = fr.draws[d];
      const bool pic = MemberSamePicture(fr, a, b), bank = MemberSameBank(fr, a, b);
      stats_.member_va.Member(pic, bank);
      mt_.nonuniform = mt_.nonuniform || !pic || !bank;
    }
    stats_.member_nonuniform_frames += mt_.nonuniform;
    if (map_on && options_.fault_member_one_material && first_draw >= 0)   // planted: every class takes the frame's first claimed draw's material
      for (auto& row : mt_.draw_of_class) for (int& v : row) if (v >= 0) v = first_draw;
    for (int site = 0; site < 2; ++site) mt_.probe_members[site] = sdraws[site], mt_.probe_world[site] = rec_world[site], mt_.rec_members[site] = rec_members[site];
    mt_.counted = true;
    mt_.stream_short = !thin && (sdraws[0] + sdraws[1]) < (rec_members[0] + rec_members[1]);
    if (options_.members_thin && !thin && mt_.stream_short) {   // SF3: a thin frame the tally did not announce would be drawn with sd = null (wrong outfits) and read as clean: it is a hole, nothing is drawn
      mt_.hole_counted = true;
      ++stats_.member_thin_holes, ++stats_.member_why[mt_.elsewhere ? wwe2k14::member_thin::kWhyElsewhere : "stream holds fewer member draws than the record has members, tally says not thin"];
      return 0;
    }
    if (thin) {
      const char* why = nullptr;
      for (int site = 0; site < 2 && !why; ++site) {
        if (site == 1 && !options_.floor) continue;
        const MemberFrame::Tally& tl = mt_.tally[site];
        if (options_.fault_member_no_tally) continue;
        wwe2k14::member_thin::Tally ct;
        ct.seen = tl.seen, ct.dropped = tl.dropped, ct.world = tl.world, ct.known = tl.known;
        wwe2k14::member_thin::Verdict v = wwe2k14::member_thin::CheckTally(ct, rec_members[site], rec_world[site], sdraws[site]);
        if (v == wwe2k14::member_thin::Verdict::NotThin)   // a site where every run holds one member: nothing dropped, the whole site must still be the record's
          v = (tl.seen == rec_members[site] && tl.world == rec_world[site] && sdraws[site] == tl.seen) ? wwe2k14::member_thin::Verdict::Ok : wwe2k14::member_thin::Verdict::SeenMismatch;
        if (v != wwe2k14::member_thin::Verdict::Ok) why = wwe2k14::member_thin::VerdictName(v);
      }
      if (!why && ambiguous) why = "member worlds are not unique within the record";
      if (!why && unmapped) why = "a kept stream draw has no member in the record";
      if (!why && wrong_site) why = "a kept stream draw is at the other site than its member";
      if (!why && mt_.nonuniform) why = "a class's kept draws bind different material";
      if (!why && !kept.empty()) why = "members ours cannot draw (their dropped draws are gone)";
      if (!why)
        for (uint32_t cls = 0; cls < kClassCount && !why; ++cls)
          for (size_t m = 0; m < scratch_[cls].members.size() && !why; ++m)
            if (mt_.draw_of_class[(m < scratch_[cls].is_floor.size() && scratch_[cls].is_floor[m]) ? 1 : 0][cls] < 0) why = "a class has no kept draw";
      if (why) {
        mt_.hole_counted = true;
        ++stats_.member_thin_holes, ++stats_.member_why[why];
        if (++member_logged_ <= 5) rdr::Log("[rdr][crowd] WARN members: a THIN frame ours cannot draw (%s; record members %llu + %llu floor, kept stream draws %llu + %llu floor): its dropped members are missing on this frame", why, static_cast<unsigned long long>(rec_members[0]), static_cast<unsigned long long>(rec_members[1]), static_cast<unsigned long long>(sdraws[0]), static_cast<unsigned long long>(sdraws[1]));
        return 0;
      }
      mt_.thin_ok = true;
      ++stats_.member_thin_frames, stats_.member_thin_members += rec_members[0] + rec_members[1], stats_.member_thin_dropped += (rec_members[0] + rec_members[1]) - (sdraws[0] + sdraws[1]);
    }
  }
  ph_.Lap(kAccounted);
  // The proof (offline every frame, live every Nth): exact worlds AND the same count as the stream draws that will be dropped. A bad frame is NOT drawn: it falls back to the stream.
  if (!thin && (options_.verify_pairing || (options_.verify_every && stats_.frames % options_.verify_every == 0))) {
    uint64_t total = 0, match = 0;
    for (uint32_t index : kept) {  // (members we do not draw still pair with a stream draw: their draws are kept)
      ++total;
      match += stream.count(WorldHashOfMat(t.objects[index].world));
    }
    for (const ClassScratch& s : scratch_)
      for (uint32_t index : s.members) {
        const Mat4& w = t.objects[index].world;
        ++total;
        const auto it = stream.find(WorldHashOfMat(w));
        match += it != stream.end();
      }
    stats_.pair_total += total, stats_.pair_match += match, stats_.stream_draws += stream_draws;
    // An off-by-one still matches ~93% of worlds, so the count equality is the test that sees it (clean 77525 == 77525; offset 80607 vs 78085).
    if (total && (match != total || (in.density >= 100 && total != stream_draws))) {
      if (++stats_.pair_bad_frames == 1 || stats_.pair_bad_frames % 100 == 0)
        rdr::Log("[rdr][crowd] WARN the record does not pair with the stream frame: %llu of %llu member worlds match, %llu members vs %llu stream crowd draws (bad frames so far %llu): this frame keeps the stream's crowd",
                 static_cast<unsigned long long>(match), static_cast<unsigned long long>(total), static_cast<unsigned long long>(total), static_cast<unsigned long long>(stream_draws),
                 static_cast<unsigned long long>(stats_.pair_bad_frames));
      return 0;
    }
  }

  ph_.Lap(kPairing);
  // Phase A (per class; thread-safe per class): the posed vertices of each mesh, and per member its constant blocks and material (from its stream draw).
  const uint32_t block_bytes = in.vs_block_bytes;
  const uint32_t fs_bytes = in.fs_block_bytes;
  const uint64_t first_vs_hash = in.draw->vs_hash, first_ps_hash = in.draw->ps_hash;
  const rdr_adapter::Draw* first_floor_sd = nullptr;  // S101 lane 82 (the floor_first_texture plant)
  for (uint32_t cls = 0; cls < kClassCount; ++cls) {
    ClassScratch& s = scratch_[cls];
    s.blocks.clear();
    s.fblocks.clear();
    s.materials.clear();
    if (s.members.empty()) continue;
    const TypeBody& body = bodies_[cls];
    const PaletteRef& pal = palette[cls];
    const float* palette_floats = (pal.score >= 0 && !options_.fault_bind_pose && size_t(pal.first) + pal.count <= t.bones.size()) ? t.bones[pal.first].m : nullptr;
    if (!palette_floats) stats_.unposed += s.members.size();  // (only the planted bind-pose fault gets here)
    ph_.Lap(kSetup);
    // (lane 270 measured a cross-frame pose cache: 0 hits in 27 797 class-frames, the palettes change on every record; removed. PoseVertices itself is the 4-wide version below.)
    for (size_t k = 0; k < body.meshes.size(); ++k) PoseVertices(body.meshes[k].vertices, palette_floats, palette_floats ? pal.count : 0, s.posed[k]);
    ph_.Lap(kPose);
    if (pose_check_)  // lane 270 (env RDR_CROWD_POSE_CHECK=1): the 4-wide pose must equal the scalar reference bit for bit, on every vertex of every class of every frame
      for (size_t k = 0; k < body.meshes.size(); ++k) {
        std::vector<PosedVertex> ref;
        PoseVerticesRef(body.meshes[k].vertices, palette_floats, palette_floats ? pal.count : 0, ref);
        ++stats_.pose_checked;
        stats_.pose_mismatch += ref.size() != s.posed[k].size() || std::memcmp(ref.data(), s.posed[k].data(), ref.size() * sizeof(PosedVertex)) != 0;
      }
    s.blocks.resize(s.members.size() * block_bytes);
    s.fblocks.resize(s.members.size() * fs_bytes);
    s.materials.resize(s.members.size());
    ph_.Lap(kAlloc);
    constexpr size_t kAhead = 4;  // lane 270: the constant banks of the stream draws are a cache line or six each, spread over 12 MB: ask for member m + kAhead's before packing member m
    s.sds.resize(s.members.size());
    for (size_t m = 0; m < s.members.size(); ++m) {
      if (thin && !options_.fault_member_own_sd) {   // lane 272: a thin frame's material is its class's kept draw (the run's first), the claim the cut rests on; the game dropped the members' own draws
        const int class_draw = mt_.draw_of_class[(m < s.is_floor.size() && s.is_floor[m]) ? 1 : 0][cls];
        s.sds[m] = class_draw >= 0 ? &fr.draws[uint32_t(class_draw)] : nullptr;
        continue;
      }
      const auto it = stream.find(WorldHashOfMat(t.objects[s.members[m]].world));
      s.sds[m] = it != stream.end() ? &fr.draws[it->second] : nullptr;
    }
    ph_.Lap(kFind);
    for (size_t m = 0; m < s.members.size(); ++m) {
      if (m + kAhead < s.members.size() && s.sds[m + kAhead]) {
        const rdr_adapter::Draw* ahead = s.sds[m + kAhead];
        if (in.vs >= 0) PrefetchBank(kVariants[in.vs], fr.constant_banks[ahead->vs_bank]);
        if (in.ps >= 0) PrefetchBank(kVariants[in.ps], fr.constant_banks[ahead->ps_bank]);
      }
      const bool floor_m = m < s.is_floor.size() && s.is_floor[m];
      uint8_t* block = s.blocks.data() + m * block_bytes;
      uint8_t* fblock = s.fblocks.data() + m * fs_bytes;
      const Mat4& w = t.objects[s.members[m]].world;
      const rdr_adapter::Draw* sd = s.sds[m];
      ClassScratch::Material& mat = s.materials[m];
      mat = ClassScratch::Material{};
      if (sd && (sd->vs_hash != first_vs_hash || sd->ps_hash != first_ps_hash) && ++stats_.other_program == 1)
        rdr::Log("[rdr][crowd] a member's own stream draw uses another program than the first dropped draw (vs %016llX / ps %016llX vs %016llX / %016llX): it gets the first draw's program and state; counted (other_program)", static_cast<unsigned long long>(sd->vs_hash), static_cast<unsigned long long>(sd->ps_hash), static_cast<unsigned long long>(first_vs_hash), static_cast<unsigned long long>(first_ps_hash));
      else if (sd && (sd->vs_hash != first_vs_hash || sd->ps_hash != first_ps_hash))
        ;  // (counted by the condition above)
      ph_.Lap(kFind);
      if (sd && sd->vs_hash == first_vs_hash && in.vs >= 0) {
        if (PackedBytes(kVariants[in.vs]) <= block_bytes) {  // lane 270: straight into the member's block (the same bytes the 8 KB temp copy gave)
          PackFromBank(kVariants[in.vs], fr.constant_banks[sd->vs_bank], fr.bool_loops[sd->bool_loop], block);
        } else {
          uint8_t tmp[8192];
          const uint32_t n = PackFromBank(kVariants[in.vs], fr.constant_banks[sd->vs_bank], fr.bool_loops[sd->bool_loop], tmp);
          std::memcpy(block, tmp, std::min(n, block_bytes));
        }
      } else {
        std::memcpy(block, in.vs_block, block_bytes);
      }
      ph_.Lap(kPackVs);
      if (sd && sd->ps_hash == first_ps_hash && in.ps >= 0) {
        if (PackedBytes(kVariants[in.ps]) <= fs_bytes) {
          PackFromBank(kVariants[in.ps], fr.constant_banks[sd->ps_bank], fr.bool_loops[sd->bool_loop], fblock);
        } else {
          uint8_t tmp[8192];
          const uint32_t n = PackFromBank(kVariants[in.ps], fr.constant_banks[sd->ps_bank], fr.bool_loops[sd->bool_loop], tmp);
          std::memcpy(fblock, tmp, std::min(n, fs_bytes));
        }
      } else {
        std::memcpy(fblock, in.fs_block, fs_bytes);
      }
      ph_.Lap(kPackPs);
      // S101 lane 82: the planted fault `floor_first_texture`: every floor member takes the first floor draw's slot 0 ("several outfits", lane 35's bug, re-found)
      const rdr_adapter::Draw* tex_sd = sd;
      if (floor_m && sd && options_.fault_floor_first_texture) {
        if (!first_floor_sd) first_floor_sd = sd;
        tex_sd = first_floor_sd;
      }
      if (tex_sd) {  // slot 0 (the diffuse: this member's shirt / variant) exactly as the stream binds it
        for (uint32_t k = 0; k < tex_sd->texture_count; ++k) {
          const rdr_adapter::TextureSlot& ts = fr.texture_slots[tex_sd->texture_first + k];
          if (ts.slot != 0 || ts.image >= in.image_count || !in.images || in.images[ts.image] == rdr::kNoImage) continue;
          mat.has = true;
          mat.image = in.images[ts.image];
          mat.sampler = fr.samplers[ts.sampler];
          mat.view_format = rdr::Format(ts.view_format);
          std::memcpy(mat.swizzle, ts.swizzle, sizeof mat.swizzle);
        }
      }
      if ((options_.floor || options_.verify_pairing) && !thin && sd && mat.has) {  // S101 lane 82: the material proof (offline / floor only: new per-member work): this member's slot-0 picture IS the one its own stream draw binds
        rdr::ImageId own = rdr::kNoImage;
        for (uint32_t k = 0; k < sd->texture_count; ++k) {
          const rdr_adapter::TextureSlot& ts = fr.texture_slots[sd->texture_first + k];
          if (ts.slot == 0 && ts.image < in.image_count && in.images) own = in.images[ts.image];
        }
        ++stats_.material_total;
        stats_.material_ok += own == mat.image;
      }
      ph_.Lap(kMaterial);
      // The world (the record's) goes over whatever the stream draw had.
      float* c = reinterpret_cast<float*>(block + world_field->offset);
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) c[r * 4 + k] = w.m[k * 4 + r];  // row r of c76-c78 = column r of the member's row-vector matrix (crowd_contrib.h WorldFromConstants)
      if (options_.fault_identity_viewproj) {
        float* vp = reinterpret_cast<float*>(block + vp_field->offset);
        for (int r = 0; r < 4; ++r)
          for (int k = 0; k < 4; ++k) vp[r * 4 + k] = r == k ? 1.f : 0.f;
      }
      ph_.Lap(kWorld);
    }
  }

  // The class checks (verified frames, when the binder gives the geometry): per (class, real LOD) the first member that pairs with a stream draw: its triangle count (non-degenerate, the
  // strip normalised) and vertex count must EQUAL the stream's, and our posed positions must equal the stream's captured blob (stride 28: position R32G32B32 at 0; lane 36: same vertex order).
  // S101 lane 50 (P2c): while the game's skin loops are being skipped the stream's captured vertex blobs are stale, so the pose comparison below would report MISMATCH for a pose that is right:
  // it is not run (the geometry counts go with it). The pairing proof above (member worlds) does not read the blobs and still runs.
  if ((options_.verify_pairing || (options_.verify_every && stats_.frames % options_.verify_every == 0)) && in.read_geometry && !wwe2k14::crowd_skip::SkippingNow(wwe2k14::crowd_skip::NowMs())) {
    for (uint32_t cls = 0; cls < kClassCount; ++cls) {
      const ClassScratch& s = scratch_[cls];
      bool done[3][2] = {};  // S101 lane 82: the floor members of a class are checked on their own (the first stands member must not hide a wrong floor body)
      for (size_t m = 0; m < s.members.size(); ++m) {
        const uint8_t real = s.real_lod[m];
        const bool floor_m = m < s.is_floor.size() && s.is_floor[m];
        if (real > 2 || done[real][floor_m]) continue;
        const auto it = stream.find(WorldHashOfMat(t.objects[s.members[m]].world));
        if (it == stream.end()) continue;
        const rdr_adapter::Draw& sd = fr.draws[it->second];
        std::vector<uint8_t> vb, ib;
        if (!sd.vertex_bytes || !sd.index_bytes || !in.read_geometry(sd.vertex_offset, sd.vertex_bytes, vb) || !in.read_geometry(sd.index_offset, sd.index_bytes, ib)) continue;
        done[real][floor_m] = true;
        const TypeBody& body = bodies_[cls];
        uint32_t stream_tris = 0;
        const uint32_t restart = sd.index_32bit ? 0xFFFFFFFFu : 0xFFFFu;
        const auto idx = [&](uint32_t k) -> uint32_t {
          uint32_t v = 0;
          if (sd.index_32bit) std::memcpy(&v, ib.data() + size_t(k) * 4, 4);
          else { uint16_t w16; std::memcpy(&w16, ib.data() + size_t(k) * 2, 2); v = w16; }
          return v;
        };
        const uint32_t n_idx = std::min<uint32_t>(sd.index_count, uint32_t(ib.size() / (sd.index_32bit ? 4 : 2)));
        if (sd.source_prim == 6) {
          for (uint32_t k = 2; k < n_idx; ++k) {
            const uint32_t a0 = idx(k - 2), b0 = idx(k - 1), c0 = idx(k);
            stream_tris += a0 != b0 && b0 != c0 && a0 != c0 && a0 != restart && b0 != restart && c0 != restart;
          }
        } else {
          stream_tris = n_idx / 3;
        }
        uint32_t ours_tris = 0, ours_verts = 0;
        for (const MeshGpu& mg : body.meshes) ours_tris += mg.index_count / 3 * (options_.fault_double_mesh ? 2u : 1u), ours_verts += uint32_t(mg.vertices.size());
        const uint32_t stream_verts = uint32_t(vb.size() / 28);
        const bool geom_ok = ours_tris == stream_tris && ours_verts == stream_verts;
        double mean = 1e9, worst = 1e9;
        if (body.meshes.size() == 1 && s.posed[0].size() == stream_verts) {
          double sum = 0, mx = 0;
          for (uint32_t k = 0; k < stream_verts; ++k) {
            float p[3];
            std::memcpy(p, vb.data() + size_t(k) * 28, 12);
            double d = 0;
            for (int c = 0; c < 3; ++c) d += std::fabs(double(p[c]) - double(s.posed[0][k].pos[c]));
            sum += d, mx = std::max(mx, d);
          }
          mean = stream_verts ? sum / stream_verts : 0, worst = mx;
        }
        const bool pose_ok = mean <= 1e-3 && worst <= 1e-2;
        ++stats_.class_total;
        stats_.class_geom_ok += geom_ok;
        stats_.class_pose_ok += pose_ok;
        if (floor_m) ++stats_.floor_class_total, stats_.floor_class_geom_ok += geom_ok, stats_.floor_class_pose_ok += pose_ok;  // S101 lane 82
        if (stats_.class_total <= 60 || !geom_ok || !pose_ok) {
          if (++class_logged_ <= 64)
            rdr::Log("[rdr][crowd] class type %u LOD %u%s: nodes %u, vertices %u vs the stream's %u, triangles %u vs %u, pose residual mean %.4f max %.4f units%s", cls % kTypes, unsigned(real), floor_m ? " (floor)" : "", palette[cls].count, ours_verts,
                     stream_verts, ours_tris, stream_tris, mean, worst, (geom_ok && pose_ok) ? "" : "  <-- MISMATCH");
        }
      }
    }
  }

  ph_.Lap(kClassChecks);
  // Phase B (serial, the builder): geometry, constants, draws.
  rdr::FrameBuilder& builder = *in.builder;
  rdr::DrawState state = in.state;
  state.vertex_layout = layout;
  state.topology = 3;           // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: our bodies are lists (the borrowed state's topology is the stream's strip)
  state.primitive_restart = 0;
  rdr::DrawOp base{};
  base.state = builder.State(state);
  base.dynamic = builder.Dynamic(in.dynamic);
  base.instances = 1;
  base.index_32bit = 1;
  const uint32_t fallback_sampler = in.textures.slots[0].sampler;  // the game's own sampler for slot 0 (anisotropy setting included)
  uint32_t drawn = 0, cards = 0;
  // lane 270: members of a class mostly share a few materials, and builder.Textures() / Sampler() hash and compare a whole TextureSet (a dozen slots) for every one. Within this call the same slot-0
  // content gives the same table id, so it is looked up once per distinct content: the key is EVERYTHING that goes into slot 0 (picture, sampler description or the game's sampler index, view format,
  // swizzle). Same ids in the same order as the uncached calls (a table's id is its first appearance). The planted fault `cache_tex_stale` leaves the picture out of the key.
  tex_keys_.clear(), tex_ids_.clear();
  tex_map_.Reset(256);
  for (uint32_t cls = 0; cls < kClassCount; ++cls) {
    ClassScratch& s = scratch_[cls];
    if (s.members.empty()) continue;
    const TypeBody& body = bodies_[cls];
    for (size_t k = 0; k < body.meshes.size(); ++k)
      s.geometry_offset[k] = builder.PushGeometry(s.posed[k].data(), uint32_t(s.posed[k].size() * sizeof(PosedVertex)), 16);
    ph_.Lap(kPushGeom);
    for (size_t m = 0; m < s.members.size(); ++m) {
      const uint32_t vs_constants = builder.PushConstants(s.blocks.data() + m * block_bytes, block_bytes);
      const uint32_t fs_constants = builder.PushConstants(s.fblocks.data() + m * fs_bytes, fs_bytes);
      ph_.Lap(kPushConst);
      const ClassScratch::Material& mat = s.materials[m];
      for (size_t k = 0; k < body.meshes.size(); ++k) {
        TexKey key;
        std::memset(&key, 0, sizeof key);
        key.has = mat.has ? 1u : 0u;
        key.image = mat.has ? mat.image : (body.meshes[k].diffuse != rdr::kNoImage ? body.meshes[k].diffuse : white_);
        if (mat.has) key.sampler = mat.sampler, key.view_format = uint32_t(mat.view_format), std::memcpy(key.swizzle, mat.swizzle, sizeof key.swizzle);
        else key.fallback_sampler = fallback_sampler;
        if (options_.fault_cache_tex_stale) key.image = 0;  // planted: the picture is not part of the key
        uint64_t hash = 0xcbf29ce484222325ull;
        for (size_t b = 0; b < sizeof key; ++b) hash = (hash ^ reinterpret_cast<const uint8_t*>(&key)[b]) * 0x100000001b3ull;
        uint32_t textures_id = 0;
        const uint32_t* found = options_.caches ? tex_map_.Find(hash) : nullptr;
        if (found && tex_keys_[*found] == key) {
          textures_id = tex_ids_[*found];
          ++stats_.tex_hit;
        } else {
          rdr::TextureSet set = in.textures;
          rdr::TextureSlot& slot = set.slots[0];
          if (mat.has) {
            slot.image = mat.image;
            slot.sampler = builder.Sampler(mat.sampler);
            slot.view_format = mat.view_format;
            std::memcpy(slot.swizzle, mat.swizzle, sizeof slot.swizzle);
          } else {  // no stream draw for this member: our own picture of its type, the game's sampler
            slot.image = body.meshes[k].diffuse != rdr::kNoImage ? body.meshes[k].diffuse : white_;
            slot.sampler = fallback_sampler;
            slot.view_format = rdr::Format::Undefined;
            std::memset(slot.swizzle, 0, sizeof slot.swizzle);
          }
          slot.vertex = 0;
          textures_id = builder.Textures(set);
          ++stats_.tex_miss;
          if (options_.caches && !found) tex_map_.Emplace(hash, uint32_t(tex_keys_.size())), tex_keys_.push_back(key), tex_ids_.push_back(textures_id);
        }
        rdr::DrawOp op = base;
        op.textures = textures_id;
        ph_.Lap(kTexSet);
        op.vs_constants = vs_constants;
        op.fs_constants = fs_constants;
        op.vertex[0] = {rdr::kFrameGeometry, s.geometry_offset[k]};
        op.index = {body.meshes[k].index, 0};
        op.count = body.meshes[k].index_count;
        if (!options_.hide_ours) builder.Draw(op);
        if (options_.fault_double_mesh && !options_.hide_ours) builder.Draw(op);
        ph_.Lap(kDraw);
      }
      ++drawn;
      cards += info[t.objects[s.members[m]].asset].card;
    }
  }
  if (!said_first_) {  // once: what the dropped draw was and what we drew (the first thing to read when a picture is wrong)
    said_first_ = true;
    uint32_t classes = 0, meshes = 0;
    for (uint32_t cls = 0; cls < kClassCount; ++cls) classes += !scratch_[cls].members.empty(), meshes += uint32_t(bodies_[cls].meshes.size());
    rdr::Log("[rdr][crowd] first frame: dropped draw %s + %s; %u members in %u classes, %u skipped; %u meshes loaded; world field at %u, view-proj at %u, block %u bytes",
             in.vs >= 0 ? kVariants[in.vs].name : "?", in.ps >= 0 ? kVariants[in.ps].name : "(none)", drawn, classes, skipped, meshes, world_field->offset, vp_field->offset, block_bytes);
  }
  stats_.skipped_members += skipped;
  stats_.unposed += unposed_kept;
  if (options_.floor) {  // S101 lane 82: the floor census (per drawn frame: drawn + kept == the floor stream draws)
    stats_.floor_stream += floor_stream_draws;
    for (const ClassScratch& sc : scratch_)
      for (uint8_t f : sc.is_floor) stats_.floor_drawn += f;
    for (uint32_t index : kept) stats_.floor_kept += IsFloorObject(t, t.objects[index]);
  }
  ph_members = drawn;
  if (drawn) {
    ++stats_.frames;
    stats_.members += drawn;
    stats_.cards += cards;
  }
  mt_.full_clean = member_on && !thin && keep_draws_.empty() && kept.empty() && !mt_.nonuniform;   // lane 272: a full frame the gate may count (nothing left to the stream, every class uniform)
  return drawn;
}

// S101 lane CD: the far cards, called by the binder at the frame's first card draw (docs\renderer\CROWD_CARDS.md). The plan (crowd_cards_core) pairs the record's cards with the
// stream's card draws by world, joins each type's picture from the frame's bake passes, and says which stream draws stay; the drawer records one instanced draw per type.
namespace {
enum CPh : size_t { kCSetup, kCPlan, kCAfter, kCDraw, kCTail };
}
const char* const CrowdOurs::kCardPhaseNames[] = {"setup", "BuildPlan", "stats/after", "cards Draw", "tail"};
const size_t CrowdOurs::kCardPhaseCount = sizeof kCardPhaseNames / sizeof kCardPhaseNames[0];

uint32_t CrowdOurs::InjectCards(rdr_adapter::CrowdInject& in) {
  keep_cards_.clear();
  cph_.Begin();
  struct PhEnd {
    PhaseClock& c;
    ~PhEnd() { c.End(kCTail); }
  } cph_end{cph_};
  ++stats_.card_frames;
  const std::shared_ptr<const SceneTables> rec = frame_record_;
  // lane 175 (review B1 / S3): THIN is a positive signal, not a guess from counts: the hook's tally for this frame says how many card draws the game recorded and how many it dropped. Unknown = not thin.
  uint32_t thin_seen = 0, thin_dropped = 0;
  uint64_t thin_world = 0;
  bool thin_signal = false;
  if (options_.cards_thin && options_.thin_counts && in.frame) {
    const uint64_t fno = in.frame->header.guest_frame;
    if (fno >= options_.thin_offset && options_.thin_counts(fno - options_.thin_offset, thin_seen, thin_dropped, thin_world)) thin_signal = thin_dropped > 0;
  }
  if (!rec || !in.frame || !in.draw) {
    if (thin_signal) {   // the game dropped cards from this frame and there is no record to draw them from: a hole
      in.card_result = 4;
      ++stats_.card_thin_holes;
    } else if (options_.cards_thin) {
      in.card_result = 2;
    }
    return ++stats_.card_no_record, ++stats_.card_fallback, 0;
  }
  if (!cards_drawer_) {
    CardsDrawer::Options o;
    o.uv_swapped = options_.fault_cards_uv_swapped, o.copy_swap_lost = options_.fault_cards_copy_swap_lost, o.no_alpha_test = options_.fault_cards_no_alpha_test;
    cards_drawer_ = std::make_unique<CardsDrawer>(renderer_, o);
  }
  cards::Input input;
  input.frame = in.frame;
  input.record = rec.get();
  input.bake_vs_hashes = &HashesOfVariant("yAudDefault_skinned_lit_lm_vertex_vertex");
  input.density = in.density;
  input.page_slot = (in.vs >= 0 && kVariants[in.vs].name && std::strcmp(kVariants[in.vs].name, "yAudience_lm_vertex_vertex") == 0) ? 16 : 13;   // lane 219: the page is read in vertex slot 16 by the lm_vertex stage, in slot 13 by lm_uv
  input.rank = in.rank;
  input.fault = options_.cards_fault;
  input.read_geometry = in.read_geometry;
  input.enforce = !options_.cards_unchecked;
  input.verify = true;  // the proof is cheap (one hash per card): every frame, offline and live
  input.thin = options_.cards_thin;  // lane 175
  input.thin_seen = thin_seen, input.thin_dropped = thin_dropped, input.thin_world = thin_world;
  cph_.Lap(kCSetup);
  cards::Plan& plan = card_plan_;   // lane 270: reused (its arrays keep their memory)
  cards::BuildPlan(input, plan);
  cph_.Lap(kCPlan);
  in.card_result = plan.thin_hole ? 4 : plan.thin ? 3 : plan.usable ? 1 : (thin_signal ? 4 : 2);  // lane 175
  // SF1 backstop: with the thin switch on, a refused frame that holds no more card draws than types and has record cards without a stream draw has probably lost cards (the tally was unknown or evicted:
  // a wrong stamp offset): a hole whatever the tally says, so a wrong offset latches after 3 instead of flickering missing far cards for the whole session.
  if (options_.cards_thin && !plan.usable && !plan.thin_hole && plan.unpaired_record > 0 && plan.stream_cards <= cards::kTypes) in.card_result = 4;
  if (options_.thin_probe && options_.thin_counts && plan.usable && !plan.thin && in.frame) {   // SF2: which stamp offset matches the full frame the game recorded (census only)
    ++stats_.thin_probe_frames;
    for (uint32_t k = 0; k < 3; ++k) {
      uint32_t s = 0, d = 0;
      uint64_t w = 0;
      const uint64_t fno = in.frame->header.guest_frame;
      if (fno >= k && options_.thin_counts(fno - k, s, d, w) && s == plan.stream_cards && w == plan.record_world_sum) ++stats_.thin_probe_match[k];
    }
    if (stats_.thin_probe_frames % 600 == 0)
      rdr::Log("[rdr][crowd] card thin stamp: offset 0 / 1 / 2 match %llu / %llu / %llu of %llu full frames", static_cast<unsigned long long>(stats_.thin_probe_match[0]),
               static_cast<unsigned long long>(stats_.thin_probe_match[1]), static_cast<unsigned long long>(stats_.thin_probe_match[2]), static_cast<unsigned long long>(stats_.thin_probe_frames));
  }
  if (options_.cards_thin && plan.usable && !plan.thin && plan.record_order_differs) in.card_result = 2;   // review S5: a full proof frame whose record order differs from the stream's disproves the thin order rule
  if (in.card_result == 4 && !plan.thin_hole) ++stats_.card_thin_holes;
  if (plan.thin_hole) {
    ++stats_.card_thin_holes;
    if (++card_thin_logged_ <= 5) rdr::Log("[rdr][crowd] WARN cards: a THIN frame ours cannot draw (%s; record cards %u, kept stream cards %u): its dropped cards are missing on this frame", plan.thin_why, plan.record_cards, plan.stream_cards);
  }
  if (plan.thin) ++stats_.card_thin_frames, stats_.card_thin_cards += plan.drawn, stats_.card_thin_dropped += plan.thin_dropped;
  stats_.card_record += plan.record_cards, stats_.card_stream += plan.stream_cards, stats_.card_unaccounted += plan.unaccounted, stats_.card_unpaired_record += plan.unpaired_record;
  (plan.bakes_scanned ? (plan.bake_count ? ++stats_.card_bake_frames : ++stats_.card_nobake_frames) : 0), stats_.card_joined_stream += plan.joined_by_stream, stats_.card_joined_position += plan.joined_by_position, stats_.card_unjoined_types += plan.unjoined_types, stats_.card_unjoined_cards += plan.unjoined_cards, stats_.card_mismatch += plan.picture_mismatch, stats_.card_inversions += plan.inversions, stats_.card_record_order += plan.record_order_differs;
  if (!plan.usable) {
    ++stats_.card_fallback;
    ++stats_.card_why[std::string(plan.why) + (*plan.state_why ? std::string(": ") + plan.state_why : std::string())];   // lane 219
    if (std::strcmp(plan.why, "no cards in the record") == 0) ++stats_.card_no_cards;
    else if (plan.bad_pairing && std::strcmp(plan.why, "bad pairing") == 0) ++stats_.card_bad_pairing;
    else if (std::strcmp(plan.why, "bad state") == 0) ++stats_.card_bad_state;
    else if (std::strcmp(plan.why, "bad order") == 0) ++stats_.card_bad_order;
    else if (std::strcmp(plan.why, "bad picture") == 0) ++stats_.card_bad_picture;
    else ++stats_.card_other;
    if ((plan.bad_pairing || plan.bad_order || plan.bad_picture || plan.bad_state) && ++card_bad_logged_ <= 5)
      rdr::Log("[rdr][crowd] WARN cards: %s on this frame (%s; record cards %u, stream card draws %u, unpaired record %u, unaccounted stream %u, order inversions %u, picture mismatches %u): every card of it stays the stream's", plan.why,
               plan.state_why, plan.record_cards, plan.stream_cards, plan.unpaired_record, plan.unaccounted, plan.inversions, plan.picture_mismatch);
    return 0;
  }
  cph_.Lap(kCAfter);
  const uint32_t drawn = cards_drawer_->Draw(in, plan);
  cph_.Lap(kCDraw);
  cph_.Count(drawn);
  if (!drawn) {
    if (options_.cards_thin && in.card_result != 4) in.card_result = plan.thin ? 4 : 2;   // review S4: a Draw that drew nothing is not a clean frame (a thin one is a hole)
    if (plan.thin) ++stats_.card_thin_holes;
    ++stats_.card_why[cards_drawer_->refusal().empty() ? std::string("drawer: drew nothing") : cards_drawer_->refusal()];   // lane 219: every drawer refusal names itself in the census
    return ++stats_.card_fallback, ++stats_.card_other, 0;
  }
  keep_cards_ = plan.keep;
  stats_.card_drawn += drawn, stats_.card_kept += plan.keep.size(), stats_.card_density += plan.density_skipped, stats_.card_holes += plan.holes;
  if (!said_cards_) {
    said_cards_ = true;
    rdr::Log("[rdr][crowd] cards: first frame: %u cards in %zu instanced draws (record cards %u, stream card draws %u, kept %zu)", drawn, plan.runs.size(), plan.record_cards, plan.stream_cards, plan.keep.size());
  }
  return drawn;
}

std::string CrowdOurs::FloorDetail() const {   // lane 156: the floor text of Report(), alone, for the window census line
  if (!options_.floor) return {};
  char f[640];
  std::snprintf(f, sizeof f,
                ", floor members drawn %llu, floor draws kept %llu of %llu floor stream draws, floor (class, LOD 0) checks: %llu of %llu equal the stream's triangles + vertices and %llu match its posed blob, %llu of %llu members use the slot-0 picture of their own stream draw",
                static_cast<unsigned long long>(stats_.floor_drawn), static_cast<unsigned long long>(stats_.floor_kept), static_cast<unsigned long long>(stats_.floor_stream),
                static_cast<unsigned long long>(stats_.floor_class_geom_ok), static_cast<unsigned long long>(stats_.floor_class_total), static_cast<unsigned long long>(stats_.floor_class_pose_ok),
                static_cast<unsigned long long>(stats_.material_ok), static_cast<unsigned long long>(stats_.material_total));
  return f;
}

// lane 295: the skip arm's one summary line, in the style of the [rdr][d3] lines (taken / refused by reason / verify). Printed every 600 judged frames by Inject and once more with the crowd report at the end.
std::string CrowdOurs::MemberSkipLine() const {
    namespace mth = wwe2k14::member_thin;
    const mth::Counters& hc = mth::g_counters;
    const auto L = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
    const auto K = [&](mth::Reason r) { return L(hc.kept_why[0][r]) + L(hc.kept_why[1][r]); };
    char k[1400];
    std::snprintf(k, sizeof k,
                  "[rdr][crowd][member skip] mode %s: taken (thin frames drawn from the record) %llu, members in them %llu, the game's dropped draws %llu; refused: holes %llu, full frames refused %llu (clean %llu), judge/body self-check disagreements %llu; "
                  "hook kept in full because: stamp unproven %llu, not proven %llu, latched %llu, tiled %llu, no palette %llu, unknown class %llu (proof runs %llu, first of run %llu); "
                  "stamp: offset 1 only %llu, ambiguous %llu, miss %llu, none (thin ok) %llu, streak %u, proven %s, resets %llu; "
                  "verify: frames %llu, members %llu, mismatching %llu (material %llu, bank %llu), unsourced %llu, non-uniform frames %llu, calls = records checked %llu differ %llu",
                  options_.members_thin ? "skip" : "census", static_cast<unsigned long long>(stats_.member_thin_frames), static_cast<unsigned long long>(stats_.member_thin_members), static_cast<unsigned long long>(stats_.member_thin_dropped),
                  static_cast<unsigned long long>(stats_.member_thin_holes), static_cast<unsigned long long>(stats_.member_full_refused), static_cast<unsigned long long>(stats_.member_full_clean), static_cast<unsigned long long>(stats_.member_judge_disagrees),
                  K(mth::kStampUnproven), K(mth::kNotProven), K(mth::kLatched), K(mth::kTiled), K(mth::kNoPalette), K(mth::kUnknownClass), K(mth::kProofRun), K(mth::kFirstOfRun),
                  static_cast<unsigned long long>(stats_.member_stamp_evidence[1]), static_cast<unsigned long long>(stats_.member_stamp_evidence[2]), static_cast<unsigned long long>(stats_.member_stamp_evidence[3]), static_cast<unsigned long long>(stats_.member_stamp_evidence[0]),
                  mth::g_stamp.streak.load(std::memory_order_relaxed), mth::g_stamp.Proven(mth::NowMs()) ? "yes" : "no", L(mth::g_stamp.resets),
                  static_cast<unsigned long long>(stats_.member_va.frames), static_cast<unsigned long long>(stats_.member_va.members), static_cast<unsigned long long>(stats_.member_va.mismatching()), static_cast<unsigned long long>(stats_.member_va.material_differs),
                  static_cast<unsigned long long>(stats_.member_va.bank_differs), static_cast<unsigned long long>(stats_.member_va.unsourced), static_cast<unsigned long long>(stats_.member_nonuniform_frames),
                  static_cast<unsigned long long>(stats_.member_va.calls_checked), static_cast<unsigned long long>(stats_.member_va.calls_differ));
    return std::string(k);
}

std::string CrowdOurs::Report() const {
  char line[640];
  std::snprintf(line, sizeof line,
                "crowd ours: %llu frames, %llu members (%llu cards), %llu unposed, %llu skipped, %llu not ready, %llu unsupported effect, %llu arena loads, %llu members on another program than the first draw, pairing %llu of %llu worlds match the stream (%llu stream crowd draws dropped), %llu frames with a bad pairing, %llu of %llu (class, LOD) checks: triangles + vertices equal the stream's, and %llu posed vertices match the stream's blob",
                static_cast<unsigned long long>(stats_.frames), static_cast<unsigned long long>(stats_.members), static_cast<unsigned long long>(stats_.cards),
                static_cast<unsigned long long>(stats_.unposed), static_cast<unsigned long long>(stats_.skipped_members), static_cast<unsigned long long>(stats_.not_ready),
                static_cast<unsigned long long>(stats_.unsupported_variant), static_cast<unsigned long long>(stats_.arena_loads), static_cast<unsigned long long>(stats_.other_program), static_cast<unsigned long long>(stats_.pair_match),
                static_cast<unsigned long long>(stats_.pair_total), static_cast<unsigned long long>(stats_.stream_draws), static_cast<unsigned long long>(stats_.pair_bad_frames), static_cast<unsigned long long>(stats_.class_geom_ok), static_cast<unsigned long long>(stats_.class_total), static_cast<unsigned long long>(stats_.class_pose_ok));
  std::string text = line;
  if (ph_.on()) {
    char c[400];
    std::snprintf(c, sizeof c, "\n[rdr][crowd][phase] caches: textures %llu hit / %llu miss (member meshes); checks: asset parse %llu assets %llu mismatches, 4-wide pose %llu meshes %llu mismatches", static_cast<unsigned long long>(stats_.tex_hit), static_cast<unsigned long long>(stats_.tex_miss), static_cast<unsigned long long>(stats_.parse_checked),
                  static_cast<unsigned long long>(stats_.parse_mismatch), static_cast<unsigned long long>(stats_.pose_checked), static_cast<unsigned long long>(stats_.pose_mismatch));
    text += c;
  }
  if (ph_.on()) text += "\n" + ph_.Report("crowd members Inject", "members") + "\n" + cph_.Report("InjectCards", "cards") + "\n" + cards::PhaseReport();  // lane 270
  if (options_.floor) {  // S101 lane 82: the seated floor people (site 0x8276ED60); only with the floor switch, so the switch-off report is unchanged
    char f[640];
    std::snprintf(f, sizeof f,
                  ", floor members drawn %llu, floor draws kept %llu of %llu floor stream draws, floor (class, LOD 0) checks: %llu of %llu equal the stream's triangles + vertices and %llu match its posed blob, %llu of %llu members use the slot-0 picture of their own stream draw",
                  static_cast<unsigned long long>(stats_.floor_drawn), static_cast<unsigned long long>(stats_.floor_kept), static_cast<unsigned long long>(stats_.floor_stream),
                  static_cast<unsigned long long>(stats_.floor_class_geom_ok), static_cast<unsigned long long>(stats_.floor_class_total), static_cast<unsigned long long>(stats_.floor_class_pose_ok),
                  static_cast<unsigned long long>(stats_.material_ok), static_cast<unsigned long long>(stats_.material_total));
    text += f;
  }
  if (InjectsCards()) {  // S101 lane CD: only with the cards switch, so the switch-off report is unchanged
    const CardsDrawer::Stats ds = cards_drawer_ ? cards_drawer_->stats() : CardsDrawer::Stats{};
    char c[900];
    std::snprintf(c, sizeof c,
                  ", cards: frames asked %llu, ours %llu, fallback %llu (no record %llu, no cards %llu, bad pairing %llu, bad order %llu, bad picture %llu, bad state %llu, other %llu); record cards %llu, stream card draws %llu, cards drawn %llu in %llu instanced draws, "
                  "kept %llu (unaccounted %llu, unjoined type %llu types / %llu cards, picture mismatch %llu), types joined by bake position %llu, density left out %llu, record cards without a stream draw %llu, order inversions %llu, types with the record in another order than the stream %llu, holes %llu, alpha-test twin built in %llu of %llu draws that ask for it (%llu draws)",
                  static_cast<unsigned long long>(stats_.card_frames), static_cast<unsigned long long>(stats_.card_frames - stats_.card_fallback), static_cast<unsigned long long>(stats_.card_fallback),
                  static_cast<unsigned long long>(stats_.card_no_record), static_cast<unsigned long long>(stats_.card_no_cards), static_cast<unsigned long long>(stats_.card_bad_pairing),
                  static_cast<unsigned long long>(stats_.card_bad_order), static_cast<unsigned long long>(stats_.card_bad_picture), static_cast<unsigned long long>(stats_.card_bad_state), static_cast<unsigned long long>(stats_.card_other),
                  static_cast<unsigned long long>(stats_.card_record), static_cast<unsigned long long>(stats_.card_stream), static_cast<unsigned long long>(stats_.card_drawn), static_cast<unsigned long long>(ds.draws),
                  static_cast<unsigned long long>(stats_.card_kept), static_cast<unsigned long long>(stats_.card_unaccounted), static_cast<unsigned long long>(stats_.card_unjoined_types),
                  static_cast<unsigned long long>(stats_.card_unjoined_cards), static_cast<unsigned long long>(stats_.card_mismatch), static_cast<unsigned long long>(stats_.card_joined_position), static_cast<unsigned long long>(stats_.card_density),
                  static_cast<unsigned long long>(stats_.card_unpaired_record), static_cast<unsigned long long>(stats_.card_inversions), static_cast<unsigned long long>(stats_.card_record_order), static_cast<unsigned long long>(stats_.card_holes),
                  static_cast<unsigned long long>(ds.alpha_twin), static_cast<unsigned long long>(ds.alpha_wanted), static_cast<unsigned long long>(ds.draws));
    text += c;
    text += "; types whose picture is the stream's own (no bake join) " + std::to_string(stats_.card_joined_stream) + "; frames with a bake " + std::to_string(stats_.card_bake_frames) + ", without " + std::to_string(stats_.card_nobake_frames);
    for (const auto& w : stats_.card_why) text += "; cards fallback " + std::to_string(w.second) + " x [" + w.first + "]";   // lane 219: why each fallback frame fell back
    if (options_.cards_thin) {  // lane 175: only with the thin switch, so the switch-off report is unchanged
      char t[320];
      std::snprintf(t, sizeof t, ", THIN cards: frames %llu, cards drawn in them %llu, the game's dropped card draws %llu, holes (thin frames ours could not draw) %llu",
                    static_cast<unsigned long long>(stats_.card_thin_frames), static_cast<unsigned long long>(stats_.card_thin_cards), static_cast<unsigned long long>(stats_.card_thin_dropped),
                    static_cast<unsigned long long>(stats_.card_thin_holes));
      text += t;
    }
  }
  if (options_.member_probe || options_.members_thin) {   // lane 272: only with the member switches, so the switch-off report is unchanged
    char m[900];
    std::snprintf(m, sizeof m,
                  ", THIN members: frames %llu, members in them %llu, the game's dropped member draws %llu, holes (thin frames ours could not draw) %llu; full frames clean %llu, refused %llu; verify: members %llu, material differs %llu, bank differs %llu, "
                  "frames with a non-uniform class %llu",
                  static_cast<unsigned long long>(stats_.member_thin_frames), static_cast<unsigned long long>(stats_.member_thin_members), static_cast<unsigned long long>(stats_.member_thin_dropped),
                  static_cast<unsigned long long>(stats_.member_thin_holes), static_cast<unsigned long long>(stats_.member_full_clean), static_cast<unsigned long long>(stats_.member_full_refused),
                  static_cast<unsigned long long>(stats_.member_va.members), static_cast<unsigned long long>(stats_.member_va.material_differs), static_cast<unsigned long long>(stats_.member_va.bank_differs),
                  static_cast<unsigned long long>(stats_.member_nonuniform_frames));
    text += m;
    if (options_.member_counts) text += ", stamp probe: offset 0 / 1 / 2 match " + std::to_string(stats_.member_probe_match[0]) + " / " + std::to_string(stats_.member_probe_match[1]) + " / " + std::to_string(stats_.member_probe_match[2]) + " of " + std::to_string(stats_.member_probe_frames) + " full frames";
    for (const auto& w : stats_.member_why) text += "; members hole " + std::to_string(w.second) + " x [" + w.first + "]";
    if (options_.member_counts) text += "\n" + MemberSkipLine();   // lane 295: the skip arm's summary (SKIP_GAME_WORK_MAP 20.12); also logged every 600 judged frames by Inject
  }
  return text;
}

}  // namespace rdr_scene
