// ring_replay: S103 lane C6. The offline replay of the ring's covers and apron skirts over a real mode-1 case (run 26: CPU vs CPU, ropes moving). 0 game runs: files only.
//   ring_replay [--plant NAME] FILE.rsc...
// It runs the SHIPPED contributor (src\scene_live\ring_recompute_contrib.h: the readers, ReadSkirtVisibility, the rules, the whole-ring verdict) on a FAKE GUEST built at the LISTING literals (the unit's
// BuildGuest / BuildPieces: lis 1 | ori for the cover descriptors, lis -31889 = 0x836F for the camera record), whose cover and skirt inputs are taken from the record's own STREAM draws:
//   - the grid corners from the game's rope rows, the cover model's inverse bind derived once from the first record (as ring_pieces_fit), the corner fades from the game's cover alphas (an undrawn
//     third-layer instance = fade 0), the descriptors {n 2, side = corner, c 0};
//   - the camera record's floats at +144 / +152 = the render camera's eye x / z (the vector sub_82609060 reads is [U] position or heading: the stream cannot tell, both name the same quadrant for this
//     camera), the skirt models' palettes = the game's skirt rows (the drawn objects, in address order, take the sides that face the camera; the side NUMBERING of the objects is [U]: the stream names
//     objects, not sides).
// Per record it compares what OUR recompute emits with what the game DREW: the cover instances (call set and node rows, 2e-3) and the skirts (count, and the rows of every drawn one bit for bit). It also
// prints what the case's own recorded OURS rows (the code of run 26) did, to show the defect. Planted faults: cv_desc_low, skirt_glob_wrong (the two address misreads), skirt_hidden_none (the visibility
// rule never hides one), cover_drop (corner 2 dropped): each must make the comparison fail.
#define main ring_unit_main
#include "ring_recompute_unit.cpp"
#include "../../src/renderer_scene/rows_ring.h"
#undef main

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>

static void MatrixFromRows(const float* rows, float* m) {   // the inverse of RowsFromMatrix: rows[4k + j] = m[4j + k] (k < 3), the fourth column (0, 0, 0, 1)
  for (int j = 0; j < 4; ++j) {
    for (int k = 0; k < 3; ++k) m[4 * j + k] = rows[4 * k + j];
    m[4 * j + 3] = j == 3 ? 1.f : 0.f;
  }
}

