// mp_unit: the offline proof of S103 lane MP (menu particles, docs\research\s103\MP_menu_particles.md; claims out\s103\decomp\claims_MP.jsonl). 0 game runs, no SDK, no game memory: it runs the SHIPPED
// headers (particle_source.h, particle_contrib.*, particle_reader.cpp, particle_tables.h, record_io.cpp, scene_kind.h) on synthetic guest memory.
//   mp_unit [--plant N]      (N = 3..7, the particle_source.h g_source_plant values; mp_check.py runs each and names the check it must fail)
// Checks (each FAIL line names its check):
//   empty.no_call              a frame with no node call: TakeEmpty gives the empty-pass marker; the record carries it; PartsOf = Whole
//   empty.node_call_not_empty  a frame WITH a node call whose words are non-zero must not read empty (planted twin 3: TakeEmpty ignores the call)
//   empty.zero_words           a call whose +3084 or +3088 is 0 (or a null camera object) reads empty = Whole (planted twin 4: the words are ignored)
//   empty.other_frame          a call noted in another frame is no call in this frame
//   marker.not_a_reason        the marker is information: WholePass / Incomplete() / the verdict ignore it; the constant matches scene_kind.h's
//   camera.node_own            the node's own camera (table slot, view * proj) classifies a menu call as a screen call; a perspective one is not; a flat frame never asks the scene camera (planted twin 6)
//   groups.every_call          two node calls of a frame are two groups, both read (planted twin 5: the last only)
//   groups.row_position        each group's batches carry the 2D row count at its call; the record round-trips them (planted twin 7: the row ignored)
//   groups.held                screen groups are Held (kPiScreenCam), never stamped, and the screen camera reaches the record (section 57)
//   match.unchanged            a frame with one non-screen call takes the old path: scene camera, no screen row, no row stamp, the verdict the old one
//   parts.verdict              PartsOf: empty = Whole, screen = Held, anything else NotRecorded (fail closed)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_game/scene_kind.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/particle_contrib.h"
#include "../../src/scene_live/particle_reader.h"
#include "../../src/scene_live/draw_camera_core.h"
#include "../../src/scene_live/particle_source.h"

using namespace rdr_scene;
using namespace rdr_scene::live;
using namespace rdr_scene::live::particles;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& detail = "") {
  if (!ok) {
    ++g_fail;
    std::printf("FAIL %s %s\n", name, detail.c_str());
  } else {
    std::printf("ok   %s\n", name);
  }
}

// ---- synthetic guest memory (the layout of tools\scene\f_test.cpp: docs\formats\particle_items.md)
struct FakeMem : GuestMem {
  std::map<uint32_t, uint32_t> w;
  bool Read32(uint32_t a, uint32_t& o) const override { auto it = w.find(a); if (it == w.end()) return false; o = it->second; return true; }
  void F(uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); w[a] = u; }
};
static FakeMem* g_mem = nullptr;
static uint64_t g_reads_scoped = 0, g_reads_unscoped = 0;   // S108 RDS review: reads made inside / outside a pass scope (safe_guest_pages.h PassLines)
static bool SrcRead(uint32_t guest, void* dst, size_t n) {   // n raw BIG-ENDIAN bytes
  ++(wwe2k14::safe_pages::PassLines::Current() ? g_reads_scoped : g_reads_unscoped);
  uint8_t* d = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; i += 4) {
    const auto it = g_mem->w.find(guest + uint32_t(i));
    if (it == g_mem->w.end()) return false;
    const uint32_t v = it->second;
    d[i] = uint8_t(v >> 24), d[i + 1] = uint8_t(v >> 16), d[i + 2] = uint8_t(v >> 8), d[i + 3] = uint8_t(v);
  }
  return true;
}

