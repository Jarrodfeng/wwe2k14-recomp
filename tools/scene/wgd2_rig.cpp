// S105 C2: OUR side of the widened whole-game diff (tools\harvest\wgd2_diff.py), offline, no game running. For every asset of a family that the player's own files hold (the pac directories
// are walked HERE with our pac reader only to know what to ask for), it calls the SAME loaders and pure choice functions the live path calls and prints one JSON line per asset:
//   assets::LoadStageEntry / LoadM2Weapon / LoadPropEntry / LoadWrestler / LoadArena (asset_load.h), family_cull::Cull (the cull rule), weapons::StageTypeOfPach / IsLadderPach / IsRingStepsId /
//   IsBeltsId / CovClassOfWeaponId / IsMapBWeaponSkipId / IsMapBEventSkipId (weapon_rows.h, weapon_plan.h). Nothing is re-implemented here.
//   wgd2_rig GAME_EXTRACTED_DIR FAMILY [--plant NAME] [--ids a,b,c] [--limit N]
//   FAMILY: struct (bgEtc.pac PACH 0..61 nested key 1) | weapon (m2.pac p0 / p1 key 1000) | event (evtobj.pac, entries 0..9) | char (every wrestler x attire) | crowd (every arena's crowd meshes)
// Output: JSONL on stdout. Names, ids and counts only (rule 1: nothing from the game's files is printed; texture NAMES are file names, not data).
// A plant is a FAULT put back into the call (the old rule), so the diff must show a class the clean run does not:
//   cull_old        the cull of the C3 rule before S105: only the stage slots follow the record bit (every other family two-sided)
//   ladder_short    IsLadderPach before the S103 LDR fix: PACH 61 left out (the ladder rows of the last arena set)
//   steps_skipped   the S103 LDR weapon drawer skipped the ring steps 29 / 32 / 35 (weapon_rows.h IsRingStepsId, "the arena loader draws them")
//   tex_first_11    LoadM2Weapon reads the texture directories from key 11 (the PW-3 plant): every weapon texture unbound
// entry: assets::LoadStageEntry / LoadM2Weapon / LoadPropEntry / LoadWrestler / LoadArena, family_cull::Cull, weapons::StageTypeOfPach (a host tool: it enters where the live path's loaders and choice functions are, to enumerate every asset the files hold)
// Build: pwsh -NoProfile -File tools\scene\build_wgd2_rig.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer_scene/family_cull.h"
#include "../../src/renderer_scene/weapon_plan.h"

namespace fc = rdr_scene::family_cull;
namespace wp = rdr_scene::weapons;
using namespace assets;

static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }

static std::string J(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 32 || c > 126) o += '?';
    else o += char(c);
  }
  return o + "\"";
}
static std::string TexName(const Model& m, int i) { return i >= 0 && size_t(i) < m.textures.size() ? m.textures[size_t(i)].name : std::string(); }

// the cull our renderer picks for a mesh's material: the production rule with both switches ON (their defaults); a plant swaps in the old rule
static int OurCull(const Material* mp, bool stage_family) {
  if (Plant("cull_old")) return (mp && mp->cull_back && (!mp->cull_from_record || stage_family)) ? 2 : 0;
  return fc::Cull(mp, true, true);
}

static void MeshRows(const Model& m, bool first, std::string& out) {
  (void)first;
  out += "[";
  bool comma = false;
  for (const Mesh& me : m.meshes) {
    const Material* mp = me.material >= 0 && size_t(me.material) < m.materials.size() ? &m.materials[size_t(me.material)] : nullptr;
    if (comma) out += ",";
    comma = true;
    char b[96];
    std::snprintf(b, sizeof b, "{\"r\":%d,\"k\":%d,\"kind\":%d,\"cw\":%u,\"cu\":%d,", me.src_record, me.src_key, me.src_kind, mp ? mp->cull_word : 0u, OurCull(mp, false));
    out += b;
    out += "\"sh\":" + J(mp ? mp->shader : "");
    if (mp) {
      out += ",\"d\":" + J(TexName(m, mp->diffuse)) + ",\"n\":" + J(TexName(m, mp->normal)) + ",\"l\":" + J(TexName(m, mp->lightmap)) + ",\"m\":" + J(TexName(m, mp->mask)) +
             ",\"s\":" + J(TexName(m, mp->specular)) + ",\"z\":" + J(TexName(m, mp->noise)) + ",\"c\":" + J(TexName(m, mp->cube)) + ",\"f\":" + J(TexName(m, mp->refmap)) + ",\"y\":[" + J(TexName(m, mp->layer[0])) + "," +
             J(TexName(m, mp->layer[1])) + "," + J(TexName(m, mp->layer[2])) + "],\"dn\":" + J(mp->diffuse_name) + ",\"cn\":" + J(mp->cube_name);
      std::snprintf(b, sizeof b, ",\"nl\":%.4g,\"nt\":[%.4g,%.4g]", double(mp->noise_lvl), double(mp->noise_tile[0]), double(mp->noise_tile[1]));   // S105 C2LA F4: the c207 values the Resolve binds (Lvl z, Tile x / y)
      out += b;
    }
    out += "}";
  }
  out += "]";
}

