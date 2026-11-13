// caw_ours_check: the offline proof of the Create a Superstar editor's wrestler drawn in place (Session 101, lane 153 = N3; docs\renderer\CAW_OURS.md). 0 game runs, headless.
// It drives the SHIPPED decision code (src\renderer_scene\caw_core.h, the class caw_ours.cpp uploads) over a capture frame by frame, every main-pass model-part draw offered to Decide
// the way the hook would. For each REPLACED draw it proves, independently of Decide's own checks, that what would be drawn is the stream's: our vertex bytes equal the stream's
// captured vertex bytes (positions within float ulps, everything else bit-equal) and our index buffer is the stream's. The palette is the stream's own (rows = 0): nothing to prove.
//   caw_ours_check --capture DIR --game DIR [--frames A-B] [--step N] [--fault LIST] [--morph-at F] [--census]
// --morph-at F (the CAW bug class: a slider moves vertices IN PLACE, same offsets and counts): from frame F + 1 on, the first replaced draw's vertex blob reads back with one position
// changed. The line MORPH says in which frame the core first refused that part (must be F + 1) and how many wrong draws the independent proof saw (must be 0).
// Faults (the hook's --replace-fault names): pinned, no_verify, wrong_leaf. With a fault the proof must SEE the wrongness (a positive control).
// Exit 0 = measured, 2 = a loud failure (no capture, no game files, nothing claimed), 3 = bad usage. Machine-readable lines: ARM, CAWPART, RESULT, MORPH.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/caw_core.h"

using namespace rdr_adapter;
using rdr_scene::CawCore;
using rdr_scene::CawDecision;
using rdr_scene::CawFaults;

