// body-scale: none (arena loader unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// S108 lane BAN3: the arena UV stream. The game's packer (sub_82BD4BE0) reads UV pairs only from S2 for every mesh; S1 is skin pairs.
// Loads bg53 and bg04 through the production loader (LoadArena -> LoadJboy) and checks, against the S2 pair read independently from the player's
// own file (its own header walk, no loader code):
//   (a) every loaded stage mesh's uv0 equals its S2 pair on every vertex (every arena mesh, stage slots and ring set; the non-skinned count is printed);
//   (b) bg53 slot 114 (the WRESTLEMANIA logo strip) uv0 spans 0..1 on both axes (the old rule gave the constant (0,1) on all 24 vertices).
// WWE_BAN3_PLANT=1: the old rule applied to the loaded model (uv0 = S1 unless S1 is constant while S2 varies, the material has no light map AND it has a
//   diffuse texture: the old jboy.h swap's three conditions); check (a) must FAIL on exactly the meshes the unfixed loader got wrong: 64 on bg53 (the 63 light-map
//   meshes + slot 15 mesh 0, a yBG_Default whose diffuse does not resolve) and 27 on bg04 [C: this unit built against the pre-BAN3 src\ (e3b95f953) reports
//   the same 64 / 27 with no plant]. The count is asserted (`.plant_count`), so a plant that fails for another reason does not pass.
//   ban3_unit --game DIR        exit 0 = all PASS, 1 = a check failed, 2 = usage / cannot read
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"

