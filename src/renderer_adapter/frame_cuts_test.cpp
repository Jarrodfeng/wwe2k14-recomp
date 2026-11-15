// S102 lane G11: unit test for frame_cuts.h (no GPU, no capture): the walk's op calls (the same calls binder.h makes: OpSetTargets / OpDraw / OpClear / OpCopy / CopyDone / Foreign) and the decisions on plain Facts.
//   clang++ -std=c++20 -I src -I src\renderer_adapter src\renderer_adapter\frame_cuts_test.cpp -o frame_cuts_test.exe
//   frame_cuts_test.exe                     every case must pass
//   frame_cuts_test.exe plant_NAME          the planted fault NAME (see frame_cuts.h Fault): the test must FAIL
//     NAME: no_version no_tint no_rect no_stencil op_writes_off note_unissued no_proj no_scaled no_clip_off no_unknown_targets blit_always restore_swap
#include <cstdio>
#include <cstring>

#include "frame_cuts.h"

using namespace rdr_adapter::frame_cuts;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// Bryan's own numbers: frame buffer 8 (colour) / 9 (depth), 1280x720; texture 2 = a copy of 8, textures 5 (depth copy) and 2 for the restore.
static Facts BlitFacts() {
  Facts f;
  f.ps_hash = kBlitPs, f.vs_hash = kBlitVs, f.tex_count = 1, f.tex_image[0] = 2, f.tex_slot[0] = 0, f.tex_plain = true;
  const uint8_t id[4] = {3, 4, 5, 6};
  std::memcpy(f.tex_swz[0], id, 4);
  f.color_count = 1, f.color_image = 8, f.depth_image = 0;
  f.target_w = 1280, f.target_h = 720, f.color_format = 1;
  f.tex_w[0] = 1280, f.tex_h[0] = 720, f.tex_format[0] = 1;
  f.rect_ok = f.quad_ok = f.tint_ok = f.vp_exact = f.proj_zero = true;
  return f;
}
static Facts RestoreFacts() {
  Facts f;
  f.ps_hash = kRestorePs, f.vs_hash = kRestoreVs, f.tex_count = 2, f.tex_plain = true;
  f.tex_image[0] = 5, f.tex_slot[0] = 0, f.tex_image[1] = 2, f.tex_slot[1] = 1;
  const uint8_t rrrr[4] = {3, 3, 3, 3}, id[4] = {3, 4, 5, 6};
  std::memcpy(f.tex_swz[0], rrrr, 4), std::memcpy(f.tex_swz[1], id, 4);
  f.depth_test = f.depth_write = true, f.depth_compare = 7;
  f.color_count = 1, f.color_image = 8, f.depth_image = 9;
  f.target_w = 1280, f.target_h = 720, f.color_format = 1, f.depth_format = 23;
  f.tex_w[0] = f.tex_w[1] = 1280, f.tex_h[0] = f.tex_h[1] = 720, f.tex_format[0] = 23, f.tex_format[1] = 1;
  f.rect_ok = f.vp_exact = f.clip_off = f.clip_std = f.quad_ok = true;
  return f;
}

// The walk of Bryan's entrance as the binder feeds it: targets 8/9, a draw into them, 8 -> 2 copied (issued, whole), other targets drawn, then the blit's own SetTargets + draw.
static void Entrance(Tracker& t, bool copy_issued = true) {
  const uint32_t c8[1] = {8}, c254[1] = {254};
  t.BeginFrame(true);
  t.OpSetTargets(c8, 1, 9);
  t.OpDraw();                      // the restore / the hidden draws
  t.OpCopy(2);
  t.CopyDone(8, 2, copy_issued, true);
  t.OpSetTargets(c254, 1, 9);
  t.OpDraw();                      // a downsample into 254
  t.OpSetTargets(c8, 1, 0);        // the blit's SetTargets
  t.OpDraw();                      // the blit op itself: its write is applied at the NEXT op, a decision about it sees the state before
}

