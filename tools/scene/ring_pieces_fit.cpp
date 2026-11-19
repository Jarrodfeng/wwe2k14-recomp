// ring_pieces_fit: S103 lane C4. Runs the SHIPPED cover and post rules (src\scene_live\ring_recompute_core.h) against the GAME's own RingRows of real mode-1 records (run 16), 0 game runs: files only.
//   ring_pieces_fit [--plant NAME] FILE.rsc...
// COVERS: the game's cover calls (class cover_hi, call = 3 corner + layer) vs CoverNodes(). The grid records the rule reads are not in a record, so they are DERIVED from the game's rope rows of the same record
// (rope node 2 of side s, layer h is grid record 24 s: row + the rest point P(s, 0, h), the rule of NodeTranslation run backwards, [C: node 2 = descriptor (a = s, b = 0), run 16's rope rows end exactly on the rest
// corners]); the cover model's inverse bind (node 2: the one constant translation, node 0 / 1 identity) is DERIVED ONCE from the first record's call 0 and must then hold for every other instance of every record.
// POSTS: the game's post calls vs PostNodes() (no input but the constants of the image words). STATIC: posts and canvas rows must be identical in every record (they never change); skirts must differ (they move).
// Planted faults: side_off (the descriptor's side + 1), ang_base (the angle base pi/2), post_step (step 0): each must make the comparison fail.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/ring_recompute_core.h"

using namespace rdr_scene;
namespace rr = rdr_scene::live::ringrecompute;

