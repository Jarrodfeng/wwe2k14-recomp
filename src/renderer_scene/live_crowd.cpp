// The crowd from the scene record alone: see live_crowd.h.
#include "live_crowd.h"
#include "rows_crowd.h"   // S106 G4-2
#include "rows_pictures.h"
#include "rows_stage_look.h"
#include "../assets/car_sel.h"   // S103 CAR fix 5
#include "../keep_stream.h"  // S103 GT: WWE2K14_STREAM()
#include "../thread_name.h"  // S103 H1

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../assets/crowd_body.h"
#include "../renderer_adapter/crowd_rank.h"   // S103 SPB: the density rank (pure; the stream binder uses the same tracker)
#include "../start_settings.h"                  // S103 SPB: PlayerQuality
#include "crowd_density.h"                       // S103 SPBF M1: the density decision (cards too)
#include "crowd_fast.h"                          // S103 CPU1: fast::ParseInt
#include "../assets/crowd_prop_game_space.h"   // S103 D4: the audience prop set
#include "crowd_prop_rows.h"                    // S103 D4: KeyOf
#include "../scene_live/sign_file_keys.h"        // S103 CSF: the published sign file keys
#include "crowd_page.h"                          // S103 B4F: the stage page of the crowd models
#include "crowd_picture_cache.h"                 // S103 D6
#include "card_atlas_rule.h"                     // S103 CB1: the one-pass card atlas
#include "sampler_table.h"                       // S103 CB1: AnisoWanted (the atlas is used at 1x only)

namespace rdr_scene::live {
static LightsFn g_lights_fn = nullptr;   // S103 L4
static LightsHasFn g_lights_has = nullptr;
void SetLightsFn(LightsFn fn, LightsHasFn has) { g_lights_fn = fn, g_lights_has = has; }
namespace {

constexpr uint32_t kTypes = 20;
constexpr uint32_t kLods = 3;
constexpr uint32_t kClasses = kLods * kTypes;
constexpr float kBodyScale = 10.0f;  // the loader's vertices are 1/10 of the game's skinned blob (S101 lane 36, bit-exact); normals are not scaled
constexpr uint32_t kCardPictureSize = 64;
constexpr uint32_t kNoPalette = 0xFFFFFFFFu;

struct StoredVertex {
  float pos[3], normal[3], uv[2];
  uint16_t bones[4];
  float weights[4];
};

// One body class: a (LOD group, person type) body of an arena as a front-end model, and the stored vertices its pose is made from.
struct BodyClass {
  bool ok = false, failed = false;
  std::string why;
  int model = -1;
  std::vector<std::vector<StoredVertex>> meshes;  // per kept mesh, in the model's own order (FrontEnd::AddModel's `src`)
  uint32_t nodes = 0;   // the body FILE's skeleton node count (assets::Model::bones; 0 = unknown): census only since S103 SN
  uint32_t slots = 0;   // S103 SN: 1 + the highest palette slot any kept vertex uses with a weight (PoseBody's index); 0 = none / unknown
  uint32_t identity_bad = 0;   // S103 SN (CENSUS ONLY, measured on bg04: every body has hundreds): used raw slots whose file palette map (Mesh::palette_nodes) is not slot -> node == slot. Not a refusal: the loader already turned each raw slot into the NODE index (Vertex::bones), and PoseBody indexes the node-ordered palette by it (the stands draw the same bodies that way with 0 bad bones)
  bool identity_known = false;   // the file gave the raw slots + the map for at least one kept mesh
};

// What a record's asset names: "pac\bg\bgNN.pac" + entry "20000 + 40 * LOD + type" (a body) or "card:TT" (a far card).
struct AssetInfo {
  bool valid = false, card = false;
  bool crowd_like = false;   // S103 D2: the entry LOOKS like a crowd body / card (valid or not): an invalid one is a hole, any other asset is another applier's
  uint32_t type = 0, lod = 0;
  int arena = -1;
};

bool ParseUInt(std::string_view s, size_t at, uint32_t& v) {
  if (at >= s.size()) return false;
  uint64_t x = 0;
  size_t i = at;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) x = x * 10 + uint64_t(s[i] - '0');
  if (i == at || i != s.size()) return false;
  v = uint32_t(x);
  return x < 0xFFFFFFFFull;
}

AssetInfo ParseAsset(const SceneTables& t, const AssetRef& a) {
  AssetInfo info;
  const std::string_view entry = t.Str(a.entry), pac = t.Str(a.pac);
  constexpr std::string_view kPac = "pac\\bg\\bg";
  if (pac.size() > kPac.size() && pac.compare(0, kPac.size(), kPac) == 0) {
    uint32_t n = 0;
    size_t i = kPac.size();
    while (i < pac.size() && pac[i] >= '0' && pac[i] <= '9') n = n * 10 + uint32_t(pac[i] - '0'), ++i;
    if (i > kPac.size()) info.arena = int(n);
  }
  uint32_t v = 0;
  {   // (record_frame.cpp IsCrowdEntry's rule.) S103 CPU1: fast::ScanInt reads what sscanf("%d") read, overflow included (crowd_fast.h) (no string copy, no sscanf: RHP.md 1.06 ms); cpu1_unit.cpp proves the two equal
    int num = 0, ty = 0;
    info.crowd_like = (entry.size() >= 5 && entry.compare(0, 5, "card:") == 0 && fast::ScanInt(entry, 5, ty)) || (fast::ScanInt(entry, 0, num) && num >= 20000 && num < 20200);
  }
  if (entry.size() > 5 && entry.compare(0, 5, "card:") == 0 && ParseUInt(entry, 5, v)) {
    info.card = true, info.type = v, info.valid = v < kTypes;
  } else if (ParseUInt(entry, 0, v) && v >= 20000 && v < 20000 + kLods * 40) {
    info.lod = (v - 20000) / 40, info.type = (v - 20000) % 40, info.valid = info.type < kTypes;
  }
  return info;
}

// The record's world (row vectors, translation in row 3) as the three rows of the game's c76-c78: row r = {m[r], m[4 + r], m[8 + r], m[12 + r]}.
void WorldRows(const Mat4& w, float* out) {
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) out[4 * r + k] = w.m[k * 4 + r];
}

// A stored body posed by a palette (`nodes` matrices of 16 floats, row vectors): the weights rule of assets::crowd_body::PoseBody. palette = null: the stored pose.
// Returns the vertex bone references that point past the palette (they add nothing; counted so a wrong palette size shows).
uint32_t PoseBody(const std::vector<StoredVertex>& stored, const float* palette, size_t nodes, std::vector<InstVertex>& out) {
  out.resize(stored.size());
  uint32_t bad = 0;
  for (size_t i = 0; i < stored.size(); ++i) {
    const StoredVertex& v = stored[i];
    InstVertex& o = out[i];
    std::memcpy(o.uv, v.uv, sizeof o.uv);
    float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, wsum = 0;
    if (palette)
      for (int j = 0; j < 4; ++j) {
        const float w = v.weights[j];
        if (!(w > 0.f)) continue;
        if (v.bones[j] >= nodes) { ++bad; continue; }
        const float* t = palette + size_t(v.bones[j]) * 16;
        for (int c = 0; c < 3; ++c) {
          p[c] += w * (v.pos[0] * t[c] + v.pos[1] * t[4 + c] + v.pos[2] * t[8 + c] + t[12 + c]);
          n[c] += w * (v.normal[0] * t[c] + v.normal[1] * t[4 + c] + v.normal[2] * t[8 + c]);
        }
        wsum += w;
      }
    if (wsum > 0.f) {
      for (int c = 0; c < 3; ++c) o.pos[c] = p[c] / wsum, o.normal[c] = n[c] / wsum;
    } else {
      std::memcpy(o.pos, v.pos, sizeof o.pos);
      std::memcpy(o.normal, v.normal, sizeof o.normal);
    }
  }
  return bad;
}

struct PoseTimer {   // the CPU pose of this frame (LiveCrowdCounts::pose_us)
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  uint32_t* us;
  explicit PoseTimer(uint32_t* us_) : us(us_) {}
  ~PoseTimer() { *us += uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()); }
};

// S103 D4: half -> float (the prop uv is stored as two halves, prop_host_vertex.h)
float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
  float v;
  if (e == 0) v = std::ldexp(float(m), -24);
  else if (e == 31) v = m ? NAN : INFINITY;
  else v = std::ldexp(float(m | 1024u), int(e) - 25);
  return sign ? -v : v;
}

// S103 D4: the arena's audience props as a model (kPropSetLod). Runs where the source loads (the loader's worker when async).
BodyStatus LoadPropSet(int arena, assets::Model& out, std::string& err) {
  const std::string root = assets::crowd_body::GameRoot();
  if (root.empty() || !assets::crowd_prop_gs::Init(root)) return err = "no game files for the audience props", BodyStatus::Failed;
  assets::crowd_prop_gs::PropSet set;
  if (!assets::crowd_prop_gs::Load(arena, set, err)) return BodyStatus::Failed;
  out = assets::Model{};
  out.label = "audience props of arena " + std::to_string(arena);
  for (const assets::crowd_prop_gs::PropMesh& pm : set.meshes) {
    assets::Mesh me;
    me.part = "crowd prop", me.src_key = int(pm.key), me.src_kind = 4;
    me.vertices.resize(pm.vertex_count);
    for (uint32_t i = 0; i < pm.vertex_count; ++i) {
      const uint8_t* b = pm.vertices.data() + size_t(i) * assets::crowd_prop_gs::kVertexBytes;
      assets::Vertex& v = me.vertices[i];
      std::memset(&v, 0, sizeof v);
      std::memcpy(v.pos, b, 12), std::memcpy(v.normal, b + 12, 12);
      uint16_t h[2];
      std::memcpy(h, b + 24, 4);
      v.uv0[0] = HalfToFloat(h[0]), v.uv0[1] = HalfToFloat(h[1]);
      v.color = 0xFFFFFFFFu, v.weights[0] = 1.f;
    }
    // the file's strip -> a triangle list (alternate winding, degenerate triangles dropped)
    for (size_t i = 0; i + 2 < pm.indices.size(); ++i) {
      uint32_t a = pm.indices[i], b = pm.indices[i + 1], c = pm.indices[i + 2];
      if (a == b || b == c || a == c) continue;
      if (i & 1) std::swap(a, b);
      me.indices.push_back(a), me.indices.push_back(b), me.indices.push_back(c);
    }
    if (!me.indices.empty()) out.meshes.push_back(std::move(me));
  }
  if (out.meshes.empty()) return err = "the audience set holds no usable prop mesh", BodyStatus::Failed;
  for (const assets::crowd_prop_gs::PropPicture& pp : set.pictures) {   // S103 FHB: the texture entries (the foam hands' prints): name = the entry id, mip 0 only
    assets::Texture t;
    t.name = std::to_string(pp.key);
    t.format = pp.format == 18 ? assets::TexFormat::kBC1 : pp.format == 19 ? assets::TexFormat::kBC2 : assets::TexFormat::kBC3;
    t.width = pp.width, t.height = pp.height, t.mips = 1, t.data = pp.bytes;
    out.textures.push_back(std::move(t));
  }
  for (const assets::crowd_prop_gs::PropPicture& pp : set.sign_pictures) {   // S103 CSF: the crowd signs' pictures (pac\audience.pac), named "S<key>": their own key space
    assets::Texture t;
    t.name = "S" + std::to_string(pp.key);
    t.format = assets::TexFormat::kBC1;   // (LoadSignPictures keeps only 64x64 DXT1)
    t.width = pp.width, t.height = pp.height, t.mips = 1, t.data = pp.bytes;
    out.textures.push_back(std::move(t));
  }
  return BodyStatus::Ready;
}