static void Emit(const char* fam, const std::string& asset, bool ok, const std::string& err, const Model& m, const std::string& ours_extra) {
  std::string line = std::string("{\"fam\":") + J(fam) + ",\"asset\":" + J(asset) + ",\"ok\":" + (ok ? "1" : "0") + ",\"err\":" + J(err) + ",\"nmesh\":" + std::to_string(m.meshes.size()) +
                     ",\"ntex\":" + std::to_string(m.textures.size()) + ",\"warn\":[";
  for (size_t i = 0; i < m.warnings.size() && i < 80; ++i) line += (i ? "," : "") + J(m.warnings[i]);
  line += "],\"meshes\":";
  MeshRows(m, true, line);
  line += ours_extra + "}";
  std::puts(line.c_str());
  std::fflush(stdout);
}
static void EmitBare(const char* fam, const std::string& asset, const std::string& ours_extra) { Model m; Emit(fam, asset, true, "", m, ours_extra); }

static bool IsLadder(int pach) { return Plant("ladder_short") ? (pach == 4 || pach == 5 || (pach >= 11 && pach <= 60)) : wp::IsLadderPach(pach); }

// ---- struct: bgEtc PACH 0..61, nested key 1, every JBOY entry
static int Structs(launcher::GameFiles& files) {
  std::string err;
  const pac::Pac* p = load_detail::OpenProps(files);
  if (!p) return std::fprintf(stderr, "bgEtc.pac not open\n"), 2;
  for (size_t pach = 0; pach < p->pachs.size() && pach <= 61; ++pach) {
    if (pach == 9 || pach == 10) continue;   // /STG/WPON and /STG/WPBS: the weapon sets (the weapon family), not structures
    Bytes nb;
    pac::Pach np;
    char x[200];
    const bool ladder = IsLadder(int(pach));
    int type = wp::StageTypeOfPach(int(pach));
    if (Plant("ladder_short") && pach == 61) type = -1;   // the old IsLadderPach range also left the type of the last rig empty
    std::snprintf(x, sizeof x, ",\"ours\":{\"type\":%d,\"ladder\":%d,\"covclass_ladder\":%d}", type, ladder ? 1 : 0, ladder ? 1 : 0);
    EmitBare("struct", "p" + std::to_string(pach), std::string(x));   // the per-PACH row: the type / ladder functions' answer, whether or not the PACH holds models
    if (!pac::GetNested(p->bytes, p->pachs[pach], 1, nb, np)) continue;
    for (const pac::Ent& e : np.ents) {
      if (e.id > 31) continue;
      Model m;
      std::string er;
      const bool ok = LoadStageEntry(files, int(pach), e.id, m, er);
      Emit("struct", "p" + std::to_string(pach) + "/" + std::to_string(e.id), ok, er, m, std::string(x));
    }
  }
  return 0;
}

// ---- weapon: m2.pac p0 / p1 key 1000, every id, entries 0 / 1 / 2 (id 14: every sub-key)
static int Weapons(launcher::GameFiles& files) {
  std::string err;
  const pac::Pac* pp = load_detail::OpenM2(files, err);
  if (!pp) return std::fprintf(stderr, "m2.pac: %s\n", err.c_str()), 2;
  for (size_t pi = 0; pi < pp->pachs.size() && pi < 2; ++pi) {
    Bytes b1;
    pac::Pach p1;
    if (!pac::GetNested(pp->bytes, pp->pachs[pi], 1000, b1, p1)) continue;
    for (const pac::Ent& e : p1.ents) {
      if (e.id >= 1000) continue;   // 40000 / 40001 / 41000 are parameter texts
      const int id = int(e.id);
      Bytes nb;
      pac::Pach np;
      if (!pac::GetNested(b1, p1, e.id, nb, np)) continue;
      char x[256];
      const bool steps_skipped = Plant("steps_skipped") && wp::IsRingStepsId(id);
      std::snprintf(x, sizeof x, ",\"set\":%zu,\"ours\":{\"belts\":%d,\"steps\":%d,\"cov\":%d,\"mapb_skip\":%d,\"drawn\":%d}", pi, wp::IsBeltsId(id) ? 1 : 0, wp::IsRingStepsId(id) ? 1 : 0, wp::CovClassOfWeaponId(id),
                    wp::IsMapBWeaponSkipId(id) ? 1 : 0, steps_skipped ? 0 : 1);
      if (id == 14) {
        for (const pac::Ent& s : np.ents) {
          if (s.id > 200) continue;
          Model m;
          std::string er;
          const bool ok = LoadM2Weapon(files, 14, m, er, Plant("tex_first_11") ? 11u : 10u, 0, int(s.id));
          Emit("weapon", "s" + std::to_string(pi) + "/14/sub" + std::to_string(s.id), ok, er, m, std::string(x));
        }
        continue;
      }
      for (uint32_t ent = 0; ent <= 2; ++ent) {
        if (!pac::Find(np, ent)) continue;
        Model m;
        std::string er;
        const bool ok = steps_skipped ? false : LoadM2Weapon(files, id, m, er, Plant("tex_first_11") ? 11u : 10u, ent);
        Emit("weapon", "s" + std::to_string(pi) + "/" + std::to_string(id) + "/" + std::to_string(ent), ok, steps_skipped ? "skipped by the old weapon drawer (ring steps)" : er, m, std::string(x));
      }
    }
  }
  return 0;
}

