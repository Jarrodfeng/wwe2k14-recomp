// S111 GUESTVTX unit (docs\research\s111\GUESTVTX.md): the stock loader (jboy.h LoadJboy, which decodes the FILE record) and the created-superstar
// decoder (guest_model_draw.h DecodeVertices, which decodes the game's PACKED vertex) give the same vertex for one record, through the field rules
// they share (src\assets\vertex_decode.h): the colour as the game's fetch reads it (x = file byte 25, w = byte 24) and a 1-pair vertex bound whole
// to its own slot. Build and run: pwsh -NoProfile -File tools\assets\build_vertex_decode_unit.ps1
//   out\tools\vertex_decode_unit.exe                  selftest: a synthetic JBOY body through LoadJboy, and its records through the game's packer
//                                                     (sub_82BD4BE0, written out below) + DecodeVertices; both planted twins (the guest's host-order
//                                                     colour, draw plant 31; the stock's old rule, load_detail::VdecOldPlant) must make checks fail
//   out\tools\vertex_decode_unit.exe real --game DIR  read-only, the player's files: ch161 and ch100 attire 0 (1) through the production LoadWrestler
//                                                     with the old rule and with the new: every vertex byte-identical; (2) the body's records packed
//                                                     as the game packs them and decoded by the guest path: the same vertex as LoadJboy's
//   out\tools\vertex_decode_unit.exe scan DIR|PAC...  read-only report over every JBOY found (S111 GUESTVTX review): record +0x5C vs the S1 stride,
//                                                     the 1-pair [U] slots, the 2..4 pair weight rule, the arena colour rule (always exit 0)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "assets/asset_load.h"
#include "assets/vertex_decode.h"
#include "renderer_scene/guest_model_draw.h"

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