struct GameFilesSource final : BodySource {
  void ArenaChanged(int old_arena, int) override { if (old_arena >= 0) assets::crowd_body::Release(old_arena); }   // S103 D2
  BodyStatus Request(int arena, int lod, int type, assets::Model& out, std::string& err) override {
    if (lod == kPropSetLod) return LoadPropSet(arena, out, err);   // S103 D4
    if (!assets::crowd_body::LoadBodyLodGameSpace(arena, lod, type, out, err)) return BodyStatus::Failed;
    if (lod > 0) {  // the LOD bodies carry no arena materials / light sets / stage light map: they take the near body's (the pictures are the type's own aud_NNa either way)
      assets::Model near_body;
      if (!assets::crowd_body::LoadBodyGameSpace(arena, type, near_body, err)) return BodyStatus::Failed;
      if (near_body.meshes.empty()) return err = "the near body has no mesh", BodyStatus::Failed;
      const int material = near_body.meshes[0].material;
      out.materials = near_body.materials;
      out.textures = near_body.textures;
      out.light_sets = near_body.light_sets;
      out.stage_lightmap = near_body.stage_lightmap;
      out.lm_scale[0] = near_body.lm_scale[0], out.lm_scale[1] = near_body.lm_scale[1], out.lm_gain = near_body.lm_gain;
      for (assets::Mesh& me : out.meshes) me.material = material, me.light_set = near_body.meshes[0].light_set;
    }
    return BodyStatus::Ready;
  }
};

}  // namespace

BodySource& GameFilesBodies() {
  static GameFilesSource source;
  return source;
}

// ---- AsyncBodies ---------------------------------------------------------------------------------------------------------------

struct AsyncBodies::State {
  struct Entry { bool done = false; BodyStatus status = BodyStatus::Pending; assets::Model model; std::string err; };
  BodySource& inner;
  std::mutex mu;
  std::condition_variable cv;
  std::map<uint64_t, Entry> entries;
  std::deque<uint64_t> queue;
  int cur_arena = -1;   // S103 D2: results and queued keys of any other arena are dropped
  bool stop = false;
  std::thread worker;
  explicit State(BodySource& s) : inner(s) {
    worker = std::thread([this] { wwe2k14::NameThread(L"live_crowd");
      for (;;) {
        uint64_t key;
        {
          std::unique_lock<std::mutex> lock(mu);
          cv.wait(lock, [&] { return stop || !queue.empty(); });
          if (stop) return;
          key = queue.front();
          queue.pop_front();
        }
        assets::Model m;
        std::string err;
        if (((key >> 8) & 255) == 255) { inner.ArenaChanged(int(key >> 16), -1); continue; }   // S103 D2: the release job (the old arena's stored model goes here, under the loader's mutex)
        const BodyStatus st = inner.Request(int(key >> 16), int((key >> 8) & 255), int(key & 255), m, err);
        std::lock_guard<std::mutex> lock(mu);
        if (int(key >> 16) != cur_arena) { entries.erase(key); continue; }
        Entry& e = entries[key];
        e.done = true, e.status = st, e.model = std::move(m), e.err = std::move(err);
      }
    });
  }
};
AsyncBodies::AsyncBodies(BodySource& inner) : s_(new State(inner)) {}
AsyncBodies::~AsyncBodies() {
  {
    std::lock_guard<std::mutex> lock(s_->mu);
    s_->stop = true;
  }
  s_->cv.notify_all();
  if (s_->worker.joinable()) s_->worker.join();
}
// S103 D2: queued requests and finished bodies of other arenas are dropped; the old arena's release runs on the worker (serialised with its loads).
void AsyncBodies::ArenaChanged(int old_arena, int new_arena) {
  std::lock_guard<std::mutex> lock(s_->mu);
  s_->cur_arena = new_arena;
  for (auto it = s_->queue.begin(); it != s_->queue.end();) it = int(*it >> 16) != new_arena ? s_->queue.erase(it) : it + 1;
  for (auto it = s_->entries.begin(); it != s_->entries.end();) it = int(it->first >> 16) != new_arena ? s_->entries.erase(it) : std::next(it);
  if (old_arena >= 0) s_->queue.push_back((uint64_t(uint32_t(old_arena)) << 16) | (uint64_t(255) << 8));
  s_->cv.notify_one();
}
BodyStatus AsyncBodies::Request(int arena, int lod, int type, assets::Model& out, std::string& err) {
  const uint64_t key = (uint64_t(uint32_t(arena)) << 16) | (uint64_t(lod) << 8) | uint64_t(type);
  std::lock_guard<std::mutex> lock(s_->mu);
  if (s_->cur_arena < 0) s_->cur_arena = arena;   // (the first arena asked for)
  auto it = s_->entries.find(key);
  if (it == s_->entries.end()) {
    s_->entries.emplace(key, State::Entry{});
    s_->queue.push_back(key);
    s_->cv.notify_one();
    return BodyStatus::Pending;
  }
  if (!it->second.done) return BodyStatus::Pending;
  const BodyStatus st = it->second.status;
  out = std::move(it->second.model);
  err = std::move(it->second.err);
  s_->entries.erase(it);
  return st;
}

std::string LiveCrowdCounts::Text() const {
  char b[900];
  std::snprintf(b, sizeof b,
                "crowd: record %u (runs %u); members drawn %u (floor %u, stored pose %u) skipped %u; cards drawn %u skipped %u; classes %u, bakes %u; draws %u; refused %u; misses %u; hard failures %u; hidden %u; "
                "CPU pose %u verts %u us, bad bones %u%s%s%s%s",
                records_crowd, run_members, members_drawn, floor_drawn, stored_pose, members_skipped, cards_drawn, cards_skipped, classes_drawn, bakes, draws, refused, misses, hard_failures,
                hidden_listed, verts_posed, pose_us, bad_bones, skip_why.empty() ? "" : "; skipped: ", skip_why.c_str(), refusal.empty() ? "" : "; refusal: ", refusal.c_str());
  char p[260];   // S103 D4
  std::snprintf(p, sizeof p, "; props %u (drawn %u, hidden %u, skipped %u, in place %u, neutral picture %u), singles %u (node mismatch %u, nodes != file skeleton %u, file slot map != identity %u); light fallbacks %u", props_listed, props_drawn, props_hidden, props_skipped, props_in_place, props_neutral_picture, singles_listed, singles_node_mismatch, singles_nodes_differ, singles_slot_map_nonidentity, light_fallbacks);
  std::string fp;   // S103 FHB
  if (props_file_picture || !file_pictures.empty()) fp = "; file pictures " + std::to_string(props_file_picture) + " [" + file_pictures + "]";
  char sg[160];   // S103 CSF
  std::snprintf(sg, sizeof sg, "; sign_pictures file %u guest %u grey %u; hand_pictures file %u guest %u grey %u", sign_pic[0], sign_pic[1], sign_pic[2], hand_pic[0], hand_pic[1], hand_pic[2]);
  char bk[96];   // S103 CB1
  std::snprintf(bk, sizeof bk, "; bake passes %u, same inputs %u", bake_passes, bakes_same_inputs);
  return std::string(b) + p + fp + sg + bk + (bodies.empty() ? std::string() : "; bodies [" + bodies + "]");
}

namespace {
struct PoseSlot {   // one posed body (every mesh): a (LOD group, type, palette) of this frame
  uint32_t lod = 0, type = 0, first = 0, count = 0;
  std::vector<std::vector<InstVertex>> meshes;
};
struct Group {      // the members of one posed body
  uint32_t lod = 0, type = 0, first = 0, count = 0;
  std::vector<float> worlds;   // 12 floats a member
  uint32_t floor = 0;
  int pose = -1;
};
struct CardGroup {
  uint32_t first = kNoPalette, count = 0;
  std::vector<float> worlds;
  int pose = -1;
  bool baked = false;
};
}  // namespace

struct LiveCrowd::Impl {
  rdr::Renderer& renderer;
  FrontEnd& fe;
  BodySource& bodies;
  LiveCrowdOptions options;
  bool PropInPlaceSkipped(const CrowdPropRow& pr) const { return (pr.flags & kCpfInPlace) && options.skip_in_place_props && !options.fault_in_place_drawn_live; }   // S103 SG2
  const SceneTables* record = nullptr;
  std::array<BodyClass, kClasses> classes;
  std::array<uint32_t, kClasses> bad_bones_by_class{};
  std::unordered_map<std::string, rdr::ImageId> shared_images;   // the classes' pictures by name (one stage light map, one picture per type)
  int card_model = -1;
  CrowdPages pages;   // S103 B4F: the stage page of every resident crowd model (SetStagePage)
  uint64_t joined_group = 0;   // S103 UP (found by up_check's sigchange arm): the page group the slots last joined; a rebuilt set names a new one and the crowd must follow even when the page KEY did not change
  rdr_adapter::CrowdRankTracker rank;   // S103 SPB: the CROWD DENSITY rank of a member (0-99 from its position, tracked across frames as the stream binder's is); render thread only
  // A model just made: its stage page texture (if it has one) joins the set and takes the current page (a class resident later gets the entrance page too).
  void RegPage(int model, int stage_tex) {
    if (model < 0 || stage_tex < 0) return;
    fe.JoinCurrentPage(model, size_t(stage_tex));   // S103 UP: the arena's one page image, before the image id is read (CrowdPages dedups by image id)
    const uint64_t img = fe.TextureImage(model, size_t(stage_tex));
    if (!img) return;
    pages.Add(model, stage_tex, img, fe.PageKeyOf(model, size_t(stage_tex)));   // S103 UP (review F2): the held key of a joined image, not an assumed 0
    pages.Sync([&](int m, int t, uint32_t w, uint32_t h, const uint8_t* px) { return fe.UpdatePage(m, size_t(t), pages.key(), w, h, px); });   // S103 UP
  }
  bool card_failed = false;   // S103 D2: the card quad exists but DrawInstanced would refuse it
  std::vector<std::vector<InstVertex>> card_quad;  // one mesh, 4 vertices
  // S103 D4: the signs / foam hands (kScCrowdProps): one front-end model per audience-set key, drawn instanced with the audience material of a LOD 0 body
  struct PropClass { bool ok = false, failed = false; std::string why; int model = -1; std::vector<std::vector<InstVertex>> posed; };
  std::map<uint32_t, PropClass> prop_classes;
  assets::Model prop_set;                 // the arena's audience props (BodySource kPropSetLod)
  int prop_set_state = 0;                 // 0 not resident, 1 resident, 2 failed
  std::string prop_set_why;
  assets::Model tmpl;                     // the audience material (materials, textures, light sets, stage light map) of the first LOD 0 body built: no meshes
  bool have_tmpl = false;
  int tmpl_material = -1, tmpl_light_set = -1;
  rdr::ImageId prop_picture = rdr::kNoImage;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<float>> prop_worlds;   // this frame: (mesh key, picture key) -> 12 floats an instance (S103 D6: the picture is part of the group)
  std::unordered_map<uint32_t, uint32_t> file_index;   // S103 CSF review 3: file key -> index in file_rows
  std::vector<CrowdPictureRow> file_rows;    // S103 FHB: the audience set's texture entries as picture rows, key = kFileKeyBit | entry id (built when the prop set is resident)
  PictureCache pic_cache;      // S103 D6: picture key -> the image made from the record's CrowdPictureRow (re-made when its bytes change)
  const SceneTables* pic_record = nullptr;   // this frame's record (the draw loop asks the cache again, same answer)
  std::array<rdr::ImageId, kTypes> card_image{};
  rdr::ImageId card_depth = rdr::kNoImage;
  // S103 CB1 (card_atlas_rule.h): ONE atlas image + depth for every type's picture, each tile's quad (v remapped), and what each tile holds (the bake's input key; valid = baked and not since cleared).
  rdr::ImageId atlas_image = rdr::kNoImage, atlas_depth = rdr::kNoImage;
  std::array<std::vector<std::vector<InstVertex>>, kTypes> atlas_quads;
  struct Tile { bool valid = false; uint64_t key = 0; };
  std::array<Tile, kTypes> tiles{};
  bool frame_atlas = false;   // this frame's baking went through the atlas (the far-card draw then samples it); decided ONCE per frame at Stage::Maps from the latched anisotropy
  uint64_t seen_dropped = 0;  // Renderer::FramesDropped at the last atlas bake: a move = a frame we recorded never reached the GPU, so no tile is trusted
  bool EnsureAtlas() {
    if (atlas_image != rdr::kNoImage && atlas_depth != rdr::kNoImage) return true;
    rdr::ImageDesc d;
    d.width = uint16_t(kAtlasWidth), d.height = uint16_t(kAtlasHeight);
    d.format = rdr::Format::RGBA8;
    d.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageSampled);
    atlas_image = renderer.CreateImage(d);
    rdr::ImageDesc dd = d;
    dd.format = rdr::Format::Depth;
    dd.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageTransient);
    atlas_depth = renderer.CreateImage(dd);
