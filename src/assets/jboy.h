// Session 96 (asset runtime loader): the JBOY model block -> assets::Mesh / Material / Bone. Written against the dumps
// of Session 95-96 (out\runtime\research_models.md for wrestlers, research_arenas.md for arenas) and re-checked on the
// real files by tools\assets\asset_dump.cpp.
//
// RECONCILIATION (verified on ch107 and bg05, 2026-11-08): the two research notes describe one format. A pac entry is
// 'JBOY', BE32 body size, then the body (blob + 8); an optional 'POF0' relocation list follows the body. EVERY offset
// below is relative to the BODY start and is already an offset in the file, so no relocation is needed to read it (the
// POF0 list only turns those same words into pointers at run time). The wrestler notes' "stream offset + 8" and
// "+12 for S0" came from measuring from the tag instead of the body: measured from the body the four streams
// tile exactly with no bias.
//
// Body header (all big-endian): +0x04 body size; +0x10 mesh ("material") count; +0x14 offset of the mesh table
// (always 0x40); +0x18 node count; +0x1C texture-name count; +0x20 node table offset; +0x24 texture-name table
// offset (16-byte names); +0x28 / +0x2C a second name table and its count (unused here).
//
// Mesh record, 0xB8 bytes (offsets inside the record):
//   +0x00 vertex count; +0x08 bone-palette count; +0x0C.. palette (node indices, up to 20)
//   +0x68 S0 (data starts 4 bytes AFTER this offset; 28 B/vertex: position f32x3, normal f32x3, colour 4 bytes)
//   +0x6C S1 (wrestlers: skin influences, 8 B each = LE u32 palette slot (255 = unused) + BE f32 weight, 1-4 per vertex;
//             arenas: the same skin pairs; a static mesh has one rigid pair, slot 0 / weight 1.0)
//   +0x70 S2 (8 B UV pairs: the diffuse UV of every model, wrestlers and arenas alike; sub_82BD4BE0 0x82BD4E70-0x82BD4F0C)
//   +0x74 S3 (12 B tangent f32x3; 0 bytes in arenas)
//   +0x7C material class name (16 B, e.g. "yCh_skin", "yBG_Default")
//   +0x94 parameter count; +0x98 offset of the parameter-offset list
//   +0x9C index block: LE32 primitive type (6 = triangle strip), BE32 index count, BE32 self offset, then BE u16 indices.
// Parameter record: name[16], then BE32 (type << 16 | size): type 0x0F sampler (BE32 index into the texture-name
// table, -1 = none), 0x0D float4, 0x0A float.
// Node record, 80 B: name[16], translation f32x3 at +16, rotation (euler, unconfirmed) at +32, BE32 parent index (-1
// root) at +48, a float4 at +64 whose meaning is unknown.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"
#include "char_draw_layer.h"   // S111 LAYERS: which character parts blend (the game's draw split)
#include "material_floats.h"   // S111 GUESTHAIR: the float parameters' one name -> field table (shared with the created-superstar build)
#include "material_record.h"   // S111 UNIFY3: the record's technique / cull words -> Material (shared with the created-superstar build)
#include "material_samplers.h"   // S111 UNIFY2: the sampler parameters' one name -> field table (shared with the created-superstar build)
#include "dds.h"
#include "load_plants.h"   // S103 W2B (review B): load_detail::SpringNodes
#include "node_alias.h"   // S103 W2B
#include "pac.h"
#include "skin_vertex_colour.h"   // S111 VERTEXSEAM: the skin vertex keeps the file's byte 24 (ID-map / wrinkle slot) as its alpha
#include "vertex_decode.h"   // S111 GUESTVTX: the vertex rules this loader shares with the created-superstar decoder (colour, 1-pair weight)

