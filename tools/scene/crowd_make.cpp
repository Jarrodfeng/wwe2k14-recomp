// crowd_make: the offline driver of the crowd contributor (Session 101, lane 27). 0 game runs: it builds a crowd snapshot FROM THE FILES (the
// viewer's crowd of one arena: the members' matrices, the 20 person types' m2.pac pose as skin palettes), feeds it to the contributor through
// the same calls the game hooks make (BeginDraw / NoteMember / EndDraw / SetPalette), Emits into a SceneTables and writes scene records:
//   crowd_make --game DIR --arena 5 --out DIR [--size 640x360] [--card-frac 0.35]
// writes in DIR: ref.rsc (the viewer's own crowd: the arena asset, drawn with rsc_draw --keep-src-kind 3), clean.rsc (the contributor's
// record), fault_palette.rsc (the palettes left at the bind pose = identity), fault_cards.rsc (the far cards' objects dropped).
// The members that the game would draw as flat cards are a fixture here: stands members past the --card-frac share of the in-view stands
// members by distance (the real rule is N + 3 S of the live camera, crowd_cards.h); the record carries them as card objects and the reader
// draws the type's body for them (C2-1), at the stored pose, as the game's impostor has no palette.
// Exit 0 when the records are written; 3 on any failure (reason on stderr).
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/assets/crowd_body.h"
#include "../../src/scene_live/crowd_contrib.h"
#include "../../src/scene_live/crowd_floor.h"  // S101 lane 82: the seated floor people's decode (pure)
#include "../../src/renderer_scene/record_io.h"

using namespace rdr_scene;
namespace crowd = rdr_scene::live::crowd;

namespace {
using Mat = std::array<float, 16>;

Mat MulM(const Mat& a, const Mat& b) {  // row vectors: p * a * b
  Mat r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[size_t(i * 4 + j)] += a[size_t(i * 4 + k)] * b[size_t(k * 4 + j)];
  return r;
}

void Norm(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-9f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// D3D style, left-handed, row vectors, depth 0..1 (SCENE_RECORD "Space and projection"): the combined view-projection.
Mat LookAtViewProj(const float eye[3], const float target[3], float fov_y_deg, float aspect, float zn, float zf) {
  float z[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  Norm(z);
  const float up[3] = {0, 1, 0};
  float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
  Norm(x);
  const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
  const Mat view = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
                    -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]),
                    -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1};
  const float ys = 1.f / std::tan(fov_y_deg * 3.14159265f / 360.f), xs = ys / aspect;
  const Mat proj = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  return MulM(view, proj);
}

// game = true: the GAME's camera (a Live record is in game space, -Y up: clip = p_game * S * VP, S = diag(1, -1, 1)): row 1 of VP negated, eye y negated.
void SetCamera(SceneTables& t, const Mat& vp, const float eye_in[3], float fov, bool game = false) {
  t.has_camera = true;
  Mat m = vp;
  float eye[3] = {eye_in[0], eye_in[1], eye_in[2]};
  if (game) {
    for (int k = 4; k < 8; ++k) m[size_t(k)] = -m[size_t(k)];
    eye[1] = -eye[1];
  }
  std::memcpy(t.camera.view.m, m.data(), sizeof(float) * 16);
  t.camera.proj = IdentityMat4();
  t.camera.flags = kCamViewIsViewProj;
  std::memcpy(t.camera.eye, eye, sizeof(float) * 3);
  t.camera.fov_y_deg = fov;
  t.camera.near_z = 10.f, t.camera.far_z = 60000.f;
}

bool Write(const std::filesystem::path& dir, const char* name, const SceneTables& t) {
  std::string err;
  if (!WriteRecord((dir / name).string(), t, err)) { std::fprintf(stderr, "crowd_make: FAIL writing %s: %s\n", name, err.c_str()); return false; }
  std::fprintf(stderr, "[crowd_make] wrote %s\n", (dir / name).string().c_str());
  return true;
}

