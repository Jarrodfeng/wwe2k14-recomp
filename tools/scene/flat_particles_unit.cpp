// S111 TITLESMOKE: src\renderer_scene\flat_particles.h (where a flat frame's particle groups go among its 2D rows). Pure: no renderer, no game, no files.
//   flat_particles_unit.exe [--plant end|off_by_one|scene_flat|old_count]   (tools\scene\flat_particles_check.py builds it and requires each plant to FAIL its named check)
// S111 FLATSEQ: the batches carry the 2D store's ticket (seq_after_p1) and the positions come from the rows' seq; a lost row before the node call must not move the group (twin old_count).
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "renderer_scene/flat_particles.h"
#include "renderer_scene/quads2d_core.h"   // S111 FLATSEQ review: the real q2d store (Ticket / LastTicket) and q2d::TakeFrame, for seq.store_chain

using namespace rdr_scene;
namespace fp = rdr_scene::flat_particles;

static int g_pass = 0, g_fail = 0;
static void Check(bool ok, const char* name, const std::string& detail = std::string()) {
  if (ok) {
    ++g_pass;
    return;
  }
  ++g_fail;
  std::printf("FAIL %s%s%s\n", name, detail.empty() ? "" : ": ", detail.c_str());
}
static ParticleBatch Batch(uint32_t first, uint32_t seq_after_p1, uint8_t site) {
  ParticleBatch b;
  b.first = first, b.count = 1, b.seq_after_p1 = seq_after_p1, b.site = site;
  return b;
}
// The frame's record rows with these tickets (only the seq matters to a position).
static std::vector<Quad2D> Rows(std::initializer_list<uint32_t> seqs) {
  std::vector<Quad2D> r;
  for (uint32_t s : seqs) {
    Quad2D q;
    q.seq = s;
    r.push_back(q);
  }
  return r;
}
static std::string StopsText(const fp::Stops& s) {
  std::string t;
  for (size_t i = 0; i < s.n; ++i) t += (i ? "," : "") + std::to_string(s.at[i]);
  return t.empty() ? std::string("none") : t;
}

