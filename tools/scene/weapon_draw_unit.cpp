// weapon_draw_unit: lane PW-3's offline proof (S103; 0 game runs, no GPU): the weapon drawer's loader and pure part (src\assets\asset_load.h LoadM2Weapon, src\renderer_scene\weapon_draw_core.h)
// on the player's own pac\m2.pac and the stream draws of a capture.
//   weapon_draw_unit --game DIR --steps DUMP.txt [--plant tex_key|transpose|merge]
// DUMP.txt = tools\scene\pw3_steps_dump.py: a capture's stream draws of the ring-steps object 0xAA141900 (frame, draw, part, VS c76..c78 as float bits).
// What it checks (each line PASS / FAIL, exit 1 on any FAIL, 2 loud failure, 3 usage):
//   loader   : m2.pac p0/1000/32 loads (3 meshes, the steps' 181 / 32 / 20 vertices), every mesh binds to the game's node numbers; id 14 (belts) is refused; an absent id fails cleanly; a census over every id of p0/1000: ASSERTS every id but 14 loads, binds and is textured (PW review fix 6); id 89's secondary models (entries 1 / 2) load, bind and are textured.
//   stream   : every dumped draw's rows are one of exactly TWO distinct row sets (the two step instances), stable over the dumped frames, and equal the arena loader's hard-coded steps (asset_load.h kA / kB) within 1e-4.
//   plan     : (contract v2) the id from the call, the newest frame only (late calls counted), one item per CALL, each secondary model its own entry (1 / 2), the steps skip (and its test switch), the belts skip, an unknown id, a node range past the pool, an empty view, the prefetch list, and the frame / secondary twins.
//   draw     : a synthetic record (sections 59 / 60 / 61 as plain arrays) for the steps, id 32, two calls of ONE object, node rows from the stream draws, posed through the SHIPPED path (Plan + BindModel + PoseCall)
//              with the skip switched off, compared with the arena loader's steps (the S101 owner_check-proven draw: bgEtc p9/32 placed by FromGameRows(stream rows) + MirrorY) per instance, per mesh:
//              vertex count, max |position| / |normal| difference, and the diffuse picture bytes, size and the material's effect name.
// Planted twins (the check named must then FAIL): tex_key (texture entries read from 11, not 10), transpose (node rows read transposed), merge (two calls of one id drawn as one).
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
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer_scene/weapon_draw_core.h"

namespace {
using namespace rdr_scene;

int g_fail = 0;
void Check(const char* name, bool ok, const char* fmt = "", ...) {
  char detail[512] = {};
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
  }
  std::printf("%s  %s%s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? "  (" : "", detail, detail[0] ? ")" : "");
  if (!ok) ++g_fail;
}

float BitsF(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

struct StreamDraw {
  int frame = 0, draw = 0, part = 0;
  float rows[12] = {};
};

bool ReadDump(const std::string& path, std::vector<StreamDraw>& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string tag;
    StreamDraw d;
    ss >> tag >> d.frame >> d.draw >> d.part;
    if (tag != "D") continue;
    for (int k = 0; k < 12; ++k) {
      std::string h;
      ss >> h;
      d.rows[k] = BitsF(uint32_t(std::strtoul(h.c_str(), nullptr, 16)));
    }
    if (ss) out.push_back(d);
  }
  return !out.empty();
}

bool SameRows(const float* a, const float* b, float tol) {
  for (int k = 0; k < 12; ++k)
    if (!(std::fabs(a[k] - b[k]) <= tol)) return false;
  return true;
}

// The arena loader's two steps (asset_load.h, the game's own swaps 6728 / 20911 / 22453 c76..c78): the reference the stream rows are compared with.
const float kA[12] = {0.70710678f, 0, -0.70710678f, 328.f, 0, 1, 0, 0, 0.70710678f, 0, 0.70710678f, -328.f};
const float kB[12] = {-0.70710678f, 0, -0.70710678f, 328.f, 0, 1, 0, 0, 0.70710678f, 0, -0.70710678f, 328.f};

const assets::Mesh* MeshOfRecord(const assets::Model& m, int rec) {
  for (const assets::Mesh& me : m.meshes)
    if (me.src_record == rec) return &me;
  return nullptr;
}

const assets::Texture* DiffuseOf(const assets::Model& m, const assets::Mesh& me) {
  if (me.material < 0 || size_t(me.material) >= m.materials.size()) return nullptr;
  const int d = m.materials[size_t(me.material)].diffuse;
  return d >= 0 && size_t(d) < m.textures.size() ? &m.textures[size_t(d)] : nullptr;
}
}  // namespace

