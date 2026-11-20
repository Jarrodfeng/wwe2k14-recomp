// body-scale: none (unit checks of the see-through order and tables, no wrestler model is built)  [S103 RGF: body_scale_feed_lint.py]
// Lane F's offline test (Session 103): the see-through order, the particle tables in the record, the contributor, and the live particle hook drawn through the
// scene front-end with a headless renderer. 0 game runs. Exit 0 = every check passed; a planted fault must make it exit 1.
//   f_test [--fault order_distance|wrong_frame|additive_off|stale_snapshot|bad_index] [--sheet OUT.png] [--no-gpu]
// The picture check compares what the hook drew with a CPU model of the same quads (blend maths in float, 8-bit compare, tolerance 3 levels); it is the proof
// that the hook honours the tables, not that the tables match the game (that is the main session's mirror run, SEE_THROUGH_ORDER.md section 7).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_particles.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/see_through_order.h"
#include "../../src/scene_live/particle_contrib.h"
#include "../../src/scene_live/particle_reader.h"
#include "../../src/scene_live/particle_namer.h"
#include "../../src/scene_live/particle_source.h"

using namespace rdr_scene;

namespace {

int g_fail = 0, g_checks = 0;
void Check(bool ok, const char* name, const std::string& detail = std::string()) {
  ++g_checks;
  if (!ok) ++g_fail;
  std::printf("[f_test] %s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
}

// ---- PNG (stored deflate, for the sheet) ----------------------------------------------------------------------------------------
uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 255] ^ (crc >> 8);
  return ~crc;
}
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }
void Chunk(std::vector<uint8_t>& out, const char* kind, const std::vector<uint8_t>& body) {
  Put32(out, uint32_t(body.size()));
  std::vector<uint8_t> kb(kind, kind + 4);
  kb.insert(kb.end(), body.begin(), body.end());
  out.insert(out.end(), kb.begin(), kb.end());
  Put32(out, Crc32(kb.data(), kb.size()));
}
bool WritePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) raw.push_back(rgba[(size_t(y) * w + x) * 4 + size_t(c)]);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t((~n) >> 8));
    z.insert(z.end(), raw.begin() + long(i), raw.begin() + long(i + n));
  }
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  Put32(z, (b << 16) | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, w), Put32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  Chunk(png, "IHDR", ihdr), Chunk(png, "IDAT", z), Chunk(png, "IEND", {});
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  return true;
}

// ---- 1. the see-through order ---------------------------------------------------------------------------------------------------
struct It { int id; uint8_t blend; float dist2; bool character; };

void TestOrder(bool fault_distance) {
  // A record-order list: alpha and additive interleaved, distances not in order (so a far-to-near sort moves things).
  const std::vector<It> in = {{0, 1, 4, false}, {1, 2, 100, false}, {2, 1, 50, true}, {3, 2, 9, false}, {4, 1, 70, false}, {5, 1, 70, true}, {6, 2, 1, true}, {7, 1, 3, false}};
  const auto key_of = [](const It& i) { SeeKey k; k.blend = i.blend, k.character = i.character, k.dist2 = i.dist2; return k; };
  std::vector<It> game = in;
  SortSeeThrough(fault_distance ? SeeOrder::Distance : SeeOrder::Game, game, key_of);
  std::vector<int> want = {0, 4, 7, 2, 5, 1, 3, 6};  // alpha (stage) in record order, alpha (characters), then additive
  std::vector<int> got;
  for (const It& i : game) got.push_back(i.id);
  Check(got == want, "order: game mode = stage alpha, character alpha, then additive, each in submit order");
  std::vector<int> sorted_ids = got;
  std::sort(sorted_ids.begin(), sorted_ids.end());
  bool perm = sorted_ids.size() == in.size();
  for (size_t i = 0; i < sorted_ids.size() && perm; ++i) perm = sorted_ids[i] == int(i);
  Check(perm, "order: every item once (none drawn twice, none missing)");
  std::vector<It> dist = in;
  SortSeeThrough(SeeOrder::Distance, dist, key_of);
  std::vector<It> ref = in;
  std::stable_sort(ref.begin(), ref.end(), [](const It& a, const It& b) { return a.dist2 > b.dist2; });
  bool same = true;
  for (size_t i = 0; i < in.size(); ++i) same = same && dist[i].id == ref[i].id;
  Check(same, "order: distance mode is the old far-to-near stable sort (off = today)");
  // Stable across a camera move: the game order must not change when only the distances change.
  std::vector<It> moved = in;
  for (It& i : moved) i.dist2 = 1000.f - i.dist2;
  SortSeeThrough(SeeOrder::Game, moved, key_of);
  std::vector<int> got2;
  for (const It& i : moved) got2.push_back(i.id);
  Check(fault_distance || got2 == want, "order: game mode does not move with the camera");
  // The loader's own slot blends (cutout 2) stay far to near, after the game's pieces of their rank.
  struct Cu { int id; float dist2; uint8_t cutout; };
  std::vector<Cu> cu = {{0, 5, 2}, {1, 9, 0}, {2, 20, 2}, {3, 1, 0}};
  SortSeeThrough(SeeOrder::Game, cu, [](const Cu& c) { SeeKey k; k.dist2 = c.dist2, k.cutout = c.cutout; return k; });
  Check(cu[0].id == 1 && cu[1].id == 3 && cu[2].id == 2 && cu[3].id == 0, "order: slot blends (cutout 2) stay far to near, after the game's own pieces");
  // S105 SZW (SZW_contract.md site 12; ZW1): with an order key (wwe2k14_rdr_stage_zwrite) the slot blends draw in the game's list order = ascending slot number inside light-set runs, never by distance.
  // key = run * 1024 + slot: slots 45, 120, 130 are one run (set A), slot 46 the next run (set B), slot 214 the last (set C): 45, 120, 130, 46, 214 whatever the distances. A piece of the game's own at the same rank stays first.
  struct So { int id; float dist2; uint8_t cutout; int32_t order; };
  std::vector<So> so = {{0, 900, 2, 2 * 1024 + 214}, {1, 100, 2, 0 * 1024 + 120}, {2, 4, 0, -1}, {3, 400, 2, 1 * 1024 + 46}, {4, 9, 2, 0 * 1024 + 45}, {5, 2500, 2, 0 * 1024 + 130}};
  SortSeeThrough(fault_distance ? SeeOrder::Distance : SeeOrder::Game, so, [](const So& c) { SeeKey k; k.dist2 = c.dist2, k.cutout = c.cutout, k.order = c.order; return k; });
  std::vector<int> so_ids;
  for (const So& c : so) so_ids.push_back(c.id);
  Check(so_ids == std::vector<int>({2, 4, 1, 5, 3, 0}), "order: slot blends with an order key draw in the game's list order (slot ascending inside light-set runs), not far to near");
  std::vector<So> so2 = {{0, 1, 2, 2 * 1024 + 214}, {1, 9, 2, 0 * 1024 + 120}, {2, 4, 0, -1}, {3, 4, 2, 1 * 1024 + 46}, {4, 99, 2, 0 * 1024 + 45}, {5, 25, 2, 0 * 1024 + 130}};   // the camera moved: the same order
  SortSeeThrough(fault_distance ? SeeOrder::Distance : SeeOrder::Game, so2, [](const So& c) { SeeKey k; k.dist2 = c.dist2, k.cutout = c.cutout, k.order = c.order; return k; });
  std::vector<int> so2_ids;
  for (const So& c : so2) so2_ids.push_back(c.id);
  Check(so2_ids == std::vector<int>({2, 4, 1, 5, 3, 0}), "order: the game's slot order does not move with the camera");
}

// ---- 2. the record ----------------------------------------------------------------------------------------------------------------
Particle MakeParticle(float cx, float cy, float half, uint32_t colour) {
  Particle p{};
  // LocalWorld rows c228-c230: world = (half * x + cx, half * y + cy, 0.5), as the effect's dot(row, (p, 1)).
  p.local_world[0] = half, p.local_world[3] = cx;
  p.local_world[5] = half, p.local_world[7] = cy;
  p.local_world[11] = 0.5f;
  const float cs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int i = 0; i < 4; ++i) {
    p.v[i].pos[0] = cs[i][0], p.v[i].pos[1] = cs[i][1], p.v[i].pos[2] = 0;
    p.v[i].color = colour;
    p.v[i].uv[0] = cs[i][0] > 0 ? 1.f : 0.f, p.v[i].uv[1] = cs[i][1] > 0 ? 1.f : 0.f;
  }
  return p;
}

void FillTables(ParticleTables& pt, uint32_t frame, uint32_t tex) {
  pt.Clear();
  ParticleBatch a;  // an alpha batch: two quads
  a.first = 0, a.count = 2, a.texture = tex, a.blend = kParticleAlpha, a.frame = frame;
  ParticleBatch b;  // an additive batch: one quad, no texture (the white stand-in)
  b.first = 2, b.count = 1, b.texture = kNoParticleTexture, b.blend = kParticleAdditive, b.frame = frame;
  ParticleBatch c;  // a lit batch (use_normal 1): one quad, drawn per particle with its own LocalWorld; the light vector is +z
  c.first = 3, c.count = 1, c.texture = tex, c.blend = kParticleAlpha, c.frame = frame, c.use_normal = 1.f, c.light[2] = 1.f;
  pt.batches = {a, b, c};
  const uint32_t orange = 0x800080FFu;  // R 255, G 0x80, B 0, A 0x80 (byte 0 = red)
  pt.particles = {MakeParticle(-0.6f, 0.0f, 0.2f, orange), MakeParticle(-0.6f, 0.5f, 0.1f, orange), MakeParticle(0.5f, 0.0f, 0.25f, orange),
                  MakeParticle(0.0f, -0.6f, 0.15f, orange)};
}

void TestRecord(bool fault_bad_index) {
  SceneTables t;
  t.header.source = uint8_t(Source::Live);
  t.header.guest_frame = 1234;
  AssetRef ar;
  ar.kind = uint8_t(AssetKind::Texture);
  ar.pac = t.AddString("pac\\prtc.pac");
  ar.entry = t.AddString("1236");
  t.assets.push_back(ar);
  FillTables(t.particles, 1234, fault_bad_index ? 9u : 0u);
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(ok, "record: a record with particles parses", err);
  const bool same = back.particles.batches.size() == t.particles.batches.size() && back.particles.particles.size() == t.particles.particles.size() &&
                    (back.particles.particles.empty() || std::memcmp(back.particles.particles.data(), t.particles.particles.data(), back.particles.particles.size() * sizeof(Particle)) == 0) &&
                    (back.particles.batches.empty() || std::memcmp(back.particles.batches.data(), t.particles.batches.data(), back.particles.batches.size() * sizeof(ParticleBatch)) == 0);
  Check(same, "record: the particle tables round-trip byte for byte (a texture index past the assets would have dropped them)");
  SceneTables none;
  none.header.source = uint8_t(Source::Live);
  const std::vector<uint8_t> nb = SerializeRecord(none);
  SceneTables nback;
  Check(ParseRecord(nb.data(), nb.size(), nback, err) && nback.particles.empty(), "record: a record without particles has none");
  // A malformed table (a batch past the particle list) is dropped as a whole, the rest of the record still reads.
  SceneTables bad = t;
  bad.particles.batches[1].count = 50;
  const std::vector<uint8_t> bb = SerializeRecord(bad);
  SceneTables bback;
  Check(ParseRecord(bb.data(), bb.size(), bback, err) && bback.particles.empty() && bback.assets.size() == 1, "record: a malformed particle table is dropped whole, the record reads");
  // A particle texture that names a Model asset is dropped too (the asset must be a Texture).
  SceneTables wrong = t;
  wrong.assets[0].kind = uint8_t(AssetKind::Model);
  const std::vector<uint8_t> wb = SerializeRecord(wrong);
  SceneTables wback;
  Check(ParseRecord(wb.data(), wb.size(), wback, err) && wback.particles.empty(), "record: a particle texture that is not a Texture asset drops the tables");
  // The sections sit at ids 54 / 55, after the crowd markers' 40 / 41.
  Check(kScParticleBatches == 54 && kScParticles == 55, "record: particle section ids are 54 and 55");
}