namespace assets {

// S103 CPF planted twin (the old rule: a low-alpha mask is a cut-out): dev builds only, read once.
inline bool CPFPlantOld() {
#ifdef RDR_DEV
  return load_detail::CpfPlant();
#else
  return false;
#endif
}
// S103 TQF: the evtobj / entrance prop classes whose pixel shader the game picks by the technique word (yProp_BumpMap / Default / Reflect + LM twins; the same predicate as
// src\renderer_scene\technique_pick.h AppliesTo). `sl` = the lower-cased shader name. TqfSkipsTexelRule: for those the loader no longer decides alpha-test / cut-out from the diffuse texels (the game never reads
// the texture to decide, docs\research\s103\PAS.md); the front end tests + blends where the picked variant shows the alpha. TqfPlant() (dev twin) restores the old texel rule.
inline bool TqfClass(const std::string& sl) { return sl.rfind("yprop_bumpmap", 0) == 0 || sl.rfind("yprop_default", 0) == 0 || sl.rfind("yprop_reflect", 0) == 0; }
inline bool TqfPlantOld() {
#ifdef RDR_DEV
  return load_detail::TqfPlant();
#else
  return false;
#endif
}
inline bool TqfSkipsTexelRule(const std::string& sl) { return TqfClass(sl) && !TqfPlantOld() && !CPFPlantOld(); }   // (the CPF plant restores the texel rule for the class too: its cap check must still catch the old rule)
// S103 CPF: the materials that keep the old cut-out rule (after TQF the yProp branch below serves only the planted twin; the live users are yBG_ materials): every yBG_ (arena) material (the game tests + blends every yBG_ draw, front_end.cpp B2 rule: a low-alpha card is a real blended card there) and the P1 domain
// (plain yBG / yProp_Default, no spec / normal map: P1's own rule decides). `sl` = the lower-cased shader name.
inline bool CPFKeepOld(const std::string& sl, const Material& mat) {
  return sl.rfind("ybg_", 0) == 0 || (sl == "yprop_default" && mat.specular < 0 && mat.normal < 0);
}

// Session 97 (props2): true when at least 8 % of a BC2 / BC3 texture's top mip has alpha <= 32 (a cut-out, not a gloss mask).
// S103 CPF: a texture with under 0.5 % solid texels (alpha >= 224) is a MASK (gloss / spec), never a cut-out: it has nothing solid to cut a shape from (Cena's 13904Cap: alpha 0..40, drawn at ~2 % opacity;
// docs\research\s103\CAP.md). `keep_old_rule` = CPFKeepOld.
inline bool AlphaCutsOut(const Texture& t, bool keep_old_rule = false) {
  const size_t blocks = size_t((std::max(t.width, 1u) + 3) / 4) * ((std::max(t.height, 1u) + 3) / 4);
  if (blocks == 0 || t.data.size() < blocks * 16) return true;  // (unknown: keep the old behaviour)
  size_t low = 0;
  size_t solid = 0;
  for (size_t b = 0; b < blocks; ++b) {
    const uint8_t* q = &t.data[b * 16];
    if (t.format == TexFormat::kBC2) {
      for (int i = 0; i < 16; ++i) {
        const unsigned v = ((q[i / 2] >> (4 * (i & 1))) & 15) * 17;
        low += v <= 32, solid += v >= 224;
      }
    } else {
      const unsigned a0 = q[0], a1 = q[1];
      uint64_t bits = 0;
      for (int i = 0; i < 6; ++i) bits |= uint64_t(q[2 + i]) << (8 * i);
      for (int i = 0; i < 16; ++i) {
        const unsigned ix = unsigned((bits >> (3 * i)) & 7);
        unsigned v;
        if (ix == 0) v = a0;
        else if (ix == 1) v = a1;
        else if (a0 > a1) v = ((8 - ix) * a0 + (ix - 1) * a1) / 7;
        else if (ix == 6) v = 0;
        else if (ix == 7) v = 255;
        else v = ((6 - ix) * a0 + (ix - 1) * a1) / 5;
        low += v <= 32, solid += v >= 224;
      }
    }
  }
  if (!keep_old_rule && !CPFPlantOld() && double(solid) < 0.005 * double(blocks) * 16.0) return false;  // S103 CPF: a mask (under 0.5 % solid texels), not a cut-out
  return double(low) >= 0.08 * double(blocks) * 16.0;
}


// S103 B2: the share (0..1) of a texture's top-mip texels whose alpha is below 255 (0 = alpha-free: BC1 without punch-through, or an alpha channel that is all 255). BC1 counts its 1-bit
// punch-through (colour0 <= colour1, index 3). Used for ONE decision (front_end.cpp): an arena part whose output alpha includes this texture's alpha (Variant::out_alpha 2) is tested + blended like the
// game's arena class when the share is > 0; an alpha-free one is not (the game's test + blend is then a no-op: a 360 habit, CLAUDE.md rule 6).
inline float AlphaClearShare(const Texture& t) {
  const size_t px = size_t(std::max(t.width, 1u)) * std::max(t.height, 1u);
  size_t below = 0, n = 0;
  if (t.format == TexFormat::kBC1 || t.format == TexFormat::kBC2 || t.format == TexFormat::kBC3) {
    const size_t blocks = size_t((std::max(t.width, 1u) + 3) / 4) * ((std::max(t.height, 1u) + 3) / 4);
    const size_t bs = t.format == TexFormat::kBC1 ? 8 : 16;
    if (blocks == 0 || t.data.size() < blocks * bs) return 0.f;
    n = blocks * 16;
    for (size_t b = 0; b < blocks; ++b) {
      const uint8_t* q = &t.data[b * bs];
      if (t.format == TexFormat::kBC1) {
        const unsigned c0 = q[0] | (q[1] << 8), c1 = q[2] | (q[3] << 8);
        if (c0 <= c1) {
          const uint32_t idx = q[4] | (q[5] << 8) | (q[6] << 16) | (uint32_t(q[7]) << 24);
          for (int i = 0; i < 16; ++i) below += ((idx >> (2 * i)) & 3) == 3;
        }
      } else if (t.format == TexFormat::kBC2) {
        for (int i = 0; i < 16; ++i) below += ((q[i / 2] >> (4 * (i & 1))) & 15) != 15;
      } else {
        const unsigned a0 = q[0], a1 = q[1];
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(q[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) {
          const unsigned ix = unsigned((bits >> (3 * i)) & 7);
          unsigned v;
          if (ix == 0) v = a0;
          else if (ix == 1) v = a1;
          else if (a0 > a1) v = ((8 - ix) * a0 + (ix - 1) * a1) / 7;
          else if (ix == 6) v = 0;
          else if (ix == 7) v = 255;
          else v = ((6 - ix) * a0 + (ix - 1) * a1) / 5;
          below += v < 255;
        }
      }
    }
  } else if (t.format == TexFormat::kB8G8R8A8 || t.format == TexFormat::kR8G8B8A8) {
    if (t.data.size() < px * 4) return 0.f;
    n = px;
    for (size_t i = 0; i < px; ++i) below += t.data[i * 4 + 3] != 255;
  } else if (t.format == TexFormat::kB4G4R4A4) {
    if (t.data.size() < px * 2) return 0.f;
    n = px;
    for (size_t i = 0; i < px; ++i) below += (t.data[i * 2 + 1] >> 4) != 15;
  }
  return n ? float(double(below) / double(n)) : 0.f;
}

// S103 P1: two statistics of a BC2 / BC3 texture's top-mip alpha: the share of exactly-zero texels and the mean (0..1). The game draws a material whose alpha has (almost) no zeros as a
// highlight mask (the FX_ALPHA_SPEC / FX_ALPHA_GLOSS shader, no discard); one with many zeros is a cut-out (docs\formats\arena_slot_flags.md, the arena-4 table).
inline bool AlphaStats(const Texture& t, double& zero_share, double& mean) {
  const size_t blocks = size_t((std::max(t.width, 1u) + 3) / 4) * ((std::max(t.height, 1u) + 3) / 4);
  zero_share = 1.0, mean = 0.0;
  if (blocks == 0 || t.data.size() < blocks * 16) return false;
  size_t zero = 0;
  double sum = 0;
  for (size_t b = 0; b < blocks; ++b) {
    const uint8_t* q = &t.data[b * 16];
    if (t.format == TexFormat::kBC2) {
      for (int i = 0; i < 16; ++i) {
        const unsigned v = ((q[i / 2] >> (4 * (i & 1))) & 15) * 17;
        zero += v == 0, sum += v;
      }
    } else {
      const unsigned a0 = q[0], a1 = q[1];
      uint64_t bits = 0;
      for (int i = 0; i < 6; ++i) bits |= uint64_t(q[2 + i]) << (8 * i);
      for (int i = 0; i < 16; ++i) {
        const unsigned ix = unsigned((bits >> (3 * i)) & 7);
        unsigned v;
        if (ix == 0) v = a0;
        else if (ix == 1) v = a1;
        else if (a0 > a1) v = ((8 - ix) * a0 + (ix - 1) * a1) / 7;
        else if (ix == 6) v = 0;
        else if (ix == 7) v = 255;
        else v = ((6 - ix) * a0 + (ix - 1) * a1) / 5;
        zero += v == 0, sum += v;
      }
    }
  }
  const double n = double(blocks) * 16.0;
  zero_share = double(zero) / n, mean = sum / n / 255.0;
  return true;
}

// S103 L5 planted twin (the old behaviour: every arena material with a level > 0 takes a highlight pair): dev builds only, read once.
inline bool L5PlantOldHighlight() {
#ifdef RDR_DEV
  static const bool v = std::getenv("WWE_RDR_L5_PLANT") != nullptr;
  return v;
#else
  return false;
#endif
}
// S103 L10 planted twin (the per-pixel-only program for every arena LM mesh: the old behaviour): dev builds only, read once.
inline bool L10PlantPerPixel() {
#ifdef RDR_DEV
  static const bool v = std::getenv("WWE_RDR_L10_PLANT") != nullptr;
  return v;
#else
  return false;
#endif
}
// The planted twin of the P1 gate (the old 0.5 cut-out on the pad): dev builds only, read once.
inline bool P1PlantOldAlpha() {
#ifdef RDR_DEV
  static const bool v = std::getenv("WWE_RDR_P1_PLANT") != nullptr;
  return v;
#else
  return false;
#endif
}

// S101 atlas: what the file DECLARES for one mesh record (effect name, every named parameter with its value, the stream strides, the
// index block), filled for every record the loader meets - also the ones it then skips (loaded = false; the reason is in
// Model::warnings from warn_begin on). Read by tools\atlas\atlas.cpp only; the loader itself ignores it. Format: docs\formats\drawables_atlas.md.
struct JboyDeclParam {
  std::string name;
  uint32_t kind = 0;       // the type word's high half: 0x0F sampler, 0x0D float4, 0x0A float (others kept as stored)
  uint32_t size = 0;       // the type word's low half
  float v[4] = {};         // float4 / float values
  int32_t tex_index = -1;  // samplers: index into the model's texture-name table (-1 = none)
  std::string tex_name;    // samplers: that table's name ("" when none / out of range)
};
struct JboyDecl {
  uint32_t mesh_index = 0, vertices = 0, palette = 0;
  uint32_t stride[4] = {28, 0, 0, 0};  // bytes per vertex of S0..S3 (as the loader derives them from the stream offsets)
  uint32_t index_count = 0, index_groups = 0;
  bool tiles = false, s1_skin = false, loaded = false;
  std::string effect;
  std::vector<JboyDeclParam> params;
  size_t warn_begin = 0;  // index in Model::warnings where this record's warnings start
};

struct JboyOptions {
  std::vector<JboyDecl>* decl = nullptr;  // S101 atlas: when set, one JboyDecl per mesh record is appended
  bool arena = false;      // arenas: no skeleton (S1 = skin pairs, S2 = UV as for every model)
  std::string part;        // "body", "piece 10000", "slot 12"...
  TexSet* tex = nullptr;   // where texture names resolve
  // Session 97: an arena piece with its own small skeleton (a crowd body): vertex bones are the piece's node indices and
  // the node records are returned here (file space), so the caller can pose it.
  std::vector<Bone>* own_nodes = nullptr;
  // Session 98 (step 16): the source recorded on every mesh (Mesh::src_*): kind 1 arena, 2 wrestler, 3 crowd, 4 prop.
  int kind = 0;
  std::string pac;
  int key = 0;
  // Session 99 (scene objects): a prop's model slot 1..9 (asset_load.h LoadWrestlerPac prop_slots). Its yProp_* materials whose diffuse
  // alpha cuts out (AlphaCutsOut) are drawn alpha-blended: Fandango 2591's slot-5 "beamglow02 / 03" light beams drew as opaque dark
  // cones. [U] the game's blend state for prop materials is not decoded (judged by the picture; slot 0 pieces keep the old rule).
  bool prop_slot = false;
  // S111 UNIFY5 review: the model is a character's (a wrestler pac's body / pieces through LoadWrestlerPac; not an event prop through LoadProp, not a prop slot): Material::char_part on every material.
  bool char_part = false;
};

namespace jboy_detail {

struct Reader {
  const uint8_t* b;
  size_t n;
  bool ok(uint64_t off, uint64_t len) const { return off <= n && len <= n - off; }
  uint32_t u32(uint64_t off) const { return ok(off, 4) ? Be32(b + off) : 0; }
  float f32(uint64_t off) const { return ok(off, 4) ? BeF(b + off) : 0.f; }
};

inline std::string CString(const uint8_t* p, size_t n) {
  size_t l = 0;
  while (l < n && p[l]) ++l;
  return std::string(reinterpret_cast<const char*>(p), l);
}

// Triangle strip -> triangle list. Alternate winding per strip triangle (D3D order); degenerates dropped.
// `bad` counts indices >= vcount (those triangles are dropped).
inline void StripToList(const std::vector<uint16_t>& idx, uint32_t vcount, std::vector<uint32_t>& out, uint32_t& bad,
                        uint32_t& degenerate) {
  size_t run = 0;
  for (size_t i = 0; i < idx.size(); ++i) {
    if (idx[i] == 0xFFFF) {  // restart marker (unconfirmed: not expected in these files)
      run = 0;
      continue;
    }
    ++run;
    if (run < 3) continue;
    uint32_t a = idx[i - 2], b = idx[i - 1], c = idx[i];
    if (a >= vcount || b >= vcount || c >= vcount) {
      ++bad;
      continue;
    }
    if (a == b || b == c || a == c) {
      ++degenerate;
      continue;
    }
    if ((run & 1) == 0) std::swap(a, b);  // run == 3: first triangle kept, run == 4 flipped
    out.push_back(a), out.push_back(b), out.push_back(c);
  }
}

}  // namespace jboy_detail

// Appends the meshes / materials (and, for wrestlers, bones) of one JBOY blob to `m`. False + err on a structural
// failure (the model is left with whatever was appended before).
inline bool LoadJboy(const Bytes& blob, const JboyOptions& o, Model& m, std::string& err) {
  using namespace jboy_detail;
  if (blob.size() < 0x48 || std::memcmp(blob.data(), "JBOY", 4) != 0) return err = o.part + ": not a JBOY", false;
  const uint32_t body_len = Be32(blob.data() + 4);
  if (body_len < 0x40 || size_t(body_len) + 8 > blob.size()) return err = o.part + ": JBOY size past the entry", false;
  const Reader r{blob.data() + 8, body_len};
  const uint32_t nmesh = r.u32(0x10), mesh_tab = r.u32(0x14), nnode = r.u32(0x18), ntex = r.u32(0x1C),
                 node_tab = r.u32(0x20), tex_tab = r.u32(0x24);
  if (nmesh > 512 || !r.ok(mesh_tab, uint64_t(nmesh) * 0xB8)) return err = o.part + ": mesh table out of range", false;
  if (nnode > 4096 || !r.ok(node_tab, uint64_t(nnode) * 80)) return err = o.part + ": node table out of range", false;
  if (ntex > 256 || !r.ok(tex_tab, uint64_t(ntex) * 16)) return err = o.part + ": texture-name table out of range", false;

  // Skeleton (wrestlers): merge by name into the model's bone list; node_map[piece node] = model bone index.
  std::vector<int> node_map(nnode, 0);
  std::vector<Matrix> corr;  // per piece node: piece bind space -> main bind space (S97)
  bool use_corr = false;
  if (o.arena && o.own_nodes && nnode <= 256) {
    o.own_nodes->clear();
    for (uint32_t i = 0; i < nnode; ++i) {
      const uint8_t* n = r.b + node_tab + 80ull * i;
      Bone bn;
      bn.name = CString(n, 16);
      const uint32_t par = Be32(n + 48);
      bn.parent = par < i ? int(par) : -1;
      float eul[3];
      for (int k = 0; k < 3; ++k) bn.local_translation[k] = BeF(n + 16 + 4 * k), eul[k] = BeF(n + 32 + 4 * k);
      EulerXYZToQuat(eul, bn.local_rotation);
      o.own_nodes->push_back(bn);
      node_map[i] = int(i);
    }
  }
  if (!o.arena) {
    std::map<std::string, std::vector<int>> by_name;
    for (size_t i = 0; i < m.bones.size(); ++i) by_name[m.bones[i].name].push_back(int(i));
    std::map<std::string, int> seen;
    int appended = 0;
    const bool wrestler_piece = load_detail::SpringNodes() && o.kind == 2 && o.key != 0 && !o.prop_slot;   // S103 W2B: a SUBM piece entry of a wrestler attire (the body is key 0)
    for (uint32_t i = 0; i < nnode; ++i) {
      const uint8_t* n = r.b + node_tab + 80ull * i;
      const std::string name = CString(n, 16);
      const int occ = seen[name]++;
      if (wrestler_piece) {
        // S103 W2B (docs\agents\s103\W2.md section 1, W2_ADV.md rows 1a / 1b / R1 / R5): the game's node map of a piece node (sub_8259E720) is -1 for a name starting `kami_b` or `spring` (6 bytes, case-sensitive, EVEN when
        // the owner has that name: the spring-hair simulation poses the piece's own copy), else sub_82BD24C8 on the OWNER's node table only: the FIRST owner node of that name, else the alias table's `to` name
        // (node_alias.h) looked up the same way, else -1. A -1 node is appended as its own bone with its (piece id, node index) tag; the body keeps its own copy of a shared name. Forced names are never looked up in
        // or pushed into by_name (R5), and a piece never maps to another piece's appended bone.
        const bool forced = name.compare(0, 6, "kami_b") == 0 || name.compare(0, 6, "spring") == 0;
        int owner_node = -1;
        if (!forced) {
          const auto first_body = [&](const std::string& nm) -> int {
            for (size_t k = 0; k < m.main_bones && k < m.bones.size(); ++k)
              if (m.bones[k].name == nm) return int(k);
            return -1;
          };
          owner_node = first_body(name);
          if (owner_node < 0)
            if (const char* to = NodeAliasOf(name.c_str()))
              if (std::strcmp(to, "null") != 0) owner_node = first_body(to);
        }
        if (owner_node >= 0) {
          node_map[i] = owner_node;
          continue;
        }
      } else {
        auto it = by_name.find(name);
        if (it != by_name.end() && occ < int(it->second.size())) {
          node_map[i] = it->second[size_t(occ)];
          continue;
        }
      }
      Bone bn;
      bn.name = name;
      if (wrestler_piece) bn.piece_id = o.key, bn.piece_node = int(i), bn.piece_nodes = nnode;
      const uint32_t par = Be32(n + 48);
      bn.parent = par < i ? node_map[par] : -1;  // parents come before children (unconfirmed for every file)
      if (par != 0xFFFFFFFFu && par >= i) m.warnings.push_back("node parent index not before its child");
      // Session 97 (skeleton reader, out\runtime\s97\skeleton.md): +16 parent-relative translation, +32 euler angles x y z
      // in radians (R = Rz Ry Rx), +48 parent, +64 a bind-space bounding sphere of the bone's vertices (not read).
      float eul[3];
      for (int k = 0; k < 3; ++k) bn.local_translation[k] = BeF(n + 16 + 4 * k), eul[k] = BeF(n + 32 + 4 * k);
      EulerXYZToQuat(eul, bn.local_rotation);
      m.bones.push_back(bn);
      node_map[i] = int(m.bones.size()) - 1;
      if (!wrestler_piece) by_name[name].push_back(node_map[i]);   // S103 W2B: a piece's appended bone is never an owner node (the game looks in the owner only)
      ++appended;
    }
    if (o.kind == 2 && o.key == 0 && !o.prop_slot && m.main_bones == 0) m.main_bones = uint32_t(m.bones.size());   // S103 W2B: the body entry's own nodes; every bone after this was added by a piece
    if (appended && appended != int(nnode) && !m.meshes.empty())
      m.warnings.push_back("piece has nodes not in the main skeleton (appended as extra bones)");
    if (m.bones.size() > 65535) m.warnings.push_back("more than 65535 bones: 16-bit bone indices clamp");  // (Session 99: was 8 bits)
    // Session 97 integration: a piece's vertices are in the bind space of the piece's OWN node table, which need not be
    // the main skeleton's (the Rock's elbow pads: the piece's arm chain hangs from a `root` at the origin, so they drew
    // at the feet). Per node: corr = inverse(piece_world) * main_world (row vectors, file space); identity when the
    // piece's skeleton matches the main one.
    std::vector<Matrix> pw(nnode), mw(m.bones.size());
    for (uint32_t i = 0; i < nnode; ++i) {
      const uint8_t* n = r.b + node_tab + 80ull * i;
      float t[3], e[3], q[4];
      for (int k = 0; k < 3; ++k) t[k] = BeF(n + 16 + 4 * k), e[k] = BeF(n + 32 + 4 * k);
      EulerXYZToQuat(e, q);
      const uint32_t par = Be32(n + 48);
      pw[i] = par < i ? Mul(FromQuatT(q, t), pw[par]) : FromQuatT(q, t);
    }
    for (size_t i = 0; i < m.bones.size(); ++i) {
      const Bone& b = m.bones[i];
      const Matrix local = FromQuatT(b.local_rotation, b.local_translation);
      mw[i] = b.parent >= 0 && size_t(b.parent) < i ? Mul(local, mw[size_t(b.parent)]) : local;
    }
    corr.resize(nnode);
    for (uint32_t i = 0; i < nnode; ++i) {
      corr[i] = Mul(InverseRigid(pw[i]), mw[size_t(node_map[i])]);
      bool non_identity = false;
      for (int k = 0; k < 16; ++k)
        if (std::fabs(corr[i][k] - Identity()[k]) > 1e-3f * (k >= 12 ? 100.f : 1.f)) use_corr = true, non_identity = true;
      // S103 W2B (review F): an appended piece node whose piece bind differs from the body's chain (the Rock's pads): our vertices were moved by corr, so the game's piece row P_j must be premultiplied by corr^-1
      // (vertex_piece x P_j = vertex_ours x corr^-1 x P_j). The record's rows are the game's own; rf_wrestlers applies this to the extra's row.
      if (non_identity && wrestler_piece) {
        Bone& ab = m.bones[size_t(node_map[i])];
        if (ab.piece_id == o.key && ab.piece_node == int(i)) ab.has_corr = true, ab.corr_inv = InverseRigid(corr[i]);
      }
    }
  }

  for (uint32_t mi = 0; mi < nmesh; ++mi) {
    const uint32_t rec = mesh_tab + mi * 0xB8;
    const uint32_t N = r.u32(rec);
    JboyDecl* dc = nullptr;  // S101 atlas: the declaration, recorded before any skip decision
    if (o.decl) {
      o.decl->push_back({});
      dc = &o.decl->back();
      dc->mesh_index = mi, dc->vertices = N, dc->palette = r.u32(rec + 8), dc->warn_begin = m.warnings.size();
      dc->effect = CString(r.b + rec + 0x7C, 16);
      const uint32_t dn = r.u32(rec + 0x94), dl = r.u32(rec + 0x98);
      if (dn <= 512 && r.ok(dl, uint64_t(dn) * 4))
        for (uint32_t k = 0; k < dn; ++k) {
          const uint32_t po = r.u32(dl + 4ull * k);
          if (!r.ok(po, 24)) continue;
          JboyDeclParam p;
          p.name = CString(r.b + po, 16);
          const uint32_t tw = r.u32(po + 16);
          p.kind = tw >> 16, p.size = tw & 0xFFFF;
          if (p.kind == 0x0F) {
            p.tex_index = int32_t(r.u32(po + 20));
            if (p.tex_index >= 0 && uint32_t(p.tex_index) < ntex) p.tex_name = CString(r.b + tex_tab + 16ull * uint32_t(p.tex_index), 16);
          } else if (p.kind == 0x0D && r.ok(po + 20, 16)) {
            for (int c = 0; c < 4; ++c) p.v[c] = r.f32(po + 20 + 4ull * uint32_t(c));
          } else if (p.kind == 0x0A && r.ok(po + 20, 4)) {
            p.v[0] = r.f32(po + 20);
          }
          dc->params.push_back(std::move(p));
        }
    }
    if (N == 0 || N > 400000) {
      m.warnings.push_back("mesh with zero or absurd vertex count skipped");
      continue;
    }
    const uint32_t npal = std::min<uint32_t>(r.u32(rec + 8), 20);
    uint32_t pal[20];
    for (uint32_t k = 0; k < 20; ++k) pal[k] = r.u32(rec + 0x0C + 4 * k);
    const uint32_t s0 = r.u32(rec + 0x68), s1 = r.u32(rec + 0x6C), s2 = r.u32(rec + 0x70), s3 = r.u32(rec + 0x74),
                   ib = r.u32(rec + 0x9C);
    // Streams must tile: S0 (28 B) .. S1 .. S2 (8 B) .. S3 (0 or 12 B) .. index block.
    bool tiles = true;
    const uint64_t s0d = uint64_t(s0) + 4;  // (64-bit: a malformed s0 near 4 GB must not wrap - Session 96 review)
    if (s0d + uint64_t(N) * 28 != s1) tiles = false;  // S0 data starts 4 bytes after its offset (measured)
    const uint64_t d1 = s2 > s1 ? s2 - s1 : 0, d2 = s3 > s2 ? s3 - s2 : 0, d3 = ib > s3 ? ib - s3 : 0;
    const uint32_t st1 = uint32_t(d1 / N), st2 = uint32_t(d2 / N), st3 = uint32_t(d3 / N);
    if (d1 % N || d2 % N || d3 % N) tiles = false;
    if (st1 != 8 && st1 != 16 && st1 != 24 && st1 != 32) tiles = false;
    if (st2 != 8) tiles = false;
    if (st3 != 0 && st3 != 12) tiles = false;
    if (dc) dc->stride[1] = st1, dc->stride[2] = st2, dc->stride[3] = st3, dc->tiles = tiles;  // S101 atlas
    if (!tiles) {
      m.warnings.push_back("mesh streams do not tile exactly: mesh skipped");
      continue;
    }
    if (!r.ok(s0d, uint64_t(N) * 28) || !r.ok(s1, d1) || !r.ok(s2, d2) || !r.ok(s3, d3)) {
      m.warnings.push_back("mesh stream past the end of the body: mesh skipped");
      continue;
    }
    // S111 GUESTVTX review: the skin-pair count is the record's (+0x5C, vdecode::GamePairCount), the count the game's packer reads S1 by. A skinned
    // body whose count is above 4 or runs past its S1 stream cannot be read as the game reads it (no mesh on the disc has one).
    const uint32_t pairs = vdecode::GamePairCount(r.u32(rec + 0x5C), st1, nnode);
    if (pairs > 4 || uint64_t(pairs) * 8 > st1) {
      m.warnings.push_back("skin pair count (mesh record +0x5C) above 4 or past the S1 stream: mesh skipped");
      continue;
    }
    if (!r.ok(ib, 12)) {
      m.warnings.push_back("index block past the end of the body: mesh skipped");
      continue;
    }
    // Index block: one or more groups {type, count, offset}, 12 bytes each, back to back; group k's BE u16 indices are at
    // its own offset (a single group: right after the head). The type word is 6 (triangle strip), stored either as
    // bytes 06 00 00 00 or 00 00 00 06 (both seen; unconfirmed why).
    const uint32_t first_ptr = r.u32(ib + 8);
    if (first_ptr < ib + 12 || (first_ptr - ib) % 12 != 0 || (first_ptr - ib) / 12 > 64 || !r.ok(ib, first_ptr - ib)) {
      m.warnings.push_back("index block head not understood: mesh skipped");
      continue;
    }
    const uint32_t ngroups = (first_ptr - ib) / 12;
    struct Group {
      uint32_t count, ptr;
    };
    std::vector<Group> groups;
    bool idx_ok = true;
    for (uint32_t g = 0; g < ngroups && idx_ok; ++g) {
      uint32_t prim = 0;
      std::memcpy(&prim, r.b + ib + 12 * g, 4);
      const uint32_t cnt = r.u32(ib + 12 * g + 4), ptr = r.u32(ib + 12 * g + 8);
      if (prim != 6 && prim != 0x06000000) {
        m.warnings.push_back("index primitive type is not 6 (triangle strip): mesh skipped");
        idx_ok = false;
      } else if (!r.ok(ptr, uint64_t(cnt) * 2)) {
        m.warnings.push_back("index data past the end of the body: mesh skipped");
        idx_ok = false;
      } else {
        groups.push_back({cnt, ptr});
      }
    }
    if (dc) {  // S101 atlas
      dc->index_groups = ngroups;
      for (const Group& g : groups) dc->index_count += g.count;
    }
    if (!idx_ok) continue;

    Mesh mesh;
    mesh.part = o.part;
    mesh.src_kind = o.kind, mesh.src_pac = o.pac, mesh.src_key = o.key;  // Session 98 (step 16)
    mesh.src_record = int(mi), mesh.index_groups = ngroups;  // S101 lane 40: for the arena join
    for (const Group& g : groups) mesh.strip_indices += g.count;
    if (o.arena) {   // S103 C4: the file's slot -> node map, raw (an arena model has no merged skeleton: node_map is all zeros)
      for (uint32_t k = 0; k < npal; ++k) mesh.palette_nodes.push_back(pal[k] >= 1 && pal[k] <= nnode ? uint16_t(pal[k] - 1) : uint16_t(0xFFFF));
      if (npal && pal[0] >= 1 && pal[0] <= nnode) mesh.rigid_node = int(pal[0] - 1);
    }
    //   // S103 C4: slot 0's node (the game's c76 = the matrix at r5 + 64 (palette[0] - 1), sub_82C04DF0 0x82C0A038-0x82C0A04C)
    if (nnode > 0) mesh.node = CString(r.b + node_tab, 16);
    for (uint32_t k = 0; k < npal; ++k)
      if (pal[k] >= 1 && pal[k] <= nnode) mesh.palette += (mesh.palette.empty() ? "" : ";") + CString(r.b + node_tab + 80ull * (pal[k] - 1), 16);
    mesh.vertices.resize(N);
    uint32_t nan_count = 0, nonunit = 0, unused_slot = 0, bad_slot = 0, bad_node = 0, zero_w = 0, extra_node = 0;
    // Session 96 integration: arena models with a skeleton (the ring set 951-982, the crowd bodies) store their streams
    // like a wrestler (S1 = skin pairs, S2 = UV); a static mesh stores one rigid pair there (slot 0, weight 1.0). S108 BAN3: both read UV from S2; this only tells
    // the skin pairs apart (bones, weights). Detected by the first vertex's S1: a little-endian slot below 256
    // and a big-endian weight in 0..1 (unconfirmed rule; checked on the ring set of arena 5).
    bool skin_streams = false;
    if (o.arena && nnode > 1 && N > 0 && st1 >= 8 && st1 % 8 == 0) {
      uint32_t slot0;
      std::memcpy(&slot0, r.b + s1, 4);
      const float w0 = BeF(r.b + s1 + 4);
      skin_streams = slot0 < 256 && w0 >= 0.f && w0 <= 1.0001f;
    }
    if (dc) dc->s1_skin = !o.arena || skin_streams;  // S101 atlas
    const bool skin_fx = !o.arena && CString(r.b + rec + 0x7C, 16).rfind("yCh_skin", 0) == 0;   // S111 VERTEXSEAM: the effects that read the colour as inExtra
    for (uint32_t v = 0; v < N; ++v) {
      Vertex& vx = mesh.vertices[v];
      const uint8_t* p0 = r.b + s0d + 28ull * v;
      for (int k = 0; k < 3; ++k) vx.pos[k] = BeF(p0 + 4 * k), vx.normal[k] = BeF(p0 + 12 + 4 * k);
      // Session 96 integration: the colour word. Arenas: bytes A R G B (0xFF C0 C0 C0 = opaque grey; unconfirmed that
      // the first byte is alpha) -> stored R G B A for the viewer. Wrestlers: NOT a colour (the research notes: its last
      // byte picks a material index; multiplying by it turned Batista's skin teal and his blue trunk lettering red) ->
      // white.
      if (o.arena) {
        vx.color = vdecode::GameColour(Be32(p0 + 24));   // S111 GUESTVTX: bytes 25, 26, 27, 24 (the game's fetch order, the one rule)
      } else {
        // Session 98 (wrestler fix 3): bytes 1-3 of the word are one grey value (ch161: 50 on the body skin, 128 / 254 on head and
        // face pieces, 255 on gear [C: byte histograms, all 50 meshes]); the skin vertex shaders pass the colour's third byte on as
        // the first lobe set's gain (vs_ch_skin_a.vert interpolator 9 -> wrestler_skin_entrance.frag in_reflect_gain). Byte 0 is
        // the material-index byte (0 / 8 / 16 / 24 / 40). Kept as RGB = that grey, A = 255 (the viewer reads the gain from R; it never
        // multiplies a wrestler's colour by it). [U: that the GPU vertex word equals the file's S0 word; bytes 1-3 being equal makes
        // the byte order moot.]
        const uint8_t g = p0[25];
        uint8_t c4[4] = {g, g, g, 0xFF};
        if (skin_fx) SkinVertexColour(p0 + 24, c4);   // S111 VERTEXSEAM: A = file byte 24, the packed ID-map / wrinkle slot the skin shader reads as inExtra.w
        std::memcpy(&vx.color, c4, 4);
      }
      if (!std::isfinite(vx.pos[0] + vx.pos[1] + vx.pos[2])) ++nan_count;
      const float nl = std::sqrt(vx.normal[0] * vx.normal[0] + vx.normal[1] * vx.normal[1] + vx.normal[2] * vx.normal[2]);
      if (!(std::fabs(nl - 1.f) < 0.05f)) ++nonunit;
      vx.tangent[0] = vx.tangent[1] = vx.tangent[2] = 0;
      if (st3 == 12)
        for (int k = 0; k < 3; ++k) vx.tangent[k] = BeF(r.b + s3 + 12ull * v + 4 * k);
      if (o.arena && !skin_streams) {
        // S108 BAN3: the game's packer (sub_82BD4BE0) reads UV pairs only from S2 for every mesh; S1 is skin pairs (a static mesh: one
        // rigid pair, slot 0 / weight 1.0). uv1 keeps the same pair for the dev viewer's own light-map read; the game's shaders never read it.
        for (int k = 0; k < 2; ++k) vx.uv0[k] = vx.uv1[k] = BeF(r.b + s2 + 8ull * v + 4 * k);
        for (int k = 0; k < 4; ++k) vx.bones[k] = 0, vx.weights[k] = k == 0 ? 1.f : 0.f;
      } else {
        for (int k = 0; k < 2; ++k) vx.uv0[k] = BeF(r.b + s2 + 8ull * v + 4 * k), vx.uv1[k] = 0;
        int nb = 0;
        uint8_t raw_slot[4] = {255, 255, 255, 255};  // S101 lane 49: the file's own S1 pairs (arena skin streams), kept for the arena drawer's bone-id vertex
        float raw_w[4] = {0, 0, 0, 0};
        uint32_t pnode[4] = {0, 0, 0, 0};
        float sum = 0;
        for (int k = 0; k < 4; ++k) vx.bones[k] = 0, vx.weights[k] = 0;
        for (uint32_t pr = 0; pr < pairs && nb < 4; ++pr) {
          const uint8_t* q = r.b + s1 + 8ull * pairs * v + 8ull * pr;   // the packer's S1 index: 8 bytes a pair of the record's count
          uint32_t slot;
          std::memcpy(&slot, q, 4);  // LE
          const float w_file = BeF(q + 4), w = vdecode::GamePairWeight(pairs, w_file);   // S111 GUESTVTX: a 1-pair vertex weighs 1.0 (the packer stores its slot alone)
          if (pr < 4) raw_slot[pr] = slot < 255 ? uint8_t(slot) : uint8_t(255), raw_w[pr] = w_file;
          if (slot == 255) {
            ++unused_slot;
            continue;
          }
          if (slot >= npal) {
            ++bad_slot;
            continue;
          }
          // Session 97: palette values are 1-based node numbers (sub_82BD7630 reads (p - 1) * 64; on Kane's data 4.2 vs
          // 10.4 mean joint distance). 0 would be no node.
          if (pal[slot] == 0 || pal[slot] > nnode) {
            ++bad_node;
            continue;
          }
          const uint32_t node = pal[slot] - 1;
          if (!(w > 0.f)) continue;
          pnode[nb] = node;
          vx.bones[nb] = uint16_t(std::min(node_map[node], 65535)), vx.weights[nb] = w, sum += w, ++nb;
        }
        if (use_corr && nb > 0 && sum > 0.f) {  // piece bind space -> main bind space, blended by the weights
          float p[3] = {0, 0, 0}, nn[3] = {0, 0, 0}, tg[3] = {0, 0, 0};
          for (int k = 0; k < nb; ++k) {
            const Matrix& t = corr[pnode[k]];
            const float w = vx.weights[k] / sum;
            for (int c = 0; c < 3; ++c) {
              p[c] += w * (vx.pos[0] * t[c] + vx.pos[1] * t[4 + c] + vx.pos[2] * t[8 + c] + t[12 + c]);
              nn[c] += w * (vx.normal[0] * t[c] + vx.normal[1] * t[4 + c] + vx.normal[2] * t[8 + c]);
              tg[c] += w * (vx.tangent[0] * t[c] + vx.tangent[1] * t[4 + c] + vx.tangent[2] * t[8 + c]);
            }
          }
          for (int c = 0; c < 3; ++c) vx.pos[c] = p[c], vx.normal[c] = nn[c], vx.tangent[c] = tg[c];
        }
        if (o.arena && skin_streams) {  // S101 lane 49
          for (int k = 0; k < 4; ++k) mesh.src_skin_slots.push_back(raw_slot[k]), mesh.src_skin_weights.push_back(raw_w[k]);
          mesh.src_skin_pairs = pairs;
        }
        if (nb == 0 || !(sum > 0.f)) {
          ++zero_w;
          // No usable influence: bind rigidly to the palette's first node (unconfirmed fallback).
          vx.bones[0] = npal && pal[0] >= 1 && pal[0] <= nnode ? uint16_t(std::min(node_map[pal[0] - 1], 65535)) : 0;
          if (use_corr && npal && pal[0] >= 1 && pal[0] <= nnode) {  // (S97 review: the fallback is corrected too)
            const Matrix& t = corr[pal[0] - 1];
            const float p0[3] = {vx.pos[0], vx.pos[1], vx.pos[2]}, n0[3] = {vx.normal[0], vx.normal[1], vx.normal[2]};
            for (int c = 0; c < 3; ++c) {
              vx.pos[c] = p0[0] * t[c] + p0[1] * t[4 + c] + p0[2] * t[8 + c] + t[12 + c];
              vx.normal[c] = n0[0] * t[c] + n0[1] * t[4 + c] + n0[2] * t[8 + c];
            }
          }
          vx.weights[0] = 1.f;
        } else if (std::fabs(sum - 1.f) > 1e-4f) {
          for (int k = 0; k < 4; ++k) vx.weights[k] /= sum;
        }
      }
    }
    if (nan_count) m.warnings.push_back("mesh has NaN/inf positions");
    if (nonunit) m.warnings.push_back("mesh has normals that are not unit length (seen on slots 272-277 with length 0.75: unconfirmed what they are)");
    if (bad_slot) m.warnings.push_back("skin slot beyond the palette");
    if (bad_node) m.warnings.push_back("palette node beyond the node table (influence dropped)");
    (void)extra_node;
    mesh.skinned = !o.arena;  // wrestler pieces follow the pose; the arena's skinned pieces are stored placed (S97)
    if (zero_w) m.warnings.push_back("vertices with no usable skin weight (bound to palette[0])");

    uint32_t bad_tri = 0, degen = 0;
    for (const Group& g : groups) {
      std::vector<uint16_t> idx(g.count);
      for (uint32_t i = 0; i < g.count; ++i) idx[i] = Be16(r.b + g.ptr + 2ull * i);
      StripToList(idx, N, mesh.indices, bad_tri, degen);
    }
    if (bad_tri) m.warnings.push_back("strip index beyond the vertex count (triangle dropped)");
    if (mesh.indices.empty()) {
      m.warnings.push_back("mesh produced no triangles: skipped");
      continue;
    }

    // Material.
    Material mat;
    // S111 UNIFY5: the record's header through the ONE first step the created-superstar build runs too (material_record.h BeginMaterial): the effect +0x7C, its name table (S111 BODYSPEC: the family
    // and the yCh missing-power default, before the parameter walk below), the technique word +0x90 (S103 D1: the technique index, PLAN.md skeleton R0: sub_82C04DF0 0x82C05AF8) and the cull word
    // +0x8C (the SCU / C3 rule below) -> technique, cull_word, cull_from_record (bit 0x2), cull_back.
    matrec::BeginMaterial(mat, CString(r.b + rec + 0x7C, 16), r.u32(rec + 0x90), r.u32(rec + 0x8C));
    mat.char_part = o.char_part;   // S111 UNIFY5 review: drawn by a character's nodes (the caster set, wrestler_caster.h)
    mat.vertex_lm = o.arena && mat.shader.compare(0, 4, "yBG_") == 0 && (mat.technique & 9u) == 1u && !L10PlantPerPixel();  // S103 L10: record word +0x90 (bit 0 set, bit 3 clear) = the game's vertex-LM program (docs/formats/arena_lightmap.md "S103 L10")
    // SCU (docs/research/harness/SCU_contract.md, CUL_ADV.md A / E [C]): the game's model draw sub_82C04DF0 reads record word +0x8C (BE u32) in its cull block
    // 0x82C0A880-0x82C0A914: bit 0x2 set = ONE sub-pass, cull on with the entry winding (2 for a normal camera = cull back, front clockwise); clear (model word t = 0 for every
    // stage model) = one sub-pass, cull off. The word is the file's (no writer in the model code). Only the stage slots 0..399 of an arena load (kind 1) take it here: the crowd,
    // props, weapons, the ring keys, the screens 800-809 (their own rule, rf_c_core.cpp) and characters keep their own cull (not audited for this bit: CUL U4).
    // S105 C3 (docs/research/s105/C3.md): the same block serves EVERY family (stage, ring, props, weapons, structures, entrance props, gm objects, characters / attires): bit set = back faces culled, for every JBOY mesh.
    // The `r` copy's winding swap stays with the stage slots (stage_slot). The screens (rf_c_core.cpp), the crowd (live_crowd.cpp) and the flash sprite (rf_c_core.cpp) set their own rule after the load.
    mat.stage_slot = o.arena && o.kind == 1 && o.key >= 0 && o.key < 400;
    const uint32_t nparam = r.u32(rec + 0x94), plist = r.u32(rec + 0x98);
    if (nparam > 512 || !r.ok(plist, uint64_t(nparam) * 4)) {
      m.warnings.push_back("material parameter list out of range");
    } else {
      for (uint32_t k = 0; k < nparam; ++k) {
        const uint32_t po = r.u32(plist + 4ull * k);
        if (!r.ok(po, 24)) {
          m.warnings.push_back("material parameter record out of range");
          continue;
        }
        const std::string pname = CString(r.b + po, 16);
        const uint32_t kind = r.u32(po + 16) >> 16;
        if (kind == 0x0F) {  // sampler: index into the texture-name table
          const int32_t ti = int32_t(r.u32(po + 20));
          const matsamp::Ctx sctx{o.arena, skin_streams};
          const bool in_table = ti >= 0 && uint32_t(ti) < ntex;
          const std::string tname = in_table ? CString(r.b + tex_tab + 16ull * uint32_t(ti), 16) : std::string();
          int* slot = matsamp::SlotOf(mat, pname, sctx);   // S111 UNIFY2: the ONE sampler table (material_samplers.h: the old chain, moved), shared with the created-superstar build (exact compare: the file's names)
          matsamp::ApplySamplerFacts(mat, pname, ti, in_table, tname, sctx);   // S111 UNIFY5: spec_pow_prm, has_lm_slot, lm_stage_page, cube_name, diffuse_name: the ONE step the created-superstar build runs on its section-72 rows (material_samplers.h)
          if (!slot || ti < 0) continue;
          if (uint32_t(ti) >= ntex) {
            m.warnings.push_back("sampler index beyond the texture-name table");
            continue;
          }
          if (!o.tex) continue;
          if (slot == &mat.noise && tname.empty()) continue;   // S105 C2LA F5: an empty sampNoisePrm name is no texture (no warning)
          *slot = o.tex->Get(m, tname);
        } else if ((kind == 0x0D && r.ok(po + 20, 16)) || (kind == 0x0A && r.ok(po + 20, 4))) {   // S111 GUESTHAIR: the float rows by name through ONE table (material_floats.h: the old chain, moved), shared with the created-superstar build
          float v[4] = {0, 0, 0, 0};
          for (uint32_t c = 0; c < (kind == 0x0D ? 4u : 1u); ++c) v[c] = r.f32(po + 20 + 4ull * c);
          if (!(kind == 0x0A && load_detail::C2lNoisePlant() && (pname == "g_fNoiseLvl" || pname == "g_iNoiseTileX" || pname == "g_iNoiseTileY")))   // the C2L twin drops only the three noise values
            matfloat::ApplyFloat(mat, pname, kind, v, &m.warnings);   // (exact compare: the file's names)
        }
      }
    }
    const std::string sl = Lower(mat.shader);
    if ((o.arena || o.prop_slot) && mat.diffuse >= 0 && size_t(mat.diffuse) < m.textures.size()) mat.diffuse_alpha_clear = AlphaClearShare(m.textures[size_t(mat.diffuse)]);   // S103 B2
    if (o.arena && TqfClass(sl)) {   // S103 TQF: any vertex colour alpha < 255 (the S0 colour word's byte 0, stored as the Vertex::color alpha byte): the game's yProp draw blends at it (Material::vertex_alpha_lt255)
      for (const Vertex& vx : mesh.vertices) if ((vx.color >> 24) < 255u) { mat.vertex_alpha_lt255 = true; break; }
    }
    if (o.kind == 2 && !o.prop_slot && sl.rfind("ych_", 0) == 0) mat.translucent_draw = mat.alpha_blend = assets::char_layer::FileTranslucent(o.pac, o.key, r.u32(rec + 0x78));   // S111 LAYERS (char_draw_layer.h): a character part blends when the game draws it in the translucent draw (a stock piece; a created part of layer 5..7: record +0x78 through 0x82007F60), not by its effect name
    else if (sl.find("trans") != std::string::npos) mat.alpha_blend = true, mat.translucent_draw = true;   // (S111 LAYERS review: no draw split known here: the name rule stands for the node too)
    if (o.arena && mat.diffuse >= 0 && !mat.alpha_blend) {
      const TexFormat f = m.textures[size_t(mat.diffuse)].format;  // first rule: alpha-carrying DXT -> cut-out (unconfirmed)
      // Session 97 (props2): only when the alpha really cuts shapes out: a texture whose alpha never goes (nearly) transparent carries a
      // gloss / specular mask there (ar_dm floor mats, ar_stair, ar_rop01: alpha 60-110 everywhere), and cutting at 0.5 made the floor
      // mats and stairs vanish [C: alpha histograms of the arena 5 textures; the cut-outs (fences, truss, chairs, haze cards) have 20-86 %
      // of their pixels at alpha <= 32, the masks 0-3 %].
      if (f == TexFormat::kBC3 || f == TexFormat::kBC2) {
        const bool cuts = AlphaCutsOut(m.textures[size_t(mat.diffuse)], CPFKeepOld(sl, mat));
        if (TqfSkipsTexelRule(sl)) mat.texel_cutout |= cuts ? 1 : 0;   // S103 TQF: the yProp_BumpMap / Default / Reflect class: the verdict is kept, not applied (the game's technique decides, front_end.cpp GameTestsAlpha)
        else mat.alpha_test = cuts;
      }
      if ((f == TexFormat::kBC3 || f == TexFormat::kBC2) && (sl == "ybg_default" || sl == "yprop_default") && mat.specular < 0 && mat.normal < 0 && !P1PlantOldAlpha()) {  // S103 P1: see Material::alpha_mask
        double zero = 1.0, mean = 0.0;
        if (AlphaStats(m.textures[size_t(mat.diffuse)], zero, mean) && zero < 0.02 && mean >= 0.10) mat.alpha_mask = true, mat.alpha_test = false, mat.texel_cutout &= ~1;
      }
      // S103 L5: the game draws every other plain arena material WITHOUT the highlight (the stairs / tier walls / exit frame of arena 4 are `DefaultLM_base`, lev 0.5, DXT1); a mask material keeps its pair.
      if ((sl == "ybg_default" || sl == "yprop_default") && mat.specular < 0 && mat.normal < 0 && !mat.alpha_mask && !L5PlantOldHighlight()) mat.no_highlight = true;
    }
    if (o.prop_slot && mat.diffuse >= 0 && !mat.alpha_blend && size_t(mat.diffuse) < m.textures.size()) {  // (Session 99, JboyOptions)
      const TexFormat f = m.textures[size_t(mat.diffuse)].format;
      if ((f == TexFormat::kBC3 || f == TexFormat::kBC2) && AlphaCutsOut(m.textures[size_t(mat.diffuse)], CPFKeepOld(sl, mat))) {  // S103 F
        if (TqfSkipsTexelRule(sl)) mat.texel_cutout |= 2;   // S103 TQF: kept, applied by the front end only where the picked variant shows the diffuse alpha (AddModelImpl)
        else mat.alpha_blend = true, mat.cutout_blend = 1;
      }
    }
    if (mat.diffuse < 0) {
      // Not an error: plain-coloured parts exist; counted so the census shows how common it is.
      m.warnings.push_back("material without a diffuse texture");
    }
    m.materials.push_back(mat);
    mesh.material = int(m.materials.size()) - 1;
    if (dc) dc->loaded = true;  // S101 atlas
    m.meshes.push_back(std::move(mesh));
  }
  return true;
}

// Bounds over all vertices (NaN positions ignored). Call once after the last piece.
inline void FinishModel(Model& m) {
  // Session 96 integration: the files' up axis is -Y (Kane's vertices run y -195 .. -2, arena 5's stands rise to
  // y -4913, found by the dumper's bounds) and its handedness is the other one from D3D's (with a 180-degree turn about X,
  // Batista's trunk lettering read mirrored). So only Y is negated: a mirror, which turns the files' -Y-up space into a
  // +Y-up space of the viewer's handedness (lettering reads correctly). Triangle winding flips with it; the viewer draws
  // both faces (D3D11_CULL_NONE), so nothing depends on it yet.
  for (Mesh& me : m.meshes)
    for (Vertex& v : me.vertices) {
      v.pos[1] = -v.pos[1];
      v.normal[1] = -v.normal[1];
      v.tangent[1] = -v.tangent[1];
    }
  // Session 97: bones keep their file-space locals (motions are in file space too); the bind matrices are chained in
  // file space (world = local * parent, row vectors) and mirrored into viewer space.
  std::vector<Matrix> world(m.bones.size());
  for (size_t i = 0; i < m.bones.size(); ++i) {
    Bone& b = m.bones[i];
    const Matrix local = FromQuatT(b.local_rotation, b.local_translation);
    world[i] = b.parent >= 0 && size_t(b.parent) < i ? Mul(local, world[size_t(b.parent)]) : local;
    b.bind_world = MirrorY(world[i]);
    b.inv_bind = InverseRigid(b.bind_world);
  }
  bool first = true;
  auto grow = [&](const float p[3]) {
    for (int k = 0; k < 3; ++k) {
      if (first || p[k] < m.bounds_min[k]) m.bounds_min[k] = p[k];
      if (first || p[k] > m.bounds_max[k]) m.bounds_max[k] = p[k];
    }
    first = false;
  };
  for (const Mesh& me : m.meshes) {
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : me.vertices) {
      if (!std::isfinite(v.pos[0] + v.pos[1] + v.pos[2])) continue;
      for (int k = 0; k < 3; ++k) mn[k] = std::min(mn[k], v.pos[k]), mx[k] = std::max(mx[k], v.pos[k]);
    }
    if (mn[0] > mx[0]) continue;
    // Session 97: a placed mesh counts at every placement (the corners of its box, transformed).
    const bool placed = me.placement >= 0 && size_t(me.placement) < m.placements.size();
    const std::vector<Matrix> one{Identity()};
    for (const Matrix& t : placed ? m.placements[size_t(me.placement)].transforms : one)
      for (int c = 0; c < 8; ++c) {
        const float p[3] = {(c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2]};
        const float q[3] = {p[0] * t[0] + p[1] * t[4] + p[2] * t[8] + t[12], p[0] * t[1] + p[1] * t[5] + p[2] * t[9] + t[13],
                            p[0] * t[2] + p[1] * t[6] + p[2] * t[10] + t[14]};
        grow(q);
      }
  }
}

}  // namespace assets