#if !WWE2K14_STREAM()
    if (atlas_image != rdr::kNoImage) renderer.SetImageClass(atlas_image, rdr::kGpuCards);   // S103 GT
    if (atlas_depth != rdr::kNoImage) renderer.SetImageClass(atlas_depth, rdr::kGpuCards);
#endif
    tiles.fill(Tile{});
    return atlas_image != rdr::kNoImage && atlas_depth != rdr::kNoImage;
  }
  const std::vector<std::vector<InstVertex>>* AtlasQuad(uint32_t type) {   // the far-card quad with this tile's v
    std::vector<std::vector<InstVertex>>& q = atlas_quads[type];
    if (q.empty() && !card_quad.empty()) {
      q = card_quad;
      for (InstVertex& v : q[0]) v.uv[1] = AtlasV(type, v.uv[1]);
    }
    return &q;
  }
  int arena = -1;
  uint32_t adopted = 0;
  // scratch kept across frames (no per-frame heap traffic once warm)
  std::vector<AssetInfo> asset_info;
  std::vector<Group> groups;
  size_t ngroups = 0, last_group = 0;
  std::array<CardGroup, kTypes> card_groups;
  std::vector<PoseSlot> pose_pool;
  size_t pose_used = 0;

  // S103 L4: the light sets of each front-end model this crowd made, as built (the loader's), the set its meshes use, and this frame's evaluation from the record (once per model and frame).
  struct LitModel { std::vector<assets::LightSet> base; int used = -1; };
  struct FrameLit { ModelLights ml; bool ok = false; };
  std::unordered_map<int, LitModel> lit_models;
  std::unordered_map<int, FrameLit> frame_lights;
  const SceneTables* lights_record = nullptr;
  uint64_t lights_frame = ~0ull;
  uint64_t lights_sig = 0;   // S103 PS1 Q5: what the evaluation reads (the record's light rows + stage-look rows): the cache is NOT a hit for another record that sits at the same address in the same guest frame (every rig job's SceneTables does)
  static uint64_t HashBytes(uint64_t h, const void* d, size_t n) {   // S103 PS1 Q5: 8 bytes at a time
    const uint8_t* p = static_cast<const uint8_t*>(d);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; h ^= h >> 29; }
    for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h ^ (uint64_t(n) << 1);
  }
  static uint64_t LightsSig(const SceneTables& t) {
    if (g_plant_lights_key_old) return 0;   // planted twin (live_crowd.h; set by the rig only): the old cache key, the record's address + frame alone
    uint64_t h = 1469598103934665603ull;
    h = HashBytes(h, t.lights.data(), t.lights.size() * sizeof(Light));
    h = HashBytes(h, t.stage_look.data(), t.stage_look.size() * sizeof(StageLookRow));
    return h;
  }
  void NoteLit(int model, const std::vector<assets::LightSet>& sets, int used) { lit_models[model] = LitModel{sets, used}; }
  // null = the model keeps the loader's sets: COUNTED in `c.light_fallbacks`, never silent (the frame is a hole at build time, LightHoles; this is the draw-time net).
  const ModelLights* LightsOf(int model, LiveCrowdCounts& c) {
    auto b = lit_models.find(model);
    if (!g_lights_fn || !record || b == lit_models.end() || b->second.base.empty()) return ++c.light_fallbacks, nullptr;
    const uint64_t sig = LightsSig(*record);
    if (lights_record != record || lights_frame != record->header.guest_frame || lights_sig != sig) lights_record = record, lights_frame = record->header.guest_frame, lights_sig = sig, frame_lights.clear();
    auto it = frame_lights.find(model);
    if (it == frame_lights.end()) {
      FrameLit fl;
      fl.ml.asset = uint32_t(model);
      fl.ml.sets = b->second.base;
      fl.ok = g_lights_fn(*record, fl.ml.sets, b->second.used);
      it = frame_lights.emplace(model, std::move(fl)).first;
    }
    if (!it->second.ok) ++c.light_fallbacks;
    return &it->second.ml;
  }

  Impl(rdr::Renderer& r, FrontEnd& f, BodySource& b, const LiveCrowdOptions& o) : renderer(r), fe(f), bodies(b), options(o) { card_image.fill(rdr::kNoImage); }
  ~Impl() { Release(); }

  // The arena's resources go (after the GPU is idle): class models, the card quad, the pictures. The classes are loaded again on the next need.
  void Release() {
    renderer.WaitIdle();
    for (BodyClass& c : classes) {
      if (c.model >= 0) fe.ReleaseModel(c.model);
      c = BodyClass{};
    }
    if (card_model >= 0) fe.ReleaseModel(card_model);
    card_model = -1;
    for (auto& kv : prop_classes)   // S103 D4
      if (kv.second.model >= 0) fe.ReleaseModel(kv.second.model);
    prop_classes.clear(), prop_worlds.clear();
    prop_set = assets::Model{}, prop_set_state = 0, prop_set_why.clear(), file_rows.clear(), file_index.clear(), crowd::SignFileKeys::Clear();   // (S103 CSF: until the set is resident again the reader reads the guest pictures)
    tmpl = assets::Model{}, have_tmpl = false, tmpl_material = tmpl_light_set = -1;
    if (prop_picture != rdr::kNoImage) renderer.DestroyImage(prop_picture), prop_picture = rdr::kNoImage;
    { PicBackend b{*this}; pic_cache.Clear(b); }   // S103 D6
    for (auto& kv : shared_images)
      if (kv.second != rdr::kNoImage) renderer.DestroyImage(kv.second);
    shared_images.clear();
    for (rdr::ImageId& id : card_image)
      if (id != rdr::kNoImage) renderer.DestroyImage(id), id = rdr::kNoImage;
    if (card_depth != rdr::kNoImage) renderer.DestroyImage(card_depth), card_depth = rdr::kNoImage;
    if (atlas_image != rdr::kNoImage) renderer.DestroyImage(atlas_image), atlas_image = rdr::kNoImage;   // S103 CB1
    if (atlas_depth != rdr::kNoImage) renderer.DestroyImage(atlas_depth), atlas_depth = rdr::kNoImage;
    tiles.fill(Tile{});
    for (auto& q : atlas_quads) q.clear();
    bad_bones_by_class.fill(0);
    card_quad.clear();
    lit_models.clear(), frame_lights.clear(), lights_record = nullptr, lights_frame = ~0ull;   // S103 L4
    pages.Clear();   // S103 B4F (the models and the shared page image are gone)
  }

  // Builds the class from a body the source delivered: a model the front-end draws only through DrawInstanced. A mesh the front-end would skip is skipped here the same
  // way, so the posed vertex lists line up with the model's meshes.
  void Build(BodyClass& c, uint32_t lod, uint32_t type, assets::Model& body) {
    c.nodes = uint32_t(body.bones.size());
    c.slots = c.identity_bad = 0, c.identity_known = false;   // S103 SN
    assets::Model clean;
    clean.label = "live crowd lod " + std::to_string(lod) + " type " + std::to_string(type);
    clean.materials = body.materials, clean.textures = body.textures, clean.light_sets = body.light_sets;
    clean.stage_lightmap = body.stage_lightmap;
    clean.lm_scale[0] = body.lm_scale[0], clean.lm_scale[1] = body.lm_scale[1], clean.lm_gain = body.lm_gain;
    // The file stores the audience's material under the generic class name; the game draws it with the audience effect (yAudience: the world rows from the member, the
    // lights and the stage light map by the world position). Named here by what the draw IS, not read from a stream draw.
    for (assets::Material& m : clean.materials) m.shader = "yAudience";
    for (assets::Mesh& me : body.meshes) {
      if (me.vertices.empty() || me.indices.size() < 3) continue;
      bool bad = false;
      for (uint32_t ix : me.indices) bad = bad || ix >= me.vertices.size();
      if (bad || me.part.rfind("crowd flash", 0) == 0) continue;
      std::vector<StoredVertex> sv(me.vertices.size());
      for (size_t i = 0; i < me.vertices.size(); ++i) {
        const assets::Vertex& v = me.vertices[i];
        for (int k = 0; k < 3; ++k) sv[i].pos[k] = v.pos[k] * kBodyScale;
        std::memcpy(sv[i].normal, v.normal, sizeof sv[i].normal);
        std::memcpy(sv[i].uv, v.uv0, sizeof sv[i].uv);
        std::memcpy(sv[i].bones, v.bones, sizeof sv[i].bones);
        std::memcpy(sv[i].weights, v.weights, sizeof sv[i].weights);
      }
      for (const StoredVertex& v : sv)   // S103 SN: the palette slots PoseBody reads (a weighted reference)
        for (int j = 0; j < 4; ++j)
          if (v.weights[j] > 0.f) c.slots = std::max(c.slots, uint32_t(v.bones[j]) + 1u);
      if (!me.palette_nodes.empty() && me.src_skin_slots.size() == me.vertices.size() * 4 && me.src_skin_weights.size() == me.src_skin_slots.size()) {   // the file's own slot -> node map, on the raw slots the vertices use
        c.identity_known = true;
        for (size_t i = 0; i < me.src_skin_slots.size(); ++i) {
          const uint32_t slot = me.src_skin_slots[i];
          if (slot == 255 || !(me.src_skin_weights[i] > 0.f)) continue;
          if (slot >= me.palette_nodes.size() || me.palette_nodes[slot] != slot) ++c.identity_bad;
        }
      }
      c.meshes.push_back(std::move(sv));
      me.skinned = false, me.placement = -1;
      clean.meshes.push_back(std::move(me));
    }
    if (clean.meshes.empty()) { c.failed = true, c.why = "no drawable mesh"; return; }
    std::vector<std::string> warnings;
    c.model = fe.AddModel(clean, warnings, &shared_images);
    c.ok = c.model >= 0;
    if (!c.ok) { c.failed = true, c.why = "AddModel failed"; return; }
    NoteLit(c.model, clean.light_sets, clean.meshes[0].light_set);   // S103 L4
    RegPage(c.model, clean.stage_lightmap);   // S103 B4F
    fe.SetHookOwned(c.model);
    if (const std::string why = fe.InstancedRefusal(c.model); !why.empty()) { c.failed = true, c.ok = false, c.why = "DrawInstanced would refuse: " + why; return; }   // S103 D2: a hole at Build time
    if (lod == 0 && !have_tmpl) {   // S103 D4: the audience material the props are drawn with
      tmpl.materials = clean.materials, tmpl.textures = clean.textures, tmpl.light_sets = clean.light_sets, tmpl.stage_lightmap = clean.stage_lightmap;
      tmpl.lm_scale[0] = clean.lm_scale[0], tmpl.lm_scale[1] = clean.lm_scale[1], tmpl.lm_gain = clean.lm_gain;
      tmpl_material = clean.meshes[0].material, tmpl_light_set = clean.meshes[0].light_set;
      have_tmpl = true;
    }
    if (card_model < 0 && lod == 0 && !clean.materials.empty()) {  // the card quad takes this body's material (cut-out, two-sided) and light map
      assets::Model q;
      q.label = "live crowd card quad";
      q.materials = clean.materials, q.textures = clean.textures, q.light_sets = clean.light_sets;
      q.stage_lightmap = clean.stage_lightmap;
      q.lm_scale[0] = clean.lm_scale[0], q.lm_scale[1] = clean.lm_scale[1], q.lm_gain = clean.lm_gain;
      // S103 B3 (T3b): the card technique draws c184.z = the gain x the card factor [0x8376383C] * 0.001 (sub_827F22C8 0x827F23B8, only when > 0); the front end reads the word from the record row kSlCardFactor.
      q.lm_card = true;
      const int mat = clean.meshes[0].material;
      if (mat >= 0 && size_t(mat) < q.materials.size()) {
        q.materials[size_t(mat)].alpha_test = true;
        q.materials[size_t(mat)].cull_back = false, q.materials[size_t(mat)].cull_from_record = false;   // S105 C3: the crowd keeps its own rule
        assets::Mesh qm;
        qm.material = mat, qm.part = "card", qm.light_set = clean.meshes[0].light_set;
        const float px[4] = {+82.f, -82.f, +82.f, -82.f}, py[4] = {0.f, 0.f, -250.f, -250.f}, pu[4] = {1.f, 0.f, 1.f, 0.f}, pv[4] = {1.f, 1.f, 0.f, 0.f};
        qm.vertices.resize(4);
        card_quad.assign(1, std::vector<InstVertex>(4));
        for (int i = 0; i < 4; ++i) {
          assets::Vertex& v = qm.vertices[size_t(i)];
          std::memset(&v, 0, sizeof v);
          v.pos[0] = px[i], v.pos[1] = py[i], v.pos[2] = 0.f, v.normal[2] = 1.f, v.uv0[0] = pu[i], v.uv0[1] = pv[i], v.color = 0xFFFFFFFFu, v.weights[0] = 1.f;
          InstVertex& iv = card_quad[0][size_t(i)];
          std::memcpy(iv.pos, v.pos, sizeof iv.pos), std::memcpy(iv.normal, v.normal, sizeof iv.normal), std::memcpy(iv.uv, v.uv0, sizeof iv.uv);
        }
        qm.indices = {0, 1, 2, 2, 1, 3};
        q.meshes.push_back(std::move(qm));
        std::vector<std::string> qw;
        card_model = fe.AddModel(q, qw, &shared_images);
        fe.SetUnlit(card_model);   // S103 L4: the game draws cards with yAudience_lm_uv + lm_pixel_color_alpha; the card picture is baked lit already (before the first bind: InstancedRefusal below)
        RegPage(card_model, q.stage_lightmap);   // S103 B4F
        fe.SetHookOwned(card_model);
        card_failed = !fe.InstancedRefusal(card_model).empty();   // S103 D2
      }
    }
  }

  // S103 D2: what the record's assets name (once per asset) and the arena they belong to; a change of arena releases the old one (the GPU is idle first).
  std::string car_key_seen;   // S103 CAR fix 5
  void SyncArena(const SceneTables& t) {
    asset_info.resize(t.assets.size());
    int a = -1;
    for (size_t i = 0; i < t.assets.size(); ++i) {
      asset_info[i] = ParseAsset(t, t.assets[i]);
      if (a < 0 && asset_info[i].valid && asset_info[i].arena >= 0) a = asset_info[i].arena;
    }
    SetArena(a);
  }
  // S110 ENTRBODIES: the arena half of SyncArena (LiveCrowd::PrefetchArena names the arena before any record carries the crowd). a < 0 = none named: nothing changes.
  void SetArena(int a) {
    // S103 CAR fix 5: a created arena whose parts changed (another save slot, the editor) is a new arena for the crowd: the old selection's bodies go the same way
    const std::string car_key = a >= 0 && assets::car::IsCustomId(a) ? assets::car::Current().Key() : std::string();
    if (a >= 0 && (a != arena || (!car_key.empty() && car_key != car_key_seen))) {
      const int old = arena;
      if (old >= 0) Release();
      arena = a;
      bodies.ArenaChanged(old, a);   // S103 D2: the old arena's stored model goes (on the source's worker), its queued requests are dropped
    }
    if (a >= 0) car_key_seen = car_key;
  }

  // S103 C1 review fix 4: a single row is drawn only when its palette (the entity's node count, read by the latch reader) has exactly the chosen body's skeleton node count. The holder's
  // LOD / type -> body mapping is [L] (crowd_latch_reader.h ReadSingles); a disagreement means the body is not the entity's mesh: refused and counted, never drawn with a wrong palette.
  // Only judged once the class is resident (a pending class is a miss already); an unknown skeleton count (0) refuses too.
  // S103 SN: the row's node count as the rule sees it (the test fault cuts it to the body's used slots - 1).
  uint32_t RowNodes(const CrowdSingleRow& sg) const {
    if (options.fault_singles_short_palette && sg.asset < asset_info.size() && asset_info[sg.asset].valid && !asset_info[sg.asset].card) {
      const BodyClass& c = classes[asset_info[sg.asset].lod * kTypes + asset_info[sg.asset].type];
      if (c.ok && c.slots > 0) return c.slots - 1;
    }
    return sg.bones_count;
  }
  // 0 = draw, 1 = refused: the palette does not cover the body's used slots.
  int SingleRefusal(const CrowdSingleRow& sg) const {
    if (sg.asset >= asset_info.size() || !asset_info[sg.asset].valid || asset_info[sg.asset].card) return 0;
    const BodyClass& c = classes[asset_info[sg.asset].lod * kTypes + asset_info[sg.asset].type];
    if (!c.ok) return 0;
    if (options.fault_singles_nodes_unchecked) return 0;   // test only: the pre-fix rule
    if (!crowd_rows::SingleNodesMatch(c.slots, RowNodes(sg))) return 1;
    return 0;
  }
  bool SingleNodesDisagree(const CrowdSingleRow& sg) const { return SingleRefusal(sg) != 0; }

  // The class if its body is resident; else null with the reason counted: Pending (queued, or over this frame's adoption limit) is a MISS, a failure is cached (never retried).
  BodyClass* Resolve(uint32_t lod, uint32_t type, LiveCrowdCounts& counts) {
    BodyClass& c = classes[lod * kTypes + type];
    if (c.ok) return &c;
    if (c.failed) { if (counts.skip_why.empty()) counts.skip_why = "body failed to load: " + c.why; return nullptr; }
    if (options.adopt_per_frame && adopted >= options.adopt_per_frame) { ++counts.misses; return nullptr; }
    assets::Model body;
    std::string err;
    const BodyStatus st = bodies.Request(arena, int(lod), int(type), body, err);
    if (st == BodyStatus::Pending) { ++counts.misses; if (counts.skip_why.empty()) counts.skip_why = "body not resident yet (queued): the frame is the game's"; return nullptr; }
    if (st == BodyStatus::Failed) { c.failed = true, c.why = err; ++counts.hard_failures; if (counts.skip_why.empty()) counts.skip_why = "body failed to load: " + err; return nullptr; }
    ++adopted;
    Build(c, lod, type, body);
    if (c.failed) { ++counts.hard_failures; if (counts.skip_why.empty()) counts.skip_why = "body unusable: " + c.why; return nullptr; }
    return &c;
  }

  // The pose of (lod, type, palette) this frame: made once, then reused by every user (the members' draws and the card bake).
  int Pose(uint32_t lod, uint32_t type, uint32_t first, uint32_t count, const SceneTables& t, BodyClass& body, LiveCrowdCounts& counts) {
    for (size_t i = 0; i < pose_used; ++i) {
      const PoseSlot& p = pose_pool[i];
      if (p.lod == lod && p.type == type && p.first == first && p.count == count) return int(i);
    }
    if (pose_used == pose_pool.size()) pose_pool.emplace_back();
    PoseSlot& p = pose_pool[pose_used];
    p.lod = lod, p.type = type, p.first = first, p.count = count;
    p.meshes.resize(body.meshes.size());
    const bool pal = first != kNoPalette && !options.fault_no_palettes;
    const float* palette = pal ? t.bones[first].m : nullptr;
    PoseTimer timer(&counts.pose_us);
    uint32_t bad = 0;
    for (size_t k = 0; k < body.meshes.size(); ++k) {
      bad += PoseBody(body.meshes[k], palette, pal ? count : 0, p.meshes[k]);
      counts.verts_posed += uint32_t(body.meshes[k].size());
    }
    counts.bad_bones += bad;
    bad_bones_by_class[lod * kTypes + type] += bad;
    return int(pose_used++);
  }

  // ---- S103 D4: the signs / foam hands ----
  uint32_t KeyFor(const CrowdPropRow& r) const {   // the mesh of a row (the plant takes the other kind's default)
    if (options.fault_props_wrong_key) return crowd_rows::DefaultKey(r.kind == kCpHand ? uint8_t(kCpSign) : uint8_t(kCpHand));
    return crowd_rows::KeyOf(r);
  }
  // 0 = the arena's prop set is resident, 1 = pending (queued: a miss), 2 = failed (prop_set_why)
  int EnsurePropSet() {
    if (prop_set_state == 1) return 0;
    if (prop_set_state == 2) return 2;
    assets::Model m;
    std::string err;
    const BodyStatus st = bodies.Request(arena, kPropSetLod, 0, m, err);
    if (st == BodyStatus::Pending) return 1;
    if (st == BodyStatus::Failed) return prop_set_state = 2, prop_set_why = err, 2;
    prop_set = std::move(m), prop_set_state = 1;
    file_rows.clear(), file_index.clear();   // S103 FHB
    for (const assets::Texture& tx : prop_set.textures) {
      CrowdPictureRow r;
      r.key = tx.name.size() && tx.name[0] == 'S' ? kSignFileKeyBit | uint32_t(std::strtoul(tx.name.c_str() + 1, nullptr, 10)) : kFileKeyBit | uint32_t(std::strtoul(tx.name.c_str(), nullptr, 10));   // S103 CSF: "S<key>" = a sign picture (audience.pac), its own key namespace
      r.width = uint16_t(tx.width), r.height = uint16_t(tx.height);
      r.format = tx.format == assets::TexFormat::kBC1 ? uint32_t(kCpfBc1) : tx.format == assets::TexFormat::kBC2 ? uint32_t(kCpfBc2) : uint32_t(kCpfBc3);
      r.nbytes = uint32_t(tx.data.size());
      if (tx.data.size() > kCrowdPictureBytes) continue;
      std::memcpy(r.bytes, tx.data.data(), tx.data.size());
      if (CrowdPictureValid(r)) {
        file_index[r.key] = uint32_t(file_rows.size());   // S103 CSF review 3: key -> row, O(1)
        file_rows.push_back(r);
        const uint64_t fnv = crowd::Fnv1a(r.bytes, r.nbytes);   // the reader's one-time check compares the guest's blocks with this
        if ((r.key & 0xF0000000u) == kSignFileKeyBit) crowd::SignFileKeys::Publish(r.key & ~kSignFileKeyBit, 0, fnv);   // S103 CSF: the reader may now leave this sign's picture to the file (once its check passes)
        else if ((r.key & 0xF0000000u) == kFileKeyBit) crowd::SignFileKeys::Publish(r.key & 0x0FFFFFFFu, 1, fnv);   // ... and a foam hand's (42000 + h - 1)
      }
    }
    prop_set.textures.clear(), prop_set.textures.shrink_to_fit();   // S103 CSF review 7: the file rows are the applier's copy; the model's textures are never read again
    return 0;
  }
  // S103 FHB: the picture a prop row should bind when the record carries no valid row for its key: a foam hand's FILE picture (entry 42000 + h - 1 = its mesh key + 1000: sub_8250B1E8 [C listing,
  // docs/renderer/CROWD_PROPS_GATES.md 11]). 0 = none (a sign: its picture's audience.pac key per style is not decoded; or a hand with no known mesh, or the prop set not resident).
  static constexpr uint32_t kFileKeyBit = 0xF0000000u;
  bool RecordHasPicture(uint32_t key, const SceneTables& t) const {
    if (!key) return false;
    for (const CrowdPictureRow& pc : t.crowd_pictures)
      if (pc.key == key && pc.nbytes && CrowdPictureValid(pc)) return true;
    return false;
  }
  static constexpr uint32_t kSignFileKeyBit = 0xE0000000u;   // S103 CSF: the sign pictures of audience.pac (a separate key space: its ids 10001-52xxx overlap the arena set's 40000-42999)
  bool IsFileKey(uint32_t key) const {   // only a key that names one of the file rows (a guest handle never matches: they carry no 0xE000 / 0xF000 high nibble with an entry id below)
    const uint32_t top = key & 0xF0000000u;
    if (top != kFileKeyBit && top != kSignFileKeyBit) return false;
    return file_index.count(key) != 0;   // S103 CSF review 3
  }
  uint32_t FilePictureKey(const CrowdPropRow& pr) const {
    if (pr.kind == kCpSign) {   // S103 CSF (H-CS1): a sign's picture is audience.pac's entry file_key (< 65000); a key the file lacks (or the runtime range) stays a guest row / the grey board
      if (options.fault_no_file_pictures || !crowd::FilePicturesOn() || !pr.file_key || pr.file_key >= crowd::kSignFileKeyLimit || prop_set_state != 1 || !crowd::SignFileKeys::Has(pr.file_key, 0)) return 0;   // (a key whose one-time check failed stays on its guest row)
      const uint32_t sk = kSignFileKeyBit | pr.file_key;
      return file_index.count(sk) ? sk : 0u;
    }
    if (options.fault_no_file_pictures || pr.kind != kCpHand || pr.model_key < 41000 || pr.model_key > 41018 || prop_set_state != 1) return 0;
    if (!crowd::FilePicturesOn() || !crowd::SignFileKeys::Has(pr.model_key + 1000u, 1)) return 0;   // S103 CSF: the one switch, and a failed check keeps the guest row
    const uint32_t k = kFileKeyBit | (pr.model_key + 1000u);
    return file_index.count(k) ? k : 0u;
  }
  // The class of `key`; the set and a LOD 0 body must be resident (callers check). A key the audience set does not hold is a FAILED class with its reason (cached).
  PropClass& ResolveProp(uint32_t key) {
    auto it = prop_classes.find(key);
    if (it != prop_classes.end()) return it->second;
    PropClass& pc = prop_classes[key];
    const assets::Mesh* src = nullptr;
    for (const assets::Mesh& m : prop_set.meshes)
      if (uint32_t(m.src_key) == key) { src = &m; break; }
    if (!src) { pc.failed = true, pc.why = "prop key " + std::to_string(key) + " is not in the audience set of arena " + std::to_string(arena); return pc; }
    assets::Model m;
    m.label = "live crowd prop " + std::to_string(key);
    m.materials = tmpl.materials, m.textures = tmpl.textures, m.light_sets = tmpl.light_sets, m.stage_lightmap = tmpl.stage_lightmap;
    m.lm_scale[0] = tmpl.lm_scale[0], m.lm_scale[1] = tmpl.lm_scale[1], m.lm_gain = tmpl.lm_gain;
    // S103 SG: back faces CULLED, as the game: the crowd entry sets cull on once for the sign loop, the hand loop, the node loop and the stands (sub_82519508 0x82519680 -> sub_82C24128: state id 56 = 2 / 6
    // by the current mode [state+116]); the stream records cull BACK, front face CLOCKWISE on every prop and stands block draw [C: brock_losswin, tools\crowd\crowd_prop_cull_check.py], which is DrawInstanced's
    // state for a cull_back material (front_end.cpp: cull 2, front_face 1), with the strip's alternate winding kept (PropMeshes above). The fault props_two_sided keeps the pre-SG rule (test only).
    if (tmpl_material >= 0 && size_t(tmpl_material) < m.materials.size()) m.materials[size_t(tmpl_material)].cull_back = !options.fault_props_two_sided, m.materials[size_t(tmpl_material)].cull_from_record = false;   // S105 C3: the crowd keeps its own rule
    assets::Mesh me = *src;
    me.material = tmpl_material, me.light_set = tmpl_light_set;
    if (options.fault_props_cull_front)   // S103 SGF planted: the winding reversed (test only)
      for (size_t i = 0; i + 2 < me.indices.size(); i += 3) std::swap(me.indices[i + 1], me.indices[i + 2]);
    pc.posed.assign(1, std::vector<InstVertex>(me.vertices.size()));
    for (size_t i = 0; i < me.vertices.size(); ++i) {
      InstVertex& iv = pc.posed[0][i];
      std::memcpy(iv.pos, me.vertices[i].pos, sizeof iv.pos), std::memcpy(iv.normal, me.vertices[i].normal, sizeof iv.normal), std::memcpy(iv.uv, me.vertices[i].uv0, sizeof iv.uv);
    }
    m.meshes.push_back(std::move(me));
    std::vector<std::string> warnings;
    pc.model = fe.AddModel(m, warnings, &shared_images);
    if (pc.model < 0) { pc.failed = true, pc.why = "AddModel failed for prop " + std::to_string(key); return pc; }
    fe.SetHookOwned(pc.model);
    NoteLit(pc.model, m.light_sets, tmpl_light_set);   // S103 L4
    RegPage(pc.model, m.stage_lightmap);   // S103 B4F
    if (const std::string why = fe.InstancedRefusal(pc.model); !why.empty()) { pc.failed = true, pc.why = "DrawInstanced would refuse prop " + std::to_string(key) + ": " + why; return pc; }
    pc.ok = true;
    return pc;
  }
  // Record-time path (no Prefetch ran): the class if everything it needs is resident, else null with the miss / failure counted.
  PropClass* PreparePropClass(uint32_t key, LiveCrowdCounts& counts) {
    const int st = EnsurePropSet();
    if (st == 1) { ++counts.misses; if (counts.skip_why.empty()) counts.skip_why = "audience props not resident yet (queued)"; return nullptr; }
    if (st == 2) { ++counts.hard_failures; if (counts.skip_why.empty()) counts.skip_why = "audience props failed to load: " + prop_set_why; return nullptr; }
    if (!have_tmpl && Resolve(0, 0, counts) == nullptr) return nullptr;
    PropClass& pc = ResolveProp(key);
    if (!pc.ok) { if (counts.skip_why.empty()) counts.skip_why = pc.why; return nullptr; }
    return &pc;
  }
  rdr::ImageId PropPicture() {   // the neutral picture of a prop (a sign has no source yet [U]; a foam hand takes its file picture, S103 FHB)
    if (prop_picture == rdr::kNoImage) {
      rdr::ImageDesc d;
      d.width = d.height = 1, d.mips = 1;
      d.usage = rdr::kUsageSampled;
      d.format = rdr::Format::RGBA8;
      prop_picture = renderer.CreateImage(d);
      const uint8_t px[4] = {200, 200, 200, 255};
      if (prop_picture != rdr::kNoImage) renderer.UpdateImage(prop_picture, 0, 0, px, 4);
    }
    return prop_picture;
  }

  // S103 D6: the image of a prop picture through PictureCache (crowd_picture_cache.h: one lookup a key a frame, only a valid row, unused images released at EndFrame).
  struct PicBackend {
    Impl& s;
    uint32_t Grey() { return uint32_t(s.PropPicture()); }
    uint32_t Create(const CrowdPictureRow& row) {
      rdr::ImageDesc d;
      d.width = row.width, d.height = row.height, d.mips = 1;
      d.usage = rdr::kUsageSampled;
      d.format = row.format == kCpfBc2 ? rdr::Format::BC2 : row.format == kCpfBc3 ? rdr::Format::BC3 : rdr::Format::BC1;
      return uint32_t(s.renderer.CreateImage(d));
    }
    void Update(uint32_t id, const CrowdPictureRow& row) { s.renderer.UpdateImage(rdr::ImageId(id), 0, 0, row.bytes, row.nbytes); }
    void Destroy(uint32_t id) { s.renderer.DestroyImage(rdr::ImageId(id)); }
  };
  rdr::ImageId PictureFor(uint32_t key, const SceneTables& t) {
    PicBackend b{*this};
    return rdr::ImageId(pic_cache.For(key, IsFileKey(key) ? file_rows : t.crowd_pictures, b));   // S103 FHB: a file key is looked up among the file rows
  }
  Group& GroupFor(uint32_t lod, uint32_t type, uint32_t first, uint32_t count) {
    const auto same = [&](const Group& g) { return g.lod == lod && g.type == type && g.first == first && g.count == count; };
    if (last_group < ngroups && same(groups[last_group])) return groups[last_group];
    for (size_t i = 0; i < ngroups; ++i)
      if (same(groups[i])) return last_group = i, groups[i];
    if (ngroups == groups.size()) groups.emplace_back();
    Group& g = groups[ngroups];
    g.lod = lod, g.type = type, g.first = first, g.count = count, g.floor = 0, g.pose = -1;
    g.worlds.clear();
    last_group = ngroups++;
    return g;
  }

  // S103 CB1: the card bake of a frame in ONE pass (card_atlas_rule.h). Per type with cards: the same pose / draw the per-type bake makes; with card_atlas bit 2 the draw's inputs are hashed first (a dry
  // DrawInstanced: state, constants, textures, posed vertices) and a picture whose key equals its last bake's is kept. Then ONE SetTargets, one clear (full when no kept picture would be lost, else one
  // rect per baked tile) and per baked tile three draws of the same body: its edge rows into the two gutter rows (viewport moved a row, scissor one row), then the tile itself.
  void RecordAtlasBake(LiveCrowdCounts& counts, const StageCtx& ctx, rdr::FrameBuilder& fb, const float* ortho, const float* eye, const float* identity) {
    struct Job { uint32_t type; InstancedDraw d; bool black; int pose; };   // (d.posed is set again at draw time: pose_pool may grow while later types are posed)
    Job jobs[kTypes];
    uint32_t nj = 0;
    const bool skip_same = (options.card_atlas & kCardAtlasSkipSame) != 0;
    if (const uint64_t dropped = renderer.FramesDropped(); dropped != seen_dropped) {   // a frame we recorded was never submitted: the tiles it claimed to bake are not on the GPU
      seen_dropped = dropped;
      tiles.fill(Tile{});
    }
    for (uint32_t type = 0; type < kTypes; ++type) {
      CardGroup& cg = card_groups[type];
      if (cg.worlds.empty()) continue;
      const uint32_t n = uint32_t(cg.worlds.size() / 12);
      BodyClass* body = Resolve(0, type, counts);
      if (!body || card_model < 0) { counts.cards_skipped += n; if (body && counts.skip_why.empty()) counts.skip_why = "no card quad model"; cg.worlds.clear(); continue; }
      if (!EnsureAtlas()) { counts.cards_skipped += n; if (counts.skip_why.empty()) counts.skip_why = "no card picture image"; cg.worlds.clear(); continue; }
      Job& j = jobs[nj];
      j.type = type, j.black = options.fault_cards_black, j.d = InstancedDraw{}, j.pose = -1;
      if (!j.black) {
        const bool pal = cg.first != kNoPalette && !options.fault_no_palettes;
        cg.pose = Pose(0, type, pal ? cg.first : kNoPalette, pal ? cg.count : 0, *record, *body, counts);   // (the members' pose of the same palette is reused)
        j.pose = cg.pose;
        InstancedDraw& d = j.d;
        d.no_blend = true, d.model = body->model, d.posed = &pose_pool[size_t(cg.pose)].meshes, d.worlds = identity, d.count = 1, d.view_proj = ortho, d.eye = eye, d.game_space = true, d.lights = LightsOf(body->model, counts);
      }
      uint64_t key = 0;
      if (skip_same && !j.black) {
        StageCtx probe = ctx;
        probe.width = kAtlasWidth, probe.height = kAtlasHeight;
        InstancedDraw dry = j.d;
        key = 1469598103934665603ull;
        dry.input_hash = &key, dry.dry = true;
        InstancedStats hs;
        const uint32_t made = fe.DrawInstanced(dry, probe, fb, hs);
        const uint64_t pg[2] = {fe.texture_update_count(TexTag::Page), fe.texture_update_bytes(TexTag::Page)};   // the page's CONTENT (a key change sends it: the counters move)
        key = AtlasMix(key, pg, sizeof pg);
        key = AtlasMix(key, &made, sizeof made);
        Tile& t = tiles[type];
        if (t.valid && (t.key == key || options.fault_atlas_stale) && made > 0) {   // kept: the atlas holds exactly this picture
          ++counts.bakes_same_inputs;
          cg.baked = true;
          continue;
        }
      }
      j.d.tag_salt = 0;
      tiles[type].key = key;   // (set valid only once it is drawn below)
      ++nj;
    }
    if (nj == 0) return;
    // ---- ONE pass ----
    rdr::TargetsOp targets;
    targets.color[0].image = atlas_image;
    targets.color_count = 1;
    targets.depth.image = atlas_depth;
    fb.SetTargets(targets);
    ++counts.bake_passes;
    bool baking[kTypes] = {};
    for (uint32_t i = 0; i < nj; ++i) baking[jobs[i].type] = true;
    bool full = true;   // a full clear would lose a kept picture: then each baked tile is cleared on its own
    for (uint32_t t = 0; t < kTypes; ++t) full = full && (baking[t] || !tiles[t].valid);
    rdr::ClearOp clear;
    clear.flags = rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil;
    clear.depth = 1.0f;
    if (full) {
      clear.full = 1;
      fb.Clear(clear);
      for (Tile& t : tiles) t.valid = false;
    } else {
      // The depth target is TRANSIENT (never stored): it must be cleared as the pass's load op, not loaded (Problem transient_needs_contents). So ONE full depth + stencil clear first (the renderer absorbs it as the
      // load op), then the colour of each baked tile by rect: the kept tiles' colour stays as it was.
      rdr::ClearOp ds = clear;
      ds.flags = rdr::kClearDepth | rdr::kClearStencil, ds.full = 1;
      fb.Clear(ds);
      for (uint32_t i = 0; i < nj; ++i) {
        rdr::ClearOp cc = clear;
        cc.flags = rdr::kClearColor, cc.full = 0;
        cc.rect = rdr::Rect{0, int32_t(AtlasTileY(jobs[i].type) - kAtlasGutter), kAtlasWidth, kAtlasPitch};
        fb.Clear(cc);
      }
    }
    StageCtx bake = ctx;
    bake.width = kAtlasWidth, bake.height = kAtlasHeight;
    bool first_job = true;
    for (uint32_t i = 0; i < nj; ++i) {
      Job& j = jobs[i];
      CardGroup& cg = card_groups[j.type];
      ++counts.bakes;
      const bool offset = first_job && options.fault_atlas_offset;   // planted: this tile's picture one row low
      first_job = false;
      if (j.black) { cg.baked = true; tiles[j.type].valid = false; continue; }   // planted: cleared and not drawn
      const int32_t y0 = int32_t(AtlasTileY(j.type));
      const rdr::Rect main_vp{0, y0 + (offset ? 1 : 0), kAtlasTile, kAtlasTile}, main_sc{0, y0, kAtlasTile, kAtlasTile};
      const rdr::Rect top_vp{0, y0 - 1, kAtlasTile, kAtlasTile}, top_sc{0, y0 - 1, kAtlasTile, 1};
      const rdr::Rect bot_vp{0, y0 + 1, kAtlasTile, kAtlasTile}, bot_sc{0, y0 + int32_t(kAtlasTile), kAtlasTile, 1};
      InstancedStats st;
      uint32_t made = 0;
      for (int part = 0; part < 3; ++part) {   // top gutter, bottom gutter, the tile
        InstancedDraw dd = j.d;
        dd.posed = &pose_pool[size_t(j.pose)].meshes;
        const rdr::Rect& vp = part == 0 ? top_vp : part == 1 ? bot_vp : main_vp;
        dd.viewport_rect = &vp, dd.scissor_rect = part == 0 ? &top_sc : part == 1 ? &bot_sc : &main_sc;
        const uint32_t m = fe.DrawInstanced(dd, bake, fb, st);
        if (part == 2) made = m;
        counts.draws += m;
      }
      counts.refused += st.meshes_refused, counts.light_fallbacks += st.light_mismatch;
      if (counts.refusal.empty()) counts.refusal = st.first_refusal;
      cg.baked = made > 0;   // an empty bake is not a picture: its cards are skipped, not drawn
      tiles[j.type].valid = made > 0 && skip_same;
    }
  }

  rdr::ImageId CardImage(uint32_t type) {
    if (card_image[type] == rdr::kNoImage) {
      rdr::ImageDesc d;
      d.width = d.height = kCardPictureSize;
      d.format = rdr::Format::RGBA8;
      d.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageSampled);
      card_image[type] = renderer.CreateImage(d);
#if !WWE2K14_STREAM()
      if (card_image[type] != rdr::kNoImage) renderer.SetImageClass(card_image[type], rdr::kGpuCards);   // S103 GT (stream-free build only: see front_end.cpp)
#endif
    }
    return card_image[type];
  }
};

