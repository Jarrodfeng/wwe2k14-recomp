// Lane RFX (Session 103): the offline unit proof that the 2D layer's sprite / hue / glyph rows sample with the address mode the GAME set (the state object's +148, sub_82C23D78) instead of a forced clamp.
// The bug it re-finds: the retro arenas' film-grain flipbook (uv 0..15, WRAP) was drawn with CLAMP_TO_EDGE: smeared top / left bands and one texel over the screen. 0 game runs, no GPU, no game file.
//
//   rfx_unit                 every check; exit 0 only when all pass
//   rfx_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (plants: src\renderer_scene\quads2d_plants.h, -DRDR_Q2D_PLANTS)
//                            address_ignored -> [RFX TILE] (the past bug), code7_wrap -> [RFX CODE7], unknown_wrap -> [RFX UNKNOWN]
//
//   [SVY ROW] / [SVY BLOCKS]  the no-alpha movie sprite T_SpriteYUV (site 0x82BC799C, the 30 Years season video): a scope makes a yuv row with three plane keys (not a hole), AlphaMix (0, 1, 1, 0), a matrix row refused, record round trip, Append draws it; planted twin yuv_site_hole (the site is a hole again) -> [SVY ROW]
//   [RFX TAP]      ReadState reads the word at +148 of the per-thread state object, FillState puts it in the row (kQhAddress); an unreadable word leaves the row without the bit
//   [RFX RECORD]   layout 5 (324 bytes a row): a row with an address round-trips byte-equal; a layout-4 row is refused
//   [RFX MODES]    the code -> (u, v) table: 0 repeat, 1 mirror, 2 and 7 clamp to edge, 4 border, 5 / 6 mixed, 3 / unknown / no bit -> clamp to edge
//   [RFX TILE]     Append: a uv 0..15 sprite row with code 0 gets a REPEAT sampler and a CPU reference of the sampler tiles (texel at frac(uv)); today's clamp would smear
//   [RFX CODE7]    code 7 -> clamp to edge (the game's own remap)
//   [RFX UNKNOWN]  code 3, an unknown code and a row with no address bit -> clamp to edge (old records draw unchanged); a movie row keeps clamp whatever the state says
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"
#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/record_io.h"

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

struct FakeMem {
  std::map<uint32_t, uint32_t> words;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  bool ReadWords(uint32_t a, uint32_t* out, uint32_t n) const {
    if (a < 0x10000u || uint64_t(a) + 4ull * n > 0x7F000000ull) return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = words.find(a + 4 * i);
      out[i] = it == words.end() ? 0u : it->second;
    }
    return true;
  }
};