// ---- 3. the contributor -----------------------------------------------------------------------------------------------------------
void TestContributor(bool fault_raw_frame) {
  using namespace rdr_scene::live::particles;
  // The hook clock runs one behind the writer's frame (scene_frame::HookFrameOf): the reader stamps 499 for the record of writer frame 500.
  ParticleContributor pc([](uint64_t f) { return f - 1; });
  pc.PlantRawFrameCompare(fault_raw_frame);  // the planted fault: the contributor compares against the writer's own frame
  ParticleSnapshot s;
  s.guest_frame = 499;
  BatchKey k1;
  k1.pac = "pac\\prtc.pac", k1.entry = "1236";
  BatchKey k2 = k1;
  k2.blend = kParticleAdditive;
  BatchKey k3 = k1;
  k3.entry = "1237";
  const uint32_t i1 = s.Intern(k1), i2 = s.Intern(k2), i3 = s.Intern(k3);
  for (int i = 0; i < 3; ++i) s.Add(i1, MakeParticle(0, 0, 0.1f, 0xFFFFFFFFu));
  for (int i = 0; i < 2; ++i) s.Add(i2, MakeParticle(0, 0, 0.1f, 0xFFFFFFFFu));
  s.Add(i1, MakeParticle(0, 0, 0.1f, 0xFFFFFFFFu));
  s.Add(i3, MakeParticle(0, 0, 0.1f, 0xFFFFFFFFu));
  Check(s.keys.size() == 3, "contributor: keys are interned (3 distinct keys for 7 entries)");
  ParticleSnapshot copy = s;
  pc.Publish(std::move(s));
  live::FrameInfo fi;
  fi.guest_frame = 500;
  Check(pc.Latch(fi), "contributor: Latch takes the snapshot at the barrier");
  SceneTables t;
  live::Counters c;
  pc.Emit(fi, t, c);
  Check(t.particles.particles.size() == 7 && t.particles.batches.size() == 4, "contributor: a snapshot of the hook clock's frame is written: 7 particles in 4 batches",
        std::to_string(t.particles.particles.size()) + " in " + std::to_string(t.particles.batches.size()));
  Check(t.particles.Valid(), "contributor: the batches cover the particles exactly");
  Check(t.assets.size() == 2, "contributor: one AssetRef per distinct texture", std::to_string(t.assets.size()));
  bool stamps = !t.particles.batches.empty();
  for (const ParticleBatch& b : t.particles.batches) stamps = stamps && b.frame == 500;
  Check(stamps, "contributor: every batch carries the record's guest frame");
  // Pending is empty after use: a second Emit with nothing published writes nothing (never last frame's particles again).
  SceneTables t2;
  live::Counters c2;
  pc.Latch(fi);
  pc.Emit(fi, t2, c2);
  Check(t2.particles.empty(), "contributor: nothing is written twice (pending is empty after use)");
  // A snapshot of the wrong frame (the reader skipped a frame: it stamped 497) is stale and never written.
  ParticleSnapshot old = copy;
  old.guest_frame = 497;
  pc.Publish(std::move(old));
  pc.Latch(fi);
  SceneTables t3;
  live::Counters c3;
  pc.Emit(fi, t3, c3);
  Check(t3.particles.empty() && c3.stale == 1, "contributor: an older snapshot is counted stale and not written");
  ParticleSnapshot big;
  big.guest_frame = 7;
  const uint32_t bk = big.Intern(k1);
  for (uint32_t i = 0; i < kMaxParticlesPerFrame + 10; ++i) big.Add(bk, MakeParticle(0, 0, 0.1f, 0xFFFFFFFFu));
  SceneTables t4;
  const uint32_t n = ParticleContributor::WriteTables(big, 7, t4);
  Check(n == kMaxParticlesPerFrame, "contributor: past the cap the rest are left out (counted, never silent)", std::to_string(n));
}

// ---- 3b. the source reader (route 2) ----------------------------------------------------------------------------------------------
struct FakeMem : rdr_scene::live::particles::GuestMem {
  std::map<uint32_t, uint32_t> w;
  bool Read32(uint32_t a, uint32_t& o) const override { auto it = w.find(a); if (it == w.end()) return false; o = it->second; return true; }
  void F(uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); w[a] = u; }
};

struct ItemSpec {
  float pos[3] = {100.f, -50.f, 300.f}, prev[3] = {100.f, -50.f, 300.f};
  uint32_t col[4] = {0, 0, 0, 0x40};
  float size[4] = {20.f, 0.f, 0.f, 20.f};
  uint32_t trail_n = 0;
  float factor = 0.f;
  uint32_t vtable = 0x82057280u;
  // S103 F3: the descriptor chain and the texture table (literal offsets: docs\formats\particle_items.md)
  uint32_t pivot = 4;       // halfword at *(*(item+48)) + 226 (the low half of the word at +224)
  uint32_t blend = 0;       // word at *(*(item+48)) + 196
  uint32_t sbyte = 0;       // the sampler byte at *[o48 + 28 idx + 112]
  int32_t zint = 0;         // the int at *[o48 + 28 idx + 108]
  uint32_t tex_idx = 0;     // sprite: size word 1; quad: word 3 of the slot-23 record
  float mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100.f, -50.f, 300.f, 1};  // quads: four rows at slot 29
};

// A synthetic entry in the layout of the probe's log + the listings (docs\formats\particle_items.md). Numbers are invented except item 3's (the log's), which the
// "real item" check uses. Slot 18 deliberately points at a DIFFERENT array than slot 21: the reader must use 21.
// The two handle managers (kHandleTableGlobal156 / 148): a global holds the manager, [mgr+8] .. [mgr+16] is an array of 8-byte {id, pointer} entries, the handle's low 16 bits index it.
void BuildHandleTables(FakeMem& m) {
  using namespace rdr_scene::live::particles;
  const struct { uint32_t global, mgr, begin, id, ptr; } t[2] = {{0x83929A98u, 0x70000, 0x72000, 0x0059002Cu, 0xAF59BF00u}, {0x8392E994u, 0x71000, 0x73000, 0x000F0007u, 0xAF4783C0u}};
  for (const auto& h : t) {
    m.w[h.global] = h.mgr, m.w[h.mgr + 8] = h.begin, m.w[h.mgr + 16] = h.begin + 8 * 0x100;
    m.w[h.begin + 8 * (h.id & 0xFFFF)] = h.id, m.w[h.begin + 8 * (h.id & 0xFFFF) + 4] = h.ptr;
  }
}