namespace {

namespace gd = rdr_scene::guestmodel;

uint32_t FBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
// float -> half, rounded to nearest (the packer's own rounding differs by at most one half ulp: the compare allows it).
uint16_t Half(float f) {
  const uint32_t u = FBits(f), ex = (u >> 23) & 0xFFu;
  const uint16_t s = uint16_t((u >> 16) & 0x8000u);
  uint32_t m = u & 0x7FFFFFu;
  const int e = int(ex) - 127 + 15;
  if (ex == 0xFFu) return uint16_t(s | 0x7C00u | (m ? 0x200u : 0u));
  if (e >= 31) return uint16_t(s | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return s;
    m |= 0x800000u;
    const int shift = 14 - e;
    return uint16_t(s | ((m + (1u << (shift - 1))) >> shift));
  }
  const uint32_t r = (uint32_t(e) << 10) | (m >> 13);
  return uint16_t(s | (r + ((m >> 12) & 1u)));
}
uint32_t Pack1011(const float n[3]) {   // sub_82C14BC0: x * 511 in the low 10 bits, y and z * 1023 in 11 bits each
  const int32_t x = int32_t(std::lround(n[0] * 511.f)), y = int32_t(std::lround(n[1] * 1023.f)), z = int32_t(std::lround(n[2] * 1023.f));
  return (uint32_t(x) & 0x3FFu) | ((uint32_t(y) & 0x7FFu) << 10) | ((uint32_t(z) & 0x7FFu) << 21);
}

// One mesh record of a JBOY body, read as LoadJboy reads it (S0 data 4 bytes after its offset, 28 bytes a vertex; S1 pairs; S2 uv; S3 tangent).
struct Rec {
  uint32_t N = 0, npal = 0, pal[20] = {}, st1 = 0, st3 = 0, pairs = 0;   // pairs = the record's skin-pair count (+0x5C), the game's (sub_82BDB460 +D58)
  uint64_t s0d = 0, s1 = 0, s2 = 0, s3 = 0;
  std::string effect;
};
bool ReadRec(const assets::Bytes& blob, uint32_t mi, Rec& r, uint32_t& nnode) {
  if (blob.size() < 0x48) return false;
  const uint8_t* body = blob.data() + 8;
  const uint64_t len = std::min<uint64_t>(assets::Be32(blob.data() + 4), blob.size() - 8);
  const auto u32 = [&](uint64_t off) { return off + 4 <= len ? assets::Be32(body + off) : 0u; };
  const uint32_t nmesh = u32(0x10), mesh_tab = u32(0x14);
  nnode = u32(0x18);
  if (mi >= nmesh) return false;
  const uint64_t rec = mesh_tab + uint64_t(mi) * 0xB8;
  r.N = u32(rec);
  if (r.N == 0) return false;
  r.npal = std::min<uint32_t>(u32(rec + 8), 20);
  for (uint32_t k = 0; k < 20; ++k) r.pal[k] = u32(rec + 0x0C + 4 * k);
  const uint32_t s0 = u32(rec + 0x68), s1 = u32(rec + 0x6C), s2 = u32(rec + 0x70), s3 = u32(rec + 0x74), ib = u32(rec + 0x9C);
  r.s0d = uint64_t(s0) + 4, r.s1 = s1, r.s2 = s2, r.s3 = s3;
  r.st1 = s2 > s1 ? (s2 - s1) / r.N : 0;
  r.st3 = ib > s3 ? (ib - s3) / r.N : 0;
  r.pairs = u32(rec + 0x5C);
  char nm[17] = {};
  if (rec + 0x7C + 16 <= len) std::memcpy(nm, body + rec + 0x7C, 16);
  r.effect = nm;
  return r.st1 >= 8 && r.st1 <= 32 && r.st1 % 8 == 0 && r.pairs >= 1 && r.pairs <= 4 && 8 * r.pairs <= r.st1 && r.s0d + 28ull * r.N <= len && r.s1 + uint64_t(r.st1) * r.N <= len && r.s2 + 8ull * r.N <= len &&
         r.s3 + uint64_t(r.st3) * r.N <= len;
}

// The flag word sub_82BDB460 builds for a part of a body with nodes and the tangent switch 2 (every wrestler body): the skin-pair table entry
// {0, 6, 8, 0xA, 0xC} for the pair count, | 0x1050 (nodes), | 0x100 (one UV set), | 0x10000 (tangent).
uint32_t FlagOf(uint32_t pairs) {
  static const uint32_t kSkin[5] = {0, 6, 8, 0xA, 0xC};
  return kSkin[pairs > 4 ? 0 : pairs] | 0x1050u | 0x100u | 0x10000u;
}
// The game's packer (sub_82BD4BE0) over one record: position + 1.0; the skin block (1 pair: one dword, +16..+18 = 255, +19 = the slot byte, the
// weight never read; 2..4 pairs: the first pairs - 1 weights, 0.0 for the rest, then the slot dword with s0 at +31 .. s3 at +28, absent 255);
// normal and tangent 10:11:11; the S0 colour word copied; half(u) at +0, half(v) at +2. Dwords in host order (the 8-in-32 swap applied), as the
// guest reader hands them on (guest_model_reader.h PartRead::vtx).
std::vector<uint32_t> PackRecord(const uint8_t* body, const Rec& r) {
  std::vector<uint32_t> d;
  const uint32_t pairs = r.pairs;   // the flag word's count (record +0x5C); the packer reads S1 at 8 bytes a pair of that count (+1CC: vertex << 3 for 1 pair .. << 5 for 4)
  for (uint32_t v = 0; v < r.N; ++v) {
    const uint8_t* p0 = body + r.s0d + 28ull * v;
    const uint8_t* q = body + r.s1 + 8ull * pairs * v;
    for (int k = 0; k < 3; ++k) d.push_back(FBits(assets::BeF(p0 + 4 * k)));
    d.push_back(FBits(1.0f));
    uint8_t s[4] = {255, 255, 255, 255};
    for (uint32_t k = 0; k < pairs && k < 4; ++k) s[k] = q[8 * k];   // the packer's byte load at pair + 0: the LE slot's low byte
    if (pairs >= 2) {
      for (uint32_t k = 0; k < 3; ++k) d.push_back(FBits(k + 1 < pairs ? assets::BeF(q + 8 * k + 4) : 0.f));
      d.push_back(uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24);
    } else if (pairs == 1) {
      d.push_back(0xFFFFFF00u | s[0]);
    }
    float n[3], t[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) n[k] = assets::BeF(p0 + 12 + 4 * k);
    if (r.st3 == 12)
      for (int k = 0; k < 3; ++k) t[k] = assets::BeF(body + r.s3 + 12ull * v + 4 * k);
    d.push_back(Pack1011(n));
    d.push_back(Pack1011(t));
    d.push_back(assets::Be32(p0 + 24));
    d.push_back(uint32_t(Half(assets::BeF(body + r.s2 + 8ull * v))) << 16 | Half(assets::BeF(body + r.s2 + 8ull * v + 4)));
  }
  return d;
}

struct Diff {
  size_t meshes = 0, verts = 0, refused = 0, pos = 0, nrm = 0, tan = 0, uv = 0, col = 0, skin = 0;
  size_t Bad() const { return refused + pos + nrm + tan + uv + col + skin; }
};
std::map<uint16_t, float> Influence(const assets::Vertex& v) {
  std::map<uint16_t, float> m;
  for (int k = 0; k < 4; ++k)
    if (v.weights[k] > 1e-4f) m[v.bones[k]] += v.weights[k];
  return m;
}
bool SameInfluence(const assets::Vertex& a, const assets::Vertex& b) {
  const std::map<uint16_t, float> x = Influence(a), y = Influence(b);
  if (x.size() != y.size()) return false;
  for (const auto& [node, w] : x) {
    const auto it = y.find(node);
    if (it == y.end() || std::fabs(it->second - w) > 2e-3f) return false;
  }
  return true;
}
bool Near1011(const float a[3], const float b[3]) {
  for (int k = 0; k < 3; ++k)
    if (!(std::fabs(a[k]) <= 1.f)) return true;   // outside the 10:11:11 range: not compared (the packed word cannot hold it)
  return std::fabs(a[0] - b[0]) <= 1.f / 511.f + 1e-6f && std::fabs(a[1] - b[1]) <= 1.f / 1023.f + 1e-6f && std::fabs(a[2] - b[2]) <= 1.f / 1023.f + 1e-6f;
}
// s = the stock vertex (LoadJboy), g = the guest decode of the packed record. A yCh_skin vertex's colour must be equal whole; any other wrestler
// mesh keeps the stock's own A = 255 / grey (VERTEXSEAM's margin, read by no yCh effect), so only R (file byte 25, both paths) is compared there.
void Compare(const assets::Vertex& s, const assets::Vertex& g, bool skin_fx, bool has_tan, Diff& df) {
  ++df.verts;
  if (std::memcmp(s.pos, g.pos, sizeof s.pos) != 0) ++df.pos;
  if (!Near1011(s.normal, g.normal)) ++df.nrm;
  if (has_tan && !Near1011(s.tangent, g.tangent)) ++df.tan;
  bool uv_ok = true;
  for (int k = 0; k < 2; ++k) uv_ok = uv_ok && std::fabs(s.uv0[k] - g.uv0[k]) <= 1e-3f * std::max(1.f, std::fabs(s.uv0[k]));
  if (!uv_ok) ++df.uv;
  if (skin_fx ? s.color != g.color : (s.color & 0xFFu) != (g.color & 0xFFu)) ++df.col;
  if (!SameInfluence(s, g)) ++df.skin;
}

// Both paths over one JBOY body: LoadJboy's meshes against the guest decode of the same records, packed as the game packs them.
Diff BothPaths(const assets::Bytes& blob, std::string& err) {
  Diff df;
  assets::Model m;
  assets::JboyOptions o;
  o.part = "body", o.kind = 2, o.key = 0;
  if (!assets::LoadJboy(blob, o, m, err)) return ++df.refused, df;
  for (const assets::Mesh& me : m.meshes) {
    Rec r;
    uint32_t nnode = 0;
    if (me.src_record < 0 || !ReadRec(blob, uint32_t(me.src_record), r, nnode) || me.vertices.size() != r.N) {
      ++df.refused;
      continue;
    }
    const uint32_t F = FlagOf(r.pairs);
    const std::vector<uint32_t> d = PackRecord(blob.data() + 8, r);
    uint16_t nodes[20];
    for (uint32_t k = 0; k < r.npal; ++k) nodes[k] = r.pal[k] >= 1 && r.pal[k] <= nnode ? uint16_t(r.pal[k] - 1) : uint16_t(0xFFFF);   // the reader's palette: [desc+12+4k] - 1
    std::vector<assets::Vertex> g;
    gd::DecodeStats ds;
    if (!gd::DecodeVertices(reinterpret_cast<const uint8_t*>(d.data()), r.N, uint32_t(d.size() / r.N * 4), F, nodes, r.npal, g, ds)) {
      ++df.refused;
      continue;
    }
    ++df.meshes;
    const bool skin_fx = r.effect.rfind("yCh_skin", 0) == 0;
    for (uint32_t v = 0; v < r.N; ++v) Compare(me.vertices[v], g[v], skin_fx, r.st3 == 12, df);
  }
  return df;
}
std::string Say(const Diff& df) {
  char b[256];
  std::snprintf(b, sizeof b, "meshes %zu, vertices %zu; refused %zu, off: position %zu normal %zu tangent %zu uv %zu colour %zu skin %zu", df.meshes, df.verts, df.refused, df.pos, df.nrm, df.tan,
                df.uv, df.col, df.skin);
  return b;
}

void PutBe32(assets::Bytes& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + size_t(i)] = uint8_t(v >> (24 - 8 * i));
}
void PutBeF(assets::Bytes& b, size_t off, float f) { PutBe32(b, off, FBits(f)); }

