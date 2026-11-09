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
//             arenas: UV sets, 8 B each, the first is the diffuse UV)
//   +0x70 S2 (8 B: wrestlers = UV; arenas = lightmap UV)
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
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"
#include "dds.h"
#include "pac.h"

namespace assets {

// Session 97 (props2): true when at least 8 % of a BC2 / BC3 texture's top mip has alpha <= 32 (a cut-out, not a gloss mask).
inline bool AlphaCutsOut(const Texture& t) {
  const size_t blocks = size_t((std::max(t.width, 1u) + 3) / 4) * ((std::max(t.height, 1u) + 3) / 4);
  if (blocks == 0 || t.data.size() < blocks * 16) return true;  // (unknown: keep the old behaviour)
  size_t low = 0;
  for (size_t b = 0; b < blocks; ++b) {
    const uint8_t* q = &t.data[b * 16];
    if (t.format == TexFormat::kBC2) {
      for (int i = 0; i < 16; ++i) low += (((q[i / 2] >> (4 * (i & 1))) & 15) * 17) <= 32;
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
        low += v <= 32;
      }
    }
  }
  return double(low) >= 0.08 * double(blocks) * 16.0;
}


struct JboyOptions {
  bool arena = false;      // arenas: S1 is UV (not skin), S2 lightmap UV, no skeleton
  std::string part;        // "body", "piece 10000", "slot 12"...
  TexSet* tex = nullptr;   // where texture names resolve
  // Session 97: an arena piece with its own small skeleton (a crowd body): vertex bones are the piece's node indices and
  // the node records are returned here (file space), so the caller can pose it.
  std::vector<Bone>* own_nodes = nullptr;
  // Session 97 (stage movie leaves, keys 800-809 of bg05): their S1 stream is the constant pair (0,1) on every vertex and the
  // real texture UV is in S2 (read from the player's files), so the viewer drew each screen from one texel (a flat red patch).
  // With this flag uv0 comes from S2 and uv1 is 0.
  bool uv_in_s2 = false;
  // Session 98 (step 16): the source recorded on every mesh (Mesh::src_*): kind 1 arena, 2 wrestler, 3 crowd, 4 prop.
  int kind = 0;
  std::string pac;
  int key = 0;
  // Session 99 (scene objects): a prop's model slot 1..9 (asset_load.h LoadWrestlerPac prop_slots). Its yProp_* materials whose diffuse
  // alpha cuts out (AlphaCutsOut) are drawn alpha-blended: Fandango 2591's slot-5 "beamglow02 / 03" light beams drew as opaque dark
  // cones. [U] the game's blend state for prop materials is not decoded (judged by the picture; slot 0 pieces keep the old rule).
  bool prop_slot = false;
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
    for (uint32_t i = 0; i < nnode; ++i) {
      const uint8_t* n = r.b + node_tab + 80ull * i;
      const std::string name = CString(n, 16);
      const int occ = seen[name]++;
      auto it = by_name.find(name);
      if (it != by_name.end() && occ < int(it->second.size())) {
        node_map[i] = it->second[size_t(occ)];
        continue;
      }
      Bone bn;
      bn.name = name;
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
      by_name[name].push_back(node_map[i]);
      ++appended;
    }
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
      for (int k = 0; k < 16; ++k)
        if (std::fabs(corr[i][k] - Identity()[k]) > 1e-3f * (k >= 12 ? 100.f : 1.f)) use_corr = true;
    }
  }

  for (uint32_t mi = 0; mi < nmesh; ++mi) {
    const uint32_t rec = mesh_tab + mi * 0xB8;
    const uint32_t N = r.u32(rec);
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
    if (!tiles) {
      m.warnings.push_back("mesh streams do not tile exactly: mesh skipped");
      continue;
    }
    if (!r.ok(s0d, uint64_t(N) * 28) || !r.ok(s1, d1) || !r.ok(s2, d2) || !r.ok(s3, d3)) {
      m.warnings.push_back("mesh stream past the end of the body: mesh skipped");
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
    if (!idx_ok) continue;

    Mesh mesh;
    mesh.part = o.part;
    mesh.src_kind = o.kind, mesh.src_pac = o.pac, mesh.src_key = o.key;  // Session 98 (step 16)
    if (nnode > 0) mesh.node = CString(r.b + node_tab, 16);
    for (uint32_t k = 0; k < npal; ++k)
      if (pal[k] >= 1 && pal[k] <= nnode) mesh.palette += (mesh.palette.empty() ? "" : ";") + CString(r.b + node_tab + 80ull * (pal[k] - 1), 16);
    mesh.vertices.resize(N);
    uint32_t nan_count = 0, nonunit = 0, unused_slot = 0, bad_slot = 0, bad_node = 0, zero_w = 0, extra_node = 0;
    // Session 96 integration: arena models with a skeleton (the ring set 951-982, the crowd bodies) store their streams
    // like a wrestler (S1 = skin pairs, S2 = UV), not like the static stage (S1 = UV, S2 = lightmap UV): read with the
    // stage layout the ring drew as a black slab. Told apart by the first vertex's S1: a little-endian slot below 256
    // and a big-endian weight in 0..1 (unconfirmed rule; checked on the ring set of arena 5).
    bool skin_streams = false;
    if (o.arena && nnode > 1 && N > 0 && st1 >= 8 && st1 % 8 == 0) {
      uint32_t slot0;
      std::memcpy(&slot0, r.b + s1, 4);
      const float w0 = BeF(r.b + s1 + 4);
      skin_streams = slot0 < 256 && w0 >= 0.f && w0 <= 1.0001f;
    }
    // Session 97 (truss): arena meshes whose S1 UV pair is one constant value (0,1) on every vertex while S2 varies keep the real
    // texture UV in S2 (the same layout as the stage movie leaves). Arena 5's truss rings / pillars (slots 162, 208, 211, 216,
    // 217) read one solid texel from S1: the lattice's open cut-out (DXT5 alpha) never showed, they drew as solid bands.
    bool uv_s2 = o.uv_in_s2, s1_const_uv = false;  // (s1_const_uv: decided after the material is known)
    if (o.arena && !skin_streams && !uv_s2 && N > 1 && st1 == 8) {
      bool s1_const = true, s2_vary = false;
      for (uint32_t v = 1; v < N && s1_const; ++v) {
        if (std::memcmp(r.b + s1 + 8ull * v, r.b + s1, 8) != 0) s1_const = false;
        if (std::memcmp(r.b + s2 + 8ull * v, r.b + s2, 8) != 0) s2_vary = true;
      }
      s1_const_uv = s1_const && s2_vary;
    }
    for (uint32_t v = 0; v < N; ++v) {
      Vertex& vx = mesh.vertices[v];
      const uint8_t* p0 = r.b + s0d + 28ull * v;
      for (int k = 0; k < 3; ++k) vx.pos[k] = BeF(p0 + 4 * k), vx.normal[k] = BeF(p0 + 12 + 4 * k);
      // Session 96 integration: the colour word. Arenas: bytes A R G B (0xFF C0 C0 C0 = opaque grey; unconfirmed that
      // the first byte is alpha) -> stored R G B A for the viewer. Wrestlers: NOT a colour (the research notes: its last
      // byte picks a material index; multiplying by it turned Batista's skin teal and his blue trunk lettering red) ->
      // white.
      if (o.arena) {
        const uint8_t c4[4] = {p0[25], p0[26], p0[27], p0[24]};
        std::memcpy(&vx.color, c4, 4);
      } else {
        // Session 98 (wrestler fix 3): bytes 1-3 of the word are one grey value (ch161: 50 on the body skin, 128 / 254 on head and
        // face pieces, 255 on gear [C: byte histograms, all 50 meshes]); the skin vertex shaders pass the colour's third byte on as
        // the first lobe set's gain (vs_ch_skin_a.vert interpolator 9 -> wrestler_skin_entrance.frag in_reflect_gain). Byte 0 is
        // the material-index byte (0 / 8 / 16 / 24 / 40). Kept as RGB = that grey, A = 255 (the viewer reads the gain from R; it never
        // multiplies a wrestler's colour by it). [U: that the GPU vertex word equals the file's S0 word; bytes 1-3 being equal makes
        // the byte order moot.]
        const uint8_t g = p0[25];
        const uint8_t c4[4] = {g, g, g, 0xFF};
        std::memcpy(&vx.color, c4, 4);
      }
      if (!std::isfinite(vx.pos[0] + vx.pos[1] + vx.pos[2])) ++nan_count;
      const float nl = std::sqrt(vx.normal[0] * vx.normal[0] + vx.normal[1] * vx.normal[1] + vx.normal[2] * vx.normal[2]);
      if (!(std::fabs(nl - 1.f) < 0.05f)) ++nonunit;
      vx.tangent[0] = vx.tangent[1] = vx.tangent[2] = 0;
      if (st3 == 12)
        for (int k = 0; k < 3; ++k) vx.tangent[k] = BeF(r.b + s3 + 12ull * v + 4 * k);
      if (o.arena && !skin_streams) {
        for (int k = 0; k < 2; ++k)
          if (uv_s2) vx.uv0[k] = BeF(r.b + s2 + 8ull * v + 4 * k), vx.uv1[k] = 0;
          else vx.uv0[k] = BeF(r.b + s1 + uint64_t(st1) * v + 4 * k), vx.uv1[k] = BeF(r.b + s2 + 8ull * v + 4 * k);
        for (int k = 0; k < 4; ++k) vx.bones[k] = 0, vx.weights[k] = k == 0 ? 1.f : 0.f;
      } else {
        for (int k = 0; k < 2; ++k) vx.uv0[k] = BeF(r.b + s2 + 8ull * v + 4 * k), vx.uv1[k] = 0;
        int nb = 0;
        uint32_t pnode[4] = {0, 0, 0, 0};
        float sum = 0;
        for (int k = 0; k < 4; ++k) vx.bones[k] = 0, vx.weights[k] = 0;
        for (uint32_t pr = 0; pr < st1 / 8 && nb < 4; ++pr) {
          const uint8_t* q = r.b + s1 + uint64_t(st1) * v + 8ull * pr;
          uint32_t slot;
          std::memcpy(&slot, q, 4);  // LE
          const float w = BeF(q + 4);
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
    mat.shader = CString(r.b + rec + 0x7C, 16);
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
          int* slot = nullptr;
          // (Session 96: both name sets everywhere - the arena's skinned models may use either)
          if (pname == "g_mDiffusePrm" || pname == "sampDiffusePrm") slot = &mat.diffuse;
          else if (pname == "g_mNormalFxPrm" || pname == "sampNormalPrm") slot = &mat.normal;
          else if (pname == "g_mMaskPrm") slot = &mat.mask;
          else if (pname == "sampLightMapPrm" && !skin_streams) slot = &mat.lightmap;
          else if (pname == "sampSpecularPrm") slot = &mat.specular;
          else if (pname == "g_mNoiseTilePrm" && !o.arena) slot = &mat.detail;  // Session 98 (wrestler fix 3): the gear's tiled detail
          if (pname == "sampLightMapPrm" && !skin_streams) mat.has_lm_slot = true;  // (Session 97: the LM family)
          if (!slot || ti < 0) continue;
          if (uint32_t(ti) >= ntex) {
            m.warnings.push_back("sampler index beyond the texture-name table");
            continue;
          }
          if (!o.tex) continue;
          *slot = o.tex->Get(m, CString(r.b + tex_tab + 16ull * uint32_t(ti), 16));
          if (slot == &mat.diffuse) mat.diffuse_name = CString(r.b + tex_tab + 16ull * uint32_t(ti), 16);
        } else if (kind == 0x0D && r.ok(po + 20, 16)) {
          float* dst = pname == "g_f4MatAmbCol"    ? mat.ambient
                       : pname == "g_f4MatDifCol"  ? mat.diffuse_color
                       : pname == "g_fvLightLimit" ? mat.light_limit
                       : (pname == "g_f4SpecularCol" || pname == "g_f4SpecCol") ? mat.spec_col
                       : pname == "g_f4Ref_COL"    ? mat.ref_col
                       : pname == "g_f3SSSSinCol"  ? mat.sss_col
                       : pname == "g_f3SSSSinCol2" ? mat.sss_col2
                                                   : nullptr;
          if (dst)
            for (int c = 0; c < (dst == mat.ref_col ? 3 : 4); ++c) dst[c] = r.f32(po + 20 + 4ull * uint32_t(c));  // (Ref_COL's w is g_f4Ref_DOT's)
          if (dst == mat.sss_col) mat.has_sss = true;
        } else if (kind == 0x0A && r.ok(po + 20, 4)) {  // scalar floats (g_iSpecularPow is stored as a float too)
          if (pname == "g_fSpecularLev" || pname == "g_fSpecLev") mat.spec_lev = r.f32(po + 20);
          else if (pname == "g_iSpecularPow" || pname == "g_fSpecPow") mat.spec_pow = r.f32(po + 20);
          else if (pname == "g_fSpecLvBase") mat.spec_lv_base = r.f32(po + 20);  // Session 98 (skin fix): see asset_model.h Material
          else if (pname == "g_fSpecPwBase") mat.spec_pw_base = r.f32(po + 20);
          else if (pname == "g_f4Ref_DOT") mat.ref_col[3] = r.f32(po + 20);
          else if (pname == "g_fFairy") mat.fairy = r.f32(po + 20);
          else if (pname == "g_fFairyEdge") mat.fairy_edge = r.f32(po + 20);
          // Session 98 (wrestler fix 3): the detail sets (asset_model.h Material::noise_*); stored as scalars (kind 0x0A) in ch161
          else if (pname == "g_f4NoiseTBump") mat.noise_near[0] = r.f32(po + 20);
          else if (pname == "g_f4NoiseTDiff") mat.noise_near[1] = r.f32(po + 20);
          else if (pname == "g_f4NoiseTSpec") mat.noise_near[2] = r.f32(po + 20);
          else if (pname == "g_f4NoiseTBumpF") mat.noise_far[0] = r.f32(po + 20);
          else if (pname == "g_f4NoiseTDiffF") mat.noise_far[1] = r.f32(po + 20);
          else if (pname == "g_f4NoiseTSpecF") mat.noise_far[2] = r.f32(po + 20);
          else if (pname == "g_fNoiseTNear") mat.noise_dist[0] = r.f32(po + 20);
          else if (pname == "g_fNoiseTFar") mat.noise_dist[1] = r.f32(po + 20);
        }
      }
    }
    const std::string sl = Lower(mat.shader);
    if (sl.find("trans") != std::string::npos) mat.alpha_blend = true;
    if (o.arena && mat.diffuse >= 0 && !mat.alpha_blend) {
      const TexFormat f = m.textures[size_t(mat.diffuse)].format;  // first rule: alpha-carrying DXT -> cut-out (unconfirmed)
      // Session 97 (props2): only when the alpha really cuts shapes out: a texture whose alpha never goes (nearly) transparent carries a
      // gloss / specular mask there (ar_dm floor mats, ar_stair, ar_rop01: alpha 60-110 everywhere), and cutting at 0.5 made the floor
      // mats and stairs vanish [C: alpha histograms of the arena 5 textures; the cut-outs (fences, truss, chairs, haze cards) have 20-86 %
      // of their pixels at alpha <= 32, the masks 0-3 %].
      if (f == TexFormat::kBC3 || f == TexFormat::kBC2) mat.alpha_test = AlphaCutsOut(m.textures[size_t(mat.diffuse)]);
    }
    if (o.prop_slot && mat.diffuse >= 0 && !mat.alpha_blend && size_t(mat.diffuse) < m.textures.size()) {  // (Session 99, JboyOptions)
      const TexFormat f = m.textures[size_t(mat.diffuse)].format;
      if ((f == TexFormat::kBC3 || f == TexFormat::kBC2) && AlphaCutsOut(m.textures[size_t(mat.diffuse)])) mat.alpha_blend = true;
    }
    if (s1_const_uv && mat.lightmap < 0 && mat.diffuse >= 0)  // (no lightmap uses S2: it is the diffuse UV; Session 97 truss)
      for (Vertex& vx : mesh.vertices) vx.uv0[0] = vx.uv1[0], vx.uv0[1] = vx.uv1[1], vx.uv1[0] = vx.uv1[1] = 0;
    if (mat.diffuse < 0) {
      // Not an error: plain-coloured parts exist; counted so the census shows how common it is.
      m.warnings.push_back("material without a diffuse texture");
    }
    m.materials.push_back(mat);
    mesh.material = int(m.materials.size()) - 1;
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
