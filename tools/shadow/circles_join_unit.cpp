// Lane 215 unit test: why circle / plane draws stayed the stream's ("late" and "constants" refusals read from the live logs of 2026-11-13). 0 game runs, no GPU.
//   circles_join_unit.exe [--plant NAME]          build: tools\shadow\build_circles_unit.ps1 (second exe); driver: tools\shadow\circles_ours_check.py join
//
//   A. the comparator (circle_shadow_core.h RowsMatch). The victory scene's light matrix is a NaN on some frames: ViewProj c4-c7 = 0x7FC00000 in all 16 words, identical in the record and in the stream
//      draw (checks 7132 / 7133, capture frame 1744 / 1687: `py -I tools\shadow\circles_join_audit.py ZIP`: bits equal, float == false). The shipped rule takes bits-or-float-equal; -0 == 0 still equal;
//      a word that really differs (a moved value, a NaN against a number) is still refused, and the first differing word is named. Plant `float_compare` (the old rule) refuses the NaN pair.
//   B. the keeper (src\renderer_adapter\snapshot_keeper.h, the code live.cpp runs). A frame the game reads back from is bound in two messages: the first part (DrawAndRead: ALL the draws, 927 of
//      them in the captured victory frame) and the rest (SubmitFrame). The hooks join the stream frame j to the stamp j-1, which lives in the record the SECOND part takes (`prev`). The simulation
//      plays presents 100..140 with read-back splits at 110 and 125 and a refused record (never written) at 130, and for every message that holds draws asks the board of the circle hook
//      and of the plane hook whether the stamp j-1 is held: it must be, in every frame (the first part included); and a record handed to a part must be exactly the one the later
//      ForSubmit takes (never a guess: after a refused record the part gets none). Plant `no_part_record` (the old DrawAndRead: no record) fails at 110 and 125.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/snapshot_keeper.h"
#include "../../src/renderer_scene/circle_shadow_core.h"
#include "../../src/renderer_scene/plane_shadow_core.h"

using namespace rdr_scene;

static int g_bad = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-40s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_bad;
}