LiveCrowd::LiveCrowd(rdr::Renderer& renderer, FrontEnd& fe, BodySource& bodies, const LiveCrowdOptions& options) : impl_(new Impl(renderer, fe, bodies, options)) {}
LiveCrowd::~LiveCrowd() = default;

// S103 D2: build time (render thread, before the frame is recorded): the record's crowd objects classified ONCE by the predicate Record's gather uses, and every class of the arena asked for.
LiveCrowd::PrefetchResult LiveCrowd::Prefetch(const SceneTables& t) {
  Impl& s = *impl_;
  PrefetchResult r;
  s.SyncArena(t);
  std::array<bool, kClasses> need{};
  uint32_t cards = 0;
  const auto classify = [&](uint32_t asset, uint32_t n) {
    if (asset >= s.asset_info.size()) { r.unhandled += n; if (r.why.empty()) r.why = "object names an asset past the table"; return; }
    const AssetInfo& info = s.asset_info[asset];
    if (!info.valid) {
      if (info.crowd_like) { r.unhandled += n; if (r.why.empty()) r.why = "crowd entry this build does not know"; }
      else r.foreign += n;
      return;
    }
    if (s.arena < 0) { r.unhandled += n; if (r.why.empty()) r.why = "no arena in the record's assets"; return; }
    r.handled += n;
    need[(info.card ? 0 : info.lod) * kTypes + info.type] = true;
    if (info.card) cards += n;
  };
  for (const Object& o : t.objects)
    if (o.klass == rdr_adapter::DrawClass::Crowd && (o.flags & kObjVisible)) classify(o.asset, 1);
  for (const CrowdRun& run : t.crowd_runs) {
    if (uint64_t(run.first_member) + run.count > t.crowd_members.size()) { r.unhandled += run.count; if (r.why.empty()) r.why = "crowd run points past the tables"; continue; }
    classify(run.asset, run.count);
  }
  for (const CrowdSingleRow& sg : t.crowd_singles)   // S103 D4: a single person is a member of its body's class
    if (!(sg.flags & kCsfHidden) && !s.options.no_singles) {
      // nothing else draws a single row: a body this build cannot draw is a hole (never "foreign")
      if (sg.asset >= s.asset_info.size() || !s.asset_info[sg.asset].valid) { ++r.unhandled; if (r.why.empty()) r.why = "single person's asset is not a crowd body"; }
      else classify(sg.asset, 1);
    }
  uint32_t prop_rows = 0;
  if (!s.options.no_props)
    for (const CrowdPropRow& pr : t.crowd_props) prop_rows += ((pr.flags & kCpfHidden) || s.PropInPlaceSkipped(pr)) ? 0u : 1u;   // S103 SG2: a row the stream draws in place is not a hole on a live frame
  if (prop_rows && s.arena < 0) { r.unhandled += prop_rows; if (r.why.empty()) r.why = "signs / foam hands listed but the record names no arena"; prop_rows = 0; }
  if (prop_rows) need[0] = true;   // the props take the audience material of a LOD 0 body
  if (s.arena < 0) return r;
  // the needed classes first (they decide this frame), then every other class of the arena in the background
  s.adopted = 0;
  LiveCrowdCounts tmp;
  for (int pass = 0; pass < 2; ++pass)
    for (uint32_t k = 0; k < kClasses; ++k) {
      if (need[k] != (pass == 0)) continue;
      if (s.Resolve(k / kTypes, k % kTypes, tmp) != nullptr) continue;
      if (s.classes[k].failed) { if (need[k]) { ++r.failures; if (r.why.empty()) r.why = s.classes[k].why; } }
      else { r.all_resident = false; if (need[k]) ++r.misses; }
    }
  for (const CrowdSingleRow& sg : t.crowd_singles)   // S103 C1 review fix 4 (the classes are resolved above)
    if (!(sg.flags & kCsfHidden) && !s.options.no_singles && s.SingleNodesDisagree(sg)) {
      ++r.unhandled, --r.handled;
      if (r.why.empty()) r.why = "single person's palette does not cover its body's used slots";
    }
  if (prop_rows) {   // S103 D4: the prop set + one class per key the rows name; a key the set lacks is a counted hole with its reason
    const int st = s.EnsurePropSet();
    if (st == 1) { ++r.misses; r.all_resident = false; }
    else if (st == 2) { r.unhandled += prop_rows; if (r.why.empty()) r.why = "audience props: " + s.prop_set_why; }
    else if (!s.have_tmpl) { if (s.classes[0].failed) r.unhandled += prop_rows; }   // (the LOD 0 body pending / failed is counted by the loop above)
    else {
      std::map<uint32_t, uint32_t> per_key;
      for (const CrowdPropRow& pr : t.crowd_props)
        if (!(pr.flags & kCpfHidden) && !s.PropInPlaceSkipped(pr)) ++per_key[s.KeyFor(pr)];
      for (const auto& kv : per_key) {
        const Impl::PropClass& pc = s.ResolveProp(kv.first);
        if (pc.ok) r.handled += kv.second;
        else { r.unhandled += kv.second; if (r.why.empty()) r.why = pc.why; }
      }
      if (s.options.props_need_picture) {   // the game's picture of a sign has no source: a prop row the camera can see is a hole
        uint32_t seen = 0;
        const bool have_cam = t.has_camera && (t.camera.flags & kCamViewIsViewProj);
        for (const CrowdPropRow& pr : t.crowd_props) {
          if ((pr.flags & kCpfHidden) || s.PropInPlaceSkipped(pr)) continue;
          bool has_pic = false;   // S103 D6: a picture row for the key (the latch reader's copy of the guest's texture) satisfies the rule
          for (const CrowdPictureRow& pc : t.crowd_pictures) has_pic = has_pic || (pc.key == pr.picture && pc.nbytes);
          has_pic = has_pic || s.FilePictureKey(pr) != 0;   // S103 FHB: a foam hand has its file picture
          if (has_pic) continue;
          bool visible = true;   // no usable camera in the record: not provably out of view
          if (have_cam) {
            const float* v = t.camera.view.m;
            const float x = pr.c[3], y = pr.c[7], z = pr.c[11];
            float c4[4];
            for (int j = 0; j < 4; ++j) c4[j] = x * v[j] + y * v[4 + j] + z * v[8 + j] + v[12 + j];
            visible = c4[3] > 0.f && std::fabs(c4[0]) <= 1.25f * c4[3] && std::fabs(c4[1]) <= 1.25f * c4[3];
          }
          seen += visible ? 1u : 0u;
        }
        if (seen) {
          const uint32_t moved = seen < r.handled ? seen : r.handled;
          r.handled -= moved, r.unhandled += seen;
          if (r.why.empty()) r.why = "visible sign / foam hand rows have no picture source yet (the game's CPU picture of a sign is not read)";
        }
      }
    }
  }
  if (cards && r.misses == 0 && r.failures == 0 && (s.card_model < 0 || s.card_failed)) {   // the quad every card is drawn with cannot be drawn
    r.handled -= cards, r.unhandled += cards;
    if (r.why.empty()) r.why = s.card_model < 0 ? "no card quad model" : "DrawInstanced would refuse the card quad";
  }
  return r;
}

