// Test seam: a built-in model (no game files) so the viewer runs before the loaders exist.
// Scale like the game: a wrestler is ~200 tall; the "arena" floor here is a few thousand units wide.
#include <algorithm>
#include <cstdint>
#include <cstring>

// (review) the viewer's other sources use stb_image (stbi_load*, pac.h's zlib decoder): the test seam holds the implementation too,
// as the game seam does, so `build_viewer.ps1 -Seam test` links
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include "loader_seam.h"

using namespace assets;

static std::string g_game;
void ViewerInit(const std::string& game_path) { g_game = game_path; }
std::string ViewerSetDlc(bool, const std::string&, const std::string&) { return "DLC: the test seam has no game files"; }
std::string ViewerDlcJson() { return "[]"; }
std::vector<std::string> ViewerDlcMotionFiles() { return {}; }

static uint32_t Pack(float r, float g, float b, float a) {
  auto c = [](float v) { return (uint32_t)(v * 255.0f + 0.5f); };
  return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

// Axis-aligned box with per-face normals, uv0 tiled `tile` times per face, uv1 = planar (x,z) over [lo,hi] of the lightmap area.
static void AddBox(Mesh& m, const float lo[3], const float hi[3], uint32_t color, float tile, const float lm_lo[2],
                   const float lm_hi[2]) {
  struct Face { int axis; int sign; };
  const Face faces[6] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, 1}, {2, -1}};
  for (auto f : faces) {
    int a1 = (f.axis + 1) % 3, a2 = (f.axis + 2) % 3;
    uint32_t base = (uint32_t)m.vertices.size();
    for (int k = 0; k < 4; k++) {
      float u = (k == 1 || k == 2) ? 1.f : 0.f, v = (k >= 2) ? 1.f : 0.f;
      Vertex vx{};
      float p[3];
      p[f.axis] = f.sign > 0 ? hi[f.axis] : lo[f.axis];
      p[a1] = lo[a1] + (hi[a1] - lo[a1]) * u;
      p[a2] = lo[a2] + (hi[a2] - lo[a2]) * v;
      memcpy(vx.pos, p, sizeof p);
      vx.normal[f.axis] = (float)f.sign;
      vx.uv0[0] = u * tile; vx.uv0[1] = v * tile;
      vx.uv1[0] = (p[0] - lm_lo[0]) / (lm_hi[0] - lm_lo[0]);
      vx.uv1[1] = (p[2] - lm_lo[1]) / (lm_hi[1] - lm_lo[1]);
      vx.color = color;
      vx.weights[0] = 1;
      m.vertices.push_back(vx);
    }
    if (f.sign > 0) { uint32_t i[6] = {0, 1, 2, 0, 2, 3}; for (auto x : i) m.indices.push_back(base + x); }
    else { uint32_t i[6] = {0, 2, 1, 0, 3, 2}; for (auto x : i) m.indices.push_back(base + x); }
  }
}

static void Grow(Model& md, const Mesh& m) {
  bool first = md.meshes.size() == 1 && &md.meshes[0] == &m;
  (void)first;
  for (auto& v : m.vertices)
    for (int i = 0; i < 3; i++) {
      md.bounds_min[i] = std::min(md.bounds_min[i], v.pos[i]);
      md.bounds_max[i] = std::max(md.bounds_max[i], v.pos[i]);
    }
}

