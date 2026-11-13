// Lane 133 unit check of the SHIPPED join (src/renderer_scene/crowd_props_core.h), no GPU, no game files: synthetic prop sets.
//   crowd_props_unit.exe [FAULT[,FAULT]]   exit 0 = every check passed; a planted fault must make its check FAIL (exit 1)
// Checks: (1) F1: two candidates with the same counts and different bytes (keys 41001 before 41006): a draw of 41006 is replaced by 41006 and counts NO mismatch, first_fail empty.
//         (2) the game's one-ulp normal joins at the default tolerance; (3) a negated normal does not; (4) an unknown mesh is Unjoined, equal counts with other bytes is Mismatch (counted once);
//         (5) a wrong host stride is kept as Variant.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/crowd_props_core.h"

using namespace rdr_scene;
using assets::crowd_prop_gs::PropMesh;
using assets::crowd_prop_gs::PropSet;

static PropMesh MakeMesh(uint32_t key, uint32_t vc, uint32_t ic, float seed) {
  PropMesh m;
  m.key = key, m.vertex_count = vc;
  m.vertices.resize(size_t(vc) * 28);
  for (uint32_t i = 0; i < vc; ++i) {
    float v[6] = {seed + float(i), seed * 2 + float(i) * 0.5f, seed * 3, 0.0f, 1.0f, 0.0f};
    std::memcpy(m.vertices.data() + size_t(i) * 28, v, 24);
    const uint16_t h[2] = {uint16_t(i), uint16_t(seed)};
    std::memcpy(m.vertices.data() + size_t(i) * 28 + 24, h, 4);
  }
  for (uint32_t i = 0; i < ic; ++i) m.indices.push_back(uint16_t(i % vc));
  return m;
}

static int g_bad = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  g_bad += !ok;
}

int main(int argc, char** argv) {
  CropFaults f;
  std::string err;
  if (argc > 1 && !CropFaults::Parse(argv[1], f, err)) return std::printf("FAIL  %s\n", err.c_str()), 2;
  auto set = std::make_shared<PropSet>();
  set->meshes = {MakeMesh(41001, 146, 247, 1.0f), MakeMesh(41006, 146, 247, 7.0f), MakeMesh(40000, 34, 77, 3.0f)};
  const PropMesh& a41006 = set->meshes[1];
  const PropMesh& a40000 = set->meshes[2];
  {   // (1) F1
    CrowdPropsCore core(f);
    core.SetProps(set);
    const auto d = core.Decide(0x82518D60, 146, 247, 28, a41006.vertices.data(), a41006.vertices.size(), reinterpret_cast<const uint8_t*>(a41006.indices.data()), a41006.indices.size() * 2);
    Check(d.ok && d.mesh == 1, "a 41006 draw joins mesh 41006 (candidate 41001 has the same counts)");
    Check(core.stats().bad_vertices == 0 && core.stats().bad_indices == 0 && core.first_fail().empty(), "F1: no mismatch counted and no 'first failed check' for a draw that joined");
  }
  {   // (2) / (3) normals
    CrowdPropsCore core(f);
    core.SetProps(set);
    std::vector<uint8_t> v = a40000.vertices;
    for (uint32_t i = 0; i < 34; i += 3) {   // the game's normal: one ulp off on some vertices
      uint32_t bits;
      std::memcpy(&bits, v.data() + size_t(i) * 28 + 12, 4);
      bits += 1;
      std::memcpy(v.data() + size_t(i) * 28 + 12, &bits, 4);
    }
    const auto d = core.Decide(0x82518D60, 34, 77, 28, v.data(), v.size(), reinterpret_cast<const uint8_t*>(a40000.indices.data()), a40000.indices.size() * 2);
    Check(d.ok, "the game's one-ulp normals join (tolerance 1e-6)");
    std::vector<uint8_t> w = a40000.vertices;
    for (uint32_t i = 0; i < 34; ++i) {
      float n[3];
      std::memcpy(n, w.data() + size_t(i) * 28 + 12, 12);
      for (float& c : n) c = -c;
      n[1] -= 0.0f;
      std::memcpy(w.data() + size_t(i) * 28 + 12, n, 12);
    }
    const auto e = core.Decide(0x82518D60, 34, 77, 28, w.data(), w.size(), reinterpret_cast<const uint8_t*>(a40000.indices.data()), a40000.indices.size() * 2);
    Check(!e.ok && e.why == rdr_adapter::KeepWhy::Mismatch, "a flipped normal is a Mismatch");
  }
  {   // (4) / (5)
    CrowdPropsCore core(f);
    core.SetProps(set);
    const auto d = core.Decide(0x82518D60, 99, 99, 28, a40000.vertices.data(), a40000.vertices.size(), reinterpret_cast<const uint8_t*>(a40000.indices.data()), a40000.indices.size() * 2);
    Check(!d.ok && d.why == rdr_adapter::KeepWhy::Unjoined, "unknown counts are Unjoined");
    const auto e = core.Decide(0x82518D60, 34, 77, 32, a40000.vertices.data(), a40000.vertices.size(), reinterpret_cast<const uint8_t*>(a40000.indices.data()), a40000.indices.size() * 2);
    Check(!e.ok && e.why == rdr_adapter::KeepWhy::Variant, "a host stride other than 28 is kept as Variant");
    std::vector<uint8_t> other = a40000.vertices;
    other[0] ^= 0x55;
    const auto g = core.Decide(0x82518D60, 34, 77, 28, other.data(), other.size(), reinterpret_cast<const uint8_t*>(a40000.indices.data()), a40000.indices.size() * 2);
    Check(!g.ok && g.why == rdr_adapter::KeepWhy::Mismatch && core.stats().bad_vertices == 1, "equal counts, other bytes: Mismatch, counted once");
  }
  std::printf("crowd_props_unit: %s\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}