// ---- event: evtobj.pac (disc), every PACH, entries 0..9
static int Events(launcher::GameFiles& files) {
  std::string err;
  const pac::Pac* p = load_detail::OpenPropFile(files, 0, err);
  if (!p) return std::fprintf(stderr, "evtobj.pac: %s\n", err.c_str()), 2;
  for (size_t at = 0; at < p->pachs.size(); ++at) {
    for (uint32_t k = 0; k <= 9; ++k) {
      if (!pac::Find(p->pachs[at], k)) continue;
      const Bytes blob = pac::Decode(p->bytes.data() + pac::Find(p->pachs[at], k)->off, pac::Find(p->pachs[at], k)->size);
      if (!load_detail::IsJboy(blob)) continue;
      Model m;
      std::string er;
      const bool ok = LoadPropEntry(files, int(at), k, m, er);
      Emit("event", std::to_string(at) + "/" + std::to_string(k), ok, er, m, "");
    }
  }
  return 0;
}

// ---- char: every wrestler x attire
static int Chars(launcher::GameFiles& files, const std::set<int>& only, int limit) {
  int n = 0;
  for (int id : ListWrestlerIds(files)) {
    if (!only.empty() && !only.count(id)) continue;
    if (limit > 0 && n >= limit) break;
    ++n;
    const int attires = AttireCount(files, id);
    if (attires <= 0) {
      EmitBare("char", std::to_string(id) + "/-", ",\"attires\":0");
      continue;
    }
    for (int a = 0; a < attires; ++a) {
      Model m;
      std::string er;
      const bool ok = LoadWrestler(files, id, a, m, er);
      Emit("char", std::to_string(id) + "/" + std::to_string(a), ok, er, m, ",\"attires\":" + std::to_string(attires));
    }
  }
  return 0;
}

// ---- crowd: every arena's crowd meshes (kind 3) + the load warnings
static int Crowd(launcher::GameFiles& files, const std::set<int>& only) {
  for (int id : ListArenaIds(files)) {
    if (!only.empty() && !only.count(id)) continue;
    Model m;
    std::string er;
    const bool ok = LoadArena(files, id, m, er);
    Model c;   // the crowd's meshes only (the report is per crowd mesh)
    c.textures = m.textures, c.materials = m.materials, c.warnings = m.warnings, c.label = m.label;
    size_t people_meshes = 0;
    for (const Mesh& me : m.meshes)
      if (me.src_kind == 3) c.meshes.push_back(me), ++people_meshes;
    Emit("crowd", std::to_string(id), ok, er, c, ",\"arena_meshes\":" + std::to_string(m.meshes.size()));
    (void)people_meshes;
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 3) return std::printf("usage: wgd2_rig GAME FAMILY [--plant NAME] [--ids a,b] [--limit N]\n"), 2;
  std::set<int> only;
  int limit = 0;
  for (int i = 3; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
    else if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) limit = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--ids") && i + 1 < argc) {
      for (char* t = std::strtok(argv[++i], ","); t; t = std::strtok(nullptr, ",")) only.insert(std::atoi(t));
    }
  }
  load_detail::C2lTexDiffusePlant() = Plant("c2l_no_texdiffuse");   // S105 C2L
  load_detail::C2lPropUvPlant() = Plant("c2l_no_prop_uv");
  load_detail::C2lNoisePlant() = Plant("c2l_no_noise");
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(argv[1]));
  const std::string fam = argv[2];
  if (fam == "struct") return Structs(files);
  if (fam == "weapon") return Weapons(files);
  if (fam == "event") return Events(files);
  if (fam == "char") return Chars(files, only, limit);
  if (fam == "crowd") return Crowd(files, only);
  return std::printf("unknown family %s\n", fam.c_str()), 2;
}