bool LiveCrowd::PrefetchArena(int arena) {   // S110 ENTRBODIES: Prefetch's background loop (every class of the arena) with the arena the game named
  Impl& s = *impl_;
  if (arena < 0) return true;
  s.SetArena(arena);
  s.adopted = 0;
  LiveCrowdCounts tmp;
  bool all = true;
  for (uint32_t k = 0; k < kClasses; ++k)
    if (s.Resolve(k / kTypes, k % kTypes, tmp) == nullptr && !s.classes[k].failed) all = false;
  return all;
}

void LiveCrowd::SetRecord(const SceneTables* record) { impl_->record = record; }
void LiveCrowd::SetSkipInPlaceProps(bool on) { impl_->options.skip_in_place_props = on; }

uint32_t LiveCrowd::LightHoles(const SceneTables& record) const {   // S103 L4
  uint32_t holes = 0;
  for (const auto& kv : impl_->lit_models) {
    const Impl::LitModel& m = kv.second;
    const bool has = g_lights_fn && g_lights_has && m.used >= 0 && size_t(m.used) < m.base.size() && m.base[size_t(m.used)].id >= 0 && g_lights_has(record, m.base[size_t(m.used)].id);
    holes += has ? 0 : 1;
  }
  return holes;
}

uint32_t LiveCrowd::BadBonesByClass(uint32_t lod, uint32_t type) const { return lod < kLods && type < kTypes ? impl_->bad_bones_by_class[lod * kTypes + type] : 0; }