// A synthetic JBOY body: 3 nodes (root, n1, n2), ONE yCh_skin mesh, palette {2, 3, 1} (slot 0 -> node 1, slot 1 -> node 2, slot 2 -> node 0), every
// vertex's colour word 0x08323C46 (byte 24 = 8: an ID / wrinkle slot; bytes 25, 26, 27 = 0x32, 0x3C, 0x46: all different). S1 holds st1 / 8 pairs a
// vertex (slot / w: the pairs in file order); the record's skin-pair count (+0x5C) is rec_pairs (1 = the 1-pair body; another value than st1 / 8 = a
// record whose count disagrees with its S1 stride).
assets::Bytes MakeJboy(const std::vector<uint32_t>& slot, const std::vector<float>& w, uint32_t st1 = 8, uint32_t rec_pairs = 1) {
  const uint32_t per = st1 / 8, N = uint32_t(slot.size()) / per;
  const uint32_t mesh_tab = 0x40, node_tab = mesh_tab + 0xB8, s0 = node_tab + 3 * 80, s0d = s0 + 4, s1 = s0d + 28 * N, s2 = s1 + st1 * N, s3 = s2 + 8 * N, ib = s3 + 12 * N,
                 idx = ib + 12, body_len = idx + 2 * N + 2;
  assets::Bytes b(8 + body_len, 0);
  std::memcpy(b.data(), "JBOY", 4);
  PutBe32(b, 4, body_len);
  const size_t B = 8;
  PutBe32(b, B + 0x10, 1), PutBe32(b, B + 0x14, mesh_tab), PutBe32(b, B + 0x18, 3), PutBe32(b, B + 0x1C, 0), PutBe32(b, B + 0x20, node_tab), PutBe32(b, B + 0x24, node_tab);
  const char* names[3] = {"root", "n1", "n2"};
  for (uint32_t i = 0; i < 3; ++i) {
    const size_t o = B + node_tab + 80 * i;
    std::memcpy(&b[o], names[i], std::strlen(names[i]));
    PutBeF(b, o + 16, 10.f * float(i));
    PutBe32(b, o + 48, i == 0 ? 0xFFFFFFFFu : i - 1);
  }
  const size_t rec = B + mesh_tab;
  PutBe32(b, rec, N), PutBe32(b, rec + 8, 3), PutBe32(b, rec + 0x0C, 2), PutBe32(b, rec + 0x10, 3), PutBe32(b, rec + 0x14, 1);
  PutBe32(b, rec + 0x5C, rec_pairs);
  PutBe32(b, rec + 0x68, s0), PutBe32(b, rec + 0x6C, s1), PutBe32(b, rec + 0x70, s2), PutBe32(b, rec + 0x74, s3), PutBe32(b, rec + 0x9C, ib);
  std::memcpy(&b[rec + 0x7C], "yCh_skin", 8);
  for (uint32_t v = 0; v < N; ++v) {
    const size_t p0 = B + s0d + 28 * v;
    PutBeF(b, p0, float(v)), PutBeF(b, p0 + 4, -1.5f), PutBeF(b, p0 + 8, 2.25f);
    PutBeF(b, p0 + 12, 0.6f), PutBeF(b, p0 + 16, 0.f), PutBeF(b, p0 + 20, 0.8f);
    PutBe32(b, p0 + 24, 0x08323C46u);
    for (uint32_t k = 0; k < per; ++k) {
      const size_t q = B + s1 + st1 * v + 8 * k;
      std::memcpy(&b[q], &slot[v * per + k], 4);   // LE slot
      PutBeF(b, q + 4, w[v * per + k]);
    }
    PutBeF(b, B + s2 + 8 * v, 0.25f * float(v)), PutBeF(b, B + s2 + 8 * v + 4, 0.5f);
    PutBeF(b, B + s3 + 12 * v, 1.f), PutBeF(b, B + s3 + 12 * v + 4, 0.f), PutBeF(b, B + s3 + 12 * v + 8, 0.f);
  }
  b[B + ib] = 6;   // LE 6: triangle strip
  PutBe32(b, B + ib + 4, N), PutBe32(b, B + ib + 8, idx);
  for (uint32_t v = 0; v < N; ++v) b[B + idx + 2 * v] = uint8_t(v >> 8), b[B + idx + 2 * v + 1] = uint8_t(v);
  return b;
}