static void BuildTest(Model& md, const std::string& label) {
  md.label = label;
  // Textures: 0 = BC1 checker 64x64, 1 = B4G4R4A4 lightmap gradient 32x32, 2 = R8G8B8A8 stripes 64x64.
  {
    Texture t; t.name = "checker_bc1"; t.format = TexFormat::kBC1; t.width = t.height = 64; t.mips = 1;
    for (int by = 0; by < 16; by++)
      for (int bx = 0; bx < 16; bx++) {
        uint8_t blk[8] = {0xFB, 0xDE, 0x08, 0x42, 0, 0, 0, 0};  // c0 = 0xDEFB (light) > c1 = 0x4208 (dark)
        uint8_t idx = (((bx / 2) + (by / 2)) & 1) ? 0x55 : 0x00;
        memset(blk + 4, idx, 4);
        t.data.insert(t.data.end(), blk, blk + 8);
      }
    md.textures.push_back(std::move(t));
  }
  {
    Texture t; t.name = "lightmap_4444"; t.format = TexFormat::kB4G4R4A4; t.width = t.height = 32; t.mips = 1;
    for (int y = 0; y < 32; y++)
      for (int x = 0; x < 32; x++) {
        unsigned v = 6 + (unsigned)(x * 9 / 31);  // 6..15
        uint16_t px = (uint16_t)((0xFu << 12) | (v << 8) | (v << 4) | v);
        t.data.push_back((uint8_t)(px & 0xFF));
        t.data.push_back((uint8_t)(px >> 8));
      }
    md.textures.push_back(std::move(t));
  }
  {
    Texture t; t.name = "stripes_rgba"; t.format = TexFormat::kR8G8B8A8; t.width = t.height = 64; t.mips = 1;
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
        bool s = ((x / 8) & 1) != 0;
        uint8_t px[4] = {(uint8_t)(s ? 200 : 120), (uint8_t)(s ? 40 : 40), (uint8_t)(s ? 40 : 120), 255};
        t.data.insert(t.data.end(), px, px + 4);
      }
    md.textures.push_back(std::move(t));
  }
  Material floor_m; floor_m.shader = "test_floor"; floor_m.diffuse = 0; floor_m.lightmap = 1;
  Material ring_m; ring_m.shader = "test_ring"; ring_m.diffuse = 2;
  Material body_m; body_m.shader = "test_vertex_colour";
  Material glass_m; glass_m.shader = "test_trans"; glass_m.alpha_blend = true;
  glass_m.diffuse_color[0] = 0.3f; glass_m.diffuse_color[1] = 0.6f; glass_m.diffuse_color[2] = 1.0f; glass_m.diffuse_color[3] = 0.45f;
  md.materials = {floor_m, ring_m, body_m, glass_m};

  float lm_lo[2] = {-1500, -1500}, lm_hi[2] = {1500, 1500};
  Mesh floor_mesh; floor_mesh.material = 0; floor_mesh.part = "floor";
  { float lo[3] = {-1500, -20, -1500}, hi[3] = {1500, 0, 1500}; AddBox(floor_mesh, lo, hi, 0xFFFFFFFF, 12, lm_lo, lm_hi); }
  Mesh ring_mesh; ring_mesh.material = 1; ring_mesh.part = "ring";
  { float lo[3] = {-350, 0, -350}, hi[3] = {350, 40, 350}; AddBox(ring_mesh, lo, hi, 0xFFFFFFFF, 2, lm_lo, lm_hi);
    float p[4][2] = {{-350, -350}, {350, -350}, {-350, 350}, {350, 350}};
    for (auto& q : p) { float l2[3] = {q[0] - 8, 40, q[1] - 8}, h2[3] = {q[0] + 8, 140, q[1] + 8}; AddBox(ring_mesh, l2, h2, 0xFFFFFFFF, 1, lm_lo, lm_hi); } }
  Mesh body; body.material = 2; body.part = "body";
  {
    float z0[2] = {-1, 1};
    float l1[3] = {-30, 40, -12}, h1[3] = {-4, 130, 12};   // left leg
    float l2[3] = {4, 40, -12}, h2[3] = {30, 130, 12};     // right leg
    float l3[3] = {-36, 130, -15}, h3[3] = {36, 200, 15};  // torso
    float l4[3] = {-14, 205, -14}, h4[3] = {14, 236, 14};  // head
    float l5[3] = {-62, 140, -10}, h5[3] = {-36, 198, 10}; // left arm
    float l6[3] = {36, 140, -10}, h6[3] = {62, 198, 10};   // right arm
    AddBox(body, l1, h1, Pack(0.2f, 0.2f, 0.8f, 1), 1, z0, z0);
    AddBox(body, l2, h2, Pack(0.2f, 0.2f, 0.8f, 1), 1, z0, z0);
    AddBox(body, l3, h3, Pack(0.9f, 0.7f, 0.2f, 1), 1, z0, z0);
    AddBox(body, l4, h4, Pack(0.9f, 0.7f, 0.55f, 1), 1, z0, z0);
    AddBox(body, l5, h5, Pack(0.9f, 0.7f, 0.55f, 1), 1, z0, z0);
    AddBox(body, l6, h6, Pack(0.9f, 0.7f, 0.55f, 1), 1, z0, z0);
    for (auto& v : body.vertices) { v.uv1[0] = v.uv1[1] = 0; }
  }
  Mesh glass; glass.material = 3; glass.part = "glass";
  {
    // an upright translucent quad in front of the wrestler (alpha-blended pass)
    float p[4][3] = {{-200, 20, 300}, {200, 20, 300}, {200, 320, 300}, {-200, 320, 300}};
    for (auto& q : p) {
      Vertex vx{};
      memcpy(vx.pos, q, sizeof q);
      vx.normal[2] = -1; vx.color = 0xFFFFFFFF; vx.weights[0] = 1;
      glass.vertices.push_back(vx);
    }
    uint32_t i[6] = {0, 1, 2, 0, 2, 3};
    glass.indices.assign(i, i + 6);
  }
  md.meshes.push_back(std::move(floor_mesh));
  md.meshes.push_back(std::move(ring_mesh));
  md.meshes.push_back(std::move(body));
  md.meshes.push_back(std::move(glass));
  bool first = true;
  for (auto& m : md.meshes) {
    if (first) {
      for (int i = 0; i < 3; i++) md.bounds_min[i] = md.bounds_max[i] = m.vertices[0].pos[i];
      first = false;
    }
    Grow(md, m);
  }
  md.warnings.push_back("test model (loader_seam_test.cpp)");
}

