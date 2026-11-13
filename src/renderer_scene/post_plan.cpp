// Session 101, lane PS: the post plan. See post_plan.h and docs\renderer\POST_PLAN.md.
#include "post_plan.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer_adapter/capture_reader.h"

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;
constexpr int kVariantCount = int(sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]));
constexpr uint32_t kSiteQueue = 0x82C1F48C, kSiteCombine = 0x82C1F674;

struct Registry {
  std::unordered_map<uint64_t, int> by_hash;
  std::vector<ChainKind> kind;   // per variant index: the chain kind of a pixel variant, Count when it is not a chain pixel shader
  std::vector<uint8_t> chain_vs; // per variant index: 1 when it is one of the post quads' vertex programs
  Registry() {
    for (const rdr::fx::HashEntry& h : rdr::fx::kHashes) by_hash.emplace(h.hash, int(h.variant));
    kind.assign(size_t(kVariantCount), ChainKind::Count);
    chain_vs.assign(size_t(kVariantCount), 0);
    for (int i = 0; i < kVariantCount; ++i) {
      const char* n = kVariants[i].name;
      if (!n) continue;
      const auto is = [&](const char* s) { return std::strcmp(n, s) == 0; };
      const auto starts = [&](const char* s) { return std::strncmp(n, s, std::strlen(s)) == 0; };
      if (is("yPostBase_copy_depth_fragment")) kind[size_t(i)] = ChainKind::CopyDepth;
      else if (is("yDownScale_dof_fragment")) kind[size_t(i)] = ChainKind::Dof;
      else if (is("yBlur_hole_fill_fragment")) kind[size_t(i)] = ChainKind::HoleFill;
      else if (is("yDownScale_box4_down_fragment")) kind[size_t(i)] = ChainKind::Box4Down;
      else if (is("yBlur_t2_fragment")) kind[size_t(i)] = ChainKind::T2;
      else if (is("yBlur_t1_t2_fragment")) kind[size_t(i)] = ChainKind::T12;
      else if (is("yBlur_t1_fragment")) kind[size_t(i)] = ChainKind::T1;
      else if (is("yBlur_m1_m2_fragment")) kind[size_t(i)] = ChainKind::M1M2;
      else if (starts("yGlareEtc_") && std::strstr(n, "_fragment")) kind[size_t(i)] = ChainKind::Combine;
      else if (starts("yMonotone_") && std::strstr(n, "_fragment")) kind[size_t(i)] = ChainKind::Monotone;
      else if (starts("yRetroFilm_") && std::strstr(n, "_fragment")) kind[size_t(i)] = ChainKind::Retro;
      const size_t len = std::strlen(n);
      const bool quad = len > 12 && std::strcmp(n + len - 12, "_quad_vertex") == 0;
      if (quad || is("yPostBase_uv_vertex") || is("yGlareEtc_corner_uv_vertex")) chain_vs[size_t(i)] = 1;
    }
  }
  int Find(uint64_t hash) const {
    const auto it = by_hash.find(hash);
    return it == by_hash.end() ? -1 : it->second;
  }
};
const Registry& Reg() {
  static const Registry r;
  return r;
}

uint8_t FilterOf(const rdr::SamplerDesc& s) { return s.mag_filter == 0 && s.min_filter == 0 ? 0 : s.mag_filter == 1 && s.min_filter == 1 ? 1 : 2; }

}  // namespace

const char* ChainKindName(ChainKind k) {
  static const char* const n[] = {"copy_depth", "dof", "hole_fill", "box4_down", "t2", "t1_t2", "t1", "m1_m2", "combine", "monotone", "retro"};
  return uint8_t(k) < uint8_t(ChainKind::Count) ? n[uint8_t(k)] : "?";
}
const char* PostShapeName(PostShape s) {
  static const char* const n[] = {"none", "match", "film", "entrance", "two_layer", "editor", "cards", "other"};
  return uint8_t(s) < sizeof(n) / sizeof(n[0]) ? n[uint8_t(s)] : "?";
}

// ---- the stream side ---------------------------------------------------------------------------------------------------------------------

// S101 lane PO: the same reader for the live path (no CaptureReader there): the target sizes come from `size` (the binder's image table); the old entry point wraps it.
bool ReadStreamChain(const rdr_adapter::CaptureReader& cap, const rdr_adapter::LoadedFrame& f, StreamChain& out, std::string& error) {
  const ChainImageSize size = [&cap](uint32_t image, uint32_t& w, uint32_t& h) {
    const rdr_adapter::ImageRecord* im = cap.FindImage(image);
    if (!im) return false;
    w = im->width, h = im->height;
    return true;
  };
  return ReadStreamChain(size, f, out, error);
}

