// prop_vertex_check: the unit test of the skinned prop host vertex writer (src\assets\prop_host_vertex.h; Session 101, lane 49). 0 game runs, no GPU.
//   prop_vertex_check --game DIR --capture DIR [--arena N] [--step N] [--fault NAME]
// For every draw of the capture whose vertex layout is the skinned prop shape (six elements: position f32x4 @0, weights u8x4 @16, bone ids u8x4 UINT @20, normal s16x4 @24,
// colour u8x4 @32, uv half2 @36, stride 40), the stage mesh whose positions equal the stream's (brute force over the arena's stage meshes, bit for bit) is found, and the SHIPPED
// writer (prop_host::MakeHostVertex, with the words from SinglePairWords as the arena loader builds them) must produce the stream's bytes on EVERY vertex:
// position exact, weights exact, bone ids exact, colour exact, uv exact (u = |file u|), normal within one packed step of 1/32767 * 33 (0.001).
// --fault NAME plants a writer fault inside the shipped code: colour_order, uv_halves_swapped, bone_zero, weight_word. The run then must FAIL its own exact check.
// Exit: 0 all hold, 1 a check failed, 2 a loud failure (cannot read the input), 3 bad usage.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/assets/arena_game_space.h"
#include "../../src/assets/prop_host_vertex.h"
#include "../../src/renderer_adapter/capture_reader.h"

namespace {
using assets::arena_gs::StageArena;
using assets::arena_gs::StageMesh;
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;

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

// A writer self-check that needs no capture: the rule itself, and that every planted fault changes the bytes it claims to.
void RuleChecks() {
  using namespace assets::prop_host;
  const uint8_t one[4] = {7, 255, 255, 255}, two[4] = {7, 2, 255, 255};
  const float w1[4] = {1.f, 0, 0, 0}, w2[4] = {0.5f, 0.5f, 0, 0};
  SkinWords a{};
  Check("rule.single_pair", SinglePairWords(1, one, w1, Faults{}, a) && a.ids[0] == 7 && a.ids[1] == 255 && a.ids[2] == 255 && a.ids[3] == 255 && a.weights[0] == 0 && a.weights[3] == 255,
        "ids %u %u %u %u weights %u %u %u %u", a.ids[0], a.ids[1], a.ids[2], a.ids[3], a.weights[0], a.weights[1], a.weights[2], a.weights[3]);
  SkinWords b{};
  Check("rule.refuses_other_shapes", !SinglePairWords(2, two, w2, Faults{}, b) && !SinglePairWords(1, one, w2, Faults{}, b) && !SinglePairWords(1, two, w1, Faults{}, b), "two pairs / weight != 1 / second slot used");
  Faults f;
  f.bone_zero = true;
  SkinWords c{};
  SinglePairWords(1, one, w1, f, c);
  Faults g;
  g.weight_word = true;
  SkinWords d{};
  SinglePairWords(1, one, w1, g, d);
  Check("rule.plants_change_bytes", c.ids[0] == 0 && d.weights[0] == 255 && d.weights[3] == 0, "bone_zero ids[0] %u, weight_word %u..%u", c.ids[0], d.weights[0], d.weights[3]);
  Check("rule.half_truncates", FloatToHalfTruncate(1.0f) == 0x3C00 && FloatToHalfTruncate(3.7e-8f) == 0 && FloatToHalfTruncate(-2.0f) == 0xC000, "1.0 -> %04x, 3.7e-8 -> %04x, -2 -> %04x", FloatToHalfTruncate(1.0f), FloatToHalfTruncate(3.7e-8f), FloatToHalfTruncate(-2.0f));
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, capture, fault;
  int arena = 4, step = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game") game = next();
    else if (a == "--capture") capture = next();
    else if (a == "--fault") fault = next();
    else if (a == "--arena") arena = std::atoi(next());
    else if (a == "--step") step = std::max(1, std::atoi(next()));
    else {
      std::fprintf(stderr, "prop_vertex_check: unknown argument %s\n", a.c_str());
      return 3;
    }
  }
  assets::prop_host::Faults faults;
  if (fault == "colour_order") faults.colour_order = true;
  else if (fault == "uv_halves_swapped") faults.uv_halves_swapped = true;
  else if (fault == "bone_zero") faults.bone_zero = true;
  else if (fault == "weight_word") faults.weight_word = true;
  else if (!fault.empty()) {
    std::fprintf(stderr, "prop_vertex_check: unknown fault %s\n", fault.c_str());
    return 3;
  }
  if (game.empty() || capture.empty()) {
    std::fprintf(stderr, "usage: prop_vertex_check --game DIR --capture DIR [--arena N] [--step N] [--fault colour_order|uv_halves_swapped|bone_zero|weight_word]\n");
    return 3;
  }
  RuleChecks();
  CaptureReader cap;
  if (!cap.Open(capture)) {
    std::fprintf(stderr, "prop_vertex_check: FAIL cannot open the capture at %s\n", capture.c_str());
    return 2;
  }
  if (!assets::arena_gs::Init(game)) {
    std::fprintf(stderr, "prop_vertex_check: FAIL no game files at %s\n", game.c_str());
    return 2;
  }
  StageArena stage;
  std::string err;
  if (!assets::arena_gs::Load(arena, stage, err)) {
    std::fprintf(stderr, "prop_vertex_check: FAIL arena %d: %s\n", arena, err.c_str());
    return 2;
  }
  std::unordered_map<uint32_t, std::vector<int>> by_count;
  size_t skinned_meshes = 0;
  for (size_t i = 0; i < stage.meshes.size(); ++i) {
    by_count[uint32_t(stage.meshes[i].vertices.size())].push_back(int(i));
    skinned_meshes += stage.meshes[i].skinned;
  }