static void BuildHandleTables(FakeMem& m) {
  const struct { uint32_t global, mgr, begin, id, ptr; } t[2] = {{0x83929A98u, 0x70000, 0x72000, 0x0059002Cu, 0xAF59BF00u}, {0x8392E994u, 0x71000, 0x73000, 0x000F0007u, 0xAF4783C0u}};
  for (const auto& h : t) {
    m.w[h.global] = h.mgr, m.w[h.mgr + 8] = h.begin, m.w[h.mgr + 16] = h.begin + 8 * 0x100;
    m.w[h.begin + 8 * (h.id & 0xFFFF)] = h.id, m.w[h.begin + 8 * (h.id & 0xFFFF) + 4] = h.ptr;
  }
}
// One camera-facing sprite item at `item` (the f_test layout; every value the reader needs, nothing else)
static void BuildItem(FakeMem& m, uint32_t item, float x) {
  m.w[item] = 0x82057280u;
  m.w[item + 40] = 1;
  m.w[item + 68] = 0;
  const uint32_t pos = item + 0x1000, col = pos + 0x100, prev = col + 0x100, size = prev + 0x100, tr = size + 0x100, decoy = tr + 0x100, tab = item + 0x1700, mat = item + 0x1800;
  const auto slot = [&](uint32_t k, uint32_t p) { m.w[item + 4 * k] = p; };
  slot(15, item + 0x2000), slot(18, decoy), slot(21, pos), slot(23, prev), slot(25, col), slot(27, size), slot(29, mat), slot(33, tab), slot(35, tr);
  BuildHandleTables(m);
  m.w[item + 148] = 0x000F0007u, m.w[item + 156] = 0x0059002Cu;
  m.w[item + 48] = item + 0x3000, m.w[item + 0x3000] = item + 0x3100;
  m.w[item + 0x3100 + 224] = 4, m.w[item + 0x3100 + 196] = 0;
  m.w[item + 0x3000 + 152] = 0, m.w[item + 0x3000 + 112] = item + 0x3200, m.w[item + 0x3200] = 0, m.w[item + 0x3000 + 108] = item + 0x3210, m.w[item + 0x3210] = 0;
  for (uint32_t i = 0; i < 8; ++i) m.w[tab + 8 * i] = 0x09150000u + i, m.w[tab + 8 * i + 4] = 0xAA0E0000u + i;
  const float p[3] = {x, -50.f, 300.f};
  for (int i = 0; i < 3; ++i) m.F(pos + 4 * uint32_t(i), p[i]), m.F(prev + 4 * uint32_t(i), p[i]), m.F(decoy + 4 * uint32_t(i), 9999.f);
  m.F(pos + 12, 1.f);
  const float sz[4] = {20.f, 0.f, 0.f, 20.f};
  const uint32_t cl[4] = {0, 0, 0, 0x40};
  for (int i = 0; i < 4; ++i) m.w[col + 4 * uint32_t(i)] = cl[i], m.F(size + 4 * uint32_t(i), sz[i]);
  m.w[size + 4] = 0;
  m.w[tr + 20] = 0, m.F(tr + 24, 0.f);
  const float uvs[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  for (int i = 0; i < 4; ++i) m.F(tr + 32 + 16 * uint32_t(i), uvs[i][0]), m.F(tr + 36 + 16 * uint32_t(i), uvs[i][1]);
}
// A particle node object at `node` with one live entry (one sprite item). `a` / `b` = its words +3084 / +3088.
static void BuildNode(FakeMem& m, uint32_t node, uint32_t entry, float x, uint32_t a = 1, uint32_t b = 1) {
  m.w[entry + 56] = 0, m.w[entry + 208] = 1, m.w[entry + 80 + 132] = 1, m.w[entry + 64] = 0, m.w[entry + 220] = 1;   // +220 = 1: the start's depth-tested word (S111 PARTZ)
  const uint32_t item = 0x100000 + entry * 0x10;   // (one item list per entry, so two nodes do not share one)
  m.w[entry + 40] = entry + 0x200, m.w[entry + 0x200] = entry + 0x210, m.w[entry + 0x210 + 12] = 1, m.w[entry + 44] = entry + 0x230, m.w[entry + 0x230] = item;
  BuildItem(m, item, x);
  m.w[node + 16] = entry, m.w[node + 3088] = b, m.w[node + 3084] = a, m.w[node + 20] = 2u << 24;
}
// The camera table for the calling thread: r13 -> [r13] = tb, [tb + 1048] = blk, [blk + 32] = slot, [0x83929A40 + 4 * slot] = camera object, view +176, proj +240.
static constexpr uint32_t kR13 = 0x9000, kTb = 0x9100, kBlk = 0x9200, kCam = 0xA000;
static void BuildCameraTable(FakeMem& m, const float view[16], const float proj[16], uint32_t slot = 5, bool null_cam = false) {
  m.w[kR13] = kTb, m.w[kTb + 1048] = kBlk, m.w[kBlk + 32] = slot;
  m.w[kCameraTable + 4 * slot] = null_cam ? 0 : kCam;
  for (int i = 0; i < 16; ++i) m.F(kCam + kCamObjView + 4 * uint32_t(i), view[i]), m.F(kCam + kCamObjProj + 4 * uint32_t(i), proj[i]);
}
static const float kScreenView[16] = {2.f / 1280.f, 0, 0, 0, 0, -2.f / 720.f, 0, 0, 0, 0, 0.0033f, 0, 0, 0, 0, 1};
static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static const float kMatchView[16] = {0.8f, 0, -0.6f, 0, 0, 1, 0, 0, 0.6f, 0, 0.8f, 0, 0, 0, 0, 1};   // S103 PRT fix 4: the match section's node camera = the scene manager's view (below)
static const float kPersp[16] = {0.8f, 0.1f, 0.5916f, 0, 0.f, 0.9864f, -0.1644f, 0, -0.6f, 0.1315f, 0.7893f, 0.5f, 0, 0, 0, 1};   // rotated + a projection term: not diagonal

static NodeCall CallOf(uint32_t node, uint32_t rows) {
  NodeCall c;
  ReadNodeCamera(&SrcRead, kR13, c);
  c.node = node, c.rows = rows;
  ReadNodeWords(&SrcRead, node, c);   // the words are read AT THE CALL (S103 MPBF 2), as the entry hook does
  return c;
}
static int g_mode = 1;
static ParticleSource* NewSource() {
  scene_frame::g_mode_cvar.store(&g_mode, std::memory_order_relaxed);
  auto* s = new ParticleSource();
  s->SetReader(&SrcRead);
  return s;
}
// Emit a frame through the real contributor (the game's wiring: Take, then TakeEmpty when Take took nothing) and return the record tables.
static SceneTables EmitFrame(ParticleSource& src, uint64_t writer_frame) {
  ParticleContributor pc([](uint64_t f) { return scene_frame::HookFrameOf(f); });
  pc.SetSource([&](uint64_t wf, ParticleSnapshot& o) { return src.Take(wf, o); });
  pc.SetEmptySource([&](uint64_t wf, ParticleSnapshot& o) { return src.TakeEmpty(wf, o); });
  live::FrameInfo fi;
  fi.guest_frame = writer_frame;
  SceneTables t;
  live::Counters c;
  pc.Latch(fi);
  pc.Emit(fi, t, c);
  return t;
}
static SceneTables RoundTrip(const SceneTables& t) {
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  if (!ParseRecord(bytes.data(), bytes.size(), back, err)) std::printf("note: ParseRecord: %s\n", err.c_str());
  return back;
}
static scene_kind::Parts PartsOfTables(const ParticleTables& p) {
  return scene_kind::PartsOf(p.EmptyPass(), uint32_t(p.batches.size()), p.status.empty() ? 0u : p.status[0].reasons, !p.screen.empty());
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) {
      const int p = std::atoi(argv[i + 1]);
      if (p == 12) g_reader_plant = 6;   // plant 12 = the old 64-entry cap (reader plant 6)
      else if (p == 15) g_reader_plant = 7;   // 15 = a flat soft sprite is drawn soft
      else if (p == 16) g_reader_plant = 8;   // 16 = the game's entry list is ignored
      else if (p == 13) g_node_cam_plant_off = true;   // 13 = the node-camera agreement check is skipped
      else g_source_plant = p;
    }
  if (g_source_plant || g_reader_plant || g_node_cam_plant_off) std::printf("plant %d\n", g_source_plant ? g_source_plant : g_node_cam_plant_off ? 13 : g_reader_plant == 7 ? 15 : g_reader_plant == 8 ? 16 : 12);

  FakeMem m;
  g_mem = &m;
  BuildNode(m, 0x4000, 0x5000, 100.f);
  BuildNode(m, 0x4100, 0x6000, 130.f);
  BuildNode(m, 0x4200, 0x7000, 160.f, /*a=*/0);   // +3084 == 0: the node returns before drawing
  BuildNode(m, 0x4300, 0x8000, 160.f, 1, /*b=*/0);   // +3088 == 0
  BuildCameraTable(m, kScreenView, kIdent);
  const uint64_t kFrame = 100;

  // ---- A. empty is whole
  {
    ParticleSource* none = NewSource();   // never noted
    const SceneTables t = RoundTrip(EmitFrame(*none, kFrame));
    Check(t.particles.EmptyPass() && t.particles.batches.empty() && PartsOfTables(t.particles) == scene_kind::Parts::Whole, "empty.no_call");
    delete none;
    ParticleSource* old = NewSource();   // a call in ANOTHER frame
    old->NoteCall(CallOf(0x4000, 2), kFrame - 5);
    const SceneTables t2 = RoundTrip(EmitFrame(*old, kFrame));
    Check(t2.particles.EmptyPass() && PartsOfTables(t2.particles) == scene_kind::Parts::Whole, "empty.other_frame");
    delete old;
  }
  {
    ParticleSource* live = NewSource();   // a call in this frame with non-zero words: NOT empty (the planted twin: TakeEmpty ignores the call)
    live->NoteCall(CallOf(0x4000, 2), kFrame);
    ParticleSnapshot probe;
    const bool te = live->TakeEmpty(kFrame, probe);
    const SceneTables t = RoundTrip(EmitFrame(*live, kFrame));
    Check(!te && !t.particles.EmptyPass() && PartsOfTables(t.particles) != scene_kind::Parts::Whole, "empty.node_call_not_empty", te ? "TakeEmpty said empty on a frame with a node call" : "the record read empty");
    delete live;
  }
  {
    bool all = true;
    for (uint32_t node : {0x4200u, 0x4300u}) {
      ParticleSource* z = NewSource();
      z->NoteCall(CallOf(node, 1), kFrame);
      const SceneTables t = RoundTrip(EmitFrame(*z, kFrame));
      all = all && t.particles.EmptyPass() && t.particles.batches.empty() && PartsOfTables(t.particles) == scene_kind::Parts::Whole;
      delete z;
    }
    BuildCameraTable(m, kScreenView, kIdent, 5, /*null_cam=*/true);   // the camera object is null: the node returns at 0x82254E68
    ParticleSource* nc = NewSource();
    nc->NoteCall(CallOf(0x4000, 1), kFrame);
    const SceneTables t = RoundTrip(EmitFrame(*nc, kFrame));
    all = all && t.particles.EmptyPass() && PartsOfTables(t.particles) == scene_kind::Parts::Whole;
    delete nc;
    BuildCameraTable(m, kScreenView, kIdent);
    Check(all, "empty.zero_words");
  }

  // ---- marker is information
  {
    ParticleStatus s;
    s.reasons = kPiEmptyPass;
    ParticleTables p;
    p.status.push_back(s);
    Check(WholePass(s) && p.Incomplete() == 0 && presence::ParticleVerdict(s, 0) == presence::PartWhy::WholeEmpty && uint32_t(kPiEmptyPass) == 256u, "marker.not_a_reason");
    ParticleStatus sc;
    sc.reasons = kPiScreenCam;
    Check(!WholePass(sc) && presence::ParticleVerdict(sc, 4) == presence::PartWhy::ScreenCam, "marker.not_a_reason", "screen cam must be a reason");
  }

  // ---- camera
  {
    NodeCall c = CallOf(0x4000, 0);
    const bool screen = c.have_camera && IsScreenCall(c) && std::fabs(c.view_proj[0] - 2.f / 1280.f) < 1e-9f && std::fabs(c.view_proj[5] + 2.f / 720.f) < 1e-9f;
    BuildCameraTable(m, kPersp, kIdent, 9);
    NodeCall p = CallOf(0x4000, 0);
    const bool persp_ok = p.have_camera && !IsScreenCall(p);
    BuildCameraTable(m, kScreenView, kIdent);
    ParticleSource* s1 = NewSource();   // a flat frame must not ask the scene camera: there is none in memory (kCamMgrPtr unset)
    s1->NoteCall(CallOf(0x4000, 2), kFrame);
    const SceneTables t = EmitFrame(*s1, kFrame);
    const bool node_cam = t.particles.screen.size() == 1 && (t.particles.Incomplete() & kPiNoCamera) == 0 && !t.particles.batches.empty();
    delete s1;
    Check(screen && persp_ok && node_cam, "camera.node_own", std::string(screen ? "" : "screen call not seen ") + (persp_ok ? "" : "perspective seen as screen ") + (node_cam ? "" : "scene camera used / no screen row"));
  }

  // ---- groups
  {
    ParticleSource* g = NewSource();
    g->NoteCall(CallOf(0x4000, 3), kFrame);    // group 1 after 3 rows
    g->NoteCall(CallOf(0x4100, 7), kFrame);    // group 2 after 7 rows
    g_reads_scoped = g_reads_unscoped = 0;
    const SceneTables raw = EmitFrame(*g, kFrame);
    Check(g_reads_scoped > 0 && g_reads_unscoped == 0 && !wwe2k14::safe_pages::PassLines::Current(), "rds.particle_take_pass_scoped", "reads in the pass scope " + std::to_string((unsigned long long)g_reads_scoped) + ", outside it " + std::to_string((unsigned long long)g_reads_unscoped));   // S108 RDS review: Take is one pass (the namer's ReadGuest walks share its line copy)
    const SceneTables t = RoundTrip(raw);
    const ParticleTables& p = t.particles;
    Check(p.batches.size() == 2 && p.particles.size() == 2 && p.Valid(), "groups.every_call", std::to_string(p.batches.size()) + " batches, " + std::to_string(p.particles.size()) + " particles");
    Check(p.batches.size() == 2 && p.batches[0].row_after_p1 == 4 && p.batches[1].row_after_p1 == 8, "groups.row_position",
          p.batches.size() == 2 ? std::to_string(p.batches[0].row_after_p1) + "," + std::to_string(p.batches[1].row_after_p1) : "batches merged");
    Check(p.screen.size() == 1 && p.screen[0].calls == 2 && p.screen[0].row_count == 7 && std::fabs(p.screen[0].view_proj[0] - 2.f / 1280.f) < 1e-9f && (p.Incomplete() & kPiScreenCam) && PartsOfTables(p) == scene_kind::Parts::Held,
          "groups.held");
    delete g;
  }

  // ---- a match frame (one non-screen call) takes the old path
  {
    BuildCameraTable(m, kMatchView, kIdent, 5);
    m.w[scene_inputs::kCamMgrPtr] = 0x6800;
    const float view[16] = {0.8f, 0, -0.6f, 0, 0, 1, 0, 0, 0.6f, 0, 0.8f, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) m.F(0x6800 + scene_inputs::kCamMgrView + 4 * uint32_t(i), view[i]);
    m.F(0x6800 + scene_inputs::kCamMgrFov, 60.f);
    ParticleSource* mt = NewSource();
    mt->NoteCall(CallOf(0x4000, 4), kFrame);
    const SceneTables t = RoundTrip(EmitFrame(*mt, kFrame));
    const ParticleTables& p = t.particles;
    bool no_rows = true;
    for (const ParticleBatch& b : p.batches) no_rows = no_rows && b.row_after_p1 == 0;
    Check(p.screen.empty() && !p.batches.empty() && no_rows && !p.EmptyPass() && (p.Incomplete() & (kPiScreenCam | kPiNoCamera)) == 0, "match.unchanged");
    delete mt;
    BuildCameraTable(m, kScreenView, kIdent);
  }

  // ---- S103 MPBF: the review's twins
  {
    // 2: the words are read at the call, not at the latch
    ParticleSource* w1 = NewSource();
    w1->NoteCall(CallOf(0x4000, 1), kFrame);   // words non-zero at the call ...
    m.w[0x4000 + 3084] = 0;                    // ... zero by the latch (the node rewrites them): still NOT empty
    const SceneTables ta = EmitFrame(*w1, kFrame);
    m.w[0x4000 + 3084] = 1;
    ParticleSource* w2 = NewSource();
    m.w[0x4000 + 3088] = 0;
    w2->NoteCall(CallOf(0x4000, 1), kFrame);   // zero at the call ...
    m.w[0x4000 + 3088] = 1;                    // ... non-zero by the latch: empty
    const SceneTables tb = EmitFrame(*w2, kFrame);
    Check(!ta.particles.EmptyPass() && tb.particles.EmptyPass(), "empty.words_at_call", std::string(ta.particles.EmptyPass() ? "words changed after the call made the frame empty; " : "") + (tb.particles.EmptyPass() ? "" : "a zero word at the call was not seen"));
    delete w1, delete w2;
    // 3: TakeEmpty's pairing: the first frame after the particles stop (noted == want - 1), a later note, and the race (noted == want) are NOT empty; two frames behind is
    bool pairing = true;
    std::string why;
    for (int d : {-3, -2, -1, 0, 1, 4}) {   // noted = kFrame + d
      ParticleSource* e = NewSource();
      e->NoteCall(CallOf(0x4000, 1), uint64_t(int64_t(kFrame) + d));
      ParticleSnapshot sn;
      const bool te = e->TakeEmpty(kFrame, sn);
      const bool want_empty = d <= -2;
      if (te != want_empty) pairing = false, why += " d=" + std::to_string(d) + (te ? " empty" : " not empty");
      delete e;
    }
    Check(pairing, "empty.pairing", why);
    // 4: every call decides (not the last one), and two distinct drawing nodes are a reason
    BuildCameraTable(m, kMatchView, kIdent, 5);
    m.w[scene_inputs::kCamMgrPtr] = 0x6800;
    const float view[16] = {0.8f, 0, -0.6f, 0, 0, 1, 0, 0, 0.6f, 0, 0.8f, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) m.F(0x6800 + scene_inputs::kCamMgrView + 4 * uint32_t(i), view[i]);
    m.F(0x6800 + scene_inputs::kCamMgrFov, 60.f);
    ParticleSource* c1 = NewSource();
    c1->NoteCall(CallOf(0x4000, 1), kFrame), c1->NoteCall(CallOf(0x4200, 2), kFrame);   // a drawing call, then one that returns early
    const SceneTables t1 = EmitFrame(*c1, kFrame);
    ParticleSource* c2 = NewSource();
    c2->NoteCall(CallOf(0x4200, 1), kFrame), c2->NoteCall(CallOf(0x4300, 2), kFrame);   // both return early
    const SceneTables t2 = EmitFrame(*c2, kFrame);
    ParticleSource* c3 = NewSource();
    c3->NoteCall(CallOf(0x4000, 1), kFrame), c3->NoteCall(CallOf(0x4100, 2), kFrame);   // two distinct nodes draw
    const SceneTables t3 = EmitFrame(*c3, kFrame);
    Check(!t1.particles.EmptyPass() && !t1.particles.batches.empty() && t2.particles.EmptyPass() && t2.particles.batches.empty(),
          "empty.every_call", std::string(t1.particles.EmptyPass() ? "the last call alone decided" : ""));
    // S103 PRT: two distinct drawing nodes are BOTH read (no reason, no drop): the old rule read only the last (plant 11)
    Check(t3.particles.particles.size() == 2 && !(t3.particles.Incomplete() & kPiMultiNode) && t1.particles.particles.size() == 1 && c3->Census().find("nodes_read 2") != std::string::npos,
          "nodes.every_node", std::to_string(t3.particles.particles.size()) + " particles for two drawing nodes");
    delete c1, delete c2, delete c3;
    // S103 PRT (PNS.md): two banks whose only entries sit at effect ids 110+ (count 224, as every PCE6 bank's shared lights): the 64-entry cap and the last-node rule each drop them
    {
      for (uint32_t n = 0; n < 2; ++n) {
        const uint32_t node = 0x7000 + 0x100 * n, base = 0x200000 + 0x40000 * n, entry = base + 432 * 110;
        BuildNode(m, node, entry, 100.f + 30.f * float(n));
        m.w[node + 16] = base, m.w[node + 3088] = 224, m.w[node + 20] = 0, m.w[node + 128] = 2u << 8, m.w[node + 3092] = 5, m.w[node + 3096] = 139;   // (bank type 5, id 139) state byte of entry 110 (word node+128, byte 2) = 2; every other entry idle
      }
      ParticleSource* hs = NewSource();
      hs->NoteCall(CallOf(0x7000, 1), kFrame), hs->NoteCall(CallOf(0x7100, 2), kFrame);
      const SceneTables th = EmitFrame(*hs, kFrame);
      Check(th.particles.particles.size() == 2 && th.particles.Valid() && hs->Census().find("node: type 5 id 139 count 224") != std::string::npos, "nodes.high_ids", std::to_string(th.particles.particles.size()) + " particles from two count-224 banks\n" + hs->Census());
      delete hs;
    }
    // S103 PRT: the global soft switch [0x836EE38C] -> +32 on, +36 range, read at the call: the batch carries lround(range * 4) (Kane 30 -> 120); switch off or unreadable = 0
    {
      m.w[kSoftSwitchGlobal] = 0x50000000u, m.w[0x50000000u + kSoftSwitchOn] = 1;
      m.F(0x50000000u + kSoftSwitchRange, 30.f);
      ParticleSource* ss = NewSource();
      ss->NoteCall(CallOf(0x4000, 1), kFrame);
      const SceneTables ts = EmitFrame(*ss, kFrame);
      m.w[0x50000000u + kSoftSwitchOn] = 0;
      ParticleSource* sh = NewSource();
      sh->NoteCall(CallOf(0x4000, 1), kFrame);
      const SceneTables tf = EmitFrame(*sh, kFrame);
      m.w.erase(kSoftSwitchGlobal);
      Check(ts.particles.batches.size() == 1 && ts.particles.batches[0].soft == 120 && tf.particles.batches.size() == 1 && tf.particles.batches[0].soft == 0, "nodes.soft_switch",
            std::to_string(ts.particles.batches.empty() ? -1 : int(ts.particles.batches[0].soft)) + " / " + std::to_string(tf.particles.batches.empty() ? -1 : int(tf.particles.batches[0].soft)));
      delete ss, delete sh;
      // S103 PRT fix 6: a range past 63.75 clamps to code 255 and is counted
      m.w[kSoftSwitchGlobal] = 0x50000000u, m.w[0x50000000u + kSoftSwitchOn] = 1;
      m.F(0x50000000u + kSoftSwitchRange, 100.f);
      ParticleSource* sc = NewSource();
      sc->NoteCall(CallOf(0x4000, 1), kFrame);
      const SceneTables tc = EmitFrame(*sc, kFrame);
      m.w.erase(kSoftSwitchGlobal);
      Check(tc.particles.batches.size() == 1 && tc.particles.batches[0].soft == 255 && sc->Census().find("soft_clamped 1") != std::string::npos, "nodes.soft_clamp", sc->Census());
      delete sc;
    }
    // S103 PRT fix 4 / fix2: a node whose own camera is not the scene camera is COUNTED; it is skipped only when g_node_cam_skip is set (default off: run 013 showed the real camera pair never matched, a wrong skip hid
    // every particle). A node whose rotation is the TRANSPOSE of the scene's is its own counter, never a mismatch.
    {
      BuildCameraTable(m, kPersp, kIdent, 5);
      ParticleSource* mc = NewSource();   // default: counted, still read
      mc->NoteCall(CallOf(0x4000, 1), kFrame);
      const SceneTables tc2 = EmitFrame(*mc, kFrame);
      g_node_cam_skip = true;
      ParticleSource* mm = NewSource();   // skip on: counted and skipped
      mm->NoteCall(CallOf(0x4000, 1), kFrame);
      const SceneTables tm = EmitFrame(*mm, kFrame);
      g_node_cam_skip = false;
      float vt[16];
      std::memcpy(vt, kMatchView, sizeof vt);
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) vt[r * 4 + k] = kMatchView[k * 4 + r];
      BuildCameraTable(m, vt, kIdent, 5);
      ParticleSource* mt = NewSource();
      mt->NoteCall(CallOf(0x4000, 1), kFrame);
      const SceneTables tt = EmitFrame(*mt, kFrame);
      BuildCameraTable(m, kMatchView, kIdent, 5);
      Check(tc2.particles.particles.size() == 1 && mc->Census().find("node_cam_mismatch 1") != std::string::npos && mc->Census().find("first mismatch") != std::string::npos, "nodes.cam_count_only", mc->Census());
      Check(tm.particles.particles.empty() && mm->Census().find("node_cam_mismatch 1") != std::string::npos, "nodes.cam_mismatch", std::to_string(tm.particles.particles.size()) + " particles");
      Check(tt.particles.particles.size() == 1 && mt->Census().find("node_cam_transposed 1") != std::string::npos && mt->Census().find("node_cam_mismatch 0") != std::string::npos, "nodes.cam_transposed", mt->Census());
      delete mc, delete mm, delete mt;
    }
    // S103 PRT fix 3: the banks are read in bank order (type, id), whichever order two walk threads call them
    {
      m.w[0x4000 + 3092] = 5, m.w[0x4000 + 3096] = 10, m.w[0x4100 + 3092] = 5, m.w[0x4100 + 3096] = 20;
      ParticleSource* so = NewSource();
      so->NoteCall(CallOf(0x4100, 1), kFrame), so->NoteCall(CallOf(0x4000, 2), kFrame);
      const SceneTables tso = EmitFrame(*so, kFrame);
      Check(tso.particles.particles.size() == 2 && std::fabs(tso.particles.particles[0].local_world[3] - 100.f) < 1e-3f, "nodes.stable_order",
            tso.particles.particles.empty() ? "none" : std::to_string(tso.particles.particles[0].local_world[3]));
      delete so;
    }
    // S103 PRT fix 5: the game draws an entry only when it is in the list [[0x83929A84] + 24] (count + 36): node 0x4100's entry (0x6000) is not
    {
      m.w[kEntryListGlobal] = 0x51000000u, m.w[0x51000000u + 24] = 0x51000100u, m.w[0x51000000u + 36] = 1, m.w[0x51000100u] = 0x5000;
      ParticleSource* se = NewSource();
      se->NoteCall(CallOf(0x4000, 1), kFrame), se->NoteCall(CallOf(0x4100, 2), kFrame);
      const SceneTables tse = EmitFrame(*se, kFrame);
      m.w.erase(kEntryListGlobal);
      Check(tse.particles.particles.size() == 1 && se->Census().find("entry_not_listed 1") != std::string::npos, "nodes.entry_list", std::to_string(tse.particles.particles.size()) + " particles; " + se->Census());
      delete se;
    }
    // S103 PRT fix 1: a soft sprite lying flat (-LocalWorld[1].z > 0.999 in game space) is interned under soft = 0 (the game leaves it unfaded); any other stays soft
    {
      CameraBasis flat, norm;
      flat.fwd[0] = 0, flat.fwd[1] = -1.f, flat.fwd[2] = 0;
      ParticleSnapshot s1, s2;
      ReaderStats r1, r2;
      r1.soft = r2.soft = 120;
      ReadNode(m, 0x4000, flat, s1, r1, nullptr);
      ReadNode(m, 0x4000, norm, s2, r2, nullptr);
      Check(!s1.entries.empty() && !s2.entries.empty() && s1.keys[s1.entries[0].key].soft == 0 && r1.soft_flat_hard == 1 && s2.keys[s2.entries[0].key].soft == 120 && r2.soft_flat_hard == 0, "soft.flat_hard",
            std::to_string(s1.entries.size()) + " / " + std::to_string(s2.entries.size()));
    }
    // 5: more calls than the cap are a reason, never a silent drop
    ParticleSource* ov = NewSource();
    for (size_t i = 0; i < kMaxCallsPerFrame + 2; ++i) ov->NoteCall(CallOf(0x4000, 1), kFrame);
    const SceneTables to = EmitFrame(*ov, kFrame);
    Check((to.particles.Incomplete() & kPiDropped) != 0 && ov->Census().find("calls_over 2") != std::string::npos && ov->Census().find("same_node_twice 17") != std::string::npos, "calls.overflow",
          "reasons " + std::to_string(to.particles.Incomplete()));
    delete ov;
    BuildCameraTable(m, kScreenView, kIdent);
    // 6: the node's matrix is +240 . +176 (claims_MP MP6): a non-commuting pair
    float v[16], p[16], expect[16];
    for (int i = 0; i < 16; ++i) v[i] = 0.5f + 0.25f * float((i * 7) % 5), p[i] = 1.f + 0.5f * float((i * 3) % 4);
    for (int r = 0; r < 4; ++r)
      for (int cc = 0; cc < 4; ++cc) {
        float a = 0;
        for (int k = 0; k < 4; ++k) a += p[r * 4 + k] * v[k * 4 + cc];   // row i of p times column j of v
        expect[r * 4 + cc] = a;
      }
    BuildCameraTable(m, v, p, 11);
    const NodeCall mc = CallOf(0x4000, 0);
    bool order = mc.have_camera;
    for (int i = 0; i < 16; ++i) order = order && std::fabs(mc.view_proj[i] - expect[i]) < 1e-4f;
    BuildCameraTable(m, kScreenView, kIdent);
    Check(order, "camera.mul_order", "view_proj is not proj . view");
    // 8: the stream-draws column also covers TakeEmpty frames (it never calls Note)
    ParticleSource* tq = NewSource();
    ParticleSnapshot sq;
    const uint64_t before = presence::Particles().te_drawn_draws.load();
    const bool tqe = tq->TakeEmpty(kFrame, sq);
    presence::Particles().NoteStreamFrame(3);
    Check(tqe && presence::Particles().te_drawn_draws.load() == before + 3 && presence::Particles().Str().find("TakeEmpty frames") != std::string::npos, "census.take_empty_column");
    delete tq;
  }

  // ---- S103 CAE: the editor frame (no scene manager: [0x836F3AA8] reads 0) takes the draw thread's camera, on the record clock, through the one shared read
  {
    FakeMem m2;
    g_mem = &m2;
    BuildNode(m2, 0x4000, 0x5000, 100.f);
    BuildCameraTable(m2, kMatchView, kIdent, 5);   // the node's own camera (non-screen): the call is a scene call
    m2.w[scene_inputs::kCamMgrPtr] = 0;        // the editor: the pointer is readable and null (reason 1)
    constexpr uint32_t kR13b = 0xC000, kTlsB = 0xC100, kBlkB = 0xC200, kDrawCam = 0xD000;   // the draw thread's own block: slot 6 -> kDrawCam (view +240 rigid, proj +176 perspective)
    m2.w[kR13b] = kTlsB, m2.w[kTlsB + 1048] = kBlkB, m2.w[kBlkB + 32] = 6, m2.w[scene_inputs::kCamTable + 4 * 6] = kDrawCam;
    const float dview[16] = {0.8f, 0, -0.6f, 0, 0, 1, 0, 0, 0.6f, 0, 0.8f, 0, -10.f, -20.f, -30.f, 1};
    const float dproj[16] = {1.2f, 0, 0, 0, 0, 2.f, 0, 0, 0, 0, 1.01f, 1.f, 0, 0, -1.f, 0};
    for (int i = 0; i < 16; ++i) m2.F(kDrawCam + scene_inputs::kCamView + 4 * uint32_t(i), dview[i]), m2.F(kDrawCam + scene_inputs::kCamProj + 4 * uint32_t(i), dproj[i]);
    m2.F(kDrawCam + scene_inputs::kCamNear, 1.f), m2.F(kDrawCam + scene_inputs::kCamFar, 500.f);
    drawcam::Cam().Reset();
    const bool noted = drawcam::Cam().Note(&SrcRead, kR13b, kFrame);
    // (a) the shared read itself: no scene manager + a draw camera -> the draw camera; none noted -> false with reason 1
    rdr_scene::Camera got;
    scene_inputs::CameraInfo ci;
    bool from_draw = false;
    const bool ok = scene_inputs::ReadSceneCameraOrDraw(&SrcRead, kFrame, &drawcam::GetForLatch, got, ci, &from_draw);
    drawcam::Cam().Reset();
    rdr_scene::Camera none;
    scene_inputs::CameraInfo ci2;
    const bool ok2 = scene_inputs::ReadSceneCameraOrDraw(&SrcRead, kFrame, &drawcam::GetForLatch, none, ci2);
    Check(noted && ok && from_draw && ci.reason == 0 && !ok2 && ci2.reason == 1 && std::fabs(got.view.m[12] + 10.f) < 1e-4f, "cae.shared_read", std::string(ok ? "" : "no camera with a draw camera noted; ") + (ok2 ? "a camera with none noted" : ""));
    // (b) the particle frame: a drawing scene call, no scene manager, a draw camera noted for this frame -> a camera, no kPiNoCamera, the billboards read
    drawcam::Cam().Reset();
    drawcam::Cam().Note(&SrcRead, kR13b, kFrame);
    ParticleSource* ed = NewSource();
    ed->NoteCall(CallOf(0x4000, 4), kFrame);
    const SceneTables te = EmitFrame(*ed, kFrame);
    Check((te.particles.Incomplete() & kPiNoCamera) == 0 && !te.particles.batches.empty(), "cae.particles_draw_camera", "reasons " + std::to_string(te.particles.Incomplete()) + (te.particles.batches.empty() ? ", no batches" : ""));
    delete ed;
    // (c) no draw camera and no scene manager: still a counted hole (fail closed), never a made-up camera
    drawcam::Cam().Reset();
    ParticleSource* ed2 = NewSource();
    ed2->NoteCall(CallOf(0x4000, 4), kFrame);
    const SceneTables te2 = EmitFrame(*ed2, kFrame);
    Check((te2.particles.Incomplete() & kPiNoCamera) != 0, "cae.particles_no_camera_stays_a_hole");
    delete ed2;
    drawcam::Cam().Reset();
    g_mem = &m;
  }

  // ---- the verdict
  {
    using scene_kind::Parts;
    Check(scene_kind::PartsOf(true, 0, 8 | 256, false) == Parts::Whole && uint32_t(kPiInfoMask) == (8u | 256u), "parts.verdict", "the pivot bit is info");
    Check(scene_kind::PartsOf(true, 0, 256, false) == Parts::Whole && scene_kind::PartsOf(false, 0, 0, false) == Parts::NotRecorded && scene_kind::PartsOf(true, 3, 256, false) == Parts::NotRecorded &&
              scene_kind::PartsOf(true, 0, 256 | 2, false) == Parts::NotRecorded && scene_kind::PartsOf(false, 4, 512, true) == Parts::Held && scene_kind::PartsOf(false, 4, 0, false) == Parts::NotRecorded,
          "parts.verdict");
  }
  std::printf(g_fail ? "mp_unit: FAIL (%d)\n" : "mp_unit: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
