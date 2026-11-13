// See crowd_body.h. Session 101, lane 27.
#include "crowd_body.h"

// S101 lane 35: stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it): the game exe and the phone library must not export stbi_* (the SDK's own
// copies would be rebound by symbol interposition on Android), and the viewer's loader seam keeps its own non-static one. Only this file uses stb here (pac.h, stage_media.h).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"  // (main checkout layout; the build gives the folder with -I)
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

#include "asset_load.h"

namespace assets::crowd_body {
namespace {
launcher::GameFiles g_files;
bool g_ready = false;

struct Stored {
  Model model;                 // the arena with its crowd at the stored pose (all meshes; only the crowd's are used)
  std::vector<Member> members;
};
std::map<int, std::unique_ptr<Stored>> g_stored;

Stored* GetStored(int arena, std::string& err) {
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());  // S101 lane 40: one loader at a time (asset_load.h is not thread-safe; the crowd and the arena load on workers)
  if (!g_ready) return err = "crowd_body: no game files (Init first)", nullptr;
  if (auto it = g_stored.find(arena); it != g_stored.end()) return it->second.get();
  auto s = std::make_unique<Stored>();
  const auto saved = load_detail::CrowdPose();
  load_detail::CrowdPose() = load_detail::CrowdPoseMode::kStored;
  const bool ok = LoadArena(g_files, arena, s->model, err);
  load_detail::CrowdPose() = saved;
  if (!ok) return nullptr;
  for (const Placement& pl : s->model.placements) {
    int type = -1;
    if (std::sscanf(pl.label.c_str(), "crowd aud_%02d", &type) != 1 || type < 0 || type > 19) continue;
    for (const Matrix& t : pl.transforms) {
      Member m;
      m.type = type;
      m.file_world = MirrorY(t);  // (the viewer's placements are MirrorY of the file-space matrix; [15] is the stands tag)
      m.stands = t[15] > 1.5f;
      m.file_world[15] = 1.f;
      s->members.push_back(m);
    }
  }
  return (g_stored[arena] = std::move(s)).get();
}
}  // namespace

bool Init(const std::string& game_path) {
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  g_stored.clear();
  return g_ready;
}

bool Members(int arena, std::vector<Member>& out, std::string& err) {
  Stored* s = GetStored(arena, err);
  if (!s) return false;
  out = s->members;
  return true;
}

bool LoadBody(int arena, int type, Model& out, std::string& err) {
  Stored* s = GetStored(arena, err);
  if (!s) return false;
  const Model& src = s->model;
  out = Model();
  out.label = "crowd body " + std::to_string(type) + " of " + src.label;
  out.light_sets = src.light_sets;
  out.lm_scale[0] = src.lm_scale[0], out.lm_scale[1] = src.lm_scale[1], out.lm_gain = src.lm_gain;
  out.materials = src.materials;
  // The type's meshes (src_kind 3 = crowd body, src_key = the entry).
  for (const Mesh& me : src.meshes) {
    if (me.src_kind != 3 || me.src_key != 20000 + type) continue;
    Mesh c = me;
    c.placement = -1;
    out.meshes.push_back(std::move(c));
  }
  if (out.meshes.empty()) return err = "crowd_body: arena " + std::to_string(arena) + " has no body of type " + std::to_string(type), false;
  // Texture pruning: only what the kept meshes' materials (and the stage lightmap) use; the other materials lose their textures.
  std::vector<char> keep_mat(out.materials.size(), 0);
  for (const Mesh& me : out.meshes)
    if (me.material >= 0 && size_t(me.material) < keep_mat.size()) keep_mat[size_t(me.material)] = 1;
  std::map<int, int> remap;
  auto map_tex = [&](int& idx) {
    if (idx < 0 || size_t(idx) >= src.textures.size()) { idx = -1; return; }
    auto it = remap.find(idx);
    if (it == remap.end()) {
      out.textures.push_back(src.textures[size_t(idx)]);
      it = remap.emplace(idx, int(out.textures.size()) - 1).first;
    }
    idx = it->second;
  };
  for (size_t i = 0; i < out.materials.size(); ++i) {
    Material& m = out.materials[i];
    if (!keep_mat[i]) { m.diffuse = m.normal = m.lightmap = m.mask = m.specular = m.detail = -1; continue; }
    map_tex(m.diffuse), map_tex(m.normal), map_tex(m.lightmap), map_tex(m.mask), map_tex(m.specular), map_tex(m.detail);
  }
  out.stage_lightmap = src.stage_lightmap;
  map_tex(out.stage_lightmap);
  return true;
}

