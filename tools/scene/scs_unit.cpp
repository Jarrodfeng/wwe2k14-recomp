// scs_unit: lane SCS's offline proof (Session 103 overnight, docs\research\s103\RFF.md part B; 0 game runs, no GPU). The in-scene 2D rows (the screen flashes and the lens flares, Quad2D::pass == kQpSceneColour)
// are drawn by OUR scene hook inside the scene pass, not dropped:
//   [SCS HOOK]   LiveScene2D (frame_hooks.h) keeps exactly the scene-colour rows of the record, in seq order, and records them at Stage::AfterBlended only; a record whose 2D tap did not run gives it nothing
//   [SCS DRAW]   a planted scene-colour flash row (T_Rect, additive, z off) is drawn as one additive draw with no depth test, no depth write
//   [SCS DEPTH]  a lens flare row (T_Sprite, matrix-placed, additive, z test on, z write off) takes the game's depth state: test on, write off, our forward LEQUAL, the row's clip z kept as it is (system z scale 1,
//                offset 0: S110 STARFRONT, the game's reversal is its viewport's, so the lens element's z 0.996 stays far);
//                Append WITHOUT Context::scene_depth (the final-image path) keeps every depth test off (the F rows are unchanged)
// Planted twin (RDR_Q2D_PLANTS, --plant scene_z_ignored): the depth state / flip is not taken -> [SCS DEPTH] must fail. The old FateOf drop is the text lint scs_lint.py's twin (scene_host.cpp is not unit-testable here).
// S105 A4B adds [SCS EARLY] (the pre-3D split at BeforeOpaque / AfterBlended), [WALK] (the main walk's begin / end and the kQhPre3DCat mark), [SCS FLAT] (the flat-frame scene rows on F), each with a planted twin.
// entry: rdr_scene::LiveScene2D::Record (the stage hook the live frame calls), rdr_scene::walkcat::Walk::Begin / End / Mark (what the sub_82C3B200 / sub_82C3B218 hooks and the op-19 recorder call), rdr_scene::q2d::Append (the in-scene lambda's draw), wwe2k14::scene_host::FateOf (DrawRows' per-row decision). The helpers Flash / Lens / Final / SetRows only build the planted rows the entries are fed: production rows come from the tap (src\caw_text.h is a different Flash).
// Planted twin scene_z_flipped (S110 STARFRONT, plant 74): the old flip z' = w - z of a z-tested row -> [SCS DEPTH] must fail.
// Usage: out\tools\scs_unit.exe [--plant scene_z_ignored | scene_z_flipped | scene_dropped_on_flat | walk_bit_never_set | walk_end_ignored]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"
#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_quad2d.h"
#include "../../src/renderer_scene/frame_hooks.h"
#include "../../src/renderer_scene/walk_cat.h"   // S105 A4B
#include "../../src/renderer_host/row_fate.h"   // S105 A4B: FateOf (the flat-frame rule)

using namespace rdr_scene;

static std::string g_failed;
static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* tag, const std::string& what) {
  ++g_checks;
  std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", tag, what.c_str());
  if (!ok) {
    ++g_failures;
    if (g_failed.find(tag) == std::string::npos) g_failed += std::string(g_failed.empty() ? "" : ", ") + "[" + tag + "]";
  }
}

static const float kScaleX = 2.0f / 1280.0f, kScaleY = 2.0f / 720.0f;
static const uint32_t kKey = 0x00060009u;