  uint64_t draws_shape = 0, draws_joined = 0, not_stage = 0, not_skinned = 0, layout_bad = 0, vertices = 0;
  uint64_t bad_pos = 0, bad_w = 0, bad_ids = 0, bad_col = 0, bad_uv = 0, bad_normal = 0;
  int worst_normal = 0;
  std::set<int> slots;
  LoadedFrame L;
  const int nframes = int(cap.frame_count());
  for (int f = 0; f < nframes; f += step) {
    if (!cap.LoadFrame(uint32_t(f), L)) {
      std::fprintf(stderr, "prop_vertex_check: FAIL cannot load frame %d\n", f);
      return 2;
    }
    for (uint32_t d = 0; d < L.draws.count; ++d) {
      const rdr_adapter::Draw& dr = L.draws[d];
      if ((dr.flags & rdr_adapter::kDrawNoGeometry) || dr.layout == 0xFFFFFFFFu) continue;
      const rdr_adapter::VertexLayout& lay = L.layouts[dr.layout];
      if (lay.element_count != 6 || lay.host_stride != 40) continue;
      const rdr_adapter::VertexElement* e = &L.elements[lay.element_first];
      const bool shape = e[0].vk_format == 109 && e[0].host_offset == 0 && e[1].vk_format == 37 && e[1].host_offset == 16 && e[2].vk_format == 41 && e[2].host_offset == 20 &&
                         e[3].vk_format == 92 && e[3].host_offset == 24 && e[4].vk_format == 37 && e[4].host_offset == 32 && e[5].vk_format == 83 && e[5].host_offset == 36;
      if (!shape) continue;
      ++draws_shape;
      std::vector<uint8_t> vb;
      if (!cap.ReadGeometry(dr.vertex_offset, dr.vertex_count * 40, vb) || vb.size() < size_t(dr.vertex_count) * 40) {
        ++layout_bad;
        continue;
      }
      const StageMesh* hit = nullptr;
      const auto it = by_count.find(dr.vertex_count);
      if (it != by_count.end()) {
        for (int mi : it->second) {
          const StageMesh& m = stage.meshes[size_t(mi)];
          bool all = true;
          for (size_t i = 0; i < m.vertices.size() && all; ++i) all = std::memcmp(vb.data() + i * 40, m.vertices[i].pos, 12) == 0;
          if (all) {
            hit = &m;
            break;
          }
        }
      }
      if (!hit) {
        ++not_stage;
        continue;
      }
      if (!hit->skinned) {
        ++not_skinned;
        continue;
      }
      ++draws_joined;
      slots.insert(hit->slot);
      for (size_t i = 0; i < hit->vertices.size(); ++i) {
        const assets::arena_gs::Vertex& fv = hit->vertices[i];
        // the words as the shipped loader built them (SinglePairWords), with the plant applied inside the same writer
        assets::prop_host::SkinWords words;
        const uint8_t slot_bytes[4] = {hit->skin_ids[4 * i], 255, 255, 255};
        const float wf[4] = {1.f, 0, 0, 0};
        if (!assets::prop_host::SinglePairWords(1, slot_bytes, wf, faults, words)) {
          ++bad_ids;
          continue;
        }
        const float uv[2] = {fv.uv[0], fv.uv[1]};
        const assets::prop_host::HostVertex ours = assets::prop_host::MakeHostVertex(fv.pos, fv.normal, fv.color, uv, words, /*abs_u=*/true, assets::prop_host::ColourOrder{}, faults);
        const uint8_t* s = vb.data() + i * 40;
        ++vertices;
        float sp[4];
        std::memcpy(sp, s, 16);
        bad_pos += std::memcmp(sp, ours.position, 12) != 0 || sp[3] != 1.0f;
        bad_w += std::memcmp(s + 16, ours.weights, 4) != 0;
        bad_ids += std::memcmp(s + 20, ours.ids, 4) != 0;
        bad_col += std::memcmp(s + 32, ours.colour, 4) != 0;
        bad_uv += std::memcmp(s + 36, ours.uv, 4) != 0;
        int16_t q[4];
        std::memcpy(q, s + 24, 8);
        int worst = 0;
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(int(q[k]) - int(ours.normal[k])));
        if (q[3] != 0) worst = 99999;
        worst_normal = std::max(worst_normal, worst);
        bad_normal += worst > 33;
      }
    }
  }
  std::printf("INFO  arena %d: %zu skinned stage meshes; %llu draws of the skinned prop shape in %d frames, %llu joined to a skinned stage mesh (slots %zu), %llu not a stage mesh, %llu not skinned, %llu unreadable\n", arena,
              skinned_meshes, (unsigned long long)draws_shape, nframes, (unsigned long long)draws_joined, slots.size(), (unsigned long long)not_stage, (unsigned long long)not_skinned, (unsigned long long)layout_bad);
  Check("prop.compared", draws_joined > 0 && vertices > 0 && layout_bad == 0 && not_skinned == 0, "%llu draws, %llu vertices", (unsigned long long)draws_joined, (unsigned long long)vertices);
  Check("prop.position_exact", vertices > 0 && bad_pos == 0, "%llu of %llu vertices differ", (unsigned long long)bad_pos, (unsigned long long)vertices);
  Check("prop.weights_exact", vertices > 0 && bad_w == 0, "%llu of %llu weight words differ from the stream's", (unsigned long long)bad_w, (unsigned long long)vertices);
  Check("prop.ids_exact", vertices > 0 && bad_ids == 0, "%llu of %llu bone-id words differ from the stream's", (unsigned long long)bad_ids, (unsigned long long)vertices);
  Check("prop.colour_exact", vertices > 0 && bad_col == 0, "%llu of %llu colour words differ from the stream's", (unsigned long long)bad_col, (unsigned long long)vertices);
  Check("prop.uv_exact", vertices > 0 && bad_uv == 0, "%llu of %llu uv words differ from the stream's (u = |file u|)", (unsigned long long)bad_uv, (unsigned long long)vertices);
  Check("prop.normal_within_step", vertices > 0 && bad_normal == 0, "%llu of %llu normals off by more than 33 packed units; worst %d (%.5f)", (unsigned long long)bad_normal, (unsigned long long)vertices, worst_normal, worst_normal / 32767.0);
  std::printf("RESULT %s fault=%s draws=%llu vertices=%llu failed=%d\n", g_fail ? "FAIL" : "OK", fault.empty() ? "none" : fault.c_str(), (unsigned long long)draws_joined, (unsigned long long)vertices, g_fail);
  return g_fail ? 1 : 0;
}