bool ViewerLoad(const std::string& kind, int id, int attire, Model& out, std::string& err) {
  if (kind != "wrestler" && kind != "arena") { err = "unknown kind '" + kind + "'"; return false; }
  out = Model();
  BuildTest(out, kind + " " + std::to_string(id) + " attire " + std::to_string(attire));
  return true;
}

std::vector<int> ViewerList(const std::string& kind) {
  if (kind == "wrestler") return {1, 2, 3};
  if (kind == "arena") return {1};
  return {};
}

bool ViewerReadFile(const std::string&, std::vector<uint8_t>&, std::string& err) { return err = "test seam has no game files", false; }

bool ViewerCrowd(bool, int, int, std::string& err) { return err = "the test seam has no crowd", false; }
bool ViewerStageKey(bool, double, std::string& err) { return err = "the test seam has no stage key", false; }
bool ViewerStageMedia(const std::string&, int, bool, const std::vector<std::pair<std::string, std::string>>&, std::string& err) { return err = "the test seam has no stage media", false; }
void ViewerStageWrestlers(const std::string&, std::vector<int>& ids, std::string& used_dir) { ids.clear(), used_dir.clear(); }
bool ViewerEntrance(int, assets::entrance::Show&, std::string& err) { return err = "the test seam has no entrances", false; }
int ViewerChampBank(int) { return -1; }
int ViewerPropPach(unsigned) { return -1; }
int ViewerPropPachDir(unsigned) { return -1; }
bool ViewerPropWorn(int) { return false; }
bool ViewerParticles(int, assets::particles::Bank&, std::string& err) { return err = "the test seam has no particles", false; }
bool ViewerSceneLights(int, assets::particles::SceneLights&) { return false; }  // (the caller clears its copy first)
int ViewerBeltTwin(int, int) { return -1; }
bool ViewerEntranceRig(int, assets::entrance::Show&, std::string& err) { return err = "the test seam has no entrances", false; }
int ViewerCustomAttires(int) { return 0; }
std::string ViewerCheckCharacterFile(const std::string&, std::string& warning) { return warning.clear(), ""; }
int ViewerEntranceAttire(int, int) { return -1; }
int ViewerModelKey(int, int) { return -1; }
bool ViewerPropBobj(unsigned) { return false; }
bool ViewerEvtTable(int, std::vector<std::vector<int>>&, std::string& err) { return err = "the test seam has no event tables", false; }