bool ReadStreamChain(const ChainImageSize& size, const rdr_adapter::LoadedFrame& f, StreamChain& out, std::string& error) {
  out = StreamChain{};
  const Registry& reg = Reg();
  out.draws_in_frame = f.draws.count;
  out.named = f.origins.count == f.draws.count && f.draws.count != 0;
  // which draws are chain draws: a post quad's vertex program + a chain pixel program (the effect), found without names; the call site is only recorded
  std::vector<int8_t> chain(f.draws.count, 0);
  std::vector<ChainKind> kind(f.draws.count, ChainKind::Count);
  std::vector<int> ps_var(f.draws.count, -1);
  for (uint32_t i = 0; i < f.draws.count; ++i) {
    const rdr_adapter::Draw& d = f.draws[i];
    if (!d.ps_hash) continue;
    const int vs = reg.Find(d.vs_hash), ps = reg.Find(d.ps_hash);
    if (vs < 0 || ps < 0 || !reg.chain_vs[size_t(vs)] || reg.kind[size_t(ps)] == ChainKind::Count) continue;
    chain[i] = 1, kind[i] = reg.kind[size_t(ps)], ps_var[i] = ps;
  }
  std::unordered_map<uint32_t, int32_t> label;  // image -> the chain draw (index into out.draws) that last wrote it; -1 another draw
  uint32_t cur_t = 0;
  bool have_t = false;
  uint32_t combine_op = ~0u;
  std::vector<uint32_t> draw_op(f.draws.count, ~0u);
  for (uint32_t k = 0; k < f.ops.count; ++k) {
    const rdr_adapter::Op& op = f.ops[k];
    if (op.kind == rdr_adapter::OpKind::SetTargets) {
      if (op.index >= f.targets.count) { error = "a targets op names a missing table entry"; return false; }
      const rdr::TargetsOp& t = f.targets[op.index];
      cur_t = t.color_count ? t.color[0].image : t.depth.image, have_t = true;
    } else if (op.kind == rdr_adapter::OpKind::Draw) {
      if (op.index >= f.draws.count) { error = "a draw op names a missing draw"; return false; }
      draw_op[op.index] = k;
      if (!chain[op.index]) {
        if (have_t) label[cur_t] = -1;
        continue;
      }
      const rdr_adapter::Draw& d = f.draws[op.index];
      ChainDraw c;
      c.kind = kind[op.index], c.draw = op.index, c.op = k, c.target = cur_t, c.ps_variant = ps_var[op.index];
      c.vs_patched_hash = d.vs_patched_hash;
      if (const rdr_adapter::DrawOrigin* o = f.OriginOf(op.index)) c.site = o->call_site;
      if (size) size(cur_t, c.target_w, c.target_h);  // S101 lane PO: was cap.FindImage
      if (d.texture_first + d.texture_count > f.texture_slots.count) { error = "a draw's texture slots are outside the table"; return false; }
      for (uint32_t s = 0; s < d.texture_count; ++s) {
        const rdr_adapter::TextureSlot& ts = f.texture_slots[d.texture_first + s];
        if (ts.sampler >= f.samplers.count) { error = "a texture slot names a missing sampler"; return false; }
        ChainSlot cs;
        cs.slot = ts.slot, cs.image = ts.image, cs.filter = FilterOf(f.samplers[ts.sampler]);
        cs.addr_u = f.samplers[ts.sampler].address_u, cs.addr_v = f.samplers[ts.sampler].address_v;  // S101 lane PO
        const auto w = label.find(ts.image);
        cs.writer = w == label.end() ? -2 : w->second;
        c.slots.push_back(cs);
      }
      if (d.ps_bank >= f.constant_banks.count) { error = "a draw's pixel bank is outside the table"; return false; }
      std::memcpy(c.regs, f.constant_banks[d.ps_bank].dwords, sizeof c.regs);
      label[cur_t] = int32_t(out.draws.size());
      if (c.kind == ChainKind::Combine) combine_op = k;
      out.draws.push_back(std::move(c));
    } else if (op.kind == rdr_adapter::OpKind::Copy) {
      if (op.index >= f.copies.count) { error = "a copy op names a missing copy"; return false; }
      const rdr::CopyOp& c = f.copies[op.index];
      out.copies.push_back({k, c.source.image, c.dest.image});
      const auto s = label.find(c.source.image);
      label[c.dest.image] = s == label.end() ? -2 : s->second;
    }
  }
  if (combine_op != ~0u)
    for (uint32_t i = 0; i < f.draws.count; ++i)
      if (!chain[i] && draw_op[i] != ~0u && draw_op[i] > combine_op) ++out.later_draws;
  return true;
}

