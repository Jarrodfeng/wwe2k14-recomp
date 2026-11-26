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
// S111 ZFINAL adds [ZFINAL] (a z-tested lens row the target model classed Final, recorded inside the game's scene pass (op 73 .. op 74), behind a nearer body, is hidden) and [ZFINAL RULE] (the bracket decides, not a
// category range; what the rule leaves alone); twins scene_by_pass_off (plant 75) and scene_end_ignored (plant 76, the bracket outlives op 74).
// S111 ZFINAL2 adds [ZFINAL2] (one rule for every row: a z-off T_Rect, a glyph and a movie row the target model classed Final, recorded inside the bracket, go to the scene hook; rows outside it stay Final on F; no site list);
// twin scene_pass_two_sites (plant 77, the old z-on two-site test).
// S111 ZFINAL2 review adds [ZFINAL2 POST] (a row recorded at or after the game's post node, category 52, is the frame's: states 7 / 8's bracket 53..69 and op 74's skipped bind-back); twin after_post_in_scene (plant 78).
// Usage: out\tools\scs_unit.exe [--plant scene_z_ignored | scene_z_flipped | scene_dropped_on_flat | walk_bit_never_set | walk_end_ignored | scene_by_pass_off | scene_end_ignored | scene_pass_two_sites | after_post_in_scene]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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
  {   // S111 ZFINAL2: every scene-colour row is the hook's, whatever its site (the game's scene pass is the op 73 .. op 74 bracket, not a site list; was fix 3's two sites)
    Quad2D other = Flash(6);
    other.site = 0x82BC3784u;   // the glyph site
    SceneTables ot = t;
    ot.quads2d = {Flash(1), other, Lens(2)};
    hook.SetRows(ot);
    Check(hook.rows() == 3, "SCS HOOK", "every scene-colour row is taken, whatever its site: the flash, a glyph-site row, the lens");
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
      Check(f.blend[0].write_mask == 0xF && l.blend[0].write_mask == 0xF, "SCS DEPTH", "scene rows keep BlendOf's mask 0xF, the game's inherited mask (S111 RSTCOVER-B review: no post of the game's or ours reads the scene's alpha; was fix 4's 0x7)");
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
  other.site = 0x82BC4444u;   // another scene-colour site (S111 ZFINAL2: drawn like the two, no site list)
  Check(sh::FateOf(a, false, 1280, 720, true, false, true) == sh::Fate::Draw && sh::FateOf(b, false, 1280, 720, true, false, true) == sh::Fate::Draw, "SCS FLAT", "scene_on_f (a flat frame): the two scene-colour rows (belt sprite, rect) are drawn on F, even with the scene hook on");
  Check(sh::FateOf(fin, false, 1280, 720, true, false, true) == sh::Fate::Draw, "SCS FLAT", "a Final row is drawn as always");
  Check(sh::FateOf(a, false, 1280, 720, true, false, false) == sh::Fate::InScene && sh::FateOf(b, false, 1280, 720, true, false, false) == sh::Fate::InScene, "SCS FLAT", "not a flat frame (Hold3D, a drawn 3D frame): the scene-colour rows stay the scene hook's (InScene), never drawn on F");
  Check(sh::FateOf(a, false, 1280, 720, false, false, false) == sh::Fate::DropPass, "SCS FLAT", "no scene hook and not flat: the old `pass` drop stands");
  Check(sh::FateOf(other, false, 1280, 720, true, false, true) == sh::Fate::Draw && sh::FateOf(other, false, 1280, 720, true, false, false) == sh::Fate::InScene, "SCS FLAT", "S111 ZFINAL2: a scene-colour row of any site is drawn on F on a flat frame and is the scene hook's on a 3D frame (no site list)");
  const Quad2D rows[] = {Final(1), Lens(2), Flash(3), other, Final(5)};   // seq order: the Draw rows of one record form runs in record order (DrawRows flushes a run at the first non-Draw row)
  std::string fates;
  for (const Quad2D& r : rows) fates += sh::FateOf(r, false, 1280, 720, true, false, true) == sh::Fate::Draw ? 'D' : 'x';
  Check(fates == "DDDDD", "SCS FLAT", "a flat record's rows: Final, belt, rect, another site's scene row, Final, all drawn in one run in seq order: " + fates);
}