// The stock decode of the synthetic body's weight-0 vertex (vertex 1: slot 2, file weight 0): bound to its own slot's node 0 with weight 1.
bool WeightZeroOwnSlot(const assets::Bytes& blob) {
  assets::Model m;
  assets::JboyOptions o;
  o.part = "body", o.kind = 2, o.key = 0;
  std::string err;
  if (!assets::LoadJboy(blob, o, m, err) || m.meshes.empty() || m.meshes[0].vertices.size() < 2) return false;
  const assets::Vertex& v = m.meshes[0].vertices[1];
  return v.bones[0] == 0 && v.weights[0] == 1.f && v.weights[1] == 0.f;
}

int Selftest() {
  Check(assets::vdecode::GameColour(0x08323C46u) == 0x08463C32u, "GameColour: R G B A = file bytes 25, 26, 27, 24 (word 0x08323C46 -> bytes 32 3C 46 08)");
  Check(assets::vdecode::GameColour(0xFFFFFFFFu) == 0xFFFFFFFFu, "GameColour: the no-colour white stays white");
  Check(assets::vdecode::GamePairWeight(1, 0.f) == 1.f && assets::vdecode::GamePairWeight(1, 0.5f) == 1.f && assets::vdecode::GamePairWeight(2, 0.25f) == 0.25f,
        "GamePairWeight: a 1-pair vertex weighs 1.0 whatever the file says; 2..4 pairs keep the file's weight");
  const assets::Bytes blob = MakeJboy({1, 2, 0}, {1.f, 0.f, 0.5f});
  std::string err;
  const Diff now = BothPaths(blob, err);
  Check(now.meshes == 1 && now.verts == 3 && now.Bad() == 0, "synthetic 1-pair body: LoadJboy and the packed guest decode agree on every field (" + Say(now) + ")" + (err.empty() ? "" : ": " + err));
  Check(WeightZeroOwnSlot(blob), "stock: the 1-pair vertex with file weight 0 binds to its own slot's node (0), weight 1");
  // The skin-pair count is the record's +0x5C (the game's flag word, sub_82BDB460 +D58), not the S1 stride: a record that says 1 pair over an S1 laid
  // out 16 bytes a vertex. The game's packer reads vertex v's one pair at S1 + 8 v (sub_82BD4BE0 +1CC), so v binds slot (0, 1, 2, 0) whole.
  const assets::Bytes odd = MakeJboy({0, 1, 2, 0, 1, 2, 0, 2}, {0.25f, 0.75f, 0.5f, 0.5f, 0.25f, 0.75f, 0.5f, 0.5f}, 16, 1);
  const Diff by_rec = BothPaths(odd, err);
  Check(by_rec.meshes == 1 && by_rec.verts == 4 && by_rec.Bad() == 0, "record +0x5C = 1 over a 16-byte S1: LoadJboy reads the pairs the game's packer reads (" + Say(by_rec) + ")" + (err.empty() ? "" : ": " + err));
  // Planted twin 1: the guest decoder's old host-order colour dword (draw plant 31): x = file byte 27.
  gd::g_draw_plant = 31;
  const Diff g_old = BothPaths(blob, err);
  gd::g_draw_plant = 0;
  Check(g_old.col == 3, "planted twin (guest host-order colour, draw plant 31): every vertex's colour differs from the stock one (" + Say(g_old) + ")");
  // Planted twin 2: the stock loader's old rule (VdecOldPlant): the weight-0 1-pair vertex falls to palette[0] (node 1); the skin rgb is byte 25 three times.
  assets::load_detail::VdecOldPlant() = true;
  const Diff s_old = BothPaths(blob, err);
  const bool own_old = WeightZeroOwnSlot(blob);
  const Diff odd_old = BothPaths(odd, err);
  assets::load_detail::VdecOldPlant() = false;
  Check(!own_old && s_old.skin == 1, "planted twin (stock old rule): the weight-0 vertex goes to palette[0], the one skin difference (" + Say(s_old) + ")");
  Check(s_old.col == 3, "planted twin (stock old rule): the skin rgb grey (byte 25 three times) differs from the game's order on every vertex");
  Check(odd_old.skin > 0, "planted twin (stock old rule): the pair count from the S1 stride / 8 blends two pairs where the game binds one (" + Say(odd_old) + ")");
  std::printf("%s (%d failed)\n", g_fail ? "vertex_decode_unit FAILED" : "vertex_decode_unit passed", g_fail);
  return g_fail ? 1 : 0;
}