void BuildItem(FakeMem& m, uint32_t item, const ItemSpec& s, uint32_t base = 0) {
  m.w[item] = s.vtable;
  m.w[item + 40] = 1;
  m.w[item + 68] = base;
  const uint32_t pos = item + 0x1000, col = pos + 0x100, prev = col + 0x100, size = prev + 0x100, tr = size + 0x100, decoy = tr + 0x100, tab = item + 0x1700, mat = item + 0x1800;
  const auto slot = [&](uint32_t k, uint32_t p) { m.w[item + 4 * (base + k)] = p; };
  slot(15, item + 0x2000), slot(18, decoy), slot(21, pos), slot(23, prev), slot(25, col), slot(27, size), slot(29, mat), slot(33, tab), slot(35, tr);
  BuildHandleTables(m);
  m.w[item + 148] = 0x000F0007u, m.w[item + 156] = 0x0059002Cu;  // the +152 / +160 cache words are NOT set: the reader resolves the handles itself (the draw that fills them does not run under OURS)
  m.w[item + 48] = item + 0x3000, m.w[item + 0x3000] = item + 0x3100;
  m.w[item + 0x3100 + 224] = s.pivot, m.w[item + 0x3100 + 196] = s.blend;
  m.w[item + 0x3000 + 152] = 0, m.w[item + 0x3000 + 112] = item + 0x3200, m.w[item + 0x3200] = s.sbyte << 24, m.w[item + 0x3000 + 108] = item + 0x3210, m.w[item + 0x3210] = uint32_t(s.zint);
  for (uint32_t i = 0; i < 8; ++i) m.w[tab + 8 * i] = 0x09150000u + i, m.w[tab + 8 * i + 4] = 0xAA0E0000u + i;  // the slot-33 table: handles {id, cached pointer}
  for (int i = 0; i < 3; ++i) m.F(pos + 4 * uint32_t(i), s.pos[i]), m.F(prev + 4 * uint32_t(i), s.prev[i]), m.F(decoy + 4 * uint32_t(i), 9999.f);
  m.F(pos + 12, 1.f);
  for (int i = 0; i < 4; ++i) m.w[col + 4 * uint32_t(i)] = s.col[i], m.F(size + 4 * uint32_t(i), s.size[i]);
  m.w[size + 4] = s.tex_idx;
  if (s.vtable == 0x820572A0u) {  // a quad: its own matrix per key, the texture index in word 3 of the slot-23 record
    m.w[prev + 12] = s.tex_idx;
    for (int i = 0; i < 16; ++i) m.F(mat + 4 * uint32_t(i), s.mat[i]);
  }
  m.w[tr + 20] = s.trail_n, m.F(tr + 24, s.factor);
  const float uvs[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  for (int i = 0; i < 4; ++i) m.F(tr + 32 + 16 * uint32_t(i), uvs[i][0]), m.F(tr + 36 + 16 * uint32_t(i), uvs[i][1]);
}

void BuildEntry(FakeMem& m, uint32_t entry, const std::vector<ItemSpec>& items, bool gate_open, uint32_t idx = 0, uint32_t e64 = 0, uint32_t f132 = 1) {
  m.w[entry + 56] = idx;
  m.w[entry + 176 * idx + 208] = gate_open ? 1 : 0;
  m.w[entry + 176 * idx + 80 + 132] = f132;
  m.w[entry + 64] = e64;
  m.w[entry + 40] = 0x100, m.w[0x100] = 0x110, m.w[0x110 + 12] = uint32_t(items.size());
  m.w[entry + 44] = 0x120;
  for (uint32_t i = 0; i < items.size(); ++i) {
    const uint32_t item = 0x10000 + 0x10000 * i;
    m.w[0x120 + 4 * i] = item;
    BuildItem(m, item, items[i]);
  }
}

void TestReader(const std::string& fault) {
  using namespace rdr_scene::live::particles;
  g_reader_plant = fault == "reader_scale" ? 1 : fault == "reader_gate" ? 2 : 0;
  // An asymmetric, rotated view (right = col 0, up = col 1, fwd = col 2): LocalWorld rows are literal values below.
  const float view[16] = {0.8f, 0.1f, 0.5916f, 0, 0.0f, 0.9864f, -0.1644f, 0, -0.6f, 0.1315f, 0.7893f, 0, 0, 0, 0, 1};
  const CameraBasis cam = BasisFromView(view);
  ItemSpec a, b;
  b.pos[0] = 110.f, b.size[0] = 4.f, b.size[3] = 4.f, b.col[3] = 0x25;
  ParticleSnapshot s;
  ReaderStats st;
  FakeMem m;
  BuildEntry(m, 0x3000, {a, b}, true);
  ReadEntry(m, 0x3000, cam, s, st);
  Check(s.entries.size() == 2 && st.items == 2, "reader: two items of one key each give two sprites", std::to_string(s.entries.size()));
  if (s.entries.size() == 2) {
    const Particle& p = s.entries[0].p;
    // LocalWorld row r = (right[r], up[r], fwd[r] | t[r]): literal values from the asymmetric view (the position comes from slot 21, not the decoy slot 18)
    const float want[12] = {0.8f, 0.1f, 0.5916f, 100.f, 0.0f, 0.9864f, -0.1644f, -50.f, -0.6f, 0.1315f, 0.7893f, 300.f};
    bool ok = true;
    for (int i = 0; i < 12; ++i) ok = ok && std::fabs(p.local_world[i] - want[i]) < 1e-4f;
    Check(ok, "reader: LocalWorld = literal rows (camera axes | position from slot 21, not slot 18)");
    Check(std::fabs(p.v[0].pos[0] + 200.f) < 1e-3f && std::fabs(p.v[3].pos[1] - 200.f) < 1e-3f && std::fabs(s.entries[1].p.v[1].pos[0] - 40.f) < 1e-3f,
          "reader: the vertices are +-size * 10 (20 -> 200, 4 -> 40)");
    Check(p.v[1].uv[0] == 1.f && p.v[1].uv[1] == 0.f && p.v[2].uv[1] == 1.f, "reader: corner uv are read from slot 35's object");
    Check(p.v[0].color == 0x80000000u && (s.entries[1].p.v[0].color >> 24) == 0x4A, "reader: the colour ints give ARGB bytes (alpha 0x40 -> 0x80, 0x25 -> 0x4A)");
  } else {
    Check(false, "reader: LocalWorld = literal rows (camera axes | position from slot 21, not slot 18)");
    Check(false, "reader: the vertices are +-size * 10 (20 -> 200, 4 -> 40)");
  }
  s.guest_frame = 9;
  SceneTables t;
  const uint32_t n = ParticleContributor::WriteTables(s, 9, t);
  Check(n == s.entries.size() && n == 2 && t.particles.Valid(), "reader: the sprites reach the record's particle tables");
  // The probe's third sprite (item A9E55D90, frame 3655): size 2.0 / 0 / pi/4 / 40.0, alpha int 0x25: v0 = (268.70, -296.98), alpha 0x4A.
  {
    ItemSpec c;
    c.size[0] = 2.f, c.size[2] = 0.78539819f, c.size[3] = 40.f, c.col[3] = 0x25;
    FakeMem r;
    BuildEntry(r, 0x3000, {c}, true);
    ParticleSnapshot sr;
    ReaderStats str;
    ReadEntry(r, 0x3000, cam, sr, str);
    const bool ok = sr.entries.size() == 1 && std::fabs(sr.entries[0].p.v[0].pos[0] - 268.70f) < 0.02f && std::fabs(sr.entries[0].p.v[0].pos[1] + 296.98f) < 0.02f &&
                    std::fabs(sr.entries[0].p.v[3].pos[0] + 268.70f) < 0.02f && std::fabs(sr.entries[0].p.v[3].pos[1] - 296.98f) < 0.02f && (sr.entries[0].p.v[0].color >> 24) == 0x4A;
    Check(ok, "reader: the probe's real third sprite: the 20x400 quad turned 45 degrees (v0 = (268.70, -296.98)), alpha 0x4A");
  }
  // A trail: N = 2, factor 1: three sprites, stepping (pos - prev) / N, the alpha byte halving each step.
  {
    ItemSpec c;
    c.prev[0] = 90.f, c.trail_n = 2, c.factor = 1.f;
    FakeMem r;
    BuildEntry(r, 0x3000, {c}, true);
    ParticleSnapshot sr;
    ReaderStats str;
    ReadEntry(r, 0x3000, cam, sr, str);
    const bool ok = sr.entries.size() == 3 && std::fabs(sr.entries[1].p.local_world[3] - 95.f) < 1e-3f && std::fabs(sr.entries[2].p.local_world[3] - 90.f) < 1e-3f &&
                    (sr.entries[1].p.v[0].color >> 24) == 0x40 && (sr.entries[2].p.v[0].color >> 24) == 0x20;
    Check(ok, "reader: a trail gives N + 1 sprites stepping back, the alpha halving each step");
  }
  // Early outs and guards.
  const auto run = [&](FakeMem& mm, uint32_t entry, ReaderStats& rs) {
    ParticleSnapshot ss;
    ReadEntry(mm, entry, cam, ss, rs);
    return ss.entries.size();
  };
  {
    FakeMem g;
    BuildEntry(g, 0x3000, {a}, false);
    ReaderStats rs;
    Check(run(g, 0x3000, rs) == 0 && rs.entries_gated == 1, "reader: an entry whose gate word is 0 gives no sprites");
    FakeMem g2;
    BuildEntry(g2, 0x3000, {a}, true, 0, 5, 0);
    ReaderStats rs2;
    Check(run(g2, 0x3000, rs2) == 0 && rs2.entries_gated == 1, "reader: the second gate (f132 == 0 && entry+64 != 0) refuses too");
    FakeMem g3;
    BuildEntry(g3, 0x3000, {a}, true, 1);
    ReaderStats rs3;
    Check(run(g3, 0x3000, rs3) == 1, "reader: sub-record index 1 is accepted");
    g3.w[0x3000 + 56] = 2;
    ReaderStats rs3b;
    Check(run(g3, 0x3000, rs3b) == 0 && rs3b.unreadable == 1, "reader: sub-record index 2 is refused");
    FakeMem q;
    ItemSpec qs = a;
    qs.vtable = 0x82057300u;  // neither the sprite nor the quad table (the third method's class is [U])
    BuildEntry(q, 0x3000, {qs, qs}, true);
    ReaderStats rq;
    Check(run(q, 0x3000, rq) == 0 && rq.items_other_class == 2, "reader: an item of another class is counted, not read");
    FakeMem z;
    ItemSpec zs = a;
    zs.col[3] = 0;
    BuildEntry(z, 0x3000, {zs}, true);
    ReaderStats rz;
    Check(run(z, 0x3000, rz) == 0 && rz.alpha_zero == 1, "reader: an alpha-0 key is skipped");
    FakeMem nf;
    ItemSpec ns = a;
    ns.pos[0] = 1e9f;
    BuildEntry(nf, 0x3000, {ns}, true);
    ReaderStats rn;
    Check(run(nf, 0x3000, rn) == 0 && rn.non_finite == 1, "reader: a huge position is skipped and counted");
    FakeMem s15;
    BuildEntry(s15, 0x3000, {a}, true);
    s15.w[0x10000 + 4 * 15] = 0;
    ReaderStats r15;
    Check(run(s15, 0x3000, r15) == 0 && r15.items_skipped == 1, "reader: slot base+15 null skips the item");
    FakeMem un;
    BuildEntry(un, 0x3000, {a}, true);
    un.w[0x72000 + 8 * 0x2C + 4] = 0;  // the +156 handle's table entry resolves to nothing: the game's +160 would be 0 and the draw ends
    ReaderStats ru;
    Check(run(un, 0x3000, ru) == 0 && ru.items_unresolved == 1, "reader: a handle that does not resolve (+156 -> +160) skips the item");
    FakeMem un2;
    BuildEntry(un2, 0x3000, {a}, true);
    un2.w[0x73000 + 8 * 7] = 0x00100007u;  // the +148 handle's entry holds another generation: the id must match
    ReaderStats ru2;
    Check(run(un2, 0x3000, ru2) == 0 && ru2.items_unresolved == 1, "reader: a handle whose table entry has another id skips the item");
    FakeMem rsm;
    BuildEntry(rsm, 0x3000, {a}, true);
    ReaderStats rrs;
    Check(run(rsm, 0x3000, rrs) == 1 && rrs.items_unresolved == 0, "reader: handles resolve through the manager tables (the +152 / +160 cache words are not needed)");
    FakeMem ni;
    BuildEntry(ni, 0x3000, {a}, true);
    ni.w[0x3000 + 44] = 0;
    ReaderStats rni;
    Check(run(ni, 0x3000, rni) == 0 && rni.entries_no_items == 1 && rni.unreadable == 0, "reader: a null item array is a normal no-draw, not an unreadable read");
    FakeMem wr;
    BuildEntry(wr, 0xFFFFFFFCu - 0x200, {a}, true);  // an entry whose +176*idx / +208 reach past 2^32 would wrap: the reader refuses wrapping ranges
    wr.w[0xFFFFFFFEu] = 1;
    uint32_t dummy = 0;
    FakeMem wrap;
    wrap.w[0xFFFFFFFEu] = 5;
    ReaderStats rw;
    ParticleSnapshot sw;
    ReadEntry(wrap, 0xFFFFFFF0u, cam, sw, rw);  // entry + 56 = 0xFFFFFFF0 + 56 wraps
    Check(sw.entries.empty() && rw.unreadable == 1 && dummy == 0, "reader: a 4-byte range that wraps mod 2^32 is refused");
  }
  // The node: flag word, state bytes 1..3, the key cap.
  {
    FakeMem nm;
    BuildEntry(nm, 0x5000, {a, b}, true);
    nm.w[0x4010] = 0x5000, nm.w[0x4000 + 3088] = 1, nm.w[0x4000 + 3084] = 1;
    size_t got[4] = {};
    for (uint32_t state = 0; state < 4; ++state) {
      nm.w[0x4014] = state << 24;  // node + 20: byte 0 is the top byte (big-endian)
      ParticleSnapshot sn;
      ReaderStats rn;
      ReadNode(nm, 0x4000, cam, sn, rn);
      got[state] = sn.entries.size();
    }
    Check(got[0] == 0 && got[1] == 0 && got[2] == 2 && got[3] == 0, "reader: only state 2 is drawn (states 0..3 tested)");
    nm.w[0x4014] = 2u << 24, nm.w[0x4000 + 3084] = 0;
    ParticleSnapshot si;
    ReaderStats ri;
    ReadNode(nm, 0x4000, cam, si, ri);
    Check(si.entries.empty() && ri.node_idle == 1, "reader: node + 3084 == 0 is the node's own early out");
    nm.w[0x4000 + 3084] = 1, nm.w[0x4000 + 3088] = 5000;  // an over-cap count is clamped, not dropped
    ParticleSnapshot sc;
    ReaderStats rc;
    ReadNode(nm, 0x4000, cam, sc, rc);
    Check(rc.over_cap == 1, "reader: an over-cap entry count is clamped and counted");
    // The particle cap: a snapshot already full drops the rest and says so.
    ParticleSnapshot full;
    const uint32_t ki = full.Intern(BatchKey());
    for (uint32_t i = 0; i < kMaxParticlesPerFrame; ++i) full.Add(ki, Particle{});
    FakeMem fm;
    BuildEntry(fm, 0x3000, {a}, true);
    ReaderStats rf;
    ReadEntry(fm, 0x3000, cam, full, rf);
    Check(full.entries.size() == kMaxParticlesPerFrame && full.dropped >= 1, "reader: the per-frame particle cap holds and the drop is counted");
  }
  g_reader_plant = 0;
}

// ---- 3b2. lane F3: texture / blend / sampler / ZOffset reads, the 3x3 pivot grid, the quad class --------------------------------------
struct NameFake : rdr_scene::live::particles::TextureNamer {  // the handle {0x09150000 + idx, 0xAA0E0000 + idx}: idx 1 and 2 have a file, anything else has none
  bool Name(uint32_t id, uint32_t /*cached_ptr: informational, ignored*/, std::string& pac, std::string& entry) const override {
    const uint32_t idx = id - 0x09150000u;
    if (idx == 1) return pac = "prtcls.pac", entry = "Smoke0001", true;
    if (idx == 2) return pac = "prtcls.pac", entry = "Light0001", true;
    return false;
  }
};

void TestReaderF3(const std::string& fault) {
  using namespace rdr_scene::live::particles;
  g_reader_plant = fault == "reader_pivot" ? 3 : fault == "reader_quad" ? 4 : 0;
  const float view[16] = {0.8f, 0.1f, 0.5916f, 0, 0.0f, 0.9864f, -0.1644f, 0, -0.6f, 0.1315f, 0.7893f, 0, 0, 0, 0, 1};
  const CameraBasis cam = BasisFromView(view);
  // The 3x3 pivot grid, literal: code -> (cx, cy), corners ((-1 + cx) * 10 w, (-1 + cy) * 10 h) .. ((1 + cx) * 10 w, (1 + cy) * 10 h) with w = h = 20; any code above 8 is centred.
  {
    static const float grid[9][2] = {{1, -1}, {0, -1}, {-1, -1}, {1, 0}, {0, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}};
    bool ok = true;
    std::string why;
    for (uint32_t code = 0; code < 12; ++code) {
      const uint32_t codes[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFFFF, 12};
      ItemSpec s;
      s.pivot = codes[code];
      FakeMem m;
      BuildEntry(m, 0x3000, {s}, true);
      ParticleSnapshot sn;
      ReaderStats st;
      ReadEntry(m, 0x3000, cam, sn, st);
      const float cx = code < 9 ? grid[code][0] : 0.f, cy = code < 9 ? grid[code][1] : 0.f;
      const bool one = sn.entries.size() == 1 && std::fabs(sn.entries[0].p.v[0].pos[0] - (-1 + cx) * 200.f) < 1e-2f && std::fabs(sn.entries[0].p.v[0].pos[1] - (-1 + cy) * 200.f) < 1e-2f &&
                       std::fabs(sn.entries[0].p.v[3].pos[0] - (1 + cx) * 200.f) < 1e-2f && std::fabs(sn.entries[0].p.v[3].pos[1] - (1 + cy) * 200.f) < 1e-2f;
      if (!one) ok = false, why += " code " + std::to_string(codes[code]);
    }
    Check(ok, "reader: the 3x3 pivot grid: codes 0..8 anchor as listed, any other code is centred", why);
    // The shift is applied before the size scale and the roll: code 3 (+1, 0), size 1, roll 90 degrees: corner (-1, -1) -> local (0, -10) -> (10, 0).
    ItemSpec s;
    s.pivot = 3, s.size[0] = 1.f, s.size[3] = 1.f, s.size[2] = 1.5707964f;
    FakeMem m;
    BuildEntry(m, 0x3000, {s}, true);
    ParticleSnapshot sn;
    ReaderStats st;
    ReadEntry(m, 0x3000, cam, sn, st);
    Check(sn.entries.size() == 1 && std::fabs(sn.entries[0].p.v[0].pos[0] - 10.f) < 1e-3f && std::fabs(sn.entries[0].p.v[0].pos[1]) < 1e-3f && st.pivot_not_centred == 1,
          "reader: the pivot shift goes through the size and the roll (code 3, 90 degrees: (0, -10) -> (10, 0)) and is counted");
  }
  // The quad class (vtable 0x820572A0): its own matrix per key (four rows at slot 29), position = row 3, corners unscaled, local_world rows = the matrix's columns.
  {
    ItemSpec q;
    q.vtable = 0x820572A0u;
    const float mat[16] = {0, 2, 0, 0, -3, 0, 0, 0, 0, 0, 5, 0, 7, 8, 9, 1};
    std::memcpy(q.mat, mat, sizeof mat);
    q.prev[0] = 7.f, q.prev[1] = 8.f, q.prev[2] = 9.f;
    FakeMem m;
    BuildEntry(m, 0x3000, {q}, true);
    ParticleSnapshot sn;
    ReaderStats st;
    ReadEntry(m, 0x3000, cam, sn, st);
    const float want[12] = {0, -3, 0, 7, 2, 0, 0, 8, 0, 0, 5, 9};  // row r = (M[0][r], M[1][r], M[2][r], M[3][r])
    bool ok = sn.entries.size() == 1 && st.items_quad == 1 && st.items_other_class == 0;
    for (int i = 0; ok && i < 12; ++i) ok = std::fabs(sn.entries[0].p.local_world[i] - want[i]) < 1e-5f;
    Check(ok, "reader: a quad takes its own matrix (local_world rows = the matrix's columns, position = row 3), not the camera's axes");
    Check(sn.entries.size() == 1 && std::fabs(sn.entries[0].p.v[0].pos[0] + 10.f) < 1e-4f && std::fabs(sn.entries[0].p.v[3].pos[1] - 10.f) < 1e-4f && sn.keys.size() == 1 && sn.keys[0].site == kParticleQuad,
          "reader: a quad's corners are unscaled (+-10: the scale is in its matrix) and its batch is the quad site");
    // Trail: N = 2, factor 1, prev x = 1: step (7 - 1) / 2 = 3 per step; alpha halves.
    ItemSpec t = q;
    t.prev[0] = 1.f, t.trail_n = 2, t.factor = 1.f;
    FakeMem mt;
    BuildEntry(mt, 0x3000, {t}, true);
    ParticleSnapshot st3;
    ReaderStats rt;
    ReadEntry(mt, 0x3000, cam, st3, rt);
    Check(st3.entries.size() == 3 && std::fabs(st3.entries[1].p.local_world[3] - 4.f) < 1e-4f && std::fabs(st3.entries[2].p.local_world[3] - 1.f) < 1e-4f &&
              (st3.entries[1].p.v[0].color >> 24) == 0x40 && (st3.entries[2].p.v[0].color >> 24) == 0x20,
          "reader: a quad's trail steps its row-3 position back and halves the alpha");
    // A quad whose matrix holds a non-finite entry is skipped and counted.
    ItemSpec bad = q;
    bad.mat[5] = 1e9f;
    FakeMem mb;
    BuildEntry(mb, 0x3000, {bad}, true);
    ParticleSnapshot sb;
    ReaderStats rb;
    ReadEntry(mb, 0x3000, cam, sb, rb);
    Check(sb.entries.empty() && rb.non_finite == 1, "reader: a quad matrix with a huge entry is skipped and counted");
  }
  // Textures: slot 33's table indexed by the size word (sprites) / slot-23 word 3 (quads); each handle {id, cached pointer} goes to the namer.
  {
    ItemSpec a, b, c, d;
    a.tex_idx = 1, b.tex_idx = 2, c.tex_idx = 0, d.vtable = 0x820572A0u, d.tex_idx = 1;
    FakeMem m;
    BuildEntry(m, 0x3000, {a, b, c, d}, true);
    NameFake namer;
    ParticleSnapshot sn;
    ReaderStats st;
    ReadEntry(m, 0x3000, cam, sn, st, &namer);
    // keys: (Smoke0001, sprite), (Light0001, sprite), (none, sprite), (Smoke0001, quad)
    const bool names = sn.keys.size() == 4 && sn.keys[0].entry == "Smoke0001" && sn.keys[1].entry == "Light0001" && sn.keys[2].entry.empty() && sn.keys[3].entry == "Smoke0001" && sn.keys[3].site == kParticleQuad;
    Check(names && st.tex_named == 3 && st.tex_unresolved == 1, "reader: the texture handle of each key goes to the namer; a handle with no file is left without a texture and counted");
    SceneTables t;
    ParticleContributor::WriteTables(sn, 5, t);
    Check(t.particles.batches.size() == 4 && t.assets.size() == 2 && t.particles.batches[0].texture == t.particles.batches[3].texture && t.particles.batches[0].texture != kNoParticleTexture &&
              t.particles.batches[2].texture == kNoParticleTexture,
          "reader: named textures become one AssetRef each (shared by the sprite and the quad), the unnamed one stays kNoParticleTexture");
    FakeMem m0;
    BuildEntry(m0, 0x3000, {a, b}, true);
    ParticleSnapshot s0;
    ReaderStats r0;
    ReadEntry(m0, 0x3000, cam, s0, r0, nullptr);
    Check(s0.entries.size() == 2 && r0.tex_unresolved == 2 && r0.tex_named == 0 && s0.keys[0].entry.empty(), "reader: with no namer every texture stays unresolved (counted), the sprites are still read");
    // two keys of ONE item using two textures: one item, count 2
    ItemSpec e;
    FakeMem me;
    BuildEntry(me, 0x3000, {e}, true);
    me.w[0x10000 + 40] = 2;
    const uint32_t size = 0x10000 + 0x1300, pos = 0x10000 + 0x1000, col = 0x10000 + 0x1100, prev = 0x10000 + 0x1200;
    me.w[size + 16 + 4] = 2, me.F(size + 16, 20.f), me.F(size + 16 + 8, 0.f), me.F(size + 16 + 12, 20.f), me.w[size + 4] = 1;
    for (int i = 0; i < 3; ++i) me.F(pos + 16 + 4 * uint32_t(i), e.pos[i]), me.F(prev + 16 + 4 * uint32_t(i), e.prev[i]);
    for (int i = 0; i < 4; ++i) me.w[col + 16 + 4 * uint32_t(i)] = e.col[i];
    ParticleSnapshot s2;
    ReaderStats r2;
    ReadEntry(me, 0x3000, cam, s2, r2, &namer);
    Check(s2.entries.size() == 2 && s2.keys.size() == 2 && s2.entries[0].key != s2.entries[1].key && s2.keys[0].entry == "Smoke0001" && s2.keys[1].entry == "Light0001",
          "reader: two keys of one item with different texture indices land in two batches (the texture is per particle)",
          "entries " + std::to_string(s2.entries.size()) + " keys " + std::to_string(s2.keys.size()) + " unreadable " + std::to_string(r2.unreadable) + " alpha0 " + std::to_string(r2.alpha_zero) + " nonfinite " + std::to_string(r2.non_finite));
  }
  // Blend code, sampler byte and ZOffset (per item): literal mapping and counts.
  {
    ItemSpec a, b, c;
    a.blend = 0, a.sbyte = 0, a.zint = 0;
    b.blend = 1, b.sbyte = 2, b.zint = -2000;
    c.blend = 5, c.sbyte = 9, c.zint = 500;
    FakeMem m;
    BuildEntry(m, 0x3000, {a, b, c}, true);
    ParticleSnapshot sn;
    ReaderStats st;
    ReadEntry(m, 0x3000, cam, sn, st);
    bool ok = sn.keys.size() == 3 && sn.keys[0].blend == kParticleAlpha && sn.keys[1].blend == kParticleAdditive && sn.keys[2].blend == kParticleAlpha && st.blend_read == 3 && st.blend_other == 1;
    Check(ok, "reader: the blend code (0 alpha, 1 additive [unconfirmed], other counted) is read per item", std::to_string(sn.keys.size()));
    ok = sn.keys.size() == 3 && sn.keys[0].addr == 1 && sn.keys[1].addr == 3 && sn.keys[2].addr == 0 && st.sampler_read == 2 && SamplerValueOfByte(0) == 2 && SamplerValueOfByte(1) == 5 &&
         SamplerValueOfByte(2) == 6 && SamplerValueOfByte(3) == 1 && SamplerValueOfByte(4) == 0;
    Check(ok, "reader: the sampler byte (0..3 -> the game's 2 / 5 / 6 / 1, kept as byte + 1) is read per item; a byte above 3 sets nothing");
    ok = sn.keys.size() == 3 && std::fabs(sn.keys[0].zoffset) < 1e-9f && std::fabs(sn.keys[1].zoffset + 2.f) < 1e-5f && std::fabs(sn.keys[2].zoffset - 0.5f) < 1e-5f && st.zoffset_read == 3;
    Check(ok, "reader: ZOffset = the int * 0.001 (c200.x)");
    SceneTables t;
    ParticleContributor::WriteTables(sn, 5, t);
    Check(t.particles.batches.size() == 3 && t.particles.batches[1].blend == kParticleAdditive && t.particles.batches[1].addr == 3 && std::fabs(t.particles.batches[1].zoffset + 2.f) < 1e-5f,
          "reader: blend, sampler byte and ZOffset reach the record's batches");
  }
  g_reader_plant = 0;
}

// ---- 3c. the particle source (lane F2): the latch read through the real GuestMem path ------------------------------------------------
FakeMem* g_src_mem = nullptr;
uint64_t g_src_reads = 0;
bool SrcRead(uint32_t guest, void* dst, size_t n) {  // n raw BIG-ENDIAN bytes, as the game's reader gives them
  ++g_src_reads;
  uint8_t* d = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; i += 4) {
    const auto it = g_src_mem->w.find(guest + uint32_t(i));
    if (it == g_src_mem->w.end()) return false;
    const uint32_t v = it->second;
    d[i] = uint8_t(v >> 24), d[i + 1] = uint8_t(v >> 16), d[i + 2] = uint8_t(v >> 8), d[i + 3] = uint8_t(v);
  }
  return true;
}

