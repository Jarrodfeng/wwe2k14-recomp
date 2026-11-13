// caster_vertex_check: the unit test of the shadow-layout vertex writers (src\assets\shadow_caster_vertex.h; Session 101, lane 85 = SV). 0 game runs, no GPU.
//   caster_vertex_check --game DIR --capture DIR [--arena N] [--step N] [--fault LIST] [--lines]
// For every caster draw of the capture's shadow passes (the wrestler maps A / B at 0x82C04634 and the plane mask at 0x82BF4D38):
//   * a wrestler / piece object is joined to a roster model BY CONTENT through its COLOUR draws (counts, then the colour vertex bytes rebuilt by wrestler_geom: the lane 45 join), its one
//     scale measured; the shadow draw's vertex buffer is then written by the SHIPPED writer (shadow_vertex::Write, in the stream's shadow layout) and must equal the stream's bytes;
//   * a plane-pass draw is joined by content (counts + bit-equal positions) to the ring entry 956 of the arena or to chCommon.pac entry 0 (the wrestlers' plane-shadow proxy), part = mesh,
//     and written the same way.
// Plus the uv probe: the colour draws of the joined wrestler parts carry a uv element; the writer's PackUv with the u-sign rule LEARNED from the stream's own first negative-u vertex
// (LearnUvSign, as ring_ours_core's DetectUvSign) must equal it on every vertex. Objects with no join (props, circles) are counted "not ours", never written.
// --fault LIST (comma): fetch_order, weights_as_fill, xdword_colour, bone_zero, uv_sign: a writer fault planted inside the shipped code; the run then must FAIL its own exact check.
// Exit: 0 all hold, 1 a check failed, 2 a loud failure (cannot read the input), 3 bad usage.
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
#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/assets/ring_game_space.h"
#include "../../src/assets/shadow_caster_vertex.h"
#include "../../src/assets/wrestler_roster.h"
#include "../../src/renderer_adapter/capture_reader.h"

namespace sv = assets::shadow_vertex;
namespace wg = assets::wrestler_geom;
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;

namespace {

constexpr uint32_t kSiteMap = 0x82C04634u, kSitePlane = 0x82BF4D38u, kSiteColour = 0x82C0A950u, kSiteMirror = 0x82C0B94Cu;

int g_fail = 0;
void Check(const char* name, bool ok, const char* fmt = "", ...) {
  char detail[600] = {};
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
  }
  std::printf("%s  %s%s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? "  (" : "", detail, detail[0] ? ")" : "");
  if (!ok) ++g_fail;
}

enum Klass { kWrestler, kRing, kProxy, kKlassCount };
const char* const kKlassName[] = {"wrestler", "ring956", "chcommon"};

struct Tally {
  uint64_t draws = 0, vertices = 0, refused = 0, part_not_mesh = 0, count_off = 0;
  uint64_t bad_vertices = 0;                    // vertices with any byte different
  uint64_t bad_role[6] = {};                    // vertices whose element of that role differs (sv::Role order)
  uint64_t role_values[6] = {};
};

struct Body {   // one wrestler / piece object joined to a roster model
  std::shared_ptr<assets::wrestler_roster::Model> model;
  float scale = 0;
  sv::FillMode fill = sv::FillMode::HiByteX;
  uint64_t sig = 0;
  int entry = -1;
};

struct Proxy {   // a plane-pass file mesh: ring 956 or chCommon entry 0
  Klass klass;
  const assets::jboy_parts::Part* part;
  int mesh;
};

uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }

// The rules that need no capture: the u-sign learner reads the stream's half (never assumes), a mesh with no negative u teaches nothing, and the plant flips the rule.
void RuleChecks(const sv::Faults& faults) {
  const uint8_t s2[16] = {0x3F, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00,    // vertex 0: u = 0.5, v = 0.5 (big-endian floats)
                          0xBF, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00};   // vertex 1: u = -0.5, v = 0.5
  assets::jboy_parts::Part p;
  p.vertices = 2;
  p.s[2] = s2;
  const uint16_t signed_stream[4] = {0x3800, 0x3800, 0xB800, 0x3800}, abs_stream[4] = {0x3800, 0x3800, 0x3800, 0x3800};
  const uint8_t* a = reinterpret_cast<const uint8_t*>(signed_stream);
  const uint8_t* b = reinterpret_cast<const uint8_t*>(abs_stream);
  Check("rule.uv_sign_learned", sv::LearnUvSign(p, a, 8, 4, 0) == sv::UvSign::Signed && sv::LearnUvSign(p, b, 8, 4, 0) == sv::UvSign::Abs && sv::LearnUvSign(p, a, 4, 4, 0) == sv::UvSign::Unknown,
        "signed stream -> Signed, |u| stream -> Abs, short buffer -> Unknown");
  p.vertices = 1;
  Check("rule.uv_sign_needs_a_negative", sv::LearnUvSign(p, a, 8, 4, 0) == sv::UvSign::Unknown && sv::FirstNegativeU(p) < 0, "no negative u: nothing to learn from");
  uint8_t h[4], hp[4];
  sv::PackUv(-0.5f, 0.5f, sv::UvSign::Signed, false, h);
  sv::PackUv(-0.5f, 0.5f, sv::UvSign::Abs, false, hp);
  uint16_t hs, ha;
  std::memcpy(&hs, h, 2), std::memcpy(&ha, hp, 2);
  Check("rule.uv_halves", hs == 0xB800 && ha == 0x3800, "signed %04x, |u| %04x", hs, ha);
  (void)faults;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, capture, fault_list;
  int arena = 4, step = 1, from = 0, to = -1;   // S101 lane 99: --from / --to (the frames [from, to) only: the oracle for a promotion whose first frames have no scene record)
  bool lines = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game") game = next();
    else if (a == "--capture") capture = next();
    else if (a == "--fault") fault_list = next();
    else if (a == "--arena") arena = std::atoi(next());
    else if (a == "--step") step = std::max(1, std::atoi(next()));
    else if (a == "--from") from = std::max(0, std::atoi(next()));   // S101 lane 99
    else if (a == "--to") to = std::atoi(next());
    else if (a == "--lines") lines = true;
    else {
      std::fprintf(stderr, "caster_vertex_check: unknown argument %s\n", a.c_str());
      return 3;
    }
  }
  sv::Faults faults;
  {
    size_t at = 0;
    while (at <= fault_list.size()) {
      size_t comma = fault_list.find(',', at);
      if (comma == std::string::npos) comma = fault_list.size();
      const std::string w = fault_list.substr(at, comma - at);
      at = comma + 1;
      if (w.empty()) continue;
      if (w == "fetch_order") faults.fetch_order = true;
      else if (w == "weights_as_fill") faults.weights_as_fill = true;
      else if (w == "xdword_colour") faults.xdword_colour = true;
      else if (w == "bone_zero") faults.bone_zero = true;
      else if (w == "uv_sign") faults.uv_sign = true;
      else {
        std::fprintf(stderr, "caster_vertex_check: unknown fault %s (fetch_order, weights_as_fill, xdword_colour, bone_zero, uv_sign)\n", w.c_str());
        return 3;
      }
    }
  }
  if (game.empty() || capture.empty()) {
    std::fprintf(stderr, "usage: caster_vertex_check --game DIR --capture DIR [--arena N] [--step N] [--fault LIST] [--lines]\n");
    return 3;
  }
  RuleChecks(faults);
  CaptureReader cap;
  if (!cap.Open(capture)) {
    std::fprintf(stderr, "caster_vertex_check: FAIL cannot open the capture at %s\n", capture.c_str());
    return 2;
  }
  if (!assets::ring_gs::Init(game)) {
    std::fprintf(stderr, "caster_vertex_check: FAIL no game files at %s\n", game.c_str());
    return 2;
  }
  assets::ring_gs::RingArena ring;
  {
    std::string err;
    if (!assets::ring_gs::Load(arena, ring, err)) {
      std::fprintf(stderr, "caster_vertex_check: FAIL ring of arena %d: %s\n", arena, err.c_str());
      return 2;
    }
  }
  // chCommon.pac entry 0 (the wrestlers' plane-shadow proxy)
  assets::Bytes proxy_blob;
  assets::jboy_parts::Parts proxy_parts;
  {
    launcher::GameFiles files;
    files.SetRoot(std::filesystem::u8path(game));
    assets::pac::Pac pc;
    std::string err;
    if (!assets::pac::Open(files, "pac\\ch\\chCommon.pac", pc, err)) {
      std::fprintf(stderr, "caster_vertex_check: FAIL chCommon.pac: %s\n", err.c_str());
      return 2;
    }
    const assets::pac::Ent* e = nullptr;
    for (const assets::pac::Pach& h : pc.pachs)
      if ((e = assets::pac::Find(h, 0)) != nullptr) {
        proxy_blob = assets::pac::Decode(pc.bytes.data() + e->off, e->size);
        break;
      }
    if (!e || !assets::jboy_parts::Parse(proxy_blob, proxy_parts, err)) {
      std::fprintf(stderr, "caster_vertex_check: FAIL chCommon.pac entry 0: %s\n", err.empty() ? "missing" : err.c_str());
      return 2;
    }
  }
  assets::wrestler_roster::Roster roster;
  {
    std::string err;
    if (!roster.Build(game, err)) {
      std::fprintf(stderr, "caster_vertex_check: FAIL roster: %s\n", err.c_str());
      return 2;
    }
  }
  std::vector<Proxy> proxies;
  for (const assets::ring_gs::RingMesh& m : ring.meshes)
    if (m.entry == 956) proxies.push_back({kRing, &m.data, m.part});
  const size_t ring_proxies = proxies.size();
  for (size_t i = 0; i < proxy_parts.parts.size(); ++i)
    if (proxy_parts.parts[i].ok && proxy_parts.parts[i].tiles) proxies.push_back({kProxy, &proxy_parts.parts[i], int(i)});
  std::printf("INFO  files: roster %zu pacs, %zu model entries; ring 956 meshes %zu; chCommon entry 0 meshes %zu\n", roster.pacs(), roster.entries().size(), ring_proxies, proxies.size() - ring_proxies);
  if (ring_proxies == 0 || proxies.size() == ring_proxies) {
    std::fprintf(stderr, "caster_vertex_check: FAIL no plane-shadow proxy meshes in the files\n");
    return 2;
  }

  // ---- state ----
  Tally tally[kKlassCount];
  uint64_t not_ours_nocolour = 0, not_ours_unjoined = 0, plane_unjoined = 0, map_draws = 0, plane_draws = 0, bad_geometry = 0, bodies_joined = 0, bodies_failed = 0;
  std::map<std::string, uint64_t> refused_why;
  std::unordered_map<uint32_t, Body> bodies;
  std::unordered_map<uint32_t, uint64_t> failed_sig;   // object -> sig of a failed join (not retried until the sig changes)
  std::unordered_map<int, std::shared_ptr<assets::wrestler_roster::Model>> model_cache;
  std::map<uint64_t, int> proxy_cache;   // (vertex_offset, count) -> proxy index or -1
  struct Prepared { std::vector<uint8_t> bytes; bool ok = false; };
  std::map<uint64_t, Prepared> prepared;
  // uv probe
  sv::UvSign uv_mode = sv::UvSign::Unknown;
  uint64_t uv_vertices = 0, uv_negative = 0, uv_bad = 0, uv_draws = 0, uv_mode_conflict = 0, uv_unknown_refused = 0;
  std::set<uint64_t> uv_done;

  LoadedFrame fr;
  std::vector<uint8_t> vb, cvb;
  std::unordered_map<uint64_t, const assets::jboy_parts::Part*> ring_cache;
  // The uv element of a colour draw against the writer's PackUv, with the u-sign rule learned from the stream's own bytes (first mesh with a negative u), then held for every other mesh.
  auto uv_probe = [&](const assets::jboy_parts::Part& part, const rdr_adapter::Draw& cd, uint64_t key) {
    if (!uv_done.insert(key ^ (cd.vertex_offset << 8)).second || cd.vertex_count != part.vertices || cd.layout >= fr.layouts.count) return;
    const rdr_adapter::VertexLayout& cl = fr.layouts[cd.layout];
    int uv_off = -1;
    for (uint32_t e = 0; e < cl.element_count; ++e)
      if (fr.elements[cl.element_first + e].source_format == 31 && fr.elements[cl.element_first + e].vk_format == 83) uv_off = fr.elements[cl.element_first + e].host_offset;
    if (uv_off < 0 || !cap.ReadGeometry(cd.vertex_offset, cd.vertex_bytes, cvb) || cvb.size() < size_t(cd.vertex_count) * cl.host_stride) return;
    ++uv_draws;
    const sv::UvSign here = sv::LearnUvSign(part, cvb.data(), cvb.size(), cl.host_stride, uint32_t(uv_off));
    if (uv_mode == sv::UvSign::Unknown) uv_mode = here;
    else if (here != sv::UvSign::Unknown && here != uv_mode) ++uv_mode_conflict;
    if (sv::FirstNegativeU(part) >= 0 && uv_mode == sv::UvSign::Unknown) {
      ++uv_unknown_refused;
      return;
    }
    for (uint32_t v = 0; v < part.vertices; ++v) {
      const uint8_t* s2 = part.s[2] + 8ull * v;
      uint8_t h4[4];
      sv::PackUv(assets::BeF(s2), assets::BeF(s2 + 4), uv_mode, faults.uv_sign, h4);
      ++uv_vertices;
      uv_negative += assets::BeF(s2) < 0.0f;
      uv_bad += std::memcmp(h4, cvb.data() + size_t(v) * cl.host_stride + uint32_t(uv_off), 4) != 0;
    }
  };
  const int nframes = to >= 0 ? std::min(to, int(cap.frame_count())) : int(cap.frame_count());   // S101 lane 99: --to
  for (int f = from; f < nframes; f += step) {   // S101 lane 99: --from
    if (!cap.LoadFrame(uint32_t(f), fr)) {
      std::fprintf(stderr, "caster_vertex_check: FAIL cannot load frame %d\n", f);
      return 2;
    }
    std::map<uint32_t, std::vector<uint32_t>> colour_by_object;
    std::map<uint64_t, uint32_t> colour_by_part;
    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = fr.OriginOf(d);
      if (!o || o->object == 0 || (o->pass_site != kSiteColour && o->pass_site != kSiteMirror)) continue;
      if (fr.draws[d].layout >= fr.layouts.count || (fr.draws[d].flags & rdr_adapter::kDrawNoGeometry)) continue;
      colour_by_object[o->object].push_back(d);
      colour_by_part.emplace((uint64_t(o->object) << 32) | o->part, d);
    }
    auto elements_of = [&](const rdr_adapter::VertexLayout& lay, std::vector<sv::Element>& els) {
      els.clear();
      for (uint32_t e = 0; e < lay.element_count; ++e) {
        const rdr_adapter::VertexElement& el = fr.elements[lay.element_first + e];
        els.push_back({el.vk_format, el.host_offset, el.source_format, el.flags, el.mini, el.guest_offset});
      }
    };
    // ---- the wrestler join: through the object's colour draws (the lane 45 method) ----
    auto join_body = [&](uint32_t object) -> Body* {
      const auto& draws = colour_by_object[object];
      std::map<uint32_t, std::array<uint32_t, 2>> sig;
      for (uint32_t d : draws) sig[fr.OriginOf(d)->part] = {fr.draws[d].vertex_count, fr.draws[d].index_count};
      uint64_t h = 1469598103934665603ull;
      for (auto& [p, s] : sig)
        for (uint32_t v : {p, s[0], s[1]}) h = Mix(h, v);
      const auto bi = bodies.find(object);
      if (bi != bodies.end() && bi->second.sig == h) return &bi->second;
      const auto fi = failed_sig.find(object);
      if (fi != failed_sig.end() && fi->second == h) return nullptr;
      bodies.erase(object);
      std::vector<int> cands;
      const auto& entries = roster.entries();
      for (size_t ci = 0; ci < entries.size(); ++ci) {
        bool fits = true;
        for (auto& [p, s] : sig)
          // lane 192: the roster's PartInfo.index_count is the DRAW count (groups joined by 0xFFFF, wrestler_geom::DrawIndexCount), the same count the shadow core's index now has
          if (p >= entries[ci].parts.size() || entries[ci].parts[p].vertices != s[0] || entries[ci].parts[p].index_count != s[1]) { fits = false; break; }
        if (fits) cands.push_back(int(ci));
      }
      std::vector<uint32_t> order = draws;
      std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return fr.draws[a].vertex_count > fr.draws[b].vertex_count; });
      if (order.size() > 6) order.resize(6);
      std::vector<sv::Element> dummy;
      for (int ci : cands) {
        auto& slot = model_cache[ci];
        if (!slot) {
          std::string err;
          slot = roster.LoadModel(ci, err);
        }
      }
      // does the colour draw `d` rebuild bit-for-bit (lane 45's rules) from `part`? `scale` 0: measure it (the body's one scale), else it is fixed.
      auto colour_twin = [&](const assets::jboy_parts::Part& part, uint32_t d, float& scale) -> bool {
        const rdr_adapter::Draw& dr = fr.draws[d];
        std::vector<uint8_t> cv;
        if (!cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, cv) || cv.size() < dr.vertex_bytes || part.vertices != dr.vertex_count) return false;
        const rdr_adapter::VertexLayout& lay = fr.layouts[dr.layout];
        std::vector<wg::LayoutElement> els;
        for (uint32_t e = 0; e < lay.element_count; ++e) {
          const rdr_adapter::VertexElement& el = fr.elements[lay.element_first + e];
          els.push_back({el.vk_format, el.host_offset, el.source_format, el.components, el.flags, el.guest_offset});
        }
        wg::Roles roles;
        if (!wg::Classify(part, els.data(), els.size(), lay.host_stride, lay.guest_stride, roles)) return false;
        if (scale == 0) {
          double residual = 0;
          const float fit = wg::FitScale(part, roles, cv.data(), &residual);
          if (!(fit > 0.1f && fit < 10.0f) || residual > 1e-5) return false;
          scale = fit;
        }
        wg::BuildOptions bo;
        bo.scale = scale;
        bo.fill0 = 0;   // the colour layout's fill0 mode (rigid parts): {0,0,0,255} (1) or {0,0,0,hi byte of the position x float} (2)
        for (const auto& r : roles.r) {
          if (r.role != wg::Role::Fill0) continue;
          const uint8_t b3 = cv[r.offset + 3];
          float x = assets::BeF(part.s[0]) * scale;
          uint32_t xb;
          std::memcpy(&xb, &x, 4);
          bo.fill0 = (cv[r.offset] || cv[r.offset + 1] || cv[r.offset + 2]) ? 3 : b3 == 255 ? 1 : uint8_t(xb >> 24) == b3 ? 2 : 3;
        }
        if (bo.fill0 > 2) return false;
        std::vector<uint8_t> ours;
        wg::BuildVertices(part, roles, bo, ours);
        wg::CompareResult cr;
        wg::Compare(roles, ours, cv.data(), part.vertices, cr);
        return cr.all_ok;
      };
      for (int ci : cands) {
        auto& model = model_cache[ci];
        if (!model) continue;
        float scale = 0;
        bool ok = false;
        for (uint32_t d : order) {   // the body's scale from the biggest draws, then EVERY colour draw of the object must rebuild with it (a candidate that fits one part only is not the body)
          const uint32_t part_ix = fr.OriginOf(d)->part;
          if (part_ix >= model->parts.parts.size()) continue;
          if (colour_twin(model->parts.parts[part_ix], d, scale)) { ok = true; break; }
          scale = 0;
        }
        if (!ok) continue;
        for (uint32_t d : draws) {
          const uint32_t part_ix = fr.OriginOf(d)->part;
          if (part_ix >= model->parts.parts.size() || !colour_twin(model->parts.parts[part_ix], d, scale)) { ok = false; break; }
        }
        if (!ok) continue;
        Body b;
        b.model = model;
        b.scale = scale;
        b.fill = sv::FillMode::HiByteX;
        b.sig = h;
        b.entry = ci;
        ++bodies_joined;
        return &(bodies[object] = b);
      }
      ++bodies_failed;
      failed_sig[object] = h;
      return nullptr;
    };

    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = fr.OriginOf(d);
      if (!o || (o->pass_site != kSiteMap && o->pass_site != kSitePlane)) continue;
      const rdr_adapter::Draw& dr = fr.draws[d];
      if ((dr.flags & rdr_adapter::kDrawNoGeometry) || dr.layout >= fr.layouts.count || !dr.index_bytes) {
        ++bad_geometry;
        continue;
      }
      const bool plane = o->pass_site == kSitePlane;
      ++(plane ? plane_draws : map_draws);
      if (!cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) {
        ++bad_geometry;
        continue;
      }
      const rdr_adapter::VertexLayout& lay = fr.layouts[dr.layout];
      std::vector<sv::Element> els;
      elements_of(lay, els);
      const assets::jboy_parts::Part* part = nullptr;
      sv::Options opt;
      Klass kl = kWrestler;
      uint64_t model_key = 0;
      if (plane) {
        const uint64_t ck = (dr.vertex_offset << 20) ^ dr.vertex_count;
        auto it = proxy_cache.find(ck);
        if (it == proxy_cache.end()) {
          int hit = -1;
          for (size_t pi = 0; pi < proxies.size() && hit < 0; ++pi) {
            const assets::jboy_parts::Part& p = *proxies[pi].part;
            if (p.vertices != dr.vertex_count || dr.vertex_bytes != size_t(dr.vertex_count) * lay.host_stride) continue;
            bool all = true;
            for (uint32_t v = 0; v < p.vertices && all; ++v) {
              for (int c = 0; c < 3 && all; ++c) {
                float fl;
                std::memcpy(&fl, vb.data() + size_t(v) * lay.host_stride + 4 * c, 4);
                all = assets::BeF(p.s[0] + 28ull * v + 4 * c) == fl;
              }
            }
            if (all) hit = int(pi);
          }
          it = proxy_cache.emplace(ck, hit).first;
        }
        if (it->second < 0) {
          ++plane_unjoined;
          continue;
        }
        const Proxy& px = proxies[size_t(it->second)];
        part = px.part;
        kl = px.klass;
        opt.scale = 1.0f;
        opt.fill = px.klass == kRing ? sv::FillMode::Weight255 : sv::FillMode::HiByteX;
        model_key = (uint64_t(it->second) << 8) | 0x80;
        if (px.mesh != int(o->part)) ++tally[kl].part_not_mesh;
      } else {
        if (!colour_by_object.count(o->object)) {
          ++not_ours_nocolour;
          continue;
        }
        Body* b = join_body(o->object);
        if (!b) {
          ++not_ours_unjoined;
          continue;
        }
        if (o->part >= b->model->parts.parts.size()) {
          ++tally[kWrestler].part_not_mesh;
          continue;
        }
        part = &b->model->parts.parts[o->part];
        opt.scale = b->scale;
        opt.fill = b->fill;
        model_key = (uint64_t(uint32_t(b->entry)) << 24) ^ (uint64_t(o->part) << 8) ^ (uint64_t(std::bit_cast<uint32_t>(b->scale)) << 40);
      }
      Tally& t = tally[kl];
      ++t.draws;
      if (part->vertices != dr.vertex_count) {
        ++t.count_off;
        continue;
      }
      uint64_t lsig = 1469598103934665603ull;
      for (const sv::Element& e : els)
        for (uint64_t v : {uint64_t(e.vk_format), uint64_t(e.host_offset), uint64_t(e.source_format), uint64_t(e.flags), uint64_t(e.mini), uint64_t(e.guest_offset)}) lsig = Mix(lsig, v);
      const uint64_t pkey = Mix(Mix(model_key, lsig), lay.host_stride);
      auto pit = prepared.find(pkey);
      if (pit == prepared.end()) {
        Prepared pr;
        sv::Layout sl;
        if (!sv::Classify(*part, els.data(), els.size(), lay.host_stride, lay.guest_stride, sl)) ++refused_why[sl.why];
        else pr.ok = sv::Write(*part, sl, opt, faults, pr.bytes);
        pit = prepared.emplace(pkey, std::move(pr)).first;
        if (pit->second.ok) {   // the layout's roles for the per-role compare are kept beside it (rebuilt below from the layout)
        }
      }
      if (!pit->second.ok) {
        ++t.refused;
        continue;
      }
      sv::Layout sl;
      sv::Classify(*part, els.data(), els.size(), lay.host_stride, lay.guest_stride, sl);
      const std::vector<uint8_t>& ours = pit->second.bytes;
      t.vertices += dr.vertex_count;
      const uint32_t stride = lay.host_stride;
      for (uint32_t v = 0; v < dr.vertex_count; ++v) {
        const uint8_t* a = ours.data() + size_t(v) * stride;
        const uint8_t* b2 = vb.data() + size_t(v) * stride;
        bool any = false;
        for (const sv::Layout::R& r : sl.r) {
          const size_t n = r.role == sv::Role::Pos ? 16 : r.role == sv::Role::Weights ? 12 : 4;
          ++t.role_values[uint8_t(r.role)];
          if (std::memcmp(a + r.offset, b2 + r.offset, n) != 0) ++t.bad_role[uint8_t(r.role)], any = true;
        }
        if (any || std::memcmp(a, b2, stride) != 0) ++t.bad_vertices;
      }
      if (lines && !plane && (f % 100) == 0) std::printf("DRAW frame %d draw %u class %s part %u verts %u\n", f, d, kKlassName[kl], o->part, dr.vertex_count);

      // ---- the uv probe on this part's colour draw (wrestler objects) ----
      if (!plane) {
        const auto cit = colour_by_part.find((uint64_t(o->object) << 32) | o->part);
        if (cit != colour_by_part.end()) uv_probe(*part, fr.draws[cit->second], model_key);
      }
    }
    // ---- the uv probe on the ring's colour draws: the ring files carry negative u (the wrestlers' do not), joined by content (counts + bit-equal positions) ----
    for (const auto& [object, draws] : colour_by_object) {
      for (uint32_t cd_i : draws) {
        const rdr_adapter::Draw& cd = fr.draws[cd_i];
        const uint64_t ck = (cd.vertex_offset << 20) ^ cd.vertex_count;
        auto it = ring_cache.find(ck);
        if (it == ring_cache.end()) {
          const assets::jboy_parts::Part* hit = nullptr;
          const rdr_adapter::VertexLayout& cl = fr.layouts[cd.layout];
          if (cd.vertex_count > 0 && cd.vertex_bytes == size_t(cd.vertex_count) * cl.host_stride) {
            bool any_count = false;
            for (const assets::ring_gs::RingMesh& m : ring.meshes) any_count = any_count || m.vertices == cd.vertex_count;
            if (any_count && cap.ReadGeometry(cd.vertex_offset, cd.vertex_bytes, cvb) && cvb.size() >= cd.vertex_bytes) {
              for (const assets::ring_gs::RingMesh& m : ring.meshes) {
                if (m.vertices != cd.vertex_count) continue;
                bool all = true;
                for (uint32_t v = 0; v < m.vertices && all; ++v)
                  for (int c = 0; c < 3 && all; ++c) {
                    float fl;
                    std::memcpy(&fl, cvb.data() + size_t(v) * cl.host_stride + 4 * c, 4);
                    all = assets::BeF(m.data.s[0] + 28ull * v + 4 * c) == fl;
                  }
                if (all) {
                  hit = &m.data;
                  break;
                }
              }
            }
          }
          it = ring_cache.emplace(ck, hit).first;
        }
        if (it->second) uv_probe(*it->second, cd, (ck << 4) ^ 0x5);
      }
    }
  }

  uint64_t total_draws = 0, total_vertices = 0, total_bad = 0, total_refused = 0, total_part = 0, total_count = 0;
  uint64_t role_bad[6] = {}, role_values[6] = {};
  for (int k = 0; k < kKlassCount; ++k) {
    const Tally& t = tally[k];
    std::printf("INFO  %s: %llu draws, %llu vertices, %llu refused, %llu part != mesh, %llu count off, %llu vertices differ\n", kKlassName[k], (unsigned long long)t.draws,
                (unsigned long long)t.vertices, (unsigned long long)t.refused, (unsigned long long)t.part_not_mesh, (unsigned long long)t.count_off, (unsigned long long)t.bad_vertices);
    total_draws += t.draws, total_vertices += t.vertices, total_bad += t.bad_vertices, total_refused += t.refused, total_part += t.part_not_mesh, total_count += t.count_off;
    for (int r = 0; r < 6; ++r) role_bad[r] += t.bad_role[r], role_values[r] += t.role_values[r];
  }
  std::printf("INFO  frames %d (step %d): map-pass draws %llu, plane-pass draws %llu; bodies joined %llu, failed %llu; not ours: no colour twin %llu, unjoined %llu; plane unjoined %llu; unreadable %llu\n",
              nframes, step, (unsigned long long)map_draws, (unsigned long long)plane_draws, (unsigned long long)bodies_joined, (unsigned long long)bodies_failed,
              (unsigned long long)not_ours_nocolour, (unsigned long long)not_ours_unjoined, (unsigned long long)plane_unjoined, (unsigned long long)bad_geometry);
  for (const auto& [why, n] : refused_why) std::printf("INFO  layout refused (%llu): %s\n", (unsigned long long)n, why.c_str());
  std::printf("INFO  uv probe (wrestler + ring colour draws): %llu colour draws, %llu vertices (%llu with a negative file u), rule learned from the stream: %s, %llu conflicts, %llu meshes left open\n", (unsigned long long)uv_draws,
              (unsigned long long)uv_vertices, (unsigned long long)uv_negative, uv_mode == sv::UvSign::Unknown ? "unknown" : uv_mode == sv::UvSign::Abs ? "|u|" : "signed",
              (unsigned long long)uv_mode_conflict, (unsigned long long)uv_unknown_refused);

  Check("sv.compared", tally[kWrestler].draws > 0 && tally[kRing].draws > 0 && tally[kProxy].draws > 0 && total_refused == 0 && total_count == 0 && bad_geometry == 0,
        "wrestler %llu / ring956 %llu / chcommon %llu draws, %llu vertices, %llu refused, %llu count off, %llu unreadable", (unsigned long long)tally[kWrestler].draws,
        (unsigned long long)tally[kRing].draws, (unsigned long long)tally[kProxy].draws, (unsigned long long)total_vertices, (unsigned long long)total_refused, (unsigned long long)total_count,
        (unsigned long long)bad_geometry);
  Check("sv.joined", plane_unjoined == 0 && bodies_joined > 0, "%llu plane draws joined to no file mesh, %llu wrestler bodies joined", (unsigned long long)plane_unjoined, (unsigned long long)bodies_joined);
  Check("sv.part_is_mesh", total_part == 0, "%llu draws whose DrawOrigin.part is not the file mesh record", (unsigned long long)total_part);
  Check("sv.position_exact", role_values[0] > 0 && role_bad[0] == 0, "%llu of %llu position elements differ", (unsigned long long)role_bad[0], (unsigned long long)role_values[0]);
  Check("sv.weights_exact", role_values[1] > 0 && role_bad[1] == 0, "%llu of %llu weight elements differ", (unsigned long long)role_bad[1], (unsigned long long)role_values[1]);
  Check("sv.ids_exact", role_values[2] > 0 && role_bad[2] == 0, "%llu of %llu bone-id elements differ", (unsigned long long)role_bad[2], (unsigned long long)role_values[2]);
  Check("sv.fill_exact", role_values[3] > 0 && role_bad[3] == 0, "%llu of %llu fill (dword 0) elements differ", (unsigned long long)role_bad[3], (unsigned long long)role_values[3]);
  Check("sv.xdword_exact", role_values[4] > 0 && role_bad[4] == 0, "%llu of %llu xdword (mini fetch of dword 0) elements differ", (unsigned long long)role_bad[4], (unsigned long long)role_values[4]);
  Check("sv.bytes_equal", total_vertices > 0 && total_bad == 0, "%llu of %llu vertices differ in any byte", (unsigned long long)total_bad, (unsigned long long)total_vertices);
  Check("sv.uv_exact", uv_vertices > 0 && uv_negative > 0 && uv_mode != sv::UvSign::Unknown && uv_bad == 0 && uv_mode_conflict == 0, "%llu of %llu uv words differ from the stream's (%llu negative-u vertices; rule %s)",
        (unsigned long long)uv_bad, (unsigned long long)uv_vertices, (unsigned long long)uv_negative, uv_mode == sv::UvSign::Abs ? "|u|" : uv_mode == sv::UvSign::Signed ? "signed" : "unknown");
  std::printf("RESULT %s fault=%s draws=%llu vertices=%llu failed=%d\n", g_fail ? "FAIL" : "OK", fault_list.empty() ? "none" : fault_list.c_str(), (unsigned long long)total_draws,
              (unsigned long long)total_vertices, g_fail);
  return g_fail ? 1 : 0;
}