int main(int argc, char** argv) {
  std::string game, steps, plant;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--steps" && i + 1 < argc) steps = argv[++i];
    else if (a == "--plant" && i + 1 < argc) plant = argv[++i];
  }
  if (game.empty() || steps.empty()) { std::fprintf(stderr, "usage: weapon_draw_unit --game DIR --steps DUMP.txt [--plant tex_key|transpose|merge]\n"); return 3; }
  std::vector<StreamDraw> dump;
  if (!ReadDump(steps, dump)) { std::fprintf(stderr, "weapon_draw_unit: FAIL cannot read draws from %s\n", steps.c_str()); return 2; }
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(game));
  const uint32_t tex_first = plant == "tex_key" ? 11u : 10u;
  weapons::Plants plants;
  plants.draw_steps = true;   // the unit draws the steps through this path on purpose (the production skip is checked in "plan")
  plants.transpose_rows = plant == "transpose";
  plants.merge_instances = plant == "merge";

  // ---- loader ----
  std::string err;
  assets::Model steps_model;
  const bool loaded = assets::LoadM2Weapon(files, 32, steps_model, err, tex_first);
  if (!loaded && plant.empty()) { std::fprintf(stderr, "weapon_draw_unit: FAIL m2 weapon 32: %s\n", err.c_str()); return 2; }
  std::vector<int> counts;
  for (const assets::Mesh& me : steps_model.meshes) counts.push_back(int(me.vertices.size()));
  std::sort(counts.begin(), counts.end());
  Check("loader.steps_32", loaded && counts == std::vector<int>({20, 32, 181}), "meshes %zu, vertices %s", steps_model.meshes.size(), [&] {
    static std::string s;
    s.clear();
    for (int c : counts) s += (s.empty() ? "" : "/") + std::to_string(c);
    return s.c_str();
  }());
  assets::Model bound;
  const uint32_t unbound = weapons::BindModel(steps_model, bound);
  Check("loader.binds_to_nodes", unbound == 0 && !bound.meshes.empty(), "%u of %zu meshes could not be bound", unbound, bound.meshes.size());
  {
    assets::Model belts;
    std::string e2;
    const bool ok14 = assets::LoadM2Weapon(files, 14, belts, e2);
    assets::Model none;
    std::string e3;
    const bool ok_none = assets::LoadM2Weapon(files, 99999, none, e3);
    Check("loader.belts_refused_absent_fails", !ok14 && !ok_none && !e2.empty() && !e3.empty(), "14: \"%s\"; 99999: \"%s\"", e2.c_str(), e3.c_str());
  }
  {   // S103 LDR (COV10 B1 / B2 / C2): the loaders of the three families the stream-free build lacked, on the player's own files
    // WPBS: the brawl-only ids are absent from m2 p0 and found in p1 (the loader looks p0, then p1); an id in neither still fails
    assets::pac::Pac p;
    std::string e;
    assets::Bytes b0, nb;
    assets::pac::Pach p0n, np;
    const bool open = assets::pac::Open(files, "pac\\m2.pac", p, e) && !p.pachs.empty() && assets::pac::GetNested(p.bytes, p.pachs[0], 1000u, b0, p0n);
    int only_p1 = 0, tried = 0, loaded_ok = 0;
    for (uint32_t id : {17u, 51u, 52u, 53u, 54u, 58u, 61u, 62u, 64u, 68u, 69u}) {
      ++tried;
      if (open && !assets::pac::GetNested(b0, p0n, id, nb, np)) ++only_p1;
      assets::Model m;
      std::string e2;
      if (assets::LoadM2Weapon(files, int(id), m, e2)) ++loaded_ok;
      else std::printf("INFO  wpbs id %u: %s\n", id, e2.c_str());
    }
    Check("loader.wpbs_p1", open && only_p1 == tried && loaded_ok == tried, "%d of %d brawl-only ids are absent from p0 (%d load from p1)", only_p1, tried, loaded_ok);
    // the weapon manager's belt: p0/1000/14/<sub>/{0, 10}; a key the pac lacks falls back to 0 (the game); no key = refused (above)
    int belts_ok = 0;
    for (int sub : {0, 1, 12, 45, 86, 90, 4242}) {
      assets::Model m;
      std::string e2;
      if (assets::LoadM2Weapon(files, 14, m, e2, 10, 0, sub) && !m.meshes.empty()) ++belts_ok;
      else std::printf("INFO  belt sub-key %d: %s\n", sub, e2.c_str());
    }
    Check("loader.belt_subkeys", belts_ok == 7, "%d of 7 belt sub-keys loaded (45 / 86 / 90 present, 4242 falls back to 0)", belts_ok);
    // the ladder rig (bgEtc PACH 4 LADM, 5 LADW, 11 + row STGL): key 1 entries 0 (rope) and 1 (belt / briefcase); PACH 62 is no ladder row
    int rig_ok = 0, rig_n = 0;
    for (int pach : {4, 5, 11, 15, 56, 57, 61})
      for (uint32_t k : {0u, 1u}) {
        ++rig_n;
        assets::Model m;
        std::string e2;
        if (assets::LoadStageEntry(files, pach, k, m, e2) && !m.meshes.empty()) ++rig_ok;
        else std::printf("INFO  stage PACH %d key 1 entry %u: %s\n", pach, k, e2.c_str());
      }
    assets::Model none62;
    std::string e62;
    // S103 LDR2 (LDC.md section 1 [C]): LADM k 3 is the light SHAFT (tall: y extent ~800 against ~160 wide) and k 2 the flat light POOL (y extent 0), so the additive rule (IsLadderLightPiece) hits the column and the pool, never the rope (k 0: ~5000 tall, 26 wide) or the case (k 1). Ratios, so the model's unit does not matter.
    bool shape_ok = false;
    {
      const auto ext = [&](uint32_t k, float (&e)[3]) {
        assets::Model m;
        std::string e3;
        if (!assets::LoadStageEntry(files, 4, k, m, e3)) return false;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        size_t n = 0;
        for (const assets::Mesh& me : m.meshes)
          for (const assets::Vertex& v : me.vertices)
            for (int a = 0; a < 3; ++a) lo[a] = lo[a] < v.pos[a] ? lo[a] : v.pos[a], hi[a] = hi[a] > v.pos[a] ? hi[a] : v.pos[a], ++n;
        for (int a = 0; a < 3; ++a) e[a] = hi[a] - lo[a];
        return n > 0;
      };
      float e2k[3] = {}, e3k[3] = {}, e0k[3] = {}, e1k[3] = {};
      const bool have = ext(2, e2k) && ext(3, e3k) && ext(0, e0k) && ext(1, e1k);
      const float w3 = e3k[0] > e3k[2] ? e3k[0] : e3k[2], w2 = e2k[0] > e2k[2] ? e2k[0] : e2k[2];
      shape_ok = have && e3k[1] > 3.f * w3 && w3 > 0.f && w2 > 0.f && e2k[1] < 0.01f * w2 && e0k[1] > 50.f * (e0k[0] > e0k[2] ? e0k[0] : e0k[2]);   // k 3 tall (800 vs 160), k 2 flat, the rope far taller than wide (not a light); the case is neither
      shape_ok = shape_ok && have && !(e1k[1] > 3.f * (e1k[0] > e1k[2] ? e1k[0] : e1k[2]));
      if (have) std::printf("INFO  LADM extents k2 %.1f %.1f %.1f, k3 %.1f %.1f %.1f, k0 %.1f %.1f %.1f, k1 %.1f %.1f %.1f\n", e2k[0], e2k[1], e2k[2], e3k[0], e3k[1], e3k[2], e0k[0], e0k[1], e0k[2], e1k[0], e1k[1], e1k[2]);
    }
    Check("loader.ladder_rig", rig_ok == rig_n && !assets::LoadStageEntry(files, 62, 0, none62, e62) && shape_ok, "%d of %d ladder rig models loaded (PACH 4 / 5 / 11 / 15 / 56 / 57 / 61, entries 0 / 1); PACH 62 refused; LADM k 3 tall light shaft, k 2 flat pool, rope tall + thin, case neither (%d)", rig_ok, rig_n, int(shape_ok));
  }
  {   // a census over every id of p0/1000 (information + one assertion: nothing crashes, id 32 is in it)
    assets::pac::Pac p;
    std::string e;
    assets::Bytes b1;
    assets::pac::Pach p1;
    bool have = assets::pac::Open(files, "pac\\m2.pac", p, e) && !p.pachs.empty() && assets::pac::GetNested(p.bytes, p.pachs[0], 1000u, b1, p1);
    std::vector<uint32_t> ids;
    if (have)
      for (const assets::pac::Ent& en : p1.ents)
        if (en.id < 1000) ids.push_back(en.id);   // the weapon ids (the directory also holds a few entries of another kind, 40000+, that no weapon id names)
    p = assets::pac::Pac{};
    int ok = 0, failed = 0, belts = 0, textured = 0, bindable = 0;
    std::vector<uint32_t> untextured;
    for (uint32_t id : ids) {
      if (id == 14) { ++belts; continue; }
      assets::Model m;
      std::string e2;
      if (!assets::LoadM2Weapon(files, int(id), m, e2)) { ++failed; std::printf("INFO  id %u failed: %s\n", id, e2.c_str()); continue; }
      ++ok;
      bool tex = true, bind = true;
      for (const assets::Mesh& me : m.meshes) {
        tex = tex && DiffuseOf(m, me) != nullptr;
        assets::Mesh c = me;
        bind = bind && rdr_scene::rf::c::BindMesh(c);
      }
      if (!tex) {
        untextured.push_back(id);
        std::printf("INFO  id %u: a mesh has no diffuse picture (meshes %zu, materials %zu, textures %zu)", id, m.meshes.size(), m.materials.size(), m.textures.size());
        for (const assets::Mesh& me : m.meshes) std::printf(" [mat %d diffuse %d]", me.material, me.material >= 0 && size_t(me.material) < m.materials.size() ? m.materials[size_t(me.material)].diffuse : -9);
        std::printf("\n");
      }
      textured += tex, bindable += bind;
    }
    std::printf("INFO  m2 p0/1000 census: %zu ids, %d loaded (%d fully textured, %d fully bindable), %d failed to load, %d belts id 14 skipped\n", ids.size(), ok, textured, bindable, failed, belts);
    Check("loader.census_has_32", have && std::find(ids.begin(), ids.end(), 32u) != ids.end(), "%zu ids", ids.size());
    // PW review fix 6: every id but 14 loads, every mesh binds to the game's node numbers, every mesh has a diffuse picture (the textures of key 10, and key 20 where the leaf has it)
    Check("loader.census_all_loaded_bound_textured", have && failed == 0 && ok + belts == int(ids.size()) && bindable == ok && untextured == std::vector<uint32_t>({40, 41, 42, 43, 44, 45, 79}), "%zu ids: %d loaded, %d textured, %d bindable, %d failed, %d belts skipped; the untextured set must be exactly the known 40-45 + 79 (their key-10 directory decodes to no picture: [U] whether the game draws them flat; any NEW untextured id fails here)", ids.size(), ok, textured, bindable, failed, belts);
    {   // the secondary models of id 89 (leaf entries 1 / 2 = k 0 / 1)
      bool sec_ok = true;
      std::string why;
      for (uint32_t e : {1u, 2u}) {
        assets::Model m;
        std::string e2;
        const bool l = assets::LoadM2Weapon(files, 89, m, e2, 10, e);
        bool tex = l && !m.meshes.empty(), bind = l;
        for (const assets::Mesh& me : m.meshes) {
          tex = tex && DiffuseOf(m, me) != nullptr;
          assets::Mesh c = me;
          bind = bind && rdr_scene::rf::c::BindMesh(c);
        }
        sec_ok = sec_ok && l && tex && bind;
        if (!l) why += " entry " + std::to_string(e) + ": " + e2;
      }
      assets::Model main89;
      std::string e3;
      const bool l89 = assets::LoadM2Weapon(files, 89, main89, e3);
      assets::Model sec1;
      std::string e4;
      assets::LoadM2Weapon(files, 89, sec1, e4, 10, 1);
      size_t v_main = 0, v_sec = 0;
      for (const assets::Mesh& me : main89.meshes) v_main += me.vertices.size();
      for (const assets::Mesh& me : sec1.meshes) v_sec += me.vertices.size();
      Check("loader.secondary_models_89", sec_ok && l89 && v_main != v_sec, "entries 1 / 2 load, bind and are textured; main %zu vertices, secondary 1 %zu (a different mesh)%s", v_main, v_sec, why.c_str());
    }
  }

  // ---- stream rows ----
  std::vector<std::vector<float>> sets;   // the distinct row sets, in order of first appearance
  bool stable = true;
  std::map<int, std::vector<int>> by_frame;   // frame -> set index per draw
  for (const StreamDraw& d : dump) {
    int at = -1;
    for (size_t k = 0; k < sets.size(); ++k)
      if (SameRows(sets[k].data(), d.rows, 1e-4f)) at = int(k);
    if (at < 0) { sets.emplace_back(d.rows, d.rows + 12); at = int(sets.size()) - 1; }
    by_frame[d.frame].push_back(at);
  }
  for (auto& kv : by_frame) {
    std::vector<int> v = kv.second;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    stable = stable && v.size() == 2;
  }
  Check("stream.two_instances_stable", sets.size() == 2 && stable, "%zu draws, %zu frames, %zu distinct row sets (each frame holds 2)", dump.size(), by_frame.size(), sets.size());
  Check("stream.equals_arena_steps", sets.size() == 2 && SameRows(sets[0].data(), kA, 1e-4f) && SameRows(sets[1].data(), kB, 1e-4f), "instance 0 = kA, instance 1 = kB (asset_load.h)");
  if (sets.size() != 2 || !loaded) {
    std::printf("RESULT FAIL %d (cannot draw: no two instances / no model)\n", g_fail);
    return 1;
  }

  // ---- plan ----
  // Contract v2: the id is the CALL's (taken from its own frame's slot table by lane PW-2), the newest frame of the record is the one drawn, each secondary model is its own entry (1 + k); only the weapon's own submit is recorded at all (the tracker).
  {
    weapons::WeaponCall calls[8];
    struct Spec { int32_t id; uint32_t frame; int k; };   // k = -1: the main model, 0 / 1: the secondary model k
    const Spec specs[8] = {{5, 10, -1}, {5, 10, -1}, {5, 10, 0}, {32, 10, -1}, {14, 10, -1}, {-1, 10, -1}, {5, 10, 1}, {5, 9, -1}};
    for (int i = 0; i < 8; ++i) {
      calls[i].object = 0x1000 + 0x100 * uint32_t(i), calls[i].instance = uint32_t(i == 1), calls[i].node_first = 0, calls[i].node_count = 2;
      calls[i].id = specs[i].id, calls[i].frame = specs[i].frame, calls[i].slot_flags = weapons::PackSlotFlags(0, specs[i].k >= 0, true, specs[i].k >= 0 ? uint32_t(specs[i].k) : 0u);
    }
    float nodes[24] = {};
    weapons::View v;
    v.calls = calls, v.n_calls = 8, v.nodes = nodes, v.n_nodes = 2;
    weapons::Census c;
    weapons::Plants none;
    none.draw_steps = false;   // S103 LDR: production draws the steps from the rows now; this block checks the plan's skip bookkeeping through the retired skip (the twin), plan.steps_switch below checks the default
    const auto items = weapons::Plan(v, none, c);
    // two calls of one id + the two secondary models' calls (also id 5) = 4 items; the steps skipped; the belts, the unknown id and the late frame counted
    Check("plan.id_from_the_call_one_item_per_call", items.size() == 4 && items[0].id == 5 && items[1].id == 5 && items[2].id == 5 && items[3].id == 5 && items[1].instance == 1 && items[0].entry == 0 && items[1].entry == 0 && items[2].entry == 1 && items[3].entry == 2, "%zu items (entries main, main, secondary 1, secondary 2)", items.size());
    Check("plan.skips_counts", c.skipped_steps == 1 && c.belts == 1 && c.unknown_id == 1 && c.bad_rows == 0 && c.late == 1, "steps %u belts %u unknown %u bad rows %u late %u", c.skipped_steps, c.belts, c.unknown_id, c.bad_rows, c.late);
    weapons::Census c2;
    weapons::Plants with_steps;
    with_steps.draw_steps = true;
    const auto items2 = weapons::Plan(v, with_steps, c2);
    Check("plan.steps_switch", items2.size() == 5 && c2.skipped_steps == 0, "%zu items with the skip off", items2.size());
    {   // the twins of the newest-frame and secondary-entry rules are caught: with the frame rule off the plan draws one more; with a secondary drawn as the main mesh the entries are all 0
      weapons::Plants pf, pr;
      pf.draw_steps = pr.draw_steps = false;   // S103 LDR: the twins are measured against the retired-skip baseline of this block
      pf.ignore_frame = true, pr.secondary_as_main = true;
      weapons::Census cf, cr;
      const size_t nf = weapons::Plan(v, pf, cf).size();
      const auto ir = weapons::Plan(v, pr, cr);
      Check("plan.twins_caught", nf == 5 && ir.size() == 4 && ir[2].entry == 0 && ir[3].entry == 0, "ignore_frame %zu items (the late call drawn), secondary_as_main entries %u / %u (the main mesh)", nf, ir.size() == 4 ? ir[2].entry : 9u, ir.size() == 4 ? ir[3].entry : 9u);
    }
    {   // review fix 2: section 59 -> what is asked for before the draw
      weapons::WeaponSlot sl[3];
      sl[0].id = 5, sl[0].slot = 0, sl[0].main = 0x1000;
      sl[1].id = 89, sl[1].slot = 1, sl[1].main = 0x2000, sl[1].secondary[0] = 0x2100, sl[1].secondary[1] = 0x2200;
      sl[2].id = 32, sl[2].slot = 2, sl[2].main = 0x3000;   // the steps: not asked for
      weapons::View pv;
      pv.slots = sl, pv.n_slots = 3;
      const auto want = weapons::Prefetch(pv, none);
      Check("plan.prefetch_all_slot_ids", want.size() == 4 && want[0] == std::make_pair(int32_t(5), 0u) && want[1] == std::make_pair(int32_t(89), 0u) && want[2] == std::make_pair(int32_t(89), 1u) && want[3] == std::make_pair(int32_t(89), 2u), "%zu (id, entry) pairs", want.size());
    }
    {   // S103 SMX: a stage id (kStageIdBase + PACH) without the stage flag is refused by the plan and the prefetch (the guard off asks m2.pac: the planted twin)
      weapons::WeaponCall sc[1];
      float sn[12] = {};
      sc[0].object = 0x5000, sc[0].instance = 0, sc[0].node_first = 0, sc[0].node_count = 2, sc[0].frame = 10, sc[0].id = weapons::StageId(3), sc[0].slot_flags = weapons::PackSlotFlags(0, false, true, 0u);
      weapons::View sv;
      sv.calls = sc, sv.n_calls = 1, sv.nodes = sn, sv.n_nodes = 2;
      weapons::Census cs;
      weapons::Plants gp = none;
      gp.stage_id_plain_ok = plant == "stage_id_plain";   // the planted twin: the guard is off in the production-side half of the check
      {   // review A1: a manager belt (BeltId 0x300000 + sub-key) is not a stage id and is still planned
        weapons::WeaponCall bc[1] = {sc[0]};
        bc[0].id = weapons::BeltId(45);
        weapons::View bv = sv;
        bv.calls = bc;
        weapons::Census cb;
        const auto bi = weapons::Plan(bv, gp, cb);
        Check("plan.belt_not_refused_as_stage", bi.size() == 1 && bi[0].id == weapons::BeltId(45) && cb.stage_id_plain == 0, "%zu items, %u counted", bi.size(), cb.stage_id_plain);
      }
      const size_t ng = weapons::Plan(sv, gp, cs).size();
      weapons::Plants tw = none;
      tw.stage_id_plain_ok = true;
      weapons::Census ct;
      const auto it = weapons::Plan(sv, tw, ct);
      weapons::WeaponSlot ss[1];
      ss[0].id = weapons::StageId(3), ss[0].slot = 0, ss[0].main = 0x1000;
      weapons::View psv;
      psv.slots = ss, psv.n_slots = 1;
      const size_t wg = weapons::Prefetch(psv, gp).size(), wt = weapons::Prefetch(psv, tw).size();
      Check("plan.stage_id_without_flag_refused", ng == 0 && cs.stage_id_plain == 1 && cs.unknown_id == 1 && cs.stage_id_plain_flags == sc[0].slot_flags && wg == 0, "guard on: %zu items, %u counted, %zu prefetched", ng, cs.stage_id_plain, wg);
      Check("plan.stage_id_twin_caught", it.size() == 1 && it[0].id == weapons::StageId(3) && wt == 1 && ct.stage_id_plain == 0, "guard off: %zu items (an m2 weapon id 65539), %zu prefetched", it.size(), wt);
    }
    calls[0].node_first = 1;   // 1 + 2 > 2 pool nodes
    weapons::Census c3;
    weapons::Plan(v, none, c3);
    Check("plan.bad_rows", c3.bad_rows == 1, "bad rows %u", c3.bad_rows);
    weapons::View empty;
    weapons::Census c4;
    Check("plan.empty_view_no_items", weapons::Plan(empty, none, c4).empty() && c4.planned == 0 && empty.empty());
    weapons::Census line;
    line.NoteDrawn(32), line.NoteDrawn(5), line.NoteDrawn(32), line.unknown_id = 2, line.no_model = 1;
    Check("plan.census_line", line.Line() == "[rdr][weapons] drawn 3 (ids 5 32), unknown id 2, no model 1", "%s", line.Line().c_str());
  }

  // ---- draw: the steps through the weapon path vs the arena loader's steps ----
  // The synthetic record: slot 0 = id 32 (main = the object the stream calls 0xAA141900), two calls of that ONE object (instances 0 / 1), the node pool = per instance the model's nodes, each node
  // taking the rows of the stream draw of the mesh bound to it (the part number of a draw = the mesh's record).
  int max_node = -1;
  for (const assets::Mesh& me : bound.meshes)
    for (uint16_t n : me.palette_nodes)
      if (n != 0xFFFF) max_node = std::max(max_node, int(n));
  const uint32_t n_per = uint32_t(max_node + 1);
  std::vector<float> pool(size_t(2) * n_per * 12, 0.f);
  bool rows_ok = true;
  const int draw_frame = dump.front().frame;
  for (const StreamDraw& d : dump) {
    if (d.frame != draw_frame) continue;
    const assets::Mesh* me = MeshOfRecord(bound, d.part);
    int inst = -1;
    for (size_t k = 0; k < sets.size(); ++k)
      if (SameRows(sets[k].data(), d.rows, 1e-4f)) inst = int(k);
    if (!me || inst < 0 || me->rigid_node < 0) { rows_ok = false; continue; }
    float* dst = &pool[(size_t(inst) * n_per + size_t(me->rigid_node)) * 12];
    bool set_already = false;
    for (int k = 0; k < 12; ++k) set_already = set_already || dst[k] != 0.f;
    if (set_already && !SameRows(dst, d.rows, 1e-6f)) rows_ok = false;   // two parts on one node with different rows: the synthetic record could not hold them
    std::memcpy(dst, d.rows, sizeof d.rows);
  }
  Check("draw.synthetic_rows_from_stream", rows_ok, "%u nodes per instance, frame %d", n_per, draw_frame);
  weapons::WeaponCall calls[2];
  for (uint32_t i = 0; i < 2; ++i) calls[i].object = 0xAA141900, calls[i].instance = i, calls[i].frame = 7, calls[i].id = 32, calls[i].slot_flags = weapons::PackSlotFlags(0, false, true, 0), calls[i].node_first = i * n_per, calls[i].node_count = n_per;
  weapons::View v;
  v.calls = calls, v.n_calls = 2, v.nodes = pool.data(), v.n_nodes = size_t(2) * n_per;
  weapons::Census census;
  const std::vector<weapons::Item> items = weapons::Plan(v, plants, census);
  Check("draw.two_instances_two_items", items.size() == 2 && items[0].instance == 0 && items[1].instance == 1, "%zu items planned from two calls of one object", items.size());

  // the reference: the arena loader's steps (bgEtc p9/32), placed by the STREAM rows through the asset loader's own FromGameRows + MirrorY
  assets::Model ref;
  assets::load_detail::AddProp(files, 32, {assets::Identity()}, "ring steps", ref, false);
  assets::FinishModel(ref);
  if (ref.meshes.empty()) { std::fprintf(stderr, "weapon_draw_unit: FAIL the arena loader's steps (bgEtc p9/32) did not load\n"); return 2; }
  std::vector<assets::Matrix> pal;
  double worst_pos = 0, worst_nrm = 0;
  int meshes_compared = 0, vertex_mismatch = 0, tex_bad = 0, effect_bad = 0;
  uint32_t missing_instances = 0;
  for (uint32_t inst = 0; inst < 2; ++inst) {
    const weapons::Item* it = nullptr;
    for (const weapons::Item& x : items)
      if (x.instance == inst) it = &x;
    if (!it) { ++missing_instances; continue; }
    assets::Model scratch = bound;
    weapons::PoseCall(bound, v, calls[it->call], plants, pal, scratch);
    const assets::Matrix W = assets::MirrorY(assets::load_detail::FromGameRows(reinterpret_cast<const float(&)[12]>(*sets[inst].data())));
    for (const assets::Mesh& rm : ref.meshes) {
      const assets::Mesh* om = MeshOfRecord(scratch, rm.src_record);
      const assets::Mesh* ob = MeshOfRecord(bound, rm.src_record);
      if (!om || !ob || om->vertices.size() != rm.vertices.size()) { ++vertex_mismatch; continue; }
      ++meshes_compared;
      for (size_t k = 0; k < rm.vertices.size(); ++k) {
        const assets::Vertex& r = rm.vertices[k];
        const assets::Vertex& o = om->vertices[k];
        for (int c = 0; c < 3; ++c) {
          const float rp = r.pos[0] * W[size_t(c)] + r.pos[1] * W[size_t(4 + c)] + r.pos[2] * W[size_t(8 + c)] + W[size_t(12 + c)];
          const float rn = r.normal[0] * W[size_t(c)] + r.normal[1] * W[size_t(4 + c)] + r.normal[2] * W[size_t(8 + c)];
          worst_pos = std::max(worst_pos, double(std::fabs(rp - o.pos[c])));
          worst_nrm = std::max(worst_nrm, double(std::fabs(rn - o.normal[c])));
        }
      }
      if (inst == 0) {   // the picture and the effect do not depend on the instance
        const assets::Texture* rt = DiffuseOf(ref, rm);
        const assets::Texture* ot = DiffuseOf(bound, *ob);
        if (!rt || !ot || rt->width != ot->width || rt->height != ot->height || rt->format != ot->format || rt->data != ot->data) ++tex_bad;
        const std::string re = rm.material >= 0 ? ref.materials[size_t(rm.material)].shader : "", oe = ob->material >= 0 ? bound.materials[size_t(ob->material)].shader : "";
        if (re.empty() || re != oe) ++effect_bad;
      }
    }
  }
  Check("draw.instances_present", missing_instances == 0, "%u of 2 step instances missing from the drawn items", missing_instances);
  Check("draw.vertex_counts_equal", vertex_mismatch == 0 && meshes_compared >= 6, "%d meshes compared (3 meshes x 2 instances), %d with another vertex count / absent", meshes_compared, vertex_mismatch);
  Check("draw.positions_equal_arena_steps", worst_pos <= 1e-3, "max |position| difference %.6f over %d meshes (stream rows, frame %d)", worst_pos, meshes_compared, draw_frame);
  Check("draw.normals_equal_arena_steps", worst_nrm <= 1e-3, "max |normal| difference %.6f", worst_nrm);
  Check("draw.diffuse_picture_equal", tex_bad == 0, "%d of %zu meshes whose diffuse picture (bytes, size, format) differs from the arena loader's", tex_bad, ref.meshes.size());
  Check("draw.effect_names_equal", effect_bad == 0, "%d meshes with another effect name", effect_bad);

  std::printf("RESULT %s %d%s%s\n", g_fail ? "FAIL" : "PASS", g_fail, plant.empty() ? "" : " plant=", plant.c_str());
  return g_fail ? 1 : 0;
}