void TestSource(const std::string& fault) {
  using namespace rdr_scene::live::particles;
  static int mode = 0;
  scene_frame::g_mode_cvar.store(&mode, std::memory_order_relaxed);
  g_source_plant = fault == "source_sview" ? 1 : fault == "old_pairing" ? 2 : 0;   // S103 F5: old_pairing = the pairing rule the live runs refuted (noted == hook frame)
  ItemSpec a, b;
  b.pos[0] = 110.f, b.size[0] = 4.f, b.size[3] = 4.f, b.col[3] = 0x25;
  FakeMem m;
  BuildEntry(m, 0x5000, {a, b}, true);
  m.w[0x4010] = 0x5000, m.w[0x4000 + 3088] = 1, m.w[0x4000 + 3084] = 1, m.w[0x4014] = 2u << 24;
  // The scene camera (inputs_core.h ReadSceneCamera): manager pointer, the view at +1488 (rigid, row-vector, GAME space), the FOV at +1552.
  const uint32_t mgr = 0x6000;
  m.w[scene_inputs::kCamMgrPtr] = mgr;
  const float view[16] = {0.8f, 0, -0.6f, 0, 0, 1, 0, 0, 0.6f, 0, 0.8f, 0, 0, 0, 0, 1};
  for (int i = 0; i < 16; ++i) m.F(mgr + scene_inputs::kCamMgrView + 4 * uint32_t(i), view[i]);
  m.F(mgr + scene_inputs::kCamMgrFov, 60.f);
  g_src_mem = &m;
  ParticleSource src;
  src.SetReader(&SrcRead);
  ParticleSnapshot s;
  mode = 0;
  g_src_reads = 0;
  src.NoteNode(0x4000, 100);
  Check(!src.Take(100, s) && g_src_reads == 0, "source: scene mode 0 reads nothing (one relaxed load)");
  mode = 1;
  const bool took = src.Take(100, s);
  Check(took && s.guest_frame == scene_frame::HookFrameOf(100) && s.entries.size() == 2, "source: the latch read gives the live sprites stamped with the HOOK clock's frame", std::to_string(s.entries.size()));
  if (took && s.entries.size() == 2) {
    // LocalWorld from the GAME-space view: rows (right[r], up[r], fwd[r] | t[r]) = (0.8, 0, -0.6 | 100) (0, 1, 0 | -50) (0.6, 0, 0.8 | 300). The viewer-space S * view would flip the y row.
    const float want[12] = {0.8f, 0.f, -0.6f, 100.f, 0.f, 1.f, 0.f, -50.f, 0.6f, 0.f, 0.8f, 300.f};
    bool ok = true;
    for (int i = 0; i < 12; ++i) ok = ok && std::fabs(s.entries[0].p.local_world[i] - want[i]) < 1e-4f;
    Check(ok, "source: the GAME-space view reaches BasisFromView (never S * view)");
  } else {
    Check(false, "source: the GAME-space view reaches BasisFromView (never S * view)");
  }
  // Through the contributor: Latch pulls the source, Emit writes the tables for the record of writer frame 100.
  {
    ParticleContributor pc([](uint64_t f) { return scene_frame::HookFrameOf(f); });
    pc.SetSource([&](uint64_t wf, ParticleSnapshot& o) { return src.Take(wf, o); });
    pc.SetCensusExtra([&] { return src.Census(); });
    live::FrameInfo fi;
    fi.guest_frame = 100;
    pc.Latch(fi);
    SceneTables t;
    live::Counters c;
    pc.Emit(fi, t, c);
    Check(t.particles.particles.size() == 2 && t.particles.Valid() && c.stale == 0, "source: Latch + Emit put the sprites in the record's tables (hook-clock stamp accepted)");
    std::string line;
    pc.Census(line);
    Check(line.find("[scene][particles] read: taken") != std::string::npos && line.find("holes") != std::string::npos, "source: the census line carries the reader's counts and holes");
  }
  // The node did not run lately: nothing to draw, counted, not a hole.
  {
    ParticleSource old;
    old.SetReader(&SrcRead);
    old.NoteNode(0x4000, 10);
    ParticleSnapshot so;
    Check(!old.Take(100, so) && old.holes() == 0 && old.Census().find("stale_node 1") != std::string::npos, "source: a stale node is counted, not a hole");
  }
  // The pass ran but the camera cannot be read: a counted hole with its reason.
  {
    FakeMem nc = m;
    nc.w.erase(scene_inputs::kCamMgrPtr);
    g_src_mem = &nc;
    ParticleSnapshot sn;
    ParticleSource ns;
    ns.SetReader(&SrcRead);
    ns.NoteNode(0x4000, 100);
    const bool nt = ns.Take(100, sn);
    Check(nt && sn.entries.empty() && (sn.status.reasons & kPiNoCamera) && ns.holes() == 1 && ns.incomplete() == 1 && ns.Census().find("no_camera 1") != std::string::npos,
          "source: no camera on a running pass is a counted hole (no_camera) and an empty snapshot carrying the not-whole status");
    // The status travels: through the contributor into the record's tables, through the file, and back.
    ParticleContributor pcn([](uint64_t f) { return scene_frame::HookFrameOf(f); });
    pcn.SetSource([&](uint64_t wf, ParticleSnapshot& o) { return ns.Take(wf, o); });
    live::FrameInfo fin;
    fin.guest_frame = 100;
    SceneTables tn;
    live::Counters cn;
    pcn.Emit(fin, tn, cn);   // no Latch: the source runs in Emit (B1)
    const std::vector<uint8_t> bytes = SerializeRecord(tn);
    SceneTables back;
    std::string perr;
    Check(ParseRecord(bytes.data(), bytes.size(), back, perr) && back.particles.Incomplete() == kPiNoCamera && back.particles.batches.empty(),
          "source: a not-whole pass is carried in the record (section 56) so the render side can report a hole");
    g_src_mem = &m;
  }
  // B1: Latch runs only when the tail decision is on; with no Latch the source runs in Emit and the sprites still reach the record.
  {
    ParticleSource es;
    es.SetReader(&SrcRead);
    es.NoteNode(0x4000, 100);
    ParticleContributor pce([](uint64_t f) { return scene_frame::HookFrameOf(f); });
    pce.SetSource([&](uint64_t wf, ParticleSnapshot& o) { return es.Take(wf, o); });
    live::FrameInfo fie;
    fie.guest_frame = 100;
    SceneTables te;
    live::Counters ce;
    pce.Emit(fie, te, ce);
    Check(te.particles.particles.size() == 2 && te.particles.Incomplete() == kPiTexture, "source: Emit WITHOUT a Latch still takes the sprites (B1: the latch is off by default)", std::to_string(te.particles.particles.size()));
  }
  // B2: an item of an unknown class, or an approximate pivot, makes the pass not whole (counted into the status).
  {
    FakeMem qm = m;
    qm.w[0x10000] = 0x82057300u;   // the first item's vtable: another class (not sprite, not quad)
    g_src_mem = &qm;
    ParticleSource qs;
    qs.SetReader(&SrcRead);
    qs.NoteNode(0x4000, 100);
    ParticleSnapshot sq;
    const bool tq = qs.Take(100, sq);
    Check(tq && sq.entries.size() == 1 && (sq.status.reasons & kPiOtherClass) && sq.status.other_class == 1 && !WholePass(sq.status), "source: an other-class item makes the pass not whole");
    FakeMem pm = m;
    pm.w[0x10000 + 0x3100 + 224] = 7;  // pivot code 7 (0, +1): decoded now, counted, no longer a reason
    g_src_mem = &pm;
    ParticleSource ps2;
    ps2.SetReader(&SrcRead);
    ps2.NoteNode(0x4000, 100);
    ParticleSnapshot sp;
    ps2.Take(100, sp);
    Check(!(sp.status.reasons & kPiPivot) && sp.status.pivot_not_centred == 1 && std::fabs(sp.entries[0].p.v[0].pos[1]) < 1e-3f, "source: a shifted pivot is decoded (v0 y = 0 for code 7), counted, and no longer makes the pass not whole");
    g_src_mem = &m;
    ParticleSnapshot sw;
    src.NoteNode(0x4000, 100);
    src.Take(100, sw);
    // F3: with no namer the textures are unresolved and the blend codes are unconfirmed: the pass is NOT whole, whatever else is clean.
    Check(sw.status.reasons == kPiTexture && !WholePass(sw.status) && !kStampParticles, "source: a clean read with no namer is not whole (texture unresolved; codes 0 and 1 are proven); the stamp stays withheld (kStampParticles false)");
    // With a namer that names every handle: only the unconfirmed blend codes remain.
    struct All : TextureNamer { bool Name(uint32_t, uint32_t, std::string& p, std::string& e) const override { return p = "prtcls.pac", e = "Smoke0001", true; } } all;
    ParticleSource sn;
    sn.SetReader(&SrcRead);
    sn.SetNamer(&all);
    sn.NoteNode(0x4000, 100);
    ParticleSnapshot sn2;
    sn.Take(100, sn2);
    Check(sn2.status.reasons == 0 && WholePass(sn2.status) && !kStampParticles && sn.Census().find("tex (named 2, unresolved 0)") != std::string::npos && sn.Census().find("confirmed 1") != std::string::npos,
          "source: with every texture named nothing remains (S103 PF: all blend codes 0-2 proven), the census says confirmed 1 (the class stamp stays withheld: kStampParticles false)", sn.Census().substr(sn.Census().find("quads")));
  }
  // B3 / B4 (S103 F5: the node is noted at the WRITER's frame, runs 28 / 29: bucket +1): the pairing must be exact, with census buckets; a second r3 in one frame is counted.
  {
    ParticleSource pr;
    pr.SetReader(&SrcRead);
    ParticleSnapshot sp;
    pr.NoteNode(0x4000, 101);   // noted one frame AFTER the writer's frame (100)
    const bool a = pr.Take(100, sp);
    pr.NoteNode(0x4000, 90);
    const bool b = pr.Take(100, sp);
    pr.NoteNode(0x4000, 99);    // the hook frame: the OLD rule's frame, refused now (it is the previous frame's pass)
    const bool d = pr.Take(100, sp);
    pr.NoteNode(0x4000, 100);   // the writer's frame: the pass the latch closes
    const bool c = pr.Take(100, sp);
    pr.NoteNode(0x4100, 100);   // a second, different r3 in that frame
    const std::string cs = pr.Census();
    Check(!a && !b && !d && c && sp.guest_frame == scene_frame::HookFrameOf(100) && cs.find("0 1, +1 1, <0 1, other 1") != std::string::npos && cs.find("nodes_extra 1") != std::string::npos,
          "source: only the exact pairing (the pass noted at the writer's frame, as the runs show) is taken; the noted-hook buckets and nodes_extra are counted", cs.substr(cs.find("pair")));
    // the plant: the old rule (noted == hook) takes the previous frame's pass and refuses the real one
    const int saved_plant = g_source_plant;
    g_source_plant = 2;
    ParticleSource po;
    po.SetReader(&SrcRead);
    ParticleSnapshot so;
    po.NoteNode(0x4000, 100);
    const bool e = po.Take(100, so);
    g_source_plant = saved_plant;
    Check(!e, "source (plant old_pairing): the old rule refuses the pass of the frame the latch closes (the fault the live runs showed: taken 0)");
  }
  // The 4-byte range rule on the real path.
  {
    FnGuestMem fm(&SrcRead);
    uint32_t v = 0;
    Check(!fm.Read32(0xFFFFFFFDu, v) && !fm.Read32(0xFFFFFFFFu, v), "source: GuestMem refuses a 4-byte range that wraps mod 2^32");
  }
  g_source_plant = 0;
  mode = 0;
  g_src_mem = nullptr;
}