int main(int argc, char** argv) {
  const std::string plant = argc > 2 && std::strcmp(argv[1], "--plant") == 0 ? argv[2] : "";
  if (plant == "end") fp::g_plant = 1;
  else if (plant == "off_by_one") fp::g_plant = 2;
  else if (plant == "scene_flat") fp::g_plant = 3;
  else if (plant == "old_count") fp::g_plant = 4;
  else if (!plant.empty()) {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  ParticleScreen sc;
  sc.view_proj[0] = 2.f / 1280.f, sc.view_proj[5] = -2.f / 720.f, sc.view_proj[10] = 0.0033f, sc.view_proj[15] = 1.f;
  sc.calls = 2, sc.row_count = 8, sc.stamp = kParticleStampSeq;

  // 1. The title's shape (docs\research\s103\MP_menu_particles.md section 1): backdrop rows, a group, 2 more rows, a group, the movie, the rest. 12 rows with the 2D store's tickets 101..112
  // (the tickets run on across frames), node calls after tickets 106 and 108 (seq_after_p1 107 and 109): the groups go before rows 6 and 8; each group = sprites then quads.
  const std::vector<Quad2D> r12 = Rows({101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112});
  const Quad2D* R = r12.data();
  ParticleTables t;
  t.batches = {Batch(0, 107, kParticleSprite), Batch(1, 107, kParticleQuad), Batch(2, 109, kParticleSprite), Batch(3, 109, kParticleQuad)};
  t.particles.resize(4);
  t.screen.push_back(sc);
  Check(fp::HasScreen(t), "screen.has");
  const fp::Stops s = fp::StopsOf(t, R, 12);
  Check(s.n == 2 && s.at[0] == 6 && s.at[1] == 8, "title.row_positions", StopsText(s));
  Check(fp::CountAt(t, 6, R, 12) == 2 && fp::CountAt(t, 8, R, 12) == 2, "title.group_sizes");
  size_t sum = 0;
  for (size_t i = 0; i < s.n; ++i) sum += fp::CountAt(t, s.at[i], R, 12);
  Check(sum == t.batches.size(), "title.every_batch_once", std::to_string(sum));

  // 2. A batch past the frame's rows goes after the last row. S111 TITLESMOKE review: a batch with no row (0) is NOT the flat drawer's: the recorder stamps a row only on a SCREEN-camera call's
  // batches (particle_source.h TakeScreen), so 0 = a scene-camera call of a screen frame (or the tap off): no stop of its own, never drawn flat with the screen camera.
  ParticleTables u = t;
  u.batches.push_back(Batch(4, 0, kParticleSprite));
  u.batches.push_back(Batch(5, 140, kParticleSprite));   // ticket 139: past every row of the frame
  u.particles.resize(6);
  const fp::Stops su = fp::StopsOf(u, R, 12);
  Check(su.n == 3 && su.at[2] == 12 && fp::CountAt(u, 12, R, 12) == 1, "end.past_rows", StopsText(su));
  size_t usum = 0;
  for (size_t i = 0; i < su.n; ++i) usum += fp::CountAt(u, su.at[i], R, 12);
  Check(usum == 5, "mixed.scene_batch_not_flat", std::to_string(usum) + " batches at the stops (5 screen-call batches wanted)");
  ParticleTables only_scene = u;
  only_scene.batches = {Batch(0, 0, kParticleSprite)};
  only_scene.particles.resize(1);
  Check(!fp::HasScreen(only_scene), "mixed.scene_batch_not_flat", "a frame whose only batch is a scene-camera call's has no flat group");

  // 3. A node call before any 2D row of the frame (the store's last ticket is the previous frame's last row, 100 -> seq_after_p1 101) goes before row 0.
  ParticleTables z = t;
  z.batches = {Batch(0, 101, kParticleSprite)};
  z.particles.resize(1);
  const fp::Stops sz = fp::StopsOf(z, R, 12);
  Check(sz.n == 1 && sz.at[0] == 0, "first.before_row_0", StopsText(sz));

  // S111 FLATSEQ: a row the record lost before the node call (ticket 3 refused by Quad2DRowProblem at q2d::TakeFrame: the record holds tickets 1, 2, 4, 5, 6). The first call came after ticket 3
  // (seq_after_p1 4), the second after ticket 5 (6): the groups go before the rows with tickets 4 and 6 (indices 2 and 4). The old count (3 and 5 rows pushed) put them at 3 and 5, one row late.
  const std::vector<Quad2D> r5 = Rows({1, 2, 4, 5, 6});
  ParticleTables hole = t;
  hole.batches = {Batch(0, 4, kParticleSprite), Batch(1, 6, kParticleSprite)};
  hole.particles.resize(2);
  const fp::Stops sh = fp::StopsOf(hole, r5.data(), r5.size());
  Check(sh.n == 2 && sh.at[0] == 2 && sh.at[1] == 4 && fp::CountAt(hole, 2, r5.data(), r5.size()) == 1, "seq.refused_row_before_node", StopsText(sh));
  // A record written before FLATSEQ (ParticleScreen::stamp 0) holds the row COUNT + 1 in the same field: read by the old rule (after 3 and 5 rows).
  ParticleTables old = hole;
  old.screen[0].stamp = 0;
  const fp::Stops so = fp::StopsOf(old, r5.data(), r5.size());
  Check(so.n == 2 && so.at[0] == 3 && so.at[1] == 5, "old_record.count_rule", StopsText(so));

  // S111 FLATSEQ review: the stamp taken from the REAL q2d store, as the node hook takes it (quads2d_tap.h SeqNow = q2d::Store::LastTicket; particle_source.h TakeScreen stamps it + 1), against the
  // rows the real q2d::TakeFrame keeps. Each row takes its ticket as the op-19 does (q2d::Store::Ticket); the third row fails Quad2DRowProblem (a non-finite corner) and is dropped as a hole. Two rows,
  // the refused row, a node call, two rows, a node call, one row: the groups go after the record's rows 0..1 and 0..3 (positions 2 and 4). A LastTicket that answered the NEXT ticket puts each group a row late.
  {
    q2d::Store store;
    q2d::Counters qc;
    const uint32_t j = 7;
    const auto push = [&](bool good) {
      Quad2D q;
      q.frame = j, q.site = q2d::kSiteRect, q.kind = kQkRect, q.pass = kQpFinal, q.blend = kQbOff, q.alpha_func = 7, q.nverts = 4, q.stride_bytes = 12;
      q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest | kQhScissor | kQhTarget;
      q.pos_scale[0] = 2.f / 1280.f, q.pos_scale[1] = 2.f / 720.f, q.color[0] = q.color[3] = 1.f;
      q.viewport[2] = q.target_size[0] = 1280, q.viewport[3] = q.target_size[1] = 720;
      if (!good) q.verts[0][0] = NAN;
      q.seq = store.Ticket();
      store.Push(q, qc);
    };
    push(true), push(true), push(false);
    const uint32_t first = store.LastTicket() + 1u;   // the first node call
    push(true), push(true);
    const uint32_t second = store.LastTicket() + 1u;   // the second node call
    push(true);
    q2d::FrameTake take;
    q2d::TakeFrame(store, j, [](const Quad2D& q) { return Quad2DRowProblem(q).empty(); }, take);
    ParticleTables chain = t;
    chain.batches = {Batch(0, first, kParticleSprite), Batch(1, second, kParticleSprite)};
    chain.particles.resize(2);
    const fp::Stops sc2 = fp::StopsOf(chain, take.rows.data(), take.rows.size());
    Check(take.rows.size() == 5 && take.hole_count == 1 && sc2.n == 2 && sc2.at[0] == 2 && sc2.at[1] == 4, "seq.store_chain",
          StopsText(sc2) + " over " + std::to_string(take.rows.size()) + " rows, " + std::to_string(take.hole_count) + " hole(s)");
  }

  // 4. A match frame (no screen row) is not a flat frame's set; a screen row with no batch draws nothing.
  ParticleTables m = t;
  m.screen.clear();
  Check(!fp::HasScreen(m), "screen.match_not_flat");
  ParticleTables e;
  e.screen.push_back(sc);
  Check(!fp::HasScreen(e), "screen.no_batches");

  std::printf("flat_particles_unit: %d passed, %d failed%s\n", g_pass, g_fail, plant.empty() ? "" : ("  (plant " + plant + ")").c_str());
  return g_fail ? 1 : 0;
}
