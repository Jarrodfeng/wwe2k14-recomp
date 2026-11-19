// S103 V1: unit test of the arena's stage visibility (src/renderer_scene/stage_vis_core.h, src/assets/stage_tree.h). No game, no SDK, no files.
//   clang++ -std=c++20 tools/arena/stage_vis_test.cpp -o out/s103/V1/stage_vis_test.exe     (tools/arena/stage_vis_check.py builds and runs it, then every plant)
// Lines: "PASS name" / "FAIL name: why"; exit 0 = all passed, 1 = a FAIL. `--plant NAME` runs the same checks with one planted fault (it must FAIL its named check).
// The oracle is independent of the matrix code: the camera basis and the tangents build the volume directly, and a box is rejected when ALL EIGHT corners lie outside one plane.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/stage_tree.h"
#include "../../src/renderer_scene/stage_vis_core.h"

namespace stagevis = rdr_scene::stagevis;
static int g_fail = 0;
static std::string g_plant;
static void Check(bool ok, const char* name, const std::string& why = "") {
  if (ok) std::printf("PASS %s\n", name);
  else std::printf("FAIL %s: %s\n", name, why.c_str()), ++g_fail;
}

struct Cam {
  float view[16], proj[16];
  float eye[3], ax[3][3];   // camera axes in world: right, up, forward
  float tanx, tany, n, f;
};

static Cam MakeCam(float yaw, float pitch, const float eye[3], float fov_y_deg, float aspect) {
  Cam c{};
  const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  const float fwd[3] = {sy * cp, sp, cy * cp};
  const float up0[3] = {0, 1, 0};
  float right[3] = {up0[1] * fwd[2] - up0[2] * fwd[1], up0[2] * fwd[0] - up0[0] * fwd[2], up0[0] * fwd[1] - up0[1] * fwd[0]};
  const float rn = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (float& v : right) v /= rn;
  const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
  for (int i = 0; i < 3; ++i) c.eye[i] = eye[i], c.ax[0][i] = right[i], c.ax[1][i] = up[i], c.ax[2][i] = fwd[i];
  std::memset(c.view, 0, sizeof c.view);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) c.view[i * 4 + j] = c.ax[j][i];
  for (int j = 0; j < 3; ++j) c.view[12 + j] = -(eye[0] * c.ax[j][0] + eye[1] * c.ax[j][1] + eye[2] * c.ax[j][2]);
  c.view[15] = 1.f;
  c.n = 10.f, c.f = 300000.f;
  c.tany = std::tan(fov_y_deg * 3.14159265f / 360.f);
  c.tanx = c.tany * aspect;
  std::memset(c.proj, 0, sizeof c.proj);
  c.proj[0] = 1.f / c.tanx, c.proj[5] = -1.f / c.tany, c.proj[10] = c.f / (c.f - c.n), c.proj[11] = 1.f, c.proj[14] = -c.n * c.f / (c.f - c.n);   // (front_end / record_frame: a negative y scale)
  return c;
}