struct Pick {
  assets::crowd_body::Member m;
  uint32_t index;   // ordinal inside the type (the stand-in for the game's array index)
  float dist;
  bool card = false;
  uint32_t lod = 0;  // fixture: some mid-distance stands members are LOD 1
};
}  // namespace


// The contributor's contract, with no game files: the properties the review asked for (each fails on the code before the lane 27 review fixes).
int Contract() {
  int bad = 0;
  auto check = [&](bool ok, const char* what) { std::fprintf(stderr, "[crowd_make] contract %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };
  crowd::Contributor& c = crowd::The();
  auto member = [](uint32_t block, uint32_t lod, uint32_t index) {
    crowd::MemberObs o;
    o.block = block, o.lod = lod, o.index = index;
    o.c[0] = o.c[5] = o.c[10] = 1.f;
    return o;
  };
  float ident[16 * 2];
  for (int i = 0; i < 32; ++i) ident[i] = (i % 16) % 5 == 0 ? 1.f : 0.f;
  auto emit = [&](live::Counters& cnt) { SceneTables t; live::FrameInfo fi; c.Emit(fi, t, cnt); return t; };
  {  // 1. a full-size record's strings: AddString is a hash lookup (6,600 ids in a few ms, not ~100 ms)
    SceneTables t;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 6600; ++i) t.AddString("crowd:block:" + std::to_string(i % 20) + ":member:" + std::to_string(i));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[crowd_make] 6600 AddString calls: %.2f ms\n", ms);
    check(ms < 40.0, "6,600 AddString calls under 40 ms (the linear scan took ~100 ms)");
    check(t.AddString("crowd:block:3:member:3") == t.AddString("crowd:block:3:member:3") && t.AddString("") == 0, "equal strings share one field");
  }
  {  // 2. stale snapshots: two repeats, then nothing (counted once); a new draw resumes
    c.Reset(); c.SetArena(5);
    c.SetPalette(0, 1, 0, ident, 2);
    c.BeginDraw(0); c.NoteMember(member(1, 0, 4)); c.EndDraw();
    live::Counters a, b, d, e;
    emit(a); emit(b); emit(d); emit(e);
    check(a.objects == 1 && b.objects == 1 && d.objects == 1 && e.objects == 0 && d.stale + e.stale == 1, "stale: the snapshot repeats twice, then nothing, counted once");
    c.BeginDraw(0); c.NoteMember(member(1, 0, 4)); c.EndDraw();
    live::Counters f; emit(f);
    check(f.objects == 1, "a new crowd draw emits again");
    c.SetArena(6);
    live::Counters g; SceneTables tg = emit(g);
    check(g.objects == 0 && tg.objects.empty(), "an arena change drops the old arena's snapshot");
  }
  {  // 3. the palette set of the ring the draw reads (the pose is one update ahead of the draw's ring)
    c.Reset(); c.SetArena(5);
    float marked[16];
    for (int i = 0; i < 16; ++i) marked[i] = (i % 5 == 0) ? 2.f : 0.f;
    c.SetPalette(0, 2, 0, ident, 1);   // ring 0: identity
    c.SetPalette(1, 2, 0, marked, 1);  // ring 1: scale 2
    c.BeginDraw(1); c.NoteMember(member(2, 0, 0)); c.EndDraw();
    live::Counters cnt; SceneTables t = emit(cnt);
    check(t.bones.size() == 1 && t.bones[0].m[0] == 2.f, "the draw's read ring (1) picks the palette set the skin loops wrote to ring 1");
  }
  {  // 4. unmodelled populations are counted, unknown member indices get unique ids, LOD 1 / 2 members are recorded
    c.Reset(); c.SetArena(5);
    c.SetPalette(0, 3, 0, ident, 2);
    c.BeginDraw(0);
    c.NoteMember(member(3, 0, crowd::kUnknown)); c.NoteMember(member(3, 0, crowd::kUnknown)); c.NoteMember(member(3, 1, 7)); c.NoteMember(member(3, 2, 8));
    c.NoteUnmodelled(); c.NoteUnmodelled();
    c.EndDraw();
    live::Counters cnt; SceneTables t = emit(cnt);
    bool unique = t.objects.size() == 4;
    for (size_t i = 0; i < t.objects.size(); ++i)
      for (size_t j = i + 1; j < t.objects.size(); ++j) unique = unique && t.objects[i].id != t.objects[j].id;
    check(unique, "unknown member indices get unique ids");
    check(cnt.unknown >= 2 + 2, "two unmodelled-population members and two unreadable indices counted unknown");
    check(t.objects.size() == 4 && t.objects[2].layer == 1 && t.objects[3].layer == 2, "LOD 1 / 2 members are recorded with their group");
  }
  {  // 5. S101 lane 82: the seated floor people (crowd_floor.h decode with a fake memory, then the contributor)
    struct Fake {  // guest memory: the group object at 0x1000 (count +20, type +24, elements pointer +64), 3 elements of 176 bytes at 0x2000 (type byte at +23)
      uint8_t mem[0x4000] = {};
      uint32_t u32(uint32_t a) const { return a + 4 <= sizeof mem ? uint32_t(mem[a]) << 24 | uint32_t(mem[a + 1]) << 16 | uint32_t(mem[a + 2]) << 8 | mem[a + 3] : 0; }
      uint8_t u8(uint32_t a) const { return a < sizeof mem ? mem[a] : 0; }
      float f32(uint32_t a) const { uint32_t v = u32(a); float f; std::memcpy(&f, &v, 4); return f; }
      void set32(uint32_t a, uint32_t v) { mem[a] = uint8_t(v >> 24), mem[a + 1] = uint8_t(v >> 16), mem[a + 2] = uint8_t(v >> 8), mem[a + 3] = uint8_t(v); }
    } fm;
    fm.set32(0x1000 + 20, 3), fm.set32(0x1000 + 24, 7), fm.set32(0x1000 + 64, 0x2000);
    for (int i = 0; i < 3; ++i) fm.mem[0x2000 + 176 * i + 23] = 7;
    for (int k = 0; k < 12; ++k) { float v = float(k) + 0.5f; uint32_t b; std::memcpy(&b, &v, 4); fm.set32(0x3000 + 4 * k, b); }
    using crowd::FloorReject;
    crowd::MemberObs o;
    // r4 76, r5 constants, r6 3, r26 object, r29 member index, r31 = 28 * g (g = 20 * LOD + type; LOD 0 type 7 -> g 7)
    check(crowd::DecodeFloorMember(76, 0x3000, 3, 0x1000, 2, 28 * 7, fm, o) == FloorReject::None && o.floor && o.block == 7 && o.lod == 0 && o.index == 2 && o.c[11] == 11.5f, "floor: a valid call decodes (type 7, member 2, the 12 constants)");
    check(crowd::DecodeFloorMember(76, 0x3000, 3, 0x1000, 2, 28 * 27, fm, o) == FloorReject::Lod, "floor: a LOD 1 group (g 27) is refused, not recorded");
    check(crowd::DecodeFloorMember(76, 0x3000, 3, 0x1000, 2, 28 * 8, fm, o) == FloorReject::Group, "floor: g % 20 != the object's type is refused");
    check(crowd::DecodeFloorMember(76, 0x3000, 3, 0x1000, 3, 28 * 7, fm, o) == FloorReject::Index, "floor: a member index past the object's count is refused");
    fm.mem[0x2000 + 176 * 1 + 23] = 9;
    check(crowd::DecodeFloorMember(76, 0x3000, 3, 0x1000, 1, 28 * 7, fm, o) == FloorReject::Element, "floor: an element whose type byte disagrees with the object's is refused");
    check(crowd::DecodeFloorMember(75, 0x3000, 3, 0x1000, 0, 28 * 7, fm, o) == FloorReject::Regs, "floor: r4 != 76 is refused");
    // the contributor: ids, LOD 0 asset, the type's LOD 0 palette, no palette -> not skinned; a floor member does not take a block's name
    c.Reset(); c.SetArena(4);
    c.SetPalette(0, 7, 0, ident, 2);
    c.BeginDraw(0);
    crowd::MemberObs a = member(7, 0, 5), b = member(7, 0, 5), n = member(9, 0, 1);
    a.floor = true, n.floor = true;
    c.NoteMember(a), c.NoteMember(b), c.NoteMember(n);
    c.EndDraw();
    live::Counters cnt; SceneTables t = emit(cnt);
    auto id_of = [&](size_t i) { return std::string(t.Str(t.objects[i].id)); };
    check(t.objects.size() == 3 && id_of(0) == "crowd:floor:7:member:5" && id_of(1) == "crowd:block:7:member:5" && id_of(0) != id_of(1), "floor: id crowd:floor:TT:member:I, distinct from the block member of the same type and index");
    check(t.objects.size() == 3 && t.objects[0].layer == 0 && t.objects[0].asset == t.objects[1].asset && (t.objects[0].flags & kObjSkinned) && t.objects[0].bones_count == 2 && t.objects[0].bones_first == t.objects[1].bones_first, "floor: the type's LOD 0 body asset and LOD 0 palette (shared with the block member)");
    check(t.objects.size() == 3 && !(t.objects[2].flags & kObjSkinned), "floor: a type with no LOD 0 palette is recorded unskinned (the seam keeps its stream draw)");
  }
  return bad;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--contract") return Contract() ? 3 : 0;
  std::string game, out;
  int arena = 5;
  uint32_t w = 640, h = 360;
  float card_frac = 0.35f;
  bool bench = false;  // all members, no cull: the Emit time of a full crowd record
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--bench") bench = true;
    else if (a == "--arena" && i + 1 < argc) arena = std::atoi(argv[++i]);
    else if (a == "--card-frac" && i + 1 < argc) card_frac = float(std::atof(argv[++i]));
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) { std::fprintf(stderr, "bad --size\n"); return 1; } }
  }
  if (game.empty() || out.empty()) { std::fprintf(stderr, "usage: crowd_make --game DIR --arena N --out DIR [--size WxH] [--card-frac F]\n"); return 1; }
  std::string err;
  if (!assets::crowd_body::Init(game)) { std::fprintf(stderr, "crowd_make: FAIL no game files at %s\n", game.c_str()); return 3; }
  std::vector<assets::crowd_body::Member> members;
  if (!assets::crowd_body::Members(arena, members, err)) { std::fprintf(stderr, "crowd_make: FAIL members: %s\n", err.c_str()); return 3; }
  std::vector<std::vector<assets::Matrix>> pal;
  if (!assets::crowd_body::Palettes(arena, 14005, -1, pal, err)) { std::fprintf(stderr, "crowd_make: FAIL palettes: %s\n", err.c_str()); return 3; }
  if (members.empty()) { std::fprintf(stderr, "crowd_make: FAIL the arena has no crowd members\n"); return 3; }

  // The camera: from above the ring (the viewer's space: Y up), towards the middle of the members on the +Z side.
  double cx = 0, cy = 0, cz = 0;
  size_t n_side = 0;
  for (const auto& m : members) {
    if (m.file_world[14] <= 0) continue;
    cx += m.file_world[12], cy += -m.file_world[13], cz += m.file_world[14], ++n_side;
  }
  if (!n_side) n_side = 1;
  const float eye[3] = {0.f, 300.f, 0.f};
  const float target[3] = {float(cx / n_side), float(cy / n_side), float(cz / n_side)};
  const float fov = 50.f;
  const Mat vp = LookAtViewProj(eye, target, fov, float(w) / float(h), 10.f, 60000.f);

  // The members in view (the game culls by its frustum too), with their distance; the far share of the stands ones are the cards.
  std::vector<Pick> picks;
  std::vector<uint32_t> per_type(20, 0);
  for (const auto& m : members) {
    const float px = m.file_world[12], py = -m.file_world[13], pz = m.file_world[14];
    const float cw = px * vp[3] + py * vp[7] + pz * vp[11] + vp[15];
    const float cxx = px * vp[0] + py * vp[4] + pz * vp[8] + vp[12], cyy = px * vp[1] + py * vp[5] + pz * vp[9] + vp[13];
    const uint32_t idx = per_type[size_t(m.type)]++;
    if (!bench && (!(cw > 1.f) || std::fabs(cxx) > 1.25f * cw || std::fabs(cyy) > 1.25f * cw)) continue;
    Pick p;
    p.m = m, p.index = idx;
    p.dist = std::sqrt((px - eye[0]) * (px - eye[0]) + (py - eye[1]) * (py - eye[1]) + (pz - eye[2]) * (pz - eye[2]));
    picks.push_back(p);
  }
  std::vector<float> sd;
  for (const Pick& p : picks) if (p.m.stands) sd.push_back(p.dist);
  std::sort(sd.begin(), sd.end());
  const float card_dist = sd.empty() ? 1e30f : sd[std::min(sd.size() - 1, size_t((1.f - card_frac) * float(sd.size())))];
  size_t cards = 0, lod1 = 0;
  for (Pick& p : picks) if (p.m.stands && p.dist >= card_dist) p.card = true, ++cards;
  for (Pick& p : picks) if (p.m.stands && !p.card && p.index % 5 == 0) p.lod = 1, ++lod1;
  std::fprintf(stderr, "[crowd_make] arena %d: %zu members, %zu in view (%zu stands members become cards past distance %.0f, %zu are LOD 1)\n", arena, members.size(), picks.size(), cards, card_dist, lod1);
  if (picks.empty()) { std::fprintf(stderr, "crowd_make: FAIL no member in view of the test camera\n"); return 3; }

  std::filesystem::create_directories(out);
  const std::filesystem::path dir(out);

  // ref.rsc: the viewer's own crowd (the arena asset; rsc_draw --keep-src-kind 3 keeps its crowd meshes only).
  {
    SceneTables t;
    t.header.source = uint8_t(Source::Viewer);
    SetCamera(t, vp, eye, fov);
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    char pac[64];
    std::snprintf(pac, sizeof pac, "pac\\bg\\bg%02d.pac", arena);
    a.pac = t.AddString(pac);
    t.assets.push_back(a);
    Object o;
    o.asset = 0;
    o.klass = rdr_adapter::DrawClass::ModelPart;
    o.world = IdentityMat4();
    o.id = t.AddString(std::string("arena:") + std::to_string(arena));
    t.objects.push_back(o);
    if (!Write(dir, "ref.rsc", t)) return 3;
  }

  // The contributor, driven as the hooks drive it.
  auto build = [&](const char* name, bool palettes_bind, bool drop_cards, bool only_cards = false) {
    crowd::Contributor& c = crowd::The();
    c.Reset();
    c.SetArena(arena);
    for (uint32_t type = 0; type < 20; ++type) {
      const auto& p = pal[type];
      if (p.empty()) continue;
      std::vector<float> fl(p.size() * 16);
      for (size_t i = 0; i < p.size(); ++i)
        for (size_t k = 0; k < 16; ++k) fl[i * 16 + k] = palettes_bind ? (k % 5 == 0 ? 1.f : 0.f) : p[i][k];
      c.SetPalette(0, type, 0, fl.data(), uint32_t(p.size()));
      c.SetPalette(0, type, 1, fl.data(), uint32_t(p.size()));  // fixture: LOD 1 = the LOD 0 palette (an identity remap), see rsc_draw
    }
    c.BeginDraw(0);
    for (const Pick& p : picks) {
      if (!p.m.stands) continue;  // the floor people are a population the live hooks do not record (op-21 at 0x82518D48 ...): added below as fixture, not claimed
      if (p.card && drop_cards) continue;
      if (only_cards && !p.card) continue;
      crowd::MemberObs o;
      o.block = uint32_t(p.m.type);
      o.lod = p.card ? crowd::kCardLayer : p.lod;
      o.index = p.index;
      o.card = p.card;
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) o.c[r * 4 + k] = p.m.file_world[size_t(k * 4 + r)];  // the constants c76-c78: row r = column r of the matrix
      c.NoteMember(o);
    }
    c.EndDraw();
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.header.guest_frame = 1;
    SetCamera(t, vp, eye, fov, true);
    live::Counters cnt;
    live::FrameInfo fi;
    const auto t0 = std::chrono::steady_clock::now();
    c.Emit(fi, t, cnt);
    const double emit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[crowd_make] %s: Emit of %u objects took %.2f ms\n", name, cnt.objects, emit_ms);
    if (bench || only_cards) return Write(dir, name, t);
    // FIXTURE: the seated floor people (the live hooks see them at other op-21 sites and do not record them): objects of the same bodies and palettes,
    // named "fixture:floor", added here so the picture is comparable to the viewer's crowd; they are NOT part of what the contributor claims.
    {
      std::map<uint32_t, uint32_t> bones_of_type;
      for (const Pick& p : picks) {
        if (p.m.stands) continue;
        char e[32];
        std::snprintf(e, sizeof e, "%d", 20000 + p.m.type);
        const uint32_t entry_f = t.AddString(e);
        uint32_t asset = 0xFFFFFFFFu;
        for (size_t i = 0; i < t.assets.size(); ++i) if (t.assets[i].entry == entry_f) asset = uint32_t(i);
        if (asset == 0xFFFFFFFFu) {
          AssetRef a;
          a.kind = uint8_t(AssetKind::Model);
          char pc[64];
          std::snprintf(pc, sizeof pc, "pac\\bg\\bg%02d.pac", arena);
          a.pac = t.AddString(pc);
          a.entry = entry_f;
          t.assets.push_back(a);
          asset = uint32_t(t.assets.size() - 1);
        }
        Object o;
        o.asset = asset;
        o.klass = rdr_adapter::DrawClass::Crowd;
        for (int i = 0; i < 16; ++i) o.world.m[i] = p.m.file_world[size_t(i)];
        char id[64];
        std::snprintf(id, sizeof id, "fixture:floor:%d:%u", p.m.type, p.index);
        o.id = t.AddString(id);
        const auto& pp = pal[size_t(p.m.type)];
        if (!bones_of_type.count(uint32_t(p.m.type))) {
          bones_of_type[uint32_t(p.m.type)] = uint32_t(t.bones.size());
          for (size_t i = 0; i < pp.size(); ++i) {
            Mat4 m;
            for (size_t k = 0; k < 16; ++k) m.m[k] = palettes_bind ? (k % 5 == 0 ? 1.f : 0.f) : pp[i][k];
            t.bones.push_back(m);
          }
        }
        if (!pp.empty()) {
          o.flags |= kObjSkinned;
          o.bones_first = bones_of_type[uint32_t(p.m.type)], o.bones_count = uint32_t(pp.size());
        }
        t.objects.push_back(o);
      }
    }
    std::fprintf(stderr, "[crowd_make] %s: objects %u, unknown %u, stale %u; assets %zu, palette matrices %zu\n", name, cnt.objects, cnt.unknown, cnt.stale,
                 t.assets.size(), t.bones.size());
    return Write(dir, name, t);
  };
  if (bench) return build("bench.rsc", false, false) ? 0 : 3;
  if (!build("clean.rsc", false, false)) return 3;
  if (!build("fault_palette.rsc", true, false)) return 3;
  if (!build("fault_cards.rsc", false, true)) return 3;
  if (!build("cards_only.rsc", false, false, true)) return 3;  // the far cards alone: what they contribute, for the derived bar
  return 0;
}
