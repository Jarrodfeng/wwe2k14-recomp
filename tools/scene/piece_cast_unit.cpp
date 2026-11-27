// body-scale: none (a caster-rule unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// S111 SOCKSHADOW (docs\research\s111\SOCKSHADOW.md): a piece the game does not draw through its owner (its SUBM shown word 0: kObjPieceHidden; or detached: kObjDetachedPiece) casts
// nothing from the owner's merged mesh; a piece faded to model alpha 0 (an EYF row) still casts (the caster passes read no model alpha). Real rf::OwnerPieceRows (ApplyWrestlers's pre-pass) and rf::AttachOwnerPieceRows (its attach)
// and LiveMapsHook (CasterOf + MeasureCasters, which asks CasterMeshCasts as RecordShadow / RecordRim / MeasureRimDepth / RecordPlane do), a synthetic record and model; 0 game files,
// 0 game runs (Renderer::Create only, as cs1b_unit.cpp).
//   1. the pre-pass: a hidden piece and a detached piece give their owner an undrawn alpha-0 row; a drawn-by-owner piece with no model-alpha row gives an alpha-0 row that is NOT undrawn.
//   2. the rule (piece_row_rule.h PieceRowCasts): undrawn rows do not cast, the EYF row and a mesh with no row do.
//   3. MeasureCasters over one owner with 4 meshes (body, sock 28, pad 10000, lid 20002): 4 casters with no rows, 2 with the pre-pass's rows.
// Planted twins (--plant NAME; exit 0 = the twin was caught, as cs1b_unit): ignore_game_hide = rf::Plants().ignore_game_hide (the sock has no row: checks 1 / 3 fail);
// ignore_detached = rf::Plants().ignore_detached_flag (the pad has no row); caster_rows = MapsFaults::caster_piece_rows_ignored (the old casters: check 3 fails).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_maps.h"
#include "../../src/renderer_scene/piece_row_rule.h"
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/rf_wrestlers.h"

namespace rdr_scene {
struct LiveMapsTestAccess {
  static bool Measure(LiveMapsHook& h, const SceneFrame& f, uint32_t& n) { float c[3]; float r; return h.MeasureCasters(f, c, r, n); }
};
}  // namespace rdr_scene

using namespace rdr_scene;

namespace {

constexpr int kKeys[4] = {0, 28, 10000, 20002};   // the body, the sock (hidden), the People's Elbow pad (detached), the lid layer (an EYF fade)

// One owner model: 4 skinned yCh_skin meshes of a wrestler entry (src_kind 2), one per key, 3 vertices each (MeasureCasters samples every 8th vertex: 1 sample a mesh).
assets::Model OwnerModel() {
  assets::Model m;
  m.label = "owner with pieces";
  assets::Material mat;
  mat.shader = "yCh_skin";
  mat.char_part = true;   // a character's part (asset_model.h): wrestler_caster::Casts
  m.materials.push_back(mat);
  for (int k = 0; k < 4; ++k) {
    assets::Mesh me;
    me.part = "part" + std::to_string(k);
    me.material = 0;
    me.skinned = true;
    me.src_kind = 2;
    me.src_key = kKeys[k];
    me.vertices.resize(3);
    for (int i = 0; i < 3; ++i) {
      assets::Vertex& v = me.vertices[size_t(i)];
      std::memset(&v, 0, sizeof v);
      v.pos[0] = float(i + 10 * k), v.pos[1] = float(i * 2), v.pos[2] = 0.5f * float(i);
      v.normal[1] = 1.f;
      v.color = 0xFFFFFFFFu;
      v.weights[0] = 1.f;
    }
    me.indices = {0, 1, 2};
    m.meshes.push_back(me);
  }
  return m;
}

Object PieceObject(SceneTables& t, const char* id, uint32_t flags, uint16_t piece) {
  Object o;
  o.id = t.AddString(id);
  o.flags = flags;
  o.pad = piece;
  o.layer = 1;   // owner slot 0 + 1
  return o;
}

// The record: the owner body "actor:slot:0" and its three piece objects as the wrestler contributor writes them (wrestler_contrib.cpp EmitItems).
SceneTables Record() {
  SceneTables t;
  Object body;
  body.id = t.AddString("actor:slot:0");
  body.flags = kObjVisible | kObjSkinned;
  body.bones_count = 1;
  t.objects.push_back(body);
  t.objects.push_back(PieceObject(t, "skel:70001000", kObjPieceHidden, 28));      // SOCKPIECE: invisible, no bones
  t.objects.push_back(PieceObject(t, "skel:70002000", kObjDetachedPiece, 10000));  // DTP
  Object lid = PieceObject(t, "skel:70003000", kObjDrawnByOwner | kObjSkinned, 20002);   // EYF: no model-runtime row in this record = alpha 0, fail closed
  lid.bones_count = 1;
  t.objects.push_back(lid);
  return t;
}

SceneFrame FrameOf(const std::vector<rf::OwnerPieceRow>& rows, bool with_rows) {
  SceneFrame f;
  f.viewer_space = false;
  f.bones.resize(1);
  std::memset(&f.bones[0], 0, sizeof(Mat4));
  f.bones[0].m[0] = f.bones[0].m[5] = f.bones[0].m[10] = f.bones[0].m[15] = 1.f;
  Object o;
  o.asset = 0;
  o.flags = kObjVisible | kObjSkinned;
  std::memset(&o.world, 0, sizeof o.world);
  o.world.m[0] = o.world.m[5] = o.world.m[10] = o.world.m[15] = 1.f;
  o.bones_first = 0, o.bones_count = 1;
  f.objects.push_back(o);
  if (with_rows) rf::AttachOwnerPieceRows(rows, 0u, 0u, f);   // ApplyWrestlers's own attach (rf_wrestlers.h): owner slot 0 onto object 0 (SOCKSHADOW review: no copy of it here, so a row that loses `undrawn` fails check 3)
  return f;
}

const rf::OwnerPieceRow* RowOf(const std::vector<rf::OwnerPieceRow>& rows, unsigned piece) {
  for (const rf::OwnerPieceRow& r : rows)
    if (r.piece == piece) return &r;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string plant = (argc > 2 && !std::strcmp(argv[1], "--plant")) ? argv[2] : "";
  if (!plant.empty() && plant != "ignore_game_hide" && plant != "ignore_detached" && plant != "caster_rows") { std::printf("usage: piece_cast_unit [--plant ignore_game_hide|ignore_detached|caster_rows]\n"); return 2; }
  rf::Plants().ignore_game_hide = plant == "ignore_game_hide";
  rf::Plants().ignore_detached_flag = plant == "ignore_detached";
  int bad = 0;
  auto check = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };

  // 1. the pre-pass
  const SceneTables t = Record();
  std::vector<rf::OwnerPieceRow> rows;
  rf::WrestlerStats st;
  rf::OwnerPieceRows(t, rows, st);
  const rf::OwnerPieceRow* sock = RowOf(rows, 28);
  const rf::OwnerPieceRow* pad = RowOf(rows, 10000);
  const rf::OwnerPieceRow* lid = RowOf(rows, 20002);
  check(sock && sock->slot == 0 && sock->alpha == 0.f && sock->undrawn && st.pieces_game_hidden == 1, "pre-pass: the hidden sock (kObjPieceHidden, 28) gives owner slot 0 an undrawn alpha-0 row (twin ignore_game_hide: no row)");
  check(pad && pad->slot == 0 && pad->alpha == 0.f && pad->undrawn && st.pieces_detached == 1, "pre-pass: the detached pad (kObjDetachedPiece, 10000) gives owner slot 0 an undrawn alpha-0 row (twin ignore_detached: no row)");
  check(lid && lid->slot == 0 && lid->alpha == 0.f && !lid->undrawn && st.pieces_alpha_missing == 1, "pre-pass: a drawn-by-owner lid with no model-alpha row gives an alpha-0 row that is NOT undrawn (an EYF fade)");
  check(rows.size() == 3, "pre-pass: three rows, none for the owner body itself");

  // 2. the rule
  std::vector<SceneFrame::PieceAlpha> pr = {{0u, 28u, 0.f, true}, {0u, 20002u, 0.f, false}};
  check(!PieceRowCasts(pr, 0u, 28) && PieceRowCasts(pr, 0u, 20002) && PieceRowCasts(pr, 0u, 0) && PieceRowCasts(pr, 1u, 28) && PieceRowCasts(pr, 0u, -1),
        "PieceRowCasts: an undrawn row casts nothing; an EYF alpha-0 row, the body (key 0), another object's mesh and a mesh of no key cast");
  check(FindPieceRow(pr, 0u, 28) == &pr[0] && FindPieceRow(pr, 0u, -1) == nullptr && PieceRowKey(true, 28) == 28 && PieceRowKey(false, 28) == -1, "FindPieceRow / PieceRowKey: the first row of (object, key); no key = no row");

  // 3. the casters
  std::string err;
  rdr::RendererOptions ro;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); return 1; }
  {
    const assets::Model model = OwnerModel();
    LiveMapsHook hook(*renderer, [&](uint32_t a) -> const assets::Model* { return a == 0 ? &model : nullptr; });
    MapsFaults faults;
    faults.caster_piece_rows_ignored = plant == "caster_rows";
    hook.SetFaults(faults);
    uint32_t n_none = 0, n_rows = 0;
    const bool ok_none = LiveMapsTestAccess::Measure(hook, FrameOf(rows, false), n_none);
    const bool ok_rows = LiveMapsTestAccess::Measure(hook, FrameOf(rows, true), n_rows);
    std::printf("      casters' samples: %u with no rows, %u with the pre-pass's rows\n", n_none, n_rows);
    check(ok_none && n_none == 4, "MeasureCasters with no rows: all 4 meshes cast (body, sock, pad, lid)");
    check(ok_rows && n_rows == 2, "MeasureCasters with the rows: the body and the faded lid cast, the hidden sock and the detached pad do not (twin caster_rows: 4)");
    renderer->WaitIdle();
  }
  std::printf("%s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
  if (!plant.empty()) return bad ? 0 : 1;   // a planted twin must be caught: exit 0 only when a check failed
  return bad ? 1 : 0;
}