bool SameBytes(const assets::Model& a, const assets::Model& b, size_t& verts, size_t& differ) {
  if (a.meshes.size() != b.meshes.size()) return false;
  for (size_t i = 0; i < a.meshes.size(); ++i) {
    const std::vector<assets::Vertex>& x = a.meshes[i].vertices;
    const std::vector<assets::Vertex>& y = b.meshes[i].vertices;
    if (x.size() != y.size()) return false;
    for (size_t v = 0; v < x.size(); ++v) {
      ++verts;
      const assets::Vertex &p = x[v], &q = y[v];
      const bool same = std::memcmp(p.pos, q.pos, sizeof p.pos) == 0 && std::memcmp(p.normal, q.normal, sizeof p.normal) == 0 && std::memcmp(p.uv0, q.uv0, sizeof p.uv0) == 0 &&
                        std::memcmp(p.uv1, q.uv1, sizeof p.uv1) == 0 && std::memcmp(p.tangent, q.tangent, sizeof p.tangent) == 0 && p.color == q.color &&
                        std::memcmp(p.bones, q.bones, sizeof p.bones) == 0 && std::memcmp(p.weights, q.weights, sizeof p.weights) == 0;
      if (!same) ++differ;
    }
  }
  return true;
}

int Real(const char* game) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  for (const int id : {161, 100}) {
    // (1) the production loader with the old rule and with the new: every vertex byte-identical on this pac.
    assets::Model now, old;
    std::string e1, e2;
    assets::load_detail::VdecOldPlant() = true;
    const bool ok_old = assets::LoadWrestler(files, id, 0, old, e1);
    assets::load_detail::VdecOldPlant() = false;
    const bool ok_now = assets::LoadWrestler(files, id, 0, now, e2);
    size_t verts = 0, differ = 0;
    const bool shape = ok_old && ok_now && SameBytes(old, now, verts, differ);
    std::printf("      ch%d attire 0: %zu meshes, %zu vertices, %zu differ old vs new\n", id, now.meshes.size(), verts, differ);
    Check(shape && verts > 0 && differ == 0, "real ch" + std::to_string(id) + ": LoadWrestler's vertices are byte-identical before and after (old rule planted vs the new)" + (e1.empty() ? "" : ": " + e1));
    // (2) the body (attire 0, entry 0) through both decoders.
    std::string err;
    const assets::pac::Pac* p = assets::load_detail::OpenCached(files, assets::load_detail::WrestlerRel(id), err);
    const assets::pac::Ent* e0 = nullptr;
    if (p)
      for (const assets::pac::Pach& c : p->pachs)
        if ((e0 = assets::pac::Find(c, 0)) != nullptr) break;
    if (!e0) {
      Check(false, "real ch" + std::to_string(id) + ": attire 0's body entry opens: " + err);
      continue;
    }
    const assets::Bytes blob = assets::pac::Decode(p->bytes.data() + e0->off, e0->size);
    const Diff df = BothPaths(blob, err);
    std::printf("      ch%d body: %s\n", id, Say(df).c_str());
    Check(df.meshes > 0 && df.Bad() == 0, "real ch" + std::to_string(id) + " body: every record packed as the game packs it decodes on the guest path to LoadJboy's vertex" + (err.empty() ? "" : ": " + err));
  }
  std::printf("%s (%d failed)\n", g_fail ? "vertex_decode_unit real FAILED" : "vertex_decode_unit real passed", g_fail);
  return g_fail ? 1 : 0;
}

