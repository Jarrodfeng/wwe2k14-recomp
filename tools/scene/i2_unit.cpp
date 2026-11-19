// Lane I2 (Session 103, landing 2, docs\renderer\REMNANT_MAP.md section 10): the offline unit proof of the 2D layer's SPRITE rows and their pictures. 0 game runs, no GPU, no game file.
//
//   i2_unit                 every check; exit 0 only when all pass
//   i2_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (plants: src\renderer_scene\quads2d_plants.h behind -DRDR_Q2D_PLANTS, and guest_tex::g_tex_plant for pic_no_swap)
//
// Checks (tag in brackets; a plant names the tag that must fail):
//   [SPRITE RECORD]   layout 5 (324 bytes a row; was 2 / 212 bytes): a sprite row with a hue shift and a picture key round-trips byte-equal; a NaN hue-shift constant refuses THIS row only; the payload's unused z / w do not
//   [SPRITE TAP]      the op-19 / op-21 / op-28 taps, over a fixture guest memory, build a sprite row: four vertices with uv, c0 / c1 / c28, the picture key (object + 400, word 0), the hue-shift triple (op 28, reg 2 x 1); a blur /
//                     gradation upload marks the row a variant; the leaf's other draw site and an op 28 outside a sprite scope make no row / no mark
//   [SPRITE BLOCKS]   BlocksOf on a plain and a hue-shifted sprite equals an independent transcription of rdr_uiquad.vert / .frag / rdr_uiquad_hsl.frag's layout (corners with uv, colour, AlphaMix, HSL); variants and non-finite refused
//   [SPRITE HSL]      the hue-shifted row picks the hue program and carries the triple (plant sprite_no_hsl)
//   [SPRITE DRAW]     Append: the sprite and hue programs, slot 0 = the row's picture (from the resolver, by KEY), blend and alpha test from the row; a row with no picture / an unknown blend / a variant is counted by reason
//   [PICTURE KEY]     key -> file picture (the content join), no key / unknown key refused and counted (plant pic_key_shift)
//   [PICTURE GUEST]   the texture OBJECT in guest memory (handle -> registry -> wrapper -> header -> fetch -> tiled, byte-swapped BC1 blocks) read to the exact blocks; cached, re-read when the object changed; an uncompressed /
//                     stale / unknown handle is a counted refusal, never a picture (plant pic_no_swap)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"   // S103 SC
#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/quads2d_pictures.h"
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
  void PutF(uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    Put(a, v);
  }
  bool ReadWords(uint32_t a, uint32_t* out, uint32_t n) const {
    if (a < 0x10000u || uint64_t(a) + 4ull * n > 0x7F000000ull) return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = words.find(a + 4 * i);
      out[i] = it == words.end() ? 0u : it->second;
    }
    return true;
  }
};
// guest_tex's memory contract: u32(addr) host value of the big-endian word, mutable fail
struct GMem {
  const FakeMem* m;
  mutable bool fail = false;
  uint32_t u32(uint32_t a) const {
    uint32_t v = 0;
    auto it = m->words.find(a);   // (the registry global sits at 0x8392DD9C: the tap's MMIO rule is not this fixture's)
    if (a < 0x10000u) fail = true;
    else if (it != m->words.end()) v = it->second;
    return v;
  }
};

static const float kScaleX = 2.0f / 1280.0f, kScaleY = 2.0f / 720.0f;
namespace particles = rdr_scene::live::particles;
static const uint32_t kObject = 0x40001000u, kVerts = 0x40002000u, kPayload = 0x40003000u, kKey = 0x00050007u;

