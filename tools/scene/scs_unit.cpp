// scs_unit: lane SCS's offline proof (Session 103 overnight, docs\research\s103\RFF.md part B; 0 game runs, no GPU). The in-scene 2D rows (the screen flashes and the lens flares, Quad2D::pass == kQpSceneColour)
// are drawn by OUR scene hook inside the scene pass, not dropped:
//   [SCS HOOK]   LiveScene2D (frame_hooks.h) keeps exactly the scene-colour rows of the record, in seq order, and records them at Stage::AfterBlended only; a record whose 2D tap did not run gives it nothing
//   [SCS DRAW]   a planted scene-colour flash row (T_Rect, additive, z off) is drawn as one additive draw with no depth test, no depth write
//   [SCS DEPTH]  a lens flare row (T_Sprite, matrix-placed, additive, z test on, z write off) takes the game's depth state: test on, write off, our forward LEQUAL, the game's reversed z flipped (system z scale -1, offset w);
//                Append WITHOUT Context::scene_depth (the final-image path) keeps every depth test off (the F rows are unchanged)
// Planted twin (RDR_Q2D_PLANTS, --plant scene_z_ignored): the depth state / flip is not taken -> [SCS DEPTH] must fail. The old FateOf drop is the text lint scs_lint.py's twin (scene_host.cpp is not unit-testable here).
// Usage: out\tools\scs_unit.exe [--plant scene_z_ignored]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"
#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/frame_hooks.h"

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
  LiveScene2D hook([&](rdr::FrameBuilder&, const StageCtx&, const Quad2D* rows, size_t n, uint64_t gf) {
    ++calls, seen_frame = gf;
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
  Check(calls == 1 && seen_frame == 77 && seqs.size() == 2 && seqs[0] == 2 && seqs[1] == 3 && passes[0] == kQpSceneColour && passes[1] == kQpSceneColour, "SCS HOOK",
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
      Check(ld.system[2] == -1.0f && ld.system[6] == 1.0f, "SCS DEPTH", "a z-tested row's clip z is flipped to our forward depth: z' = w - z (scale -1, offset 1 x w)");
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

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
#ifdef RDR_Q2D_PLANTS
  if (plant == "scene_z_ignored") q2d::g_plant = 60;
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
  if (!plant.empty()) {
    const bool caught = g_failed.find("[SCS DEPTH]") != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? "caught by its named check [SCS DEPTH]" : "its named check [SCS DEPTH] did not fail", g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: scs_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