// ---- the plan ----------------------------------------------------------------------------------------------------------------------------

namespace {

void Take(const ChainDraw& d, int reg, int count, float (*dst)[4]) {
  for (int i = 0; i < count; ++i) std::memcpy(dst[i], d.regs[reg + i], 16);
}
void Take1(const ChainDraw& d, int reg, float* dst) { std::memcpy(dst, d.regs[reg], 16); }

std::string Rle(const std::vector<ChainDraw>& draws) {
  std::string out;
  for (size_t i = 0; i < draws.size();) {
    size_t j = i;
    while (j < draws.size() && draws[j].kind == draws[i].kind) ++j;
    if (!out.empty()) out += ' ';
    out += ChainKindName(draws[i].kind);
    if (j - i > 1) out += "x" + std::to_string(j - i);
    i = j;
  }
  return out;
}

// the 13 draws of the HDR chain, in order
constexpr ChainKind kHdr[13] = {ChainKind::Box4Down, ChainKind::T2, ChainKind::T12, ChainKind::T12, ChainKind::T1, ChainKind::T1, ChainKind::T1, ChainKind::T1,
                                ChainKind::T1, ChainKind::T1, ChainKind::T1, ChainKind::T1, ChainKind::M1M2};

PostShape ShapeOf(bool film, bool full, size_t layers, uint32_t restores) {
  // lane 140: a restore (copy_depth) in front of the one layer is not a new shape: the restores stay the stream's, the claim starts at the dof (checkC_director: 4911 frames 'copy_depth dof hole_fill combine')
  // lane 238: the film pass after a FULL chain (one or two layers, any restores) is a Film frame too: every part is proven (the full chain in Check B / Bryan, the film in wm30)
  if (film) return layers == 1 || (full && layers == 2) ? PostShape::Film : PostShape::Other;
  if (!full) return layers == 1 ? PostShape::Match : PostShape::Other;
  if (layers >= 2) return PostShape::TwoLayer;
  if (layers == 1) return restores == 0 ? PostShape::Editor : restores == 1 ? PostShape::Entrance : restores == 2 ? PostShape::Cards : PostShape::Other;
  return PostShape::Other;
}

int LayerOfWriter(const PostPlan& p, int32_t writer) {
  for (size_t i = 0; i < p.layers.size(); ++i)
    if (int32_t(p.layers[i].hole_fill) == writer) return int(i);
  return -1;
}

}  // namespace