bool LoadBodyGameSpace(int arena, int type, Model& out, std::string& err) {
  if (!LoadBody(arena, type, out, err)) return false;
  for (Mesh& me : out.meshes)
    for (Vertex& v : me.vertices) {
      v.pos[1] = -v.pos[1];
      v.normal[1] = -v.normal[1];
      v.tangent[1] = -v.tangent[1];
    }
  return true;
}

bool LoadBodyLodGameSpace(int arena, int lod, int type, Model& out, std::string& err) {
  using namespace load_detail;
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());  // S101 lane 40: one loader at a time (asset_load.h is not thread-safe; the crowd and the arena load on workers)
  if (lod == 0) return LoadBodyGameSpace(arena, type, out, err);
  if (!g_ready) return err = "crowd_body: no game files (Init first)", false;
  if (lod < 0 || lod > 2 || type < 0 || type > 19) return err = "crowd_body: bad lod / type", false;
  const pac::Pac* p = OpenCached(g_files, ArenaRel(arena), err);
  if (!p) return false;
  const pac::Pach& h = p->pachs[0];
  Bytes sb;
  pac::Pach sp;
  if (!pac::GetNested(p->bytes, h, 20000, sb, sp)) return err = "crowd_body: the arena has no crowd set (entry 20000)", false;
  TexSet tex;
  if (const pac::Ent* e = pac::Find(sp, 21000)) tex.AddDir(pac::Decode(sb.data() + e->off, e->size));
  const uint32_t entry = 20000u + 40u * uint32_t(lod) + uint32_t(type);
  const Bytes blob = pac::Get(sb, sp, entry);
  if (!IsJboy(blob)) return err = "crowd_body: no body at entry " + std::to_string(entry), false;
  out = Model();
  out.label = "crowd body " + std::to_string(type) + " lod " + std::to_string(lod);
  JboyOptions o;
  o.arena = true;
  o.part = "crowd " + std::to_string(type);
  o.kind = 3, o.key = int(entry);
  o.tex = &tex;
  std::vector<Bone> nodes;
  o.own_nodes = &nodes;  // (no FinishModel: the vertices stay in the file's space)
  if (!LoadJboy(blob, o, out, err)) return false;
  char tn[16];
  std::snprintf(tn, sizeof tn, "aud_%02da", type);
  const int ti = tex.Get(out, tn);
  for (Mesh& me : out.meshes)
    if (ti >= 0 && me.material >= 0) out.materials[size_t(me.material)].diffuse = ti;
  return !out.meshes.empty() ? true : (err = "crowd_body: entry " + std::to_string(entry) + " has no mesh", false);
}

void Release(int arena) {
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());  // S101 lane 40: one loader at a time (asset_load.h is not thread-safe; the crowd and the arena load on workers)
  g_stored.erase(arena);
  load_detail::TheCache() = load_detail::Cache{};  // the arena pac the loader kept open (~8 MB) goes too
}