void LiveCrowd::Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& fb) {
  Impl& s = *impl_;
  const SceneTables* t = s.record;
  if (stage == Stage::Maps) {
    counts_ = LiveCrowdCounts{};
    if (s.options.report_bodies)   // S103 SN (rig): what each resident body's skeleton / used slots / file map look like
      for (uint32_t k = 0; k < kClasses; ++k) {
        const BodyClass& bc = s.classes[k];
        if (!bc.ok) continue;
        char bl[120];
        std::snprintf(bl, sizeof bl, "L%uT%u nodes %u slots %u identity %s%s; ", k / kTypes, k % kTypes, bc.nodes, bc.slots, bc.identity_known ? (bc.identity_bad ? "bad " : "ok") : "unknown", bc.identity_bad ? std::to_string(bc.identity_bad).c_str() : "");
        counts_.bodies += bl;
      }
    ++frames_;
    s.rank.enabled = wwe2k14::start::Quality().density_track;   // S103 SPB (SP2 M3)
    s.rank.NextFrame(frames_);
    s.ngroups = 0, s.last_group = 0, s.pose_used = 0, s.adopted = 0;
    for (CardGroup& g : s.card_groups) g.first = kNoPalette, g.count = 0, g.worlds.clear(), g.pose = -1, g.baked = false;
    for (auto& kv : s.prop_worlds) kv.second.clear();   // S103 D4
    s.pic_cache.BeginFrame();   // S103 D6 review 1: looked up again every frame (the bytes may have changed)
    if (!t) return;
    s.SyncArena(*t);   // S103 D2: what the record's assets name, once per asset; the arena (a change releases the old one: the GPU is idle first)
    // ---- gather: the record's crowd objects and run members into classes ----
    const auto skip = [&](const char* why) { if (counts_.skip_why.empty()) counts_.skip_why = why; };
    bool in_singles = false;   // S103 D4
    const auto add_member = [&](const AssetInfo& info, bool floor, uint32_t first, uint32_t count, bool palette_ok, const float* rows) {
      if (s.options.only_singles && !in_singles) return;   // (the check's mask: the stands are left out, uncounted)
      // S103 SPB (SP2 M3; review M1): the player's CROWD DENSITY. A figure is drawn while its rank (0-99 of its world position: c76.w c77.w c78.w, the translation) is below the percent, as the stream binder does
      // (binder.h CrowdRank: the same tracker, so a waved sign keeps its rank) for the members AND the far cards (the stream's card path culls them too). 100 = every figure (the default: no test, no cost).
      // The decision is crowd_density.h Decide (unit test tools\crowd\crowd_density_test.cpp).
      const auto verdict = crowd_density::Decide(info.card, s.options.no_cards, s.options.no_members, wwe2k14::start::Quality().crowd_density, [&]() {
        uint32_t w3, w7, w11;
        std::memcpy(&w3, rows + 3, 4), std::memcpy(&w7, rows + 7, 4), std::memcpy(&w11, rows + 11, 4);
        return s.rank.Rank(w3, w7, w11);
      });
      if (verdict == crowd_density::Figure::Skipped) {
        if (info.card) ++counts_.cards_skipped, skip("cards left out (no_cards)");
        else ++counts_.members_skipped, skip("members left out (no_members)");
        return;
      }
      if (verdict == crowd_density::Figure::Culled) { ++counts_.density_culled; return; }
      if (info.card) {
        CardGroup& g = s.card_groups[info.type];
        if (g.first == kNoPalette && palette_ok) g.first = first, g.count = count;
        g.worlds.insert(g.worlds.end(), rows, rows + 12);
        return;
      }
      const bool pal = palette_ok && !s.options.fault_no_palettes;
      if (!pal) ++counts_.stored_pose;   // no palette in the record: the stored pose, drawn and counted (never dropped)
      Group& g = s.GroupFor(info.lod, info.type, pal ? first : kNoPalette, pal ? count : 0);
      g.worlds.insert(g.worlds.end(), rows, rows + 12);
      g.floor += floor;
    };
    float rows[12];
    for (const Object& o : t->objects) {
      if (o.klass != rdr_adapter::DrawClass::Crowd) continue;
      if (!(o.flags & kObjVisible)) {
        ++counts_.hidden_listed;
        if (!s.options.fault_ignore_hidden) continue;
      }
      ++counts_.records_crowd;
      if (o.asset >= s.asset_info.size()) { skip("object names an asset past the table"); continue; }
      const AssetInfo& info = s.asset_info[o.asset];
      if (!info.valid) {
        if (!info.crowd_like) { ++counts_.foreign; continue; }   // S103 D2: another applier's object (ApplyModels), not a skip
        skip("asset is not a crowd body or card"); ++(info.card ? counts_.cards_skipped : counts_.members_skipped); continue;
      }
      const bool palette_ok = (o.flags & kObjSkinned) && o.bones_count && size_t(o.bones_first) + o.bones_count <= t->bones.size();
      WorldRows(o.world, rows);
      const std::string_view id = t->Str(o.id);
      add_member(info, id.size() > 12 && id.compare(0, 12, "crowd:floor:") == 0, o.bones_first, o.bones_count, palette_ok, rows);
    }
    for (const CrowdRun& run : t->crowd_runs) {   // run markers (S102 T19): read directly, no per-member object or id
      if (run.asset >= s.asset_info.size() || uint64_t(run.first_member) + run.count > t->crowd_members.size()) { skip("crowd run points past the tables"); continue; }
      const AssetInfo& info = s.asset_info[run.asset];
      const bool palette_ok = run.skinned && run.bones_count && size_t(run.bones_first) + run.bones_count <= t->bones.size();
      for (uint32_t k = 0; k < run.count; ++k) {
        ++counts_.records_crowd, ++counts_.run_members;
        if (!info.valid) {
          if (!info.crowd_like) { ++counts_.foreign; continue; }
          skip("run asset is not a crowd body or card"); ++(info.card ? counts_.cards_skipped : counts_.members_skipped); continue;
        }
        add_member(info, run.kind == kRunFloor, run.bones_first, run.bones_count, palette_ok, t->crowd_members[run.first_member + k].c);
      }
    }
    // ---- S103 D4: the single standing people: one body each, placed by its own row (members of the body's class) ----
    for (const CrowdSingleRow& sg : t->crowd_singles) {
      ++counts_.singles_listed;
      if (s.options.no_singles) { ++counts_.members_skipped; skip("single people left out (no_singles)"); continue; }
      if (sg.flags & kCsfHidden) {
        ++counts_.hidden_listed;
        if (!s.options.fault_ignore_hidden) continue;
      }
      ++counts_.records_crowd;
      if (sg.asset >= s.asset_info.size()) { ++counts_.members_skipped; skip("single person names an asset past the table"); continue; }
      const AssetInfo& info = s.asset_info[sg.asset];
      if (!info.valid) { ++counts_.members_skipped; skip("single person's asset is not a crowd body"); continue; }
      if (s.SingleRefusal(sg)) { ++counts_.members_skipped, ++counts_.singles_node_mismatch; skip("single person's palette is shorter than the body's used palette slots"); continue; }   // S103 SN: the palette must cover the body's used slots
      if (s.classes[info.lod * kTypes + info.type].ok && s.classes[info.lod * kTypes + info.type].identity_bad) ++counts_.singles_slot_map_nonidentity;   // census only
      if (s.classes[info.lod * kTypes + info.type].ok && s.classes[info.lod * kTypes + info.type].nodes != s.RowNodes(sg)) ++counts_.singles_nodes_differ;   // census only
      const bool palette_ok = sg.bones_count && size_t(sg.bones_first) + sg.bones_count <= t->bones.size();
      static const float kOrigin[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      float sc[12];
      std::memcpy(sc, sg.c, sizeof sc);
      if (s.options.fault_singles_transposed)
        for (int a = 0; a < 3; ++a)
          for (int b = 0; b < 3; ++b) sc[4 * a + b] = sg.c[4 * b + a];
      in_singles = true;
      add_member(info, false, sg.bones_first, s.RowNodes(sg), palette_ok, s.options.fault_singles_identity ? kOrigin : sc);
      in_singles = false;
    }
    // ---- S103 D4: the signs / foam hands: the row's c76-c78 as the loop transposed them, grouped by mesh key ----
    for (const CrowdPropRow& pr : t->crowd_props) {
      ++counts_.props_listed;
      if (s.options.no_props) { ++counts_.props_skipped; skip("signs / foam hands left out (no_props)"); continue; }
      if (pr.flags & kCpfHidden) { ++counts_.props_hidden; continue; }
      if (s.PropInPlaceSkipped(pr)) { ++counts_.props_in_place; continue; }   // S103 SG2: the stream draws it in place
      float c[12];
      std::memcpy(c, pr.c, sizeof c);
      if (s.options.fault_props_transposed)   // planted: the 3x3 read transposed
        for (int a = 0; a < 3; ++a)
          for (int b = 0; b < 3; ++b) c[4 * a + b] = pr.c[4 * b + a];
      s.pic_record = t;
      uint32_t pic_key = pr.picture;
      if (!s.RecordHasPicture(pic_key, *t))   // S103 FHB: no valid record row for the key: a foam hand's file picture
        if (const uint32_t fk = s.FilePictureKey(pr)) pic_key = fk;
      const uint32_t group_pic = s.PictureFor(pic_key, *t) == s.prop_picture ? 0u : pic_key;   // S103 D6   // the grey fallback is ONE group whatever the key
      {   // S103 CSF census: where each drawn prop's picture came from, per site (sign / hand)
        uint32_t* pc = pr.kind == kCpSign ? counts_.sign_pic : counts_.hand_pic;
        if (!group_pic) ++pc[2];
        else if (s.IsFileKey(group_pic)) ++pc[0];
        else ++pc[1];
      }
      std::vector<float>& w = s.prop_worlds[{s.KeyFor(pr), group_pic}];
      w.insert(w.end(), c, c + 12);
    }
    if (s.options.fault_inst_first_world) {   // planted: every instance of a draw takes the first member's world
      for (size_t i = 0; i < s.ngroups; ++i)
        for (size_t k = 12; k < s.groups[i].worlds.size(); k += 12) std::memcpy(&s.groups[i].worlds[k], s.groups[i].worlds.data(), 12 * sizeof(float));
      for (CardGroup& g : s.card_groups)
        for (size_t k = 12; k < g.worlds.size(); k += 12) std::memcpy(&g.worlds[k], g.worlds.data(), 12 * sizeof(float));
    }
    if (s.arena < 0) return;
    // ---- resolve the classes and pose each (lod, type, palette) ONCE ----
    for (size_t i = 0; i < s.ngroups; ++i) {
      Group& g = s.groups[i];
      BodyClass* body = s.Resolve(g.lod, g.type, counts_);
      if (!body) { counts_.members_skipped += uint32_t(g.worlds.size() / 12); g.pose = -1; continue; }
      g.pose = s.Pose(g.lod, g.type, g.first, g.count, *t, *body, counts_);
    }
    for (auto& kv : s.prop_worlds) {   // S103 D4: each key's class (built at Prefetch; here only when no Prefetch ran)
      if (kv.second.empty()) continue;
      if (!s.PreparePropClass(kv.first.first, counts_)) { counts_.props_skipped += uint32_t(kv.second.size() / 12); kv.second.clear(); }
    }
    // ---- the card bake: our own 64 x 64 picture per type with cards, from the type's near body posed with the card palette, LIT with the frame's crowd set (S103 L4: the game's bake runs skinned_lit_lm_vertex; the far card quad is then drawn UNLIT, the picture is already lit) ----
    float ortho[16] = {2.f / 164.f, 0, 0, 0, 0, -2.f / 250.f, 0, 0, 0, 0, 0.001f, 0, 0, -1.f, 0.5f, 1.f};  // the game's card camera (CROWD_CARDS_PROMOTION_MAP 1.3, lane CC: x 2/164, y -2/250 offset -1, z 0.001 + 0.5)
    if (s.options.fault_cards_back) ortho[0] = -ortho[0], ortho[10] = -ortho[10];   // planted: the camera looks from behind (the back of the person, not mirrored)
    static const float kEye[3] = {0, 0, 0};
    static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // S103 CB1: ONE pass for every picture (card_atlas_rule.h), at 1x anisotropy only (the sampler's taps then never leave the gutter row)
    s.frame_atlas = (s.options.card_atlas & kCardAtlasOnePass) && CardAtlasUsable(sampler_table::AnisoWanted());   // (the far-card draw below forces slot 0 to 1x while the atlas is on: no second read of the setting)
    if (s.frame_atlas) {
      s.RecordAtlasBake(counts_, ctx, fb, ortho, kEye, kIdentity);
      return;
    }
    for (uint32_t type = 0; type < kTypes; ++type) {
      CardGroup& cg = s.card_groups[type];
      if (cg.worlds.empty()) continue;
      const uint32_t n = uint32_t(cg.worlds.size() / 12);
      BodyClass* body = s.Resolve(0, type, counts_);
      if (!body || s.card_model < 0) { counts_.cards_skipped += n; if (body && counts_.skip_why.empty()) counts_.skip_why = "no card quad model"; cg.worlds.clear(); continue; }
      const rdr::ImageId image = s.CardImage(type);
      if (image == rdr::kNoImage) { counts_.cards_skipped += n; if (counts_.skip_why.empty()) counts_.skip_why = "no card picture image"; cg.worlds.clear(); continue; }
      if (s.card_depth == rdr::kNoImage) {
        rdr::ImageDesc d;
        d.width = d.height = kCardPictureSize;
        d.format = rdr::Format::Depth;
        d.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageTransient);
        s.card_depth = s.renderer.CreateImage(d);
#if !WWE2K14_STREAM()
        if (s.card_depth != rdr::kNoImage) s.renderer.SetImageClass(s.card_depth, rdr::kGpuCards);   // S103 GT
#endif
      }
      rdr::TargetsOp targets;
      targets.color[0].image = image;
      targets.color_count = 1;
      targets.depth.image = s.card_depth;
      fb.SetTargets(targets);
      rdr::ClearOp clear;
      clear.flags = rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil;
      clear.full = 1;
      clear.depth = 1.0f;
      fb.Clear(clear);
      ++counts_.bakes, ++counts_.bake_passes;
      if (s.options.fault_cards_black) { cg.baked = true; continue; }  // planted: the picture stays cleared (transparent black: the cut-out leaves nothing)
      const bool pal = cg.first != kNoPalette && !s.options.fault_no_palettes;
      cg.pose = s.Pose(0, type, pal ? cg.first : kNoPalette, pal ? cg.count : 0, *t, *body, counts_);   // (the members' pose of the same palette is reused)
      InstancedDraw d;
      d.no_blend = true, d.model = body->model, d.posed = &s.pose_pool[size_t(cg.pose)].meshes, d.worlds = kIdentity, d.count = 1, d.view_proj = ortho, d.eye = kEye, d.game_space = true, d.lights = s.LightsOf(body->model, counts_);
      StageCtx bake = ctx;
      bake.width = bake.height = kCardPictureSize;
      InstancedStats st;
      d.group = rdr::kGrpCrowdCards;   // S103 GSTP (the bake passes are 64x64 colour-class passes: their draws carry a group too)
      const uint32_t made = s.fe.DrawInstanced(d, bake, fb, st);
      counts_.draws += made;
      counts_.refused += st.meshes_refused, counts_.light_fallbacks += st.light_mismatch;
      if (counts_.refusal.empty()) counts_.refusal = st.first_refusal;
      cg.baked = made > 0;   // an empty bake is not a picture: its cards are skipped, not drawn
    }
    return;
  }
  if (stage != Stage::AfterOpaque || !t) return;
  // ---- the members: one instanced draw per (class, mesh) ----
  for (size_t i = 0; i < s.ngroups; ++i) {
    Group& g = s.groups[i];
    if (g.pose < 0) continue;   // (counted as skipped at Maps)
    const uint32_t n = uint32_t(g.worlds.size() / 12);
    InstancedDraw d;
    d.model = s.classes[g.lod * kTypes + g.type].model, d.posed = &s.pose_pool[size_t(g.pose)].meshes, d.worlds = g.worlds.data(), d.count = n, d.view_proj = ctx.view_proj, d.eye = ctx.eye, d.game_space = true, d.lights = s.LightsOf(d.model, counts_);   // S103 L4
    InstancedStats st;
    d.group = g.floor ? rdr::kGrpCrowdFloor : rdr::kGrpCrowd;   // S103 GSTP
    const uint32_t made = s.fe.DrawInstanced(d, ctx, fb, st);
    counts_.refused += st.meshes_refused, counts_.light_fallbacks += st.light_mismatch;
    if (counts_.refusal.empty()) counts_.refusal = st.first_refusal;
    counts_.draws += made;
    if (made) counts_.members_drawn += n, counts_.floor_drawn += g.floor, ++counts_.classes_drawn;
    else counts_.members_skipped += n, (counts_.skip_why.empty() ? (void)(counts_.skip_why = "DrawInstanced drew nothing for a class: " + st.first_refusal) : (void)0);
  }
  // ---- the far cards: one instanced draw of the quad per type, the type's baked picture as slot 0 ----
  for (uint32_t type = 0; type < kTypes; ++type) {
    CardGroup& cg = s.card_groups[type];
    const uint32_t n = uint32_t(cg.worlds.size() / 12);
    if (!n) continue;
    if (s.card_model < 0 || !cg.baked) { counts_.cards_skipped += n; if (counts_.skip_why.empty()) counts_.skip_why = "card picture not baked"; continue; }
    InstancedDraw d;
    d.model = s.card_model, d.posed = s.frame_atlas ? s.AtlasQuad(type) : &s.card_quad, d.worlds = cg.worlds.data(), d.count = n, d.slot0 = s.frame_atlas ? s.atlas_image : s.card_image[type], d.tag_salt = s.frame_atlas ? type + 1u : 0u, d.aniso_override = s.frame_atlas ? 1 : 0;   // S103 CB1: the atlas tile's quad
    d.view_proj = ctx.view_proj, d.eye = ctx.eye, d.game_space = true, d.cull_none = true, d.clamp_slot0 = true;   // (S103 L4: the quad is the UNLIT variant: no light is read)
    InstancedStats st;
    d.group = rdr::kGrpCrowdCards;   // S103 GSTP
    const uint32_t made = s.fe.DrawInstanced(d, ctx, fb, st);
    counts_.refused += st.meshes_refused, counts_.light_fallbacks += st.light_mismatch;
    if (counts_.refusal.empty()) counts_.refusal = st.first_refusal;
    counts_.draws += made;
    if (made) counts_.cards_drawn += n; else counts_.cards_skipped += n;
  }
  // ---- S103 D4: the signs / foam hands: one instanced draw per mesh key, the neutral picture as slot 0 ----
  for (auto& kv : s.prop_worlds) {
    const uint32_t n = uint32_t(kv.second.size() / 12);
    if (!n) continue;
    auto it = s.prop_classes.find(kv.first.first);
    if (it == s.prop_classes.end() || !it->second.ok) { counts_.props_skipped += n; if (counts_.skip_why.empty()) counts_.skip_why = "prop class not resident"; continue; }
    InstancedDraw d;
    d.model = it->second.model, d.posed = &it->second.posed, d.worlds = kv.second.data(), d.count = n;
    d.slot0 = kv.first.second && s.pic_record ? s.PictureFor(kv.first.second, *s.pic_record) : s.PropPicture();   // S103 D6: the record's picture of the group
    const bool neutral = d.slot0 == s.prop_picture;
    if (s.IsFileKey(kv.first.second) && !neutral) {   // S103 FHB
      counts_.props_file_picture += n;
      for (const CrowdPictureRow& r : s.file_rows)
        if (r.key == kv.first.second) {
          uint64_t h = 1469598103934665603ull;
          for (uint32_t i = 0; i < r.nbytes; ++i) h = (h ^ r.bytes[i]) * 1099511628211ull;
          char pfb[96];
          std::snprintf(pfb, sizeof pfb, "%s%u %ux%u fmt%u fnv %016llX; ", (r.key & 0xF0000000u) == Impl::kSignFileKeyBit ? "S" : "", r.key & 0x0FFFFFFFu, unsigned(r.width), unsigned(r.height), r.format, (unsigned long long)h);
          if (counts_.file_pictures.find(pfb) == std::string::npos) counts_.file_pictures += pfb;
          break;
        }
    }
    d.view_proj = ctx.view_proj, d.eye = ctx.eye, d.game_space = true, d.lights = s.LightsOf(d.model, counts_);   // S103 L4 (S103 SG: no cull_none: the prop material culls back faces, as the game)
    InstancedStats st;
    d.group = rdr::kGrpCrowd;   // S103 GSTP (the signs / foam hands are the crowd's)
    const uint32_t made = s.fe.DrawInstanced(d, ctx, fb, st);
    counts_.refused += st.meshes_refused, counts_.light_fallbacks += st.light_mismatch;
    if (counts_.refusal.empty()) counts_.refusal = st.first_refusal;
    counts_.draws += made;
    if (made) counts_.props_drawn += n, counts_.props_neutral_picture += neutral ? n : 0u; else counts_.props_skipped += n;
  }
  { Impl::PicBackend b{s}; s.pic_cache.EndFrame(b); }   // S103 D6 review 6
}