PostPlan BuildPostPlan(const StreamChain& s) {
  PostPlan p;
  const auto& D = s.draws;
  p.sequence = Rle(D);
  p.later_draws = s.later_draws;
  const auto refuse = [&](const std::string& why) {
    p.ok = false, p.refusal = why;
    return p;
  };
  // the one combine
  int n_combine = 0, ci = -1;
  for (size_t i = 0; i < D.size(); ++i)
    if (D[i].kind == ChainKind::Combine) ++n_combine, ci = int(i);
  if (n_combine != 1) return refuse(n_combine == 0 ? "no combine in the frame" : "more than one combine in the frame");
  p.combine = uint32_t(ci);
  const ChainDraw& C = D[size_t(ci)];
  std::string name = kVariants[C.ps_variant].name;
  name = name.substr(10, name.size() - 10 - 9);  // "yGlareEtc_" ... "_fragment"
  p.combine_variant = name;
  if (name.find("night") != std::string::npos || name.find("tonemap") != std::string::npos || name.find("adapt") != std::string::npos)
    return refuse("combine permutation " + name + " is not ours yet");
  p.dof = name.find("dof") != std::string::npos;
  p.fog = name.find("fog") != std::string::npos;
  p.bloom_variant = name.find("bloom") != std::string::npos;
  p.combine_target = C.target;
  // after the combine: nothing, or one film draw, last
  size_t after = D.size() - size_t(ci) - 1;
  if (after > 1) return refuse("more than one chain draw after the combine");
  if (after == 1) {
    const ChainDraw& F = D[size_t(ci) + 1];
    if (F.kind == ChainKind::Retro) return refuse("yRetroFilm is not ours yet");
    if (F.kind != ChainKind::Monotone) return refuse("a chain draw after the combine that is not the film pass");
    // lane 238 (review N2): the pass records the film over the combine's output: the stream's film must read it (slot 0, written by the combine) and write the combine's target
    if (F.target != C.target || !F.Slot(0) || F.Slot(0)->writer != int32_t(ci)) return refuse("the film pass does not read the combine's output / write its target");
    p.film = std::strstr(kVariants[F.ps_variant].name, "_clean_") == nullptr;
  }
  // the walk back from the combine: the HDR chain, then the layers with the restores in front of them
  int i = ci - 1;
  // lane 238: only restores (copy_depth) in front of draw n: they stay the stream's, the claim starts after them
  const auto restores_only_before = [&](int n) {
    for (int k = 0; k < n; ++k)
      if (D[size_t(k)].kind != ChainKind::CopyDepth) return false;
    return true;
  };
  // lane 296: what may stand in front of the blur t2 (the claim starts at it; all of it stays the stream's): restores, and, with a dof permutation, ONE earlier t2 = the far layer's blur
  // ('copy_depth t2 copy_depthx2 t2 box4_down ...', runs 7183 / 7201: the 30 Years cutscene). The earlier t2 is the stream's layer, like Bryan's layer 1; the combine's slot 6 reads its output.
  int t2a = -1;
  const auto leading_ok = [&](int n) {
    int far = -1;
    for (int k = 0; k < n; ++k) {
      if (D[size_t(k)].kind == ChainKind::CopyDepth) continue;
      if (D[size_t(k)].kind == ChainKind::T2 && p.dof && far < 0 && g_plan_plant != PlanPlant::T2OneOnly) { far = k; continue; }   // (planted T2OneOnly: the rule before lane 296 = run 7201's refusal)
      return false;
    }
    t2a = far;
    return true;
  };
  int t2i = -1;
  if (!p.dof && !p.bloom_variant && restores_only_before(ci)) {   // lane 224: the combine alone (CAW editor: 873 of 990 chain frames in run 7122). Its permutation has no dof and no bloom, so it reads nothing a chain pass makes.
    p.combine_only = true;                                         // lane 238: also with the film pass after it ('combine monotone') and with restores in front ('copy_depth combine [monotone]')
    p.restores = uint32_t(ci), p.claim_first = uint32_t(ci);
    i = -1;
  } else
  if (i >= 0 && D[size_t(i)].kind == ChainKind::T2 && p.dof && leading_ok(i)) {   // lane 140: the blur by one t2 (Check J2 / wm30: 'a partial bloom / glare chain' for 163 + 50 frames); lane 238: restores in front
    t2i = i;
  } else
  if (i >= 0 && (D[size_t(i)].kind == ChainKind::M1M2 || D[size_t(i)].kind == ChainKind::T1 || D[size_t(i)].kind == ChainKind::T12 || D[size_t(i)].kind == ChainKind::T2 || D[size_t(i)].kind == ChainKind::Box4Down)) {
    if (i < 12) return refuse("a partial bloom / glare chain");
    for (int k = 0; k < 13; ++k)
      if (D[size_t(i - 12 + k)].kind != kHdr[k]) return refuse("a bloom / glare chain that is not the known order");
    p.hdr_drawn = true;
    i -= 13;
    // lane 238: the victory scene's 'copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine': the blur is one t2, and the HDR chain reads it
    // lane 277: also when the combine has NO dof permutation (the 30 Years cutscene, run 7183: 'copy_depth t2 box4_down t2 ... m1_m2 combine(fog_bloom) monotone'): the stream still draws the DoF blur, it only feeds the bright pass
    if (i >= 0 && D[size_t(i)].kind == ChainKind::T2 && (p.dof || g_plan_plant != PlanPlant::T2NeedsDof) && leading_ok(i)) t2i = i;   // (planted T2NeedsDof: the rule before lane 277 = run 7183's refusal)
  }
  if (t2i >= 0) {
    const ChainDraw& T = D[size_t(t2i)];
    p.t2_blur = true;
    p.t2_index = uint32_t(t2i), p.claim_first = uint32_t(t2i), p.restores = uint32_t(t2i);
    const ChainSlot* s0 = T.Slot(0);
    if (g_plan_plant == PlanPlant::T2SourceStale) s0 = C.Slot(2);   // planted: the source is the dropped copy's destination (img4), not a picture the stream made before the claim
    if (s0) p.blur_source_image = s0->image;
    // F2 (lane 140 review): the source must be a picture a stream op made before the claim (writer -1); a chain pass's output or a picture nothing wrote this frame is not
    if (!s0 || s0->writer != -1) return refuse("the t2 blur's source slot is not made in this frame by a stream op (writer " + std::to_string(s0 ? s0->writer : -9) + ")");
    // F3: the pass blurs into its quarter-size image (combine / 4)
    if (!T.target_w || !T.target_h || T.target_w * 4 != C.target_w || T.target_h * 4 != C.target_h)
      return refuse("the t2 blur's target (" + std::to_string(T.target_w) + "x" + std::to_string(T.target_h) + ") is not a quarter of the combine's (" + std::to_string(C.target_w) + "x" + std::to_string(C.target_h) + ")");
    if (t2a >= 0) {   // lane 296: the earlier t2 stays the stream's; ours records the later one, and nothing claimed may read the earlier one except the combine's slot 6
      const ChainDraw& A = D[size_t(t2a)];
      if (!A.target_w || !A.target_h || A.target_w * 4 != C.target_w || A.target_h * 4 != C.target_h)
        return refuse("the far t2 blur's target (" + std::to_string(A.target_w) + "x" + std::to_string(A.target_h) + ") is not a quarter of the combine's");
      for (size_t k = size_t(t2i); k < D.size(); ++k)
        for (const ChainSlot& sl : D[k].slots)
          if (sl.writer >= 0 && sl.writer < t2i && D[size_t(sl.writer)].kind == ChainKind::T2 && !(k == size_t(ci) && sl.slot == 6 && sl.writer == t2a))
            return refuse("a claimed chain draw reads the earlier t2's output (draw " + std::to_string(k) + " slot " + std::to_string(sl.slot) + ")");
      p.t2_far = t2a;
    }
    // lane 238: the HDR chain's bright pass must read the t2's output (the pass feeds it the same image)
    if (p.hdr_drawn) {
      const ChainSlot* b0 = D[size_t(t2i) + 1].Slot(0);
      if (!b0 || b0->writer != int32_t(t2i)) return refuse("the bright pass does not read the t2 blur's output");
    }
    i = -1;
  }
  std::vector<PostLayer> rev;
  while (i >= 1 && D[size_t(i)].kind == ChainKind::HoleFill && D[size_t(i - 1)].kind == ChainKind::Dof) {
    PostLayer l;
    l.hole_fill = uint32_t(i), l.dof = uint32_t(i - 1);
    i -= 2;
    while (i >= 0 && D[size_t(i)].kind == ChainKind::CopyDepth) ++l.restores_before, --i;
    rev.push_back(l);
  }
  if (i >= 0) return refuse("chain draws before the known sequence (" + std::string(ChainKindName(D[size_t(i)].kind)) + ")");
  p.layers.assign(rev.rbegin(), rev.rend());
  for (const PostLayer& l : p.layers) p.restores += l.restores_before;
  p.shape = p.t2_blur || p.combine_only ? (after == 1 ? PostShape::Film : PostShape::Match) : ShapeOf(after == 1, p.hdr_drawn, p.layers.size(), p.restores);   // (lane 140: t2_blur)
  if (p.shape == PostShape::Other || p.shape == PostShape::None) return refuse("a chain shape that is not known: " + p.sequence);

  // the layers: their inputs, the depth filter (the rule: point where the frame has a restore, linear where it has none)
  for (size_t li = 0; li < p.layers.size(); ++li) {
    PostLayer& l = p.layers[li];
    const ChainDraw& dof = D[l.dof];
    const ChainDraw& hole = D[l.hole_fill];
    if (const ChainSlot* c0 = dof.Slot(0)) l.colour_image = c0->image;
    if (const ChainSlot* c1 = dof.Slot(1)) l.depth_image = c1->image, l.stream_filter = c1->filter;
    l.blur_target = hole.target;
    l.depth_linear = l.stream_filter == 2 ? p.restores == 0 : l.stream_filter == 1;
    if (g_plan_plant == PlanPlant::OldRestoreRule) l.depth_linear = p.restores == 0;   // lane 140 planted: the old rule   // lane 140: the pass follows the stream's own sampler (Check J: 'dof hole_fill combine' frames with no restore bind it POINT); the restore rule is only the fallback for a mixed sampler
    if (g_plan_plant == PlanPlant::DofFilterFixed) l.depth_linear = true;  // planted: the old "match linear" guess
    l.owned = li + 1 == p.layers.size();
    Take(dof, 0, 4, l.tables.dof_offsets), Take1(dof, 21, l.tables.dof_param2_down);
    Take(hole, 1, 12, l.tables.hole_offsets), Take(hole, 16, 13, l.tables.hole_weights);
  }
  // the combine's slots and which layer's blur they hold
  for (const ChainSlot& c : C.slots) {
    if (c.slot < 9) p.slot_image[c.slot] = c.image;
    if (!p.slots.empty()) p.slots += ' ';
    p.slots += "s" + std::to_string(c.slot);
  }
  if (const ChainSlot* s2 = C.Slot(2)) p.slot2_layer = LayerOfWriter(p, s2->writer);
  if (const ChainSlot* s6 = C.Slot(6)) p.slot6_layer = LayerOfWriter(p, s6->writer);
  if (g_plan_plant == PlanPlant::LayerMerged && p.slot6_layer >= 0) {  // planted: slot 6 read as slot 2, the layers collapse into one
    p.slot6_layer = p.slot2_layer, p.slot_image[6] = p.slot_image[2];
    if (p.layers.size() > 1) p.layers.erase(p.layers.begin(), p.layers.end() - 1);
    p.layers.back().owned = true;
    p.slot2_layer = p.slot6_layer = int(p.layers.size()) - 1;
  }
  if (p.t2_blur && !p.dof) {
    // lane 277: the combine has no dof permutation: it reads nothing the blur made (slots 2 / 6, if bound, are not fetched); the bright pass reads the t2 (checked above)
    if (!p.blur_source_image) return refuse("the t2 blur reads image 0");
  } else
  if (p.t2_blur) {   // lane 140: slots 2 and 6 must hold the t2's own output (a copy of its target)
    const ChainSlot* s2 = C.Slot(2);
    const ChainSlot* s6 = C.Slot(6);
    const int32_t want6 = p.t2_far >= 0 ? p.t2_far : int32_t(p.t2_index);   // lane 296: two t2 layers: slot 2 = the later (recorded) blur, slot 6 = the earlier (the stream's) one, as Bryan's layers
    if (g_plan_plant == PlanPlant::T2SlotsForeign) {   // planted: no writer check, slot 6 fed the scene colour
      if (s6) p.slot_image[6] = p.slot_image[0], p.far_blur_separate = true, p.far_blur_image = p.slot_image[0];
    } else {
      if (!s2 || !s6 || s2->writer != int32_t(p.t2_index) || s6->writer != want6) return refuse("the combine's blur slots are not the t2 pass's output");
      if (p.t2_far >= 0) {
        if (p.slot_image[6] == p.slot_image[2]) return refuse("the two t2 layers' blur slots hold one image");
        for (const ChainCopy& cp : s.copies)   // lane 296b (SF1): the far blur's own copy must lie before the claim, or a claimed draw can write its source and the pass samples last frame's blur
          if (g_plan_plant != PlanPlant::FarCopyUnchecked && cp.dst == p.slot_image[6] && cp.op > D[p.t2_index].op && cp.op < C.op) return refuse("the far t2 blur's copy lies inside the claimed range");
        p.far_blur_separate = true, p.far_blur_image = p.slot_image[6];
        {   // lane 296b (SF2): the one-line account of the first accepted two-layer frame (slot-role question: which image is which, who wrote it, where the far copy sits against the claim)
          int64_t far_copy = INT64_MIN;
          for (const ChainCopy& cp : s.copies) if (cp.dst == p.slot_image[6]) far_copy = int64_t(cp.op) - int64_t(D[p.claim_first].op);
          char note[400];
          std::snprintf(note, sizeof note, "slot 2 = image %u (writer draw %d, t2 target %ux%u), slot 6 = image %u (writer draw %d, t2 target %ux%u), combine target %ux%u, far copy op %s%lld vs claim_first",
                        p.slot_image[2], s2->writer, D[p.t2_index].target_w, D[p.t2_index].target_h, p.slot_image[6], s6->writer, D[size_t(p.t2_far)].target_w, D[size_t(p.t2_far)].target_h,
                        C.target_w, C.target_h, far_copy == INT64_MIN ? "(none) " : far_copy > 0 ? "+" : "", far_copy == INT64_MIN ? 0LL : (long long)far_copy);
          p.t2_far_note = note;
        }
      }
    }
    if (!p.blur_source_image) return refuse("the t2 blur reads image 0");
  } else
  if (p.dof) {
    if (p.slot2_layer < 0 || p.slot6_layer < 0) return refuse("the combine's blur slots are not images the chain's hole_fill made");
    if (p.slot2_layer != int(p.layers.size()) - 1) return refuse("the combine's slot 2 is not the last layer's blur");
    p.far_blur_separate = p.slot6_layer != p.slot2_layer;
    if (p.far_blur_separate) p.far_blur_image = p.slot_image[6];
  }
  // the bloom / glare side
  if (p.hdr_drawn) {
    const auto is_blur = [&](const ChainDraw& d) { return p.t2_blur && (size_t(&d - D.data()) == p.t2_index || int32_t(&d - D.data()) == p.t2_far); };   // (lane 296: the far layer's t2 is not bloom work either)   // lane 238: the DoF blur's t2 is not bloom work
    for (const ChainDraw& d : D) {
      if (is_blur(d)) continue;
      if (d.kind == ChainKind::Box4Down || d.kind == ChainKind::T2 || d.kind == ChainKind::T12) ++p.bloom_side_draws;
      if (d.kind == ChainKind::T1 || d.kind == ChainKind::M1M2) ++p.glare_side_draws;
    }
    std::vector<uint32_t> bloom_t, glare_t;
    for (const ChainDraw& d : D) {
      if (is_blur(d)) continue;
      if (d.kind == ChainKind::Box4Down || d.kind == ChainKind::T2 || d.kind == ChainKind::T12) bloom_t.push_back(d.target);
      if (d.kind == ChainKind::T1 || d.kind == ChainKind::M1M2) glare_t.push_back(d.target);
    }
    const auto in = [](const std::vector<uint32_t>& v, uint32_t x) { for (uint32_t y : v) if (y == x) return true; return false; };
    for (const ChainCopy& c : s.copies) {
      if (in(bloom_t, c.src)) ++p.bloom_side_copies;
      if (in(glare_t, c.src)) ++p.glare_side_copies;
    }
    if (p.bloom_variant) {
      p.bloom_zero = C.regs[31][0] == 0.f, p.glare_zero = C.regs[32][0] == 0.f;
      if (g_plan_plant == PlanPlant::DeadBloomKept) p.bloom_zero = p.glare_zero = false;  // planted: the dead work is kept
    } else {
      p.bloom_zero = p.glare_zero = true;  // the combine cannot read the HDR chain: all of it is dead
    }
    for (size_t k = 0; k < D.size(); ++k) {
      if (is_blur(D[k])) continue;
      const ChainKind kd = D[k].kind;
      const bool bloom_blur = kd == ChainKind::T2 || kd == ChainKind::T12, glare = kd == ChainKind::T1 || kd == ChainKind::M1M2;
      const bool bright = kd == ChainKind::Box4Down;
      if ((bloom_blur && p.bloom_zero) || (glare && p.glare_zero) || (bright && p.bloom_zero && p.glare_zero)) p.skipped_draw_list.push_back(uint32_t(k));
    }
    p.skipped_draws = uint32_t(p.skipped_draw_list.size());
    std::vector<uint32_t> skip_t, keep_t;
    for (size_t k = 0; k < D.size(); ++k) {
      if (is_blur(D[k])) continue;
      bool sk = false;
      for (uint32_t x : p.skipped_draw_list) sk = sk || x == k;
      const ChainKind kd = D[k].kind;
      if (kd == ChainKind::Box4Down || kd == ChainKind::T2 || kd == ChainKind::T12 || kd == ChainKind::T1 || kd == ChainKind::M1M2) (sk ? skip_t : keep_t).push_back(D[k].target);
    }
    for (const ChainCopy& c : s.copies)
      if (in(skip_t, c.src) && !in(keep_t, c.src)) ++p.skipped_copies;
  }
  // DoF off, bloom live: the dof + hole_fill passes still draw because the bloom reads them. DoF off and no live bloom: they are dead too.
  const bool hdr_live = p.hdr_drawn && !(p.bloom_zero && p.glare_zero);
  p.bloom_source_dof = !p.dof && hdr_live && !p.t2_blur;   // (lane 277: with a t2 blur the bloom's source is the t2, not the dof + hole_fill pair)
  p.skip_dof_chain = !p.dof && !hdr_live && !p.combine_only;   // (lane 224: the combine alone has no dof chain to skip)
  // where the chain ends: the structural last chain draw (the film pass after the combine, else the combine)
  p.chain_last = uint32_t(D.size() - 1);
  p.chain_last_draw = D.back().draw;
  p.chain_end_found = true;
  if (g_plan_plant == PlanPlant::LastPassByHash) {  // planted: the old way, the draw whose vertex program hash is the combine's (0xD6B8DEB0775CC47F)
    p.chain_end_found = false;
    for (size_t k = 0; k < D.size(); ++k)
      if (D[k].vs_patched_hash == 0xD6B8DEB0775CC47Full) p.chain_last = uint32_t(k), p.chain_last_draw = D[k].draw, p.chain_end_found = true;
  }
  p.chain_first_draw = p.t2_blur || p.combine_only ? D[p.claim_first].draw : D[p.layers.back().dof].draw;   // lane 140: t2_blur
  for (const ChainDraw& d : D)
    for (const ChainSlot& c : d.slots)
      if (c.image == 0) ++p.null_inputs;

  // the tables, in PostPass's terms
  PostParams& q = p.params;
  q.width = C.target_w ? C.target_w : q.width, q.height = C.target_h ? C.target_h : q.height;
  q.dof = p.dof, q.fog = p.fog;
  q.bloom = q.glare = p.hdr_drawn;  // the chain is in the stream; the pass leaves out the dead parts (dead_bloom / dead_glare)
  q.dead_bloom = p.hdr_drawn && p.bloom_zero, q.dead_glare = p.hdr_drawn && p.glare_zero;
  q.dof_chain = p.bloom_source_dof;
  q.bloom_variant = p.bloom_variant;
  q.film = p.film;
  if (!p.layers.empty()) {   // lane 140: no layers in the t2 blur's frames
    const PostLayer& last = p.layers.back();
    q.dof_depth_linear = last.depth_linear;
    std::memcpy(q.dof_offsets, last.tables.dof_offsets, sizeof q.dof_offsets);
    std::memcpy(q.dof_param2_down, last.tables.dof_param2_down, sizeof q.dof_param2_down);
    std::memcpy(q.hole_offsets, last.tables.hole_offsets, sizeof q.hole_offsets);
    std::memcpy(q.hole_weights, last.tables.hole_weights, sizeof q.hole_weights);
  }
  q.t2_blur = p.t2_blur;   // lane 140 (the T2 draw's tables are taken below with the bloom's)
  if (p.dof) Take1(C, 20, q.DOFParam), Take1(C, 21, q.DOFParam2);
  if (p.fog) Take1(C, 27, q.RGB), Take1(C, 29, q.Alpha), Take1(C, 30, q.ZAdj);
  if (p.bloom_variant) Take1(C, 31, q.BloomScale), Take1(C, 32, q.StarScale);
  int t12 = 0, t1 = 0;
  for (const ChainDraw& d : D) {
    switch (d.kind) {
      case ChainKind::Box4Down: Take(d, 0, 4, q.bright_offsets), Take1(d, 16, q.bright_down_color); break;
      case ChainKind::T2:
        if (int32_t(&d - D.data()) == p.t2_far) break;   // lane 296: the far layer stays the stream's: its tables are not ours
        if (p.t2_blur && size_t(&d - D.data()) == p.t2_index) Take(d, 0, 13, q.blur_t2_offsets), Take(d, 16, 13, q.blur_t2_weights);   // lane 238: the DoF blur's tables
        else Take(d, 0, 13, q.bloom_t2_offsets), Take(d, 16, 13, q.bloom_t2_weights);
        break;
      case ChainKind::T12:
        if (t12 < 2) Take(d, 0, 15, q.bloom_t12_offsets[t12]), Take(d, 16, 15, q.bloom_t12_weights[t12]);
        ++t12;
        break;
      case ChainKind::T1:
        if (t1 < 8) Take(d, 0, 8, q.glare_offsets[t1]), Take(d, 16, 8, q.glare_weights[t1]);
        ++t1;
        break;
      case ChainKind::M1M2:
        Take(d, 16, 4, q.glare_merge);
        for (int sl = 0; sl < 4; ++sl)   // lane 238: the pass follows the stream's own filter per slot (run 7131: slot 3 bound POINT in 46 binds); a mixed sampler (min != mag) stays linear here and the core refuses the frame (filter)
          if (const ChainSlot* ms = d.Slot(sl)) q.glare_merge_linear[sl] = ms->filter != 0;
        break;
      case ChainKind::Monotone:
        Take1(d, 0, q.film_cb), Take1(d, 1, q.film_cr), Take1(d, 2, q.film_alpha);
        break;
      default: break;
    }
  }
  p.ok = true;
  return p;
}

}  // namespace rdr_scene