// the oracle: true when the box is NOT wholly outside any plane of the stage volume, with tan_x = max(display, tan(atan(tan_y) x term)) and a slack
static bool OracleVisible(const Cam& c, const float lo[3], const float hi[3], bool flip, float term, float slack) {
  float l[3] = {lo[0], lo[1], lo[2]}, h[3] = {hi[0], hi[1], hi[2]};
  if (flip) l[0] = -hi[0], h[0] = -lo[0];
  float tx = std::fmax(c.tanx, std::tan(std::atan(c.tany) * term)) * slack, ty = c.tany * slack;
  int out[6] = {};
  for (int k = 0; k < 8; ++k) {
    const float q[3] = {(k & 1) ? h[0] : l[0], (k & 2) ? h[1] : l[1], (k & 4) ? h[2] : l[2]};
    float cc[3];
    for (int j = 0; j < 3; ++j) cc[j] = (q[0] - c.eye[0]) * c.ax[j][0] + (q[1] - c.eye[1]) * c.ax[j][1] + (q[2] - c.eye[2]) * c.ax[j][2];
    out[0] += cc[0] < -cc[2] * tx;
    out[1] += cc[0] > cc[2] * tx;
    out[2] += cc[1] < -cc[2] * ty;
    out[3] += cc[1] > cc[2] * ty;
    out[4] += cc[2] < c.n;
    out[5] += cc[2] > c.f;
  }
  for (int k = 0; k < 6; ++k)
    if (out[k] == 8) return false;
  return true;
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) g_plant = argv[i + 1];

  // ---- the tree text and the box in the viewer's space ----
  const char* text =
      "; comment line\r\nNODE MY_NAME = VT_ROOT\r\n{\r\n\t-100 -200 -10 300 400 50\r\n}\r\n2\r\n\r\n"
      "NODE MY_NAME = seatA PARENT_NAME = VT_ROOT\r\n{\r\n\t0 10 5 20 30 40.5\r\n}\r\n0\r\n\r\n"
      "NODE MY_NAME = seatB PARENT_NAME = VT_ROOT\r\n{\r\n\t0 10 5 20 30 40.5\r\n}\r\n0\r\n";
  const auto tree = assets::stage_tree::Parse(text);
  bool ok = tree.size() == 3 && tree.count("seatA") && tree.at("seatA").parent == "VT_ROOT" && tree.at("VT_ROOT").kids == 2 && tree.at("VT_ROOT").parent.empty();
  Check(ok, "parse.tree", "nodes " + std::to_string(tree.size()));
  if (ok) {
    float lo[3], hi[3];
    assets::stage_tree::ToViewer(tree.at("seatA"), lo, hi);
    Check(lo[0] == 0 && lo[1] == 5 && lo[2] == 10 && hi[0] == 20 && hi[1] == 40.5f && hi[2] == 30, "parse.viewer_axes", "(x, z, y) of the tree's (x, y, z)");
  }

  // ---- the join: confirmed by the slot's own meshes ----
  assets::Model m;
  const auto add_slot = [&](int slot, const char* node, const float lo[3], const float hi[3]) {
    assets::Mesh me;
    me.part = "slot " + std::to_string(slot);
    me.node = node;
    assets::Vertex a{}, b{};
    for (int i = 0; i < 3; ++i) a.pos[i] = lo[i], b.pos[i] = hi[i];
    me.vertices = {a, b};
    m.meshes.push_back(me);
  };
  const float boxlo[3] = {0, 5, 10}, boxhi[3] = {20, 40.5f, 30}, other_lo[3] = {500, 5, 10}, other_hi[3] = {520, 40.5f, 30};
  add_slot(5, "seatA", boxlo, boxhi);        // joined
  add_slot(6, "seatB", other_lo, other_hi);  // the name is a tree node but the slot's own meshes are elsewhere (a name collision across models)
  add_slot(7, "seatA", boxlo, boxhi);        // moved by the stage key
  add_slot(8, "nowhere", boxlo, boxhi);      // not a tree node
  add_slot(9, "seatA", boxlo, boxhi);        // mirrored copy
  add_slot(500, "seatA", boxlo, boxhi);      // not a stage slot (>= 400)
  {   // review 1: a corner-placed slot (the fence corners) is drawn at its corner, not at the stored box: never boxed. Review 2: the box is the union of node box and mesh box.
    assets::Placement corner;
    corner.label = g_plant == "placement_ignored" ? "slot 11 mirrored" : "slot 11 corner";
    m.placements.push_back(corner);
    add_slot(11, "seatA", boxlo, boxhi);
    m.meshes.back().placement = int(m.placements.size()) - 1;
    const float wide_hi[3] = {21.5f, 40.5f, 30};   // the slot's meshes reach 1.5 past the node box (inside the 2-unit confirmation)
    add_slot(12, "seatA", boxlo, wide_hi);
  }
  std::set<int> moving = {g_plant == "moving_boxed" ? -1 : 7}, mirrored = {9};
  assets::stage_tree::Fill(m, text, moving, mirrored, g_plant == "collision_joined" ? 1e9f : 2.0f);
  std::set<int> have;
  bool twin9 = false;
  for (const assets::StageBox& b : m.stage_boxes) have.insert(b.slot), twin9 = twin9 || (b.slot == 9 && b.twin);
  Check(have.count(5) && have.count(9) && !have.count(8) && !have.count(500), "fill.join", "slots with a box wrong");
  Check(!have.count(6), "fill.collision_refused", "a tree node whose box is not the slot's own geometry must not be joined");
  Check(!have.count(7), "fill.moving_skipped", "an m(N) slot is placed by the stage key: its node box is local");
  Check(twin9, "fill.twin_flag", "the r flag reaches the box");
  Check(!have.count(11), "fill.placed_never_boxed", "a slot drawn at a placement (a corner) must not be rejected at its stored box");
  {
    bool union_ok = false;
    for (const assets::StageBox& bx : m.stage_boxes)
      if (bx.slot == 12) union_ok = bx.hi[0] >= 21.5f && bx.lo[0] <= 0.f;
    Check(union_ok, "fill.union_box", "the box must cover the slot's own meshes");
  }

  // ---- the volume against the oracle ----
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> U(-1.f, 1.f);
  const float term = g_plant == "term1" ? 1.0f : stagevis::kStageTerm16x9;
  const float slack = g_plant == "slack1" ? 1.0f : stagevis::kSlack;
  int total = 0, mismatches = 0, kept = 0, widened_extra = 0, flip_only = 0;
  for (int it = 0; it < 4000; ++it) {
    const float eye[3] = {U(rng) * 3000, 300 + U(rng) * 200, U(rng) * 3000};
    const float aspect = it % 3 == 0 ? 16.f / 9.f : it % 3 == 1 ? 21.f / 9.f : 4.f / 3.f;
    const Cam c = MakeCam(U(rng) * 3.14159f, U(rng) * 0.4f, eye, 40.f + U(rng) * 8.f, aspect);
    const stagevis::Planes pl = stagevis::StagePlanes(c.view, c.proj, stagevis::kStageTerm16x9, stagevis::kSlack, g_plant == "no_widen");
    const stagevis::Planes pl_game = stagevis::StagePlanes(c.view, c.proj, term, slack, g_plant == "no_widen");
    for (int b = 0; b < 12; ++b) {
      const float cx = U(rng) * 6000, cy = U(rng) * 2500, cz = U(rng) * 6000, ex = 20 + std::fabs(U(rng)) * 900, ey = 10 + std::fabs(U(rng)) * 300, ez = 20 + std::fabs(U(rng)) * 900;
      const float lo[3] = {cx - ex, cy - ey, cz - ez}, hi[3] = {cx + ex, cy + ey, cz + ez};
      const bool flip = (b & 1) != 0 && g_plant != "no_flip" ? true : false;
      const bool mine = stagevis::BoxVisible(pl_game, lo, hi, flip), want = OracleVisible(c, lo, hi, (b & 1) != 0, stagevis::kStageTerm16x9, stagevis::kSlack);
      ++total;
      kept += want;
      if (mine != want) ++mismatches;
      if (want && !OracleVisible(c, lo, hi, (b & 1) != 0, 1.0f, 1.0f)) ++widened_extra;
      if (it % 3 == 0 && (b & 1)) {   // the mirrored copy at 16:9: a box the plain test rejects and the flipped test keeps
        const bool a = OracleVisible(c, lo, hi, false, stagevis::kStageTerm16x9, stagevis::kSlack), f = OracleVisible(c, lo, hi, true, stagevis::kStageTerm16x9, stagevis::kSlack);
        const bool mine_flip = stagevis::BoxVisible(pl, lo, hi, g_plant != "no_flip");
        if (a != f && mine_flip == f) ++flip_only;
        if (a != f && mine_flip != f) ++mismatches;
      }
    }
  }
  Check(mismatches == 0, "frustum.matches_oracle", std::to_string(mismatches) + " of " + std::to_string(total) + " boxes differ from the independent corner oracle");
  Check(kept > total / 10 && kept < total * 9 / 10, "frustum.rejects_something", "kept " + std::to_string(kept) + " of " + std::to_string(total));
  Check(widened_extra > 20, "frustum.widens_beyond_display", "boxes only the stage's wider x term keeps: " + std::to_string(widened_extra));
  Check(flip_only > 20, "mirror.flip", "mirrored-copy boxes decided by the flip: " + std::to_string(flip_only));

  // ---- a wider screen sees more (no 16:9 limit) and the margin ----
  {
    const float eye[3] = {0, 0, 0};
    const Cam w16 = MakeCam(0.f, 0.f, eye, 42.f, 16.f / 9.f), w21 = MakeCam(0.f, 0.f, eye, 42.f, 21.f / 9.f), w32 = MakeCam(0.f, 0.f, eye, 42.f, 32.f / 9.f);
    const float lo[3] = {3000, -100, 2000}, hi[3] = {3200, 100, 2200};   // far to the right at ~56 degrees: outside the 16:9 stage volume, inside the wider ones
    const bool v16 = stagevis::BoxVisible(stagevis::StagePlanes(w16.view, w16.proj), lo, hi), v32 = stagevis::BoxVisible(stagevis::StagePlanes(w32.view, w32.proj), lo, hi);
    Check(!v16 && v32, "frustum.wide_screen_sees_more", std::string("16:9 ") + (v16 ? "keeps" : "rejects") + ", 32:9 " + (v32 ? "keeps" : "rejects"));
    (void)w21;
    // 1 % outside the stage x edge at 16:9 is still kept (the measured nearest miss was 0.4 %)
    const float tx = std::tan(std::atan(w16.tany) * stagevis::kStageTerm16x9);
    const float z = 2000.f, xe = z * tx * 1.01f;
    const float lo2[3] = {xe - 0.5f, -10, z - 0.5f}, hi2[3] = {xe + 0.5f, 10, z + 0.5f};   // (a tiny box whose nearest x edge sits 1 % outside the exact edge... its centre is 1 % out)
    Check(stagevis::BoxVisible(stagevis::StagePlanes(w16.view, w16.proj, stagevis::kStageTerm16x9, g_plant == "slack1" ? 1.0f : stagevis::kSlack), lo2, hi2), "frustum.slack_margin", "the 5 % margin must keep a box 1 % outside");
  }
  std::printf(g_fail ? "stage_vis_test: FAILED (%d)\n" : "stage_vis_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
