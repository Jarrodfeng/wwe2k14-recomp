// Offline unit for the guest model reader / planner / texture resolver / rows / consumer (Session 103, lanes CW-2 and CW-2b). No game, no GPU, 0 game runs. Forged guest memory only (hard rule 1: no game bytes).
//   build: clang++ -std=c++20 -O1 -DNOMINMAX tools\scene\guest_model_unit.cpp src\renderer_scene\record_io.cpp ...   (tools\scene\guest_model_unit.py does it)
//   usage: guest_model_unit.exe [--plant N]    N = 1..13 reader / planner plants (guest_model_reader.h), 21..27 = decoder plants (guest_model_draw.h: N - 20). Exit 0 = every check passed; with a plant: exit 1 = caught.
// The vertex forges are built from the game's PACKER rules (sub_82BD4BE0, CW2_review bug 5: the byte stores of the skin block, the 10:11:11 words, the half2 uv dwords), NOT from the decoder's own layout table.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/guest_model_draw.h"
#include "../../src/renderer_scene/guest_model_absent.h"   // S110 STOREABSENT
#include "../../src/renderer_scene/logo_bool.h"        // S105 A5c
#include "../../src/renderer_scene/piece_row_rule.h"   // S105 A5b
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/guest_model_emit.h"
#include "../../src/scene_live/guest_model_reader.h"
#include "../../src/scene_live/guest_model_store.h"
#include "../../src/scene_live/guest_model_tex.h"

using namespace rdr_scene;
namespace gm = rdr_scene::live::guest_model;
namespace ul = rdr_scene::live::unlocklog;

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
  else std::printf("ok   %s\n", what.c_str());
}
static bool Near(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }

