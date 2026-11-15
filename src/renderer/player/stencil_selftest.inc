// S102 lane R24-C review F1: the synthetic stencil scenes (included by rplay.cpp after OpaqueState / FullViewport; shape of LightCubeSelfTest). No capture: frames built by hand, rendered Full /
// DisplayOnly / PersistentOnly, the picture read back and judged by pixel. Run with rplay --stencil-selftest [--stencil-dead 1] [--stencil-dead-poison] [--stencil-dead-plant N] --validation 1.
//   S1   a copy carries a LIVE stencil: A (64x64) gets stencil 2 on its left half, B (128x64) has stencil 1 everywhere, copy A -> B rect (0,0,64,64), then a pass on B draws white where EQUAL 2 and red
//        where EQUAL 1: x 0-31 white, 32-63 black, 64-127 red. Plant 2 (a depth-only copy) turns 0-63 red (poison: black).
//   S1b  the same through a chain A -> B -> C (tests on C). Plant 4 (the copy keeps both aspects but does not mark its source needed) must fail with the poison.
//   S2   an in-pass stencil clear to 3 on the left half, read by another pass (another colour target, the same depth): left half white. Plant 3 (the clear stripped) turns it black.
//   S2b  the same with no reader: the clear is stripped (in_stripped 1), the pass's stencil load is dead (loads_dead 1), the picture (a green left-half quad) is unchanged.
struct StencilScene {
  Renderer& r;
  uint16_t prog = 0, layout = 0;
  ImageId colour(uint32_t w, uint32_t h) {
    ImageDesc d;
    d.width = uint16_t(w), d.height = uint16_t(h), d.format = Format::RGBA8, d.usage = uint8_t(kUsageTarget | kUsageSampled);
    return r.CreateImage(d);
  }
  ImageId depth(uint32_t w, uint32_t h) {
    ImageDesc d;
    d.width = uint16_t(w), d.height = uint16_t(h), d.format = Format::Depth, d.usage = uint8_t(kUsageTarget | kUsageSampled);
    return r.CreateImage(d);
  }
};

// a quad over NDC [x0,x1] x [-1,1], colour (cr, cg, cb)
static void StencilQuad(FrameBuilder& b, float x0, float x1, float cr, float cg, float cb, DrawOp& draw) {
  const float v[] = {x0, -1, cr, cg, cb, x1, -1, cr, cg, cb, x1, 1, cr, cg, cb, x0, -1, cr, cg, cb, x1, 1, cr, cg, cb, x0, 1, cr, cg, cb};
  draw.vertex[0] = {kFrameGeometry, b.PushGeometry(v, sizeof(v))};
  draw.count = 6;
  draw.instances = 1;
}

// state: stencil test on with the given compare / pass op (front and back), reference from the dynamic block
static DrawState StencilState(uint16_t prog, uint16_t layout, bool test, uint8_t compare, uint8_t pass_op) {
  DrawState s = OpaqueState(prog, layout, false);
  s.stencil_test = test ? 1 : 0;
  s.front = {0, pass_op, 0, compare};
  s.back = s.front;
  return s;
}