static void PutSpriteVerts(FakeMem& m, uint32_t at, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z = 0.5f) {
  const float v[4][5] = {{x0, y0, z, u0, v0}, {x1, y0, z, u1, v0}, {x1, y1, z, u1, v1}, {x0, y1, z, u0, v1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) m.PutF(at + 20 * i + 4 * c, v[i][c]);
}

static Quad2D GoodSprite(uint32_t seq, bool hsl) {
  Quad2D q;
  q.kind = kQkSprite, q.site = q2d::kSiteSprite, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 20;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest | kQhPlanes;
  q.blend = kQbAlpha;
  const float v[4][5] = {{100, 50, 0.5f, 0.25f, 0.125f}, {356, 50, 0.5f, 0.75f, 0.125f}, {356, 114, 0.5f, 0.75f, 0.5f}, {100, 114, 0.5f, 0.25f, 0.5f}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY, q.pos_scale[2] = NAN, q.pos_scale[3] = NAN;   // the leaf uploads two floats; z / w are its stack
  q.color[0] = 1.f, q.color[1] = 0.5f, q.color[2] = 0.25f, q.color[3] = 0.75f;
  q.tex_key[0] = kKey;
  if (hsl) q.have |= kQhHsl, q.hsl[0] = 0.25f, q.hsl[1] = 0.1f, q.hsl[2] = -0.2f;
  ValidSC(q);
  return q;
}
static bool Same(const Quad2D& a, const Quad2D& b) { return std::memcmp(&a, &b, sizeof(Quad2D)) == 0; }

// ---- [SPRITE RECORD] -----------------------------------------------------------------------------------------------------------------------------
static void RecordChecks() {
  SceneTables t;
  t.has_camera = true;
  t.quads2d = {GoodSprite(1, true), GoodSprite(2, false)};
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(sizeof(Quad2D) == 324 && kQuad2DLayout == 5 && ok && back.quads2d.size() == 2 && Same(back.quads2d[0], t.quads2d[0]) && Same(back.quads2d[1], t.quads2d[1]) && back.quad2d_errors == 0, "SPRITE RECORD",
        "layout 5 (324 bytes a row; was 2 / 212 bytes, 3 / S103 P3c): a hue-shifted and a plain sprite row round-trip byte-equal (picture key, hue triple, uv, a NaN z / w of c0 included)");
  SceneTables bad = t;
  bad.quads2d[0].hsl[1] = INFINITY;
  const std::vector<uint8_t> b2 = SerializeRecord(bad);
  SceneTables back2;
  const bool ok2 = ParseRecord(b2.data(), b2.size(), back2, err);
  Check(ok2 && back2.quads2d.size() == 1 && Same(back2.quads2d[0], t.quads2d[1]) && back2.quad2d_errors == 1, "SPRITE RECORD", "a hue-shifted row with a non-finite hue constant is refused alone (counted); the plain row and the record are read");
  SceneTables v1 = t;
  v1.quads2d[1].version = 1;
  const std::vector<uint8_t> b3 = SerializeRecord(v1);
  SceneTables back3;
  Check(ParseRecord(b3.data(), b3.size(), back3, err) && back3.quads2d.size() == 1 && back3.quad2d_errors == 1, "SPRITE RECORD", "a row of layout 1 (the old 196-byte meaning) is refused alone");
}

// ---- [SPRITE TAP] --------------------------------------------------------------------------------------------------------------------------------
static void TapChecks() {
  using namespace q2d;
  FakeMem m;
  m.Put(kObject + kSpriteKeyOffset, kKey);
  PutSpriteVerts(m, kVerts, 100, 50, 356, 114, 0.25f, 0.125f, 0.75f, 0.5f);
  // c0 (two floats + the stack), c1 colour, c28 flag; then the hue triple (op 28, reg 2 x 1)
  m.PutF(kPayload, kScaleX), m.PutF(kPayload + 4, kScaleY), m.Put(kPayload + 8, 0x7FC00000u), m.Put(kPayload + 12, 0x7FC00000u);
  m.PutF(kPayload + 16, 1.f), m.PutF(kPayload + 20, 0.5f), m.PutF(kPayload + 24, 0.25f), m.PutF(kPayload + 28, 0.75f);
  m.PutF(kPayload + 32, 0.f), m.PutF(kPayload + 36, 0.f), m.PutF(kPayload + 40, 0.f), m.PutF(kPayload + 44, 0.f);
  m.PutF(kPayload + 64, 0.25f), m.PutF(kPayload + 68, 0.1f), m.PutF(kPayload + 72, -0.2f);   // (the 16-byte unit's w is not populated: the tap must not need it)
  Store store;
  Counters c;
  Scope s;
  const uint32_t F = 9;
  struct M2 {
    const FakeMem& m;
    bool ReadWords(uint32_t a, uint32_t* o, uint32_t n) const { return m.ReadWords(a, o, n); }
  } mem{m};
  // a hue-shifted sprite: c0, c1, c28, hue triple, then the draw
  OpenScope(s, kQkSprite, kObject);
  OnOp21(s, mem, 0, 1, kPayload, c);
  OnOp21(s, mem, 1, 1, kPayload + 16, c);
  OnOp21(s, mem, 28, 1, kPayload + 32, c);
  OnOp28(s, mem, kHslReg, 1, kPayload + 64, c);
  OnOp19(s, mem, store, F, kSiteSprite, 13, 4, 20, kVerts, c);
  CloseScope(s, store, F, c);
  std::vector<Quad2D> rows;
  store.Drain(F, rows);
  const bool one = rows.size() == 1;
  const Quad2D r = one ? rows[0] : Quad2D{};
  Check(one && r.kind == kQkSprite && r.site == kSiteSprite && r.object == kObject && r.nverts == 4 && r.stride_bytes == 20 && r.verts[0][0] == 100 && r.verts[0][3] == 0.25f && r.verts[0][4] == 0.125f &&
            r.verts[2][0] == 356 && r.verts[2][1] == 114 && r.verts[2][3] == 0.75f && r.verts[2][4] == 0.5f && r.verts[3][0] == 100 && r.verts[3][4] == 0.5f,
        "SPRITE TAP", "four vertices with their uv, exactly as the guest wrote them (stride 20)");
  Check(one && r.tex_key[0] == kKey && (r.have & kQhPlanes), "SPRITE TAP", "the picture key is the word at object + 400");
  Check(one && r.pos_scale[0] == kScaleX && r.pos_scale[1] == kScaleY && r.color[0] == 1.f && r.color[1] == 0.5f && r.color[3] == 0.75f && r.use_proj == 0.f && (r.have & (kQhPosScale | kQhColour | kQhProj)) == (kQhPosScale | kQhColour | kQhProj),
        "SPRITE TAP", "c0, c1, c28 are read");
  Check(one && (r.have & kQhHsl) && r.hsl[0] == 0.25f && r.hsl[1] == 0.1f && r.hsl[2] == -0.2f && !(r.have & kQhVariant) && c.hsl_rows == 1, "SPRITE TAP", "the hue-shift triple (op 28, reg 2 x 1) is read: hue 0.25, chroma 0.1, lightness -0.2");
  // a plain sprite: no op 28
  OpenScope(s, kQkSprite, kObject);
  OnOp21(s, mem, 0, 1, kPayload, c);
  OnOp21(s, mem, 1, 1, kPayload + 16, c);
  OnOp21(s, mem, 28, 1, kPayload + 32, c);
  OnOp19(s, mem, store, F, kSiteSprite, 13, 4, 20, kVerts, c);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Check(rows.size() == 1 && !(rows[0].have & (kQhHsl | kQhVariant)) && rows[0].hsl[0] == 0.f, "SPRITE TAP", "a sprite with no op 28 carries no hue shift (the plain program)");
  // a blur / gradation sprite: op 28 reg 3 x 8
  OpenScope(s, kQkSprite, kObject);
  OnOp21(s, mem, 0, 1, kPayload, c);
  OnOp21(s, mem, 1, 1, kPayload + 16, c);
  OnOp21(s, mem, 28, 1, kPayload + 32, c);
  OnOp28(s, mem, kVariantReg, 8, kPayload, c);
  OnOp19(s, mem, store, F, kSiteSprite, 13, 4, 20, kVerts, c);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Check(rows.size() == 1 && (rows[0].have & kQhVariant) && c.variant_rows == 1, "SPRITE TAP", "an op 28 of c3 x 8 (blur / gradation offsets) marks the row a variant");
  // the leaf's other draw site, and an op 28 outside a sprite scope
  const uint64_t ignored = c.ignored_site;
  OpenScope(s, kQkSprite, kObject);
  OnOp19(s, mem, store, F, 0x82BC5408u, 13, 4, 20, kVerts, c);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Scope rect;
  OpenScope(rect, kQkRect, kObject);
  const uint64_t seen = c.op28_in_scope;
  OnOp28(rect, mem, kHslReg, 1, kPayload + 64, c);
  OnOp28(s, mem, kHslReg, 1, kPayload + 64, c);   // (closed scope)
  Check(rows.empty() && c.ignored_site == ignored + 1 && c.op28_in_scope == seen && !(rect.have & kQhHsl), "SPRITE TAP",
        "the sprite leaf's other draw call (0x82BC5408) makes no row (counted); an op 28 in a rect scope or after the scope closed marks nothing");
}

// ---- [SPRITE BLOCKS] / [SPRITE HSL] -------------------------------------------------------------------------------------------------------------
static void BlockChecks() {
  using namespace q2d;
  const Quad2D plain = GoodSprite(1, false), hue = GoodSprite(2, true);
  Blocks b, h;
  const Refuse rp = BlocksOf(plain, b), rh = BlocksOf(hue, h);
  // written out from the shaders: UnitScale (2/1280, 2/720, 0, 0.5), Pos[i] = (x, y, z, 0), Uv[i] = (u, v, 0, 0) from float 20; Color, AlphaMix (1, 0, 1, 0), HSL
  float vs[36] = {}, fs[12] = {};
  vs[0] = kScaleX, vs[1] = kScaleY, vs[3] = 0.5f;
  const float uv[4][2] = {{0.25f, 0.125f}, {0.75f, 0.125f}, {0.75f, 0.5f}, {0.25f, 0.5f}};
  const float pos[4][3] = {{100, 50, 0.5f}, {356, 50, 0.5f}, {356, 114, 0.5f}, {100, 114, 0.5f}};
  for (int i = 0; i < 4; ++i) {
    for (int c = 0; c < 3; ++c) vs[4 + 4 * i + c] = pos[i][c];
    vs[20 + 4 * i] = uv[i][0], vs[20 + 4 * i + 1] = uv[i][1];
  }
  fs[0] = 1.f, fs[1] = 0.5f, fs[2] = 0.25f, fs[3] = 0.75f, fs[4] = 1.f, fs[5] = 0.f, fs[6] = 1.f, fs[7] = 0.f;
  Check(rp == Refuse::None && b.program == Program::Sprite && b.vs_bytes == 144 && b.fs_bytes == 32 && std::memcmp(b.vs, vs, 144) == 0 && std::memcmp(b.fs, fs, 32) == 0, "SPRITE BLOCKS",
        "a plain sprite: UnitScale + corners + uv (the Uv block at float 20), Color, AlphaMix - equal to the written-out layout; a NaN z / w of c0 does not matter");
  fs[8] = 0.25f, fs[9] = 0.1f, fs[10] = -0.2f;
  Check(rh == Refuse::None && h.program == Program::SpriteHsl && h.vs_bytes == 144 && h.fs_bytes == 48 && std::memcmp(h.vs, vs, 144) == 0 && std::memcmp(h.fs, fs, 48) == 0, "SPRITE HSL",
        "a hue-shifted sprite: the hue program, Color, AlphaMix, then HSL (0.25, 0.1, -0.2) - equal to the written-out layout");
  Quad2D variant = GoodSprite(3, false);
  variant.have |= kQhVariant;
  Quad2D inf = GoodSprite(4, true);
  inf.hsl[2] = NAN;
  Quad2D nocol = GoodSprite(5, false);
  nocol.have &= ~uint32_t(kQhColour);
  Quad2D proj = GoodSprite(6, false);
  proj.use_proj = 1.f;
  Blocks x;
  Check(BlocksOf(variant, x) == Refuse::Variant && BlocksOf(inf, x) == Refuse::NotFinite && BlocksOf(nocol, x) == Refuse::MissingConstants && BlocksOf(proj, x) == Refuse::Projection, "SPRITE BLOCKS",
        "a blur / gradation sprite, a non-finite hue constant, a missing colour and a projection-matrix sprite are each refused by its own reason");
}

// ---- [SPRITE DRAW] -----------------------------------------------------------------------------------------------------------------------------
static void DrawChecks() {
  using namespace q2d;
  rdr::FrameBuilder fb;
  fb.Begin(1);
  Programs programs;
  programs.rect = 11, programs.movie = 22, programs.layout = 3, programs.sprite = 33, programs.sprite_hsl = 44;
  Context ctx;
  ctx.width = 1920, ctx.height = 1080;
  ctx.picture = [](const Quad2D& row) {
    rdr::TextureSlot t;
    t.image = row.tex_key[0] == kKey ? rdr::ImageId(900) : row.tex_key[0] == kKey + 1 ? rdr::ImageId(901) : rdr::kNoImage;   // the resolver's mapping, by KEY
    return t;
  };
  Quad2D a = GoodSprite(1, false);
  Quad2D b = GoodSprite(2, true);  b.tex_key[0] = kKey + 1;
  Quad2D c = GoodSprite(3, false);  c.blend = kQbAdd;  c.have |= kQhAlphaTest;  c.alpha_func = 6;  c.alpha_ref = 1.f / 255.f;
  Quad2D nopic = GoodSprite(4, false);  nopic.tex_key[0] = 0x777;
  Quad2D unknown = GoodSprite(5, false);  unknown.blend = kQbUnknown;
  Quad2D variant = GoodSprite(6, false);  variant.have |= kQhVariant;
  std::vector<Quad2D> rows = {a, b, c, nopic, unknown, variant};
  const Report rep = Append(fb, programs, ctx, rows.data(), rows.size());
  const rdr::Frame frame = fb.Finish();
  Check(rep.drawn == 3 && rep.sprite == 2 && rep.sprite_hsl == 1 && rep.refused[uint8_t(Refuse::Picture)] == 1 && rep.refused[uint8_t(Refuse::Blend)] == 1 && rep.refused[uint8_t(Refuse::Variant)] == 1 && frame.draws.count == 3, "SPRITE DRAW",
        "six sprite rows: three draws (two plain, one hue-shifted); the unknown picture key, the unknown blend and the blur / gradation row are refused, each by its own reason");
  if (frame.draws.count == 3) {
    const rdr::DrawState& s0 = frame.states[frame.draws[0].state];
    const rdr::DrawState& s1 = frame.states[frame.draws[1].state];
    const rdr::DrawState& s2 = frame.states[frame.draws[2].state];
    Check(s0.program == 33 && s1.program == 44 && s2.program == 33 && s0.blend[0].enable == 1 && s0.blend[0].src_color == 6 && s0.blend[0].dst_color == 7 && s2.blend[0].dst_color == 1 && s0.depth_test == 0, "SPRITE DRAW",
          "plain = program 33, hue-shifted = program 44, blend from the row (alpha, additive), no depth");
    const rdr::TextureSet& t0 = frame.texture_sets[frame.draws[0].textures];
    const rdr::TextureSet& t1 = frame.texture_sets[frame.draws[1].textures];
    Check(t0.slots[0].image == 900 && t1.slots[0].image == 901 && t0.slots[1].image == rdr::kNoImage, "SPRITE DRAW", "slot 0 is the picture the row's KEY resolves to (900 / 901), not anything the draw carried");
    const rdr::DynamicState& y2 = frame.dynamics[frame.draws[2].dynamic];
    Check(y2.system[8] == 1.f / 255.f && y2.system[9] == 6.f && frame.dynamics[frame.draws[0].dynamic].system[9] == 7.f, "SPRITE DRAW", "the alpha test is the row's (greater-or-equal 1/255) or 'always'");
    Blocks want;
    BlocksOf(b, want);
    const rdr::DrawOp& d1 = frame.draws[1];
    Check(std::memcmp(frame.constants.data + d1.vs_constants, want.vs, want.vs_bytes) == 0 && std::memcmp(frame.constants.data + d1.fs_constants, want.fs, want.fs_bytes) == 0, "SPRITE DRAW", "the constants pushed for the hue-shifted draw are the row's blocks, hue triple included, byte for byte");
  } else {
    Check(false, "SPRITE DRAW", "three draws were made");
  }
  rdr::FrameBuilder fb2;
  fb2.Begin(1);
  Context none = ctx;
  none.picture = nullptr;
  const Report r2 = Append(fb2, programs, none, rows.data(), 1);
  Check(r2.drawn == 0 && r2.refused[uint8_t(Refuse::Picture)] == 1, "SPRITE DRAW", "no picture resolver at all: the row is refused as 'picture', never drawn bare");
}

// ---- [PICTURE KEY] ----------------------------------------------------------------------------------------------------------------------------------
// ---- [PICTURE GUEST] --------------------------------------------------------------------------------------------------------------------------------
namespace gt = rdr_scene::live::guest_tex;
static const uint32_t kMgr = 0x40100000u, kEntries = 0x40101000u, kWrapper = 0x40102000u, kHeader = 0x40103000u, kPixels = 0x00200000u;   // (pixel memory: a PHYSICAL address)

static void PutRegistry(FakeMem& m, uint32_t key, uint32_t wrapper, uint32_t stored_id) {
  m.Put(particles::kTextureHandleGlobal, kMgr);
  m.Put(kMgr + 8, kEntries);
  m.Put(kMgr + 16, kEntries + 8 * 0x10000u);
  const uint32_t idx = key & 0xFFFFu;
  m.Put(kEntries + 8 * idx, stored_id);
  m.Put(kEntries + 8 * idx + 4, wrapper);
}
// The six fetch dwords of a tiled 16 x 16 BC1 picture (4 x 4 blocks), pitch 32 texels, endian 2 (8 in 32), base `phys`.
static void PutObject(FakeMem& m, uint32_t wrapper, uint32_t header, uint32_t format, uint32_t phys, uint32_t endian = 2) {
  m.Put(wrapper, particles::kWrapperVtableA);
  m.Put(wrapper + particles::kWrapperKind, 0);
  m.Put(wrapper + particles::kWrapperTexture, header);
  const uint32_t f0 = (1u << 31) | (1u << 22);          // tiled, pitch / 32 = 1
  const uint32_t f1 = format | (endian << 6) | (wwe2k14::guest_view::ToEViewCpu(phys) & 0xFFFFF000u);   // S103 TX: the header holds the CPU E-view word
  const uint32_t f2 = 15u | (15u << 13);                 // 16 x 16
  const uint32_t f5 = 1u << 9;                           // dimension 2D
  m.Put(header + particles::kHeaderFetch + 0, f0), m.Put(header + particles::kHeaderFetch + 4, f1), m.Put(header + particles::kHeaderFetch + 8, f2);
  m.Put(header + particles::kHeaderFetch + 12, 0), m.Put(header + particles::kHeaderFetch + 16, 0), m.Put(header + particles::kHeaderFetch + 20, f5);
}

static void PictureChecks() {
  using namespace q2d;
  FakeMem m;
  // the expected mip 0: 4 x 4 blocks of 8 bytes, a recognisable pattern
  std::vector<uint8_t> expect(16 * 8);
  for (size_t i = 0; i < expect.size(); ++i) expect[i] = uint8_t(i * 7 + 3);
  // the guest's memory: each block tiled and byte-swapped the way the game stores it (SwapBlock is its own inverse for the 8-in-32 order)
  std::map<uint32_t, uint8_t> pix;
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      uint8_t blk[8];
      std::memcpy(blk, &expect[(y * 4 + x) * 8], 8);
      const int saved_plant = gt::g_tex_plant;   // (the picture the GAME stored is built with the true swap, whatever plant is on)
      gt::g_tex_plant = 0;
      gt::SwapBlock(blk, 8, 2);
      gt::g_tex_plant = saved_plant;
      const uint32_t off = gt::TiledOffset2D(int32_t(x), int32_t(y), 32, 3);
      for (int k = 0; k < 8; ++k) pix[kPixels + off + k] = blk[k];
    }
  const auto phys = [&](uint32_t addr, uint32_t n, uint8_t* dst) {
    for (uint32_t i = 0; i < n; ++i) {
      auto it = pix.find(addr + i);
      dst[i] = it == pix.end() ? 0 : it->second;
    }
    return true;
  };
  const uint32_t keyA = 0x00040003u, keyB = 0x00040004u, keyU = 0x00040005u, keyC = 0x00040006u;
  PutRegistry(m, keyA, kWrapper, keyA);
  PutObject(m, kWrapper, kHeader, 18, kPixels);   // BC1
  m.Put(particles::kTextureHandleGlobal, kMgr);
  // an uncompressed texture (format 6: 8888 — a surface the GPU may draw into): refused
  m.Put(kEntries + 8 * (keyU & 0xFFFF), keyU), m.Put(kEntries + 8 * (keyU & 0xFFFF) + 4, kWrapper + 0x400);
  PutObject(m, kWrapper + 0x400, kHeader + 0x400, 6, kPixels);
  // a stale entry (the stored id is not the handle)
  m.Put(kEntries + 8 * (keyC & 0xFFFF), keyC + 1), m.Put(kEntries + 8 * (keyC & 0xFFFF) + 4, kWrapper);
  GMem gm{&m};
  PictureTable table;
  table.JoinFile(keyB, FileRef{77, 256, 64});
  table.JoinFile(keyB + 10, FileRef{88, 128, 128});
  Picture p;
  Check(table.Resolve(keyB, gm, phys, p) == PicWhy::Ok && p.source == PicSource::File && p.file.entry == 77 && p.file.width == 256, "PICTURE KEY", "a key the content join knows resolves to ITS file picture (entry 77), not to any other joined key's");
  Check(table.Resolve(0, gm, phys, p) == PicWhy::NoKey && table.Resolve(0x00090009u, gm, phys, p) == PicWhy::GuestRefused && table.counters().guest_why[uint8_t(gt::Why::NoHandle)] == 1 && table.counters().refused[uint8_t(PicWhy::NoKey)] == 1, "PICTURE KEY",
        "no key and a key nothing knows (not in the registry) are counted refusals, never a picture");
  const PicWhy w = table.Resolve(keyA, gm, phys, p);
  if (w != PicWhy::Ok) { std::printf("  (guest read refused: ");
    for (int i = 0; i < 9; ++i) std::printf("%s %llu; ", gt::WhyText(gt::Why(i)), (unsigned long long)table.counters().guest_why[i]);
    std::printf(")\n"); }
  Check(w == PicWhy::Ok && p.source == PicSource::Guest && p.blocks && *p.blocks == expect && p.shape.width == 16 && p.shape.height == 16 && p.shape.game_format == 18, "PICTURE GUEST",
        "a BC1 texture object in guest memory (registry -> wrapper -> header -> fetch, tiled, byte-swapped) reads to the exact 16 blocks");
  const uint64_t reads = table.counters().guest_read;
  const bool again = table.Resolve(keyA, gm, phys, p) == PicWhy::Ok && p.blocks && *p.blocks == expect;
  Check(again && table.counters().guest_read == reads && table.counters().changed == 0, "PICTURE GUEST", "the same object asked again is answered from the cache (no second read)");
  const uint64_t hash0 = p.hash;
  m.Put(kHeader + particles::kHeaderFetch + 4, 18u | (2u << 6) | (wwe2k14::guest_view::ToEViewCpu(kPixels + 0x1000) & 0xFFFFF000u));   // the refill that keeps the key: another base
  const PicWhy w2 = table.Resolve(keyA, gm, phys, p);
  Check(w2 == PicWhy::Ok && table.counters().changed == 1 && table.counters().guest_read == reads + 1 && p.hash != hash0, "PICTURE GUEST", "a key whose texture object changed is read again (counted as changed), not served stale");
  // F2: a refill IN PLACE (same address, same size, every fetch dword unchanged): new bytes must be read, not the cached ones
  m.Put(kHeader + particles::kHeaderFetch + 4, 18u | (2u << 6) | (wwe2k14::guest_view::ToEViewCpu(kPixels) & 0xFFFFF000u));   // back to the first base
  table.Resolve(keyA, gm, phys, p);
  const uint64_t ch0 = table.counters().changed;
  for (auto& kv : pix) kv.second ^= 0x5A;   // the game wrote another picture over the same memory
  const PicWhy w3 = table.Resolve(keyA, gm, phys, p);
  std::vector<uint8_t> expect2 = expect;
  for (size_t i = 0; i < expect2.size(); ++i) expect2[i] ^= 0x5A;
  Check(w3 == PicWhy::Ok && p.blocks && *p.blocks == expect2 && table.counters().changed == ch0 + 1, "PICTURE GUEST", "a refill in place (same address and size, fetch dwords unchanged) is re-read from the new bytes, counted as changed - never the cached old picture");
  Check(table.Resolve(keyU, gm, phys, p) == PicWhy::GuestRefused && table.counters().guest_why[uint8_t(gt::Why::Format)] == 1 && table.Resolve(keyC, gm, phys, p) == PicWhy::GuestRefused && table.counters().guest_why[uint8_t(gt::Why::NoHandle)] == 2,
        "PICTURE GUEST", "an uncompressed texture (a surface the GPU draws into) and a stale registry entry are refused and counted, never read as a picture");
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
#ifdef RDR_Q2D_PLANTS
  if (plant == "sprite_no_hsl") q2d::g_plant = 11;
  else if (plant == "pic_key_shift") q2d::g_plant = 12;
  else if (plant == "pic_no_swap") rdr_scene::live::guest_tex::g_tex_plant = 3;
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
  RecordChecks();
  TapChecks();
  BlockChecks();
  DrawChecks();
  PictureChecks();
  if (!plant.empty()) {
    const char* want = plant == "sprite_no_hsl" ? "[SPRITE HSL]" : plant == "pic_key_shift" ? "[PICTURE KEY]" : "[PICTURE GUEST]";
    const bool caught = g_failed.find(want) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? (std::string("caught by its named check ") + want).c_str() : (std::string("its named check ") + want + " did NOT fail").c_str(), g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: i2_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