static constexpr float kScaleX = 2.f / 1280.f, kScaleY = 2.f / 720.f;
static constexpr uint32_t kKey = 0x1234;
// A valid sprite row; the picture is 'image 900'. The uv of the four corners is 0..15 (the film grain's tiled quad).
static Quad2D GrainRow(uint32_t seq, uint32_t code, bool has_bit) {
  Quad2D q;
  q.kind = kQkSprite, q.site = q2d::kSiteSprite, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 20;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest | kQhPlanes;
  q.blend = kQbAlpha;
  const float v[4][5] = {{0, 0, 0.5f, 0, 0}, {1280, 0, 0.5f, 15, 0}, {1280, 1280, 0.5f, 15, 15}, {0, 1280, 0.5f, 0, 15}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY;
  q.color[0] = q.color[1] = q.color[2] = 1.f, q.color[3] = 100.f / 255.f;
  q.tex_key[0] = kKey;
  if (has_bit) q.have |= kQhAddress, q.address = code;
  ValidSC(q);
  return q;
}

// The CPU reference of one axis of the GPU sampler (Vulkan 16.? address modes): the texel index of coordinate `u` on an `n`-texel axis; -1 = the border (transparent black).
static int TexelOf(uint8_t mode, float u, int n) {
  float t = u;
  if (mode == 0) t = u - std::floor(u);
  else if (mode == 1) {
    const float f = u - 2.f * std::floor(u / 2.f);
    t = f <= 1.f ? f : 2.f - f;
  } else if (mode == 3) {
    if (u < 0.f || u > 1.f) return -1;
  } else t = u < 0.f ? 0.f : u > 1.f ? 1.f : u;   // clamp to edge
  int i = int(std::floor(t * float(n)));
  return i < 0 ? 0 : i > n - 1 ? n - 1 : i;
}

static rdr::SamplerDesc SamplerOf(const Quad2D& row, bool* drawn = nullptr) {
  using namespace q2d;
  rdr::FrameBuilder fb;
  fb.Begin(1);
  Programs programs;
  programs.rect = 11, programs.movie = 22, programs.layout = 3, programs.sprite = 33, programs.sprite_hsl = 44, programs.glyph = 55;
  Context ctx;
  ctx.width = 1920, ctx.height = 1080;
  ctx.picture = [](const Quad2D& r) {
    rdr::TextureSlot t;
    t.image = r.tex_key[0] == kKey ? rdr::ImageId(900) : rdr::kNoImage;
    return t;
  };
  const Report rep = Append(fb, programs, ctx, &row, 1);
  if (drawn) *drawn = rep.drawn == 1;
  const rdr::Frame frame = fb.Finish();
  rdr::SamplerDesc none{};
  none.address_u = none.address_v = 99;
  if (frame.draws.count != 1) return none;
  const rdr::TextureSet& ts = frame.texture_sets[frame.draws[0].textures];
  return frame.samplers[ts.slots[0].sampler];
}

static void TapChecks() {
  using namespace q2d;
  FakeMem m;
  const uint32_t r13 = 0x40000000u, block = 0x40001000u, obj = 0x40002000u;
  m.Put(r13, block);
  m.Put(block + kStateSlot, obj);
  m.Put(obj + kStBlendEnable, 1);
  m.Put(obj + kStAddress0, 0);   // the grain's WRAP
  StateWords s;
  Counters c;
  Quad2D q = GrainRow(1, 99, false);
  const bool ok = ReadState(m, r13, s);
  FillState(q, s, c);
  Check(ok && s.address_read && s.address0 == 0 && (q.have & kQhAddress) && q.address == 0, "RFX TAP", "the state object's +148 (pixel sampler 0 address code) = 0 is read and lands in the row with kQhAddress");
  m.Put(obj + kStAddress0, 7);
  Quad2D q7 = GrainRow(2, 99, false);
  ReadState(m, r13, s), FillState(q7, s, c);
  Check((q7.have & kQhAddress) && q7.address == 7, "RFX TAP", "code 7 is carried as read (the drawer remaps it)");
  Quad2D qn = GrainRow(3, 99, false);
  StateWords none;
  FillState(qn, none, c);
  Check(!(qn.have & kQhAddress) && qn.address == 0xFFFFFFFFu, "RFX TAP", "an address word that could not be read leaves the row without the bit (the old clamp)");
}

static void RecordChecks() {
  SceneTables t;
  t.has_camera = true;
  t.quads2d = {GrainRow(1, 0, true), GrainRow(2, 0, false)};
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string e;
  const bool ok = !bytes.empty() && ParseRecord(bytes.data(), bytes.size(), back, e) && back.quads2d.size() == 2 && back.quad2d_errors == 0 &&
                  std::memcmp(&back.quads2d[0], &t.quads2d[0], sizeof(Quad2D)) == 0 && std::memcmp(&back.quads2d[1], &t.quads2d[1], sizeof(Quad2D)) == 0;
  Check(sizeof(Quad2D) == 324 && kQuad2DLayout == 5 && ok && back.quads2d[0].address == 0 && (back.quads2d[0].have & kQhAddress) && !(back.quads2d[1].have & kQhAddress), "RFX RECORD",
        "layout 5, 324 bytes a row: a row with its address and a row without round-trip byte-equal");
  Quad2D old = GrainRow(3, 0, true);
  old.version = 4;
  Check(!Quad2DRowProblem(old).empty(), "RFX RECORD", "a layout-4 row is refused (fail closed)");
}

static void ModeChecks() {
  struct Row { uint32_t code; bool bit; uint8_t u, v; const char* tag; };
  // 0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 clamp to border
  const Row rows[] = {{0, true, 0, 0, "RFX TILE"}, {1, true, 1, 1, "RFX MODES"}, {2, true, 2, 2, "RFX MODES"}, {4, true, 3, 3, "RFX MODES"}, {5, true, 0, 1, "RFX MODES"}, {6, true, 1, 0, "RFX MODES"},
                      {7, true, 2, 2, "RFX CODE7"}, {3, true, 2, 2, "RFX UNKNOWN"}, {8, true, 2, 2, "RFX UNKNOWN"}, {0xFFFFFFFFu, true, 2, 2, "RFX UNKNOWN"}, {0, false, 2, 2, "RFX UNKNOWN"}};
  for (const Row& r : rows) {
    uint8_t u = 99, v = 99;
    q2d::AddressModes(GrainRow(1, r.code, r.bit), u, v);
    char b[160];
    std::snprintf(b, sizeof b, "code %u%s -> address (u, v) = (%d, %d), want (%d, %d)", r.code, r.bit ? "" : " without the bit", u, v, r.u, r.v);
    Check(u == r.u && v == r.v, r.tag, b);
  }
}

static void DrawChecks() {
  // [RFX TILE] the drawer's sampler for the grain row, then the CPU reference: a 4-texel axis at uv 7.3 and 0.3
  const rdr::SamplerDesc s0 = SamplerOf(GrainRow(1, 0, true));
  const bool tiles = s0.address_u == 0 && s0.address_v == 0 && TexelOf(s0.address_u, 7.3f, 4) == 1 && TexelOf(s0.address_v, 15.f, 4) == 0 && TexelOf(s0.address_u, 0.3f, 4) == 1 && s0.mag_filter == 1 && s0.min_filter == 1;
  Check(tiles, "RFX TILE", "a uv 0..15 sprite row with the game's WRAP (code 0) draws with a REPEAT sampler: the texel at uv 7.3 is the texel at 0.3 (tiled), not the edge texel");
  const rdr::SamplerDesc sc = SamplerOf(GrainRow(1, 2, true));
  Check(sc.address_u == 2 && TexelOf(sc.address_u, 7.3f, 4) == 3 && TexelOf(sc.address_v, 7.3f, 4) == 3, "RFX TILE", "a code-2 row stays CLAMP_TO_EDGE (the texel at uv 7.3 is the edge texel)");
  const rdr::SamplerDesc s7 = SamplerOf(GrainRow(1, 7, true));
  Check(s7.address_u == 2 && s7.address_v == 2, "RFX CODE7", "code 7 through the drawer: clamp to edge (the game's own 7 -> 2)");
  const rdr::SamplerDesc s3 = SamplerOf(GrainRow(1, 3, true));
  const rdr::SamplerDesc sb = SamplerOf(GrainRow(1, 0, false));
  const rdr::SamplerDesc su = SamplerOf(GrainRow(1, 200, true));
  Check(s3.address_u == 2 && sb.address_u == 2 && su.address_u == 2 && su.address_v == 2 && sb.address_v == 2, "RFX UNKNOWN", "code 3, no address bit and an unknown code each draw with clamp to edge (an old record draws as before)");
  Quad2D hsl = GrainRow(2, 0, true);
  hsl.have |= kQhHsl, hsl.hsl[0] = 0.25f;
  const rdr::SamplerDesc sh = SamplerOf(hsl);
  Check(sh.address_u == 0 && sh.address_v == 0, "RFX TILE", "the hue-shifted sprite program takes the game's address mode too");
  Quad2D glyph = GrainRow(3, 1, true);
  glyph.kind = kQkGlyph, glyph.site = q2d::kSiteGlyph, glyph.prim = 6, glyph.have |= kQhChannel;
  glyph.hsl[0] = 1.f;
  bool gd = false;
  const rdr::SamplerDesc sg = SamplerOf(glyph, &gd);
  Check(!gd || (sg.address_u == 2 && sg.address_v == 2), "RFX MODES", "a glyph-leaf row (site 0x82BC3784: its +148 is left over) keeps clamp whatever the code says");
  Quad2D gm = GrainRow(4, 1, true);
  gm.site = q2d::kSiteGlyphM;
  const rdr::SamplerDesc smm = SamplerOf(gm);
  Check(smm.address_u == 1 && smm.address_v == 1, "RFX MODES", "a row of the sprite-variant leaf (site 0x82BC8BEC, which sets its address before the draw) takes the mirror sampler for code 1");
  Quad2D rect = GrainRow(5, 0, true);
  rect.site = q2d::kSiteRect, rect.kind = kQkRect;
  {
    Quad2D g = GrainRow(5, 0, true), y = GrainRow(6, 0, true), yn = GrainRow(7, 0, true);
    g.kind = kQkGlyph, y.kind = kQkYuvAlpha, yn.kind = kQkYuv;
    Check(q2d::RowSetsAddress(GrainRow(1, 0, true)) && q2d::RowSetsAddress(y) && q2d::RowSetsAddress(yn) && !q2d::RowSetsAddress(g) && !q2d::RowSetsAddress(rect), "RFX MODES",
          "the rows that honour the code (from the site table's kinds): sprite (T_Sprite, the sprite-variant leaf), yuv-alpha, yuv; the glyph leaf and the rects keep clamp");
  }
  {   // a layout-4 capture (a real 320-byte stride, two rows, the second with scissor_on 1 and a rect) is still read: 'address unknown' = clamp, the scissor intact
    Quad2D r1 = GrainRow(1, 0, true), r2 = GrainRow(2, 0, true);
    r2.scissor_on = 1, r2.scissor[0] = 10, r2.scissor[1] = 20, r2.scissor[2] = 300, r2.scissor[3] = 200;
    SceneTables t;
    t.has_camera = true;
    t.quads2d = {r1, r2};
    std::vector<uint8_t> by = SerializeRecord(t);
    bool patched = false;
    for (size_t i = 0; i + 24 <= by.size() && !patched; i += 4) {
      uint32_t id, es;
      uint64_t count, off;
      std::memcpy(&id, &by[i], 4), std::memcpy(&es, &by[i + 4], 4), std::memcpy(&count, &by[i + 8], 8), std::memcpy(&off, &by[i + 16], 8);
      if (id == kScQuads2D && es == sizeof(Quad2D) && count == 2) {
        // repack the two rows at the old 320-byte stride: the layout-5 row minus the `address` word (which sits just before scissor_on), version 4
        std::vector<uint8_t> packed;
        for (int k = 0; k < 2; ++k) {
          const uint8_t* row = &by[size_t(off) + size_t(k) * sizeof(Quad2D)];
          const size_t at = offsetof(Quad2D, address);
          std::vector<uint8_t> r(row, row + at);
          r.insert(r.end(), row + at + 4, row + sizeof(Quad2D));   // scissor_on follows: 316 + 4 = 320 bytes
          const uint32_t ver = 4;
          std::memcpy(r.data(), &ver, 4);
          packed.insert(packed.end(), r.begin(), r.end());
        }
        std::memcpy(&by[size_t(off)], packed.data(), packed.size());
        es = kQuad2DLayout4Bytes, std::memcpy(&by[i + 4], &es, 4), patched = true;
      }
    }
    SceneTables back;
    std::string e;
    const bool ok = patched && ParseRecord(by.data(), by.size(), back, e) && back.quads2d.size() == 2 && back.quad2d_errors == 0;
    Check(ok && !(back.quads2d[0].have & kQhAddress) && back.quads2d[0].address == 0xFFFFFFFFu && back.quads2d[0].version == kQuad2DLayout && back.quads2d[0].scissor_on == 0 &&
              back.quads2d[1].scissor_on == 1 && back.quads2d[1].scissor[0] == 10 && back.quads2d[1].scissor[1] == 20 && back.quads2d[1].scissor[2] == 300 && back.quads2d[1].scissor[3] == 200 && back.quads2d[1].seq == 2,
          "RFX RECORD", "a layout-4 capture (two 320-byte rows, scissor_on 1 and its rect in row 2) is read as 'address unknown' (not refused): the old captures draw as before, row 2's scissor intact");
    if (ok) {
      const rdr::SamplerDesc sl = SamplerOf(back.quads2d[0]);
      Check(sl.address_u == 2 && sl.address_v == 2, "RFX UNKNOWN", "a layout-4 row draws with clamp to edge");
    }
  }
}

// ---- [SVY ...] the no-alpha movie sprite T_SpriteYUV (site 0x82BC799C): the 30 Years season video ------------------------------------------------------------------------
static void PutMovie(FakeMem& m, uint32_t at) {
  const float v[4][5] = {{160, 600, 0.5f, 0, 0}, {1120, 600, 0.5f, 1, 0}, {1120, 680, 0.5f, 1, 1}, {160, 680, 0.5f, 0, 1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) {
      uint32_t w;
      std::memcpy(&w, &v[i][c], 4);
      m.Put(at + 20 * i + 4 * c, w);
    }
}
static void PutRegs(FakeMem& m, uint32_t at, std::initializer_list<std::initializer_list<float>> rows) {
  uint32_t a = at;
  for (auto row : rows)
    for (float f : row) {
      uint32_t w;
      std::memcpy(&w, &f, 4);
      m.Put(a, w), a += 4;
    }
}
static Quad2D YuvRow(uint32_t seq) {
  Quad2D q;
  q.kind = kQkYuv, q.site = q2d::kSiteMovieYuv, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 20;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest | kQhPlanes;
  q.blend = kQbAlpha;
  const float v[4][5] = {{160, 600, 0.5f, 0, 0}, {1120, 600, 0.5f, 1, 0}, {1120, 680, 0.5f, 1, 1}, {160, 680, 0.5f, 0, 1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY;
  q.color[0] = q.color[1] = q.color[2] = 1.f, q.color[3] = 0.8f;
  ValidSC(q);
  return q;
}
static void SvyChecks() {
  using namespace q2d;
  {   // the tap: a T_SpriteYUV scope + its op-19 make ONE row of kind yuv, three plane keys, the flush stamp (and the +424 technique pointer is never a plane key)
    FakeMem m;
    Store store;
    Counters c;
    Scope s;
    const uint32_t obj = 0x70002000u;
    PutMovie(m, 0x70000000u);
    for (int p = 0; p < 3; ++p) m.Put(obj + kMoviePlaneOffsets[p], 0x4000u + p);
    m.Put(obj + 424, 0xDEADBEEFu);
    PutRegs(m, 0x70001000u, {{kScaleX, kScaleY, 1, 1}, {1.f, 1.f, 1.f, 0.8f}});
    PutRegs(m, 0x70001100u, {{0.f, 0.f, 0.f, 0.f}});
    store.NoteFlush(0x6666u);
    OpenScope(s, kQkYuv, obj);
    OnOp21(s, m, 0, 2, 0x70001000u, c);
    OnOp21(s, m, 28, 1, 0x70001100u, c);
    OnOp19(s, m, store, 12, kSiteMovieYuv, 13, 4, 20, 0x70000000u, c);
    CloseScope(s, store, 12, c);
    std::vector<Quad2D> rows;
    store.Drain(12, rows);
    std::vector<Store::Hole> holes;
    store.DrainHoles(12, holes);
    const bool one = rows.size() == 1 && holes.empty();
    Check(one && rows[0].kind == kQkYuv && rows[0].stride_bytes == 20 && rows[0].verts[1][0] == 1120.f && (rows[0].have & kQhPlanes), "SVY ROW", "the no-alpha movie sprite (site 0x82BC799C) makes a yuv row, not a hole");
    Check(one && rows[0].tex_key[0] == 0x4000u && rows[0].tex_key[1] == 0x4001u && rows[0].tex_key[2] == 0x4002u && rows[0].tex_key[3] == 0 && rows[0].plane_set == 0x6666u && (rows[0].have & kQhFlush), "SVY ROW",
          "three plane keys (+400 / +408 / +416; +424 = the technique is not read as a key) and the flush stamp");
    FakeMem m2;
    Store st2;
    Counters c2;
    Scope none;
    OnOp19(none, m2, st2, 12, kSiteMovieYuv, 13, 4, 20, 0x70000000u, c2);
    std::vector<Store::Hole> h2;
    st2.DrainHoles(12, h2);
    Check(h2.size() == 1, "SVY ROW", "(a draw with no scope open is still a counted hole: the scope is the leaf's)");
  }
  {   // the blocks: AlphaMix (0, 1, 1, 0); a projection row refused; the Alpha twin keeps (1, 0, 1, 0)
    Blocks b;
    const Refuse r = BlocksOf(YuvRow(1), b);
    Check(r == Refuse::None && b.program == Program::Movie && b.fs[16] == 0.f && b.fs[17] == 1.f && b.fs[18] == 1.f && b.fs[19] == 0.f && b.fs[3] == 0.8f, "SVY BLOCKS", "a yuv row draws the movie program with AlphaMix (0, 1, 1, 0): alpha = 1, then x Color");
    Quad2D al = YuvRow(2);
    al.kind = kQkYuvAlpha, al.site = kSiteMovie;
    Blocks ba;
    BlocksOf(al, ba);
    Check(ba.fs[16] == 1.f && ba.fs[17] == 0.f, "SVY BLOCKS", "the Alpha twin keeps AlphaMix (1, 0, 1, 0)");
    Quad2D pr = YuvRow(3);
    pr.use_proj = 1.f, pr.have |= kQhMatrix;
    Check(BlocksOf(pr, b) == Refuse::Projection, "SVY BLOCKS", "a matrix-placed yuv row is refused 'projection' (the movie program has no matrix variant), as for the Alpha row");
  }
  {   // the record: the row round-trips; Append over four slots draws it with the movie program
    SceneTables t;
    t.has_camera = true;
    t.quads2d = {YuvRow(1)};
    const std::vector<uint8_t> by = SerializeRecord(t);
    SceneTables back;
    std::string e;
    const bool ok = !by.empty() && ParseRecord(by.data(), by.size(), back, e) && back.quads2d.size() == 1 && back.quad2d_errors == 0 && std::memcmp(&back.quads2d[0], &t.quads2d[0], sizeof(Quad2D)) == 0;
    Check(ok && Quad2DRowProblem(back.quads2d[0]).empty(), "SVY ROW", "a yuv row round-trips through the record and passes the row check");
    rdr::FrameBuilder fb;
    fb.Begin(1);
    Programs programs;
    programs.rect = 11, programs.movie = 22, programs.layout = 3, programs.sprite = 33;
    Context ctx;
    ctx.width = 1920, ctx.height = 1080;
    ctx.plane = [](const Quad2D&, int s) {
      rdr::TextureSlot ts;
      ts.image = rdr::ImageId(700 + s);
      return ts;
    };
    const Report rep = Append(fb, programs, ctx, &back.quads2d[0], 1);
    const rdr::Frame frame = fb.Finish();
    Check(rep.drawn == 1 && rep.movie == 1 && frame.draws.count == 1 && frame.states[frame.draws[0].state].program == 22, "SVY ROW", "Append draws the yuv row with the movie program (the season video is no longer a refused / dropped row)");
  }
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
#ifdef RDR_Q2D_PLANTS
  if (plant == "address_ignored") q2d::g_plant = 61;
  else if (plant == "code7_wrap") q2d::g_plant = 62;
  else if (plant == "unknown_wrap") q2d::g_plant = 63;
  else if (plant == "yuv_site_hole") q2d::g_plant = 64;
  else
#endif
      if (!plant.empty()) {
    std::fprintf(stderr, "unknown plant %s\n", plant.c_str());
    return 2;
  }
  TapChecks();
  RecordChecks();
  ModeChecks();
  DrawChecks();
  SvyChecks();
  if (!plant.empty()) {
    const char* want = plant == "address_ignored" ? "[RFX TILE]" : plant == "code7_wrap" ? "[RFX CODE7]" : plant == "yuv_site_hole" ? "[SVY ROW]" : "[RFX UNKNOWN]";
    const bool caught = g_failed.find(want) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? (std::string("caught by its named check ") + want).c_str() : (std::string("its named check ") + want + " did NOT fail").c_str(), g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: rfx_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