static float FromBits(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

static void ComparatorChecks(bool float_compare) {
  circles::StreamRows s;
  float rows[circles::kRows * 4] = {};
  for (int i = 0; i < 4; ++i) s.c0[i] = rows[i] = 1.0f + float(i);
  for (int i = 0; i < 16; ++i) s.vp[i] = rows[16 + i] = 0.25f * float(i + 1);
  for (uint32_t i = 0; i < kCirclePoints * 4; ++i) s.pts[i] = rows[32 + i] = float(i) * 0.5f;
  circles::RowDiff d;
  circles::StreamRows ns = s;   // the captured light matrix of the victory scene's last frames: all 16 words NaN (0x7FC00000) on both sides
  float nrows[circles::kRows * 4];
  std::memcpy(nrows, rows, sizeof rows);
  for (int i = 0; i < 16; ++i) ns.vp[i] = nrows[16 + i] = FromBits(0x7FC00000u);
  const bool nan_pair = circles::RowsMatch(nrows, ns, float_compare, &d);
  Check(nan_pair, "comparator.nan_pair_equal", nan_pair ? "bit-equal NaN ViewProj accepted" : "refused: c" + std::to_string(d.reg) + " lane " + std::to_string(d.lane));

  circles::StreamRows z = s;   // -0 against +0: float-equal, accepted as before
  float zr[circles::kRows * 4];
  std::memcpy(zr, rows, sizeof rows);
  z.c0[1] = 0.0f, zr[1] = FromBits(0x80000000u);
  Check(circles::RowsMatch(zr, z, float_compare), "comparator.negative_zero_equal");

  circles::StreamRows m = s;   // a real difference in a point: refused and NAMED (register 9 = the second point row, lane 2)
  m.pts[1 * 4 + 2] += 1.0f;
  circles::RowDiff d2;
  const bool refused = !circles::RowsMatch(rows, m, float_compare, &d2);
  Check(refused && d2.found && d2.reg == 9 && d2.lane == 2, "comparator.real_difference_refused", "named c" + std::to_string(d2.reg) + " lane " + std::to_string(d2.lane));

  circles::StreamRows n = ns;   // a NaN against a number is NOT equal
  n.vp[5] = 7.0f;
  circles::RowDiff d3;
  const bool nan_vs_number = !circles::RowsMatch(nrows, n, float_compare, &d3);
  Check(nan_vs_number && d3.found && d3.reg == 5 && d3.lane == 1, "comparator.nan_vs_number_refused", "named c" + std::to_string(d3.reg) + " lane " + std::to_string(d3.lane));

  circles::StreamRows q = ns;   // two DIFFERENT NaN payloads: float == says unequal and the bits differ: refused (the record must carry the very word)
  float qr[circles::kRows * 4];
  std::memcpy(qr, nrows, sizeof nrows);
  qr[16] = FromBits(0x7FC00001u);
  Check(!circles::RowsMatch(qr, q, float_compare), "comparator.other_nan_payload_refused");
}

// A record of the circle hook's stamp `s` (5 entries) and of the plane hook's (one frame + one blur draw), as the writer cuts them: the record written at present p holds the stamp p.
static std::shared_ptr<const SceneTables> MakeRecord(uint32_t stamp) {
  auto t = std::make_shared<SceneTables>();
  for (uint32_t slot = 0; slot < 5; ++slot) {
    CircleShadow e;
    e.frame = stamp, e.slot = slot, e.character = 0x1000 + slot, e.flags = kCircleHaveViewProj | kCircleHaveParam;
    e.shadow_param[0] = float(stamp);
    t->circle_shadows.push_back(e);
  }
  PlaneFrame pf;
  pf.frame = stamp, pf.have = kPlaneHaveAll;
  t->plane_frames.push_back(pf);
  PlaneDraw pd;
  pd.frame = stamp, pd.kind = kPlaneBlur, pd.ordinal = 0, pd.have = kPlaneDrawHaveBlur;
  t->plane_draws.push_back(pd);
  return t;
}

static void KeeperChecks(bool no_part_record) {
  rdr_adapter::SnapshotKeeper keeper;
  keeper.no_part_record = no_part_record;
  circles::Board circle_board;
  plane::Board plane_board;
  const auto set_record = [&](const std::shared_ptr<const SceneTables>& r) {   // what live.cpp does for each hook: nothing for a null record
    if (!r) return;
    circle_board.Add(r->circle_shadows);
    plane_board.Add(r->plane_frames, r->plane_draws);
  };
  const auto draws_need_stamp = [&](uint32_t frame) {   // the join: the circle entry k and the plane frame / blur draw of stamp frame - 1
    const std::vector<CircleShadow>* c = circle_board.At(frame - 1);
    return c && c->size() == 5 && plane_board.FrameAt(frame - 1) && plane_board.Find(frame - 1, kPlaneBlur, 0, 0, 0);
  };
  const auto expected_gap = [](uint32_t p) { return p == 100 || p == 130 || p == 131; };   // the first present has no previous record; a refused record 130 withholds the prev of frame 130 (the wrong-frame guard) and stamp 130 never exists for frame 131
  int frames_with_draws = 0, missing = 0, parts = 0, parts_missing = 0, wrong_record = 0, none_for_part = 0;
  std::string first_missing;
  uint64_t last_written = 0;
  for (uint32_t p = 100; p <= 140; ++p) {
    const bool split = (p == 110 || p == 125 || p == 135);
    const bool refused = (p == 130);   // the writer's record for this present never reaches the sinks (a refused record)
    if (split) {   // the first part: every draw of the frame; message.record = what DrawAndRead gives it
      ++parts;
      const auto rec = keeper.ForPart(p);
      if (rec) {
        // never a guess: the record must be the one the frame's SubmitFrame takes as `prev` (the stamp p - 1), not an older or newer one
        if (rec->circle_shadows.empty() || rec->circle_shadows[0].frame != p - 1) ++wrong_record;
      } else {
        ++none_for_part;
      }
      set_record(rec);
      ++frames_with_draws;
      if (!draws_need_stamp(p) && !expected_gap(p)) {
        ++missing, ++parts_missing;
        if (first_missing.empty()) first_missing = "part of present " + std::to_string(p) + " (stamp " + std::to_string(p - 1) + " not held)";
      }
    }
    // EndFrame: the writer's record for this present reaches the sinks (a refused record does not)
    if (!refused) {
      rdr_scene::live::FrameInfo fi;
      fi.guest_frame = p;
      keeper.OnRecordShared(fi, MakeRecord(p));
      last_written = p;
    }
    // SubmitFrame: the whole frame (not split) or the rest of a split frame (no draws: the first part took them)
    const auto rec = keeper.ForSubmit(p);
    set_record(rec);
    if (!split) {
      ++frames_with_draws;
      if (!draws_need_stamp(p) && !expected_gap(p)) {
        ++missing;
        if (first_missing.empty()) first_missing = "frame " + std::to_string(p) + " (stamp " + std::to_string(p - 1) + " not held)";
      }
    }
  }
  (void)last_written;
  Check(parts == 3, "keeper.read_back_parts_simulated", std::to_string(parts) + " parts");
  // present 131 follows the refused record 130: its stamp 130 never existed anywhere, so the draws of frame 131 are legitimately without it (the stream's); count only the other frames
  Check(missing == 0, "keeper.every_frame_has_its_stamp",
        std::to_string(frames_with_draws) + " frames with draws, " + std::to_string(missing) + " without stamp j-1 (" + (first_missing.empty() ? "none" : first_missing) + "), " + std::to_string(parts_missing) + " first parts");
  Check(wrong_record == 0, "keeper.part_record_is_the_submit_record", std::to_string(wrong_record) + " parts handed a record that is not stamp j-1");
  Check(none_for_part == 0, "keeper.part_gets_a_record", std::to_string(none_for_part) + " parts left without");

  // after a refused record the part must get NONE (nothing guessed): present 131 split
  rdr_adapter::SnapshotKeeper k2;
  rdr_scene::live::FrameInfo fi;
  fi.guest_frame = 128, k2.OnRecordShared(fi, MakeRecord(128)), k2.ForSubmit(128);
  // present 129's record is refused (never written); 130 is split: cur is 128, last_submitted is 128 -> pairs with frame 129's... but frame 130 needs stamp 129, which does not exist
  const auto stale = k2.ForPart(130);
  Check(stale == nullptr, "keeper.part_after_refused_is_honest", stale ? "handed a record to frame 130 whose previous present (129) was refused: one frame stale" : "none (cur 128 is not the previous present of 130)");
  const auto next_part = k2.ForPart(129);
  Check(next_part && next_part->circle_shadows[0].frame == 128, "keeper.part_next_present_gets_record");
  rdr_adapter::SnapshotKeeper k3;
  Check(k3.ForPart(100) == nullptr, "keeper.part_before_any_record_none");
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  if (plant != "" && plant != "float_compare" && plant != "no_part_record") {
    std::printf("FAIL  unknown plant '%s'\n", plant.c_str());
    return 2;
  }
  ComparatorChecks(plant == "float_compare");
  KeeperChecks(plant == "no_part_record");
  std::printf("%s: %d failed\n", g_bad ? "FAIL" : "PASS", g_bad);
  return g_bad ? 1 : 0;
}