int main(int argc, char** argv) {
  std::vector<const char*> files;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
    else files.push_back(argv[i]);
  }
  scene_frame::g_mode_cvar.store(&g_mode);
  float inv_t[3] = {0, 0, 0};
  bool have_inv = false;
  uint64_t recs = 0, cov_inst = 0, cov_equal = 0, cov_differ = 0, sk_inst = 0, sk_equal = 0, sk_differ = 0, sk_count_equal = 0, whole_clean = 0, skipped = 0;
  uint64_t old_cov_ours = 0, old_cov_game = 0, old_sk_ours = 0, old_sk_game = 0;
  double cov_worst = 0, rope_gap = 0;
  for (const char* path : files) {
    SceneTables t;
    std::string err;
    if (!ReadRecord(path, t, err)) { std::printf("%s: read failed: %s\n", path, err.c_str()); return 1; }
    // ---- the stream's draws
    std::map<uint32_t, const RingRows*> rope, cover;
    std::vector<const RingRows*> skirts;
    for (const RingRows& r : t.ring_rows) {
      if (r.flags & kRrRecomputed) {
        old_cov_ours += (r.klass == kRcCoverHi), old_sk_ours += (r.klass == kRcSkirt);
        continue;
      }
      if (r.klass == kRcRopeHi || r.klass == kRcRopeLo) rope[r.call] = &r;
      else if (r.klass == kRcCoverHi) cover[r.call] = &r, ++old_cov_game;
      else if (r.klass == kRcSkirt) skirts.push_back(&r), ++old_sk_game;
    }
    if (rope.size() != 12 || cover.empty()) { ++skipped; continue; }
    ++recs;
    auto node = [&](const RingRows* r, uint32_t k, float* rows12) { std::memcpy(rows12, t.ring_nodes[r->first_node + k].rows, 48); };
    // ---- the fake guest at the listing literals, then the record's own inputs over it
    // THE GRID. The game's cover rows hold grid record + inverse bind * R(angle): the stream does not carry the grid layers, so pass 0 runs with an all-zero grid, and pass 1 sets every corner record
    // to (the game's cover node 2 translation) - (pass 0's): the grid VALUES are therefore the stream's by construction, and what the replay checks is the READ PATH and the RULE (the descriptors, the
    // counts, the angle and rotation rows, the fades, the call set, the visible skirts, the whole-ring verdict). The rope-derived corner (as ring_pieces_fit uses) is printed apart: while the ropes move it
    // sits up to ~2 units off the grid record the cover reads (informational: the live mirror reads the grid itself).
    float recv[3][4][3] = {};
    std::unique_ptr<SceneTables> op;
    std::array<const RingRows*, 4> by_side{};
    rr::RingCensus st;
    for (int pass = 0; pass < 2; ++pass) {
    BuildGuest(false);
    g_guest.Put(kX + rr::kCountsOff + 4 * 3, 1);   // side 3: one descriptor (12 ropes), as the unit's whole-ring test
    g_guest.Put(kX + rr::kEntriesOff + 12 * (128 * 3), 1), g_guest.Put(kX + rr::kEntriesOff + 12 * (128 * 3) + 4, 0), g_guest.Put(kX + rr::kEntriesOff + 12 * (128 * 3) + 8, 0);
    const uint32_t stride = 80;
    for (uint32_t h = 0; h < 3; ++h)
      for (uint32_t s = 0; s < 4; ++s) {
        const uint32_t rec = LayerBuf(h) + stride * (24 * s);
        g_guest.PutF(rec, recv[h][s][0]), g_guest.PutF(rec + 4, recv[h][s][1]), g_guest.PutF(rec + 8, recv[h][s][2]);
      }
    (void)inv_t, (void)have_inv;
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t d = kX + ((0x10000u | 62056u) - 8u) + 12u * (3 * c) - (g_plant == "cv_desc_low" ? 0x10000u : 0u);
      g_guest.Put(kX + 31932u * 4u + 4 * c, 1);
      g_guest.Put(d, 2), g_guest.Put(d + 4, c), g_guest.Put(d + 8, 0);
      float alpha = 0.f;
      auto it = cover.find(3 * c + 2);
      if (it != cover.end()) alpha = it->second->alpha;
      g_guest.PutF(kX + 4u * (2342 + c), alpha);
      g_guest.Put(kX + 4u * (2364 + c), 0);   // no third-layer override
    }
    // the camera record: the render camera's eye x / z at +144 / +152
    {
      const uint32_t P = 0x40900000, arr = 0x40910000;
      g_guest.PutF(arr + 144, t.camera.eye[0]), g_guest.PutF(arr + 152, t.camera.eye[2]);
      (void)P;
    }
    // the skirt models: the drawn objects (address order) take the sides that face the camera; the hidden side gets a dummy palette
    uint32_t hidden = rr::kSkirtHiddenSide[rr::QuadrantOf(t.camera.eye[0], t.camera.eye[2], 0.f)];
    std::vector<const RingRows*> drawn = skirts;
    std::sort(drawn.begin(), drawn.end(), [](const RingRows* a, const RingRows* b) { return a->object < b->object; });
    by_side = {};
    {
      size_t di = 0;
      for (uint32_t s = 0; s < 4; ++s) {
        if (s == hidden) continue;
        if (di < drawn.size()) by_side[s] = drawn[di++];
      }
    }
    for (uint32_t s = 0; s < 4; ++s) {
      const uint32_t m = kSkBase + 0x10000u * s, pal = m + 0x8000;
      const RingRows* g = by_side[s];
      const uint32_t n = g ? g->n_nodes : 4;
      g_guest.Put(m + 300, n);
      for (uint32_t k = 0; k < n; ++k) {
        float mat[16] = {};
        if (g) {
          float r12[12];
          node(g, k, r12);
          MatrixFromRows(r12, mat);
        } else {
          for (int w = 0; w < 16; ++w) mat[w] = (w % 5 == 0) ? 1.f : 0.f;
        }
        for (int w = 0; w < 16; ++w) g_guest.PutF(pal + 64 * k + 4 * uint32_t(w), mat[w]);
      }
    }
    if (g_plant == "skirt_glob_wrong") g_guest.Put(0x836F3AA8u, 0);   // the reader's address finds nothing (the unit plants the reverse: this is the shipped-vs-fake mismatch)
    // ---- our recompute, through the shipped contributor
    rr::RingRecomputeContributor con;
    con.SetReader(ReadThunk);
    con.NoteRing(kX, 5000 + recs);
    FrameInfo f;
    f.guest_frame = 5000 + recs;
    con.Latch(f);
    op = std::make_unique<SceneTables>();
    Counters cc;
    con.Emit(f, *op, cc);
    st = con.Stats();
    if (pass == 0) {   // the grid record the cover of (corner c, layer h) reads is (side (c + 1) % 4): game node 2 translation - ours at zero grid
      for (uint32_t c = 0; c < 4; ++c)
        for (uint32_t h = 0; h < 3; ++h) {
          const RingRows* g = cover.count(3 * c + h) ? cover[3 * c + h] : nullptr;
          const RingRows* oo = nullptr;
          for (const RingRows& r : op->ring_rows)
            if (r.klass == kRcCoverHi && r.call == 3 * c + h) oo = &r;
          if (!g || !oo || g->n_nodes < 3 || oo->n_nodes < 3) continue;
          const float* gt = t.ring_nodes[g->first_node + 2].rows;
          const float* ot = op->ring_nodes[oo->first_node + 2].rows;
          const uint32_t s = (c + 1) % 4;
          recv[h][s][0] = gt[3] - ot[3], recv[h][s][1] = gt[7] - ot[7], recv[h][s][2] = gt[11] - ot[11];
        }
    }
    }   // passes
    SceneTables& o = *op;
    whole_clean += st.whole_clean;
    for (uint32_t c = 0; c < 4; ++c) {   // informational: the rope-derived corner against the cover-derived grid record (y)
      const uint32_t s = (c + 1) % 4;
      auto it = rope.find(3 * s);
      if (it == rope.end() || it->second->n_nodes < 3) continue;
      rope_gap = std::max(rope_gap, double(std::fabs(t.ring_nodes[it->second->first_node + 2].rows[7] - recv[0][s][1])));
    }
    // ---- covers: the call set and the rows
    std::map<uint32_t, const RingRows*> ours_cov;
    std::map<uint32_t, const RingRows*> ours_sk;
    for (const RingRows& r : o.ring_rows) {
      if (r.klass == kRcCoverHi) ours_cov[r.call] = &r;
      if (r.klass == kRcSkirt) ours_sk[r.pad2] = &r;
    }
    for (uint32_t call = 0; call < 12; ++call) {
      const bool g_has = cover.count(call) != 0, o_has = ours_cov.count(call) != 0 && !(g_plant == "cover_drop" && call / 3 == 2);
      if (!g_has && !o_has) continue;
      ++cov_inst;
      if (g_has != o_has) { ++cov_differ; continue; }
      double d = 0;
      const RingRows* g = cover[call];
      const RingRows* oo = ours_cov[call];
      if (g->n_nodes != oo->n_nodes) d = 1e9;
      for (uint32_t k = 0; k < std::min<uint32_t>(g->n_nodes, oo->n_nodes); ++k)
        for (int q = 0; q < 12; ++q) d = std::max(d, double(std::fabs(t.ring_nodes[g->first_node + k].rows[q] - o.ring_nodes[oo->first_node + k].rows[q])));
      cov_worst = std::max(cov_worst, d);
      (d < 2e-3 ? cov_equal : cov_differ)++;
    }
    // ---- skirts: one per drawn object; ours per side
    uint32_t ours_count = uint32_t(ours_sk.size());
    if (g_plant == "skirt_hidden_none") ours_count = 4;
    if (ours_count == skirts.size()) ++sk_count_equal;
    for (uint32_t s = 0; s < 4; ++s) {
      const RingRows* g = by_side[s];
      const bool o_has = ours_sk.count(s) != 0;
      if (!g && !o_has) continue;
      ++sk_inst;
      if (!g || !o_has) { ++sk_differ; continue; }
      const RingRows* oo = ours_sk[s];
      bool same = g->n_nodes == oo->n_nodes;
      for (uint32_t k = 0; same && k < g->n_nodes; ++k)
        for (int q = 0; q < 12; ++q) same = same && t.ring_nodes[g->first_node + k].rows[q] == o.ring_nodes[oo->first_node + k].rows[q];
      (same ? sk_equal : sk_differ)++;
    }
  }
  std::printf("records %llu (skipped %llu): covers %llu instances: equal %llu, differ %llu (worst %.6g); skirts %llu instances: equal %llu, differ %llu, records with the same skirt count %llu; whole-ring clean %llu of %llu; rope-derived corner y vs the grid record: worst gap %.4g; "
              "the case's recorded OURS (the code of run 26): cover rows %llu of the game's %llu, skirt rows %llu of the game's %llu\n",
              (unsigned long long)recs, (unsigned long long)skipped, (unsigned long long)cov_inst, (unsigned long long)cov_equal, (unsigned long long)cov_differ, cov_worst, (unsigned long long)sk_inst,
              (unsigned long long)sk_equal, (unsigned long long)sk_differ, (unsigned long long)sk_count_equal, (unsigned long long)whole_clean, (unsigned long long)recs, rope_gap, (unsigned long long)old_cov_ours,
              (unsigned long long)old_cov_game, (unsigned long long)old_sk_ours, (unsigned long long)old_sk_game);
  return 0;
}