static int StencilSelfTest(Renderer& renderer) {
  int fails = 0;
  const auto say = [&](bool ok, const char* text) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", text), fails += ok ? 0 : 1; };
  ProgramDesc tri_program;
  tri_program.name = "test_tri";
  tri_program.vertex_spirv = kSpv_test_tri_vert;
  tri_program.vertex_bytes = sizeof(kSpv_test_tri_vert);
  tri_program.fragment_spirv = kSpv_test_tri_frag;
  tri_program.fragment_bytes = sizeof(kSpv_test_tri_frag);
  StencilScene sc{renderer};
  sc.prog = renderer.AddProgram(tri_program);
  VertexLayoutDesc tri_layout{};
  tri_layout.attribute_count = 2;
  tri_layout.attributes[0] = {0, 0, kFormatR32G32Sfloat, 0};
  tri_layout.attributes[1] = {1, 0, 106 /* R32G32B32_SFLOAT */, 8};
  tri_layout.strides[0] = 20;
  sc.layout = renderer.AddVertexLayout(tri_layout);
  FrameBuilder builder;
  uint64_t frame_no = 0;
  const RenderMode modes[3] = {RenderMode::Full, RenderMode::DisplayOnly, RenderMode::PersistentOnly};
  const char* mode_names[3] = {"Full", "DisplayOnly", "PersistentOnly"};

  const auto pixel = [&](ImageId img, uint32_t x, uint32_t y, uint8_t out[4]) {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    ImageRef ref;
    ref.image = img;
    if (!renderer.ReadImage(ref, px, w, h) || x >= w || y >= h) return false;
    for (int i = 0; i < 4; ++i) out[i] = px[(size_t(y) * w + x) * 4 + i];
    return true;
  };
  const auto is = [&](ImageId img, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t p[4] = {};
    return pixel(img, x, y, p) && p[0] == r && p[1] == g && p[2] == b;
  };
  const auto draw_pass = [&](FrameBuilder& b, ImageId col, ImageId dep, uint32_t w, uint32_t h, bool clear_all, uint8_t stencil_clear, float x0, float x1, float cr, float cg, float cb, bool test, uint8_t compare, uint8_t pass_op, uint8_t ref) {
    TargetsOp t;
    t.color[0].image = col, t.color_count = 1, t.depth.image = dep;
    b.SetTargets(t);
    if (clear_all) {
      ClearOp c;
      c.flags = uint8_t(kClearColor | kClearDepth | (stencil_clear == 255 ? 0 : kClearStencil)), c.full = 1,   // stencil_clear 255 = no stencil in the load clear
       c.color[3] = 1.0f, c.depth = 1.0f, c.stencil = stencil_clear;
      b.Clear(c);
    }
    const float offset[4] = {0, 0, 0.5f, 0}, tint[4] = {1, 1, 1, 1};
    DrawOp d{};
    d.state = b.State(StencilState(sc.prog, sc.layout, test, compare, pass_op));
    DynamicState dy = FullViewport(float(w), float(h));
    dy.stencil_reference = ref, dy.stencil_compare_mask = 0xFF, dy.stencil_write_mask = 0xFF;
    d.dynamic = b.Dynamic(dy);
    d.textures = b.Textures(TextureSet{});
    d.vs_constants = b.PushConstants(offset, sizeof(offset)), d.fs_constants = b.PushConstants(tint, sizeof(tint));
    StencilQuad(b, x0, x1, cr, cg, cb, d);
    d.blend_key_low = 1;
    const BlendRange all{0, 16};
    d.blend_first = b.PushBlendRanges(&all, 1), d.blend_count = 1;
    b.Draw(d);
  };
  const uint8_t kEqual = 2, kAlways = 7, kReplace = 2, kKeep = 0;

  // ---- S1 / S1b ---------------------------------------------------------------------------------------------------------------------------------------------------
  for (int chain = 0; chain < 2; ++chain) {
    for (int m = 0; m < 3; ++m) {
      const ImageId Xc = sc.colour(64, 64), Yc = sc.colour(128, 64), A = sc.depth(64, 64), B = sc.depth(128, 64), C = chain ? sc.depth(128, 64) : kNoImage;
      const ImageId Z = chain ? C : B;
      builder.Begin(++frame_no);
      draw_pass(builder, Yc, B, 128, 64, true, 1, -1, -1, 0, 0, 0, false, kAlways, kKeep, 0);   // Y black, B's stencil 1 (nothing drawn: a zero-width quad)
      draw_pass(builder, Xc, A, 64, 64, true, 0, -1, 0, 0, 0, 0, true, kAlways, kReplace, 2);   // A's stencil 0, REPLACE 2 on the left half
      CopyOp c1;
      c1.source.image = A, c1.dest.image = B;
      c1.source_rect = {0, 0, 64, 64}, c1.dest_rect = {0, 0, 64, 64};
      builder.Copy(c1);
      if (chain) {
        CopyOp c2;
        c2.source.image = B, c2.dest.image = C;
        c2.source_rect = {0, 0, 128, 64}, c2.dest_rect = {0, 0, 128, 64};
        builder.Copy(c2);
      }
      draw_pass(builder, Yc, Z, 128, 64, false, 0, -1, 1, 1, 1, 1, true, kEqual, kKeep, 2);   // white where EQUAL 2
      draw_pass(builder, Yc, Z, 128, 64, false, 0, -1, 1, 1, 0, 0, true, kEqual, kKeep, 1);   // red where EQUAL 1
      const Frame f = builder.Finish();
      renderer.RenderFrame(f, modes[m]);
      renderer.WaitIdle();
      char what[160];
      if (m < 2) {
        const bool ok = is(Yc, 10, 32, 255, 255, 255) && is(Yc, 40, 32, 0, 0, 0) && is(Yc, 100, 32, 255, 0, 0);
        std::snprintf(what, sizeof what, "%s %s: x 10 white, x 40 black, x 100 red", chain ? "S1b chain" : "S1 copy", mode_names[m]);
        say(ok, what);
      } else {
        std::snprintf(what, sizeof what, "%s %s: rendered with no validation error (nothing recorded)", chain ? "S1b chain" : "S1 copy", mode_names[m]);
        say(renderer.validation_errors() == 0, what);
      }
    }
  }
  // ---- S2 / S2b ---------------------------------------------------------------------------------------------------------------------------------------------------
  for (int reader = 1; reader >= 0; --reader) {
    for (int m = 0; m < 3; ++m) {
      const ImageId X1 = sc.colour(64, 64), X2 = sc.colour(64, 64), D = sc.depth(64, 64);
      const uint64_t stripped0 = g_sd_in_stripped.load(), dead0 = g_sd_loads_dead.load();
      builder.Begin(++frame_no);
      draw_pass(builder, X1, D, 64, 64, true, 255, -1, 0, 0, 1, 0, false, kAlways, kKeep, 0);   // green left half; stencil cleared 0 by the load op
      ClearOp c;
      c.flags = kClearStencil, c.full = 0, c.stencil = 3, c.rect = {0, 0, 32, 64};   // INSIDE the pass (a draw came first): stencil 3 on the left half
      builder.Clear(c);
      if (reader) draw_pass(builder, X2, D, 64, 64, false, 0, -1, 1, 1, 1, 1, true, kEqual, kKeep, 3);   // white where EQUAL 3
      const Frame f = builder.Finish();
      renderer.RenderFrame(f, modes[m]);
      renderer.WaitIdle();
      char what[160];
      if (reader && m == 1) {   // DisplayOnly skips the persistent first pass (the depth it writes carries state to the reader): judged by validation only
        say(renderer.validation_errors() == 0, "S2 in-pass clear read later DisplayOnly: rendered with no validation error");
      } else if (m < 2) {
        const bool ok = reader ? (is(X2, 10, 32, 255, 255, 255) && is(X2, 50, 32, 0, 0, 0)) : (is(X1, 10, 32, 0, 255, 0) && is(X1, 50, 32, 0, 0, 0));
        std::snprintf(what, sizeof what, "%s %s: %s", reader ? "S2 in-pass clear read later" : "S2b in-pass clear, no reader", mode_names[m], reader ? "x 10 white, x 50 black" : "x 10 green, x 50 black");
        say(ok, what);
      } else {
        std::snprintf(what, sizeof what, "%s %s: rendered with no validation error", reader ? "S2" : "S2b", mode_names[m]);
        say(renderer.validation_errors() == 0, what);
      }
      if (!reader && m == 0 && g_stencil_dead.load() && !g_stencil_dead_plant.load()) {
        std::snprintf(what, sizeof what, "S2b counters: in-pass clears stripped %llu (expected 1), loads dead %llu (expected 1)", static_cast<unsigned long long>(g_sd_in_stripped.load() - stripped0), static_cast<unsigned long long>(g_sd_loads_dead.load() - dead0));
        say(g_sd_in_stripped.load() - stripped0 == 1 && g_sd_loads_dead.load() - dead0 == 1, what);
      }
    }
  }
  say(renderer.validation_errors() == 0, "no validation error over all scenes and modes");
  std::printf("[rplay][stencil_selftest] %s (stencil_dead %d, poison %d, plant %d)\n", fails ? "FAIL" : "PASS", int(g_stencil_dead.load()), int(g_stencil_dead_poison.load()), g_stencil_dead_plant.load());
  return fails ? 1 : 0;
}