// ---- 3c. lane F4: the blend proof (run 22) and the handle -> texture namer ------------------------------------------------------------
// Literal values are the run-22 log's (wwe2k14_080.log, Bryan's entrance): blend code 1, pivot 4, sampler byte 0, ZOffset 0, handles 0x0059002C / 0x000F0007 (item +156 / +148), the slot-33
// entry {0x091903BD, 0xAA0E70D0}. The wrapper, header and fetch dwords below are INVENTED (the probe did not dump them); the stream side's blend is the scan of bryan_cut.zip (docs).
struct F4Hasher : rdr_scene::live::particles::ContentHasher {   // the "guest memory" of a texture: base dword -> its mip-0 DXT5 bytes
  std::map<uint32_t, std::vector<uint8_t>> bytes;
  bool MipZeroKey(const uint32_t fetch[6], uint64_t& key) const override {
    const auto it = bytes.find(fetch[1]);
    if (it == bytes.end()) return false;
    key = rdr_scene::live::particles::ContentKey(4, 4, 0x35545844u, it->second.data(), it->second.size());
    return true;
  }
};

std::vector<uint8_t> F4Dds(uint8_t fill) {   // a 4 x 4 DXT5 DDS (one 16-byte block)
  std::vector<uint8_t> d(128 + 16, 0);
  std::memcpy(d.data(), "DDS ", 4);
  const uint32_t h = 4, w = 4, fourcc = 0x35545844u;
  std::memcpy(&d[12], &h, 4), std::memcpy(&d[16], &w, 4), std::memcpy(&d[84], &fourcc, 4);
  for (int i = 0; i < 16; ++i) d[128 + i] = uint8_t(fill + i);
  return d;
}