// S103 B4F: the arena's stage page (rf_arena_live.cpp UpdatePages, same key and pixels) for every resident crowd model; a model resident later takes it when it is made (Impl::RegPage).
void LiveCrowd::SetStagePage(uint64_t key, uint32_t w, uint32_t h, const uint8_t* rgba) {
  if (!rgba || !w || !h) return;
  FrontEnd& fe = impl_->fe;
  {   // S103 UP: every slot joins the arena's one page image BEFORE any upload (a sibling on the same private image must not stand in for it); a slot that left a retired set's group follows its new image (review F1)
    const std::vector<CrowdPages::Slot>& sl = impl_->pages.slots();
    for (size_t i = 0; i < sl.size(); ++i) {
      const int mdl = sl[i].model;
      const size_t tex = size_t(sl[i].tex);
      fe.JoinCurrentPage(mdl, tex);
      const uint64_t img = fe.TextureImage(mdl, tex);
      if (img && img != sl[i].image) impl_->pages.Rebind(i, img, fe.PageKeyOf(mdl, tex));
    }
    impl_->joined_group = fe.current_page_group();
  }
  impl_->pages.Set(key, w, h, rgba, [&](int m, int t, uint32_t pw, uint32_t ph, const uint8_t* px) { return fe.UpdatePage(m, size_t(t), key, pw, ph, px); });   // S103 UP: the group's one image (key held = nothing sent)
}
bool LiveCrowd::ReadCardPicture(uint32_t type, std::vector<uint8_t>& tile, std::vector<uint8_t>& gutter_top, std::vector<uint8_t>& gutter_bottom) {   // S103 CB1
  Impl& s = *impl_;
  tile.clear(), gutter_top.clear(), gutter_bottom.clear();
  if (type >= kTypes || !s.card_groups[type].baked) return false;
  std::vector<uint8_t> px;
  uint32_t w = 0, h = 0;
  if (s.frame_atlas) {
    if (s.atlas_image == rdr::kNoImage || !s.renderer.ReadImage(rdr::ImageRef{s.atlas_image, 0, 0, 0}, px, w, h) || w != kAtlasWidth || h != kAtlasHeight) return false;
    const size_t row = size_t(kAtlasWidth) * 4, y0 = AtlasTileY(type);
    tile.assign(px.begin() + long(y0 * row), px.begin() + long((y0 + kAtlasTile) * row));
    gutter_top.assign(px.begin() + long((y0 - 1) * row), px.begin() + long(y0 * row));
    gutter_bottom.assign(px.begin() + long((y0 + kAtlasTile) * row), px.begin() + long((y0 + kAtlasTile + 1) * row));
    return true;
  }
  if (s.card_image[type] == rdr::kNoImage || !s.renderer.ReadImage(rdr::ImageRef{s.card_image[type], 0, 0, 0}, px, w, h) || w != kCardPictureSize || h != kCardPictureSize) return false;
  tile = std::move(px);
  return true;
}
uint64_t LiveCrowd::StagePageKey() const {
  const uint64_t g = impl_->fe.current_page_group();
  if (g != 0 && impl_->joined_group != g) return ~uint64_t(0);   // S103 UP: not on the live set's page group yet = stale (the arena then calls SetStagePage once: every slot joins)
  return impl_->pages.HeldKey();
}   // S103 B4F review 2: a refused upload keeps the crowd stale

}  // namespace rdr_scene::live