// The screen flash (RFF B2): T_Rect, site 0x82BC3EE8, additive (code 1), z off, drawn in the scene target.
static Quad2D Flash(uint32_t seq) {
  Quad2D q;
  q.kind = kQkRect, q.site = 0x82BC3EE8u, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 12, q.prim = 6;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhState;
  q.blend = kQbAdd;
  q.state = uint8_t(1 | 64);   // code 1 (additive), blend enable, z off, z write off
  const float v[4][5] = {{0, 0, 0, 0, 0}, {1280, 0, 0, 0, 0}, {0, 720, 0, 0, 0}, {1280, 720, 0, 0, 0}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY, q.pos_scale[2] = NAN, q.pos_scale[3] = NAN;
  q.color[0] = 1.f, q.color[1] = 1.f, q.color[2] = 0.784f, q.color[3] = 0.424f;
  ValidSC(q);
  q.pass = kQpSceneColour;
  return q;
}

// A lens flare element (RFF B2): T_Sprite, site 0x82BC5688, additive, MATRIX-placed, z test on, z write off.
static Quad2D Lens(uint32_t seq) {
  Quad2D q;
  q.kind = kQkSprite, q.site = 0x82BC5688u, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 20, q.prim = q2d::kGlyphPrimStrip;
  q.have = kQhVerts | kQhColour | kQhProj | kQhMatrix | kQhBlend | kQhState | kQhPlanes;
  q.use_proj = 1.0f;
  q.blend = kQbAdd;
  q.state = uint8_t(1 | 16 | 64);   // code 1 (additive), blend enable, z TEST on, z write off
  const float v[4][5] = {{-1, -1, 0.5f, 0, 0}, {1, -1, 0.5f, 1, 0}, {-1, 1, 0.5f, 0, 1}, {1, 1, 0.5f, 1, 1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  for (int k = 0; k < 4; ++k) q.proj[4 * k + k] = 1.0f;
  q.color[0] = q.color[1] = q.color[2] = q.color[3] = 1.f;
  q.tex_key[0] = kKey;
  ValidSC(q);
  q.pass = kQpSceneColour;
  return q;
}

static Quad2D Final(uint32_t seq) {
  Quad2D q = Flash(seq);
  q.pass = kQpFinal;
  return q;
}

static void HookChecks() {
  SceneTables t;
  t.header.guest_frame = 77;
  t.quad2d_holes = 0;
  Quad2D off = Flash(4);
  off.pass = kQpOffscreen;
  t.quads2d = {Final(1), Flash(2), off, Lens(3), Final(5)};
  int calls = 0;
  uint64_t seen_frame = 0;
  std::vector<uint32_t> seqs;
  std::vector<uint8_t> passes;
  uint8_t last_phase = 99;
  LiveScene2D hook([&](rdr::FrameBuilder&, const StageCtx&, const Quad2D* rows, size_t n, uint64_t gf, uint8_t phase) {
    ++calls, seen_frame = gf, last_phase = phase;
    for (size_t i = 0; i < n; ++i) seqs.push_back(rows[i].seq), passes.push_back(rows[i].pass);
  });
  hook.SetRows(t);
  rdr::FrameBuilder fb;
  StageCtx ctx;
  hook.Record(Stage::Maps, ctx, fb);
  hook.Record(Stage::BeforeOpaque, ctx, fb);
  hook.Record(Stage::BeforePost, ctx, fb);
  Check(calls == 0, "SCS HOOK", "the hook records nothing at any stage but AfterBlended");
  hook.Record(Stage::AfterBlended, ctx, fb);
  Check(calls == 1 && last_phase == LiveScene2D::kPhaseOnly && seen_frame == 77 && seqs.size() == 2 && seqs[0] == 2 && seqs[1] == 3 && passes[0] == kQpSceneColour && passes[1] == kQpSceneColour, "SCS HOOK",
        "one call at AfterBlended with exactly the scene-colour rows (the flash, the lens) in seq order, the record's guest frame; the final / offscreen rows are not its");
  SceneTables nt = t;
  nt.quad2d_holes = kQuad2DNotTaken;
  hook.SetRows(nt);
  calls = 0;
  hook.Record(Stage::AfterBlended, ctx, fb);
  Check(calls == 0 && hook.rows() == 0, "SCS HOOK", "a record whose 2D tap did not run gives the hook no rows");
  {   // fix 3: only the two expected sites; another scene-colour site is not the hook's (the host keeps DropPass for it)
    Quad2D other = Flash(6);
    other.site = 0x82BC4444u;
    SceneTables ot = t;
    ot.quads2d = {Flash(1), other, Lens(2)};
    hook.SetRows(ot);
    Check(hook.rows() == 2 && Scene2DSite(0x82BC3EE8u) && Scene2DSite(0x82BC5688u) && !Scene2DSite(0x82BC4444u), "SCS HOOK", "only the rect site 0x82BC3EE8 and the sprite site 0x82BC5688 are taken; a scene-colour row of another site is not");
  }
  {   // S105 A4B: a row the tap marked kQhPre3DCat (CAF's category-3 black quad) is drawn at BeforeOpaque, BEFORE the 3D; the late rows still at AfterBlended; the report continues across the two calls
    Quad2D early = Flash(1);
    early.have |= kQhPre3DCat;
    SceneTables et;
    et.header.guest_frame = 88;
    et.quad2d_holes = 0;
    et.quads2d = {early, Flash(2), Lens(3)};
    std::vector<std::pair<Stage, size_t>> order;   // (the stage it was drawn at, rows)
    std::vector<uint8_t> phases;
    std::vector<uint32_t> eseq;
    Stage cur = Stage::Maps;
    LiveScene2D eh([&](rdr::FrameBuilder&, const StageCtx&, const Quad2D* rows, size_t n, uint64_t, uint8_t phase) {
      order.push_back({cur, n}), phases.push_back(phase);
      for (size_t i = 0; i < n; ++i) eseq.push_back(rows[i].seq);
    });
    eh.SetRows(et);
    rdr::FrameBuilder efb;
    StageCtx ectx;
    for (Stage st : {Stage::Maps, Stage::BeforeOpaque, Stage::AfterBlended, Stage::BeforePost}) cur = st, eh.Record(st, ectx, efb);
    Check(order.size() == 2 && order[0].first == Stage::BeforeOpaque && order[0].second == 1 && order[1].first == Stage::AfterBlended && order[1].second == 2 && eseq.size() == 3 && eseq[0] == 1 && eseq[1] == 2 && eseq[2] == 3, "SCS EARLY",
          "a category-3 row (kQhPre3DCat) is drawn at BeforeOpaque, before the 3D; the category-13+ rows (flash, lens) after it at AfterBlended, each group in seq order");
    Check(phases.size() == 2 && phases[0] == LiveScene2D::kPhaseEarly && phases[1] == LiveScene2D::kPhaseLate && eh.rows() == 3 && eh.early_rows() == 1, "SCS EARLY", "the two calls say kPhaseEarly then kPhaseLate (the owner's report goes on); rows() counts both, early_rows() the early one");
    SceneTables ot = et;   // a frame with only early rows: one call, no late
    ot.quads2d = {early};
    eh.SetRows(ot);
    order.clear(), phases.clear();
    for (Stage st : {Stage::BeforeOpaque, Stage::AfterBlended}) cur = st, eh.Record(st, ectx, efb);
    Check(order.size() == 1 && order[0].first == Stage::BeforeOpaque && phases[0] == LiveScene2D::kPhaseEarly, "SCS EARLY", "a record with only an early row draws once, at BeforeOpaque");
  }
}

// S105 A4B (entry: walkcat::Walk::Begin / End, walkcat::Mark: the hooks sub_82C3B200 / sub_82C3B218 and the op-19 recorder call exactly these, in this order): the pre-3D mark follows the MAIN walk's category.
static void WalkChecks() {
  using namespace walkcat;
  Walk w;
  Quad2D rect = Flash(1);
  w.Begin(3);   // sub_82C3B200 (cat 3: CAF's black quad)
  Quad2D a = rect;
  Mark(a, w);
  Check((a.have & kQhPre3DCat) != 0, "WALK", "begin(3) then a scene-colour row: the row gets kQhPre3DCat");
  w.End();   // sub_82C3B218
  Quad2D b = rect;
  Mark(b, w);
  Check((b.have & kQhPre3DCat) == 0, "WALK", "after end the category is reset: a later scene-colour row gets no bit");
  w.Begin(36);   // the flashes and lens flares
  Quad2D c = rect;
  Mark(c, w);
  Check((c.have & kQhPre3DCat) == 0, "WALK", "begin(36) (after the wrestlers): no bit");
  w.End();
  w.Begin(13);
  Quad2D d = rect;
  Mark(d, w);
  Check((d.have & kQhPre3DCat) == 0, "WALK", "begin(13), the first wrestler category: no bit");
  w.End();
  w.Begin(2);
  Quad2D f = Final(1);
  Mark(f, w);
  Check((f.have & kQhPre3DCat) == 0, "WALK", "a Final-pass row is never marked, whatever the category");
  w.End();
  w.Begin(36);   // a node's own sub-walk (sub_82C2F960, e.g. the rim map's category 3) is NOT hooked: with the main category 36 open it stays unmarked (a hook on the sub-walker would have set the bit)
  Quad2D g = rect;
  Mark(g, w);
  Check((g.have & kQhPre3DCat) == 0, "WALK", "a sub-walk inside category 36 gives no bit (only the main walk's begin / end set the category)");
  w.End();
}

static void DepthChecks() {
  using namespace q2d;
  Programs programs;
  programs.rect = 11, programs.movie = 22, programs.layout = 3, programs.sprite = 33, programs.sprite_hsl = 44, programs.glyph = 55;
  programs.rect_proj = 66, programs.sprite_proj = 77, programs.glyph_proj = 88;
  const auto make_ctx = [](bool scene) {
    Context ctx;
    ctx.width = 1920, ctx.height = 1080;
    ctx.scene_depth = scene;
    ctx.picture = [](const Quad2D& row) {
      rdr::TextureSlot s;
      s.image = row.tex_key[0] == kKey ? rdr::ImageId(900) : rdr::kNoImage;
      return s;
    };
    return ctx;
  };
  std::vector<Quad2D> rows = {Flash(1), Lens(2)};
  {   // in the scene pass
    rdr::FrameBuilder fb;
    fb.Begin(1);
    const Report rep = Append(fb, programs, make_ctx(true), rows.data(), rows.size());
    const rdr::Frame frame = fb.Finish();
    Check(rep.drawn == 2 && rep.refused_total() == 0 && frame.draws.count == 2, "SCS DRAW", "the planted flash (T_Rect additive) and the lens (T_Sprite, matrix-placed) are both drawn, none refused");
    if (frame.draws.count == 2) {
      const rdr::DrawState& f = frame.states[frame.draws[0].state];
      const rdr::DrawState& l = frame.states[frame.draws[1].state];
      Check(f.program == 11 && f.blend[0].enable == 1 && f.blend[0].dst_color == 1 && f.blend[0].src_color == 6 && f.depth_test == 0 && f.depth_write == 0, "SCS DRAW",
            "the flash: the rect program, ADDITIVE blend (SRC_ALPHA, ONE), no depth test, no depth write");
      const rdr::DynamicState& fd = frame.dynamics[frame.draws[0].dynamic];
      Check(fd.system[2] == 1.0f && fd.system[6] == 0.0f, "SCS DEPTH", "a z-off row keeps the plain clip z (scale 1, offset 0)");
      const rdr::DynamicState& ld = frame.dynamics[frame.draws[1].dynamic];
      Check(l.program == 77 && l.depth_test == 1 && l.depth_write == 0 && l.depth_compare == 3 && l.blend[0].dst_color == 1, "SCS DEPTH",
            "the lens: the matrix sprite program, additive, depth test ON, depth write OFF, compare LESS_OR_EQUAL (the game's z enable 16 / z write 32 from the row's state)");
      Check(f.blend[0].write_mask == 0x7 && l.blend[0].write_mask == 0x7, "SCS DEPTH", "fix 4: scene rows write rgb only (mask 0x7): the post reads the scene's alpha");
      Check(ld.system[2] == 1.0f && ld.system[6] == 0.0f, "SCS DEPTH", "a z-tested row keeps its clip z (scale 1, offset 0): the game reverses its depth in the viewport, so the lens element's z 0.996 is far in both (S110 STARFRONT)");
      {   // S110 STARFRONT: the number. The lens element's z 0.996094 (capture c2d_entrance_s15 f60 draw 981) through the row's ndc z scale / offset lands at our forward depth 0.996 (far): a body at forward 0.9 hides it
        const float z_game = 0.996094f, ours = z_game * ld.system[2] + ld.system[6];
        Check(std::fabs(ours - z_game) < 1e-6f && 0.9f <= ours, "SCS DEPTH", "the lens element's depth in ours is " + std::to_string(ours) + ": behind a body at forward depth 0.9 (LESS_OR_EQUAL fails), as the game's far 0.0039 behind its reversed 0.1");
      }
      {   // S110 STARFRONT review: the depth DECISION, game vs ours, per scene depth (not only the mapped number). Game: clip z / w through its viewport depth range min 1, max 0 (window = 1 - ndc: capture
          // c2d_entrance_s15 f60, every draw), GEQUAL (6) against its reversed scene depth (1 - the scene's ndc, cleared to 0). Ours: the row program's z' = z * system[2] + system[6] * w (rdr_uiquad*.vert),
          // / w, through OUR viewport depth range (viewport[4] .. [5]), the row's own test / compare against our forward scene depth (the scene's ndc, cleared to 1). Both scenes' ndc from the D3D projection
          // record_frame.cpp:392 builds (n 10, f 300000; the game's c56-c59 factor to the same near / far, record_frame.cpp:385). Expected: hidden where anything nearer than the row is in front, drawn elsewhere.
        const float n = 10.f, f = 300000.f;
        const auto ndc_at = [&](float view_z) { return f / (f - n) - n * f / ((f - n) * view_z); };
        const auto cmp = [](int op, float a, float b) { return op == 1 ? a < b : op == 3 ? a <= b : op == 4 ? a > b : op == 6 ? a >= b : op == 7; };
        // the row's clip z / w: the element outside arena 33 (sub_827D5840's 16777215 = 2^32 / 256: clip z 1 - 1/256), pixel-placed (w 1) and through a matrix (w 2560); arena 33's (0.9995 x the light's ndc, light at 3000)
        struct RowZ { const char* what; float z, w; } rowz[] = {{"the element (pixel-placed)", 0.996094f, 1.f}, {"the element (matrix-placed, w 2560)", 0.996094f * 2560.f, 2560.f},
                                                                {"the arena-33 element (light at 3000)", 0.9995f * ndc_at(3000.f), 1.f}};
        struct Scene { const char* what; float view_z; bool cleared; } scenes[] = {{"a body at 500 units", 500.f, false}, {"the set at 2000", 2000.f, false}, {"the set at 10000", 10000.f, false}, {"the cleared background", 0.f, true}};
        for (const RowZ& r : rowz) {
          const float row_ndc = r.z / r.w, row_view = n * f / ((f - n) * (f / (f - n) - row_ndc));   // the row's depth as a view distance (ndc_at inverted)
          for (const Scene& s : scenes) {
            const bool game = cmp(6, 1.f - row_ndc, s.cleared ? 0.f : 1.f - ndc_at(s.view_z));
            const float ours_ndc = (r.z * ld.system[2] + ld.system[6] * r.w) / r.w, ours_win = ld.viewport[4] + ours_ndc * (ld.viewport[5] - ld.viewport[4]);
            const bool ours = !l.depth_test || cmp(l.depth_compare, ours_win, s.cleared ? 1.f : ndc_at(s.view_z));
            const bool expect = s.cleared || s.view_z > row_view;
            Check(game == expect && ours == expect, "SCS DEPTH", std::string(r.what) + " (" + std::to_string(int(row_view)) + " units) against " + s.what + ": game " + (game ? "drawn" : "hidden") +
                                                                 ", ours " + (ours ? "drawn" : "hidden") + ", expected " + (expect ? "drawn" : "hidden"));
          }
        }
      }
    }
  }
  {   // the final-image path: unchanged
    rdr::FrameBuilder fb;
    fb.Begin(1);
    const Report rep = Append(fb, programs, make_ctx(false), rows.data(), rows.size());
    const rdr::Frame frame = fb.Finish();
    bool ok = rep.drawn == 2 && frame.draws.count == 2;
    for (size_t i = 0; ok && i < frame.draws.count; ++i) {
      const rdr::DrawState& s = frame.states[frame.draws[i].state];
      const rdr::DynamicState& d = frame.dynamics[frame.draws[i].dynamic];
      ok = s.depth_test == 0 && s.depth_write == 0 && d.system[2] == 1.0f && d.system[6] == 0.0f;
    }
    Check(ok, "SCS DRAW", "without Context::scene_depth (rows on the final image F) every row keeps depth off and the plain clip z");
  }
}

// S105 A4B (entry: wwe2k14::scene_host::FateOf, what DrawRows asks per row with scene_on_f = SceneRowsOnF(plan.reason)): a flat frame draws its scene-colour rows on F, in seq order; any other frame keeps them InScene / dropped.
static void FlatChecks() {
  namespace sh = wwe2k14::scene_host;
  const Quad2D a = Lens(1), b = Flash(2), fin = Final(3);
  Quad2D other = Flash(4);
  other.site = 0x82BC4444u;   // another scene-colour site: never the hook's
  Check(sh::FateOf(a, false, 1280, 720, true, false, true) == sh::Fate::Draw && sh::FateOf(b, false, 1280, 720, true, false, true) == sh::Fate::Draw, "SCS FLAT", "scene_on_f (a flat frame): the two scene-colour rows (belt sprite, rect) are drawn on F, even with the scene hook on");
  Check(sh::FateOf(fin, false, 1280, 720, true, false, true) == sh::Fate::Draw, "SCS FLAT", "a Final row is drawn as always");
  Check(sh::FateOf(a, false, 1280, 720, true, false, false) == sh::Fate::InScene && sh::FateOf(b, false, 1280, 720, true, false, false) == sh::Fate::InScene, "SCS FLAT", "not a flat frame (Hold3D, a drawn 3D frame): the scene-colour rows stay the scene hook's (InScene), never drawn on F");
  Check(sh::FateOf(a, false, 1280, 720, false, false, false) == sh::Fate::DropPass, "SCS FLAT", "no scene hook and not flat: the old `pass` drop stands");
  Check(sh::FateOf(other, false, 1280, 720, true, false, true) == sh::Fate::DropPass, "SCS FLAT", "a scene-colour row of another site is never drawn on F (a third thing in the scene pass shows up as a drop)");
  const Quad2D rows[] = {Final(1), Lens(2), Flash(3), other, Final(5)};   // seq order: the Draw rows of one record form runs in record order (DrawRows flushes a run at the first non-Draw row)
  std::string fates;
  for (const Quad2D& r : rows) fates += sh::FateOf(r, false, 1280, 720, true, false, true) == sh::Fate::Draw ? 'D' : 'x';
  Check(fates == "DDDxD", "SCS FLAT", "a flat record's rows: Final, belt, rect drawn in one run in seq order, the unknown scene site dropped, the last Final row drawn: " + fates);
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
#ifdef RDR_Q2D_PLANTS
  if (plant == "scene_z_ignored") q2d::g_plant = 60;
  else if (plant == "scene_z_flipped") q2d::g_plant = 74;           // S110 STARFRONT: [SCS DEPTH]
  else if (plant == "scene_dropped_on_flat") q2d::g_plant = 71;     // S105 A4B: [SCS FLAT]
  else if (plant == "walk_bit_never_set") q2d::g_plant = 72;        // S105 A4B: [WALK]
  else if (plant == "walk_end_ignored") q2d::g_plant = 73;          // S105 A4B: [WALK]
  else if (!plant.empty()) {
    std::fprintf(stderr, "unknown plant %s\n", plant.c_str());
    return 2;
  }
#else
  if (!plant.empty()) {
    std::fprintf(stderr, "built without -DRDR_Q2D_PLANTS\n");
    return 2;
  }
#endif
  HookChecks();
  DepthChecks();
  WalkChecks();
  FlatChecks();
  if (!plant.empty()) {
    const char* tag = plant == "scene_dropped_on_flat" ? "[SCS FLAT]" : (plant == "walk_bit_never_set" || plant == "walk_end_ignored") ? "[WALK]" : "[SCS DEPTH]";   // S105 A4B: each plant's own named check
    const bool caught = g_failed.find(tag) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? "caught by its named check" : "its named check did not fail", g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: scs_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