std::vector<uint8_t> F4Block(uint8_t fill) {
  const std::vector<uint8_t> d = F4Dds(fill);
  return std::vector<uint8_t>(d.begin() + 128, d.end());
}

std::vector<uint8_t> F4Directory(const std::vector<std::pair<std::string, uint8_t>>& texs) {   // a sprite directory: LE32 count, 12 bytes, then 32-byte records and the blobs
  std::vector<uint8_t> dir(0x10 + 32 * texs.size(), 0);
  const uint32_t count = uint32_t(texs.size());
  std::memcpy(dir.data(), &count, 4);
  for (size_t i = 0; i < texs.size(); ++i) {
    const std::vector<uint8_t> dds = F4Dds(texs[i].second);
    const uint32_t size = uint32_t(dds.size()), off = uint32_t(dir.size());
    uint8_t* r = dir.data() + 0x10 + 32 * i;
    std::memcpy(r, texs[i].first.c_str(), texs[i].first.size());
    std::memcpy(r + 16, "dds", 3);
    std::memcpy(r + 20, &size, 4), std::memcpy(r + 24, &off, 4);
    dir.insert(dir.end(), dds.begin(), dds.end());
  }
  return dir;
}

void TestNamerF4(const std::string& fault) {
  using namespace rdr_scene::live::particles;
  g_reader_plant = fault == "reader_blend" ? 5 : 0;
  g_namer_plant = fault == "namer_cached" ? 1 : fault == "namer_diag_uncapped" ? 2 : 0;
  // (1) The blend proof: only code 1 (additive) is proven; 0 and 2 and above are not and keep the pass not whole.
  {
    Check(BlendOfCode(1) == kParticleAdditive && BlendCodeProven(1) && BlendCodeProven(0) && BlendCodeProven(2) && !BlendCodeProven(3) && kBlendCodesConfirmed,
          "blend proof: code 1 = additive (run 22) and code 0 = alpha (their WRITER: the emitter header word block+12 of the prtcls files, S103 F5) are proven; code 2 (reverse-subtract, S103 PF: the setter sub_82CA90E0 writes RB_BLENDCONTROL op 4) is proven too; only codes above 2 stay unproven");
    const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const CameraBasis cam = BasisFromView(view);
    ItemSpec a, b, c;
    a.blend = 1, b.blend = 0, c.blend = 3;   // sbyte 0, zint 0, pivot 4 are the log's values (the ItemSpec defaults)
    FakeMem m;
    BuildEntry(m, 0x3000, {a, b, c}, true);
    ParticleSnapshot sn;
    ReaderStats st;
    ReadEntry(m, 0x3000, cam, sn, st);
    Check(st.blend_read == 3 && st.blend_unproven == 1, "reader: blend_unproven counts the items whose code is not proven (0 / 1 / 2 proven; 3 not)", std::to_string(st.blend_unproven));
  }
  // (2) The namer: handle id -> registry -> wrapper -> header -> fetch dwords -> content -> prtcls name. The cached pointer is passed as 0 on purpose.
  {
    NameIndex idx;
    const std::vector<uint8_t> dir = F4Directory({{"Bo03", 10}, {"Bo_01", 10}, {"Light0001", 40}, {"Smoke0001", 90}});
    const uint32_t n = idx.AddSpriteDirectory(dir.data(), dir.size(), "pac\\prtcls.pac", "p0107/2/1");
    const std::vector<uint8_t> dir2 = F4Directory({{"Light0001", 41}});   // a second directory with the SAME name and different bytes
    idx.AddSpriteDirectory(dir2.data(), dir2.size(), "pac\\prtcls.pac", "p0139/5/1");
    Check(n == 3 && idx.size() == 4 && idx.duplicates() == 1 && idx.Find(1) == nullptr, "namer: a sprite directory indexes by mip-0 content; equal bytes (Bo03 / Bo_01) are one entry, the first name wins; the same name with other bytes in another directory is a separate entry");
    F4Hasher hs;
    hs.bytes[wwe2k14::guest_view::ToPhysical(0xAA0F0001u)] = F4Block(40);    // the texture the handle's header points at: Light0001's bytes
    hs.bytes[wwe2k14::guest_view::ToPhysical(0xAA0F0002u)] = F4Block(200);   // bytes no directory holds
    hs.bytes[wwe2k14::guest_view::ToPhysical(0xAA0F0003u)] = F4Block(41);    // the other Light0001 (same name, other directory, other bytes)
    FakeMem m;
    m.w[kTextureHandleGlobal] = 0x50000;
    m.w[0x50000 + 8] = 0x51000, m.w[0x50000 + 16] = 0x51000 + 8 * 0x400;   // the registry: 0x400 entries
    const uint32_t id = 0x091903BDu;   // the log's slot-33 handle, index 957
    const uint32_t wrapper = 0xAA0E70D0u, header = 0xAA0E7400u;
    m.w[0x51000 + 8 * (id & 0xFFFF)] = id, m.w[0x51000 + 8 * (id & 0xFFFF) + 4] = wrapper;
    m.w[wrapper] = kWrapperVtableA, m.w[wrapper + 52] = 0, m.w[wrapper + 80] = header;
    for (uint32_t i = 0; i < 6; ++i) m.w[header + 28 + 4 * i] = 0x1000 + i;
    m.w[header + 28 + 4] = 0xAA0F0001u;   // fetch dword 1 (the base address) names the "guest memory" above
    HandleTextureNamer namer(m, hs, idx);
    std::string pac, entry;
    Check(namer.Name(id, 0, pac, entry) && pac == "pac\\prtcls.pac" && entry == "p0107/2/1/Light0001",
          "namer: the log's handle 0x091903BD is named from its content, with the cached pointer passed as 0 (resolved by handle id, never the cached pointer)", pac + "/" + entry);
    std::string pac2, entry2;
    const bool again = namer.Name(id, 0xDEAD0000u, pac2, entry2);
    Check(again && entry2 == "p0107/2/1/Light0001" && namer.stats().cache_hits >= 1 && namer.stats().named == 2, "namer: a second ask is a cache hit (keyed by id, wrapper and base dword; a different cached pointer changes nothing)");
    m.w[header + 28 + 4] = 0xAA0F0002u;   // the registry slot now holds other content: the cache entry is stale
    Check(!namer.Name(id, 0, pac2, entry2) && namer.stats().not_in_index == 1, "namer: changed content is read again, and bytes no directory holds stay unnamed (counted not_in_index, never a guess)");
    m.w[header + 28 + 4] = 0xAA0F0003u;
    Check(namer.Name(id, 0, pac2, entry2) && entry2 == "p0139/5/1/Light0001", "namer: two same-named textures with different bytes get different entries (the leaf path is part of the entry)", entry2);
    m.w[header + 28 + 4] = 0xAA0F0002u;
    const uint64_t asks_before = namer.stats().not_in_index;
    namer.Name(id, 0, pac2, entry2);
    Check(namer.stats().not_in_index == asks_before + 1, "namer: a miss is not cached (asked again, counted again)");
    m.w[header + 28 + 4] = 0xAA0F0099u;
    Check(!namer.Name(id, 0, pac2, entry2) && namer.stats().no_key == 1, "namer: a texture the hasher cannot read stays unnamed (counted no_key)");
    m.w[header + 28 + 4] = 0xAA0F0001u;
    Check(namer.Name(id, 0, pac2, entry2) && entry2 == "p0107/2/1/Light0001", "namer: the old content comes back by itself");
    m.w[0x51000 + 8 * (id & 0xFFFF)] = id + 0x10000;   // a registry entry that belongs to another handle
    Check(!namer.Name(id, 0, pac2, entry2) && namer.stats().no_handle == 1, "namer: a registry entry with another id is not this handle (counted no_handle)");
    m.w[0x51000 + 8 * (id & 0xFFFF)] = id;
    m.w[wrapper + 52] = 1;
    Check(!namer.Name(id, 0, pac2, entry2) && namer.stats().not_2d == 1, "namer: a cube wrapper (kind 1) is not a sprite texture (counted not_2d)");
    m.w[wrapper + 52] = 0;
    m.w[wrapper] = 0x82057280u;   // some other class's vtable at the entry pointer: not a texture wrapper
    Check(!namer.Name(id, 0, pac2, entry2) && namer.stats().not_2d == 2, "namer: an entry whose vtable is not the wrapper class (0x8206ADB8 / 0x82210B40) is not read (counted not_2d)");
    m.w[wrapper] = kWrapperVtableB;
    Check(namer.Name(id, 0, pac2, entry2), "namer: the wrapper class's second vtable copy (0x82210B40) is accepted");
    m.w[wrapper] = kWrapperVtableA;
    m.w[wrapper + 80] = 0;
    Check(!namer.Name(id, 0, pac2, entry2) && namer.stats().no_header == 1, "namer: a wrapper with no texture header is unnamed (counted no_header)");
    m.w[wrapper + 80] = header;
    // (3) Through the reader: the slot-33 entry {0x091903BD, 0xAA0E70D0} of an item with the log's other values names the key; the pass has no reasons at all.
    const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const CameraBasis cam = BasisFromView(view);
    ItemSpec a;
    a.blend = 1;
    FakeMem im = m;
    BuildEntry(im, 0x3000, {a}, true);
    const uint32_t tab = 0x10000 + 0x1700;
    im.w[tab] = id, im.w[tab + 4] = wrapper;
    HandleTextureNamer nm2(im, hs, idx);
    ParticleSnapshot sn;
    ReaderStats st;
    ReadEntry(im, 0x3000, cam, sn, st, &nm2);
    Check(sn.keys.size() == 1 && sn.keys[0].entry == "p0107/2/1/Light0001" && sn.keys[0].pac == "pac\\prtcls.pac" && sn.keys[0].blend == kParticleAdditive && st.tex_named == 1 && st.tex_unresolved == 0 && st.blend_unproven == 0,
          "reader + namer: an item with the log's values (code 1, handle 0x091903BD) becomes an additive batch of Light0001 with nothing unproven");
    SceneTables t;
    ParticleContributor::WriteTables(sn, 5, t);
    Check(t.particles.batches.size() == 1 && t.particles.batches[0].texture != kNoParticleTexture && t.assets.size() == 1 && t.particles.batches[0].blend == kParticleAdditive && t.particles.status.empty(),
          "reader + namer: the record gets one Texture AssetRef and no status (a whole pass)");
  }
  // (4) S103 PF: the dev-only miss diagnostic. Off by default: no lines. On: at most 8 lines however many misses, each naming the handle, the key, the fetch size / format / address, the head bytes and 3 nearest keys.
  {
    NameIndex idx;
    const std::vector<uint8_t> dir = F4Directory({{"Bo03", 10}, {"Light0001", 40}, {"Smoke0001", 90}, {"Fire0001", 7}});
    idx.AddSpriteDirectory(dir.data(), dir.size(), "pac\\prtcls.pac", "p0107/2/1");
    F4Hasher hs;
    hs.bytes[wwe2k14::guest_view::ToPhysical(0xAA0F0002u)] = F4Block(200);   // bytes no directory holds
    FakeMem m;
    m.w[kTextureHandleGlobal] = 0x50000;
    m.w[0x50000 + 8] = 0x51000, m.w[0x50000 + 16] = 0x51000 + 8 * 0x400;
    const uint32_t id = 0x091903BDu, wrapper = 0xAA0E70D0u, header = 0xAA0E7400u;
    m.w[0x51000 + 8 * (id & 0xFFFF)] = id, m.w[0x51000 + 8 * (id & 0xFFFF) + 4] = wrapper;
    m.w[wrapper] = kWrapperVtableA, m.w[wrapper + 52] = 0, m.w[wrapper + 80] = header;
    for (uint32_t i = 0; i < 6; ++i) m.w[header + 28 + 4 * i] = 0x1000 + i;
    m.w[header + 28 + 4] = 0xAA0F0002u;
    m.w[header + 28 + 8] = 3u | (3u << 13);   // size 4 x 4
    HandleTextureNamer off(m, hs, idx);
    std::string pac, entry;
    for (int i = 0; i < 12; ++i) off.Name(id, 0, pac, entry);
    Check(off.TakeDiag().empty() && off.stats().diag_lines == 0 && off.stats().not_in_index == 12, "namer diag: off by default (the glue turns it on under IsDevBuild only): 12 misses, no line");
    HandleTextureNamer on(m, hs, idx);
    on.EnableDiag(true);
    for (int i = 0; i < 12; ++i) on.Name(id, 0, pac, entry);
    const std::vector<std::string> lines = on.TakeDiag();
    size_t nearest = 0;
    for (char ch : lines.empty() ? std::string() : lines[0]) nearest += ch == '\n';
    Check(lines.size() <= kMaxDiagLines && lines.size() == 8 && on.stats().diag_lines == 8 && on.stats().not_in_index == 12 && nearest == 3 &&
              lines[0].find("handle 091903BD") != std::string::npos && lines[0].find("fetch size 4x4") != std::string::npos && lines[0].find("nearest: key=") != std::string::npos,
          "namer diag: 12 misses -> at most 8 diagnostic lines, each with the handle, key, fetch size / format / address and the 3 nearest index keys", std::to_string(lines.size()));
  }
  g_reader_plant = 0;
  g_namer_plant = 0;
}