// S111 ZFINAL (docs\research\s111\ZFINAL.md; entry: walkcat::Mark (the op-19 recorder's call), wwe2k14::scene_host::FateOf (DrawRows), LiveScene2D::SetRows / Record (the scene hook), q2d::Append (both draw paths)).
// The lens element as the live tap records it: T_Sprite 0x82BC5688, pixel-placed (every 0x82BC5688 draw in c2d_entrance_s15 / a44 / d02), z 0.996094 (sub_82BC4890, v = 16777215), z test on, write off; pass = what the
// target model said: the frame (the thread's modelled target was the default surface at the op-19).
static Quad2D PixelLens(uint32_t seq) {
  Quad2D q = Lens(seq);
  q.have = (q.have & ~uint32_t(kQhMatrix)) | kQhPosScale;
  q.use_proj = 0.0f;
  for (float& p : q.proj) p = 0.0f;
  const float v[4][5] = {{600, 300, 0.996094f, 0, 0}, {680, 300, 0.996094f, 1, 0}, {600, 380, 0.996094f, 0, 1}, {680, 380, 0.996094f, 1, 1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY;
  q.pass = kQpFinal;
  return q;
}
// The host's path for one row, end to end: DrawRows' FateOf -> InScene goes to the scene hook (SetRows, Record at AfterBlended) and its lambda's Append with Context::scene_depth; Draw goes to F (no scene depth).
// Returns true when the drawn row is HIDDEN behind a body at forward depth 0.9 (scs_unit DepthChecks' body: LESS_OR_EQUAL fails for the element's 0.996).
static bool HiddenBehindBody(const Quad2D& row, uint8_t* fate_out, size_t* hooked_out) {
  namespace sh = wwe2k14::scene_host;
  using namespace q2d;
  const sh::Fate fate = sh::FateOf(row, false, 1280, 720, true, false, false);
  SceneTables t;
  t.header.guest_frame = 9;
  t.quad2d_holes = 0;
  t.quads2d = {row};
  std::vector<Quad2D> hooked;
  LiveScene2D hook([&](rdr::FrameBuilder&, const StageCtx&, const Quad2D* rows, size_t n, uint64_t, uint8_t) { hooked.assign(rows, rows + n); });
  hook.SetRows(t);
  rdr::FrameBuilder hfb;
  StageCtx sctx;
  hook.Record(Stage::AfterBlended, sctx, hfb);
  if (fate_out) *fate_out = uint8_t(fate);
  if (hooked_out) *hooked_out = hooked.size();
  const bool in_scene = fate == sh::Fate::InScene && hooked.size() == 1;
  if (!in_scene && fate != sh::Fate::Draw) return false;   // dropped: not drawn at all (the caller's check says which)
  Programs programs;
  programs.rect = 11, programs.movie = 22, programs.layout = 3, programs.sprite = 33, programs.sprite_hsl = 44, programs.glyph = 55;
  programs.rect_proj = 66, programs.sprite_proj = 77, programs.glyph_proj = 88;
  Context ctx;
  ctx.width = 1280, ctx.height = 720;
  ctx.scene_depth = in_scene;   // scene_host.cpp: the hook's lambda sets it; DrawRows' F context never does
  ctx.picture = [](const Quad2D& r) {
    rdr::TextureSlot s;
    s.image = r.tex_key[0] == kKey ? rdr::ImageId(900) : rdr::kNoImage;
    return s;
  };
  const Quad2D drawn_row = in_scene ? hooked[0] : row;
  rdr::FrameBuilder fb;
  fb.Begin(1);
  const Report rep = Append(fb, programs, ctx, &drawn_row, 1);
  const rdr::Frame frame = fb.Finish();
  if (rep.drawn != 1 || frame.draws.count != 1) return false;
  const rdr::DrawState& s = frame.states[frame.draws[0].state];
  const rdr::DynamicState& d = frame.dynamics[frame.draws[0].dynamic];
  const float body = 0.9f;
  const float ndc = drawn_row.verts[0][2] * d.system[2] + d.system[6], win = d.viewport[4] + ndc * (d.viewport[5] - d.viewport[4]);
  return s.depth_test == 1 && s.depth_compare == 3 && !(win <= body);
}
// The walk as the hooks see it: categories 0..cat in order (sub_82C3B200 / sub_82C3B218 around each); `ops` = the scene ops the render-state nodes record (73 = op 73 at that category, 74 = op 74), each before
// the row in its category; the row is recorded at `cat` (walkcat::Mark, the op-19 recorder's call).
static Quad2D RowAt(Quad2D q, uint32_t cat, const std::vector<std::pair<uint32_t, int>>& ops) {
  using namespace walkcat;
  Walk w;
  for (uint32_t c = 0; c <= cat; ++c) {
    w.Begin(c);
    for (const auto& o : ops)
      if (o.first == c) o.second == 73 ? w.SceneBegin() : w.SceneEnd();
    if (c == cat) Mark(q, w);
    w.End();
  }
  return q;
}
static void ZFinalChecks() {
  namespace sh = wwe2k14::scene_host;
  using namespace walkcat;
  {   // the bug: the game's scene pass open (op 73 at category 2, the state node sub_827F10C8), the lens at category 36, the model said Final
    Quad2D lens = RowAt(PixelLens(1), 36, {{2, 73}, {51, 74}});
    uint8_t fate = 255;
    size_t hooked = 0;
    const bool hidden = HiddenBehindBody(lens, &fate, &hooked);
    Check(lens.pass == kQpSceneColour && (lens.have & kQhSceneByPass) != 0 && (lens.have & kQhPre3DCat) == 0 && fate == uint8_t(sh::Fate::InScene) && hooked == 1 && hidden, "ZFINAL",
          "a z-tested lens row the target model classed Final, recorded at the game's category 36 (inside its scene pass): reclassed scene colour, InScene, the hook draws it with the scene depth: hidden behind a nearer body (fate " +
              std::to_string(fate) + ", hooked " + std::to_string(hooked) + ", hidden " + std::to_string(hidden) + ")");
  }
  {   // the bracket the game's state nodes record, not a category range (sub_827F0140 node categories, sub_827F0C90 the state's enabled nodes)
    const std::vector<std::pair<uint32_t, int>> match = {{2, 73}, {51, 74}}, dof = {{2, 73}, {37, 74}, {40, 73}, {44, 74}}, late = {{53, 73}, {69, 74}};
    const Quad2D m45 = RowAt(PixelLens(2), 45, match), d42 = RowAt(PixelLens(3), 42, dof);
    Check(m45.pass == kQpSceneColour && d42.pass == kQpSceneColour && (m45.have & d42.have & kQhSceneByPass), "ZFINAL RULE",
          "a z-tested lens row the model classed Final INSIDE the game's bracket is reclassed whatever its category: 45 in the match's (op 73 at 2, op 74 at 51), 42 in the depth-of-field form's second span (40..44) (S111 ZFINAL2 review: states 7 / 8's 53..69 is after the post: [ZFINAL2 POST])");
    const Quad2D m57 = RowAt(PixelLens(5), 57, match), d38 = RowAt(PixelLens(6), 38, dof), l36 = RowAt(PixelLens(7), 36, late), none = RowAt(PixelLens(8), 36, {}), at2 = RowAt(PixelLens(9), 2, {});
    Check(m57.pass == kQpFinal && d38.pass == kQpFinal && l36.pass == kQpFinal && none.pass == kQpFinal && at2.pass == kQpFinal && !((m57.have | d38.have | l36.have | none.have | at2.have) & kQhSceneByPass), "ZFINAL RULE",
          "a z-tested lens row OUTSIDE the bracket keeps its Final class: 57 after the match's op 74, 38 between the depth-of-field form's two spans, 36 before states 7 / 8's op 73 at 53, 36 on a walk with no op 73, 2 before any op 73");
    const Quad2D nowalk = [] { Quad2D q = PixelLens(10); walkcat::Walk w; w.SceneBegin(); Mark(q, w); return q; }();
    Check(nowalk.pass == kQpSceneColour, "ZFINAL RULE", "the bracket, not the category, decides: a row recorded after op 73 outside any main-walk category step (the clock or a category end between) is still in the scene pass");
    // (S111 ZFINAL2: a z-off row, another site's row and a row with no state read inside the bracket are now scene rows too: ZFinal2Checks)
    Quad2D off = PixelLens(13);
    off.pass = kQpOffscreen;
    Check(RowAt(off, 36, match).pass == kQpOffscreen, "ZFINAL RULE", "an offscreen z-tested row (a page) inside the bracket stays offscreen");
    const Quad2D scene = RowAt(Lens(15), 36, match);
    Check(scene.pass == kQpSceneColour && !(scene.have & kQhSceneByPass), "ZFINAL RULE", "a row the target model already classed scene colour is not marked reclassed");
  }
  {   // a reclassed row recorded before the 3D goes early, like every pre-3D scene-colour row
    const Quad2D lens = RowAt(PixelLens(16), 3, {{2, 73}, {51, 74}});
    Check(lens.pass == kQpSceneColour && (lens.have & kQhSceneByPass) && (lens.have & kQhPre3DCat), "ZFINAL", "a z-tested lens row classed Final at category 3 inside the bracket: reclassed scene colour and marked pre-3D (drawn at BeforeOpaque)");
  }
}

// S111 ZFINAL2 (docs\research\s111\ZFINAL2.md; entry: walkcat::Mark (the op-19 recorder's call), wwe2k14::scene_host::FateOf (DrawRows), LiveScene2D::SetRows / Record (the scene hook), q2d::Append (the hook's draw)).
// One rule for every row: a row the game records inside its scene pass (op 73 .. op 74) is drawn in our scene pass with its own state, whatever its site and z state; outside it the target model's class stands.
static Quad2D GlyphRow(uint32_t seq) {   // a text glyph row (site 0x82BC3784, the glyph leaf sub_82BC3518), z off, classed Final by the target model
  Quad2D q = Final(seq);
  q.kind = kQkGlyph, q.site = 0x82BC3784u, q.stride_bytes = 20;
  return q;
}
static Quad2D MovieRow(uint32_t seq) {   // a movie row (site 0x82BC7E50, T_SpriteYUVAlpha sub_82BC7A10), z off, classed Final by the target model
  Quad2D q = Final(seq);
  q.kind = kQkYuvAlpha, q.site = 0x82BC7E50u, q.stride_bytes = 20;
  return q;
}
struct HostPath {
  uint8_t fate = 255;
  size_t hooked = 0;
  bool drawn = false, depth_test = true, depth_write = true;
};
// The host's path for one row of a drawn 3D frame: DrawRows' FateOf, the scene hook's SetRows / Record (both stages), and for a rect row the hook lambda's Append with Context::scene_depth (its depth state).
static HostPath HostPathOf(const Quad2D& row) {
  namespace sh = wwe2k14::scene_host;
  using namespace q2d;
  HostPath p;
  p.fate = uint8_t(sh::FateOf(row, false, 1280, 720, true, false, false));
  SceneTables t;
  t.header.guest_frame = 9;
  t.quad2d_holes = 0;
  t.quads2d = {row};
  std::vector<Quad2D> hooked;
  LiveScene2D hook([&](rdr::FrameBuilder&, const StageCtx&, const Quad2D* rows, size_t n, uint64_t, uint8_t) { hooked.insert(hooked.end(), rows, rows + n); });
  hook.SetRows(t);
  rdr::FrameBuilder hfb;
  StageCtx sctx;
  hook.Record(Stage::BeforeOpaque, sctx, hfb);
  hook.Record(Stage::AfterBlended, sctx, hfb);
  p.hooked = hooked.size();
  if (p.hooked != 1 || row.kind != kQkRect) return p;
  Programs programs;
  programs.rect = 11, programs.movie = 22, programs.layout = 3, programs.sprite = 33, programs.sprite_hsl = 44, programs.glyph = 55;
  programs.rect_proj = 66, programs.sprite_proj = 77, programs.glyph_proj = 88;
  Context ctx;
  ctx.width = 1280, ctx.height = 720;
  ctx.scene_depth = true;   // scene_host.cpp: the hook's lambda sets it
  rdr::FrameBuilder fb;
  fb.Begin(1);
  const Report rep = Append(fb, programs, ctx, hooked.data(), 1);
  const rdr::Frame frame = fb.Finish();
  if (rep.drawn == 1 && frame.draws.count == 1) {
    const rdr::DrawState& s = frame.states[frame.draws[0].state];
    p.drawn = true, p.depth_test = s.depth_test != 0, p.depth_write = s.depth_write != 0;
  }
  return p;
}
static void ZFinal2Checks() {
  namespace sh = wwe2k14::scene_host;
  const std::vector<std::pair<uint32_t, int>> match = {{2, 73}, {51, 74}};   // the match's bracket: op 73 at category 2, op 74 at 51
  const auto said = [](const HostPath& p) { return std::string(" (fate ") + std::to_string(p.fate) + ", hooked " + std::to_string(p.hooked) + ")"; };
  {   // the screen flash: a z-off T_Rect row the target model put on the frame, inside the bracket
    const Quad2D flash = RowAt(Final(21), 36, match);
    const HostPath p = HostPathOf(flash);
    Check(flash.pass == kQpSceneColour && (flash.have & kQhSceneByPass) && p.fate == uint8_t(sh::Fate::InScene) && p.hooked == 1 && p.drawn && !p.depth_test && !p.depth_write, "ZFINAL2",
          "a z-off T_Rect row (the screen flash, site 0x82BC3EE8) the target model classed Final, recorded inside the bracket at category 36: reclassed scene colour, InScene, drawn by the scene hook (under our post) with no depth test, no depth write" + said(p));
  }
  {   // a glyph row inside the bracket
    const Quad2D glyph = RowAt(GlyphRow(22), 36, match);
    const HostPath p = HostPathOf(glyph);
    Check(glyph.pass == kQpSceneColour && (glyph.have & kQhSceneByPass) && p.fate == uint8_t(sh::Fate::InScene) && p.hooked == 1, "ZFINAL2",
          "a z-off glyph row (site 0x82BC3784) the target model classed Final, recorded inside the bracket: reclassed scene colour, InScene, the scene hook's row" + said(p));
  }
  {   // a movie row inside the bracket (the host lambda gives the hook's Append the planes: scs_lint wiring)
    const Quad2D movie = RowAt(MovieRow(23), 36, match);
    const HostPath p = HostPathOf(movie);
    Check(movie.pass == kQpSceneColour && (movie.have & kQhSceneByPass) && p.fate == uint8_t(sh::Fate::InScene) && p.hooked == 1, "ZFINAL2",
          "a movie row (site 0x82BC7E50) the target model classed Final, recorded inside the bracket: reclassed scene colour, InScene, the scene hook's row" + said(p));
  }
  {   // rows outside the bracket stay Final, drawn on F, never the hook's
    const Quad2D f57 = RowAt(Final(24), 57, match), g57 = RowAt(GlyphRow(25), 57, match), g36 = RowAt(GlyphRow(26), 36, {});
    const HostPath pf = HostPathOf(f57), pg = HostPathOf(g57), pn = HostPathOf(g36);
    Check(f57.pass == kQpFinal && g57.pass == kQpFinal && g36.pass == kQpFinal && !((f57.have | g57.have | g36.have) & kQhSceneByPass) && pf.fate == uint8_t(sh::Fate::Draw) && pg.fate == uint8_t(sh::Fate::Draw) &&
              pn.fate == uint8_t(sh::Fate::Draw) && pf.hooked == 0 && pg.hooked == 0 && pn.hooked == 0, "ZFINAL2",
          "rows outside the bracket keep their Final class and are drawn on F, not by the hook: a T_Rect and a glyph at category 57 (after the match's op 74), a glyph on a walk with no op 73" + said(pf) + said(pg) + said(pn));
  }
  {   // what the rule leaves alone, and the row with no state read
    Quad2D off = GlyphRow(27);
    off.pass = kQpOffscreen;
    const Quad2D o = RowAt(off, 36, match);
    Check(o.pass == kQpOffscreen && !(o.have & kQhSceneByPass) && HostPathOf(o).hooked == 0, "ZFINAL2", "an offscreen row inside the bracket (a target the game bound inside its scene pass) stays offscreen, not the hook's");
    Quad2D ns = Final(28);
    ns.have &= ~uint32_t(kQhState);
    const Quad2D n2 = RowAt(ns, 36, match);
    const HostPath p = HostPathOf(n2);
    Check(n2.pass == kQpSceneColour && p.hooked == 1 && (!p.drawn || !p.depth_test), "ZFINAL2", "a row whose state object was not read, inside the bracket: a scene row, never drawn with a guessed depth test" + said(p));
  }
}

// S111 ZFINAL2 review (docs\research\s111\ZFINAL2.md, Review; entry: walkcat::Mark, wwe2k14::scene_host::FateOf, LiveScene2D::SetRows / Record). The game's post node is category 52 (sub_82C1E840, registered at 52 by
// all 12 creators), so a 2D draw recorded at 52 or later is drawn over the post's output whatever target is bound. States 7 / 8's bracket (op 73 at 53, op 74 at 69: the +116 / +120 copy pair) is a resolve + copy
// around that 2D, not a scene pass under the post. Rows there are the frame's, drawn on F; before the post nothing changes.
static void ZFinal2PostChecks() {
  namespace sh = wwe2k14::scene_host;
  const std::vector<std::pair<uint32_t, int>> late = {{53, 73}, {69, 74}}, match = {{2, 73}, {51, 74}};
  const auto said = [](const Quad2D& q, const HostPath& p) { return std::string(" (pass ") + std::to_string(unsigned(q.pass)) + ", have " + std::to_string(q.have) + ", fate " + std::to_string(p.fate) + ", hooked " + std::to_string(p.hooked) + ")"; };
  const auto frame_row = [](const Quad2D& q, const HostPath& p) { return q.pass == kQpFinal && p.fate == uint8_t(wwe2k14::scene_host::Fate::Draw) && p.hooked == 0 && !(q.have & kQhSceneByPass); };
  {   // states 7 / 8: a menu glyph at 60 and a z-tested lens row at 60 the target model put on the frame, inside the copy pair's bracket: they stay Final, drawn on F (over our post), never the hook's
    const Quad2D g = RowAt(GlyphRow(41), 60, late), l = RowAt(PixelLens(42), 60, late);
    const HostPath pg = HostPathOf(g), pl = HostPathOf(l);
    Check(frame_row(g, pg) && frame_row(l, pl) && !((g.have | l.have) & kQhAfterPost), "ZFINAL2 POST",
          "states 7 / 8 (op 73 at 53, op 74 at 69): a Final glyph and a Final z-tested lens row at category 60 are not reclassed into the scene (60 is after the post node, 52): drawn on F" + said(g, pg) + said(l, pl));
  }
  {   // states 7 / 8: rows the target model classed scene colour (the copy pair's op 73 bound the scene object): put back on the frame
    Quad2D sg = GlyphRow(43);
    sg.pass = kQpSceneColour;
    const Quad2D g = RowAt(sg, 60, late), l = RowAt(Lens(44), 54, late);
    const HostPath pg = HostPathOf(g), pl = HostPathOf(l);
    Check(frame_row(g, pg) && frame_row(l, pl) && (g.have & l.have & kQhAfterPost), "ZFINAL2 POST",
          "states 7 / 8: a scene-colour glyph at 60 and a scene-colour lens row at 54 (the model followed the copy pair's op 73) are the frame's: kQpFinal + kQhAfterPost, drawn on F, not by the hook" + said(g, pg) + said(l, pl));
  }
  {   // op 74's skipped bind-back (sub_82C22650 +0x124, the scene's band layout): the model keeps the scene bound after the match's op 74; the FADE at 57 is drawn after the post
    Quad2D f = Final(45);
    f.pass = kQpSceneColour;
    const Quad2D r = RowAt(f, 57, match);
    const HostPath p = HostPathOf(r);
    Check(frame_row(r, p) && (r.have & kQhAfterPost), "ZFINAL2 POST", "a scene-colour T_Rect at category 57 (after the post node) is the frame's whatever the model kept bound: kQpFinal + kQhAfterPost, drawn on F" + said(r, p));
  }
  {   // before the post nothing changes: the match's bracket up to 51, and a row outside any main-walk category
    const Quad2D s36 = RowAt(Lens(46), 36, match), f50 = RowAt(Final(47), 50, match);
    const Quad2D nowalk = [] { Quad2D q = Lens(48); walkcat::Walk w; w.SceneBegin(); walkcat::Mark(q, w); return q; }();
    const HostPath p36 = HostPathOf(s36), p50 = HostPathOf(f50);
    Check(s36.pass == kQpSceneColour && f50.pass == kQpSceneColour && (f50.have & kQhSceneByPass) && nowalk.pass == kQpSceneColour && !((s36.have | f50.have | nowalk.have) & kQhAfterPost) &&
              p36.fate == uint8_t(sh::Fate::InScene) && p50.fate == uint8_t(sh::Fate::InScene) && p36.hooked == 1 && p50.hooked == 1, "ZFINAL2 POST",
          "before the post node: a scene-colour lens at 36 and a Final T_Rect at 50 inside the match's bracket are scene rows (the hook's), and a scene-colour row with no main-walk category is left alone" + said(s36, p36) + said(f50, p50));
  }
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
  else if (plant == "scene_by_pass_off") q2d::g_plant = 75;         // S111 ZFINAL: [ZFINAL]
  else if (plant == "scene_end_ignored") q2d::g_plant = 76;         // S111 ZFINAL review: [ZFINAL RULE]
  else if (plant == "scene_pass_two_sites") q2d::g_plant = 77;      // S111 ZFINAL2: [ZFINAL2]
  else if (plant == "after_post_in_scene") q2d::g_plant = 78;       // S111 ZFINAL2 review: [ZFINAL2 POST]
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
  ZFinalChecks();   // S111 ZFINAL
  ZFinal2Checks();   // S111 ZFINAL2
  ZFinal2PostChecks();   // S111 ZFINAL2 review
  if (!plant.empty()) {
    const char* tag = plant == "scene_dropped_on_flat" ? "[SCS FLAT]" : (plant == "walk_bit_never_set" || plant == "walk_end_ignored") ? "[WALK]" : plant == "scene_by_pass_off" ? "[ZFINAL]" : plant == "scene_end_ignored" ? "[ZFINAL RULE]" : plant == "scene_pass_two_sites" ? "[ZFINAL2]" : plant == "after_post_in_scene" ? "[ZFINAL2 POST]" : "[SCS DEPTH]";   // S105 A4B: each plant's own named check
    const bool caught = g_failed.find(tag) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? "caught by its named check" : "its named check did not fail", g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: scs_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
