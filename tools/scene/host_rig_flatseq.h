// S111 FLATSEQ (docs\research\s111\FLATSEQ.md): host_rig --mode flatseq [--plant flat_parts_first|old_count|end]: a flat frame's particle groups through the REAL SceneHost::DrawRows (the
// TITLESMOKE review's proof gap: tools\scene\flat_particles_unit.cpp drives flat_particles.h alone, so a DrawRows that drew a group before the rows it follows, or placed it by another rule, passed it).
// One synthetic flat record (no stage, no object, no Scene3D stamp: SceneHost::Handle takes it as a flat frame, black + its rows) is posted on a headless offscreen renderer: five plain T_Rect rows
// with the 2D store's tickets 1, 2, 4, 5, 6 (ticket 3 was a row the record lost: refused by Quad2DRowProblem at q2d::TakeFrame, a hole) and two screen-camera node calls, the first made after
// ticket 3 (3 rows pushed then: the old count 3), the second after ticket 5 (count 5). The host's steps (TestSeam::flat_trace) must be rows [0, 2), the group at 2, rows [2, 4), the group at 4,
// rows [4, 5). Planted twins (each must exit 1 with FAIL flatseq.order): flat_parts_first = TestSeam::flat_parts_first (a group before the rows it follows are flushed); old_count =
// flat_particles g_plant 4 (the stamp read as the row count: the first group one row late, after ticket 4); end = g_plant 1 (every group after the last row).
#pragma once
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_host/scene_host.h"
#include "../../src/renderer_scene/flat_particles.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_quad2d.h"

namespace host_rig_flatseq {

namespace sh = ::wwe2k14::scene_host;

// A plain pixel-placed T_Rect row of the final frame that passes Quad2DRowProblem (an 80 x 80 box at x0).
inline rdr_scene::Quad2D Row(uint32_t seq, uint32_t frame, float x0) {
  rdr_scene::Quad2D q;
  q.frame = frame, q.seq = seq, q.site = 0x82BC3EE8u, q.kind = rdr_scene::kQkRect, q.pass = rdr_scene::kQpFinal, q.blend = rdr_scene::kQbOff, q.alpha_func = 7;
  q.nverts = 4, q.stride_bytes = 12;
  q.have = rdr_scene::kQhVerts | rdr_scene::kQhPosScale | rdr_scene::kQhColour | rdr_scene::kQhProj | rdr_scene::kQhBlend | rdr_scene::kQhAlphaTest | rdr_scene::kQhScissor | rdr_scene::kQhTarget;
  const float xy[4][2] = {{x0, 100.f}, {x0 + 80.f, 100.f}, {x0, 180.f}, {x0 + 80.f, 180.f}};
  for (int v = 0; v < 4; ++v) q.verts[v][0] = xy[v][0], q.verts[v][1] = xy[v][1];
  q.pos_scale[0] = 2.f / 1280.f, q.pos_scale[1] = 2.f / 720.f;
  q.color[0] = 1.f, q.color[3] = 1.f;
  q.viewport[2] = 1280, q.viewport[3] = 720;
  q.target_size[0] = 1280, q.target_size[1] = 720;
  return q;
}

inline rdr_scene::ParticleBatch Batch(uint32_t first, uint32_t seq_after_p1, uint32_t frame) {
  rdr_scene::ParticleBatch b;
  b.first = first, b.count = 1, b.seq_after_p1 = seq_after_p1, b.frame = frame;
  return b;
}

inline int Run(const std::string& plant) {
  if (plant == "old_count") rdr_scene::flat_particles::g_plant = 4;
  else if (plant == "end") rdr_scene::flat_particles::g_plant = 1;
  else if (!plant.empty() && plant != "flat_parts_first") {
    std::fprintf(stderr, "host_rig: unknown flatseq plant %s\n", plant.c_str());
    return 2;
  }
  constexpr uint32_t kW = 1280, kH = 720;
  constexpr uint32_t kFrame = 100;
  auto rec = std::make_shared<rdr_scene::SceneTables>();
  rec->header.guest_frame = kFrame;
  rec->quad2d_holes = 1;   // the lost row (ticket 3)
  const uint32_t seqs[5] = {1, 2, 4, 5, 6};
  for (int i = 0; i < 5; ++i) rec->quads2d.push_back(Row(seqs[i], kFrame, 100.f + 120.f * float(i)));
  rdr_scene::ParticleScreen sc;
  sc.view_proj[0] = 2.f / 1280.f, sc.view_proj[5] = -2.f / 720.f, sc.view_proj[10] = 0.0033f, sc.view_proj[15] = 1.f;
  sc.calls = 2, sc.row_count = 5, sc.stamp = rdr_scene::kParticleStampSeq;
  rec->particles.screen.push_back(sc);
  rec->particles.batches = {Batch(0, 3 + 1, kFrame), Batch(1, 5 + 1, kFrame)};
  rec->particles.particles.resize(2);

  sh::HostOptions ho;
  ho.renderer.present_offscreen_w = kW, ho.renderer.present_offscreen_h = kH;
  ho.renderer.vsync = false;
  sh::SceneHost host;
  std::string err;
  if (!host.Start(ho, err)) {
    std::fprintf(stderr, "host_rig: FAIL host: %s\n", err.c_str());
    return 3;
  }
  std::mutex mu;
  std::string trace;
  sh::TestSeam seam;
  seam.flat_parts_first = plant == "flat_parts_first";
  seam.flat_trace = [&mu, &trace](char step, size_t a, size_t b) {
    std::lock_guard<std::mutex> lock(mu);
    char buf[64];
    if (step == 'R') std::snprintf(buf, sizeof buf, "%sR%zu-%zu", trace.empty() ? "" : " ", a, b);
    else std::snprintf(buf, sizeof buf, "%sP%zu/%zu", trace.empty() ? "" : " ", a, b);
    trace += buf;
  };
  host.SetTestSeam(seam);
  std::string got;
  uint64_t posted = 0;
  for (int tries = 0; tries < 100 && got.empty(); ++tries) {   // the host may pace a tick out (30 fps) or build first: post again until DrawRows reports its steps
    {
      std::lock_guard<std::mutex> lock(mu);
      trace.clear();
    }
    sh::Msg m;
    m.guest_frame = kFrame;
    m.width = kW, m.height = kH;
    m.record = rec;
    host.Post(std::move(m));
    ++posted;
    if (!host.WaitHandled(posted, std::chrono::milliseconds(60000))) {
      std::fprintf(stderr, "host_rig: FAIL the host did not handle post %llu\n", (unsigned long long)posted);
      host.Stop();
      return 3;
    }
    {
      std::lock_guard<std::mutex> lock(mu);
      got = trace;
    }
    if (got.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const int source = int(host.LastSource());
  host.Stop();
  const std::string want = "R0-2 P2/1 R2-4 P4/1 R4-5";
  std::printf("flatseq steps: %s (want %s), posts %llu, last source %d\n", got.empty() ? "none" : got.c_str(), want.c_str(), (unsigned long long)posted, source);
  if (got.empty()) {
    std::printf("flatseq: DrawRows never reported a step (the flat record was not drawn): not a verdict\n");
    return 3;
  }
  if (got != want) {
    std::printf("FAIL flatseq.order: the host drew the rows and the particle groups as %s\n", got.c_str());
    return 1;
  }
  std::printf("flatseq result: PASS\n");
  return 0;
}

}  // namespace host_rig_flatseq