int main(int argc, char** argv) {
  if (argc > 1) {
    const char* name = argv[1];
    if (std::strncmp(name, "plant_", 6) == 0) name += 6;
    if (!ParseFault(name, g_fault)) { std::printf("unknown plant %s\n", argv[1]); return 2; }
  }
  const uint32_t c8[1] = {8};
  // 1. a blit of an unchanged whole copy: taken
  {
    Tracker t;
    Entrance(t);
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Blit);
    CHECK(t.census.blit_why[int(Why::Taken)] == 1 && t.census.blit_seen == 1);
  }
  // 2. T drawn into after the copy: stale (plant op_writes_off: draws register no write; no_version: the version rule skipped)
  {
    Tracker t;
    t.BeginFrame(true);
    t.OpSetTargets(c8, 1, 9);
    t.OpCopy(2);
    t.CopyDone(8, 2, true, true);
    t.OpDraw();                    // a draw into 8 after the copy
    t.OpDraw();                    // the blit's op
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep && t.census.blit_why[int(Why::Stale)] == 1);
  }
  // 2b. a clear of T, a copy into T, a copy into X after the copy: stale
  {
    Tracker t;
    t.BeginFrame(true);
    t.OpSetTargets(c8, 1, 9);
    t.OpCopy(2); t.CopyDone(8, 2, true, true);
    t.OpClear(); t.OpDraw();
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep);
    t.BeginFrame(true);
    t.OpSetTargets(c8, 1, 9);
    t.OpCopy(2); t.CopyDone(8, 2, true, true);
    t.OpCopy(8); t.OpDraw();       // another copy INTO the target
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep);
    t.BeginFrame(true);
    t.OpSetTargets(c8, 1, 9);
    t.OpCopy(2); t.CopyDone(8, 2, true, true);
    t.OpCopy(2); t.OpDraw();       // another write INTO the texture
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep);
  }
  // 3. F5: a write the capture's own ops do not show (the post group's pass drawn at the first claimed draw): Foreign() between the copy and the blit
  {
    Tracker t;
    t.BeginFrame(true);
    t.OpSetTargets(c8, 1, 9);
    t.OpCopy(2); t.CopyDone(8, 2, true, true);
    t.Foreign(8);
    t.OpDraw();
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep && t.census.blit_why[int(Why::Stale)] == 1);
  }
  // 4. a copy the walk did not issue (an alias elide, a drop) is no provenance
  {
    Tracker t;
    Entrance(t, false);
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep && t.census.blit_why[int(Why::Provenance)] == 1);   // (note_unissued plants this one to Blit)
  }
  // 5. no copy / a copy from another image / a copy that is not whole
  {
    Tracker t;
    t.BeginFrame(true);
    t.OpSetTargets(c8, 1, 9);
    t.OpDraw();
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep && t.census.blit_why[int(Why::Provenance)] == 1);
    t.OpCopy(2); t.CopyDone(7, 2, true, true);
    t.OpDraw();
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep && t.census.blit_why[int(Why::Provenance)] == 2);
    t.OpCopy(2); t.CopyDone(8, 2, true, false);   // a part copy
    t.OpDraw();
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep && t.census.blit_why[int(Why::Provenance)] == 3);
  }
  // 6. F6: a draw / clear before any target switch writes an unknown image: the frame is unusable from there
  {
    Tracker t;
    t.BeginFrame(true);
    t.OpDraw();                    // no targets known
    t.OpSetTargets(c8, 1, 9);
    t.OpCopy(2); t.CopyDone(8, 2, true, true);
    t.OpDraw();
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep && t.census.blit_why[int(Why::Unusable)] == 1 && t.census.unknown_targets == 1);   // (no_unknown_targets plants this one to Blit)
    t.BeginFrame(true);
    t.OpClear();                   // a clear with no targets: the same
    t.OpSetTargets(c8, 1, 9);
    t.OpCopy(2); t.CopyDone(8, 2, true, true);
    t.OpDraw();
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep);
  }
  // 7. the blit's shader, constants, tint, quad, viewport, clip-off
  {
    Tracker t;
    Entrance(t);
    Facts f;
    f = BlitFacts(); f.vs_hash = kRestoreVs;      CHECK(t.Decide(f).kind == Verdict::Keep);   // another vertex shader
    f = BlitFacts(); f.proj_zero = false;         CHECK(t.Decide(f).kind == Verdict::Keep);   // c28.x == 1: a projection (no_proj plants this one to Blit)
    f = BlitFacts(); f.tint_ok = false;           CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_tint)
    f = BlitFacts(); f.quad_ok = false;           CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_rect)
    f = BlitFacts(); f.rect_ok = false;           CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_rect)
    f = BlitFacts(); f.vp_exact = false;          CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_rect)
    f = BlitFacts(); f.clip_off = true;           CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_rect)
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Blit);
  }
  // 8. F3: a scaled target and an unscaled texture (or the reverse): the copy resampled, the blit is not the identity
  {
    Tracker t;
    Entrance(t);
    Facts f = BlitFacts();
    f.scaled_color = true;                        CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_scaled plants this one to Blit)
    f = BlitFacts(); f.scaled_tex[0] = true;      CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.scaled_color = f.scaled_tex[0] = true; CHECK(t.Decide(f).kind == Verdict::Blit);
  }
  // 9. state, textures, sizes, aliases, uploads, unusable frames, thin draws
  {
    Tracker t;
    Entrance(t);
    Facts f;
    f = BlitFacts(); f.blend = true;           CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.write_mask = 7;         CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.depth_write = true;     CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.alpha_test = true;      CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.stencil_test = true;    CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.tex_count = 2;          CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.tex_plain = false;      CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.tex_swz[0][0] = 5;      CHECK(t.Decide(f).kind == Verdict::Keep);   // a channel swap
    f = BlitFacts(); f.tex_w[0] = 640;         CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.tex_format[0] = 7;      CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.thin = true;            CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.color_count = 2;        CHECK(t.Decide(f).kind == Verdict::Keep);
    f = BlitFacts(); f.same_colour = true;     CHECK(t.Decide(f).kind == Verdict::Keep && t.census.blit_why[int(Why::Alias)] == 1);   // F8
    t.MarkDirty(2);
    CHECK(t.Decide(BlitFacts()).kind == Verdict::Keep);
    Tracker u;
    u.BeginFrame(false);
    u.OpSetTargets(c8, 1, 9); u.OpCopy(2); u.CopyDone(8, 2, true, true); u.OpDraw();
    CHECK(u.Decide(BlitFacts()).kind == Verdict::Keep && u.census.blit_why[int(Why::Unusable)] == 1 && u.census.frames_unusable == 1);
    u.BeginFrame(true);
    CHECK(u.Decide(BlitFacts()).kind == Verdict::Keep);   // a new frame forgets every copy
  }
  // 10. the restore: taken, and each rule
  {
    Tracker t;
    t.BeginFrame(true);
    t.OpSetTargets(c8, 1, 9);
    t.OpDraw();
    Verdict v = t.Decide(RestoreFacts());
    CHECK(v.kind == Verdict::Restore && v.colour_src == 2 && v.colour_dst == 8 && v.depth_src == 5 && v.depth_dst == 9 && v.copy_depth);   // (restore_swap plants the sources swapped)
    Facts f;
    f = RestoreFacts(); f.blend = true;             CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.depth_compare = 1;        CHECK(t.Decide(f).kind == Verdict::Keep);   // not ALWAYS
    f = RestoreFacts(); f.depth_write = false;      CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.depth_test = false;       CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.write_mask = 3;           CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.stencil_test = true;      CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.rect_ok = false;          CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_rect)
    f = RestoreFacts(); f.quad_ok = false;          CHECK(t.Decide(f).kind == Verdict::Keep);   // (no_rect)
    f = RestoreFacts(); f.vs_hash = kBlitVs;        CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.vp_exact = false;         CHECK(t.Decide(f).kind == Verdict::Keep);   // F4: a viewport of 2W (no_clip_off plants this one to Restore)
    f = RestoreFacts(); f.clip_off = false;         CHECK(t.Decide(f).kind == Verdict::Keep);   // F4: not the 360's pixel-space rectangle
    f = RestoreFacts(); f.clip_std = false;         CHECK(t.Decide(f).kind == Verdict::Keep);   // F4: another clip-off mapping
    f = RestoreFacts(); f.depth_image = 0;          CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.tex_count = 1;            CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.tex_swz[0][1] = 4;        CHECK(t.Decide(f).kind == Verdict::Keep);   // the depth source must read R in every channel
    f = RestoreFacts(); f.tex_swz[1][0] = 5;        CHECK(t.Decide(f).kind == Verdict::Keep);   // the colour source must read identity
    f = RestoreFacts(); f.tex_slot[0] = 1, f.tex_slot[1] = 0;   // the slots swapped: slot 0 is the colour now: its swizzle is not RRRR
                                                    CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.tex_w[0] = 640;           CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.tex_format[0] = 1;        CHECK(t.Decide(f).kind == Verdict::Keep);   // a colour texture where the depth one belongs
    f = RestoreFacts(); f.depth_format = 0;         CHECK(t.Decide(f).kind == Verdict::Keep);
    f = RestoreFacts(); f.scaled_tex[1] = true;     CHECK(t.Decide(f).kind == Verdict::Keep);   // F3 (no_scaled)
    f = RestoreFacts(); f.scaled_depth = true;      CHECK(t.Decide(f).kind == Verdict::Keep);   // F3
    f = RestoreFacts(); f.scaled_color = f.scaled_depth = f.scaled_tex[0] = f.scaled_tex[1] = true; CHECK(t.Decide(f).kind == Verdict::Restore);
    f = RestoreFacts(); f.same_colour = true;       CHECK(t.Decide(f).kind == Verdict::Keep);   // F8
    f = RestoreFacts(); f.same_depth = true;        CHECK(t.Decide(f).kind == Verdict::Keep);   // F8
    t.MarkStencil(9);
    CHECK(t.Decide(RestoreFacts()).kind == Verdict::Keep && t.census.restore_why[int(Why::Stencil)] == 1);   // (no_stencil plants this one to Restore)
    t.BeginFrame(true);
    CHECK(t.Decide(RestoreFacts()).kind == Verdict::Restore);   // a new frame forgets the stencil mark
  }
  // 11. other shaders are not ours and not counted
  {
    Tracker t;
    t.BeginFrame(true);
    Facts f = BlitFacts();
    f.ps_hash = 0x1234;
    CHECK(t.Decide(f).kind == Verdict::Keep && t.census.blit_seen == 0 && t.census.restore_seen == 0);
  }
  // 12. blit_always: every draw with the sprite ps goes, whatever it is
  {
    Tracker t;
    t.BeginFrame(true);
    Facts f = BlitFacts();
    f.blend = true;
    CHECK(t.Decide(f).kind == Verdict::Keep);   // (blit_always plants this one to Blit)
  }
  if (g_fail) std::printf("%d FAILED\n", g_fail);
  else std::printf("all passed\n");
  return g_fail ? 1 : 0;
}