// ---- scan (S111 GUESTVTX review): every JBOY under the given folders / pac files, read-only ----------------------------------------------------
// Per mesh record (the standard 0xB8 layout LoadJboy reads, streams that tile as LoadJboy requires): the game's skin-pair count (record +0x5C,
// sub_82BDB460 +D58) against the S1 stride / 8. Per vertex of a skinned body (nodes > 0) whose two counts agree: a 1-pair slot byte outside the
// palette or on a palette entry with no node (the note's [U]), a 1-pair file weight that is not 1 / not above 0, a slot word above 255 (the
// game reads its low byte only), and a 2..4 pair vertex whose stock influence (present pairs renormalised, jboy.h) differs from the game's
// (the first pairs - 1 stored weights, the last 1 - their sum: sub_82BD4BE0 + the shader). Every vertex: the arena colour rule
// (vdecode::GameColour of the BE word) against the old inline bytes {25, 26, 27, 24}; under a `bg` folder also through LoadJboy's arena branch.
struct ScanTally {
  size_t pacs = 0, models = 0, meshes = 0, untiled = 0, p5c_differ = 0, p5c_differ_nodes = 0, verts = 0;
  size_t one_meshes = 0, one_verts = 0, one_slot255 = 0, one_out_of_pal = 0, one_bad_node = 0, one_w_not1 = 0, one_w_le0 = 0, slot_hi = 0;
  size_t multi_verts = 0, multi_differ = 0, multi_game_outside = 0;
  size_t colour_verts = 0, colour_differ = 0, arena_loaded_verts = 0, arena_loaded_differ = 0;
};
std::map<std::string, ScanTally> g_scan;
std::vector<std::string> g_scan_examples;
void ScanExample(const std::string& s) {
  if (g_scan_examples.size() < 40) g_scan_examples.push_back(s);
}