// ---- 4. the hook drawn ------------------------------------------------------------------------------------------------------------
struct Px { float r, g, b; };

// CPU model of a flat colour over the clear colour: alpha blend or additive, the effect's texture x vertex colour.
Px Blend(Px dst, Px src, float a, bool additive) {
  if (additive) return {std::min(1.f, dst.r + src.r * a), std::min(1.f, dst.g + src.g * a), std::min(1.f, dst.b + src.b * a)};
  return {src.r * a + dst.r * (1 - a), src.g * a + dst.g * (1 - a), src.b * a + dst.b * (1 - a)};
}

void TestDraw(const std::string& fault, const std::string& sheet) {
  constexpr uint32_t W = 128, H = 128;
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { Check(false, "draw: a headless renderer", err); return; }
  FrontEnd fe(*renderer);
  // A real texture: 1x1 RGBA8 (128, 255, 64): the effect's output is texture x vertex colour, so a batch that names it is tinted differently from the white stand-in.
  rdr::ImageDesc td;
  td.width = td.height = 1;
  td.format = rdr::Format::RGBA8;
  td.usage = rdr::kUsageSampled;
  const rdr::ImageId tex_image = renderer->CreateImage(td);
  const uint8_t tex_px[4] = {128, 255, 64, 255};
  renderer->UpdateImage(tex_image, 0, 0, tex_px, 4);
  ParticleFaults pf;
  pf.additive_off = fault == "additive_off";
  pf.no_mirror = fault == "no_mirror";
  LiveParticles lp(*renderer, [&](uint32_t asset) { return asset == 0 ? tex_image : rdr::kNoImage; }, pf);
  const uint32_t frame = 777;
  lp.SetFrame(frame);
  ParticleTables pt;
  FillTables(pt, fault == "wrong_frame" ? frame + 1 : frame, 0);
  SceneFrame sf;
  sf.width = W, sf.height = H;
  sf.viewer_space = false;  // the tables are in the game's space
  sf.clear[0] = 0.1f, sf.clear[1] = 0.2f, sf.clear[2] = 0.3f, sf.clear[3] = 1.f;
  sf.camera.view = IdentityMat4();
  sf.camera.proj = IdentityMat4();
  sf.particles = &pt;
  sf.stage_hooks.push_back(&lp);
  std::vector<uint8_t> rgba;
  const bool ok = fe.RenderToPixels(sf, rgba, err);
  Check(ok, "draw: the frame renders with the particle hook", err);
  if (!ok) return;
  Check(fe.validation_errors() == 0, "draw: 0 validation errors", std::to_string(fe.validation_errors()));
  std::printf("[f_test] %s\n", lp.CensusLine().c_str());

  // The expected picture. One orientation only: clip y is UP (clip +1 is the top row), as the front-end's `system[1] = -1` makes it; a flipped picture must fail.
  const Px clear = {0.1f, 0.2f, 0.3f};
  const float a = 128.f / 255.f;
  const Px tex = {128.f / 255.f, 1.f, 64.f / 255.f};
  const auto src_of = [&](size_t particle) {  // batch A (particles 0, 1) and C (3) sample the texture; B (2) the white stand-in. Lit (C): brightness 0.2 (the sprite normal is zero: max(0, 0.1) * 2)
    const Px vcol = {1.f, 128.f / 255.f, 0.f};
    if (particle == 2) return vcol;
    Px s = {vcol.r * tex.r, vcol.g * tex.g, vcol.b * tex.b};
    if (particle == 3) s = {s.r * 0.2f, s.g * 0.2f, s.b * 0.2f};
    return s;
  };
  std::vector<uint8_t> expect(size_t(W) * H * 4);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const float cxp = (float(x) + 0.5f) / W * 2.f - 1.f, cyp = 1.f - (float(y) + 0.5f) / H * 2.f;
      Px p = clear;
      for (size_t i = 0; i < pt.particles.size(); ++i) {
        const Particle& q = pt.particles[i];
        const float h = q.local_world[0], cx = q.local_world[3], cy = q.local_world[7];
        if (std::fabs(cxp - cx) <= h && std::fabs(cyp - cy) <= h) p = Blend(p, src_of(i), a, i == 2);
      }
      uint8_t* o = &expect[(size_t(y) * W + x) * 4];
      o[0] = uint8_t(std::lround(std::min(1.f, p.r) * 255)), o[1] = uint8_t(std::lround(p.g * 255)), o[2] = uint8_t(std::lround(p.b * 255)), o[3] = 255;
    }
  uint32_t worst = 0, bad = 0;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x)
      for (int c = 0; c < 3; ++c) {
        const int d = std::abs(int(rgba[(size_t(y) * W + x) * 4 + size_t(c)]) - int(expect[(size_t(y) * W + x) * 4 + size_t(c)]));
        worst = std::max<uint32_t>(worst, uint32_t(d));
        if (d > 3) { ++bad; break; }
      }
  // Edge pixels may differ by a fraction of a pixel (coverage rule): allow a thin band, never a region.
  Check(bad <= 4 * (W + H) / 2, "draw: the hook's picture equals the CPU model of the tables (textured, white, additive and lit batches; y up)",
        "pixels off by > 3 levels: " + std::to_string(bad) + ", worst " + std::to_string(worst));
  const auto centre = [&](float cx, float cy, uint8_t* out) {  // the pixel at a clip position (y up)
    const uint32_t px = uint32_t((cx + 1.f) / 2.f * W), py = uint32_t((1.f - cy) / 2.f * H);
    std::memcpy(out, &rgba[(size_t(py) * W + px) * 4], 3);
  };
  const auto near3 = [](const uint8_t* got, Px want) {
    return std::abs(int(got[0]) - int(std::lround(want.r * 255))) <= 3 && std::abs(int(got[1]) - int(std::lround(want.g * 255))) <= 3 && std::abs(int(got[2]) - int(std::lround(want.b * 255))) <= 3;
  };
  uint8_t got[3];
  centre(0.5f, 0.0f, got);
  Check(near3(got, Blend(clear, src_of(2), a, true)), "draw: the additive quad adds (centre pixel)", std::to_string(got[0]) + "," + std::to_string(got[1]) + "," + std::to_string(got[2]));
  centre(-0.6f, 0.0f, got);
  Check(near3(got, Blend(clear, src_of(0), a, false)), "draw: the textured alpha quad takes the texture's colour (centre pixel)");
  centre(0.0f, -0.6f, got);
  Check(near3(got, Blend(clear, src_of(3), a, false)), "draw: the lit quad is dimmed by the effect's light (centre pixel, y up)");
  Check(lp.stats().draws == 3 && lp.stats().particles == 4 && lp.stats().lit_particles == 1 && lp.stats().no_texture == 1,
        "draw: draws are one per unlit batch plus one per lit particle (3 draws, 4 particles, 1 lit, 1 white stand-in)", lp.CensusLine());
  if (!sheet.empty()) {  // sheet: expected | ours, side by side
    std::vector<uint8_t> both(size_t(W) * 2 * H * 4, 255);
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        std::memcpy(&both[(size_t(y) * W * 2 + x) * 4], &expect[(size_t(y) * W + x) * 4], 4);
        std::memcpy(&both[(size_t(y) * W * 2 + W + x) * 4], &rgba[(size_t(y) * W + x) * 4], 4);
      }
    WritePng(sheet, both, W * 2, H);
  }
  // The same tables in a VIEWER-SPACE frame (what rf::BuildFrame makes: viewer_space = true, view' = S * view with S = diag(1, -1, 1)): the hook mirrors once and the picture is the same.
  {
    SceneFrame vf = sf;
    vf.viewer_space = true;
    vf.camera.view = IdentityMat4();
    vf.camera.view.m[5] = -1.f;  // S
    std::vector<uint8_t> vrgba;
    const bool vok = fe.RenderToPixels(vf, vrgba, err);
    Check(vok, "draw (viewer space): the frame renders", err);
    uint32_t vbad = 0, vworst = 0;
    for (size_t i = 0; vok && i < size_t(W) * H; ++i)
      for (int c = 0; c < 3; ++c) {
        const int d = std::abs(int(vrgba[i * 4 + size_t(c)]) - int(expect[i * 4 + size_t(c)]));
        vworst = std::max<uint32_t>(vworst, uint32_t(d));
        if (d > 3) { ++vbad; break; }
      }
    Check(vok && vbad <= 4 * (W + H) / 2, "draw (viewer space): the same picture as the game-space frame (Y mirrored once; lit and unlit batches)",
          "pixels off by > 3 levels: " + std::to_string(vbad) + ", worst " + std::to_string(vworst));
    Check(lp.stats().viewer_space_mirrored == 1, "draw (viewer space): the mirrored frame is counted", std::to_string(lp.stats().viewer_space_mirrored));
  }
  renderer->DestroyImage(tex_image);
}

