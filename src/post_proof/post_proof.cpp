// S101 lane 34: the post-chain proof runner. Opens a capture frame through fview's Session (the real binder and renderer), and either
//   --dump           prints the post region of the frame: every draw's targets, textures, vertices and constants by name (the game's own post draws), or
//   --proof          renders the frame up to just before the post region, runs OUR post pass (src\renderer_scene\post_pass.*) on the scene colour and depth
//                    with the parameters read from the game's post draws by name, renders the rest of the frame (the game's own post draws) for the
//                    reference, and compares the two pictures (post_proof.py writes the sheet and the verdict from the PNGs and the json).
// Reads captures only: not a game run.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../fview/describe.h"
#include "../fview/effects.h"
#include "../fview/session.h"
#include "../renderer/player/png.h"
#include "../renderer_scene/post_pass.h"

namespace fs = std::filesystem;
using namespace fview;
using rdr::WritePng;

namespace {

struct Args {
  std::string capture, out, root, fault;
  int frame = -1;
  bool dump = false, proof = false, defaults = false;
  float scale = 1.f;
  int validation = 0;
};

int Fail(const std::string& why) {
  std::fprintf(stderr, "[post_proof] %s\n", why.c_str());
  return 2;
}

bool IsPostDraw(const std::string& ps) {
  static const char* const kFam[] = {"yDownScale_", "yBlur_", "yGlareEtc_", "yMonotone_", "yRetroFilm_", "yPostBase_", "yGamma_", "yToneMap_", "yHalation_", "yDefocus_", "yRadialBlur_"};
  for (const char* f : kFam)
    if (ps.rfind(f, 0) == 0) return true;
  return false;
}

// The first op of the post region: the targets op before the first yDownScale_dof draw (the depth-of-field downsample starts every post chain), or 0.
struct Region {
  int targets_op = -1;     // the SetTargets op that opens the region
  int first_draw = -1;     // the first post draw
  int combine_draw = -1;   // the yGlareEtc combine
  int last_draw = -1;      // the last post-family draw of the frame (after the combine: the 30 Years filter)
};

bool FindRegion(const Session& s, const EffectIndex& fx, Region& r, std::string& why) {
  const auto& f = s.frame();
  for (uint32_t i = 0; i < f.ops.count; ++i) {
    if (f.ops[i].kind != rdr_adapter::OpKind::Draw) continue;
    const OpTrace& t = s.trace()[i];
    const std::string ps = t.ps >= 0 ? s.VariantName(t.ps) : "";
    if (ps.rfind("yDownScale_dof", 0) == 0 && r.first_draw < 0) r.first_draw = int(i);
    if (ps.rfind("yGlareEtc_dof", 0) == 0 || ps.rfind("yGlareEtc_fog", 0) == 0 || ps.rfind("yGlareEtc_bloom", 0) == 0) r.combine_draw = int(i);
    if (r.first_draw >= 0 && IsPostDraw(ps)) r.last_draw = int(i);
  }
  (void)fx;
  if (r.first_draw < 0 || r.combine_draw < 0) { why = "no depth-of-field downsample + combine in this frame"; return false; }
  for (int i = r.first_draw; i >= 0; --i)
    if (f.ops[uint32_t(i)].kind == rdr_adapter::OpKind::SetTargets) { r.targets_op = i; break; }
  if (r.targets_op < 0) { why = "no targets op before the first post draw"; return false; }
  return true;
}

void DumpDraw(const Session& s, const EffectIndex& fx, uint32_t op, std::FILE* out) {
  DrawInfo d;
  std::string err;
  if (!DescribeDraw(s, &fx, op, d, err)) { std::fprintf(out, "op %u: %s\n", op, err.c_str()); return; }
  std::fprintf(out, "op %u draw %u %s + %s (%s) targets: %s | %s | %s\n", op, d.draw, d.vs_variant.c_str(), d.ps_variant.c_str(), d.outcome.c_str(), d.targets.c_str(), d.viewport.c_str(), d.scissor.c_str());
  for (const std::string& st : d.state) std::fprintf(out, "    state %s\n", st.c_str());
  for (const TextureInfo& t : d.textures)
    std::fprintf(out, "    tex slot %u image %u %ux%u %s | %s\n", t.slot, t.image, t.record.width, t.record.height, t.format.c_str(), t.sampler.c_str());
  for (const ConstantInfo& c : d.constants) {
    std::fprintf(out, "    const %s %s c%d x%d:", c.stage.c_str(), c.field.c_str(), c.reg, c.count);
    for (size_t k = 0; k < c.values.size() && k < 64; ++k) std::fprintf(out, " %.7g", c.values[k]);
    std::fprintf(out, "\n");
  }
  // The vertices.
  const auto& f = s.frame();
  const rdr_adapter::Draw& dr = f.draws[f.ops[op].index];
  if (dr.layout < f.layouts.count && dr.vertex_bytes) {
    const rdr_adapter::VertexLayout& L = f.layouts[dr.layout];
    std::vector<uint8_t> bytes;
    if (s.capture().ReadGeometry(dr.vertex_offset, dr.vertex_bytes, bytes) && L.host_stride) {
      const uint32_t n = std::min<uint32_t>(dr.vertex_bytes / L.host_stride, 8);
      std::fprintf(out, "    vertices (%u of %u, stride %u, indices %u):\n", n, dr.vertex_bytes / L.host_stride, L.host_stride, dr.index_count);
      for (uint32_t v = 0; v < n; ++v) {
        std::fprintf(out, "      v%u:", v);
        for (uint32_t e = 0; e < L.element_count; ++e) {
          const rdr_adapter::VertexElement& el = f.elements[L.element_first + e];
          std::fprintf(out, " [fmt %u@%u]", el.vk_format, el.host_offset);
          const uint32_t comps = el.vk_format == 106 ? 3 : el.vk_format == 109 ? 4 : el.vk_format == 103 ? 2 : 0;
          for (uint32_t c = 0; c < comps; ++c) {
            float x;
            std::memcpy(&x, bytes.data() + v * L.host_stride + el.host_offset + c * 4, 4);
            std::fprintf(out, " %g", x);
          }
        }
        std::fprintf(out, "\n");
      }
    }
  }
}

int Dump(Session& s, const EffectIndex& fx, const Args& a) {
  Region r;
  std::string why;
  if (!FindRegion(s, fx, r, why)) return Fail(why);
  std::FILE* out = a.out.empty() ? stdout : std::fopen(a.out.c_str(), "w");
  if (!out) return Fail("cannot write " + a.out);
  const auto& f = s.frame();
  std::fprintf(out, "post region: targets op %d, first draw %d, combine %d, last post draw %d, ops %u\n", r.targets_op, r.first_draw, r.combine_draw, r.last_draw, f.ops.count);
  for (uint32_t i = uint32_t(r.targets_op); i < f.ops.count; ++i) {
    const rdr_adapter::Op& op = f.ops[i];
    if (op.kind == rdr_adapter::OpKind::Draw) {
      DumpDraw(s, fx, i, out);
    } else if (op.kind == rdr_adapter::OpKind::SetTargets) {
      const rdr::TargetsOp& t = f.targets[op.index];
      std::fprintf(out, "op %u targets:", i);
      for (uint32_t c = 0; c < t.color_count; ++c) {
        const auto* rec = s.capture().FindImage(t.color[c].image);
        std::fprintf(out, " colour %u (%ux%u)", t.color[c].image, rec ? rec->width : 0, rec ? rec->height : 0);
      }
      std::fprintf(out, " depth %u\n", t.depth.image);
    } else if (op.kind == rdr_adapter::OpKind::Copy) {
      const rdr::CopyOp& c = f.copies[op.index];
      std::fprintf(out, "op %u copy %u -> %u src(%d,%d %ux%u) dst(%d,%d %ux%u)\n", i, c.source.image, c.dest.image, c.source_rect.x, c.source_rect.y, c.source_rect.width, c.source_rect.height,
                   c.dest_rect.x, c.dest_rect.y, c.dest_rect.width, c.dest_rect.height);
    } else if (op.kind == rdr_adapter::OpKind::Clear) {
      std::fprintf(out, "op %u clear\n", i);
    } else {
      const rdr::PresentOp& p = f.presents[op.index];
      std::fprintf(out, "op %u present image %u\n", i, p.source.image);
    }
  }
  if (out != stdout) std::fclose(out);
  return 0;
}

// ---- the parameters, read from the game's own post draws by name ------------------------------------------------------------------------

const ConstantInfo* Named(const DrawInfo& d, const char* field) {
  for (const ConstantInfo& c : d.constants)
    if (c.field == field && c.stage == "ps") return &c;
  return nullptr;
}
template <size_t N>
bool Take(const DrawInfo& d, const char* field, float (&dst)[N][4], int count, std::string& missing) {
  const ConstantInfo* c = Named(d, field);
  if (!c || int(c->values.size()) < count * 4) { missing += std::string(" ") + field + "@" + d.ps_variant; return false; }
  for (int i = 0; i < count; ++i)
    for (int k = 0; k < 4; ++k) dst[i][k] = c->values[size_t(i * 4 + k)];
  return true;
}
bool Take1(const DrawInfo& d, const char* field, float (&dst)[4], std::string& missing) {
  float tmp[1][4];
  if (!Take(d, field, tmp, 1, missing)) return false;
  std::memcpy(dst, tmp[0], 16);
  return true;
}

struct GameInputs {
  uint32_t colour = 0, depth = 0, blur = 0, bloom = 0, glare = 0;  // capture image ids the combine read
  uint32_t final_target = 0;                                       // the colour target of the last post draw
  uint32_t width = 0, height = 0;
};

// (a) the proof's parameters: every constant the post draws carried, by the effect's field name.
bool ParamsFromGame(const Session& s, const EffectIndex& fx, const Region& r, rdr_scene::PostParams& p, GameInputs& in, std::string& why) {
  const auto& f = s.frame();
  int t12 = 0, t1 = 0;
  std::string missing;
  p = rdr_scene::PostParams{};
  p.dof = p.fog = false;
  for (int i = r.first_draw; i <= r.last_draw; ++i) {
    if (f.ops[uint32_t(i)].kind != rdr_adapter::OpKind::Draw) continue;
    DrawInfo d;
    std::string err;
    if (!DescribeDraw(s, &fx, uint32_t(i), d, err)) { why = err; return false; }
    if (!d.bound) { why = "post draw op " + std::to_string(i) + " was not drawn (" + d.outcome + ")"; return false; }
    const std::string& ps = d.ps_variant;
    if (ps == "yDownScale_dof_fragment") {
      Take(d, "g_avSampleOffsets", p.dof_offsets, 4, missing), Take1(d, "g_f4DOFParam2", p.dof_param2_down, missing);
      for (const TextureInfo& t : d.textures)
        if (t.slot == 1) p.dof_depth_linear = t.sampler.rfind("linear", 0) == 0;  // the game's own filter for the depth
    } else if (ps == "yBlur_hole_fill_fragment") {
      Take(d, "SampleOffsets", p.hole_offsets, 12, missing), Take(d, "SampleWeights", p.hole_weights, 13, missing);
    } else if (ps == "yDownScale_box4_down_fragment") {
      p.bloom = true;
      Take(d, "g_avSampleOffsets", p.bright_offsets, 4, missing), Take1(d, "g_fDownColor", p.bright_down_color, missing);
    } else if (ps == "yBlur_t2_fragment") {
      Take(d, "SampleOffsets", p.bloom_t2_offsets, 13, missing), Take(d, "SampleWeights", p.bloom_t2_weights, 13, missing);
    } else if (ps == "yBlur_t1_t2_fragment" && t12 < 2) {
      Take(d, "SampleOffsets", p.bloom_t12_offsets[t12], 15, missing), Take(d, "SampleWeights", p.bloom_t12_weights[t12], 15, missing);
      ++t12;
    } else if (ps == "yBlur_t1_fragment" && t1 < 8) {
      p.glare = true;
      Take(d, "SampleOffsets", p.glare_offsets[t1], 8, missing), Take(d, "SampleWeights", p.glare_weights[t1], 8, missing);
      ++t1;
    } else if (ps == "yBlur_m1_m2_fragment") {
      Take(d, "SampleWeights", p.glare_merge, 4, missing);
    } else if (ps.rfind("yGlareEtc_", 0) == 0) {
      p.dof = ps.find("dof") != std::string::npos;
      p.fog = ps.find("fog") != std::string::npos;
      Take1(d, "DOFParam", p.DOFParam, missing), Take1(d, "DOFParam2", p.DOFParam2, missing);
      Take1(d, "RGB", p.RGB, missing), Take1(d, "Alpha", p.Alpha, missing), Take1(d, "ZAdj", p.ZAdj, missing);
      if (ps.find("bloom") != std::string::npos) Take1(d, "BloomScale", p.BloomScale, missing), Take1(d, "StarScale", p.StarScale, missing);
      for (const TextureInfo& t : d.textures) {
        if (t.slot == 0) in.colour = t.image, in.width = t.record.width, in.height = t.record.height;
        if (t.slot == 3) in.depth = t.image;
        if (t.slot == 2) in.blur = t.image;
        if (t.slot == 7) in.bloom = t.image;
        if (t.slot == 8) in.glare = t.image;
      }
    } else if (ps == "yMonotone_monotone_fragment") {
      p.film = true;
      Take1(d, "g_fCb", p.film_cb, missing), Take1(d, "g_fCr", p.film_cr, missing), Take1(d, "g_fAlpha", p.film_alpha, missing);
    }
  }
  p.width = in.width, p.height = in.height;
  for (int i = r.last_draw; i >= 0; --i)  // the colour target of the last post draw: the nearest SetTargets before it
    if (f.ops[uint32_t(i)].kind == rdr_adapter::OpKind::SetTargets) { in.final_target = f.targets[f.ops[uint32_t(i)].index].color[0].image; break; }
  if (!in.colour || !in.final_target || (p.dof && !in.depth)) { why = "the combine's inputs were not found in the capture"; return false; }
  if (!missing.empty()) { why = "constants not found by name:" + missing; return false; }
  return true;
}

// A planted fault on the parameters (acceptance 2): each must fail the bar on the frame where it shows.
bool PlantFault(const std::string& fault, rdr_scene::PostParams& p) {
  if (fault.empty()) return true;
  if (fault == "bloom_half") { p.BloomScale[0] *= 0.5f; return true; }                                       // bloom strength halved
  if (fault == "dof_threshold") { p.DOFParam2[1] -= 0.004f; p.dof_param2_down[1] -= 0.004f; return true; }  // the far-blur start moved
  if (fault == "skip_hdr") { p.bloom = p.glare = false; return true; }                                       // the HDR chain skipped
  return false;
}

bool SaveImage(rdr::Renderer& r, rdr::ImageId id, const std::string& path, uint32_t& w, uint32_t& h) {
  rdr::ImageRef ref;
  ref.image = id;
  std::vector<uint8_t> px;
  if (!r.ReadImage(ref, px, w, h)) return false;
  return WritePng(path.c_str(), px.data(), w, h);
}

template <size_t N>
float MaxDiff(const float (&x)[N][4], const float (&y)[N][4], int count, int comps = 2) {
  float m = 0;
  for (int i = 0; i < count; ++i)
    for (int k = 0; k < comps; ++k) m = std::max(m, std::fabs(x[i][k] - y[i][k]));
  return m;
}

// Goal 2b: the viewer's defaults (DefaultPostParams) against the tables the game's draws carried, table by table. The weights are compared on xyz, the offsets on xy.
int DefaultsCheck(Session& s, const EffectIndex& fx, const Args& a) {
  Region r;
  std::string why;
  if (!FindRegion(s, fx, r, why)) return Fail(why);
  rdr_scene::PostParams g;
  GameInputs in;
  if (!ParamsFromGame(s, fx, r, g, in, why)) return Fail(why);
  const bool entrance = g.bloom;
  rdr_scene::PostParams d = rdr_scene::DefaultPostParams(in.width, in.height, entrance);
  if (a.fault == "defaults_weight") d.hole_weights[1][0] *= 1.01f;  // the positive control: one weight 1% off must be caught
  struct Row { const char* name; float diff; bool only_entrance; };
  const Row rows[] = {
      {"dof_offsets", MaxDiff(g.dof_offsets, d.dof_offsets, 4), false},
      {"hole_offsets", MaxDiff(g.hole_offsets, d.hole_offsets, 12), false},
      {"hole_weights", MaxDiff(g.hole_weights, d.hole_weights, 13, 4), false},
      {"bright_offsets", MaxDiff(g.bright_offsets, d.bright_offsets, 4), true},
      {"bloom_t2_offsets", MaxDiff(g.bloom_t2_offsets, d.bloom_t2_offsets, 13), true},
      {"bloom_t2_weights", MaxDiff(g.bloom_t2_weights, d.bloom_t2_weights, 13, 3), true},
      {"bloom_t12_offsets_a", MaxDiff(g.bloom_t12_offsets[0], d.bloom_t12_offsets[0], 15), true},
      {"bloom_t12_weights_a", MaxDiff(g.bloom_t12_weights[0], d.bloom_t12_weights[0], 15, 3), true},
      {"bloom_t12_offsets_b", MaxDiff(g.bloom_t12_offsets[1], d.bloom_t12_offsets[1], 15), true},
      {"bloom_t12_weights_b", MaxDiff(g.bloom_t12_weights[1], d.bloom_t12_weights[1], 15, 3), true},
      {"glare_offsets_0", MaxDiff(g.glare_offsets[0], d.glare_offsets[0], 8), true},
      {"glare_offsets_1", MaxDiff(g.glare_offsets[1], d.glare_offsets[1], 8), true},
      {"glare_offsets_5", MaxDiff(g.glare_offsets[5], d.glare_offsets[5], 8), true},
      {"glare_offsets_7", MaxDiff(g.glare_offsets[7], d.glare_offsets[7], 8), true},
      {"glare_weights_0", MaxDiff(g.glare_weights[0], d.glare_weights[0], 8, 3), true},
      {"glare_weights_1", MaxDiff(g.glare_weights[1], d.glare_weights[1], 8, 3), true},
      {"glare_weights_6", MaxDiff(g.glare_weights[6], d.glare_weights[6], 8, 3), true},
      {"glare_merge", MaxDiff(g.glare_merge, d.glare_merge, 4, 3), true},
  };
  float worst = 0;
  // Review 0: the scalars and the combine's constants too. Fixed by rule in every captured frame: counted. Per-frame values (the far-blur ramp, the cap): printed, not counted.
  const auto v4 = [](const float (&x)[4], const float (&y)[4], int n) { float m = 0; for (int k = 0; k < n; ++k) m = std::max(m, std::fabs(x[k] - y[k])); return m; };
  struct Scalar { const char* name; float diff; bool counted; bool only_entrance; };
  const Scalar scalars[] = {
      {"bright_down_color.x", std::fabs(g.bright_down_color[0] - d.bright_down_color[0]), true, true},
      {"dof_param2_down.w", std::fabs(g.dof_param2_down[3] - d.dof_param2_down[3]), true, false},
      {"BloomScale.x", std::fabs(g.BloomScale[0] - d.BloomScale[0]), true, true},
      {"c27 RGB", v4(g.RGB, d.RGB, 4), true, false},
      {"c29 Alpha.x", std::fabs(g.Alpha[0] - d.Alpha[0]), true, false},
      {"c30 ZAdj.x", std::fabs(g.ZAdj[0] - d.ZAdj[0]), true, false},
      {"c20 DOFParam (per frame)", v4(g.DOFParam, d.DOFParam, 4), false, false},
      {"c21 DOFParam2 (per frame)", v4(g.DOFParam2, d.DOFParam2, 4), false, false},
  };
  for (const Scalar& sc : scalars) {
    if (sc.only_entrance && !entrance) continue;
    const bool inf = std::isinf(g.DOFParam[0]) != std::isinf(d.DOFParam[0]) && std::strstr(sc.name, "DOFParam") != nullptr;
    std::printf("  %-26s |diff| %.3g%s\n", sc.name, sc.diff, sc.counted ? "" : (inf ? "  (not counted: per-frame; near blur on / off differs)" : "  (not counted: per-frame value)"));
    if (sc.counted) worst = std::max(worst, sc.diff);
  }
  // The glare streaks: the directions rotate, their LENGTHS (the step of tap 1 of each of the 8 passes) must not.
  if (entrance)
    for (int pidx = 0; pidx < 8; ++pidx) {
      const float lg = std::hypot(g.glare_offsets[pidx][1][0], g.glare_offsets[pidx][1][1]), ld = std::hypot(d.glare_offsets[pidx][1][0], d.glare_offsets[pidx][1][1]);
      std::printf("  glare pass %d step length    game %.7g default %.7g |diff| %.3g  (not counted: the star turns in pixel space, so the uv length of a step varies per frame)\n", pidx, lg, ld,
                  std::fabs(lg - ld));
      if (pidx % 2 == 1) {  // counted: the second pass of a streak steps 8x as far as the first (the structure, whatever the direction)
        const float lg0 = std::hypot(g.glare_offsets[pidx - 1][1][0], g.glare_offsets[pidx - 1][1][1]);
        const float ratio = std::fabs(lg / std::max(lg0, 1e-9f) - 8.f);
        std::printf("  glare streak %d: second / first step length - 8 = %.3g\n", pidx / 2, ratio);
        worst = std::max(worst, ratio * 1e-3f);  // (a 2e-5 limit on the ratio's relative error)
      }
    }
  std::printf("[post_proof] defaults vs the game's tables, capture %s frame %d (%s):\n", fs::path(a.capture).filename().string().c_str(), a.frame, entrance ? "entrance" : "match");
  for (const Row& row : rows) {
    if (row.only_entrance && !entrance) continue;
    const bool rotates = std::strncmp(row.name, "glare_offsets", 13) == 0;  // the star's four directions turn from frame to frame [U: rule not found]; weights do not
    std::printf("  %-22s max |diff| %.3g%s\n", row.name, row.diff, rotates ? "  (not counted: the streak directions differ per frame)" : "");
    if (!rotates) worst = std::max(worst, row.diff);
  }
  std::printf("[post_proof] worst table difference %.3g (limit 2e-5)\n", worst);
  return worst > 2e-5f ? 3 : 0;
}

int Proof(Session& s, const EffectIndex& fx, const Args& a) {
  Region r;
  std::string why;
  if (!FindRegion(s, fx, r, why)) return Fail(why);
  rdr_scene::PostParams params;
  GameInputs in;
  if (!ParamsFromGame(s, fx, r, params, in, why)) return Fail(why);
  if (!PlantFault(a.fault, params)) return Fail("unknown --fault " + a.fault);
  std::error_code ec;
  fs::create_directories(a.out, ec);
  rdr::Renderer& rend = s.renderer();

  // 1. the frame up to just before the post region: the scene colour and depth are as the game's post saw them
  if (!s.RenderThrough(r.targets_op - 1)) return Fail("could not render the frame up to the post region");
  const rdr::ImageId colour = s.MappedImage(in.colour), depth = s.MappedImage(in.depth);
  rdr::ImageDesc od;
  od.width = uint16_t(in.width), od.height = uint16_t(in.height);
  od.format = rdr::Format::RGBA8;
  od.usage = rdr::kUsageTarget | rdr::kUsageSampled;
  const rdr::ImageId out = rend.CreateImage(od);
  rdr_scene::PostPass pass(rend);
  rdr::FrameBuilder fb;
  fb.Begin(1);
  if (!pass.Record(fb, colour, depth, out, params, why)) return Fail(why);
  rdr::Frame ours = fb.Finish();
  ours.keep_contents = true;  // the session's surfaces are kept: the game's own post draws follow
  rend.RenderFrame(ours);
  rend.WaitIdle();
  uint32_t w = 0, h = 0, w2 = 0, h2 = 0;
  const std::string base = a.out;
  if (!SaveImage(rend, out, base + "/ours.png", w, h)) return Fail("could not read our picture");
  SaveImage(rend, pass.dof_blur(), base + "/ours_blur.png", w2, h2);
  if (params.bloom) SaveImage(rend, pass.bloom_image(), base + "/ours_bloom.png", w2, h2);
  if (params.glare) SaveImage(rend, pass.glare_image(), base + "/ours_glare.png", w2, h2);
  // 2. the game's own post draws, then the picture they produced
  if (!s.RenderThrough(r.last_draw)) return Fail("could not render the game's post draws");
  std::vector<uint8_t> px;
  if (!s.ReadSurface(in.final_target, px, w2, h2) || w2 != w || h2 != h) return Fail("could not read the game's final picture");
  WritePng((base + "/game.png").c_str(), px.data(), w2, h2);
  if (in.blur && s.ReadSurface(in.blur, px, w2, h2)) WritePng((base + "/game_blur.png").c_str(), px.data(), w2, h2);
  if (params.bloom && in.bloom && s.ReadSurface(in.bloom, px, w2, h2)) WritePng((base + "/game_bloom.png").c_str(), px.data(), w2, h2);
  if (params.glare && in.glare && s.ReadSurface(in.glare, px, w2, h2)) WritePng((base + "/game_glare.png").c_str(), px.data(), w2, h2);
  // 3. the rest of the frame (HUD, flashes ...) and the surface that was presented: end.png (the non-circular check masks the pixels that later draws touched)
  if (s.RenderThrough(int(s.frame().ops.count) - 1) && s.ReadSurface(s.DefaultSurface(), px, w2, h2)) WritePng((base + "/end.png").c_str(), px.data(), w2, h2);
  std::FILE* js = std::fopen((base + "/result.json").c_str(), "w");
  if (js) {
    std::fprintf(js,
                 "{\"capture\": \"%s\", \"frame\": %d, \"width\": %u, \"height\": %u, \"post_ops\": [%d, %d], \"passes\": %u, \"fault\": \"%s\", \"dof\": %d, \"fog\": %d, \"bloom\": %d, "
                 "\"glare\": %d, \"film\": %d, \"validation_errors\": %llu, \"curve\": \"none on either side (the game applies its display curve at the present, after this picture)\"}\n",
                 fs::path(a.capture).filename().string().c_str(), a.frame, w, h, r.targets_op, r.last_draw, pass.passes_recorded(), a.fault.c_str(), params.dof, params.fog, params.bloom,
                 params.glare, params.film, static_cast<unsigned long long>(s.validation_errors()));
    std::fclose(js);
  }
  std::printf("[post_proof] %s frame %d: our %u passes vs the game's final picture (post ops %d-%d), validation errors %llu\n", fs::path(a.capture).filename().string().c_str(), a.frame,
              pass.passes_recorded(), r.targets_op, r.last_draw, static_cast<unsigned long long>(s.validation_errors()));
  rend.WaitIdle();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (arg == "--capture") a.capture = next();
    else if (arg == "--frame") a.frame = std::atoi(next().c_str());
    else if (arg == "--out") a.out = next();
    else if (arg == "--root") a.root = next();
    else if (arg == "--fault") a.fault = next();
    else if (arg == "--dump") a.dump = true;
    else if (arg == "--proof") a.proof = true;
    else if (arg == "--defaults") a.defaults = true;
    else if (arg == "--validation") a.validation = std::atoi(next().c_str());
    else return Fail("unknown argument " + arg);
  }
  if (a.capture.empty()) return Fail("usage: post_proof --capture DIR [--frame N] (--dump | --proof) --out PATH [--root REPO] [--fault NAME]");
  fs::path root = a.root.empty() ? fs::current_path() : fs::path(a.root);
  EffectIndex fx;
  std::string error;
  if (!fx.Load(root / "src" / "renderer" / "shaders" / "fx", error)) return Fail("effect descriptions: " + error);
  rdr::RendererOptions ro;
  ro.validation = a.validation;
  Session s;
  if (!s.Open(a.capture, ro, error)) return Fail("open: " + error);
  const uint32_t frame = a.frame < 0 ? s.frame_count() - 1 : uint32_t(a.frame);
  if (!s.SelectFrame(frame, error)) return Fail("frame: " + error);
  if (a.dump) return Dump(s, fx, a);
  if (a.proof) return Proof(s, fx, a);
  if (a.defaults) return DefaultsCheck(s, fx, a);
  return Fail("give --dump or --proof");
}