namespace {
int g_bad = 0;
void Check(bool ok, const std::string& what, const std::string& detail = std::string()) {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  ", detail.c_str());
  if (!ok) ++g_bad;
}
uint32_t B32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float BF(const uint8_t* p) { const uint32_t u = B32(p); float f; std::memcpy(&f, &u, 4); return f; }

// The file's streams for one mesh record: S1 / S2 base pointers (JBOY body offsets), vertex count.
struct Rec { bool ok = false; uint32_t n = 0, s1 = 0, s2 = 0; };
Rec ReadRec(const assets::Bytes& blob, int mi) {
  Rec r;
  if (blob.size() < 0x48 || std::memcmp(blob.data(), "JBOY", 4) != 0) return r;
  const uint8_t* b = blob.data() + 8;
  const uint32_t nmesh = B32(b + 0x10), tab = B32(b + 0x14);
  if (mi < 0 || uint32_t(mi) >= nmesh) return r;
  const uint8_t* rec = b + tab + 0xB8ull * uint32_t(mi);
  r.n = B32(rec), r.s1 = B32(rec + 0x6C), r.s2 = B32(rec + 0x70), r.ok = true;
  return r;
}

void Arena(launcher::GameFiles& files, int id, bool plant, int expect_plant_bad) {
  assets::Model m;
  std::string err;
  const bool ok = assets::LoadArena(files, id, m, err);
  char lab[64], buf[200];
  std::snprintf(lab, sizeof lab, "bg%02d", id);
  Check(ok, std::string(lab) + ".loads", ok ? m.label : err);
  if (!ok) return;
  assets::pac::Pac p;
  std::string e2;
  if (!assets::pac::Open(files, assets::load_detail::ArenaRel(id), p, e2)) return Check(false, std::string(lab) + ".opens_pac", e2);
  const assets::pac::Pach& h = p.pachs[0];
  size_t checked = 0, nonskin = 0, bad = 0, bad_nonskin = 0, no_ref = 0;
  for (const assets::Mesh& me : m.meshes) {
    if (me.src_kind != 1 || me.src_record < 0) continue;   // every arena mesh (stage slots and the ring set)
    const assets::pac::Ent* e = assets::pac::Find(h, uint32_t(me.src_key));
    if (!e) { ++no_ref; continue; }
    const assets::Bytes blob = assets::pac::Decode(p.bytes.data() + e->off, e->size);
    const Rec rc = ReadRec(blob, me.src_record);
    if (!rc.ok || rc.n != me.vertices.size()) { ++no_ref; continue; }
    const uint8_t* body = blob.data() + 8;
    const bool skinned_mesh = me.src_skin_pairs != 0;
    bool s1_const = rc.n > 1, s2_vary = false;
    for (uint32_t v = 1; v < rc.n; ++v) {
      if (std::memcmp(body + rc.s1 + 8ull * v, body + rc.s1, 8) != 0) s1_const = false;
      if (std::memcmp(body + rc.s2 + 8ull * v, body + rc.s2, 8) != 0) s2_vary = true;
    }
    const bool has_mat = me.material >= 0 && size_t(me.material) < m.materials.size();
    const bool has_lm = has_mat && m.materials[size_t(me.material)].lightmap >= 0;
    const bool has_dif = has_mat && m.materials[size_t(me.material)].diffuse >= 0;
    bool mesh_bad = false;
    for (uint32_t v = 0; v < rc.n && !mesh_bad; ++v) {
      float u[2] = {me.vertices[v].uv0[0], me.vertices[v].uv0[1]};
      if (plant && !skinned_mesh && !(me.src_key >= 800 && me.src_key <= 809) && !(s1_const && s2_vary && !has_lm && has_dif))   // the old rule: S1 unless (S1 constant, S2 varies, no light map, a diffuse) or a movie key (uv_in_s2)
        u[0] = BF(body + rc.s1 + 8ull * v), u[1] = BF(body + rc.s1 + 8ull * v + 4);
      const float w0 = BF(body + rc.s2 + 8ull * v), w1 = BF(body + rc.s2 + 8ull * v + 4);
      if (!(u[0] == w0 && u[1] == w1)) mesh_bad = true;
    }
    ++checked;
    if (!skinned_mesh) ++nonskin;
    if (mesh_bad) ++bad, bad_nonskin += !skinned_mesh;
  }
  std::snprintf(buf, sizeof buf, "%zu stage meshes, %zu non-skinned, %zu differ from S2 (%zu non-skinned), %zu without a reference", checked, nonskin, bad, bad_nonskin, no_ref);
  Check(no_ref == 0 && checked > 0, std::string(lab) + ".reference_found", buf);
  std::printf("INFO  %s: %zu non-skinned meshes (the note's 492 / 401 counts a different set; not asserted)\n", lab, nonskin);
  Check(bad == 0, std::string(lab) + ".uv0_is_S2_on_every_mesh", buf);
  if (plant) {
    std::snprintf(buf, sizeof buf, "old rule differs on %zu meshes (expected %d: the unfixed loader's count)", bad, expect_plant_bad);
    Check(bad == size_t(expect_plant_bad), std::string(lab) + ".plant_count", buf);
  }
  if (id == 53) {
    const assets::Mesh* s114 = nullptr;
    for (const assets::Mesh& me : m.meshes)
      if (me.src_kind == 1 && me.src_key == 114) { s114 = &me; break; }
    Check(s114 != nullptr, "bg53.slot114_present");
    if (s114) {
      float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
      for (const assets::Vertex& v : s114->vertices)
        for (int k = 0; k < 2; ++k) lo[k] = std::fmin(lo[k], v.uv0[k]), hi[k] = std::fmax(hi[k], v.uv0[k]);
      std::snprintf(buf, sizeof buf, "%zu vertices, u %g..%g, v %g..%g", s114->vertices.size(), lo[0], hi[0], lo[1], hi[1]);
      Check(lo[0] <= 0.01f && hi[0] >= 0.99f && lo[1] <= 0.01f && hi[1] >= 0.99f, "bg53.slot114_uv_spans_0_to_1", buf);
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string game;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--game" && i + 1 < argc) game = argv[++i];
  if (game.empty()) return std::fprintf(stderr, "usage: ban3_unit --game DIR\n"), 2;
  const char* pl = std::getenv("WWE_BAN3_PLANT");
  const bool plant = pl && *pl && *pl != '0';
  launcher::GameFiles files;
  files.SetRoot(game);
  Arena(files, 53, plant, 64);
  Arena(files, 4, plant, 27);
  std::printf("%s  ban3_unit %s\n", g_bad ? "FAIL" : "PASS", plant ? "(planted old rule)" : "");
  return g_bad ? 1 : 0;
}