// ---- 5. S103 PRT: a soft batch picks the soft program, copies our scene depth and fades by the gap to it ---------------------------------------
// A perspective camera (near 1, far 100; row vectors: clip.z = A z + B, clip.w = z). The scene is empty: the depth is the cleared far value, so the scene distance is 100. A particle at view z 70 with range 50: gap = min(|100 - 70|, 50) = 30,
// alpha x 0.6; the same particle hard keeps its full alpha. The fault `soft_off` draws the soft batch hard (the soft checks must then fail).
void TestSoft(const std::string& fault) {
  constexpr uint32_t W = 128, H = 128;
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { Check(false, "soft: a headless renderer", err); return; }
  FrontEnd fe(*renderer);
  rdr::ImageDesc td;
  td.width = td.height = 1;
  td.format = rdr::Format::RGBA8;
  td.usage = rdr::kUsageSampled;
  const rdr::ImageId tex_image = renderer->CreateImage(td);
  const uint8_t tex_px[4] = {128, 255, 64, 255};
  renderer->UpdateImage(tex_image, 0, 0, tex_px, 4);
  LiveParticles lp(*renderer, [&](uint32_t asset) { return asset == 0 ? tex_image : rdr::kNoImage; });
  const uint32_t frame = 900;
  lp.SetFrame(frame);
  const auto render = [&](uint8_t soft_code, std::vector<uint8_t>& rgba) {
    ParticleTables pt;
    ParticleBatch b;
    b.first = 0, b.count = 1, b.texture = 0, b.blend = kParticleAlpha, b.frame = frame, b.soft = fault == "soft_off" ? uint8_t(0) : soft_code;
    pt.batches = {b};
    Particle p = MakeParticle(0.f, 0.f, 14.f, 0x800080FFu);
    p.local_world[11] = 70.f;
    pt.particles = {p};
    SceneFrame sf;
    sf.width = W, sf.height = H;
    sf.viewer_space = false;
    sf.clear[0] = 0.1f, sf.clear[1] = 0.2f, sf.clear[2] = 0.3f, sf.clear[3] = 1.f;
    sf.camera.view = IdentityMat4();
    sf.camera.proj = IdentityMat4();
    const float n = 1.f, f = 100.f;
    sf.camera.proj.m[10] = f / (f - n), sf.camera.proj.m[11] = 1.f, sf.camera.proj.m[14] = -n * f / (f - n), sf.camera.proj.m[15] = 0.f;
    sf.particles = &pt;
    sf.stage_hooks.push_back(&lp);
    return fe.RenderToPixels(sf, rgba, err);
  };
  std::vector<uint8_t> soft_px, hard_px;
  const bool ok1 = render(200, soft_px);   // range 200 / 4 = 50
  const LiveParticlesStats after_soft = lp.stats();
  const bool ok2 = render(0, hard_px);
  Check(ok1 && ok2, "soft: both frames render", err);
  if (!ok1 || !ok2) return;
  Check(fe.validation_errors() == 0, "soft: 0 validation errors (the depth copy ends the pass, the next draw loads)", std::to_string(fe.validation_errors()));
  Check(after_soft.soft_drawn == 1 && after_soft.soft_depth_copies == 1 && after_soft.soft_as_hard == 0, "soft: the soft batch is drawn with the soft program after one depth copy", lp.CensusLine());
  const size_t mid = (size_t(H / 2) * W + W / 2) * 4;
  const Px clear = {0.1f, 0.2f, 0.3f};
  const Px s = {(128.f / 255.f), (128.f / 255.f), 0.f};   // vertex colour (1, 128/255, 0) x texture (128/255, 1, 64/255)
  const float a = 128.f / 255.f;
  const Px want_soft = Blend(clear, s, a * 0.6f, false), want_hard = Blend(clear, s, a, false);
  const auto near3 = [&](const std::vector<uint8_t>& px, size_t at, Px want) {
    return std::abs(int(px[at]) - int(std::lround(want.r * 255))) <= 3 && std::abs(int(px[at + 1]) - int(std::lround(want.g * 255))) <= 3 && std::abs(int(px[at + 2]) - int(std::lround(want.b * 255))) <= 3;
  };
  Check(near3(soft_px, mid, want_soft), "soft: the centre pixel is faded by gap / range = 30 / 50 against OUR depth (linearised with our projection)",
        std::to_string(soft_px[mid]) + "," + std::to_string(soft_px[mid + 1]) + "," + std::to_string(soft_px[mid + 2]) + " want " + std::to_string(int(std::lround(want_soft.r * 255))) + "," + std::to_string(int(std::lround(want_soft.g * 255))));
  Check(near3(hard_px, mid, want_hard), "soft: the same particle hard keeps its full alpha");
  {   // S103 PRT fix 2: the switch off draws a soft batch hard, with no depth copy
    std::vector<uint8_t> off_px;
    rdr_scene::g_soft_particles.store(false);
    const bool ok3 = render(200, off_px);
    rdr_scene::g_soft_particles.store(true);
    Check(ok3 && lp.stats().soft_depth_copies == after_soft.soft_depth_copies && lp.stats().soft_as_hard == after_soft.soft_as_hard + 1 && near3(off_px, mid, want_hard), "soft: the switch off draws a soft batch hard, with no depth copy", lp.CensusLine());
  }
  renderer->DestroyImage(tex_image);
}

}  // namespace

int main(int argc, char** argv) {
  std::string fault, sheet;
  bool gpu = true;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--fault") && i + 1 < argc) fault = argv[++i];
    else if (!std::strcmp(argv[i], "--sheet") && i + 1 < argc) sheet = argv[++i];
    else if (!std::strcmp(argv[i], "--no-gpu")) gpu = false;
  }
  TestOrder(fault == "order_distance");
  TestRecord(fault == "bad_index");
  TestContributor(fault == "stale_snapshot");
  TestReader(fault);
  TestReaderF3(fault);
  TestNamerF4(fault);
  TestSource(fault);
  if (gpu) TestDraw(fault, sheet);
  if (gpu) TestSoft(fault);
  std::printf("[f_test] %d checks, %d failed%s%s\n", g_checks, g_fail, fault.empty() ? "" : ", planted fault: ", fault.c_str());
  return g_fail ? 1 : 0;
}