int main(int argc, char** argv) {
  std::string capture, game, fault_list;
  int first = 0, last = -1, step = 1, morph_at = -1;
  bool census = false, fit = false, diff = false;   // lane 179: diff
  int dump_part = -1, dump_v = 0;                    // lane 179
  uint32_t find_v = 0, find_i = 0;
  double morph_rel = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--capture") capture = next();
    else if (a == "--game") game = next();
    else if (a == "--fault") fault_list = next();
    else if (a == "--frames") { if (std::sscanf(next(), "%d-%d", &first, &last) != 2) { std::fprintf(stderr, "bad --frames\n"); return 3; } }
    else if (a == "--step") step = std::max(1, std::atoi(next()));
    else if (a == "--morph-at") morph_at = std::atoi(next());
    else if (a == "--morph-rel") morph_rel = std::atof(next());   // the morph size as a fraction of max(1, |x|) (the tolerance boundary arms); default 0.5 units
    else if (a == "--find") { find_v = uint32_t(std::atoi(next())); find_i = uint32_t(std::atoi(next())); }   // list the edit meshes with these counts
    else if (a == "--dump") { dump_part = std::atoi(next()); dump_v = std::atoi(next()); }   // lane 179: with --diff, the first N vertices of one part
    else if (a == "--diff") diff = true;   // lane 179: per part, per count-fitting candidate: the stream minus 96/95 x the file, per axis
    else if (a == "--census") census = true;
    else if (a == "--fit") fit = true;   // lane 153 experiment: is a stream part's position array a linear blend of the file's variants of that mesh?
    else { std::fprintf(stderr, "caw_ours_check: unknown argument %s\n", a.c_str()); return 3; }
  }
  if (capture.empty() || game.empty()) { std::fprintf(stderr, "usage: caw_ours_check --capture DIR --game DIR [--frames A-B] [--fault LIST] [--morph-at F]\n"); return 3; }
  CawFaults faults;
  { std::string err; if (!CawFaults::Parse(fault_list, faults, err)) { std::fprintf(stderr, "caw_ours_check: %s\n", err.c_str()); return 3; } }
  const auto t0 = std::chrono::steady_clock::now();
  CaptureReader cap;
  if (!cap.Open(capture)) { std::fprintf(stderr, "caw_ours_check: FAIL cannot open the capture at %s\n", capture.c_str()); return 2; }
  assets::wrestler_roster::Roster roster;
  { std::string err; if (!roster.BuildEdit(game, err)) { std::fprintf(stderr, "caw_ours_check: FAIL edit parts: %s\n", err.c_str()); return 2; } }
  std::fprintf(stderr, "caw_ours_check: %zu pacs, %zu distinct-leaf mesh records, %.1f s\n", roster.pacs(), roster.edit_parts().size(), roster.seconds());
  const int nframes = int(cap.frame_count());
  if (last < 0 || last >= nframes) last = nframes - 1;
  if (find_v) {
    for (const auto& ep : roster.edit_parts()) if (ep.vertices == find_v && ep.index_count == find_i) std::printf("FOUND ed%02u pach %u leaf %u mesh %u" "%c", ep.pac, ep.pach, ep.leaf, ep.mesh, 10);
    return 0;
  }
  if (fit) {   // ---- experiment: least squares of the stream's positions on the variants' positions ----
    namespace wg = assets::wrestler_geom;
    LoadedFrame ff;
    if (!cap.LoadFrame(uint32_t(first), ff)) return 2;
    std::map<uint64_t, std::vector<size_t>> by;
    for (size_t i = 0; i < roster.edit_parts().size(); ++i) by[uint64_t(roster.edit_parts()[i].vertices) << 32 | roster.edit_parts()[i].index_count].push_back(i);
    for (uint32_t d = 0; d < ff.draws.count; ++d) {
      const DrawOrigin* o = ff.OriginOf(d);
      if (!o || o->pass_site != kModelPartColourSite || o->object == 0) continue;
      const Draw& dr = ff.draws[d];
      auto it = by.find(uint64_t(dr.vertex_count) << 32 | dr.index_count);
      if (it == by.end() || dr.layout >= ff.layouts.count) continue;
      const VertexLayout& lay = ff.layouts[dr.layout];
      std::vector<uint8_t> svb;
      if (!cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, svb)) continue;
      std::vector<std::vector<double>> V;
      std::vector<std::string> names;
      int posoff = -1;
      for (size_t ci : it->second) {
        std::string err;
        auto leaf = roster.LoadEditLeaf(ci, err);
        const auto& ep = roster.edit_parts()[ci];
        if (!leaf || ep.mesh >= leaf->parts.parts.size()) continue;
        const assets::jboy_parts::Part& part = leaf->parts.parts[ep.mesh];
        std::array<wg::LayoutElement, 16> els;
        if (lay.element_count == 0 || lay.element_count > els.size()) continue;
        for (uint32_t e = 0; e < lay.element_count; ++e) { const VertexElement& el = ff.elements[lay.element_first + e]; els[e] = {el.vk_format, el.host_offset, el.source_format, el.components, el.flags, el.guest_offset}; }
        wg::Roles roles;
        if (!wg::Classify(part, els.data(), lay.element_count, lay.host_stride, lay.guest_stride, roles, true, false)) continue;
        wg::BuildOptions bo;
        bo.scale = 1.0f;
        std::vector<uint8_t> ours;
        wg::BuildVertices(part, roles, bo, ours);
        for (const auto& r : roles.r) if (r.role == wg::Role::Pos) posoff = r.offset;
        if (posoff < 0) continue;
        std::vector<double> col(size_t(dr.vertex_count) * 3);
        for (uint32_t v = 0; v < dr.vertex_count; ++v) for (int c = 0; c < 3; ++c) { float x; std::memcpy(&x, ours.data() + size_t(v) * lay.host_stride + posoff + 4 * c, 4); col[size_t(v) * 3 + c] = x; }
        V.push_back(col);
        char nb[80]; std::snprintf(nb, sizeof nb, "ed%02u/p%u/%u", ep.pac, ep.pach, ep.leaf);
        names.push_back(nb);
      }
      if (V.empty() || posoff < 0) continue;
      std::vector<double> S(size_t(dr.vertex_count) * 3);
      for (uint32_t v = 0; v < dr.vertex_count; ++v) for (int c = 0; c < 3; ++c) { float x; std::memcpy(&x, svb.data() + size_t(v) * lay.host_stride + posoff + 4 * c, 4); S[size_t(v) * 3 + c] = x; }
      {   // per variant, per axis: S = a * P + b (a scale and a shift per axis)
        double best = 1e30; size_t bk = 0; double ba[3] = {}, bb[3] = {};
        for (size_t k = 0; k < V.size(); ++k) {
          double worst = 0, aa[3], bbv[3];
          for (int c = 0; c < 3; ++c) {
            double n = 0, sp = 0, ss = 0, spp = 0, sps = 0;
            for (uint32_t v = 0; v < dr.vertex_count; ++v) { const double p = V[k][size_t(v) * 3 + c], s = S[size_t(v) * 3 + c]; ++n; sp += p; ss += s; spp += p * p; sps += p * s; }
            const double den = n * spp - sp * sp;
            const double a = den != 0 ? (n * sps - sp * ss) / den : 1.0, b = (ss - a * sp) / n;
            aa[c] = a, bbv[c] = b;
            for (uint32_t v = 0; v < dr.vertex_count; ++v) worst = std::max(worst, std::fabs(a * V[k][size_t(v) * 3 + c] + b - S[size_t(v) * 3 + c]));
          }
          if (worst < best) { best = worst, bk = k; for (int c = 0; c < 3; ++c) ba[c] = aa[c], bb[c] = bbv[c]; }
        }
        std::printf("AFFINE object %08X part %u: best variant %s max_abs_resid %.3g a=(%.7f %.7f %.7f) b=(%.5g %.5g %.5g)\n", o->object, o->part, names[bk].c_str(), best, ba[0], ba[1], ba[2], bb[0], bb[1], bb[2]);
      }
      const size_t K = V.size();
      std::vector<std::vector<double>> A(K, std::vector<double>(K + 1, 0.0));
      for (size_t a = 0; a < K; ++a) {
        for (size_t b = 0; b < K; ++b) { double s = 0; for (size_t i = 0; i < S.size(); ++i) s += V[a][i] * V[b][i]; A[a][b] = s; }
        double s = 0; for (size_t i = 0; i < S.size(); ++i) s += V[a][i] * S[i];
        A[a][K] = s;
        A[a][a] += 1e-9;
      }
      for (size_t c = 0; c < K; ++c) {   // gauss
        size_t p = c; for (size_t r = c + 1; r < K; ++r) if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
        std::swap(A[c], A[p]);
        if (std::fabs(A[c][c]) < 1e-14) continue;
        for (size_t r = 0; r < K; ++r) { if (r == c) continue; const double m = A[r][c] / A[c][c]; for (size_t k = c; k <= K; ++k) A[r][k] -= m * A[c][k]; }
      }
      std::vector<double> w(K);
      for (size_t c = 0; c < K; ++c) w[c] = std::fabs(A[c][c]) < 1e-14 ? 0 : A[c][K] / A[c][c];
      double maxr = 0, maxrel = 0;
      for (size_t i = 0; i < S.size(); ++i) { double p = 0; for (size_t k = 0; k < K; ++k) p += w[k] * V[k][i]; maxr = std::max(maxr, std::fabs(p - S[i])); maxrel = std::max(maxrel, std::fabs(p - S[i]) / std::max(1.0, std::fabs(S[i]))); }
      double sumw = 0; for (double x : w) sumw += x;
      std::printf("FIT object %08X part %u counts %u/%u variants %zu: sum_w %.6f max_abs_resid %.3g max_rel_resid %.3g | weights:", o->object, o->part, dr.vertex_count, dr.index_count, K, sumw, maxr, maxrel);
      for (size_t k = 0; k < K; ++k) if (std::fabs(w[k]) > 1e-4) std::printf(" %s=%.5f", names[k].c_str(), w[k]);
      std::printf("\n");
    }
    return 0;
  }
  if (diff) {   // ---- lane 179 experiment: stream position minus (96/95 x the candidate's file position), per axis, every count-fitting candidate ----
    namespace wg = assets::wrestler_geom;
    LoadedFrame ff;
    if (!cap.LoadFrame(uint32_t(first), ff)) return 2;
    std::map<uint64_t, std::vector<size_t>> by;
    for (size_t i = 0; i < roster.edit_parts().size(); ++i) by[uint64_t(roster.edit_parts()[i].vertices) << 32 | roster.edit_parts()[i].index_count].push_back(i);
    const double K = 96.0 / 95.0;
    for (uint32_t d = 0; d < ff.draws.count; ++d) {
      const DrawOrigin* o = ff.OriginOf(d);
      if (!o || o->pass_site != kModelPartColourSite || o->object == 0) continue;
      const Draw& dr = ff.draws[d];
      auto it = by.find(uint64_t(dr.vertex_count) << 32 | dr.index_count);
      if (it == by.end() || dr.layout >= ff.layouts.count) continue;
      const VertexLayout& lay = ff.layouts[dr.layout];
      std::vector<uint8_t> svb;
      if (!cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, svb)) continue;
      for (size_t ci : it->second) {
        std::string err;
        auto leaf = roster.LoadEditLeaf(ci, err);
        const auto& ep = roster.edit_parts()[ci];
        if (!leaf || ep.mesh >= leaf->parts.parts.size()) continue;
        const assets::jboy_parts::Part& part = leaf->parts.parts[ep.mesh];
        std::array<wg::LayoutElement, 16> els;
        if (lay.element_count == 0 || lay.element_count > els.size()) continue;
        for (uint32_t e = 0; e < lay.element_count; ++e) { const VertexElement& el = ff.elements[lay.element_first + e]; els[e] = {el.vk_format, el.host_offset, el.source_format, el.components, el.flags, el.guest_offset}; }
        wg::Roles roles;
        if (!wg::Classify(part, els.data(), lay.element_count, lay.host_stride, lay.guest_stride, roles, true, false)) continue;
        wg::BuildOptions bo;
        bo.scale = 1.0f;
        std::vector<uint8_t> ours;
        wg::BuildVertices(part, roles, bo, ours);
        int posoff = -1;
        for (const auto& r : roles.r) if (r.role == wg::Role::Pos) posoff = r.offset;
        if (posoff < 0) continue;
        double mx[3] = {}, sm[3] = {}, ss[3] = {}, mn1 = 1e30, mx1 = -1e30;
        for (uint32_t v = 0; v < dr.vertex_count; ++v) for (int c = 0; c < 3; ++c) {
          float a, b;
          std::memcpy(&a, ours.data() + size_t(v) * lay.host_stride + posoff + 4 * c, 4);
          std::memcpy(&b, svb.data() + size_t(v) * lay.host_stride + posoff + 4 * c, 4);
          const double e = double(b) - K * double(a);
          mx[c] = std::max(mx[c], std::fabs(e)), sm[c] += e, ss[c] += e * e;
          if (c == 1) mn1 = std::min(mn1, e), mx1 = std::max(mx1, e);
        }
        const double n = dr.vertex_count;
        std::printf("DIFF part %u cand ed%02u/p%u/%u mesh %u: max_abs (%.3g %.3g %.3g) mean (%.3g %.3g %.3g) rms (%.3g %.3g %.3g) range y [%.4g %.4g]\n", o->part, ep.pac, ep.pach, ep.leaf, ep.mesh,
                    mx[0], mx[1], mx[2], sm[0] / n, sm[1] / n, sm[2] / n, std::sqrt(ss[0] / n), std::sqrt(ss[1] / n), std::sqrt(ss[2] / n), mn1, mx1);
        if (dump_v > 0 && o->part == uint32_t(dump_part)) {
          for (const auto& r : roles.r) if (r.role == wg::Role::Fill0) {   // lane 179: the vertices whose fill0 byte differs from the file x's top byte, with both x floats
            int bad = 0;
            for (uint32_t v = 0; v < dr.vertex_count && bad < dump_v; ++v) {
              float xs, xo;
              std::memcpy(&xs, svb.data() + size_t(v) * lay.host_stride + posoff, 4);
              std::memcpy(&xo, ours.data() + size_t(v) * lay.host_stride + posoff, 4);
              uint32_t bs, bo;
              std::memcpy(&bs, &xs, 4), std::memcpy(&bo, &xo, 4);
              const uint8_t f = svb[size_t(v) * lay.host_stride + r.offset + 3];
              if (f != uint8_t(bo >> 24) && f != 255) { ++bad; std::printf("   FILL0 v%u stream fill0 byte %02x, stream x %.9g (top byte %02x), file x*1 %.9g (top byte %02x)\n", v, f, double(xs), bs >> 24, double(xo), bo >> 24); }
            }
          }
          uint32_t off_count = 0;
          int shown = 0;
          for (uint32_t v = 0; v < dr.vertex_count; ++v) {
            float a[3], b[3];
            double worst = 0;
            for (int c = 0; c < 3; ++c) { std::memcpy(&a[c], ours.data() + size_t(v) * lay.host_stride + posoff + 4 * c, 4); std::memcpy(&b[c], svb.data() + size_t(v) * lay.host_stride + posoff + 4 * c, 4); worst = std::max(worst, std::fabs(double(b[c]) - K * double(a[c]))); }
            if (worst > 5e-4) ++off_count;
            if (worst > 5e-4 && shown < dump_v) {
              ++shown;
              int hit = -1, mirror_hit = -1;   // the file vertex whose scaled position IS this stream vertex, and the one whose x-mirrored scaled position is
              for (uint32_t j = 0; j < dr.vertex_count; ++j) {
                float f[3];
                for (int c = 0; c < 3; ++c) std::memcpy(&f[c], ours.data() + size_t(j) * lay.host_stride + posoff + 4 * c, 4);
                const double e0 = std::fabs(double(b[0]) - K * double(f[0])), e1 = std::fabs(double(b[1]) - K * double(f[1])), e2 = std::fabs(double(b[2]) - K * double(f[2]));
                const double m0 = std::fabs(double(b[0]) + K * double(f[0]));
                if (hit < 0 && std::max(e0, std::max(e1, e2)) < 1e-3) hit = int(j);
                if (mirror_hit < 0 && std::max(m0, std::max(e1, e2)) < 1e-3) mirror_hit = int(j);
              }
              int same_other = 0, same_other_hit = 0, total_other = 0;   // the bytes outside the position: equal to the file's own vertex v / the vertex `hit`
              for (uint32_t k = 0; k < lay.host_stride; ++k) {
                if (int(k) >= posoff && int(k) < posoff + 16) continue;
                ++total_other;
                if (ours[size_t(v) * lay.host_stride + k] == svb[size_t(v) * lay.host_stride + k]) ++same_other;
                if (hit >= 0 && ours[size_t(hit) * lay.host_stride + k] == svb[size_t(v) * lay.host_stride + k]) ++same_other_hit;
              }
              std::printf("   v%u OFF %.4g file (%.6f %.6f %.6f) stream (%.6f %.6f %.6f) = file vertex %d, x-mirror of file vertex %d | other bytes equal to file v%u: %d/%d, to the hit: %d/%d\n", v, worst, double(a[0]), double(a[1]), double(a[2]), double(b[0]), double(b[1]), double(b[2]), hit, mirror_hit, v, same_other, total_other, same_other_hit, total_other);
            }
          }
          std::printf("   vertices beyond 5e-4 absolute: %u of %u\n", off_count, dr.vertex_count);
        }
      }
    }
    return 0;
  }
  CawCore core(roster, faults, /*sync=*/true);
  uint64_t frames = 0, claimed = 0, replaced = 0, proof = 0, wrong_vertex = 0, wrong_index = 0, wrong_count = 0;
  uint64_t kept[size_t(KeepWhy::Count)] = {};
  std::map<std::string, std::pair<uint64_t, uint64_t>> by_part;   // "object:part" -> replaced, kept
  std::map<std::string, std::string> bg_src;                       // lane 169: "object:part" -> the bg66.pac entry its join was confirmed against
  uint64_t bg_replaced = 0;
  std::map<std::string, std::string> body_src;   // lane 179: "object:part" -> the edit leaf its join was confirmed against
  LoadedFrame fr;
  std::vector<uint8_t> vb, ib;
  uint64_t morph_offset = 0, morph_bytes = 0, morph_object = 0, morph_part = 0;
  bool morph_set = false;
  int morph_refused = -1;
  uint64_t morph_wrong_after = 0, refused_before = 0;
  for (int f = first; f <= last; f += step) {
    if (!cap.LoadFrame(uint32_t(f), fr)) { std::fprintf(stderr, "caw_ours_check: FAIL cannot load frame %d\n", f); return 2; }
    ++frames;
    // The slider move (review S1): geometry is content-addressed, a changed vertex array has a NEW offset. From frame morph_at + 1 the morphed part's draw points at morph_offset + kMorphShift,
    // where the capture's bytes are the original's with vertex 0's x moved (by morph_rel of max(1, |x|) when given, else 0.5 units).
    constexpr uint64_t kMorphShift = 1ull << 40;
    auto read = [&](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) {
      if (morph_set && f > morph_at && offset == morph_offset + kMorphShift) {
        if (!cap.ReadGeometry(morph_offset, bytes, out)) return false;
        if (out.size() >= 16) {
          float x;
          std::memcpy(&x, out.data(), 4);
          x += morph_rel > 0 ? float(morph_rel * std::max(1.0, std::fabs(double(x)))) : 0.5f;
          std::memcpy(out.data(), &x, 4);
        }
        return true;
      }
      return cap.ReadGeometry(offset, bytes, out);
    };
    if (!core.BeginFrame(fr)) continue;
    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const DrawOrigin* o = fr.OriginOf(d);
      if (!o || (o->pass_site != kModelPartColourSite && o->pass_site != kModelPartMirrorSite) || o->object == 0) continue;
      ++claimed;
      if (morph_set && f > morph_at && o->object == morph_object && o->part == morph_part) const_cast<Draw&>(fr.draws[d]).vertex_offset = morph_offset + kMorphShift;   // (the harness's own frame copy)
      CawDecision dec;
      KeepWhy why = KeepWhy::NotOurs;
      const bool decided = core.Decide(fr, d, read, dec, why);
      char pk[64];
      std::snprintf(pk, sizeof pk, "%08X:%u", o->object, o->part);
      if (!decided) {
        ++kept[size_t(why)];
        ++by_part[pk].second;
        if (morph_set && f > morph_at && morph_refused < 0 && o->object == morph_object && o->part == morph_part && core.stats().stale_caught > refused_before) morph_refused = f;
        continue;
      }
      ++replaced;
      ++by_part[pk].first;
      if (dec.backdrop) { char sb[96]; std::snprintf(sb, sizeof sb, "pac=%u pach=%u entry=%u", dec.src_pac, dec.src_pach, dec.src_leaf); bg_src[pk] = sb; ++bg_replaced; }   // lane 169
      else { char sb[96]; std::snprintf(sb, sizeof sb, "pac=%u pach=%u leaf=%u mesh=%u", dec.src_pac, dec.src_pach, dec.src_leaf, dec.mesh); body_src[pk] = sb; }   // lane 179
      ++proof;
      const Draw& dr = fr.draws[d];
      if (morph_at >= 0 && !morph_set && f == morph_at) {
        morph_set = true, morph_offset = dr.vertex_offset, morph_bytes = dr.vertex_bytes, morph_object = o->object, morph_part = o->part;
        refused_before = core.stats().stale_caught;
      }
      if (dec.prepared->vertex_count != dr.vertex_count || dec.prepared->index.size() != dr.index_count) { ++wrong_count; continue; }
      const VertexLayout& lay = fr.layouts[dr.layout];
      if (dr.vertex_bytes != size_t(dec.prepared->stride) * dr.vertex_count || dec.prepared->stride != lay.host_stride) { ++wrong_vertex; continue; }
      if (!read(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) { ++wrong_vertex; continue; }
      bool bad = false;
      for (uint32_t v = 0; v < dr.vertex_count && !bad; ++v) {
        const uint8_t* a = dec.prepared->vertex.data() + size_t(v) * lay.host_stride;
        const uint8_t* b = vb.data() + size_t(v) * lay.host_stride;
        if (!std::memcmp(a, b, lay.host_stride)) continue;
        for (uint32_t e = 0; e < lay.element_count && !bad; ++e) {
          const VertexElement& el = fr.elements[lay.element_first + e];
          const size_t n = el.vk_format == 109 ? 16 : el.vk_format == 106 ? 12 : el.vk_format == 92 ? 8 : 4;
          if (!std::memcmp(a + el.host_offset, b + el.host_offset, n)) continue;
          if (el.vk_format == 109) {
            for (int c = 0; c < 3; ++c) {
              float fa, fb;
              std::memcpy(&fa, a + el.host_offset + 4 * c, 4), std::memcpy(&fb, b + el.host_offset + 4 * c, 4);
              if (!(std::fabs(double(fa) - double(fb)) <= 5e-4 * std::max(1.0, std::fabs(double(fb))))) bad = true;
            }
            if (std::memcmp(a + el.host_offset + 12, b + el.host_offset + 12, 4)) bad = true;
          } else if (el.vk_format == 92) {
            for (int c = 0; c < 4; ++c) {
              int16_t qa, qb;
              std::memcpy(&qa, a + el.host_offset + 2 * c, 2), std::memcpy(&qb, b + el.host_offset + 2 * c, 2);
              if (std::abs(int(qa) - int(qb)) > 70) bad = true;
            }
          } else if (el.vk_format == 37 && el.guest_offset == 0) {   // lane 179: fill0 = the high byte of the STREAM's own position x (a vertex near x = 0 moved by 1e-5 flips it)
            bool fill_ok = false;
            for (uint32_t e2 = 0; e2 < lay.element_count; ++e2) {
              const VertexElement& pe = fr.elements[lay.element_first + e2];
              if (pe.vk_format != 109) continue;
              float xa, xb;
              uint32_t bb;
              std::memcpy(&xa, a + pe.host_offset, 4), std::memcpy(&xb, b + pe.host_offset, 4), std::memcpy(&bb, &xb, 4);
              fill_ok = a[el.host_offset] == b[el.host_offset] && a[el.host_offset + 1] == b[el.host_offset + 1] && a[el.host_offset + 2] == b[el.host_offset + 2] && b[el.host_offset + 3] == uint8_t(bb >> 24) &&
                        std::fabs(double(xa) - double(xb)) <= 5e-4 * std::max(1.0, std::fabs(double(xb)));
            }
            if (!fill_ok) bad = true;
          } else bad = true;
        }
      }
      if (bad) { ++wrong_vertex; if (morph_set && f > morph_at) ++morph_wrong_after; }
      if (!cap.ReadGeometry(dr.index_offset, dr.index_bytes, ib) || ib.size() != dec.prepared->index.size() * 2 || std::memcmp(ib.data(), dec.prepared->index.data(), ib.size())) ++wrong_index;
    }
  }
  if (claimed == 0) { std::fprintf(stderr, "caw_ours_check: FAIL no main-pass model draw in frames %d-%d\n", first, last); return 2; }
  std::printf("ARM faults=%s frames=%llu\n", fault_list.empty() ? "none" : fault_list.c_str(), (unsigned long long)frames);
  std::printf("%s\n", core.Report().c_str());
  for (const auto& kv : by_part) if (kv.second.first) std::printf("CAWPART %s replaced=%llu kept=%llu\n", kv.first.c_str(), (unsigned long long)kv.second.first, (unsigned long long)kv.second.second);
  for (const auto& kv : body_src) std::printf("PARTSRC %s %s\n", kv.first.c_str(), kv.second.c_str());   // lane 179
  for (const auto& kv : bg_src) std::printf("BGSRC %s %s\n", kv.first.c_str(), kv.second.c_str());   // lane 169 (N3b)
  std::printf("BACKDROP replaced=%llu\n", (unsigned long long)bg_replaced);
  std::printf("RESULT claimed=%llu replaced=%llu kept_unjoined=%llu kept_mismatch=%llu kept_variant=%llu kept_not_ours=%llu proof_draws=%llu wrong_vertex=%llu wrong_index=%llu wrong_count=%llu stale_caught=%llu\n",
              (unsigned long long)claimed, (unsigned long long)replaced, (unsigned long long)kept[size_t(KeepWhy::Unjoined)], (unsigned long long)kept[size_t(KeepWhy::Mismatch)],
              (unsigned long long)kept[size_t(KeepWhy::Variant)], (unsigned long long)kept[size_t(KeepWhy::NotOurs)], (unsigned long long)proof, (unsigned long long)wrong_vertex,
              (unsigned long long)wrong_index, (unsigned long long)wrong_count, (unsigned long long)core.stats().stale_caught);
  if (census) for (const std::string& n : core.notes) std::printf("NOTE %s\n", n.c_str());
  if (morph_at >= 0) std::printf("MORPH set=%d frame=%d refused_frame=%d wrong_after=%llu\n", morph_set ? 1 : 0, morph_at, morph_refused, (unsigned long long)morph_wrong_after);
  (void)census;
  std::printf("run time %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return 0;
}