void ScanJboy(const assets::Bytes& blob, const std::string& where, ScanTally& t, bool arena_folder) {
  if (blob.size() < 0x48 || std::memcmp(blob.data(), "JBOY", 4) != 0) return;
  const uint32_t body_len = assets::Be32(blob.data() + 4);
  if (body_len < 0x40 || size_t(body_len) + 8 > blob.size()) return;
  const uint8_t* b = blob.data() + 8;
  const auto u32 = [&](uint64_t off) { return off + 4 <= body_len ? assets::Be32(b + off) : 0u; };
  const uint32_t nmesh = u32(0x10), mesh_tab = u32(0x14), nnode = u32(0x18);
  if (nmesh > 512 || uint64_t(mesh_tab) + uint64_t(nmesh) * 0xB8 > body_len) return;
  ++t.models;
  for (uint32_t mi = 0; mi < nmesh; ++mi) {
    const uint64_t rec = mesh_tab + uint64_t(mi) * 0xB8;
    const uint32_t N = u32(rec);
    if (N == 0 || N > 400000) continue;
    const uint32_t npal = std::min<uint32_t>(u32(rec + 8), 20), P = u32(rec + 0x5C);
    uint32_t pal[20];
    for (uint32_t k = 0; k < 20; ++k) pal[k] = u32(rec + 0x0C + 4 * k);
    const uint32_t s0 = u32(rec + 0x68), s1 = u32(rec + 0x6C), s2 = u32(rec + 0x70), s3 = u32(rec + 0x74), ib = u32(rec + 0x9C);
    const uint64_t s0d = uint64_t(s0) + 4, d1 = s2 > s1 ? s2 - s1 : 0, d2 = s3 > s2 ? s3 - s2 : 0, d3 = ib > s3 ? ib - s3 : 0;
    const uint32_t st1 = uint32_t(d1 / N), st2 = uint32_t(d2 / N), st3 = uint32_t(d3 / N);
    const bool tiles = s0d + uint64_t(N) * 28 == s1 && !(d1 % N) && !(d2 % N) && !(d3 % N) && (st1 == 8 || st1 == 16 || st1 == 24 || st1 == 32) && st2 == 8 &&
                       (st3 == 0 || st3 == 12) && s0d + 28ull * N <= body_len && uint64_t(s1) + d1 <= body_len;
    if (!tiles) {
      ++t.untiled;
      continue;
    }
    ++t.meshes;
    char eff[17] = {};
    if (rec + 0x7C + 16 <= body_len) std::memcpy(eff, b + rec + 0x7C, 16);
    if (P * 8 != st1) {
      ++t.p5c_differ;
      if (nnode > 0) ++t.p5c_differ_nodes;
      ScanExample(where + " mesh " + std::to_string(mi) + " (" + eff + ", nodes " + std::to_string(nnode) + "): +0x5C = " + std::to_string(P) + ", S1 stride / 8 = " + std::to_string(st1 / 8));
    }
    for (uint32_t v = 0; v < N; ++v) {
      const uint8_t* p0 = b + s0d + 28ull * v;
      uint32_t old_c;
      const uint8_t c4[4] = {p0[25], p0[26], p0[27], p0[24]};
      std::memcpy(&old_c, c4, 4);
      ++t.colour_verts;
      if (assets::vdecode::GameColour(assets::Be32(p0 + 24)) != old_c) ++t.colour_differ;
    }
    t.verts += N;
    if (nnode == 0 || P * 8 != st1) continue;
    const auto node_ok = [&](uint32_t slot) { return slot < npal && pal[slot] >= 1 && pal[slot] <= nnode; };
    if (P == 1) {
      ++t.one_meshes;
      for (uint32_t v = 0; v < N; ++v) {
        const uint8_t* q = b + s1 + 8ull * v;
        uint32_t slot;
        std::memcpy(&slot, q, 4);
        const float w = assets::BeF(q + 4);
        ++t.one_verts;
        if (slot > 255) ++t.slot_hi;
        if (q[0] == 255) ++t.one_slot255;
        else if (q[0] >= npal) ++t.one_out_of_pal, ScanExample(where + " mesh " + std::to_string(mi) + " v " + std::to_string(v) + ": 1-pair slot " + std::to_string(q[0]) + " >= palette " + std::to_string(npal));
        else if (!node_ok(q[0])) ++t.one_bad_node;
        if (!(w > 0.f)) ++t.one_w_le0;
        else if (std::fabs(w - 1.f) > 1e-4f) ++t.one_w_not1;
      }
      continue;
    }
    if (P > 4) continue;
    for (uint32_t v = 0; v < N; ++v) {
      const uint8_t* q = b + s1 + uint64_t(st1) * v;
      ++t.multi_verts;
      std::map<uint32_t, float> game, ours;
      float gsum = 0;
      bool outside = false;
      for (uint32_t k = 0; k < P; ++k) {   // the game: slot byte at pair + 0; the stored weights of all but the last pair, the last takes 1 - their sum
        const uint32_t sb = q[8 * k];
        const float w = k + 1 < P ? assets::BeF(q + 8 * k + 4) : 1.f - gsum;
        if (k + 1 < P) gsum += w;
        if (std::fabs(w) <= 1e-4f) continue;
        if (!node_ok(sb)) {
          outside = true;
          continue;
        }
        game[pal[sb] - 1] += w;
      }
      float sum = 0;
      for (uint32_t k = 0; k < P; ++k) {   // ours (jboy.h LoadJboy): drop 255 / outside / no node / weight <= 0, renormalise by the sum
        uint32_t slot;
        std::memcpy(&slot, q + 8 * k, 4);
        const float w = assets::BeF(q + 8 * k + 4);
        if (slot > 255) ++t.slot_hi;
        if (slot == 255 || !node_ok(slot) || !(w > 0.f)) continue;
        ours[pal[slot] - 1] += w, sum += w;
      }
      if (ours.empty() || !(sum > 0.f)) ours = {{npal && pal[0] >= 1 && pal[0] <= nnode ? pal[0] - 1 : 0u, 1.f}};
      else if (std::fabs(sum - 1.f) > 1e-4f)
        for (auto& [n, w] : ours) w /= sum;
      if (outside) ++t.multi_game_outside;
      bool same = game.size() == ours.size();
      for (const auto& [n, w] : game) {
        const auto it = ours.find(n);
        same = same && it != ours.end() && std::fabs(it->second - w) <= 2e-3f;
      }
      if (!same) {
        ++t.multi_differ;
        char ex[160];
        std::snprintf(ex, sizeof ex, " mesh %u v %u (%s, %u pairs): game and stock influence differ (stored weight sum %.4f)", mi, v, eff, P, gsum);
        ScanExample(where + ex);
      }
    }
  }
  if (!arena_folder) return;
  assets::Model m;
  assets::JboyOptions o;
  o.part = "scan", o.arena = true;
  std::string err;
  if (!assets::LoadJboy(blob, o, m, err)) return;
  for (const assets::Mesh& me : m.meshes) {
    Rec r;
    uint32_t nn = 0;
    if (me.src_record < 0 || !ReadRec(blob, uint32_t(me.src_record), r, nn) || me.vertices.size() != r.N) continue;
    for (uint32_t v = 0; v < r.N; ++v) {
      const uint8_t* p0 = blob.data() + 8 + r.s0d + 28ull * v;
      const uint8_t c4[4] = {p0[25], p0[26], p0[27], p0[24]};
      uint32_t old_c;
      std::memcpy(&old_c, c4, 4);
      ++t.arena_loaded_verts;
      if (me.vertices[v].color != old_c) ++t.arena_loaded_differ;
    }
  }
}

