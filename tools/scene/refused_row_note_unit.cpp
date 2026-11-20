// S105 A4 piece 3: unit of src\renderer_host\refused_row_note.h (the first-refused-row note: once per mode entry, full-screen opaque rows before it). No game, no SDK, no GPU.
//   refused_row_note_unit.exe [--plant N]      (tools\scene\refused_row_note_check.py builds it with -DRDR_REFUSED_ROW_PLANTS and runs it clean and with every plant)
// entry: wwe2k14::scene_host::refused_row::Gate::Note, Gate::Frame, Line (what SceneHost::DrawRows calls, in its order: Frame(dropped) every frame with rows, then Note(site, why), then Line when Note says yes) and ServeDecision (what SceneHost::ScenePicture calls for each scene-copy row the resolver passes it, with the record's rows)
// Planted twins (each must fail its NAMED check): 1 never re-arms -> gate.rearm; 2 notes every pair every frame -> gate.once; 3 no row is full-screen opaque -> line.opaque_under; 4 serves any pass -> serve.only_final; 5 serves a redundant opaque row -> serve.redundant_first.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../src/renderer_host/refused_row_note.h"

namespace rr = wwe2k14::scene_host::refused_row;
static int g_fail = 0;
#define CHECK(name, c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s (line %d): %s\n", name, __LINE__, #c); } } while (0)

static rdr_scene::Quad2D Row(float x0, float y0, float x1, float y1, uint8_t blend, uint32_t site) {
  rdr_scene::Quad2D r;
  r.verts[0][0] = x0, r.verts[0][1] = y0;
  r.verts[1][0] = x1, r.verts[1][1] = y0;
  r.verts[2][0] = x0, r.verts[2][1] = y1;
  r.verts[3][0] = x1, r.verts[3][1] = y1;
  r.blend = blend, r.site = site, r.pass = rdr_scene::kQpFinal;
  return r;
}

int main(int argc, char** argv) {
  int plant = 0;
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) plant = std::atoi(argv[i + 1]);
#ifdef RDR_REFUSED_ROW_PLANTS
  rr::g_plant = plant;