bool Palettes(int arena, int act, int frame, std::vector<std::vector<Matrix>>& out, std::string& err) {
  using namespace load_detail;
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());  // S101 lane 40: one loader at a time (asset_load.h is not thread-safe; the crowd and the arena load on workers)
  if (!g_ready) return err = "crowd_body: no game files (Init first)", false;
  const pac::Pac* p = OpenCached(g_files, ArenaRel(arena), err);
  if (!p) return false;
  const pac::Pach& h = p->pachs[0];
  const Bytes& buf = p->bytes;
  Bytes sb;
  pac::Pach sp;
  if (!pac::GetNested(buf, h, 20000, sb, sp)) return err = "crowd_body: the arena has no crowd set (entry 20000)", false;
  TexSet tex;
  if (const pac::Ent* e = pac::Find(sp, 21000)) tex.AddDir(pac::Decode(sb.data() + e->off, e->size));
  if (!crowdmot::TheM2().Load(g_files, err)) return false;
  out.assign(20, {});
  int got = 0;
  for (int type = 0; type < 20; ++type) {
    const Bytes blob = pac::Get(sb, sp, 20000u + uint32_t(type));
    if (!IsJboy(blob)) continue;
    Model scratch;
    JboyOptions o;
    o.arena = true;
    o.part = "crowd " + std::to_string(type);
    o.kind = 3, o.key = 20000 + type;
    o.tex = &tex;
    std::vector<Bone> nodes;
    o.own_nodes = &nodes;
    std::string e2;
    if (!LoadJboy(blob, o, scratch, e2)) continue;
    const crowdmot::Keys& ks = crowdmot::TheM2().KeysFor(act, type);
    const int len = crowdmot::Length(ks);
    const double fr = len > 0 ? (frame >= 0 ? frame : 7 * type) % len : 0;
    std::vector<Matrix> local;
    if (!crowdmot::Pose(ks, fr, nodes, local) || local.size() != nodes.size() || nodes.empty() || nodes.size() > 22) continue;
    // The same skin matrices as PoseCrowdBody: inverse(bind_world) * pose_world per node (file space).
    const size_t n = nodes.size();
    std::vector<Matrix> bind(n), pose(n);
    out[size_t(type)].resize(n);
    for (size_t i = 0; i < n; ++i) {
      const Bone& b = nodes[i];
      const Matrix lb = FromQuatT(b.local_rotation, b.local_translation);
      const bool hp = b.parent >= 0 && size_t(b.parent) < i;
      bind[i] = hp ? Mul(lb, bind[size_t(b.parent)]) : lb;
      pose[i] = hp ? Mul(local[i], pose[size_t(b.parent)]) : local[i];
      out[size_t(type)][i] = Mul(InverseRigid(bind[i]), pose[i]);
    }
    ++got;
  }
  if (got == 0) return err = "crowd_body: no person type could be posed", false;
  return true;
}

void PoseBody(const Model& stored, const float* palette, size_t count, Model& out) {
  out = stored;
  std::vector<Matrix> sk(count);
  for (size_t i = 0; i < count; ++i) {
    for (size_t k = 0; k < 16; ++k) sk[i][k] = palette[i * 16 + k];

  }
  for (Mesh& me : out.meshes)
    for (Vertex& v : me.vertices) {
      float p[3] = {0, 0, 0}, n[3] = {0, 0, 0};
      float wsum = 0;
      for (int j = 0; j < 4; ++j) {
        const float w = v.weights[j];
        if (!(w > 0.f) || v.bones[j] >= count) continue;
        const Matrix& t = sk[v.bones[j]];
        for (size_t c = 0; c < 3; ++c) {
          p[c] += w * (v.pos[0] * t[c] + v.pos[1] * t[4 + c] + v.pos[2] * t[8 + c] + t[12 + c]);
          n[c] += w * (v.normal[0] * t[c] + v.normal[1] * t[4 + c] + v.normal[2] * t[8 + c]);
        }
        wsum += w;
      }
      if (wsum > 0.f)
        for (size_t c = 0; c < 3; ++c) v.pos[c] = p[c] / wsum, v.normal[c] = n[c] / wsum;
    }
}

}  // namespace assets::crowd_body