void ScanPach(const assets::Bytes& buf, const assets::pac::Pach& p, int depth, const std::string& where, ScanTally& t, bool arena_folder) {
  for (const assets::pac::Ent& e : p.ents) {
    const assets::Bytes d = assets::pac::Decode(buf.data() + e.off, e.size);
    const std::string at = where + "/" + std::to_string(e.id);
    if (d.size() >= 8 && std::memcmp(d.data(), "PACH", 4) == 0 && depth < 6) {
      assets::pac::Pach np;
      if (assets::pac::ParsePach(d, 0, d.size(), np)) ScanPach(d, np, depth + 1, at, t, arena_folder);
    } else if (d.size() >= 4 && std::memcmp(d.data(), "JBOY", 4) == 0) {
      ScanJboy(d, at, t, arena_folder);
    }
  }
}

int Scan(const std::vector<std::string>& roots) {
  std::vector<std::filesystem::path> files;
  for (const std::string& r : roots) {
    const std::filesystem::path root(r);
    if (std::filesystem::is_regular_file(root)) files.push_back(root);
    else if (std::filesystem::is_directory(root))
      for (const auto& de : std::filesystem::recursive_directory_iterator(root))
        if (de.is_regular_file() && de.path().extension() == ".pac") files.push_back(de.path());
  }
  std::sort(files.begin(), files.end());
  for (const std::filesystem::path& f : files) {
    const std::string fam = f.parent_path().filename().string();
    ScanTally& t = g_scan[fam];
    std::ifstream in(f, std::ios::binary);
    assets::Bytes raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assets::pac::Pac pac;
    std::string err;
    if (!assets::pac::OpenBytes(std::move(raw), f.string(), pac, err)) {
      std::printf("      skip %s: %s\n", f.string().c_str(), err.c_str());
      continue;
    }
    ++t.pacs;
    for (size_t k = 0; k < pac.pachs.size(); ++k) ScanPach(pac.bytes, pac.pachs[k], 0, f.filename().string() + "/p" + std::to_string(k), t, fam == "bg");
  }
  for (const auto& [fam, t] : g_scan)
    std::printf("%-6s pacs %zu models %zu meshes %zu (untiled %zu) vertices %zu | +0x5C != S1/8: %zu (%zu with nodes) | 1-pair meshes %zu vertices %zu: slot 255 %zu, "
                "slot outside the palette %zu, palette entry without a node %zu, file weight <= 0 %zu, weight != 1 %zu | slot word > 255 %zu | 2..4-pair vertices %zu: "
                "game vs stock influence differ %zu, game weight on a slot outside / without a node %zu | colour rule vs old bytes differ %zu of %zu; arena branch differ %zu of %zu\n",
                fam.c_str(), t.pacs, t.models, t.meshes, t.untiled, t.verts, t.p5c_differ, t.p5c_differ_nodes, t.one_meshes, t.one_verts, t.one_slot255, t.one_out_of_pal,
                t.one_bad_node, t.one_w_le0, t.one_w_not1, t.slot_hi, t.multi_verts, t.multi_differ, t.multi_game_outside, t.colour_differ, t.colour_verts,
                t.arena_loaded_differ, t.arena_loaded_verts);
  for (const std::string& e : g_scan_examples) std::printf("      e.g. %s\n", e.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "scan") return Scan(std::vector<std::string>(argv + 2, argv + argc));
  if (argc >= 2 && std::string(argv[1]) == "real") {
    const char* game = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--game") game = argv[i + 1];
    if (!game) {
      std::printf("usage: vertex_decode_unit real --game DIR\n");
      return 2;
    }
    return Real(game);
  }
  return Selftest();
}