static float F(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
static std::string g_plant;

struct Calls {
  std::map<uint32_t, std::vector<std::array<float, 12>>> by_call;   // call -> node rows (12 floats each)
};
static Calls Collect(const SceneTables& t, std::initializer_list<uint8_t> classes) {
  Calls c;
  for (const RingRows& r : t.ring_rows) {
    if (r.flags & kRrRecomputed) continue;
    bool want = false;
    for (uint8_t k : classes) want = want || r.klass == k;
    if (!want) continue;
    auto& v = c.by_call[r.call];
    v.clear();
    for (uint32_t k = 0; k < r.n_nodes; ++k) {
      std::array<float, 12> a;
      std::memcpy(a.data(), t.ring_nodes[r.first_node + k].rows, 48);
      v.push_back(a);
    }
  }
  return c;
}

int main(int argc, char** argv) {
  std::vector<const char*> files;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
    else files.push_back(argv[i]);
  }
  float inv_t[3] = {0, 0, 0};
  bool have_inv = false;
  double cv_worst = 0, post_worst = 0;
  uint64_t cv_inst = 0, cv_bad = 0, post_inst = 0, post_bad = 0, recs = 0, post_changed = 0, canvas_changed = 0, skirt_changed = 0, skirt_recs = 0;
  std::map<uint32_t, std::vector<std::array<float, 12>>> first_posts, first_canv, prev_skirts;
  for (const char* path : files) {
    SceneTables t;
    std::string err;
    if (!ReadRecord(path, t, err)) { std::printf("%s: read failed: %s\n", path, err.c_str()); return 1; }
    const Calls rope = Collect(t, {kRcRopeHi, kRcRopeLo}), cover = Collect(t, {kRcCoverHi}), post = Collect(t, {kRcPostHi}), canvas = Collect(t, {kRcCanvas}), skirt = Collect(t, {kRcSkirt});
    if (rope.by_call.size() != 12 || cover.by_call.size() != 12 || post.by_call.size() != 4) { std::printf("%s: calls rope %zu cover %zu post %zu (want 12 / 12 / 4)\n", path, rope.by_call.size(), cover.by_call.size(), post.by_call.size()); return 1; }
    ++recs;
    // ---- the grid layers from the rope rows: corner records 24 s of layer h
    rr::Inputs grid;
    for (uint32_t h = 0; h < 3; ++h) {
      grid.layer[h].assign(size_t(rr::kRecCount) * 3, 0.f);
      for (uint32_t s = 0; s < 4; ++s) {
        const auto& nodes = rope.by_call.at(3 * s + h);
        if (nodes.size() < 3) { std::printf("%s: rope %u has %zu nodes\n", path, 3 * s + h, nodes.size()); return 1; }
        float p[3];
        rr::BasePoint(int32_t(s), 0, h, p);
        const std::array<float, 12>& n2 = nodes[2];
        float src[3] = {n2[3], n2[7], n2[11]};
        if ((s & 1u) == 0) src[0] += p[0];
        else src[2] += p[2];
        for (int k = 0; k < 3; ++k) grid.layer[h][size_t(24 * s) * 3 + size_t(k)] = src[k];
      }
    }
    // ---- covers
    rr::CvInputs cv;
    cv.ok = true;
    cv.ang_base = F(g_plant == "ang_base" ? 0x3FC90FDBu : 0x4096CBE4u), cv.ang_step = F(0x3FC90FDBu), cv.pi = F(0x40490FDBu), cv.neg_pi = F(0xC0490FDBu), cv.two_pi = F(0x40C90FDBu);
    rr::ModelPal& m = cv.models[0];
    m.model = 1, m.palette = 1, m.n = 3;
    m.pal.assign(48, 0.f), m.inv.assign(48, 0.f);
    for (int k = 0; k < 3; ++k) m.pal[size_t(k) * 16 + size_t(0)] = m.pal[size_t(k) * 16 + 5] = m.pal[size_t(k) * 16 + 10] = m.pal[size_t(k) * 16 + 15] = 1.f, m.inv[size_t(k) * 16 + 0] = m.inv[size_t(k) * 16 + 5] = m.inv[size_t(k) * 16 + 10] = m.inv[size_t(k) * 16 + 15] = 1.f;
    for (uint32_t c = 0; c < 4; ++c) {
      cv.count[c] = 1;
      cv.desc[c].push_back(rr::CvDesc{2, int32_t(c) + (g_plant == "side_off" ? 1 : 0), 0});
    }
    if (!have_inv) {   // derive node 2's inverse-bind translation from THIS record's call 0 (corner 0, layer 0): t = rec + inv.t * R(angle 0) = rec + inv.t
      const auto& cvn = cover.by_call.at(0)[2];
      const float* rec = &grid.layer[0][size_t(24 * 1) * 3];   // side (0 + 1) % 4 = 1
      inv_t[0] = cvn[3] - rec[0], inv_t[1] = cvn[7] - rec[1], inv_t[2] = cvn[11] - rec[2];
      have_inv = true;
      std::printf("derived inv.t of cover node 2 from the first record's call 0: (%.5f, %.5f, %.5f)\n", inv_t[0], inv_t[1], inv_t[2]);
    }
    m.inv[2 * 16 + 12] = inv_t[0], m.inv[2 * 16 + 13] = inv_t[1], m.inv[2 * 16 + 14] = inv_t[2];
    rr::RingCensus cn;
    for (uint32_t c = 0; c < 4; ++c)
      for (uint32_t l = 0; l < 3; ++l) {
        std::vector<float> nodes;
        float alpha = 0;
        ++cv_inst;
        if (!rr::CoverNodes(cv, grid, 0, c, l, nodes, alpha, cn)) { ++cv_bad; continue; }
        double d = 0;
        const auto& g = cover.by_call.at(3 * c + l);
        for (size_t k = 0; k < g.size() && k < 3; ++k) {
          RingNodeRows ours;
          live::arenaworld::RowsFromMatrix(&nodes[k * 16], ours.rows);
          for (int f = 0; f < 12; ++f) d = std::max(d, double(std::fabs(ours.rows[f] - g[k][size_t(f)])));
        }
        cv_worst = std::max(cv_worst, d);
        if (!(d < 2e-3)) ++cv_bad;
      }
    // ---- posts
    rr::PostInputs po;
    po.ok = true;
    po.step = F(g_plant == "post_step" ? 0u : 0x3FC90FDBu), po.pi = F(0x40490FDBu), po.neg_pi = F(0xC0490FDBu), po.two_pi = F(0x40C90FDBu);
    for (uint32_t i = 0; i < 4; ++i) {
      std::vector<float> nodes;
      const auto& g = post.by_call.at(i);
      rr::PostNodes(po, i, uint32_t(g.size()), nodes);
      double d = 0;
      for (size_t k = 0; k < g.size(); ++k) {
        RingNodeRows ours;
        live::arenaworld::RowsFromMatrix(&nodes[k * 16], ours.rows);
        for (int f = 0; f < 12; ++f) d = std::max(d, double(std::fabs(ours.rows[f] - g[k][size_t(f)])));
      }
      ++post_inst;
      post_worst = std::max(post_worst, d);
      if (!(d < 1e-5)) ++post_bad;
    }
    // ---- static / moving
    if (recs == 1) {
      first_posts = post.by_call;
      first_canv = canvas.by_call;
      prev_skirts = skirt.by_call;
    } else {
      if (first_posts != post.by_call) ++post_changed;
      if (first_canv != canvas.by_call) ++canvas_changed;
      ++skirt_recs;
      if (prev_skirts != skirt.by_call) ++skirt_changed;
      prev_skirts = skirt.by_call;
    }
  }
  std::printf("records %llu: covers %llu instances, %llu off (worst %.6g); posts %llu instances, %llu off (worst %.6g); posts changed in %llu records, canvas changed in %llu records, skirts changed in %llu of %llu consecutive records\n",
              (unsigned long long)recs, (unsigned long long)cv_inst, (unsigned long long)cv_bad, cv_worst, (unsigned long long)post_inst, (unsigned long long)post_bad, post_worst, (unsigned long long)post_changed,
              (unsigned long long)canvas_changed, (unsigned long long)skirt_changed, (unsigned long long)skirt_recs);
  return 0;
}