// Forged guest memory: a flat big-endian image, two windows (the model heap and the page that holds the texture registry's global).
struct Mem {
  std::vector<uint8_t> b = std::vector<uint8_t>(12u << 20, 0), hi = std::vector<uint8_t>(0x100000, 0);
  static constexpr uint32_t kBase = 0x40000000u, kHi = 0x83900000u;
  uint32_t next = kBase + 0x1000;
  uint8_t* at(uint32_t a) { return a >= kHi ? hi.data() + (a - kHi) : b.data() + (a - kBase); }
  const uint8_t* at(uint32_t a) const { return a >= kHi ? hi.data() + (a - kHi) : b.data() + (a - kBase); }
  bool in(uint32_t a, uint32_t n) const {
    if (a >= kHi) return uint64_t(a - kHi) + n <= hi.size();
    return a >= kBase && uint64_t(a - kBase) + n <= b.size();
  }
  bool u32(uint32_t a, uint32_t& v) const { if (!in(a, 4)) return false; const uint8_t* p = at(a); v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; return true; }
  bool bytes(uint32_t a, uint32_t n, uint8_t* d) const { if (!in(a, n)) return false; std::memcpy(d, at(a), n); return true; }
  void put32(uint32_t a, uint32_t v) { uint8_t* p = at(a); p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
  void put16(uint32_t a, uint32_t v) { uint8_t* p = at(a); p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
  uint32_t alloc(uint32_t n) { const uint32_t a = next; next += (n + 63) & ~63u; return a; }
  void putstr(uint32_t a, const std::string& s) { std::memset(at(a), 0, 16); std::memcpy(at(a), s.data(), std::min<size_t>(15, s.size())); }
};
// Pixel memory by physical address (a separate forge): 3 textures' pages.
struct Phys {
  static constexpr uint32_t kBase = 0x1C100000u;
  std::vector<uint8_t> b = std::vector<uint8_t>(0x40000, 0);
  bool read(uint32_t a, uint32_t n, uint8_t* d) const { if (a < kBase || uint64_t(a - kBase) + n > b.size()) return false; std::memcpy(d, b.data() + (a - kBase), n); return true; }
};
// A counting view of the memory: what the planner reads per call (the cost unit).
struct CountMem {
  const Mem& m;
  mutable uint64_t words = 0, big_bytes = 0, calls = 0;
  bool u32(uint32_t a, uint32_t& v) const { ++words; return m.u32(a, v); }
  bool bytes(uint32_t a, uint32_t n, uint8_t* d) const { ++calls; if (n >= 256) big_bytes += n; return m.bytes(a, n, d); }
};

// ---- the packer (sub_82BD4BE0), as the byte stores the game makes -------------------------------------------------------------------------------------------------------------------------------
static uint32_t FBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static uint32_t Be(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) { return (uint32_t(b0) << 24) | (uint32_t(b1) << 16) | (uint32_t(b2) << 8) | b3; }   // memory order -> the value a PPC word load sees
static uint16_t Half(float f) {   // normal range only (the forges use exact quarters)
  uint32_t x = FBits(f);
  const uint32_t s = (x >> 16) & 0x8000u;
  const int e = int((x >> 23) & 255) - 127 + 15;
  const uint32_t m = (x >> 13) & 0x3FFu;
  return uint16_t(s | (uint32_t(e) << 10) | m);
}
static uint32_t Pack1011(const float n[3]) {   // sub_82C14BC0: x:10 low (511), y:11 (1023), z:11 high (1023)
  const int32_t x = int32_t(std::lround(n[0] * 511.f)), y = int32_t(std::lround(n[1] * 1023.f)), z = int32_t(std::lround(n[2] * 1023.f));
  return (uint32_t(x) & 0x3FFu) | ((uint32_t(y) & 0x7FFu) << 10) | ((uint32_t(z) & 0x7FFu) << 21);
}
struct Vtx {
  float pos[3] = {0, 0, 0}, w[3] = {0, 0, 0};
  uint8_t s[4] = {255, 255, 255, 255};
  float n[3] = {0, 0, 0}, t[3] = {0, 0, 0};
  uint32_t colour = 0xFFFFFFFFu;
  float uv[4][2] = {};
};
static std::vector<uint32_t> Pack(uint32_t F, const Vtx& v) {
  std::vector<uint32_t> d = {FBits(v.pos[0]), FBits(v.pos[1]), FBits(v.pos[2]), FBits(1.0f)};
  if ((F & 12) == 12) d.push_back(FBits(v.w[0])), d.push_back(FBits(v.w[1])), d.push_back(FBits(v.w[2])), d.push_back(Be(v.s[3], v.s[2], v.s[1], v.s[0]));   // +16 +20 +24 weights; +31 s0 +30 s1 +29 s2 +28 s3
  else if (((F & 14) & ~4u) == 10) d.push_back(FBits(v.w[0])), d.push_back(FBits(v.w[1])), d.push_back(FBits(0.f)), d.push_back(Be(255, v.s[2], v.s[1], v.s[0]));   // +28 = 255
  else if ((F & 8) == 8) d.push_back(FBits(v.w[0])), d.push_back(FBits(0.f)), d.push_back(FBits(0.f)), d.push_back(Be(255, 255, v.s[1], v.s[0]));
  else if ((F & 6) == 6) d.push_back(Be(255, 255, 255, v.s[0]));   // +16 +17 +18 = 255, +19 = s0
  if (F & 0x10) d.push_back(Pack1011(v.n));
  if (F & 0x10000) d.push_back(Pack1011(v.t));
  if (F & 0x40) d.push_back(v.colour);
  const uint32_t nuv = (F & 0xF00) >> 8;
  for (uint32_t k = 0; k < nuv && k < 4; ++k) d.push_back((uint32_t(Half(v.uv[k][0])) << 16) | Half(v.uv[k][1]));   // half(u) at +0, half(v) at +2
  return d;
}

// ---- the forged model ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Samp { std::string name; int32_t t = -1; uint32_t kind = 15; };
struct PartSpec {
  uint32_t F = 0x156, vcount = 0;
  std::vector<uint32_t> host_dwords;   // vcount * stride/4 values (host order); the image holds them big-endian
  std::vector<uint16_t> idx;           // n0 (+ n1) indices, strip + 0xFFFF restarts
  uint32_t n0 = 0, n1 = 0;
  std::vector<uint32_t> palette;       // 1-based node numbers ([desc+12+4k])
  std::string effect = "yCh_skin";
  std::vector<Samp> samplers;
  uint32_t stride_override = 0;        // 0 = dwords * 4
};
struct Ov { std::string name; uint32_t value_key = 0; };
struct ModelSpec {
  std::vector<PartSpec> parts;
  bool two_ranges = false, extra_streams = false;
  std::vector<uint32_t> tex_keys;   // the model's texture table: word 0 of each 8-byte key
  std::vector<Ov> overrides;
};

static uint32_t BuildModel(Mem& m, const ModelSpec& spec) {
  const std::vector<PartSpec>& parts = spec.parts;
  const uint32_t model = m.alloc(1024), n = uint32_t(parts.size());
  const uint32_t desc = m.alloc(184 * n), vbs_arr = m.alloc(4 * n), ib_arr = m.alloc(4 * n), stride_arr = m.alloc(8 * n), n0_arr = m.alloc(2 * n), n1_arr = m.alloc(2 * n);
  m.put32(model + 824, desc), m.put32(model + 372, n), m.put32(model + 336, vbs_arr), m.put32(model + 344, ib_arr), m.put32(model + 360, stride_arr);
  m.put32(model + 376, n0_arr), m.put32(model + 380, n1_arr), m.put32(model + 548, spec.two_ranges ? 1 : 0), m.put32(model + 348, 0), m.put32(model + 352, 0);
  if (spec.extra_streams) {
    const uint32_t arr = m.alloc(40);
    m.put32(arr + 16, 1);
    m.put32(model + 232, arr);
  }
  if (!spec.tex_keys.empty()) {
    const uint32_t keys = m.alloc(8 * uint32_t(spec.tex_keys.size()));
    for (size_t i = 0; i < spec.tex_keys.size(); ++i) m.put32(keys + 8 * uint32_t(i), spec.tex_keys[i]), m.put32(keys + 8 * uint32_t(i) + 4, 0);
    m.put32(model + 256, uint32_t(spec.tex_keys.size())), m.put32(model + 260, keys);
  }
  if (!spec.overrides.empty()) {
    const uint32_t obj = m.alloc(1200), ents = m.alloc(20 * uint32_t(spec.overrides.size()));
    for (size_t i = 0; i < spec.overrides.size(); ++i) {
      const uint32_t nm = m.alloc(16), val = m.alloc(8);
      m.putstr(nm, spec.overrides[i].name);
      m.put32(val, spec.overrides[i].value_key), m.put32(val + 4, 0);
      m.put32(ents + 20 * uint32_t(i), val), m.put32(ents + 20 * uint32_t(i) + 8, nm);
    }
    m.put32(obj + 1132, ents), m.put32(obj + 1136, uint32_t(spec.overrides.size()));
    m.put32(model + 832, obj);
  }
  for (uint32_t p = 0; p < n; ++p) {
    const PartSpec& s = parts[p];
    const uint32_t stride = s.stride_override ? s.stride_override : uint32_t(s.host_dwords.size() / std::max<uint32_t>(1, s.vcount)) * 4;
    const uint32_t d = desc + 184 * p;
    m.put32(d + 0, s.vcount), m.put32(d + 8, uint32_t(s.palette.size()));
    for (size_t k = 0; k < s.palette.size(); ++k) m.put32(d + 12 + 4 * uint32_t(k), s.palette[k]);
    m.putstr(d + 124, s.effect);
    if (!s.samplers.empty()) {
      const uint32_t arr = m.alloc(4 * uint32_t(s.samplers.size() + 1));
      for (size_t i = 0; i < s.samplers.size(); ++i) {
        const uint32_t po = m.alloc(32);
        m.putstr(po, s.samplers[i].name);
        m.put16(po + 16, s.samplers[i].kind), m.put16(po + 18, 4), m.put32(po + 20, uint32_t(s.samplers[i].t));
        m.put32(arr + 4 * uint32_t(i), po);
      }
      m.put32(d + 148, uint32_t(s.samplers.size())), m.put32(d + 152, arr);
    }
    const uint32_t vstreams = m.alloc(8), vh = m.alloc(32), vdata = m.alloc(s.vcount * stride);
    m.put32(vbs_arr + 4 * p, vstreams), m.put32(vstreams, vh);
    m.put32(vh + 24, vdata | 3u), m.put32(vh + 28, (s.vcount * stride) | 0x10000002u);   // fetch type bits low; bit 28 and the endian bit set
    m.put32(stride_arr + 8 * p, s.F), m.put32(stride_arr + 8 * p + 4, stride);
    for (size_t i = 0; i < s.host_dwords.size(); ++i) m.put32(vdata + 4 * uint32_t(i), s.host_dwords[i]);   // host value stored big-endian: the reader's swap gives the value back
    const uint32_t ih = m.alloc(32), idata = m.alloc(uint32_t(s.idx.size()) * 2);
    m.put32(ib_arr + 4 * p, ih), m.put32(ih + 24, idata), m.put32(ih + 28, uint32_t(s.idx.size()) * 2);
    for (size_t i = 0; i < s.idx.size(); ++i) m.put16(idata + 2 * uint32_t(i), s.idx[i]);
    m.put16(n0_arr + 2 * p, s.n0), m.put16(n1_arr + 2 * p, s.n1);
  }
  return model;
}
static uint32_t VdataOf(const Mem& m, uint32_t model, uint32_t p) {   // the guest data address of part p's vertex buffer
  uint32_t a = 0, v = 0, h = 0, d = 0;
  m.u32(model + 336, a), m.u32(a + 4 * p, v), m.u32(v, h), m.u32(h + 24, d);
  return d & ~3u;
}

static std::vector<uint32_t> Dwords(uint32_t vcount, uint32_t dw, uint32_t seed) {
  std::vector<uint32_t> d(size_t(vcount) * dw);
  uint32_t x = seed * 2654435761u + 1;
  for (uint32_t& v : d) { x = x * 1664525u + 1013904223u; v = x; }
  return d;
}
static constexpr uint32_t kFRigid8 = 0x156;   // rigid slot, normal, colour, 1 uv set: 8 dwords (the stock-body parts)

// ---- the checks -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static void CheckForged() {
  Mem m;
  // part 0: 6 vertices, stride 32, a strip with a restart: [0 1 2 3] | [2 3 4 5]; part 1: 4 vertices, the n1 second range: [0 1 2 3] and [3 2 1 0]
  ModelSpec spec;
  spec.two_ranges = true, spec.extra_streams = true;
  PartSpec a, b;
  a.vcount = 6, a.host_dwords = Dwords(6, 8, 1), a.idx = {0, 1, 2, 3, 0xFFFF, 2, 3, 4, 5}, a.n0 = 9, a.n1 = 0;
  b.vcount = 4, b.host_dwords = Dwords(4, 8, 2), b.idx = {0, 1, 2, 3, 3, 2, 1, 0}, b.n0 = 4, b.n1 = 4;
  spec.parts = {a, b};
  const uint32_t model = BuildModel(m, spec);
  const gm::ModelRead r = gm::ReadModel(m, model);
  Check(r.why == gm::Why::Ok && r.parts == 2 && r.parts_ok == 2, "forged model: 2 parts read");
  Check(r.parts_ok == 2 && r.p[0].vertex_count == 6 && r.p[0].stride == 32 && r.p[1].vertex_count == 4 && r.p[0].vflags == kFRigid8, "vertex counts, strides (the second word of the pair, [m+360]+8p+4) and F (the first word)");
  bool vtx_eq = r.parts_ok == 2;
  for (int p = 0; vtx_eq && p < 2; ++p) {
    const auto& src = (p ? b : a).host_dwords;
    vtx_eq = r.p[p].vtx && r.p[p].vtx->size() == src.size() * 4 && std::memcmp(r.p[p].vtx->data(), src.data(), src.size() * 4) == 0;
  }
  Check(vtx_eq, "vertex bytes equal the forged values (data address masked, 8-in-32 swap)");
  Check(r.parts_ok == 2 && r.p[0].idx == a.idx && r.p[0].restarts == 1 && r.p[0].n0 == 9 && r.p[0].n1 == 0, "part 0 indices (big-endian u16) with its restart");
  Check(r.parts_ok == 2 && r.p[1].n0 == 4 && r.p[1].n1 == 4 && (r.p[1].flags & gm::kPartN1) && r.p[1].idx == b.idx, "part 1: the n1 second range read");
  Check(r.parts_extra_streams == 2, "extra streams counted (a HOLE), not read");
  std::vector<uint32_t> tl;
  if (r.parts_ok == 2) gm::StripsToList(r.p[0].idx, r.p[0].vertex_count, r.p[0].n0, r.p[0].n1, tl);
  const std::vector<uint32_t> want0 = {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5};
  Check(tl == want0, "strip with a restart -> the exact triangle list (winding alternates, restart resets)");
  std::vector<uint32_t> tl1;
  gm::StripStats s1 = r.parts_ok == 2 ? gm::StripsToList(r.p[1].idx, r.p[1].vertex_count, r.p[1].n0, r.p[1].n1, tl1) : gm::StripStats{};
  Check(tl1.size() == 12 && s1.triangles == 4, "two ranges each make their own strip (2 + 2 triangles)");
  // never cached by address: the same address, changed memory
  Mem m2 = m;
  m2.put32(VdataOf(m2, model, 0), 0x12345678u);
  gm::AddressCachedReader cr;
  const uint64_t h1 = cr.Read(m, model).hash;
  const uint64_t h2 = cr.Read(m2, model).hash;
  Check(h1 != h2, "memory reused at the same address reads as a different model (no cache by address)");
  Check(gm::ReadModel(m, model).hash == h1, "the same content hashes the same");
  Mem z;
  const uint32_t empty = z.alloc(1024);
  Check(gm::ReadModel(z, empty).why == gm::Why::NoDesc, "[m+824] == 0 is unreadable and says so");
  // S103 WC R3: [m+824] == 0 and [m+220] = D (class 0x8206C7A0: vtable slot +60 = sub_827C17B8, `return [D+12]` = H): the descriptors are [H+20] + 184 p for p < [H+16] [C WNCC_adversary.md 4a-4c]
  uint32_t desc0 = 0;
  m.u32(model + 824, desc0);
  auto forge_d = [&](Mem& mm, uint32_t getter, uint32_t h_count) {
    const uint32_t d = mm.alloc(16), dvt = mm.alloc(128), h = mm.alloc(64);
    mm.put32(d, dvt), mm.put32(dvt + 60, getter), mm.put32(d + 12, h);
    mm.put32(h + 16, h_count), mm.put32(h + 20, desc0);
    mm.put32(model + 824, 0), mm.put32(model + 220, d);
    return dvt;
  };
  Mem m3 = m;
  forge_d(m3, 0x827C17B8u, 2);
  const gm::ModelRead r3 = gm::ReadModel(m3, model);
  Check(r3.why == gm::Why::Ok && r3.desc_via_d && !r3.parts_mismatch && r3.parts_ok == 2 && r3.hash == r.hash, "WC R3: [m+824] == 0 with a data object reads the same rows as the [m+824] path (same hash)");
  Mem m4 = m;
  const uint32_t dvt4 = forge_d(m4, 0x12345678u, 2);
  const gm::ModelRead r4 = gm::ReadModel(m4, model);
  Check(r4.why == gm::Why::NoDesc && !r4.desc_via_d && r4.d_vtable == dvt4, "WC R3: a D whose +60 is not the getter stays unreadable, and its vtable is named for the census");
  Mem m5 = m;
  forge_d(m5, 0x827C17B8u, 3);
  const gm::ModelRead r5 = gm::ReadModel(m5, model);
  Check(r5.why == gm::Why::Ok && r5.parts_mismatch && r5.parts_ok == 2, "WC R3: [H+16] != [m+372] is a census counter, not a refusal");
  Mem m6 = m;
  forge_d(m6, 0x827C17B8u, 1);
  const gm::ModelRead r6 = gm::ReadModel(m6, model);
  Check(r6.why == gm::Why::Ok && r6.parts_mismatch && r6.parts == 1 && r6.parts_ok == 1, "WC R3: [H+16] below [m+372]: only the descriptors the game has are read");
}

// Every F variant, vertices built by the packer's rules, read through the reader (the layout from F) and decoded with a NON-identity palette.
struct Variant { const char* name; uint32_t F; };
static void CheckVariants() {
  const Variant vars[] = {
      {"A 4 pairs, normal, colour, 1 uv", 0x0C | 0x10 | 0x40 | 0x100},
      {"A 4 pairs, normal, tangent, colour, 2 uv", 0x0C | 0x10 | 0x10000 | 0x40 | 0x200},
      {"B 3 pairs, normal, colour, 1 uv", 0x0A | 0x10 | 0x40 | 0x100},
      {"C 2 pairs, normal, colour, 3 uv", 0x08 | 0x10 | 0x40 | 0x300},
      {"D rigid, normal, colour, 1 uv", 0x06 | 0x10 | 0x40 | 0x100},
      {"D rigid, normal, colour, 4 uv", 0x06 | 0x10 | 0x40 | 0x400},
      {"no skin, normal, colour, 1 uv (the default F 0x152)", 0x52 | 0x100},
      {"A with the declaration-only bit 0x1000", 0x0C | 0x10 | 0x40 | 0x100 | 0x1000},
  };
  const uint32_t kPal = 12;
  std::vector<uint32_t> pal(kPal);
  for (uint32_t k = 0; k < kPal; ++k) pal[k] = (k * 13 + 5) % 97 + 1;   // non-identity, 1-based
  for (const Variant& v : vars) {
    const uint32_t F = v.F;
    Vtx vx[3];
    for (int i = 0; i < 3; ++i) {
      vx[i].pos[0] = 1.5f + float(i), vx[i].pos[1] = -2.f, vx[i].pos[2] = 3.25f;
      vx[i].w[0] = 0.5f, vx[i].w[1] = 0.25f, vx[i].w[2] = 0.125f;
      vx[i].s[0] = uint8_t(3 + i), vx[i].s[1] = 7, vx[i].s[2] = 9, vx[i].s[3] = 11;
      vx[i].n[0] = 0.5f, vx[i].n[1] = -0.25f, vx[i].n[2] = 1.0f;
      vx[i].t[0] = -1.0f, vx[i].t[1] = 0.5f, vx[i].t[2] = 0.0f;
      vx[i].colour = 0xFF8040C0u + uint32_t(i);
      for (int k = 0; k < 4; ++k) vx[i].uv[k][0] = 0.25f + float(k), vx[i].uv[k][1] = 0.75f - 0.5f * float(k);
    }
    PartSpec part;
    part.F = F, part.vcount = 3, part.idx = {0, 1, 2}, part.n0 = 3, part.palette = pal;
    for (int i = 0; i < 3; ++i) {
      const std::vector<uint32_t> d = Pack(F, vx[i]);
      part.host_dwords.insert(part.host_dwords.end(), d.begin(), d.end());
    }
    Mem m;
    ModelSpec spec;
    spec.parts = {part};
    const uint32_t model = BuildModel(m, spec);
    const gm::ModelRead r = gm::ReadModel(m, model);
    const std::string nm = std::string("F ") + v.name;
    Check(r.parts_ok == 1 && r.p[0].stride == uint32_t(part.host_dwords.size() / 3 * 4), nm + ": reads, stride = what the packer wrote");
    if (r.parts_ok != 1) continue;
    std::vector<assets::Vertex> out;
    guestmodel::DecodeStats ds;
    const bool ok = guestmodel::DecodeVertices(r.p[0].vtx->data(), 3, r.p[0].stride, F, r.p[0].nodes, r.p[0].node_count, out, ds);
    Check(ok && out.size() == 3, nm + ": decodes");
    if (!ok || out.size() != 3) continue;
    bool pos_ok = true, uv_ok = true, nrm_ok = true, col_ok = true, bone_ok = true, tan_ok = true;
    const bool skin_pairs = (F & 12) == 12 || (F & 10) == 10 || (F & 8);
    for (int i = 0; i < 3; ++i) {
      const assets::Vertex& o = out[size_t(i)];
      pos_ok = pos_ok && Near(o.pos[0], vx[i].pos[0]) && Near(o.pos[1], vx[i].pos[1]) && Near(o.pos[2], vx[i].pos[2]);
      if ((F & 0xF00) >= 0x100) uv_ok = uv_ok && o.uv0[0] == vx[i].uv[0][0] && o.uv0[1] == vx[i].uv[0][1];
      if (F & 0x10) nrm_ok = nrm_ok && Near(o.normal[0], 0.5f, 1.0f / 511.f) && Near(o.normal[1], -0.25f, 1.0f / 1023.f) && Near(o.normal[2], 1.0f, 1.0f / 1023.f);
      if (F & 0x10000) tan_ok = tan_ok && Near(o.tangent[0], -1.0f, 1.0f / 511.f) && Near(o.tangent[1], 0.5f, 1.0f / 1023.f) && Near(o.tangent[2], 0.f);
      if (F & 0x40) col_ok = col_ok && o.color == vx[i].colour;
      // expected bones / weights from the PACKER's inputs and the shader rule: the last present bone takes 1 - sum
      uint16_t eb[4] = {0, 0, 0, 0};
      float ew[4] = {0, 0, 0, 0};
      if ((F & 12) == 12) { for (int k = 0; k < 4; ++k) eb[k] = uint16_t(pal[vx[i].s[k]] - 1); ew[0] = 0.5f, ew[1] = 0.25f, ew[2] = 0.125f, ew[3] = 1.f - 0.875f; }
      else if ((F & 10) == 10) { for (int k = 0; k < 3; ++k) eb[k] = uint16_t(pal[vx[i].s[k]] - 1); ew[0] = 0.5f, ew[1] = 0.25f, ew[2] = 1.f - 0.75f; }
      else if (F & 8) { for (int k = 0; k < 2; ++k) eb[k] = uint16_t(pal[vx[i].s[k]] - 1); ew[0] = 0.5f, ew[1] = 0.5f; }
      else if ((F & 6) == 6) { eb[0] = uint16_t(pal[vx[i].s[0]] - 1); ew[0] = 1.f; }
      else ew[0] = 1.f;
      for (int k = 0; k < 4; ++k) bone_ok = bone_ok && o.bones[k] == eb[k] && Near(o.weights[k], ew[k]);
      (void)skin_pairs;
    }
    Check(pos_ok, nm + ": position");
    Check(uv_ok, nm + ": uv0 = (u, v): half(u) is the HIGH half of the host dword (the packer's +0), the jboy loader's order");
    Check(nrm_ok, nm + ": normal 10:11:11 (x low)");
    Check(tan_ok, nm + ": tangent 10:11:11 (only when F says so)");
    Check(col_ok, nm + ": colour dword raw (only when F says so)");
    Check(bone_ok, nm + ": palette slot -> node = palette[slot] - 1; the last present bone gets 1 - sum (2 / 3 / 4 pairs)");
  }
  // the 2- and 3-pair cases through the reader's whole path with a partial set: explicit numbers (a planted "partial weights dropped" fails here)
  {
    Vtx v;
    v.pos[0] = 1, v.w[0] = 0.6f, v.w[1] = 0.3f, v.s[0] = 2, v.s[1] = 5, v.s[2] = 6;
    const uint32_t F3 = 0x0A | 0x10 | 0x40 | 0x100;
    std::vector<uint32_t> d = Pack(F3, v);
    std::vector<assets::Vertex> out;
    guestmodel::DecodeStats ds;
    const uint16_t nodes[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    const bool ok = guestmodel::DecodeVertices(reinterpret_cast<const uint8_t*>(d.data()), 1, uint32_t(d.size() * 4), F3, nodes, 8, out, ds);
    Check(ok && out[0].bones[0] == 12 && out[0].bones[1] == 15 && out[0].bones[2] == 16 && out[0].weights[3] == 0.f && Near(out[0].weights[0], 0.6f) && Near(out[0].weights[1], 0.3f) && Near(out[0].weights[2], 0.1f, 1e-6f), "3 pairs (w0, w1, 0): the third present bone takes 1 - 0.9");
    Vtx v2;
    v2.w[0] = 0.7f, v2.s[0] = 4, v2.s[1] = 1;
    const uint32_t F2 = 0x08 | 0x10 | 0x40 | 0x100;
    d = Pack(F2, v2);
    const bool ok2 = guestmodel::DecodeVertices(reinterpret_cast<const uint8_t*>(d.data()), 1, uint32_t(d.size() * 4), F2, nodes, 8, out, ds);
    Check(ok2 && out[0].bones[0] == 14 && out[0].bones[1] == 11 && Near(out[0].weights[0], 0.7f) && Near(out[0].weights[1], 0.3f, 1e-6f) && out[0].weights[2] == 0.f, "2 pairs (w0, 0, 0): the second bone takes 1 - 0.7");
  }
  // unknown bits / an F that disagrees with the stride are refused, never guessed
  {
    const uint32_t Fbad = (0x06 | 0x10 | 0x40 | 0x100) | 0x20000;   // an unknown bit; the stride the packer would write for the known bits
    PartSpec part;
    part.F = Fbad, part.vcount = 3, part.idx = {0, 1, 2}, part.n0 = 3, part.palette = {1, 2, 3, 4};
    Vtx vx;
    for (int i = 0; i < 3; ++i) { const std::vector<uint32_t> d = Pack(0x06 | 0x10 | 0x40 | 0x100, vx); part.host_dwords.insert(part.host_dwords.end(), d.begin(), d.end()); }
    Mem m;
    ModelSpec spec;
    spec.parts = {part};
    const uint32_t model = BuildModel(m, spec);
    const gm::ModelRead r = gm::ReadModel(m, model);
    Check(r.parts_ok == 0 && r.p[0].why == gm::Why::BadFlags && r.parts_bad_flags == 1, "an unknown F bit is refused (BadFlags), whatever the stride says");
    std::vector<assets::Vertex> out;
    guestmodel::DecodeStats ds;
    Check(!guestmodel::DecodeVertices(reinterpret_cast<const uint8_t*>(part.host_dwords.data()), 3, uint32_t(part.host_dwords.size() / 3 * 4), Fbad, nullptr, 0, out, ds), "the decoder refuses an unknown F bit too");
    PartSpec mism = part;
    mism.F = 0x06 | 0x10 | 0x40 | 0x100;
    mism.stride_override = 36;   // 9 dwords written by nobody: F says 8
    mism.host_dwords = Dwords(3, 9, 5);
    Mem m2;
    ModelSpec spec2;
    spec2.parts = {mism};
    const gm::ModelRead r2 = gm::ReadModel(m2, BuildModel(m2, spec2));
    Check(r2.parts_ok == 0 && r2.p[0].why == gm::Why::BadFlags, "a stride that F does not make (CW-2's stride-only guess accepted any) is refused");
  }
}

static void CheckOracle() {
  // CAW_OURS.md section 1: the stock body = 12 parts (vertices / draw index count); part 4 = ed02 leaf 2002 mesh 1, groups 685 + 678 joined by 0xFFFF (draw 1364). The leaves are forged to those counts.
  const uint32_t cnt[12][2] = {{925, 2747}, {132, 322}, {49, 106}, {532, 1497}, {468, 1364}, {481, 1388}, {702, 1928}, {702, 1932}, {836, 2283}, {183, 354}, {362, 980}, {236, 367}};
  std::vector<std::vector<uint32_t>> dwords;
  std::vector<std::vector<uint16_t>> joined;
  ModelSpec spec;
  for (int p = 0; p < 12; ++p) {
    const uint32_t vcount = cnt[p][0];
    dwords.push_back(Dwords(vcount, 8, 100 + p));
    uint32_t x = 7 + p;
    auto strip = [&](uint32_t n) { std::vector<uint16_t> g(n); for (uint16_t& i : g) { x = x * 1103515245u + 12345u; i = uint16_t((x >> 8) % vcount); } return g; };
    std::vector<std::vector<uint16_t>> groups;
    if (p == 4) groups = {strip(685), strip(678)};
    else groups = {strip(cnt[p][1])};
    std::vector<uint16_t> j;   // the leaf join (caw_core's JoinedIndices rule): groups joined by one 0xFFFF
    for (size_t g = 0; g < groups.size(); ++g) { if (g) j.push_back(0xFFFF); j.insert(j.end(), groups[g].begin(), groups[g].end()); }
    joined.push_back(j);
    PartSpec ps;
    ps.vcount = vcount, ps.host_dwords = dwords.back(), ps.idx = j, ps.n0 = uint32_t(j.size()), ps.palette = {1, 2, 3};
    spec.parts.push_back(ps);
  }
  Mem m;
  const uint32_t model = BuildModel(m, spec);
  const gm::ModelRead r = gm::ReadModel(m, model);
  bool ok = r.parts == 12 && r.parts_ok == 12;
  uint32_t bad = 0;
  for (int p = 0; ok && p < 12; ++p) {
    const gm::PartRead& pr = r.p[size_t(p)];
    const bool eq = pr.vertex_count == cnt[p][0] && pr.n0 == cnt[p][1] && pr.n1 == 0 && pr.idx == joined[size_t(p)] && pr.vtx && pr.vtx->size() == dwords[size_t(p)].size() * 4 && std::memcmp(pr.vtx->data(), dwords[size_t(p)].data(), pr.vtx->size()) == 0;
    if (!eq) ++bad;
  }
  Check(ok && bad == 0, "oracle: the reader's decode of a forged image of the 12 stock-body leaves equals the leaf join (12 of 12 parts: vertices, draw index counts, bytes; part 4 = 685 + restart + 678)");
}

// ---- the planner: the filter and the cost -----------------------------------------------------------------------------------------------------------------------------------------------------
static ModelSpec OneBody(uint32_t seed);
// S108 RDS phase 2: a view of the forged memory that records every page a read ASKS (a range's pages, before any refusal) and refuses the pages outside `ok_pages` when set.
struct PageMem {
  explicit PageMem(const Mem& mm) : m(mm) {}
  const Mem& m;
  mutable std::vector<uint32_t> asked;
  mutable uint64_t reads = 0;
  const std::vector<uint32_t>* ok_pages = nullptr;
  bool range(uint32_t a, uint32_t n) const {
    bool ok = true;
    for (uint64_t p = a >> 12; p <= (uint64_t(a) + n - 1) >> 12; ++p) {
      if (std::find(asked.begin(), asked.end(), uint32_t(p)) == asked.end()) asked.push_back(uint32_t(p));
      if (ok_pages && std::find(ok_pages->begin(), ok_pages->end(), uint32_t(p)) == ok_pages->end()) ok = false;
    }
    return ok;
  }
  bool u32(uint32_t a, uint32_t& v) const { ++reads; return range(a, 4) && m.u32(a, v); }
  bool bytes(uint32_t a, uint32_t n, uint8_t* d) const { ++reads; return range(a, n) && m.bytes(a, n, d); }
};
// The layout pass reads each 128-byte aligned line once (guest_model_reader.h Rd): the same layout as the field reads (key, every part's words), no page the field reads do not ask, far fewer reads.
// The second model object sits at a page end (its last layout word, +832, ends at the page end; the next page is never asked by the field pass and is unreadable for the line pass).
// Twins: plant 15 (the window starts AT the field) asks the page past that end; plant 16 (no line copy) fails the cost.
static void CheckLineCopy() {
  Mem m;
  const uint32_t body = BuildModel(m, OneBody(5));
  const uint32_t pg = (m.next + 0x1FFFu) & ~0xFFFu;
  const uint32_t moved = pg + 0x1000u - (gm::kOffOverride + 4);
  for (uint32_t o = 0; o < gm::kOffOverride + 4; o += 4) { uint32_t v = 0; m.u32(body + o, v); m.put32(moved + o, v); }
  m.next = pg + 0x3000u;
  bool same = true, subset = true, cost = true;
  uint64_t field_reads = 0, line_reads = 0;
  for (uint32_t model : {body, moved}) {
    const int keep = gm::g_gm_plant;
    gm::g_gm_plant = 16;   // the reference: one read per field
    PageMem ref{m};
    const gm::ModelRead a = gm::ReadModelLayout(ref, model);
    gm::g_gm_plant = keep;
    PageMem sub{m};
    sub.ok_pages = &ref.asked;
    const gm::ModelRead b = gm::ReadModelLayout(sub, model);
    bool eq = a.why == gm::Why::Ok && a.why == b.why && a.layout_key == b.layout_key && a.parts == b.parts && a.parts_ok == b.parts_ok && a.parts_ok == 12 && a.p.size() == b.p.size();
    for (size_t p = 0; eq && p < a.p.size(); ++p) {
      const gm::PartRead &x = a.p[p], &y = b.p[p];
      eq = x.why == y.why && x.vertex_count == y.vertex_count && x.stride == y.stride && x.vflags == y.vflags && x.n0 == y.n0 && x.n1 == y.n1 && x.node_count == y.node_count &&
           std::memcmp(x.nodes, y.nodes, sizeof x.nodes) == 0 && std::memcmp(x.effect, y.effect, sizeof x.effect) == 0 && x.vdata == y.vdata && x.idata == y.idata && x.samplers.size() == y.samplers.size();
    }
    same = same && eq;
    for (uint32_t pgn : sub.asked)
      if (std::find(ref.asked.begin(), ref.asked.end(), pgn) == ref.asked.end()) subset = false;
    cost = cost && sub.reads * 3 <= ref.reads;
    field_reads += ref.reads, line_reads += sub.reads;
  }
  Check(same, "rds.layout_line_copy_same: the line-copy layout pass equals the per-field pass (layout key, 12 parts' words), at a page end too");
  Check(subset, "rds.layout_line_copy_pages: no page asked that the per-field pass does not ask (the model at the page end included)");
  Check(cost, "rds.layout_line_copy_cost: reads per layout pass " + std::to_string(line_reads / 2) + " with the line copy, " + std::to_string(field_reads / 2) + " one per field (at least 3x fewer)");
}
static ModelSpec OneBody(uint32_t seed) {
  ModelSpec spec;
  for (int p = 0; p < 12; ++p) {
    PartSpec ps;
    ps.vcount = 200 + 10 * uint32_t(p), ps.host_dwords = Dwords(ps.vcount, 8, seed * 100 + uint32_t(p)), ps.idx = {0, 1, 2, 3, 4, 5};
    ps.n0 = 6, ps.palette = {1, 2, 3};
    spec.parts.push_back(ps);
  }
  return spec;
}
static void CheckPlanner() {
  ul::Ring ring;
  Mem m;
  uint32_t named[6], unnamed = 0;
  for (int i = 0; i < 6; ++i) named[i] = BuildModel(m, OneBody(1 + uint32_t(i)));
  unnamed = BuildModel(m, OneBody(9));
  // the filter twin: a named wrestler must NOT be read (zero words, zero bytes); the cost unit: a match-like set (6 named + 1 unnamed) reads ONLY the unnamed one
  {
    gm::Planner pl;
    const CountMem cm{m};
    for (uint64_t frame = 100; frame < 110; ++frame) {
      for (int i = 0; i < 6; ++i) pl.Update(cm, named[i], true, true, frame, nullptr, ring);
      pl.Update(cm, unnamed, true, false, frame, nullptr, ring);
    }
    const gm::PlannerStats& st = pl.stats();
    uint64_t one_body = 0;
    for (const gm::PartRead& p : gm::ReadModel(m, unnamed).p) one_body += uint64_t(p.vertex_count) * p.stride;   // (the counting view counts runs of >= 256 bytes: the vertex runs)
    Check(st.skipped_named == 60 && pl.size() == 1, "filter: 6 named bodies x 10 frames are skipped and keep no state (a named wrestler is NOT read)");
    Check(st.byte_reads == 1 && cm.big_bytes == one_body && st.reused == 9, "cost: 7 models for 10 frames = ONE byte read of the unnamed body (" + std::to_string(cm.big_bytes) + " B), 9 frames reuse it (layout words only)");
    std::printf("note: cost per call of the unnamed 12-part body: %.0f guest word reads + %.0f small byte reads (layout pass), vertex + index bytes %llu B read once\n", double(cm.words) / double(st.layout_reads),
                double(cm.calls) / double(st.layout_reads), (unsigned long long)st.bytes_read);
    // an unlock of one of its buffers makes the next call re-read; a forged frame without an unlock does not
    ul::Note(VdataOf(m, unnamed, 3), ring);
    pl.Update(cm, unnamed, true, false, 110, nullptr, ring);
    Check(pl.stats().byte_reads == 2, "an unlock of one of its buffers (VB or IB data address) makes the next call re-read once");
    ul::Note(0x70000000u, ring);   // another buffer
    pl.Update(cm, unnamed, true, false, 111, nullptr, ring);
    Check(pl.stats().byte_reads == 2, "an unlock of an unrelated buffer does not");
    // memory reused at the same address with new content and NO unlock: the layout words change when the buffer header does; when only the bytes do, the forced re-read catches it and counts it
    Mem m2 = m;
    m2.put32(VdataOf(m2, unnamed, 2) + 16, 0xCAFEBABEu);
    const CountMem cm2{m2};
    bool fresh = false;
    for (uint64_t f2 = 112; f2 < 112 + gm::kForceRereadCalls + 2; ++f2) { bool fr = false; pl.Update(cm2, unnamed, true, false, f2, &fr, ring); fresh = fresh || fr; }
    Check(fresh && pl.stats().forced == 1 && pl.stats().missed_unlock == 1, "bytes changed with no unlock are caught by the forced re-read period and counted as a missed unlock");
    Check((gm::kForceRereadCalls & 1) == 1 && (kGmResendFrames & 1) == 1, "the forced re-read period and the blob resend period are odd");
  }
  // a changed layout (the model object reused for another body at the same address) is re-read at once
  {
    gm::Planner pl;
    Mem mm;
    const uint32_t body = BuildModel(mm, OneBody(3));
    pl.Update(CountMem{mm}, body, true, false, 1, nullptr, ring);
    ModelSpec other = OneBody(4);
    other.parts.pop_back();
    Mem m3;   // the same address (a fresh image allocates from the same start) now holds another body
    const uint32_t body2 = BuildModel(m3, other);
    Check(body2 == body && pl.Update(CountMem{m3}, body2, true, false, 2, nullptr, ring)->parts == 11 && pl.stats().byte_reads == 2, "an address reused for another body (a different layout) is re-read at once");
  }
  // ring overrun: more unlocks than the ring holds since the read = dirty for everyone
  {
    ul::Ring r2;
    const uint64_t since = ul::Now(r2);
    for (uint32_t i = 0; i < ul::kRing + 5; ++i) ul::Note(0x50000000u + 4 * i, r2);
    const uint32_t a = 0x71000000u;
    Check(ul::Dirty(&a, 1, since, r2), "a ring overrun marks every reader dirty (never a stale model)");
  }
}

// ---- the textures: key resolution through a forged [m+260] table and registry ------------------------------------------------------------------------------------------------------------------
struct TexWorld {
  Mem m;
  Phys phys;
  uint32_t registry = 0, begin = 0;
  uint32_t next_idx = 0;
  uint32_t wrapper_of[64] = {}, header_of[64] = {};   // by (id & 0xFFFF): lets a test register a texture late / change its header
  std::vector<uint8_t> pattern[4];
  TexWorld() {
    registry = m.alloc(64);
    begin = m.alloc(8 * 16);
    m.put32(0x8392DD9Cu, registry), m.put32(registry + 8, begin), m.put32(registry + 16, begin + 8 * 16);
  }
  // a 64x64 BC1 texture whose pixel page is at `base`; returns its handle. `pattern[slot]` = the expected blocks.
  uint32_t AddTexture(uint32_t slot, uint32_t base, uint8_t seed, bool register_it = true) {
    const uint32_t w = 64, h = 64, bx = w / 4, by = h / 4, row_bytes = 32 * 8;
    pattern[slot].assign(size_t(bx) * by * 8, 0);
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t i = 0; i < row_bytes; ++i) {
        const uint8_t v = uint8_t(seed + y * 7 + i * 3);
        phys.b[base - Phys::kBase + y * row_bytes + i] = v;
        if (i < bx * 8) pattern[slot][size_t(y) * bx * 8 + i] = v;
      }
    const uint32_t id = 0x00010000u | next_idx++;
    const uint32_t wrapper = m.alloc(128), header = m.alloc(64);
    m.put32(wrapper + 0, 0x8206ADB8u), m.put32(wrapper + 52, 0), m.put32(wrapper + 80, header);
    m.put32(header + 28, 2u << 22);                    // untiled, pitch / 32 = 2
    m.put32(header + 32, base | 18u);                  // base, format 18 (BC1), endian 0
    m.put32(header + 36, (w - 1) | ((h - 1) << 13));
    m.put32(header + 40, 0), m.put32(header + 44, 0), m.put32(header + 48, 1u << 9);   // dimension 1 = 2D
    wrapper_of[id & 63] = wrapper, header_of[id & 63] = header;
    if (register_it) Register(id);
    return id;
  }
  void Register(uint32_t id) { m.put32(begin + 8 * (id & 0xFFFF), id), m.put32(begin + 8 * (id & 0xFFFF) + 4, wrapper_of[id & 63]); }
};
static void CheckTextures() {
  TexWorld w;
  const uint32_t hA = w.AddTexture(0, 0x1C100000u, 11), hB_unregistered = 0x00010009u, hC = w.AddTexture(1, 0x1C110000u, 99);
  // part 0: diffuse = table[0] (hA), normal = table[1] (not in the registry: missing), a kind-3 param (ignored), a sampler with t = -1 (none) and t = 5 (beyond the table)
  PartSpec p0;
  p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
  p0.effect = "yCh_skin_flex";
  p0.samplers = {{"sampDiffusePrm", 0, 15}, {"sampNormalPrm", 1, 15}, {"g_fSpecLev", 0, 3}, {"sampSpecularPrm", -1, 15}, {"g_mMaskPrm", 5, 15}};
  ModelSpec spec;
  spec.parts = {p0};
  spec.tex_keys = {hA, hB_unregistered};
  const uint32_t model = BuildModel(w.m, spec);
  const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
  gm::TexResolver res;
  gm::ModelRead r = gm::ReadModel(w.m, model);
  Check(r.parts_ok == 1 && r.p[0].samplers.size() == 2 && std::string(r.p[0].samplers[0].name) == "sampDiffusePrm" && r.p[0].samplers[0].key == hA && r.p[0].samplers[1].key == hB_unregistered,
        "samplers: only kind-15 parameters with a table key are read (kind 3, t = -1 and t >= the table are not); the key is word 0 of [m+260] + 8t");
  if (r.parts_ok != 1 || r.p[0].samplers.size() != 2) { Check(false, "textures: the forged part did not read; the rest of the texture checks are skipped"); return; }
  res.Resolve(w.m, phys, r, 100);
  Check(r.tex_state[0][0] == kGtBound && r.tex_hash[0][0] != 0 && r.tex_state[0][1] == kGtMissing && r.textures.size() == 1 && r.textures[0].bytes && *r.textures[0].bytes == w.pattern[0] && r.textures[0].width == 64 && r.textures[0].format == 18,
        "the key resolves through the registry to the game's texture object: the diffuse is bound with the exact blocks (untiled, pitch padding skipped); an unregistered key is missing");
  Check(res.stats().bound == 1 && res.stats().missing == 1 && res.stats().byte_reads == 1, "counts: 1 bound, 1 missing, 1 byte read");
  // the identity cache: the same texture object is not read twice
  gm::ModelRead r2 = gm::ReadModel(w.m, model);
  res.Resolve(w.m, phys, r2, 101);
  Check(res.stats().byte_reads == 1 && res.stats().cache_hits == 1 && r2.tex_hash == r.tex_hash, "the same texture object is read once (identity = handle + fetch dwords)");
  // the override block: a name-matched entry whose handle resolves wins (state Override); one that does not resolve falls back to the model's own key
  {
    ModelSpec s2 = spec;
    s2.overrides = {{"sampDiffusePrm", hC}};
    const uint32_t mo = BuildModel(w.m, s2);
    gm::ModelRead ro = gm::ReadModel(w.m, mo);
    if (ro.parts_ok != 1 || ro.p[0].samplers.size() != 2) { Check(false, "textures: the override model did not read"); return; }
    gm::TexResolver res2;
    res2.Resolve(w.m, phys, ro, 200);
    Check(ro.p[0].samplers[0].has_override && ro.ov_matched == 1 && ro.tex_state[0][0] == kGtOverride && ro.textures.size() >= 1 && *ro.textures[0].bytes == w.pattern[1] && res2.stats().from_override == 1,
          "override: the entry named like the parameter is bound first (state Override, the composite texture's blocks)");
    ModelSpec s3 = spec;
    s3.overrides = {{"sampDiffusePrm", 0x00010777u}};   // a handle the registry does not hold
    gm::ModelRead rf = gm::ReadModel(w.m, BuildModel(w.m, s3));
    if (rf.parts_ok != 1) { Check(false, "textures: the fallback model did not read"); return; }
    gm::TexResolver res3;
    res3.Resolve(w.m, phys, rf, 200);
    Check(rf.tex_state[0][0] == kGtBound && res3.stats().override_refused == 1 && *rf.textures[0].bytes == w.pattern[0], "an override handle that does not resolve falls back to the model's own key (counted override_refused)");
    // an override texture is re-read when its refresh comes (the game may rewrite it in place); a bound one is not
    gm::TexResolver res4;
    gm::ModelRead rr = gm::ReadModel(w.m, mo);
    res4.Resolve(w.m, phys, rr, 300);
    for (uint32_t i = 0; i < 64; ++i) w.phys.b[0x1C110000u - Phys::kBase + i] ^= 0x5A;   // rewrite the composite texture in place
    gm::ModelRead rr2 = gm::ReadModel(w.m, mo);
    res4.Resolve(w.m, phys, rr2, 300 + 10);
    const bool same_early = rr2.tex_hash == rr.tex_hash;
    gm::ModelRead rr3 = gm::ReadModel(w.m, mo);
    const bool changed = res4.Resolve(w.m, phys, rr3, 300 + gm::TexResolver::kOverrideRefreshFrames);
    Check(same_early && changed && rr3.tex_hash != rr.tex_hash, "an override texture rewritten in place is re-read at its refresh period (not before), a new hash");
  }
  // the per-call read budget: the rest are deferred (incomplete) and finish on the next call
  {
    PartSpec q = p0;
    q.samplers = {{"sampDiffusePrm", 0, 15}, {"sampNormalPrm", 1, 15}};
    ModelSpec s4;
    s4.parts = {q};
    const uint32_t hD = w.AddTexture(2, 0x1C120000u, 55);
    s4.tex_keys = {hA, hD};
    gm::ModelRead rb = gm::ReadModel(w.m, BuildModel(w.m, s4));
    gm::TexResolver res5;
    res5.Resolve(w.m, phys, rb, 400, 1);
    const bool deferred = rb.tex_incomplete;
    res5.Resolve(w.m, phys, rb, 401, 1);
    Check(deferred && !rb.tex_incomplete && rb.textures.size() == 2, "a budget of 1 read a call defers the second picture (incomplete) and the next call finishes it");
  }
}

// ---- CTF (S103, docs\research\s103\CTEX.md sites 1-5): the game's lower-case effect / parameter names, the printed refusal reasons, the retry of a late texture ------------------------------------------------
// S105 A5b: the created superstar's eyelid layer (guest part 11, yCh_trans) must be built BLENDED (the file loader's `*trans*` rule), a skin / anisotropic part must not; and a guest part never answers the front
// end's per-piece row lookup (its src_key is a part index, not a piece id). entry: guestmodel::BuildModel (production order: forged guest memory -> ReadModel -> AppendModelRows -> record bytes -> ParseRecord -> Absorb -> BuildModel)
// then the front end's rule on the built meshes (MeshAnswersPieceRow, called by front_end.cpp's EYF lookup). Planted twins: draw plant 12 (42) = the old no-blend rule, plant 13 (43) = every src_kind 2 mesh answers.
static void CheckBlend() {
  live::guest_model::ByteStore::Process().Clear();
  TexWorld w;
  const uint32_t hNormal = w.AddTexture(0, 0x1C100000u, 21);
  // the 7th part is a trans part with NO normal picture (review F1: its opacity would be the stand-in's: stays opaque, counted)
  const char* const effects[] = {"ych_trans", "ych_aniso_trans", "ych_skin", "ych_anisotropic", "ych_transtiling", "ych_cloth_trans", "ych_trans"};
  ModelSpec spec;
  spec.tex_keys = {hNormal};
  for (const char* e : effects) {
    PartSpec p;
    p.F = 0x0C | 0x10 | 0x40 | 0x100;
    p.vcount = 4, p.idx = {0, 1, 2, 3, 3, 2, 1, 0}, p.n0 = 4, p.n1 = 4, p.palette = {20, 30, 40, 50}, p.effect = e;
    Vtx vx;
    vx.pos[0] = 2.f, vx.w[0] = 0.5f, vx.s[0] = 0, vx.n[2] = 1.f, vx.uv[0][0] = 0.25f, vx.uv[0][1] = 0.75f;
    for (int i = 0; i < 4; ++i) { const std::vector<uint32_t> d = Pack(p.F, vx); p.host_dwords.insert(p.host_dwords.end(), d.begin(), d.end()); }
    if (spec.parts.size() < 6) p.samplers = {{"g_mNormalFxPrm", 0, 15}};
    spec.parts.push_back(p);
  }
  spec.two_ranges = true;
  const uint32_t model = BuildModel(w.m, spec);
  gm::ModelRead r = gm::ReadModel(w.m, model);
  gm::TexResolver blend_res;
  blend_res.Resolve(w.m, [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }, r, 10);
  SceneTables t;
  gm::BlobSender blobs;
  gm::AppendModelRows(r, t.guest_models, blobs, 1000);
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  guestmodel::Store blend_store;
  assets::Model bmdl;
  guestmodel::BuildStats bbs;
  bool built = false;
  if (ParseRecord(bytes.data(), bytes.size(), back, err)) blend_store.Absorb(back), built = guestmodel::BuildModel(back, blend_store, model, bmdl, bbs);
  if (!built || bmdl.meshes.size() != 7) { Check(false, "CBL: the forged 6-part model did not build"); return; }
  const auto blends = [&](size_t i) { return bmdl.materials[bmdl.meshes[i].material].alpha_blend; };
  Check(blends(0) && blends(1) && !blends(2) && !blends(3) && blends(4) && blends(5) && !blends(6) && bbs.blended == 4 && bbs.blended_no_normal == 1 && bbs.effect_fallback == 0,
        "CBL: yCh_trans / yCh_Aniso_trans / yCh_TransTiling / yCh_cloth_trans parts build blended (like the file loader's `*trans*` rule), yCh_skin and yCh_Anisotropic do not, and a trans part with no normal picture stays opaque and is counted (blended " + std::to_string(bbs.blended) + ")");
  // the front end's piece-row lookup (front_end.cpp EYF): a guest part carries src_kind 2 + src_key = the part index; no piece row may match it, a stock mesh of the same kind still does
  bool any_guest = false, shape = true;
  for (size_t i = 0; i < bmdl.meshes.size(); ++i) {
    const assets::Mesh& me = bmdl.meshes[i];
    shape = shape && me.src_kind == 2 && me.src_key == int(i) && me.src_pac == "guest";
    any_guest = any_guest || rdr_scene::MeshAnswersPieceRow(me.src_kind, me.src_pac);
  }
  Check(shape && !any_guest && rdr_scene::MeshAnswersPieceRow(2, "ch100.pac") && !rdr_scene::MeshAnswersPieceRow(1, "ch100.pac"),
        "CBL: a guest part (src_kind 2, src_key = part index 0..5, so piece id 1 could collide with part 1) never answers the piece-row lookup; a stock piece mesh still does");
}

static void CheckCtf() {
  live::guest_model::ByteStore::Process().Clear();
  // P1 case: the game's names are lower case in memory ("ych_gear", "sampdiffuseprm"); the part must get the gear pipeline and a Diffuse slot
  {
    TexWorld w;
    const uint32_t hA = w.AddTexture(0, 0x1C100000u, 21);
    PartSpec p0;
    p0.F = 0x0C | 0x10 | 0x40 | 0x100;
    p0.vcount = 4, p0.idx = {0, 1, 2, 3, 3, 2, 1, 0}, p0.n0 = 4, p0.n1 = 4, p0.palette = {20, 30, 40, 50}, p0.effect = "ych_gear";
    Vtx vx;
    vx.pos[0] = 2.f, vx.w[0] = 0.5f, vx.s[0] = 0, vx.n[2] = 1.f, vx.uv[0][0] = 0.25f, vx.uv[0][1] = 0.75f;
    for (int i = 0; i < 4; ++i) { const std::vector<uint32_t> d = Pack(p0.F, vx); p0.host_dwords.insert(p0.host_dwords.end(), d.begin(), d.end()); }
    p0.samplers = {{"sampdiffuseprm", 0, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.two_ranges = true;
    spec.tex_keys = {hA};
    const uint32_t model = BuildModel(w.m, spec);
    gm::ModelRead r = gm::ReadModel(w.m, model);
    gm::TexResolver res;
    res.Resolve(w.m, [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }, r, 10);
    SceneTables t;
    gm::BlobSender blobs;
    gm::AppendModelRows(r, t.guest_models, blobs, 1000);
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    std::string err;
    guestmodel::Store store;
    assets::Model mdl;
    guestmodel::BuildStats bs;
    bool built = false;
    if (ParseRecord(bytes.data(), bytes.size(), back, err)) store.Absorb(back), built = guestmodel::BuildModel(back, store, model, mdl, bs);
    Check(built && mdl.meshes.size() == 1 && mdl.materials[mdl.meshes[0].material].shader == "yCh_gear" && bs.effect_fallback == 0 && mdl.materials[mdl.meshes[0].material].diffuse >= 0 && bs.no_diffuse == 0,
          "CTF case: the game's lower-case 'ych_gear' + 'sampdiffuseprm' get the gear pipeline (not the skin fallback) and a Diffuse slot");
  }
  // the pipeline-name map itself: the game's lower case -> our mixed case, and an unknown name is not mapped
  const auto fx = [](const char* n) { const char* e = guestmodel::PipelineEffect(n); return std::string(e ? e : "(none)"); };
  Check(fx("ych_reflect") == "yCh_Reflect" && fx("YCH_LS") == "yCh_Ls" && fx("ych_skin_flex") == "yCh_skin_flex" &&
            guestmodel::PipelineEffect("notAnEffect") == nullptr && guestmodel::SlotOfParam("G_MDIFFUSEPRM") == guestmodel::TexSlot::Diffuse,
        "CTF case: effect names map case-insensitively to the pipeline's names; unknown names stay unknown");
  // P2 reasons: a forged NoHandle, Format (game format 49) and key 0 print exactly 1 / 1 / 1, missing 3
  {
    TexWorld w;
    const uint32_t hNo = 0x00010009u, hFmt = w.AddTexture(0, 0x1C100000u, 5);
    w.m.put32(w.header_of[hFmt & 63] + 32, 0x1C100000u | 49u);
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.effect = "ych_skin";
    p0.samplers = {{"sampDiffusePrm", 0, 15}, {"sampNormalPrm", 1, 15}, {"g_mMaskPrm", 2, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {hNo, hFmt, 0};
    gm::ModelRead r = gm::ReadModel(w.m, BuildModel(w.m, spec));
    if (r.parts_ok != 1 || r.p[0].samplers.size() != 3) { Check(false, "CTF reasons: the forged part did not read"); return; }
    gm::TexResolver res;
    res.Resolve(w.m, [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }, r, 100);
    const gm::TexStats& s = res.stats();
    Check(s.why[uint32_t(live::guest_tex::Why::NoHandle)] == 1 && s.why[uint32_t(live::guest_tex::Why::Format)] == 1 && s.why[gm::kWhyNoKey] == 1 && s.missing == 3 && s.bound == 0,
          "CTF reasons: a NoHandle, a Format (game format 49) and a key 0 refusal are counted 1 / 1 / 1 with missing 3 (key 0 is its own reason)");
    const std::string sum = gm::WhySummary(s);
    Check(sum.find("no handle 1") != std::string::npos && sum.find("format not supported 1") != std::string::npos && sum.find("no key 1") != std::string::npos, "CTF reasons: the printed summary names each reason ('" + sum + "')");
    const bool probes = r.n_refused == 3 && r.refused[0].key0 == hNo && r.refused[0].why == uint8_t(live::guest_tex::Why::NoHandle) && r.refused[1].key0 == hFmt && r.refused[1].why == uint8_t(live::guest_tex::Why::Format) && r.refused[1].stored_id == hFmt &&
                        r.refused[1].vtable == 0x8206ADB8u && r.refused[1].kind == 0 && r.refused[1].header != 0 && (r.refused[1].fetch[1] & 0x3Fu) == 49u && r.refused[2].key0 == 0 && r.refused[2].why == uint8_t(gm::kWhyNoKey);
    Check(probes, "CTF reasons: the first refused samplers carry their key words, registry id, wrapper vtable / kind, header and fetch dwords");
    Check(r.tex_retry, "CTF reasons: a NoHandle miss is retryable (a late texture can appear)");
    // a Format-only model is not retryable
    PartSpec q = p0;
    q.samplers = {{"g_mMaskPrm", 1, 15}};
    ModelSpec s2;
    s2.parts = {q};
    s2.tex_keys = {hNo, hFmt};
    gm::ModelRead rf = gm::ReadModel(w.m, BuildModel(w.m, s2));
    gm::TexResolver res2;
    if (rf.parts_ok == 1) res2.Resolve(w.m, [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }, rf, 100);
    Check(rf.parts_ok == 1 && !rf.tex_retry && !gm::TexDue(rf, false, 0, 100000), "CTF reasons: a Format refusal is permanent: no retry is asked for");
  }
  // P3 retry: the registry gains the entry on call 3; the resolver is asked again at the retry period and reads Bound by call ~65, no allocation-driven resolve every frame
  {
    TexWorld w;
    const uint32_t id = w.AddTexture(0, 0x1C100000u, 9, false);
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.effect = "ych_skin";
    p0.samplers = {{"sampDiffusePrm", 0, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {id};
    gm::ModelRead r = gm::ReadModel(w.m, BuildModel(w.m, spec));
    if (r.parts_ok != 1) { Check(false, "CTF retry: the forged part did not read"); return; }
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::TexResolver res;
    uint64_t checked = 1, first_bound = 0, resolves = 1;
    res.Resolve(w.m, phys, r, 1);   // the model's first read (call 1): the texture is not registered
    const bool missing_first = r.tex_state[0][0] == kGtMissing && r.tex_retry;
    for (uint64_t f = 2; f <= 80; ++f) {
      if (f == 3) w.Register(id);
      if (gm::TexCallStep(r, false, false, checked, f)) res.Resolve(w.m, phys, r, f), ++resolves;
      if (!first_bound && r.tex_state[0][0] == kGtBound) first_bound = f;
    }
    Check(missing_first && first_bound >= 3 && first_bound <= 65 && resolves <= 3 && !r.tex_retry && r.textures.size() == 1 && *r.textures[0].bytes == w.pattern[0],
          "CTF retry: a texture registered on call 3 reads Bound at the retry period (call " + std::to_string(first_bound) + ", by 65), in " + std::to_string(resolves) + " resolves, and stops asking once bound");
  }
  // review 1: an override (composite) handle refused with the model's own key 0 prints the OVERRIDE's reason (here: pixel memory not allocated), tagged as the override handle, never "no key"
  {
    TexWorld w;
    const uint32_t hMem = w.AddTexture(0, 0x1C100000u, 5);
    w.m.put32(w.header_of[hMem & 63] + 32, 18u);   // base 0: the composite's surface is not allocated yet
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.effect = "ych_skin";
    p0.samplers = {{"sampDiffusePrm", 0, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {0};
    spec.overrides = {{"sampDiffusePrm", hMem}};
    gm::ModelRead r = gm::ReadModel(w.m, BuildModel(w.m, spec));
    if (r.parts_ok != 1 || r.p[0].samplers.size() != 1 || !r.p[0].samplers[0].has_override) { Check(false, "CTF override reason: the forged part did not read"); return; }
    gm::TexResolver res;
    res.Resolve(w.m, [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }, r, 100);
    const std::string line = r.n_refused ? gm::RefusedText(r, 0) : std::string();
    Check(r.n_refused == 1 && r.refused[0].ov && r.refused[0].key0 == hMem && r.refused[0].why == uint8_t(live::guest_tex::Why::Memory) && r.refused[0].stored_id == hMem && line.find("(override handle)") != std::string::npos &&
              line.find("pixel memory unreadable") != std::string::npos && line.find("no key") == std::string::npos && res.stats().why[gm::kWhyNoKey] == 0 && res.stats().override_refused == 1 && r.tex_retry,
          "CTF override reason: an override refused with the model key 0 prints the OVERRIDE's reason (" + line.substr(0, 90) + "...), not 'no key'; a Memory miss is retryable");
  }
  // review 3: the contributor's due / checked rule (TexCallStep, the same function Run calls): a model with an override block drawn EVERY frame is re-resolved at the 61-frame period (frames 1, 62, 123, 184)
  {
    TexWorld w;
    const uint32_t hA = w.AddTexture(0, 0x1C100000u, 21), hC = w.AddTexture(1, 0x1C110000u, 99);
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.effect = "ych_skin";
    p0.samplers = {{"sampDiffusePrm", 0, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {hA};
    spec.overrides = {{"sampDiffusePrm", hC}};
    gm::ModelRead r = gm::ReadModel(w.m, BuildModel(w.m, spec));
    if (r.parts_ok != 1) { Check(false, "CTF checked: the forged part did not read"); return; }
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::TexResolver res;
    uint64_t checked = 0, resolves = 0;
    for (uint64_t f = 1; f <= 200; ++f)
      if (gm::TexCallStep(r, true, f == 1, checked, f)) res.Resolve(w.m, phys, r, f), ++resolves;
    Check(resolves == 4 && checked == 184, "CTF checked: tex_checked moves only when the resolver ran: an override model drawn every frame resolves at frames 1, 62, 123, 184 (" + std::to_string(resolves) + " resolves)");
  }
  // review 5: a retryable miss that stays missing is counted ONCE (why / missing / samplers), however often it is re-asked
  {
    TexWorld w;
    const uint32_t id = w.AddTexture(0, 0x1C100000u, 9, false);
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.effect = "ych_skin";
    p0.samplers = {{"sampDiffusePrm", 0, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {id};
    gm::ModelRead r = gm::ReadModel(w.m, BuildModel(w.m, spec));
    if (r.parts_ok != 1) { Check(false, "CTF once: the forged part did not read"); return; }
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::TexResolver res;
    uint64_t checked = 0;
    for (uint64_t f = 1; f <= 400; ++f)
      if (gm::TexCallStep(r, false, f == 1, checked, f)) res.Resolve(w.m, phys, r, f);
    const gm::TexStats& s = res.stats();
    Check(s.retry_calls >= 5 && s.why[uint32_t(live::guest_tex::Why::NoHandle)] == 1 && s.missing == 1 && s.samplers == 1, "CTF once: a miss re-asked " + std::to_string(s.retry_calls) + " times is counted once (no handle 1, missing 1, samplers 1)");
    // a re-ask that changes nothing copies nothing: ResolveTo reports no change
    gm::TexOut scratch;
    const bool changed = res.ResolveTo(w.m, phys, r, scratch, 500);
    Check(!changed, "CTF cost: a re-ask with the same answer reports 'unchanged' (the contributor then copies nothing)");
  }
}

// ---- rows, the record, the consumer ---------------------------------------------------------------------------------------------------------------------------------------------------------
// ---- CTX2 (S103, docs\research\s103\CAWD.md sites 1-8, Q1, Q2; review fixes 1-9): linear k_8_8_8_8 pictures, the creator guard, the in-place refresh, the census delta, the override dump, the cube row state -------------------------------
// A w x h k_8_8_8_8 texture object (the fetch words the game's CAW model has: CAWD Q2 table); `f3` = fetch dword 3 (0xC14 = the BGRA identity swizzle 0x60A), `tiled` = fetch dword 0 bit 31. The bytes at `base` are the pattern.
static uint32_t Add8888(TexWorld& w, uint32_t base, uint32_t wd, uint32_t ht, uint32_t endian, uint32_t f3, bool tiled, uint8_t seed, std::vector<uint8_t>& pat) {
  pat.assign(size_t(wd) * ht * 4, 0);
  for (size_t i = 0; i < pat.size(); ++i) pat[i] = uint8_t(seed + i * 131u + (i >> 10) * 7u);
  std::memcpy(w.phys.b.data() + (base - Phys::kBase), pat.data(), pat.size());
  const uint32_t id = 0x00010000u | w.next_idx++;
  const uint32_t wrapper = w.m.alloc(128), header = w.m.alloc(64);
  w.m.put32(wrapper + 0, 0x8206ADB8u), w.m.put32(wrapper + 52, 0), w.m.put32(wrapper + 80, header);
  w.m.put32(header + 28, ((wd / 32) << 22) | (tiled ? 0x80000000u : 0u));   // pitch / 32, linear (CAWD: dword0 08000002 -> pitch 1024)
  w.m.put32(header + 32, base | (endian << 6) | 6u);                         // format 6 = k_8_8_8_8
  w.m.put32(header + 36, (wd - 1) | ((ht - 1) << 13));
  w.m.put32(header + 40, f3), w.m.put32(header + 44, 0), w.m.put32(header + 48, 1u << 9);   // dimension 1 = 2D
  w.wrapper_of[id & 63] = wrapper, w.header_of[id & 63] = header;
  w.Register(id);
  return id;
}
using gm::kCreatorGpu;
using gm::kCreatorLoader;
using gm::kCreatorMovie;
using gm::kCreatorUnknown;
// The unit's birth oracle (picsrc::PictureSource::BirthOf's shape): seen / creator / site.
static bool g_unit_seen = true;
static uint8_t g_unit_creator = kCreatorLoader;
static uint32_t g_unit_site = 0x82BAA6C0u;
static bool UnitBorn(uint32_t, uint8_t& cr, uint32_t& site) { cr = g_unit_creator; site = g_unit_seen ? g_unit_site : 0; return g_unit_seen; }
static void Born(bool seen, uint8_t cr, uint32_t site) { g_unit_seen = seen, g_unit_creator = cr, g_unit_site = site; }
static gm::ModelRead OneSamplerModel(TexWorld& w, const char* name, uint32_t key) {
  PartSpec p0;
  p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
  p0.effect = "ych_skin";
  p0.samplers = {{name, 0, 15}};
  ModelSpec spec;
  spec.parts = {p0};
  spec.tex_keys = {key};
  return gm::ReadModel(w.m, BuildModel(w.m, spec));
}
static void CheckCawd() {
  live::guest_model::ByteStore::Process().Clear();
  const uint32_t kBgra = 0xC14u;   // swizzle (0xC14 >> 1) & 0xFFF = 0x60A: selectors 2, 1, 0, 3
  Born(true, kCreatorLoader, 0x82BAA6C0u);
  // sites 1-6 + 7: a 1024 x 1024 linear BGRA picture (the CAW diffuse: exactly 4 MB) comes back byte-identical, is carried by the rows (GuestTexBytes / GuestModelTextureProblem) and maps to BGRA8
  {
    TexWorld w;
    w.phys.b.assign(0x500000, 0);
    std::vector<uint8_t> pat;
    const uint32_t id = Add8888(w, Phys::kBase, 1024, 1024, 0, kBgra, false, 17, pat);
    gm::ModelRead r = OneSamplerModel(w, "g_mDiffusePrm", id);
    if (r.parts_ok != 1) { Check(false, "CAWD: the forged part did not read"); return; }
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::TexResolver res;
    res.born_of = &UnitBorn;
    res.Resolve(w.m, phys, r, 100);
    Check(r.tex_state[0][0] == kGtBound && r.textures.size() == 1 && r.textures[0].format == 6 && r.textures[0].width == 1024 && r.textures[0].height == 1024 && r.textures[0].bytes && *r.textures[0].bytes == pat,
          "CAWD: a 1024 x 1024 linear k_8_8_8_8 picture (BGRA identity swizzle, loader-born) is bound, byte-identical (4 MB)");
    Check(res.stats().why[uint32_t(live::guest_tex::Why::Format)] == 0 && res.stats().tex8888 == 1 && r.tex_8888, "CAWD: the 8888 picture is counted (tex8888 1), not refused as format, and the read is flagged tex_8888");
    if (!r.textures.empty() && r.textures[0].bytes) {
      Check(rdr_scene::GuestTexBytes(1024, 1024, 6) == (4u << 20) && rdr_scene::GuestTexBytes(512, 1024, 6) == (2u << 20) && rdr_scene::GuestTexBytes(64, 64, 18) == 2048, "CAWD: GuestTexBytes: format 6 = w * h * 4, BC1 unchanged");
      SceneTables t;
      gm::BlobSender blobs;
      gm::AppendModelRows(r, t.guest_models, blobs, 1000);
      Check(t.guest_models.textures.size() == 1 && *rdr_scene::GuestModelTextureProblem(t.guest_models.textures[0], t.guest_models.blobs.size()) == 0 && t.guest_models.textures[0].nbytes == (4u << 20),
            "CAWD: the row writer carries the 4 MB picture and GuestModelTextureProblem accepts it (limit is `>`)");
      GuestModelTexture bad = t.guest_models.textures[0];
      bad.nbytes += 4;
      Check(*rdr_scene::GuestModelTextureProblem(bad, t.guest_models.blobs.size()) != 0, "CAWD: a wrong 8888 byte count is refused by the row check");
    }
    Check(guestmodel::TexFormatOf(6) == assets::TexFormat::kB8G8R8A8 && guestmodel::TexFormatOf(18) == assets::TexFormat::kBC1 && guestmodel::TexFormatOf(20) == assets::TexFormat::kBC3, "CAWD site 5: TexFormatOf(6) = kB8G8R8A8, the BC formats unchanged");
  }
  // endian 2 (8in32: the CPU wrote the dword big-endian): a known colour A = 0x80, R = 0x11, G = 0x22, B = 0x33 (memory bytes 80 11 22 33) comes out as host bytes B, G, R, A = 33 22 11 80 (review 9)
  {
    TexWorld w;
    std::vector<uint8_t> pat;
    const uint32_t id = Add8888(w, Phys::kBase, 64, 64, 2, kBgra, false, 5, pat);
    const uint8_t px[4] = {0x80, 0x11, 0x22, 0x33};
    std::memcpy(w.phys.b.data(), px, 4);
    gm::ModelRead r = OneSamplerModel(w, "g_mDiffusePrm", id);
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::TexResolver res;
    res.born_of = &UnitBorn;
    res.Resolve(w.m, phys, r, 100);
    const bool ok = r.textures.size() == 1 && r.textures[0].bytes && r.textures[0].bytes->size() == pat.size();
    Check(ok && (*r.textures[0].bytes)[0] == 0x33 && (*r.textures[0].bytes)[1] == 0x22 && (*r.textures[0].bytes)[2] == 0x11 && (*r.textures[0].bytes)[3] == 0x80,
          "CAWD review 9: endian 2: the pixel (A 80, R 11, G 22, B 33) is read as host bytes B, G, R, A = 33 22 11 80 (the BGRA8 upload's order)");
    bool all = ok;
    for (size_t i = 4; all && i + 4 <= pat.size(); i += 4)
      for (size_t k = 0; k < 4; ++k) all = all && (*r.textures[0].bytes)[i + k] == pat[i + 3 - k];
    Check(all, "CAWD: endian 2 swaps every dword of the picture");
  }
  // a non-BGRA swizzle is refused as Format, tiled 8888 is refused as Format (never a swapped / untiled-wrong picture)
  {
    TexWorld w;
    std::vector<uint8_t> pat;
    const uint32_t idS = Add8888(w, Phys::kBase, 64, 64, 0, 0u, false, 1, pat);              // swizzle 0: X, X, X, X: not the BGRA identity
    const uint32_t idT = Add8888(w, Phys::kBase + 0x4000, 64, 64, 0, kBgra, true, 2, pat);   // tiled
    gm::TexResolver res;
    res.born_of = &UnitBorn;
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::ModelRead rs = OneSamplerModel(w, "g_mDiffusePrm", idS);
    res.Resolve(w.m, phys, rs, 100);
    gm::ModelRead rt = OneSamplerModel(w, "g_mDiffusePrm", idT);
    res.Resolve(w.m, phys, rt, 101);
    Check(rs.tex_state[0][0] == kGtMissing && rt.tex_state[0][0] == kGtMissing && res.stats().why[uint32_t(live::guest_tex::Why::Format)] == 2 && res.stats().tex8888 == 0,
          "CAWD: a non-BGRA swizzle and a tiled 8888 are both refused as Format (2), nothing read");
  }
  // site 8 (review 1): the creator guard is A4's: only a LOADER-born 8888 picture is read. Gpu, Movie, a site nobody classified (the CEditEx composite's 0x83418F50) and "never seen born" are each refused, the note names the site
  {
    TexWorld w;
    std::vector<uint8_t> pat;
    const uint32_t id = Add8888(w, Phys::kBase, 64, 64, 0, kBgra, false, 3, pat);
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    struct Case { bool seen; uint8_t cr; uint32_t site; uint32_t why; const char* note; const char* name; } cases[] = {
        {true, kCreatorGpu, 0x827E8D64u, gm::kWhyGpuCreator, "born at site 827E8D64", "gpu"},
        {true, kCreatorMovie, 0x827FBB84u, gm::kWhyGpuCreator, "movie", "movie"},
        {true, kCreatorUnknown, 0x83418F50u, gm::kWhyUnclassified, "born at site 83418F50", "an unclassified site (the CEditEx composite's birth site)"},
        {false, kCreatorUnknown, 0u, gm::kWhyUnclassified, "never seen born", "never seen born"}};
    for (const Case& c : cases) {
      gm::TexResolver res;
      res.born_of = &UnitBorn;
      Born(c.seen, c.cr, c.site);
      gm::ModelRead r = OneSamplerModel(w, "g_mDiffusePrm", id);
      res.Resolve(w.m, phys, r, 100);
      const std::vector<std::string> notes = res.TakeNotes();
      Check(r.tex_state[0][0] == kGtMissing && res.stats().why[c.why] == 1 && res.stats().byte_reads == 0 && !r.tex_8888 && notes.size() == 1 && notes[0].find(c.note) != std::string::npos && notes[0].find("refused") != std::string::npos,
            std::string("CAWD review 1: a 8888 picture born ") + c.name + " is refused (nothing read) and the note names it");
    }
    // a loader-born one is read; the same handle seen first at an unclassified site and later as a loader is logged again (the verdict changed)
    gm::TexResolver res;
    res.born_of = &UnitBorn;
    Born(true, kCreatorUnknown, 0x83418F50u);
    gm::ModelRead ra = OneSamplerModel(w, "g_mDiffusePrm", id);
    res.Resolve(w.m, phys, ra, 100);
    res.Resolve(w.m, phys, ra, 101);   // same verdict: no second note
    Born(true, kCreatorLoader, 0x82BAA6C0u);
    gm::ModelRead rb = OneSamplerModel(w, "g_mDiffusePrm", id);
    res.Resolve(w.m, phys, rb, 102);
    res.Resolve(w.m, phys, rb, 103);
    const std::vector<std::string> n3 = res.TakeNotes();
    Check(rb.tex_state[0][0] == kGtBound && n3.size() == 2 && n3[0].find("refused") != std::string::npos && n3[1].find("read as bytes") != std::string::npos && n3[1].find("creator loader") != std::string::npos && res.stats().tex8888 == 1,
          "CAWD review 8: a handle whose verdict changes (unclassified -> loader) is logged again; an unchanged verdict is not");
  }
  // review 2: a loader-born 8888 page the editor rewrites in place is not frozen: the resolver is asked again every 61 frames, a sampled guard says "same" (no re-read) or "changed" (one full read, a new hash)
  {
    TexWorld w;
    std::vector<uint8_t> pat;
    const uint32_t id = Add8888(w, Phys::kBase, 64, 64, 0, kBgra, false, 9, pat);
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::TexResolver res;
    res.born_of = &UnitBorn;
    Born(true, kCreatorLoader, 0x82BAA6C0u);
    gm::ModelRead r = OneSamplerModel(w, "g_mDiffusePrm", id);
    res.Resolve(w.m, phys, r, 100);
    const uint64_t h0 = r.tex_hash[0][0];
    uint64_t checked = 100;
    gm::TexOut scr;
    const auto step = [&](uint64_t f) {
      bool changed = false;
      if (gm::TexCallStep(r, false, false, checked, f) && res.ResolveTo(w.m, phys, r, scr, f)) changed = true, gm::ApplyTex(r, std::move(scr));
      return changed;
    };
    bool early = false;
    for (uint64_t f = 101; f <= 161; ++f) early = step(f) || early;
    Check(r.tex_8888 && !early && r.tex_hash[0][0] == h0 && res.stats().byte_reads == 1 && res.stats().tex8888_guard_same == 1, "CAWD review 2: untouched 8888 page: the 61-frame check runs the guard only (guard unchanged 1, still 1 byte read, same hash)");
    for (size_t i = 0; i < pat.size(); ++i) w.phys.b[i] ^= 0x5A;   // the editor rewrites the picture in place
    uint64_t new_at = 0;
    for (uint64_t f = 162; f <= 230 && !new_at; ++f)
      if (step(f)) new_at = f;
    Check(new_at == 222 && r.tex_hash[0][0] != h0 && res.stats().byte_reads == 2 && res.stats().tex8888_rereads == 1 && r.textures.size() == 1 && (*r.textures[0].bytes)[0] == uint8_t(pat[0] ^ 0x5A),
          "CAWD review 2: the page rewritten in place is seen 61 frames after the last check (frame 222): the guard changed, one full re-read, a new hash and the new bytes");
  }
  // review 5: an 8888 picture is up to 4 MB: one full read a call (3 pictures finish on the third call, never 3 in one frame)
  {
    TexWorld w;
    std::vector<uint8_t> pat;
    const uint32_t i0 = Add8888(w, Phys::kBase, 64, 64, 0, kBgra, false, 1, pat), i1 = Add8888(w, Phys::kBase + 0x4000, 64, 64, 0, kBgra, false, 2, pat), i2 = Add8888(w, Phys::kBase + 0x8000, 64, 64, 0, kBgra, false, 3, pat);
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.effect = "ych_skin";
    p0.samplers = {{"g_mDiffusePrm", 0, 15}, {"g_mNormalFxPrm", 1, 15}, {"g_mMaskPrm", 2, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {i0, i1, i2};
    gm::ModelRead r = gm::ReadModel(w.m, BuildModel(w.m, spec));
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    gm::TexResolver res;
    res.born_of = &UnitBorn;
    res.Resolve(w.m, phys, r, 100, 8);   // a budget of 8 plain reads would allow all three: the 8888 cap is one
    const bool one = r.textures.size() == 1 && r.tex_incomplete;
    res.Resolve(w.m, phys, r, 101, 8);
    res.Resolve(w.m, phys, r, 102, 8);
    Check(one && r.textures.size() == 3 && !r.tex_incomplete && r.tex_state[0][0] == kGtBound && r.tex_state[0][1] == kGtBound && r.tex_state[0][2] == kGtBound, "CAWD review 5: three 8888 pictures read one a call (incomplete until the third call)");
  }
  // kind 1 / review 4: a cube is recognised by the texture object, not by name: the wrapper's mode word picks the header (+84), its fetch dword 5 dimension is 3 -> row state kGtEngineDefault, counted engine_default_cube, probed (NoteRefused), not missing
  {
    TexWorld w;
    const uint32_t id = w.AddTexture(0, 0x1C100000u, 9);
    const uint32_t cubeh = w.m.alloc(64);
    w.m.put32(w.wrapper_of[id & 63] + 52, 1), w.m.put32(w.wrapper_of[id & 63] + 84, cubeh), w.m.put32(cubeh + 48, 3u << 9);   // kind 1, header at +84, dimension 3 (cube)
    const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
    for (const char* nm : {"g_mSpecPowPrm", "g_mDiffusePrm"}) {   // by the object's dimension, whatever the sampler is called
      gm::ModelRead r = OneSamplerModel(w, nm, id);
      gm::TexResolver res;
      res.Resolve(w.m, phys, r, 100);
      Check(r.tex_state[0][0] == kGtEngineDefault && r.tex_hash[0][0] == 0 && res.stats().engine_default_cube == 1 && res.stats().missing == 0 && gm::WhySummary(res.stats()) == "none" && !r.tex_retry && r.n_refused == 1 && r.refused[0].why == gm::kWhyEngineCube && r.refused[0].header == cubeh,
            std::string("CAWD review 4: a kind-1 wrapper whose +84 header says dimension 3 (") + nm + ") is kGtEngineDefault: engine_default_cube 1, not missing, no refusal reason, probed with its +84 header");
    }
    // dimension 1 (2D) at +84 is NOT a cube: still a refusal (not plain 2D)
    w.m.put32(cubeh + 48, 1u << 9);
    gm::ModelRead r2 = OneSamplerModel(w, "g_mSpecPowPrm", id);
    gm::TexResolver res2;
    res2.Resolve(w.m, phys, r2, 100);
    Check(r2.tex_state[0][0] == kGtMissing && res2.stats().engine_default_cube == 0 && res2.stats().missing == 1 && res2.stats().why[uint32_t(live::guest_tex::Why::NotPlain2D)] == 1, "CAWD review 4: a kind-1 wrapper that is not a cube (dimension 1) stays a refusal, even for g_mSpecPowPrm");
    // the draw side counts by the ROW's state: a cube row = tex_default_cube, an unresolved diffuse = tex_missing
    w.m.put32(cubeh + 48, 3u << 9);
    PartSpec p;
    p.F = 0x0C | 0x10 | 0x40 | 0x100;
    p.vcount = 4, p.idx = {0, 1, 2, 3, 3, 2, 1, 0}, p.n0 = 4, p.n1 = 4, p.palette = {20, 30, 40, 50}, p.effect = "ych_skin";
    Vtx vx;
    vx.pos[0] = 2.f, vx.w[0] = 0.5f, vx.s[0] = 0, vx.n[2] = 1.f, vx.uv[0][0] = 0.25f, vx.uv[0][1] = 0.75f;
    for (int i = 0; i < 4; ++i) { const std::vector<uint32_t> d = Pack(p.F, vx); p.host_dwords.insert(p.host_dwords.end(), d.begin(), d.end()); }
    p.samplers = {{"g_mSpecPowPrm", 0, 15}, {"g_mDiffusePrm", 1, 15}};
    ModelSpec spec;
    spec.parts = {p};
    spec.two_ranges = true;
    spec.tex_keys = {id, 0x00010777u};
    gm::ModelRead rr = gm::ReadModel(w.m, BuildModel(w.m, spec));
    gm::TexResolver res3;
    res3.Resolve(w.m, phys, rr, 100);
    SceneTables t;
    gm::BlobSender blobs;
    gm::AppendModelRows(rr, t.guest_models, blobs, 1000);
    Check(t.guest_models.parts.size() == 1 && t.guest_models.parts[0].tex[0].state == kGtEngineDefault && *rdr_scene::GuestModelRowProblem(t.guest_models.parts[0], t.guest_models.blobs.size()) == 0, "CAWD review 4: the row carries state kGtEngineDefault and the row check accepts it");
    guestmodel::Store store;
    store.Absorb(t);
    assets::Model mdl;
    guestmodel::BuildStats bs;
    const bool built = guestmodel::BuildModel(t, store, rr.model, mdl, bs);
    Check(built && bs.tex_default_cube == 1 && bs.tex_missing == 1, "CAWD review 4 (draw): the cube row counts tex_default_cube 1, the unresolved diffuse tex_missing 1");
    // the flat plant is a parameter, not a global (review 7): the same body with flat = true gets the magenta colour on its diffuse-less material
    assets::Model m2;
    guestmodel::BuildStats bs2;
    guestmodel::BuildModel(t, store, rr.model, m2, bs2, true);
    Check(!mdl.materials.empty() && !m2.materials.empty() && mdl.materials[0].diffuse_color[1] == 1.f && m2.materials[0].diffuse_color[0] == 1.f && m2.materials[0].diffuse_color[1] == 0.f && m2.materials[0].diffuse_color[2] == 1.f,
          "CAWD review 7: BuildModel(..., flat = true) gives the diffuse-less material magenta; the default call leaves it white");
  }
  // Q1: the census is re-printed per window with the DELTA, and the delta is real (`actors drawn` read at the end of a window, not at the first model build)
  {
    guestmodel::DrawTotals prev;
    ++guestmodel::Counters().skel_holes;   // review 3: a stock wrestler's own-skeleton piece hole alone must not start the CAW lines
    const std::string quiet = guestmodel::WindowCensusLine(prev, 1, 600);
    uint64_t drawn_a = 0;
    for (uint64_t f = 10; f < 13; ++f) guestmodel::NoteActorDrawn(f), ++drawn_a;   // 3 actors drawn in window A (what [rf][wrestlers] drawn counts for unnamed actors)
    ++guestmodel::Counters().models_built;
    const std::string a = guestmodel::WindowCensusLine(prev, 601, 1200);
    uint64_t drawn_b = 0;
    guestmodel::NoteActorDrawn(1300), ++drawn_b;
    ++guestmodel::Counters().skel_holes;
    const std::string b = guestmodel::WindowCensusLine(prev, 1201, 1800);
    const std::string c = guestmodel::WindowCensusLine(prev, 1801, 2400);   // nothing drawn: +0
    Check(quiet.empty(), "Q1 / review 3: no CAW counter moved (only a skel hole): no window line");
    Check(a.find("actors drawn +" + std::to_string(drawn_a) + ",") != std::string::npos && a.find("last draw frame 12") != std::string::npos && a.find("frames 601-1200") != std::string::npos && a.find("models built +1") != std::string::npos,
          "Q1: window A line carries the delta (actors drawn +3) and the last draw frame, equal to what the draw path counted");
    Check(b.find("actors drawn +" + std::to_string(drawn_b) + ",") != std::string::npos && b.find("own-skeleton piece holes +1") != std::string::npos && b.find("last draw frame 1300") != std::string::npos && b.find("models built +0") != std::string::npos,
          "Q1: window B is a delta (+1, skel hole +1, models built +0), not the running total");
    Check(c.find("actors drawn +0,") != std::string::npos && c.find("actors drawn from them 4 (last at frame 1300") != std::string::npos, "Q1: an idle window reads +0 and still carries the totals (4) and the last draw frame");
    Check(rdr_scene::live::drawcam::Cam().LastBodyDraw() == 1300 && rdr_scene::live::drawcam::Cam().CensusText().find(" last_body_draw 1300") != std::string::npos, "S103 CAWB (CAWV Q3): the draw-camera census carries last_body_draw, the frame NoteActorDrawn last counted");
  }
  // Q2: the override block's entry names and first handle word are dumped (a name that differs only in case is counted apart, not matched)
  {
    TexWorld w;
    const uint32_t hA = w.AddTexture(0, 0x1C100000u, 11), hC = w.AddTexture(1, 0x1C110000u, 77);
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.samplers = {{"sampDiffusePrm", 0, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {hA};
    spec.overrides = {{"SAMPDIFFUSEPRM", hC}, {"g_mOther", hA}};
    gm::ModelRead r = gm::ReadModel(w.m, BuildModel(w.m, spec));
    Check(r.parts_ok == 1 && r.ov_entries == 2 && r.ov_matched == 0 && r.ov_matched_ci == 1 && r.ov_dump_n == 2 && std::string(r.ov_names[0]) == "SAMPDIFFUSEPRM" && std::string(r.ov_names[1]) == "g_mOther" && r.ov_handle0[0] == hC && r.ov_handle0[1] == hA,
          "Q2: the override entries' names + first handle words are dumped; 0 matched by name, 1 equal only ignoring case");
  }
}

// entry: rdr_scene::live::guest_model::ReadModel (the writer's order: ReadModel -> TexResolver::Resolve -> AppendModelRows) and guestmodel::BuildModel (the consumer); the dirty table is entered by NoteFlush, as the sub_82CA6BE0 hook calls it (lint L12 keeps that call in the hook)
// ---- S105 A5c: the CAW logo pictures + the flush rule (docs/research/s105/A5c.md; entry: gm::ReadModel -> TexResolver::Resolve -> AppendModelRows -> guestmodel::BuildModel, the writer / consumer order of the live run) ----
// A forged CAW model: [m+324] = the callback sub_822E6CC8, [m+328] = the editor object E, [E+2608] = the composer, [composer+100] = the buffer index, [composer+116+4*buf] = the logo set, [set+4] = two 8-byte keys.
struct LogoWorld {
  TexWorld w;
  std::vector<uint8_t> pat7, pat4;
  uint32_t id7 = 0, id4 = 0, idD = 0, E = 0, comp = 0, model = 0;
  uint32_t hdr7 = 0;
  static constexpr uint32_t kBgra = 0xC14u;
  explicit LogoWorld(bool callback, uint32_t buf = 0, int32_t diffuse_t = 0, bool with_logo_keys = true) {
    w.phys.b.assign(0x200000, 0);
    Born(true, kCreatorLoader, 0x82BAA6C0u);
    id7 = Add8888(w, Phys::kBase, 256, 256, 0, kBgra, false, 31, pat7);
    id4 = Add8888(w, Phys::kBase + 0x40000u, 256, 256, 0, kBgra, false, 47, pat4);
    idD = w.AddTexture(0, Phys::kBase + 0x80000u, 9);
    hdr7 = w.header_of[id7 & 63];
    PartSpec p0;
    p0.vcount = 4, p0.host_dwords = Dwords(4, 8, 1), p0.idx = {0, 1, 2, 3}, p0.n0 = 4, p0.palette = {1, 2};
    p0.effect = "ych_skin";
    p0.samplers = {{"g_mDiffusePrm", diffuse_t, 15}};
    ModelSpec spec;
    spec.parts = {p0};
    spec.tex_keys = {idD, idD};
    model = BuildModel(w.m, spec);
    E = w.m.alloc(4096), comp = w.m.alloc(256);
    const uint32_t set0 = w.m.alloc(16), set1 = w.m.alloc(16), arr0 = w.m.alloc(32), arr1 = w.m.alloc(32);
    w.m.put32(arr0, with_logo_keys ? id7 : 0), w.m.put32(arr0 + 4, 0x11), w.m.put32(arr0 + 8, with_logo_keys ? id4 : 0), w.m.put32(arr0 + 12, 0x22);
    w.m.put32(arr1, 0), w.m.put32(arr1 + 4, 0), w.m.put32(arr1 + 8, 0), w.m.put32(arr1 + 12, 0);   // the other buffer's set: empty keys
    w.m.put32(set0 + 4, arr0), w.m.put32(set1 + 4, arr1);
    w.m.put32(comp + 100, buf), w.m.put32(comp + 116, set0), w.m.put32(comp + 120, set1), w.m.put32(comp + 124, set0);   // (+124: what an unbounded buf = 2 would read)
    w.m.put32(E + 2608, comp);
    w.m.put32(model + 324, callback ? gm::kLogoCb : 0u), w.m.put32(model + 328, E);
  }
  gm::ModelRead Read() { return gm::ReadModel(w.m, model); }
  const auto Phys_() const { return [this](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }; }
  // resolve until nothing is deferred (an 8888 picture is one full read a call)
  void Settle(gm::TexResolver& res, gm::ModelRead& r, uint64_t frame) {
    res.born_of = &UnitBorn;
    for (int i = 0; i < 8; ++i) {
      res.Resolve(w.m, Phys_(), r, frame);
      if (!r.tex_incomplete) break;
    }
  }
};
static const std::vector<uint8_t>* PicOf(const gm::ModelRead& r, uint64_t hash) {
  for (const gm::TexRead& t : r.textures)
    if (t.hash == hash && t.bytes) return t.bytes.get();
  return nullptr;
}

static void CheckLogo() {
  live::guest_model::ByteStore::Process().Clear();
  Born(true, kCreatorLoader, 0x82BAA6C0u);
  live::guest_tex_dirty::Seq();
  // (a) a forged CAW model WITH the callback: both logo keys are read, resolved to the two 1024-class (here 256) linear BGRA pictures, carried by the rows, and built onto the material at slots 7 (entry 0) / 4 (entry 1)
  {
    LogoWorld lw(true);
    gm::ModelRead r = lw.Read();
    Check(r.parts_ok == 1 && r.logo.has && r.logo.key[0] == lw.id7 && r.logo.key[1] == lw.id4 && r.logo.key1[0] == 0x11 && r.logo.key1[1] == 0x22, "A5c (a): the model-level logo words are read ([m+324] = the callback): both keys, entry 0 then entry 1");
    gm::TexResolver res;
    lw.Settle(res, r, 100);
    const std::vector<uint8_t>* b7 = PicOf(r, r.logo_hash[0]);
    const std::vector<uint8_t>* b4 = PicOf(r, r.logo_hash[1]);
    Check(r.logo_state[0] == kGtBound && r.logo_state[1] == kGtBound && r.logo_hash[0] && r.logo_hash[1] && r.logo_hash[0] != r.logo_hash[1] && b7 && b4 && *b7 == lw.pat7 && *b4 == lw.pat4 && res.stats().logo_bound >= 2 && res.stats().logo_missing == 0,
          "A5c (a): both logo keys are resolved through the 8888 path (byte-identical pictures), counted logo_bound (per resolver call that answered it)");
    GuestModelTables g;
    gm::BlobSender bs;
    const uint32_t added = gm::AppendModelRows(r, g, bs, 100);
    const bool rows_ok = added == 1 && g.parts.size() == 1 && g.parts[0].logo7_tex && g.parts[0].logo4_tex && g.parts[0].logo7_tex <= g.textures.size() && g.parts[0].logo4_tex <= g.textures.size() &&
                         g.textures[g.parts[0].logo7_tex - 1].hash == r.logo_hash[0] && g.textures[g.parts[0].logo4_tex - 1].hash == r.logo_hash[1] && (g.parts[0].flags & kGmDiffuseT0);
    Check(rows_ok, "A5c (a): the part row carries the logo pictures' texture-row indices (+1) and kGmDiffuseT0");
    SceneTables t;
    t.guest_models = g;
    guestmodel::Store logo_store;
    logo_store.Absorb(t);
    assets::Model lmdl;
    guestmodel::BuildStats st;
    const bool built = guestmodel::BuildModel(t, logo_store, lw.model, lmdl, st);
    bool mat_ok = built && lmdl.meshes.size() == 1 && lmdl.meshes[0].material >= 0;
    if (mat_ok) {
      const assets::Material& mt = lmdl.materials[size_t(lmdl.meshes[0].material)];
      mat_ok = mt.logo7 >= 0 && mt.logo4 >= 0 && mt.logo7 != mt.logo4 && mt.diffuse_t0 && lmdl.textures[size_t(mt.logo7)].data == lw.pat7 && lmdl.textures[size_t(mt.logo4)].data == lw.pat4 &&
               lmdl.textures[size_t(mt.logo7)].format == assets::TexFormat::kB8G8R8A8;
    }
    Check(mat_ok && st.logo_parts == 2 && st.logo_missing == 0, "A5c (a): the consumer builds the logo pictures into the material: logo7 = entry 0's bytes, logo4 = entry 1's bytes, as 8888");
    // the census / log lines the game check reads
    Check(guestmodel::CensusLine().find("parts with a logo picture") != std::string::npos, "A5c: the guest draw census line names the logo parts");
  }
  // (b) a model WITHOUT the callback binds black as today: no logo words, rows without logo indices, a material without logo pictures; a twin word (cb = 0 but the callback-shaped arguments are present) must not leak
  {
    LogoWorld lw(false);
    gm::ModelRead r = lw.Read();
    gm::TexResolver res;
    lw.Settle(res, r, 100);
    GuestModelTables g;
    gm::BlobSender bs;
    gm::AppendModelRows(r, g, bs, 100);
    SceneTables t;
    t.guest_models = g;
    guestmodel::Store logo_store;
    logo_store.Absorb(t);
    assets::Model lmdl;
    guestmodel::BuildStats st;
    const bool built = guestmodel::BuildModel(t, logo_store, lw.model, lmdl, st);
    const bool black = built && !r.logo.has && r.logo_state[0] == kGtMissing && r.logo_state[1] == kGtMissing && g.parts.size() == 1 && !g.parts[0].logo7_tex && !g.parts[0].logo4_tex &&
                       lmdl.materials[size_t(lmdl.meshes[0].material)].logo7 < 0 && lmdl.materials[size_t(lmdl.meshes[0].material)].logo4 < 0 && st.logo_parts == 0;
    Check(black, "A5c (b): a model whose [m+324] is not the logo callback reads no logo words and binds black (no logo pictures on the material)");
    LogoWorld b2(true, 2);   // buf 2: past the two sets
    gm::ModelRead rb = b2.Read();
    Check(!rb.logo.has, "A5c (b): a composer buffer index >= 2 is refused (not followed)");
    LogoWorld z(true, 0, 0, false);   // both keys 0
    gm::ModelRead rz = z.Read();
    Check(!rz.logo.has, "A5c (b): a logo set whose keys are 0 is refused");
  }
  // (c) bool 150 follows kGmDiffuseT0: the part's diffuse sampler is texture index 0 -> the flag -> the material -> bit 22 of Bools word 4 (Bools[1].x); index 1 -> no flag, the bit stays off
  {
    for (int t_idx = 0; t_idx < 2; ++t_idx) {
      LogoWorld lw(true, 0, t_idx);
      gm::ModelRead r = lw.Read();
      gm::TexResolver res;
      lw.Settle(res, r, 100);
      GuestModelTables g;
      gm::BlobSender bs;
      gm::AppendModelRows(r, g, bs, 100);
      SceneTables t;
      t.guest_models = g;
      guestmodel::Store logo_store;
      logo_store.Absorb(t);
      assets::Model lmdl;
      guestmodel::BuildStats st;
      const bool built = guestmodel::BuildModel(t, logo_store, lw.model, lmdl, st);
      const bool flag = g.parts.size() == 1 && (g.parts[0].flags & kGmDiffuseT0) != 0;
      const bool mat_flag = built && lmdl.materials[size_t(lmdl.meshes[0].material)].diffuse_t0;
      float bools[8] = {};
      const uint32_t base_bits = 0xEu;
      std::memcpy(bools, &base_bits, 4);
      SetLogoBool(bools, 2, mat_flag);
      uint32_t w0, w4;
      std::memcpy(&w0, bools, 4), std::memcpy(&w4, bools + 4, 4);
      const bool bit150 = (w4 & (1u << 22)) != 0;
      Check(flag == (t_idx == 0) && mat_flag == (t_idx == 0) && bit150 == (t_idx == 0) && w0 == 0xEu && (w4 & ~(1u << 22)) == 0,
            std::string("A5c (c): bit 150 of Bools = the part's kGmDiffuseT0 (diffuse sampler texture index ") + std::to_string(t_idx) + "): the vertex bits 1-3 stay, no other bit moves");
    }
  }
  // (d) a dirty (flushed) texture is refreshed at once, for the logo's 8888 picture (a small edit the 64-texel guard misses) AND a BC picture (never re-read before): the game's unlock flush marks the header
  {
    LogoWorld lw(true);
    gm::ModelRead r = lw.Read();
    gm::TexResolver res;
    lw.Settle(res, r, 100);
    if (r.parts_ok != 1 || r.tex_hash.empty() || r.tex_hash[0].empty()) { Check(false, "A5c (d): the forged CAW part did not read"); return; }
    const uint64_t h7 = r.logo_hash[0], hD = r.tex_hash[0][0];
    // an edit of ONE texel the sampled guard cannot see (the guard's texels are fixed: x = (37 i + 11 (i >> 3)) % w, y = (61 i + 7 (i / 4)) % h)
    uint32_t ex = 0, ey = 0;
    for (uint32_t cand = 1; cand < 200; ++cand) {
      bool hit = false;
      for (uint32_t i = 0; i < 64; ++i)
        if ((i * 37u + (i >> 3) * 11u) % 256u == cand && (i * 61u + 7u * (i / 4u)) % 256u == cand + 1) hit = true;
      if (!hit) { ex = cand, ey = cand + 1; break; }
    }
    lw.w.phys.b[size_t(ey * 256 + ex) * 4] ^= 0x5A;
    lw.w.phys.b[0x80000] ^= 0xA5;   // the BC diffuse's first block
    const auto frame_run = [&](uint64_t f) { gm::ModelRead probe = r; gm::TexOut o; const bool ch = res.ResolveTo(lw.w.m, lw.Phys_(), probe, o, f); return std::make_pair(ch, o); };
    const auto before = frame_run(110);   // no flush yet, inside the 61-frame period: the held pictures stand
    Check(!before.first && before.second.logo_hash[0] == h7 && before.second.tex_hash[0][0] == hD, "A5c (d): without a flush the held pictures stand (inside the refresh period)");
    uint64_t seen = live::guest_tex_dirty::Seq(), checked = 100;
    const bool due_before = gm::TexCallStep(r, false, false, checked, 111, &seen);
    live::guest_tex_dirty::NoteFlush(lw.hdr7);                             // the composer's copy ends in sub_82CA6BE0(texture)
    live::guest_tex_dirty::NoteFlush(lw.w.header_of[lw.idD & 63]);         // the match composer's DXT pack
    const bool due_after = gm::TexCallStep(r, false, false, checked, 112, &seen);
    Check(!due_before && due_after, "A5c (d): a flush of a watched picture makes the resolver due at once (TexCallStep), and only once per flush batch");
    gm::TexOut o;
    const bool changed = res.ResolveTo(lw.w.m, lw.Phys_(), r, o, 112);
    gm::ModelRead probe = r;
    gm::ApplyTex(probe, std::move(o));
    const std::vector<uint8_t>* nb = PicOf(probe, probe.logo_hash[0]);
    Check(changed && probe.logo_hash[0] != h7 && nb && (*nb)[size_t(ey * 256 + ex) * 4] == uint8_t(lw.pat7[size_t(ey * 256 + ex) * 4] ^ 0x5A) && res.stats().dirty_rereads >= 1,
          "A5c (d): the flushed 8888 logo is re-read at once (a one-texel edit the guard cannot see), the new bytes are carried");
    Check(probe.tex_hash[0][0] != hD && res.stats().dirty_rereads >= 2, "A5c (d): a flushed BC picture (the match composer's DXT pack) is re-read at once too (it never was)");
    const bool again = gm::TexCallStep(probe, false, false, checked, 113, &seen);
    Check(!again, "A5c (d): the read stored the flush generation: no new flush, no new run");
    // the rebuilt model key changes (a new picture = a new model)
    GuestModelTables g1, g2;
    gm::BlobSender b1, b2;
    gm::AppendModelRows(r, g1, b1, 112);
    gm::AppendModelRows(probe, g2, b2, 112);
    Check(g1.parts.size() == 1 && g2.parts.size() == 1 && g1.parts[0].model_hash != g2.parts[0].model_hash, "A5c (d): the new logo picture is a new model key (the consumer rebuilds)");
  }
}

static gm::ModelRead BodyWithTextures(TexWorld& w, uint32_t& model_out) {
  const uint32_t hA = w.AddTexture(0, 0x1C100000u, 21), hC = w.AddTexture(1, 0x1C110000u, 77);
  PartSpec p0;
  p0.F = 0x0C | 0x10 | 0x40 | 0x100;
  p0.vcount = 4, p0.idx = {0, 1, 2, 3, 3, 2, 1, 0}, p0.n0 = 4, p0.n1 = 4, p0.palette = {20, 30, 40, 50}, p0.effect = "yCh_skin_flex";
  Vtx vx;
  vx.pos[0] = 2.f, vx.w[0] = 0.5f, vx.w[1] = 0.25f, vx.w[2] = 0.125f, vx.s[0] = 0, vx.s[1] = 1, vx.s[2] = 2, vx.s[3] = 3, vx.n[2] = 1.f, vx.uv[0][0] = 0.25f, vx.uv[0][1] = 0.75f;
  for (int i = 0; i < 4; ++i) { const std::vector<uint32_t> d = Pack(p0.F, vx); p0.host_dwords.insert(p0.host_dwords.end(), d.begin(), d.end()); }
  p0.samplers = {{"sampDiffusePrm", 0, 15}, {"sampNormalPrm", 1, 15}};
  PartSpec p1 = p0;
  p1.effect = "notAnEffect";
  p1.samplers = {{"sampDiffusePrm", 0, 15}};
  ModelSpec spec;
  spec.parts = {p0, p1};
  spec.two_ranges = true;
  spec.tex_keys = {hA, hC};
  model_out = BuildModel(w.m, spec);
  gm::ModelRead r = gm::ReadModel(w.m, model_out);
  gm::TexResolver res;
  res.Resolve(w.m, [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }, r, 10);
  return r;
}
// CAWH: the guest model is built in VIEWER space. A 2-bone file-space body (y -190 .. 0, bone 1 at y -150) with bone 1 rotated 30 degrees; the palette is the live one (inv_bind x world in file space, mirrored S.M.S as
// rf_wrestlers does); the built vertices are skinned on the CPU (front_end's rule: p * palette[bone], weighted) and must equal the viewer-space image (y negated) of the file-space skinning.
static void CheckSpace() {
  const float kPi = 3.14159265f, a = 30.f * kPi / 180.f;
  assets::Matrix bind1 = assets::Identity(), rz = assets::Identity();
  bind1[13] = -150.f;
  rz[0] = std::cos(a), rz[1] = std::sin(a), rz[4] = -std::sin(a), rz[5] = std::cos(a);
  const assets::Matrix pf[2] = {assets::Identity(), assets::Mul(assets::InverseRigid(bind1), assets::Mul(rz, bind1))};   // file-space palette: inv_bind x posed world
  const assets::Matrix pv[2] = {assets::MirrorY(pf[0]), assets::MirrorY(pf[1])};                                         // what rf_wrestlers hands the front end
  const float P[3][3] = {{0.f, -190.f, 0.f}, {10.f, -100.f, 5.f}, {-5.f, -2.f, 0.f}};
  const float W0[3] = {0.5f, 0.25f, 1.0f};   // weight of bone 0; bone 1 takes the rest
  PartSpec part;
  part.F = 0x08 | 0x10 | 0x40 | 0x100, part.vcount = 3, part.idx = {0, 1, 2}, part.n0 = 3, part.palette = {1, 2};
  for (int i = 0; i < 3; ++i) {
    Vtx v;
    v.pos[0] = P[i][0], v.pos[1] = P[i][1], v.pos[2] = P[i][2], v.w[0] = W0[i], v.s[0] = 0, v.s[1] = 1, v.n[1] = -0.75f, v.n[2] = 0.5f;
    const std::vector<uint32_t> d = Pack(part.F, v);
    part.host_dwords.insert(part.host_dwords.end(), d.begin(), d.end());
  }
  Mem m;
  ModelSpec spec;
  spec.parts = {part};
  const gm::ModelRead rr = gm::ReadModel(m, BuildModel(m, spec));
  Check(rr.parts_ok == 1, "CAWH space: the forged 2-bone file-space body reads");
  if (rr.parts_ok != 1) return;
  SceneTables t;
  gm::BlobSender blobs;
  gm::AppendModelRows(rr, t.guest_models, blobs, 1000);
  guestmodel::Store store;
  store.Absorb(t);
  assets::Model mdl;
  guestmodel::BuildStats bs;
  const bool built = guestmodel::BuildModel(t, store, rr.model, mdl, bs);
  Check(built && mdl.meshes.size() == 1 && mdl.meshes[0].vertices.size() == 3, "CAWH space: the body builds");
  if (!built || mdl.meshes.size() != 1 || mdl.meshes[0].vertices.size() != 3) return;
  const auto xf = [](const float v[3], const assets::Matrix& M, float o[3]) {
    for (int j = 0; j < 3; ++j) o[j] = v[0] * M[j] + v[1] * M[4 + j] + v[2] * M[8 + j] + M[12 + j];
  };
  bool skin_ok = true;
  for (int i = 0; i < 3; ++i) {
    const assets::Vertex& v = mdl.meshes[0].vertices[size_t(i)];
    float exp[3] = {0, 0, 0}, got[3] = {0, 0, 0};
    for (int k = 0; k < 2; ++k) {   // the file-space skin of the forged vertex
      float e[3];
      xf(P[i], pf[k], e);
      for (int j = 0; j < 3; ++j) exp[j] += (k == 0 ? W0[i] : 1.f - W0[i]) * e[j];
    }
    exp[1] = -exp[1];   // its viewer-space image
    for (int k = 0; k < 4; ++k) {   // the viewer-space skin of the BUILT vertex with the mirrored palette
      if (v.weights[k] <= 0.f) continue;
      float g[3];
      xf(v.pos, pv[v.bones[k] & 1], g);
      for (int j = 0; j < 3; ++j) got[j] += v.weights[k] * g[j];
    }
    for (int j = 0; j < 3; ++j) skin_ok = skin_ok && Near(exp[j], got[j], 1e-3f);
  }
  Check(skin_ok, "CAWH space: the built vertex skinned with the mirrored palette = the viewer-space image of the file-space skin (the strands' cause: file-space vertices through S.M.S)");
  Check(mdl.bounds_max[1] > 0.f && mdl.bounds_min[1] >= -1e-3f && mdl.meshes[0].vertices[0].normal[1] > 0.f, "CAWH space: bounds are viewer space (max y > 0, the body stands on +Y) and the normal y is mirrored");
  const auto file_space = [](float ymin, float ymax) { return ymax <= 0.f && ymin < 0.f; };
  Check(file_space(-197.8f, -0.2f) && !file_space(0.2f, 197.8f), "CAWH past-bug re-find: run 018's body bounds (y -197.8 .. -0.2) read as FILE space, the mirrored ones do not");
}

static void CheckRecordAndConsumer() {
  live::guest_model::ByteStore::Process().Clear();
  TexWorld w;
  uint32_t model = 0;
  gm::ModelRead r = BodyWithTextures(w, model);
  Check(r.parts_ok == 2 && r.textures.size() == 2 && r.tex_state[0][0] == kGtBound && r.tex_state[0][1] == kGtBound, "forged body: 2 parts, 2 pictures bound");
  if (r.parts_ok != 2 || r.textures.size() != 2) return;
  SceneTables t;
  gm::BlobSender blobs;
  const uint32_t added = gm::AppendModelRows(r, t.guest_models, blobs, 1000);
  Check(added == 2 && t.guest_models.parts.size() == 2 && t.guest_models.textures.size() == 2, "the real row writer: 2 part rows and 2 picture rows");
  const GuestModelPart& pr = t.guest_models.parts[0];
  Check(pr.vflags == r.p[0].vflags && pr.node_count == 4 && pr.nodes[0] == 19 && pr.nodes[3] == 49 && std::string(pr.effect) == "yCh_skin_flex" && pr.tex_count == 2 && pr.tex[0].state == kGtBound && pr.tex[0].hash == r.tex_hash[0][0],
        "a part row carries F, the palette nodes (palette - 1), the effect name and its samplers' picture hashes");
  // the second Emit within the resend period names the bytes without carrying them; a resend (odd period) carries them again
  SceneTables t2;
  const uint32_t added2 = gm::AppendModelRows(r, t2.guest_models, blobs, 1010);
  Check(added2 == 2 && t2.guest_models.blobs.empty() && t2.guest_models.parts[0].vtx_off == kGmNoBytes && t2.guest_models.textures[0].off == kGmNoBytes, "bytes are not repeated inside the resend period");
  SceneTables t3;
  gm::AppendModelRows(r, t3.guest_models, blobs, 1000 + kGmResendFrames);
  Check(!t3.guest_models.blobs.empty() && t3.guest_models.parts[0].vtx_off != kGmNoBytes, "and are sent again when the period has passed");
  // the record round trip (sections 62 / 63 / 64) and old records
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(ok && back.guest_models.parts.size() == 2 && back.guest_models.textures.size() == 2 && back.guest_models.blobs.size() == t.guest_models.blobs.size() && back.guest_models.errors == 0 && std::string(back.guest_models.parts[0].tex[1].name) == "sampNormalPrm",
        "record round trip: part rows, picture rows and blobs survive");
  SceneTables plain;
  const std::vector<uint8_t> pb = SerializeRecord(plain);
  SceneTables pback;
  Check(ParseRecord(pb.data(), pb.size(), pback, err) && pback.guest_models.Empty(), "a record without the sections parses as before (old records still read)");
  SceneTables badrow = t;
  badrow.guest_models.textures[0].off = 4096 * 1024;   // past the blob
  const std::vector<uint8_t> bb = SerializeRecord(badrow);
  SceneTables bback;
  Check(ParseRecord(bb.data(), bb.size(), bback, err) && bback.guest_models.Empty() && bback.guest_models.errors == 1, "a bad picture row drops the guest-model tables only, the record is read");
  // the consumer: a store fed by a record that carries the bytes builds the model
  {
    guestmodel::Store store;
    store.Absorb(back);
    assets::Model mdl;
    guestmodel::BuildStats bs;
    const bool built = guestmodel::BuildModel(back, store, model, mdl, bs);
    Check(built && mdl.meshes.size() == 2 && bs.triangles == 8, "consumer: the record's own bytes build the model (2 parts, 2 ranges of 2 triangles each)");
    bool tex_ok = built && mdl.textures.size() == 2;
    for (size_t i = 0; tex_ok && i < 2; ++i) tex_ok = mdl.textures[i].format == assets::TexFormat::kBC1 && mdl.textures[i].width == 64 && (mdl.textures[i].data == w.pattern[0] || mdl.textures[i].data == w.pattern[1]);
    Check(tex_ok && built && mdl.materials.size() == 2 && mdl.materials[mdl.meshes[0].material].diffuse >= 0 && mdl.materials[mdl.meshes[0].material].normal >= 0 && mdl.materials[mdl.meshes[1].material].normal < 0,
          "consumer: each part's material binds the game's own pictures by parameter name (diffuse, normal), as BC1");
    Check(built && mdl.materials[mdl.meshes[0].material].shader == "yCh_skin_flex" && mdl.materials[mdl.meshes[1].material].shader == "yCh_skin" && bs.effect_fallback == 1, "consumer: the effect NAME is the material's shader; an unknown name falls back to yCh_skin (counted)");
    Check(built && mdl.meshes[0].vertices[0].bones[0] == 19 && mdl.meshes[0].vertices[0].bones[3] == 49 && mdl.meshes[0].skinned, "consumer: palette slots became skeleton nodes (slot 0 -> palette[0] - 1 = 19)");
    // S103 CAWS (CAWG rows 3-5; P3 = plant 30: every node + 200): the model's highest node + 1 (BuildStats::max_node) is checked against the actor's palette; the forged nodes sit inside the stock 131-matrix palette
    {
      uint32_t want = 0;
      for (const assets::Mesh& me : mdl.meshes)
        for (const assets::Vertex& v : me.vertices)
          for (int k = 0; k < 4; ++k) want = std::max<uint32_t>(want, uint32_t(v.bones[k]) + 1u);
      const uint64_t before = guestmodel::Counters().nodes_ge_palette;
      Check(built && bs.max_node == want && bs.max_node >= 50, "CAWS: BuildStats::max_node = the highest node a vertex uses + 1 (the forge reaches node 49)");
      Check(!guestmodel::NoteActorPalette(bs.max_node, 131) && guestmodel::Counters().nodes_ge_palette == before, "CAWS P3: the forged body's nodes sit inside a 131-matrix palette: nodes_ge_palette stays put (plant 30 adds 200 to every node and must trip this)");
      Check(!guestmodel::NoteActorPalette(131, 131) && guestmodel::NoteActorPalette(132, 131) && !guestmodel::NoteActorPalette(500, 0) && guestmodel::Counters().nodes_ge_palette == before + 1, "CAWS: the palette check is max node + 1 > palette (131 fits 131, 132 does not; an actor with no palette is not asked)");
      guestmodel::Counters().nodes_ge_palette = before;
      guestmodel::Counters().bad_slots = 7, guestmodel::Counters().unskinned = 3;
      const std::string line = guestmodel::CensusLine();
      Check(line.find("bad slots 7") != std::string::npos && line.find("unskinned vertices 3") != std::string::npos && line.find("actor draws with a node >= the palette 0") != std::string::npos, "CAWS: the census line prints bad_slots, unskinned and nodes_ge_palette (CAWG row 5)");
      guestmodel::Counters().bad_slots = 0, guestmodel::Counters().unskinned = 0;
    }
  }
  // the in-process consumer takes bytes BY HASH from the writer's store when records carry none (a consumer that skipped the carrying record, or came late)
  {
    live::guest_model::ByteStore& bs0 = live::guest_model::ByteStore::Process();
    for (const gm::PartRead& p : r.p) { bs0.PutShared(p.vtx_hash, p.vtx, 1); std::vector<uint8_t> ib(p.idx.size() * 2); std::memcpy(ib.data(), p.idx.data(), ib.size()); bs0.Put(p.idx_hash, std::move(ib), 1); }
    for (const gm::TexRead& tx : r.textures) bs0.PutShared(tx.hash, tx.bytes, 1);
    guestmodel::Store late;   // never saw record `t`
    late.Absorb(t2);          // only the record that names hashes without bytes
    assets::Model mdl;
    guestmodel::BuildStats bs;
    Check(guestmodel::BuildModel(t2, late, model, mdl, bs) && mdl.textures.size() == 2 && late.counts().from_process > 0, "consumer: a record that names bytes without carrying them is answered from the writer's store by hash (a late / skipping consumer is whole)");
    live::guest_model::ByteStore::Process().Clear();
    guestmodel::Store none;
    none.Absorb(t2);
    Check(!guestmodel::BuildModel(t2, none, model, mdl, bs) && bs.hole_bytes == 2, "and a hash nobody holds is a hole (never half a body)");
  }
  // per-cache state: two caches keep their own store and memo (no function-local statics shared by the live and the mirror cache)
  {
    std::shared_ptr<void> slot_a, slot_b;
    guestmodel::CacheState& a = guestmodel::StateIn(slot_a);
    guestmodel::CacheState& b = guestmodel::StateIn(slot_b);
    guestmodel::AbsorbRecord(a, back);
    Check(&a != &b && !a.store.Empty() && b.store.Empty() && &guestmodel::StateIn(slot_a) == &a, "per-ModelCache state: the cache that absorbed a record holds its bytes, the other does not");
    SceneTables empty;
    guestmodel::AbsorbRecord(b, empty);
    Check(b.store.Empty(), "AbsorbRecord of a record with no guest rows keeps a cache with no store empty (free)");
  }
  // a texture the record does not carry (bytes dropped) leaves the slot unbound, counted, the model still builds
  {
    SceneTables t4 = back;
    t4.guest_models.textures.clear();
    guestmodel::Store store;
    store.Absorb(back);
    assets::Model mdl;
    guestmodel::BuildStats bs;
    const bool built = guestmodel::BuildModel(t4, store, model, mdl, bs);
    Check(built && bs.tex_missing == 3 && mdl.textures.empty() && bs.no_diffuse == 2, "pictures the record does not name are missing (counted), never a wrong picture");
  }
  const size_t merged = 1568 + 80 + sizeof(GuestModelTables);
  std::printf("note: merged SceneTables size = base 1568 + PW 80 + guest_models %zu = %zu (this tree: 1568 + %zu = %zu)\n", sizeof(GuestModelTables), merged, sizeof(GuestModelTables), 1568 + sizeof(GuestModelTables));
}

// S110 WHITEFACE (docs\research\s110\WHITEFACE.md): a wrestler's face parts carry 68 material parameters; the reader once refused every texture of a part with more than 64 (`pc <= 64`), so the face bound white.
// The reader now walks the game's own count; only a garbage count (> kParamCountGarbage) is refused and counted. Plant 17 = the old `pc <= 64` guard.
static void CheckWhiteFace() {
  live::guest_model::ByteStore::Process().Clear();
  struct Out { bool built = false; uint32_t no_diffuse = 99, param_unreadable = 99; std::vector<bool> has_diffuse; };
  // one forged model of `n_params`-parameter parts; each entry = {total params, index of the diffuse among them (-1 = none), extra sampler params right after it}. Every sampler shares texture #0.
  const auto run = [](const std::vector<std::array<int, 3>>& specs, uint32_t garbage_part) {
    TexWorld w;
    const uint32_t hA = w.AddTexture(0, 0x1C100000u, 21);
    ModelSpec spec;
    spec.two_ranges = true;
    spec.tex_keys = {hA};
    for (const auto& s : specs) {
      PartSpec p0;
      p0.F = 0x0C | 0x10 | 0x40 | 0x100;
      p0.vcount = 4, p0.idx = {0, 1, 2, 3, 3, 2, 1, 0}, p0.n0 = 4, p0.n1 = 4, p0.palette = {20, 30, 40, 50}, p0.effect = "yCh_skin";
      Vtx vx;
      vx.pos[0] = 2.f, vx.w[0] = 0.5f, vx.s[0] = 0, vx.n[2] = 1.f, vx.uv[0][0] = 0.25f, vx.uv[0][1] = 0.75f;
      for (int i = 0; i < 4; ++i) { const std::vector<uint32_t> d = Pack(p0.F, vx); p0.host_dwords.insert(p0.host_dwords.end(), d.begin(), d.end()); }
      static const char* const kExtra[] = {"sampNormalPrm", "g_mMaskPrm", "g_mSpecPrm", "g_mEnvPrm", "g_mWrinklePrm", "g_mSweatPrm"};
      for (int k = 0; k < s[0]; ++k) p0.samplers.push_back({"g_fFiller", 0, 3});   // kind 3: not a sampler
      if (s[1] >= 0) p0.samplers[size_t(s[1])] = {"g_mDiffusePrm", 0, 15};
      for (int k = 0; k < s[2]; ++k) p0.samplers[size_t(s[1] + 1 + k)] = {kExtra[k], 0, 15};
      spec.parts.push_back(p0);
    }
    const uint32_t model = BuildModel(w.m, spec);
    if (garbage_part != 0xFFFFFFFFu) {
      uint32_t desc = 0;
      w.m.u32(model + 824, desc);
      w.m.put32(desc + 184 * garbage_part + 148, 0x7FFFFFFFu);   // [desc+148] = the parameter count
    }
    gm::ModelRead r = gm::ReadModel(w.m, model);
    gm::TexResolver res;
    res.Resolve(w.m, [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); }, r, 10);
    SceneTables t;
    gm::BlobSender blobs;
    gm::AppendModelRows(r, t.guest_models, blobs, 1000);
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    std::string err;
    guestmodel::Store store;
    assets::Model mdl;
    guestmodel::BuildStats bs;
    Out o;
    if (ParseRecord(bytes.data(), bytes.size(), back, err)) store.Absorb(back), o.built = guestmodel::BuildModel(back, store, model, mdl, bs);
    o.no_diffuse = uint32_t(bs.no_diffuse), o.param_unreadable = r.param_unreadable;
    for (const assets::Mesh& me : mdl.meshes) o.has_diffuse.push_back(mdl.materials[me.material].diffuse >= 0);
    return o;
  };
  // 1: a 45-parameter part and a 68-parameter part (diffuse first, 7 samplers: diffuse + 6 more; the 7th is past kMaxSamplers) share texture #0: BOTH build with their diffuse
  const Out a = run({{45, 0, 0}, {68, 0, 6}}, 0xFFFFFFFFu);
  Check(a.built && a.has_diffuse.size() == 2 && a.has_diffuse[0] && a.has_diffuse[1] && a.no_diffuse == 0 && a.param_unreadable == 0, "WHITEFACE: a 68-parameter part (diffuse first, 7 samplers, sharing texture #0 with a 45-parameter part) builds WITH its diffuse: no_diffuse " + std::to_string(a.no_diffuse));
  // 2: the diffuse is the LAST of 300 parameters: the walk has no cap of ours
  const Out b = run({{300, 299, 0}}, 0xFFFFFFFFu);
  Check(b.built && b.has_diffuse.size() == 1 && b.has_diffuse[0] && b.no_diffuse == 0 && b.param_unreadable == 0, "WHITEFACE: a 300-parameter part with the diffuse as its LAST parameter builds with its diffuse (the game's count is walked whole)");
  // 3: a garbage count (0x7FFFFFFF) is refused and counted; the model still builds, that part without a diffuse
  const Out c = run({{45, 0, 0}, {45, 0, 0}}, 1);
  Check(c.built && c.has_diffuse.size() == 2 && c.has_diffuse[0] && !c.has_diffuse[1] && c.no_diffuse == 1 && c.param_unreadable == 1, "WHITEFACE: a garbage parameter count (0x7FFFFFFF) refuses that part's parameters and is counted (params unreadable 1); the other part is untouched");
}

// S110 WHITEEYES (docs\research\s110\WHITEEYES.md): after a Create a Superstar pick the composer recopies its pictures (each copy ends in the unlock flush); the resolver reads one 8888 picture a call,
// so a flushed picture it already holds is DEFERRED and keeps its old hash in the read. The read must carry that picture's bytes: the rows name the hash, and when the re-read finds the same content (the
// eyes did not change) the model key is the same, so the consumer keeps the build made on the deferred frame. Plant 60 (g_gm_tex_plant 38) = the old exit: the hash without its bytes, the eye part white.
static void CheckWhiteEyes() {
  live::guest_model::ByteStore::Process().Clear();
  const uint32_t kBgra = 0xC14u;
  TexWorld w;
  std::vector<uint8_t> pat_face, pat_eye;
  const uint32_t idF = Add8888(w, Phys::kBase, 64, 64, 0, kBgra, false, 5, pat_face), idE = Add8888(w, Phys::kBase + 0x4000u, 64, 64, 0, kBgra, false, 77, pat_eye);
  ModelSpec spec;
  for (int k = 0; k < 2; ++k) {   // part 0 = the face (its picture changes with the pick), part 1 = the eye (its picture does not)
    PartSpec p;
    p.vcount = 4, p.host_dwords = Dwords(4, 8, uint32_t(1 + k)), p.idx = {0, 1, 2, 3}, p.n0 = 4, p.palette = {1, 2};
    p.effect = k == 0 ? "ych_skin" : "ych_reflect";
    p.samplers = {{"g_mDiffusePrm", k, 15}};
    spec.parts.push_back(p);
  }
  spec.tex_keys = {idF, idE};
  const uint32_t model = BuildModel(w.m, spec);
  gm::ModelRead r = gm::ReadModel(w.m, model);
  if (r.parts_ok != 2) { Check(false, "WHITEEYES: the forged two-part model did not read"); return; }
  const auto phys = [&](uint32_t a, uint32_t n, uint8_t* d) { return w.phys.read(a, n, d); };
  gm::TexResolver res;
  res.born_of = &UnitBorn;
  Born(true, kCreatorLoader, 0x82BAA6C0u);
  // the eye part's model key (the rows writer's own: geometry + every sampler's hash and state)
  const auto key_of = [](const gm::ModelRead& m) { GuestModelTables g; gm::BlobSender b; gm::AppendModelRows(m, g, b, 1); return g.parts.size() == 2 ? g.parts[1].model_hash : uint64_t(0); };
  res.Resolve(w.m, phys, r, 100);   // one 8888 read a call: the face first, the eye next call
  const gm::ModelRead first = r;
  for (uint64_t f = 101; f < 104 && r.tex_incomplete; ++f) res.Resolve(w.m, phys, r, f);
  if (r.tex_incomplete || r.tex_state[0][0] != kGtBound || r.tex_state[1][0] != kGtBound || !PicOf(r, r.tex_hash[1][0])) { Check(false, "WHITEEYES: the two pictures did not settle"); return; }
  const uint64_t hFace = r.tex_hash[0][0], hEye = r.tex_hash[1][0];
  const std::vector<uint8_t> eye_bytes = *PicOf(r, hEye);
  // review: the FIRST sight of the eye is put off too (no held bytes to carry): it names no hash and no picture (state missing), so that frame's white build sits under a key that says so, and the key
  // moves when the eye's bytes arrive (a rebuild, never the permanent case)
  Check(first.tex_incomplete && first.tex_state[1][0] == kGtMissing && first.tex_hash[1][0] == 0 && first.textures.size() == 1 && key_of(first) != 0 && key_of(first) != key_of(r),
        "WHITEEYES: a first-sight eye put off names no hash and no picture (state missing); its model key differs from the settled read's");
  // the pick: the face picture changes, the eye picture does not; the composer's copy flushes both
  for (size_t i = 0; i < 64; ++i) w.phys.b[i] ^= 0x5A;
  live::guest_tex_dirty::NoteFlush(w.header_of[idF & 63]);
  live::guest_tex_dirty::NoteFlush(w.header_of[idE & 63]);
  gm::TexOut o;
  res.ResolveTo(w.m, phys, r, o, 120);
  gm::ModelRead mid = r;
  gm::ApplyTex(mid, std::move(o));
  Check(mid.tex_incomplete && mid.tex_hash[0][0] != hFace && mid.tex_hash[1][0] == hEye && mid.tex_state[1][0] == kGtBound && res.stats().dirty_deferred >= 1,
        "WHITEEYES: on the flush the face is re-read (new hash) and the eye is deferred (one 8888 read a call): it keeps its held hash, state bound, the read incomplete");
  Check(PicOf(mid, hEye) && *PicOf(mid, hEye) == eye_bytes, "WHITEEYES: the deferred eye's held bytes ride in the read with the hash it names");
  // the consumer on the deferred frame: the rows -> the record -> the build; the eye part keeps its picture
  SceneTables t;
  gm::BlobSender blobs;
  gm::AppendModelRows(mid, t.guest_models, blobs, 120);
  guestmodel::Store store;
  store.Absorb(t);
  assets::Model mdl;
  guestmodel::BuildStats bs;
  const bool built = guestmodel::BuildModel(t, store, model, mdl, bs);
  bool eye_ok = built && mdl.meshes.size() == 2 && mdl.meshes[1].material >= 0 && mdl.materials[size_t(mdl.meshes[1].material)].diffuse >= 0;
  if (eye_ok) eye_ok = mdl.textures[size_t(mdl.materials[size_t(mdl.meshes[1].material)].diffuse)].data == eye_bytes;
  Check(eye_ok && bs.no_diffuse == 0 && bs.tex_missing == 0, "WHITEEYES: the model built on the deferred frame has the eye's picture (no_diffuse " + std::to_string(bs.no_diffuse) + ", missing " + std::to_string(bs.tex_missing) + ")");
  // why that frame's build must be whole: the next call re-reads the eye, finds the same bytes, and the model key does not move (the consumer keeps the deferred frame's build)
  gm::TexOut o2;
  res.ResolveTo(w.m, phys, mid, o2, 121);
  gm::ModelRead done = mid;
  gm::ApplyTex(done, std::move(o2));
  GuestModelTables g_mid, g_done;
  gm::BlobSender b1, b2;
  gm::AppendModelRows(mid, g_mid, b1, 120);
  gm::AppendModelRows(done, g_done, b2, 121);
  Check(!done.tex_incomplete && done.tex_hash[1][0] == hEye && g_mid.parts.size() == 2 && g_done.parts.size() == 2 && g_mid.parts[1].model_hash == g_done.parts[1].model_hash,
        "WHITEEYES: the completed read (eye re-read, same bytes) has the SAME model key as the deferred one: the build on the deferred frame is the one that stays");
  // review: an eye whose bytes DID change while put off: the deferred read carries the OLD bytes under the OLD hash (key and content agree), and the re-read names the new picture: a new key, a rebuild
  for (size_t i = 0; i < 64; ++i) w.phys.b[i] ^= 0x33, w.phys.b[0x4000u + i] ^= 0x33;
  live::guest_tex_dirty::NoteFlush(w.header_of[idF & 63]);
  live::guest_tex_dirty::NoteFlush(w.header_of[idE & 63]);
  gm::TexOut o3, o4;
  res.ResolveTo(w.m, phys, done, o3, 130);
  gm::ModelRead put_off = done;
  gm::ApplyTex(put_off, std::move(o3));
  Check(put_off.tex_incomplete && put_off.tex_hash[0][0] != done.tex_hash[0][0] && put_off.tex_hash[1][0] == hEye && PicOf(put_off, hEye) && *PicOf(put_off, hEye) == eye_bytes,
        "WHITEEYES: an eye changed while put off rides with its OLD bytes under its OLD hash");
  res.ResolveTo(w.m, phys, put_off, o4, 131);
  gm::ModelRead after = put_off;
  gm::ApplyTex(after, std::move(o4));
  Check(!after.tex_incomplete && after.tex_hash[1][0] != hEye && after.tex_hash[1][0] != 0 && PicOf(after, after.tex_hash[1][0]) && *PicOf(after, after.tex_hash[1][0]) != eye_bytes && key_of(after) != key_of(put_off),
        "WHITEEYES: its re-read names the new picture with its bytes: a new model key (the put-off frame's build is replaced)");
}

// S110 STOREABSENT (docs\research\s110\STOREABSENT.md): a build made while a picture its rows NAME has no bytes in either store is provisional: drawn, not final. The cache's gate (guest_model_absent.h,
// the rule GuestModelFor runs: KeptBuildStands before reuse, NoteBuild after a build) rebuilds it only when one of those pictures arrives; and neither store refuses a new picture at its cap (the least
// recently named go instead). Plants: 61 (draw 15) a provisional build is final, 62 (draw 16) a provisional build is rebuilt every call, 63 (draw 17) the cache's Store refuses at its cap,
// 64 (store 1) the process ByteStore refuses at its cap.
static void CheckStoreAbsent() {
  live::guest_model::ByteStore::Process().Clear();
  TexWorld w;
  uint32_t model = 0;
  const gm::ModelRead r = BodyWithTextures(w, model);
  if (r.parts_ok != 2 || r.textures.size() != 2) { Check(false, "STOREABSENT: the forged body did not read"); return; }
  SceneTables carried;   // geometry and pictures carried
  gm::BlobSender blobs;
  gm::AppendModelRows(r, carried.guest_models, blobs, 1000);
  SceneTables named = carried;   // the pictures NAMED, their bytes in no record (the carrying record was missed) and not in the process store
  for (GuestModelTexture& x : named.guest_models.textures) x.off = kGmNoBytes;
  const std::string key = "guest:storeabsent";
  // one cache's frames, as GuestModelFor runs them: a kept build stands unless the gate says a picture it lacked has arrived
  struct Run { std::shared_ptr<void> slot; bool kept = false; int builds = 0; assets::Model shown; guestmodel::BuildStats last; };
  const auto frame = [&](Run& run, const SceneTables& rec) {
    guestmodel::CacheState& cs = guestmodel::StateIn(run.slot);
    guestmodel::AbsorbRecord(cs, rec);
    if (run.kept && guestmodel::KeptBuildStands(cs, rec, key.c_str())) return;
    assets::Model m;
    guestmodel::BuildStats bs;
    if (!guestmodel::BuildModel(rec, cs.store, model, m, bs)) return;
    guestmodel::NoteBuild(cs, key.c_str(), bs);
    run.shown = std::move(m), run.last = bs, run.kept = true, ++run.builds;
  };
  const auto diffuse_on = [](const assets::Model& m) {
    if (m.meshes.size() != 2) return false;
    for (const assets::Mesh& me : m.meshes)
      if (me.material < 0 || m.materials[size_t(me.material)].diffuse < 0) return false;
    return true;
  };
  // 1: absent at build: the model builds (geometry whole), every named picture counted absent: provisional
  Run a;
  frame(a, named);
  Check(a.builds == 1 && a.last.tex_absent == 3 && a.last.tex_missing == 3 && a.last.absent.size() == 2 && a.last.no_diffuse == 2 && !diffuse_on(a.shown),
        "STOREABSENT: a build whose named pictures have no bytes is made (geometry whole) with 3 samplers absent and 2 distinct pictures listed: provisional");
  // 2: the pictures stay absent: the kept build stands (no rebuild a frame)
  for (int f = 0; f < 50; ++f) frame(a, named);
  Check(a.builds == 1, "STOREABSENT: a permanently absent picture does not rebuild every frame (builds " + std::to_string(a.builds) + " after 51 frames)");
  // 3: the bytes arrive in the process store (the writer's read): the next frame rebuilds once and the pictures are ON
  for (const gm::TexRead& tx : r.textures) live::guest_model::ByteStore::Process().PutShared(tx.hash, tx.bytes, 2000);
  frame(a, named);
  Check(a.builds == 2 && diffuse_on(a.shown) && a.last.tex_absent == 0 && a.last.absent.empty() && a.shown.textures.size() == 2,
        "STOREABSENT: when the bytes arrive the provisional build is rebuilt once: every part's picture is ON (builds " + std::to_string(a.builds) + ")");
  for (int f = 0; f < 20; ++f) frame(a, named);
  Check(a.builds == 2, "STOREABSENT: the whole rebuild is final (no further builds)");
  // 4: the bytes arrive in a record (the resend): the cache's Store takes them and the provisional build is rebuilt the same way
  live::guest_model::ByteStore::Process().Clear();
  Run b;
  for (int f = 0; f < 5; ++f) frame(b, named);
  frame(b, carried);
  Check(b.builds == 2 && diffuse_on(b.shown), "STOREABSENT: a record that carries the bytes (the resend) rebuilds the provisional build with its pictures ON");
  // 4b (review): N absent pictures, ONE arrives: one rebuild, the other keeps the build provisional (it stands while that one stays away), and its own arrival makes the build whole
  live::guest_model::ByteStore::Process().Clear();
  Run c;
  frame(c, named);
  live::guest_model::ByteStore::Process().PutShared(r.textures[0].hash, r.textures[0].bytes, 3000);
  frame(c, named);
  const size_t left_after_one = c.last.absent.size();
  for (int f = 0; f < 20; ++f) frame(c, named);
  const int builds_after_one = c.builds;
  live::guest_model::ByteStore::Process().PutShared(r.textures[1].hash, r.textures[1].bytes, 3001);
  for (int f = 0; f < 6; ++f) frame(c, named);
  Check(builds_after_one == 2 && left_after_one == 1 && c.builds == 3 && c.last.absent.empty() && diffuse_on(c.shown),
        "STOREABSENT: one of two absent pictures arrives: one rebuild, the other keeps it provisional with no rebuild a frame, its arrival makes it whole (builds " + std::to_string(builds_after_one) + " then " + std::to_string(c.builds) + ", left " + std::to_string(left_after_one) + ")");
  // 5: the cache's Store over its cap takes the new picture and drops the least recently named
  live::guest_model::ByteStore::Process().Clear();
  const auto pics = [](const std::vector<uint64_t>& hs) {
    SceneTables s;
    for (size_t i = 0; i < hs.size(); ++i) {
      GuestModelTexture x;
      x.hash = hs[i], x.width = 4, x.height = 4, x.format = 20, x.nbytes = 16, x.off = uint32_t(16 * i);
      s.guest_models.textures.push_back(x);
    }
    s.guest_models.blobs.assign(16 * hs.size(), 7);
    return s;
  };
  {
    guestmodel::Store s(32);   // room for two pictures
    s.Absorb(pics({0xA1}));
    s.Absorb(pics({0xA2}));
    s.Absorb(pics({0xA3}));
    const bool new_in = s.Tex(0xA3) != nullptr, mid_in = s.Tex(0xA2) != nullptr, old_out = s.Tex(0xA1) == nullptr;
    Check(new_in && mid_in && old_out && s.bytes() == 32 && s.counts().dropped == 1, "STOREABSENT: the cache's Store at its cap takes the new picture and drops the least recently named (it no longer refuses the new one)");
    guestmodel::Store one(32);
    one.Absorb(pics({0xB1, 0xB2, 0xB3}));
    Check(one.Tex(0xB1) != nullptr && one.Tex(0xB2) != nullptr && one.Tex(0xB3) != nullptr && one.bytes() == 48, "STOREABSENT: what one record names stays even over the cap (it is drawn now)");
  }
  // 6: the process ByteStore at its cap: the same rule by the writer's frame
  {
    gm::ByteStore bs(32);
    bs.Put(0xC1, std::vector<uint8_t>(16, 1), 1);
    bs.Put(0xC2, std::vector<uint8_t>(16, 2), 2);
    bs.Put(0xC3, std::vector<uint8_t>(16, 3), 3);
    Check(bs.Peek(0xC3) != nullptr && bs.Peek(0xC2) != nullptr && bs.Peek(0xC1) == nullptr && bs.bytes() == 32 && bs.full() == 0, "STOREABSENT: the process ByteStore at its cap takes the new picture and drops the least recently put (it no longer refuses the new one)");
    gm::ByteStore now(32);
    now.Put(0xD1, std::vector<uint8_t>(16, 1), 5);
    now.Put(0xD2, std::vector<uint8_t>(16, 2), 5);
    now.Put(0xD3, std::vector<uint8_t>(16, 3), 5);
    Check(now.Peek(0xD1) != nullptr && now.Peek(0xD2) != nullptr && now.Peek(0xD3) != nullptr && now.full() == 1, "STOREABSENT: what this frame put stays even over the cap (counted in full)");
  }
  live::guest_model::ByteStore::Process().Clear();
}

int main(int argc, char** argv) {
  int plant = 0;
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) plant = std::atoi(argv[i + 1]);
  if (plant >= 1 && plant <= 17) gm::g_gm_plant = plant;   // 17 = S110 WHITEFACE: the old `pc <= 64` guard   // 15 / 16 = S108 RDS: the layout line copy's window unaligned / no line copy
  if (plant >= 21 && plant <= 30) guestmodel::g_draw_plant = plant - 20;   // 30 (draw plant 10, CAWS P3): every decoded node + 200   // 29 (draw plant 9, CTX2): the actors_drawn increment is left out
  if (plant == 41) guestmodel::g_draw_plant = 11;                           // 41 (draw plant 11, CAWH): the file-space -> viewer-space mirror is skipped
  if (plant == 42) guestmodel::g_draw_plant = 12;                           // 42 (draw plant 12, S105 A5b): no guest part blends (the old rule)
  if (plant == 44) guestmodel::g_draw_plant = 13;                           // 44 (draw plant 13, S105 A5b review F1): a trans part blends even with no normal picture
  if (plant == 43) rdr_scene::g_piece_row_plant = 1;                        // 43 (S105 A5b): every src_kind 2 mesh answers the piece-row lookup, guest parts included
  if (plant == 51 || plant == 52 || plant == 53 || plant == 54) gm::g_gm_logo_plant = plant - 50;   // S105 A5c: 51 the callback word is not checked, 52 buf unbounded, 53 key 0 accepted, 54 kGmDiffuseT0 never set
  if (plant == 55) gm::g_gm_tex_plant = 36;                                  // S105 A5c: a flushed picture is not refreshed at once
  if (plant == 56) gm::g_gm_tex_plant = 37;                                  // S105 A5c: the logo keys are not resolved
  if (plant == 60) gm::g_gm_tex_plant = 38;                                  // S110 WHITEEYES: a deferred refresh names the held hash without its bytes
  if (plant == 61 || plant == 62 || plant == 63) guestmodel::g_draw_plant = plant - 46;   // S110 STOREABSENT: 61 (draw 15) a provisional build is final, 62 (draw 16) rebuilt every call, 63 (draw 17) the Store refuses at its cap
  if (plant == 64) gm::g_store_plant = 1;                                    // S110 STOREABSENT: the process ByteStore refuses at its cap (the old rule)
  if (plant == 57) guestmodel::g_draw_plant = 14;                            // S105 A5c: the logo pictures are not put on the material
  if (plant == 58) g_logo_bool_plant = 1;                                    // S105 A5c: bit 150 never set
  if (plant == 59) g_logo_bool_plant = 2;                                    // S105 A5c: bit 150 always set
  if (plant >= 31 && plant <= 35) gm::g_gm_tex_plant = plant;               // 34 (CTX2): allow_8888 off; 35: the 8888 in-place refresh off (frozen pictures)
  CheckForged();
  CheckVariants();
  CheckOracle();
  CheckPlanner();
  CheckLineCopy();   // S108 RDS
  CheckTextures();
  CheckCtf();
  CheckWhiteFace();   // S110 WHITEFACE
  CheckWhiteEyes();   // S110 WHITEEYES
  CheckStoreAbsent();   // S110 STOREABSENT
  CheckBlend();
  CheckCawd();
  CheckLogo();
  CheckRecordAndConsumer();
  CheckSpace();
  std::printf("%s: %d failure(s)%s\n", g_fail ? "FAILED" : "all checks passed", g_fail, plant ? " (planted)" : "");
  return g_fail ? 1 : 0;
}