#endif
  if (plant) std::printf("plant %d\n", plant);

  // Gate: one note at the first dropped frame, none while the rows keep dropping, a new one after 30 quiet frames (the next mode entry).
  {
    rr::Gate g;
    int notes = 0;
    for (int f = 0; f < 200; ++f) g.Frame(true), notes += g.Note(0x827F10C8u, "pass_scene_colour") ? 1 : 0;   // mode 1: 200 frames of the same dropped row
    CHECK("gate.once", notes == 1);
    for (int f = 0; f < 29; ++f) g.Frame(false);
    g.Frame(true);
    notes += g.Note(0x827F10C8u, "pass_scene_colour") ? 1 : 0;   // only 29 quiet frames: still the same mode
    CHECK("gate.not_rearmed_early", notes == 1);
    // F8: a different (site, why) in the same mode IS noted (the picture row behind the first dropped row), once
    int pic = 0;
    for (int f = 0; f < 20; ++f) g.Frame(true), pic += g.Note(0x827FC400u, "picture") ? 1 : 0;
    CHECK("gate.distinct_pair", pic == 1);
    for (int f = 0; f < 30; ++f) g.Frame(false);   // a menu between
    int second = 0;
    for (int f = 0; f < 50; ++f) g.Frame(true), second += g.Note(0x827F10C8u, "pass_scene_colour") ? 1 : 0;
    CHECK("gate.rearm", second == 1);   // the second mode entry notes again, once
  }
  // ServeDecision (review F6): only a Final row; an opaque full-screen row with no Final row before it is redundant; a row after a Final row is served.
  {
    rdr_scene::Quad2D rows[3];
    rows[0] = Row(0, 0, 1280, 720, rdr_scene::kQbOff, 0x827FC400u);   // an opaque full-screen sprite FIRST: nothing drawn before it on F
    rows[0].pass = rdr_scene::kQpFinal, rows[0].seq = 10;
    rows[1] = Row(0, 0, 1280, 720, rdr_scene::kQbAlpha, 0x827F10C8u);  // a blended full-screen sprite
    rows[1].pass = rdr_scene::kQpFinal, rows[1].seq = 20;
    rows[2] = Row(0, 0, 1280, 720, rdr_scene::kQbOff, 0x827FC400u);   // an opaque full-screen sprite AFTER a Final row
    rows[2].pass = rdr_scene::kQpFinal, rows[2].seq = 30;
    CHECK("serve.redundant_first", rr::ServeDecision(rows, 3, rows[0], true) == rr::Serve::Redundant);
    CHECK("serve.after_final", rr::ServeDecision(rows, 3, rows[1], true) == rr::Serve::Yes && rr::ServeDecision(rows, 3, rows[2], true) == rr::Serve::Yes);
    rdr_scene::Quad2D sc = rows[1];
    sc.pass = rdr_scene::kQpSceneColour;
    rdr_scene::Quad2D unk = rows[1];
    unk.pass = rdr_scene::kQpUnknown;
    rdr_scene::Quad2D off = rows[1];
    off.pass = rdr_scene::kQpOffscreen;
    CHECK("serve.only_final", rr::ServeDecision(rows, 3, sc, true) == rr::Serve::NotFinal && rr::ServeDecision(rows, 3, unk, true) == rr::Serve::NotFinal && rr::ServeDecision(rows, 3, off, true) == rr::Serve::NotFinal);
    CHECK("serve.no_picture", rr::ServeDecision(rows, 3, rows[1], false) == rr::Serve::NoPicture);
  }
  // Line: the refused row's fields, and the full-screen opaque row under it.
  {
    rdr_scene::Quad2D rows[3];
    rows[0] = Row(0, 0, 1280, 720, rdr_scene::kQbOff, 0x827F1260u);      // full-screen, opaque
    rows[1] = Row(100, 100, 300, 200, rdr_scene::kQbAlpha, 0x82BC3EE8u);  // a small alpha row
    rows[2] = Row(0, 0, 1280, 720, rdr_scene::kQbAlpha, 0x827FC400u);     // the refused sprite
    rows[2].pass = rdr_scene::kQpSceneColour, rows[2].kind = 2, rows[2].target_size[0] = 1280, rows[2].target_size[1] = 720, rows[2].tex_key[0] = 0x00010000u;
    const std::string s = rr::Line(rows, 3, 2, "picture", 55537);
    CHECK("line.fields", s.find("record 55537") != std::string::npos && s.find("index 2 of 3") != std::string::npos && s.find("why picture") != std::string::npos && s.find("site 0x827FC400") != std::string::npos &&
                             s.find("pass 2") != std::string::npos && s.find("target 1280x720") != std::string::npos && s.find("rect 0,0 1280x720") != std::string::npos && s.find("blend 2") != std::string::npos &&
                             s.find("tex_key 0x00010000") != std::string::npos);
    CHECK("line.opaque_under", s.find("full-screen opaque rows before it: 1 [0 (site 0x827F1260 pass 1)]") != std::string::npos);
    // the other side: with no opaque row before it the note says so (never a false "covered"); a small or alpha row is not "opaque full-screen"
    rdr_scene::Quad2D alone[2] = {rows[1], rows[2]};
    CHECK("line.none_under", rr::Line(alone, 2, 1, "picture", 1).find("no full-screen opaque row before it") != std::string::npos || plant == 3);
    // a row index past the end gives no line (never a read past the table)
    CHECK("line.bounds", rr::Line(rows, 3, 3, "picture", 1).empty() && rr::Line(nullptr, 0, 0, "picture", 1).empty());
  }
  std::printf("refused_row_note_unit%s: %s\n", plant ? " (planted)" : "", g_fail ? "FAIL" : "all checks passed");
  return g_fail ? 1 : 0;
}
