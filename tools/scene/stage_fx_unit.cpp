// stage_fx_unit: the offline proof of S103 lane C2's stage writers (docs\renderer\STAGE_LIVE_WRITERS.md). 0 game runs, no SDK, no game memory.
//   stage_fx_unit [--plant NAME] [--write-rows PATH]
// It runs the SHIPPED code: the slot -> model -> skin reader (src\scene_live\stage_fx_core.h), the contributor with its latch (stage_fx_contrib.h) against a FAKE GUEST built from the offsets the
// listings give (sub_824E5340: stage + 6164 + 4 slot, motion ids at stage + 1224 + 12 slot; the part-descriptor rule of arena_world_hook.h; world array +192, inverse bind +224, palette +200), the screen-row
// join, the record round trip (record_io), the YUV -> RGBA conversion and the screen feed with its shared cache (yuv_rgba.h, screen_feed_yuv.h).
// THE ORACLE for the rows is written by hand from named values (a Y rotation c / s, translations): the packer's row k is column k of the row-vector matrix (c76 = (m00, m10, m20, m30)); the skin of a
// node is inverse bind x world (rotation then translation: p * inv * world), worked by hand below; the YUV oracle is the textbook integer BT.601 limited-range conversion (2 levels).
// A plant is a FAULT in the fake guest or the input of the thing under test; each must make a NAMED check fail (exit 1 when any check failed):
//   transposed_guest   the guest keeps its matrices transposed                           -> "slot.rows_values"
//   node_off           the part descriptor holds the node at +16 (the reader reads +12)     -> "slot.node"
//   inv_swapped        [model+192] and [model+224] hold each other's pointers              -> "slot.skin_rule"
//   stage_off          the model array starts one slot late                                -> "slot.model_join"
//   motion_zero        every motion id is 0 (a wrong stage object looks like this)          -> "slot.motion_gate"
//   mode_ignored       the latch ignores --wwe2k14_rdr_scene = 0                            -> "mode.off_reads_nothing"
//   swap_uv            the planes are handed over Cb <-> Cr                                 -> "yuv.values"
//   chroma_swapped     the two chroma texels of a row are handed over in the other order    -> "yuv.chroma_halved"
//   lose_section       the round trip loses the stage rows                                  -> "record.roundtrip"
//   join_shift         the record's arena-world entry for a model is 1.0 off in x           -> "join.arena_worlds"
//   feed_no_cache      the feed names every frame with a new content id                     -> "feed.shared"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_arena_world.h"
#include "../../src/renderer_scene/rows_stage_fx.h"
#include "../../src/renderer_scene/screen_feed_yuv.h"
#include "../../src/renderer_scene/yuv_rgba.h"
#include "../../src/scene_live/arena_id_core.h"
#include "../../src/scene_live/stage_fx_contrib.h"

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace sfx = rdr_scene::live::stagefx;
namespace si = scene_inputs;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}
static bool Near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }

// ---- the fake guest: host-order words at 4-aligned guest addresses; reads hand back big-endian bytes
struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutF(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); words[a] = b; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = sfx::Swap32(it->second);
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
};
static Guest g_guest;
static uint64_t g_reads_scoped = 0, g_reads_unscoped = 0;   // S108 RDS review: reads made inside / outside a pass scope (safe_guest_pages.h PassLines; the game's ReadGuest serves the scoped ones from the pass's line copy)
static bool ReadThunk(uint32_t a, void* dst, size_t n) { ++(wwe2k14::safe_pages::PassLines::Current() ? g_reads_scoped : g_reads_unscoped); return g_guest.Read(a, dst, n); }

static constexpr uint32_t kStage = 0x40100000;
static uint32_t ModelOf(uint32_t slot) { return 0x40200000 + slot * 0x400; }
static uint32_t PalOf(uint32_t slot) { return 0x40300000 + slot * 0x400; }
static uint32_t WorldArrOf(uint32_t slot) { return 0x40400000 + slot * 0x400; }
static uint32_t InvArrOf(uint32_t slot) { return 0x40500000 + slot * 0x400; }
static uint32_t DescOf(uint32_t slot) { return ModelOf(slot) + 0x300; }

// A row-vector 4x4 (p' = p * M): rotation about Y by (c, s) and translation (tx, ty, tz) in m[12..14]. Rows by hand: row0 = (c, 0, s, tx), row1 = (0, 1, 0, ty), row2 = (-s, 0, c, tz).
static void RotY(float c, float s, float tx, float ty, float tz, float m[16]) {
  const float v[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, tx, ty, tz, 1};
  std::memcpy(m, v, sizeof v);
}
static void PutMatrix(uint32_t a, const float m[16]) {
  for (int i = 0; i < 16; ++i) {
    const int r = i / 4, k = i % 4;
    g_guest.PutF(a + 4u * uint32_t(i), Plant("transposed_guest") ? m[4 * k + r] : m[i]);
  }
}
struct Expect { uint32_t slot; float rows[12]; };
static std::vector<Expect> g_expect;

// One slot model: world matrix `world_m` and inverse bind `inv_m` at node `node`; the palette (skin the game's own vtable +100 would make) = `pal_m`, or null = no palette.
static void PutModel(uint32_t slot, uint32_t node, const float* world_m, const float* inv_m, const float* pal_m) {
  const uint32_t stage_arr = kStage + sfx::kStageModels + (Plant("stage_off") ? 4u : 0u);
  g_guest.Put(stage_arr + 4u * slot, ModelOf(slot));
  g_guest.Put(kStage + sfx::kStageMotionIds + 12u * slot, Plant("motion_zero") ? 0u : (0x0123u << 16));   // u16 motion id in the high half of the big-endian word
  const uint32_t M = ModelOf(slot);
  g_guest.Put(M + (Plant("inv_swapped") ? 224 : 192), WorldArrOf(slot));
  g_guest.Put(M + (Plant("inv_swapped") ? 192 : 224), InvArrOf(slot));
  g_guest.Put(M + 200, pal_m ? PalOf(slot) : 0);
  g_guest.Put(M + 824, DescOf(slot));
  g_guest.Put(DescOf(slot) + 8, 1);
  g_guest.Put(DescOf(slot) + 12, Plant("node_off") ? 1 : node);
  g_guest.Put(DescOf(slot) + 16, Plant("node_off") ? node : 77);   // decoy
  PutMatrix(WorldArrOf(slot) + 64u * (node - 1), world_m);
  PutMatrix(InvArrOf(slot) + 64u * (node - 1), inv_m);
  if (pal_m) PutMatrix(PalOf(slot) + 64u * (node - 1), pal_m);
}

static void BuildGuest() {
  g_guest.words.clear();
  g_expect.clear();
  float id[16];
  RotY(1.f, 0.f, 0.f, 0.f, 0.f, id);
  // slot 101: identity inverse bind, RotY(60 deg) + (100, -250, 30): rows (0.5, 0, 0.866, 100), (0, 1, 0, -250), (-0.866, 0, 0.5, 30); the palette agrees
  float m101[16];
  RotY(0.5f, 0.8660254f, 100.f, -250.f, 30.f, m101);
  PutModel(101, 1, m101, id, m101);
  g_expect.push_back({101, {0.5f, 0, 0.8660254f, 100.f, 0, 1, 0, -250.f, -0.8660254f, 0, 0.5f, 30.f}});
  // slot 102: node 3 (matrix at +128), identity rotation, translation (-561, 0, 560)
  float m102[16];
  RotY(1.f, 0.f, -561.f, 0.f, 560.f, m102);
  PutModel(102, 3, m102, id, m102);
  g_expect.push_back({102, {1, 0, 0, -561.f, 0, 1, 0, 0, 0, 0, 1, 560.f}});
  // slot 103: THE SKIN RULE with no palette at all. world = RotY(90 deg) + (5, 6, 7); inverse bind = translation (-5, 0, 0). skin = inv x world = p * inv * world: the inverse's translation (-5, 0, 0)
  // goes through the rotation: (x c + z s, y, -x s + z c) = (0, 0, 5), plus the world's (5, 6, 7) = (5, 6, 12). Rows: (0, 0, 1, 5), (0, 1, 0, 6), (-1, 0, 0, 12). (The BARE world would give tz = 7: the old fallback's fault.)
  float w103[16], i103[16];
  RotY(0.f, 1.f, 5.f, 6.f, 7.f, w103);
  RotY(1.f, 0.f, -5.f, 0.f, 0.f, i103);
  PutModel(103, 1, w103, i103, nullptr);
  g_expect.push_back({103, {0, 0, 1.f, 5.f, 0, 1, 0, 6.f, -1.f, 0, 0, 12.f}});
  // slot 104: in the moving list, a motion id, but the stage holds no model: no row
  g_guest.Put(kStage + sfx::kStageModels + 4u * 104, 0);
  g_guest.Put(kStage + sfx::kStageMotionIds + 12u * 104, 0x0123u << 16);
  // slot 105: the palette DIFFERS from the derived skin: the derived skin is the row, the flag is set
  float m105[16], m105p[16];
  RotY(1.f, 0.f, 10.f, 20.f, 30.f, m105);
  RotY(1.f, 0.f, 11.f, 21.f, 31.f, m105p);
  PutModel(105, 1, m105, id, m105p);
  g_expect.push_back({105, {1, 0, 0, 10.f, 0, 1, 0, 20.f, 0, 0, 1, 30.f}});
  // slot 110: a model, a motion id, but NOT in the moving list: no row
  float m110[16];
  RotY(1.f, 0.f, 1.f, 2.f, 3.f, m110);
  PutModel(110, 1, m110, id, m110);
  // slot 111: a model whose arrays cannot be read: counted unreadable
  g_guest.Put(kStage + sfx::kStageModels + 4u * 111, ModelOf(111));
  g_guest.Put(kStage + sfx::kStageMotionIds + 12u * 111, 0x0123u << 16);
  g_guest.Put(ModelOf(111) + 192, WorldArrOf(111));   // pointers present, the memory behind them is not
  g_guest.Put(ModelOf(111) + 224, InvArrOf(111));
  g_guest.Put(ModelOf(111) + 824, 0);
  // slot 112: a model exists but the motion id is 0 (the MotionSetup loop made no motion player): the slot does not move, no row
  float m112[16];
  RotY(1.f, 0.f, 4.f, 5.f, 6.f, m112);
  PutModel(112, 1, m112, id, m112);
  g_guest.Put(kStage + sfx::kStageMotionIds + 12u * 112, 0);
}

static void Moving(std::vector<uint16_t>& out) { out = {101, 102, 103, 104, 105, 111, 112}; }
static int32_t g_arena = 4;
static int32_t ArenaNow() { return g_arena; }
static int g_mode = 2;

static const StageFxRow* RowOf(const std::vector<StageFxRow>& v, uint16_t slot) {
  for (const StageFxRow& r : v)
    if (r.kind == kFxSlotWorld && r.slot == slot) return &r;
  return nullptr;
}

// A fake plane source: two screens showing the same movie (content id 11) and one showing another (12).
struct FakePlanes : rf::c::PlaneSource {
  uint8_t y[16] = {16, 82, 145, 235, 0, 0, 0, 0, 100, 50, 200, 30, 0, 0, 0, 0};
  uint8_t cb[4] = {128, 90, 0, 0}, cr[4] = {128, 240, 0, 0};
  uint64_t counter = 0;
  bool Planes(uint32_t key, uint32_t serial, rf::c::YuvPlanes& out, uint64_t& id) override {
    (void)serial;
    out.w = 4, out.h = 2, out.y = y, out.y_pitch = 8, out.cb = cb, out.cb_pitch = 4, out.cr = cr, out.cr_pitch = 4, out.a = nullptr;
    id = Plant("feed_no_cache") ? 1000 + counter++ : (key == 800 || key == 801 ? 11 : 12);
    return true;
  }
};

int main(int argc, char** argv) {
  std::string write_rows;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
    else if (!std::strcmp(argv[i], "--write-rows") && i + 1 < argc) write_rows = argv[++i];
  }
  scene_frame::g_mode_cvar.store(&g_mode);
  BuildGuest();

  // ---------------------------------------------------------------- 1. the slot rows
  std::vector<StageFxRow> rows;
  sfx::SlotCensus cen;
  std::vector<uint16_t> moving;
  Moving(moving);
  sfx::BuildSlotRows(ReadThunk, kStage, moving, 300, rows, cen);
  {
    bool ok = true;
    std::string why;
    for (const Expect& e : g_expect) {
      const StageFxRow* r = RowOf(rows, uint16_t(e.slot));
      if (!r) { ok = false; why += " no row for " + std::to_string(e.slot); continue; }
      for (int i = 0; i < 12; ++i)
        if (!Near(r->world[i], e.rows[i], 1e-4f)) { ok = false; why += " slot " + std::to_string(e.slot) + " [" + std::to_string(i) + "]=" + std::to_string(r->world[i]) + " want " + std::to_string(e.rows[i]); break; }
    }
    Check(ok, "slot.rows_values", why);
  }
  {
    const StageFxRow* r = RowOf(rows, 102);
    Check(r && Near(r->world[3], -561.f) && Near(r->world[11], 560.f), "slot.node", r ? ("tx " + std::to_string(r->world[3])) : "no row");
  }
  {
    const StageFxRow* r3 = RowOf(rows, 103);   // the skin rule: inverse bind x world, from +192 / +224 with NO palette; the bare world would put tz at 7
    Check(r3 && Near(r3->world[11], 12.f, 1e-4f) && Near(r3->world[3], 5.f, 1e-4f) && (r3->flags & sfx::kSfPaletteUnread), "slot.skin_rule", r3 ? ("tz " + std::to_string(r3->world[11]) + " flags " + std::to_string(r3->flags)) : "no row");
    const StageFxRow* r5 = RowOf(rows, 105);
    Check(r5 && Near(r5->world[3], 10.f) && (r5->flags & sfx::kSfPaletteDiffer), "slot.differ_flag", r5 ? ("tx " + std::to_string(r5->world[3]) + " flags " + std::to_string(r5->flags)) : "no row");
    const StageFxRow* r1 = RowOf(rows, 101);
    Check(r1 && !(r1->flags & sfx::kSfPaletteDiffer) && !(r1->flags & sfx::kSfPaletteUnread), "slot.agrees_with_palette", r1 ? ("flags " + std::to_string(r1->flags)) : "no row");   // like with like: a palette that agrees is not flagged
  }
  {
    const StageFxRow* r = RowOf(rows, 101);
    Check(r && r->slot == 101 && Near(r->world[3], 100.f) && r->pad == ModelOf(101), "slot.model_join", r ? ("tx " + std::to_string(r->world[3])) : "no row");   // by SLOT through the stage's array
  }
  Check(!RowOf(rows, 104) && cen.no_model == 1, "slot.no_model", "rows " + std::to_string(rows.size()) + " no_model " + std::to_string(cen.no_model));
  Check(rows.size() == 4 && !RowOf(rows, 110) && !RowOf(rows, 111) && cen.unreadable == 1, "slot.only_moving", "rows " + std::to_string(rows.size()) + " unreadable " + std::to_string(cen.unreadable));
  Check(RowOf(rows, 101) && !RowOf(rows, 112) && cen.no_motion == 1, "slot.motion_gate", "no_motion " + std::to_string(cen.no_motion));
  {
    std::vector<StageFxRow> none;
    sfx::SlotCensus c2;
    sfx::BuildSlotRows(ReadThunk, 0, moving, 301, none, c2);
    Check(none.empty() && c2.no_stage == 1, "stage.unknown_no_rows");
    std::vector<StageFxRow> cap;
    sfx::SlotCensus c3;
    sfx::BuildSlotRows(ReadThunk, kStage, moving, 302, cap, c3, 2);
    Check(cap.size() == 2 && c3.rows_dropped_cap == 2, "slot.cap_counted", "rows " + std::to_string(cap.size()) + " dropped " + std::to_string(c3.rows_dropped_cap));
  }
  {   // the arena-worlds cross-check: each row against the entry of the same model address in the same record (the pairing + A1)
    std::vector<ArenaWorld> worlds;
    for (const StageFxRow& r : rows) {
      ArenaWorld a;
      a.kind = kAwObject, a.object = r.pad, a.frame = 299;
      std::memcpy(a.world, r.world, sizeof a.world);
      if (Plant("join_shift") && r.slot == 102) a.world[3] += 1.f;
      worlds.push_back(a);
    }
    if (!worlds.empty()) {
      ArenaWorld stale = worlds[0];   // an older entry for the first model (the last one wins): wrong on purpose
      stale.world[3] += 50.f;
      worlds.insert(worlds.begin(), stale);
    }
    ArenaWorld other;   // an entry for a model that is not ours: ignored
    other.kind = kAwObject, other.object = 0x5555;
    worlds.push_back(other);
    sfx::SlotCensus cj;
    sfx::CrossCheckArenaWorlds(rows, 0, worlds, cj);
    Check(cj.join_equal == 4 && cj.join_differ == 0 && cj.join_none == 0, "join.arena_worlds", "equal " + std::to_string(cj.join_equal) + " differ " + std::to_string(cj.join_differ) + " none " + std::to_string(cj.join_none));
    sfx::SlotCensus cn;
    sfx::CrossCheckArenaWorlds(rows, 0, std::vector<ArenaWorld>(), cn);
    Check(cn.join_none == 4, "join.none_without_entries", "none " + std::to_string(cn.join_none));   // under OURS the walk is skipped: no entries
  }

  // ---------------------------------------------------------------- 2. the contributor: latch, mode gate, screens, the stage lifetime
  static void (*kMovingFn)(std::vector<uint16_t>&) = +[](std::vector<uint16_t>& o) { Moving(o); };
  static size_t (*kScreensFn)(sfx::ScreenSource*, size_t) = +[](sfx::ScreenSource* out, size_t cap) -> size_t {
    if (cap < 3) return 0;
    out[0] = {800, 7, 256, 128};
    out[1] = {802, 9, 512, 256};
    out[2] = {805, 3, 0, 0};     // no size: refused at the latch
    return 3;
  };
  {
    sfx::StageFxContributor con;
    con.SetReader(ReadThunk);
    con.SetMoving(kMovingFn);
    con.SetScreens(kScreensFn);
    con.SetArenaFn(ArenaNow);
    con.SetStage(kStage);
    FrameInfo f;
    f.guest_frame = 500;
    g_reads_scoped = g_reads_unscoped = 0;
    const bool latched = con.Latch(f);
    Check(g_reads_scoped > 0 && g_reads_unscoped == 0 && !wwe2k14::safe_pages::PassLines::Current(), "rds.stagefx_take_pass_scoped",
          "reads in the pass scope " + std::to_string((unsigned long long)g_reads_scoped) + ", outside it " + std::to_string((unsigned long long)g_reads_unscoped));   // S108 RDS review: the take is one pass (its line copy dies with it)
    SceneTables t;
    for (uint8_t k = 0; k < 3; ++k) {   // the arena contributor's part of the record: a TronPicture Target (index = key - 800) + an OnTarget Movie for keys 800, 801, 802
      Target tg;
      tg.role = rdr_adapter::SurfaceRole::TronPicture;
      tg.index = k;
      t.targets.push_back(tg);
      Movie m;
      std::memset(static_cast<void*>(&m), 0, sizeof m);
      m.show = uint8_t(MovieShow::OnTarget);
      m.target = uint32_t(t.targets.size() - 1);
      t.movies.push_back(m);
    }
    for (const StageFxRow& r : rows) {   // mirror mode: the model-entry hook's entries are in the record already (arena worlds registered first)
      ArenaWorld a;
      a.kind = kAwObject, a.object = r.pad;
      std::memcpy(a.world, r.world, sizeof a.world);
      t.arena_worlds.push_back(a);
    }
    Counters c;
    con.Emit(f, t, c);
    size_t slot_rows = 0, screen_rows = 0;
    const StageFxRow* s800 = nullptr;
    const StageFxRow* s802 = nullptr;
    for (const StageFxRow& r : t.stage_fx) {
      if (r.kind == kFxSlotWorld) ++slot_rows;
      if (r.kind == kFxScreen) {
        ++screen_rows;
        if (r.slot == 800) s800 = &r;
        if (r.slot == 802) s802 = &r;
      }
    }
    Check(latched && slot_rows == 4 && c.objects >= 4, "contrib.slot_rows", "latched " + std::to_string(latched) + " slot rows " + std::to_string(slot_rows));
    Check(con.SlotStats().join_equal == 4 && con.SlotStats().join_differ == 0, "contrib.join_census", "equal " + std::to_string(con.SlotStats().join_equal));
    Check(screen_rows == 2 && s800 && s802 && s800->movie == 0 && s802->movie == 2 && s800->seq == 7 && s800->w == 256 && s800->h == 128 && s802->w == 512, "contrib.screen_rows",
          "screen rows " + std::to_string(screen_rows) + (s800 ? " movie800 " + std::to_string(s800->movie) : ""));
    {   // a screen key with no Movie in the record: no row, counted unknown
      SceneTables t2;
      Counters c2;
      FrameInfo f2;
      f2.guest_frame = 501;
      con.Latch(f2);
      con.Emit(f2, t2, c2);
      size_t n = 0;
      for (const StageFxRow& r : t2.stage_fx) n += r.kind == kFxScreen;
      Check(n == 0 && c2.unknown >= 2, "contrib.screen_no_movie", "rows " + std::to_string(n) + " unknown " + std::to_string(c2.unknown));
    }
    {   // a deferred emit with no latch must not read guest memory
      SceneTables t3;
      Counters c3;
      FrameInfo f3;
      f3.guest_frame = 502;
      live::tail_detail::t_deferred_emit = true;
      con.Emit(f3, t3, c3);
      live::tail_detail::t_deferred_emit = false;
      Check(t3.stage_fx.empty() && c3.stale == 1, "emit.deferred_no_read", "rows " + std::to_string(t3.stage_fx.size()) + " stale " + std::to_string(c3.stale));
    }
    {   // the stage object's lifetime: a different published arena clears it (no rows read from a freed stage); a stage set under the new arena works
      g_arena = 5;
      SceneTables t4;
      Counters c4;
      FrameInfo f4;
      f4.guest_frame = 503;
      con.Latch(f4);
      con.Emit(f4, t4, c4);
      size_t n = 0;
      for (const StageFxRow& r : t4.stage_fx) n += r.kind == kFxSlotWorld;
      const bool cleared = con.Stage() == 0 && n == 0;
      con.SetStage(kStage);   // the new arena's MotionSetup
      SceneTables t5;
      Counters c5;
      FrameInfo f5;
      f5.guest_frame = 504;
      con.Latch(f5);
      con.Emit(f5, t5, c5);
      size_t n5 = 0;
      for (const StageFxRow& r : t5.stage_fx) n5 += r.kind == kFxSlotWorld;
      Check(cleared && n5 == 4, "stage.cleared_on_arena_change", "cleared " + std::to_string(cleared) + " rows after reset " + std::to_string(n5));
      g_arena = 4;
    }
    {   // S103 CAE: the CAW editor: the stage was built for arena 66 (stage + 1216, constructor sub_824E55B0) while the published arena still says 0 (stale G+64, or the hook has not run since the stage was set):
        // the stage must NOT be cleared (it would never come back: MotionSetup runs once), and a stage whose own id changes under it (a freed object reused) IS cleared.
      g_guest.Put(kStage + 1216, 66);
      g_arena = 0;
      con.SetStage(kStage);
      FrameInfo f6;
      f6.guest_frame = 505;
      con.Latch(f6);
      const bool kept = con.Stage() == kStage;
      g_guest.Put(kStage + 1216, 7);   // the object now names another arena
      con.Latch(f6);
      const bool gone = con.Stage() == 0;
      Check(kept && gone, "stage.own_id_guard", std::string(kept ? "" : "the editor's stage (66, published 0) was cleared; ") + (gone ? "" : "a stage whose own id changed was kept"));
      g_guest.words.erase(kStage + 1216);
      g_arena = 4;
      con.SetStage(kStage);
    }
    {   // S103 CAE F1: the game frees its stage (pre-hook on the destructor sub_824E9858): the stage goes at once, though the freed block still holds the same +1216 and G+64 changed after it.
        // (the old rule only compared published arena ids: with the same id in the freed block and a new G+64 it kept reading freed motion players)
      g_guest.Put(kStage + 1216, 4);
      g_arena = 4;
      con.PlantNoFreeHook(Plant("stage_free_ignored"));
      con.SetStage(kStage);
      con.OnStageFreed(kStage + 0x100);   // another object: the current stage stays
      const bool other_kept = con.Stage() == kStage;
      con.OnStageFreed(kStage);
      g_arena = 5;   // G+64 changes after the free (the next match's arena): the freed block still says 4
      FrameInfo f7;
      f7.guest_frame = 506;
      con.Latch(f7);
      SceneTables t7;
      Counters c7;
      con.Emit(f7, t7, c7);
      size_t n7 = 0;
      for (const StageFxRow& r : t7.stage_fx) n7 += r.kind == kFxSlotWorld;
      Check(other_kept && con.Stage() == 0 && n7 == 0 && con.Freed() == 1, "stage.freed_by_game", std::string(other_kept ? "" : "another object's free cleared the stage; ") + "stage " + (con.Stage() ? "kept (rows read from a freed stage) " : "cleared ") + "rows " + std::to_string(n7));
      con.PlantNoFreeHook(false);
      g_arena = 4;
      con.SetStage(kStage);
    }
    {   // S103 CAE F2: slots_ready keyed on the stage's arena: the editor's stage (66) with a stale published 0 whose table is loaded is NOT ready
      namespace ra = rdr_scene::live::arena;
      ra::g_arena_id_plant = Plant("slots_ready_published_only") ? 2 : 0;
      const bool stale = ra::SlotsReady(66, 0, /*needs_slots(0)=*/false, /*needs_slots(66)=*/true);
      const bool loading = ra::SlotsReady(66, 66, false, true);
      const bool ready = ra::SlotsReady(66, 66, false, false);
      const bool unknown = ra::SlotsReady(-1, 66, false, false);
      Check(!stale && !loading && ready && !unknown, "arena.slots_ready_of_stage", std::string(stale ? "ready for the wrong arena; " : "") + (ready ? "" : "not ready with the right table; ") + (unknown ? "ready with no stage arena" : ""));
      ra::g_arena_id_plant = 0;
    }
    {   // S103 PHS: an ENTRANCE is the stage-page object's entrance word, not the phase word: the editor (phase 4, [obj+88] = 0) is not one and stamps the pools input; an entrance (phase 4, [obj+88] != 0) is one
      si::g_entrance_plant = Plant("phase_is_entrance") ? 1 : 0;
      const int editor = si::PageEntranceState(true, 0, true, 4);
      const int entrance = si::PageEntranceState(true, 2, true, 4);
      const int match = si::PageEntranceState(true, 0, true, 1);
      const int unread = si::PageEntranceState(false, 0, true, 4);
      Check(editor == 1 && entrance == 2 && match == 1 && unread == 0, "phase.entrance_from_page", "editor " + std::to_string(editor) + " entrance " + std::to_string(entrance) + " match " + std::to_string(match) + " unread " + std::to_string(unread));
      Check(si::PoolsStamp(editor, false, false) && si::PoolsStamp(match, false, false) && !si::PoolsStamp(unread, false, false) && !si::PoolsStamp(entrance, false, false) && si::PoolsStamp(entrance, true, true) && !si::PoolsStamp(entrance, true, false),
            "phase.pools_stamp_editor");
      si::g_entrance_plant = 0;
    }
    {   // S103 CAE: the arena id the hook publishes (arena_id_core.h ResolveArenaId): the stage's id wins, G+64 only while no stage is known, the disagreements are counted
      namespace ra = rdr_scene::live::arena;
      ra::g_arena_id_plant = Plant("arena_g64_only") ? 1 : 0;
      ra::ArenaIdCensus cn;
      const int32_t editor = ra::ResolveArenaId(true, 66, true, 0, cn);   // the stage is bg66, G+64 is the stale 0
      const int32_t match = ra::ResolveArenaId(true, 12, true, 12, cn);   // a match: the same arena in both
      const int32_t nostage = ra::ResolveArenaId(false, 0, true, 9, cn);   // no stage known: G+64
      const int32_t none = ra::ResolveArenaId(false, 0, false, 0, cn);
      const int32_t wild = ra::ResolveArenaId(true, 4000, true, 5, cn);   // a stage id that is not an arena: falls to the word
      Check(editor == 66 && match == 12 && nostage == 9 && none == -1 && wild == 5, "arena.id_from_stage", "editor " + std::to_string(editor) + " match " + std::to_string(match) + " no_stage " + std::to_string(nostage) + " none " + std::to_string(none) + " wild " + std::to_string(wild));
      const std::string line = cn.Text(true, 66, true, 0);
      Check(cn.disagree.load() == 1 && cn.from_stage.load() == 2 && cn.from_word.load() == 2 && cn.unknown.load() == 1 && line.find("stage id 66, published word 0, disagreements 1") == 0, "arena.disagreement_census", line);
      ra::g_arena_id_plant = 0;
    }
    if (!write_rows.empty()) {   // the driver's input (c_driver --rows): the rows this writer made, raw
      FILE* fp = std::fopen(write_rows.c_str(), "wb");
      if (fp) {
        std::fwrite(rows.data(), sizeof(StageFxRow), rows.size(), fp);
        std::fclose(fp);
      }
    }
  }
  {   // the mode gate: --wwe2k14_rdr_scene = 0 reads nothing and writes nothing, latched or not
    g_mode = Plant("mode_ignored") ? 2 : 0;
    sfx::StageFxContributor off;
    off.SetReader(ReadThunk);
    off.SetMoving(kMovingFn);
    off.SetScreens(kScreensFn);
    off.SetStage(kStage);
    FrameInfo f;
    f.guest_frame = 600;
    const bool latched = off.Latch(f);
    SceneTables t;
    Counters c;
    off.Emit(f, t, c);
    FrameInfo f2;
    f2.guest_frame = 601;   // and an unlatched Emit
    off.Emit(f2, t, c);
    Check(latched && t.stage_fx.empty() && off.SlotStats().frames == 0, "mode.off_reads_nothing", "rows " + std::to_string(t.stage_fx.size()) + " frames " + std::to_string(off.SlotStats().frames));
    g_mode = 2;
  }

  // ---------------------------------------------------------------- 3. the record round trip (the rows survive the file the offline applier reads)
  {
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.stage_fx = rows;
    StageFxRow sc;
    sc.kind = kFxScreen, sc.slot = 801, sc.movie = 1, sc.seq = 42, sc.w = 320, sc.h = 180;
    t.stage_fx.push_back(sc);
    if (Plant("lose_section")) t.stage_fx.clear();
    const std::string path = "stage_fx_unit_roundtrip.rsc";
    std::string err;
    SceneTables back;
    bool ok = WriteRecord(path, t, err) && ReadRecord(path, back, err);
    SceneTables orig;
    orig.stage_fx = rows;
    orig.stage_fx.push_back(sc);
    ok = ok && back.stage_fx.size() == orig.stage_fx.size() && std::memcmp(back.stage_fx.data(), orig.stage_fx.data(), orig.stage_fx.size() * sizeof(StageFxRow)) == 0;
    Check(ok, "record.roundtrip", err);
    std::remove(path.c_str());
  }

  // ---------------------------------------------------------------- 4. the YUV -> RGBA conversion and the feed
  {
    using rf::c::YuvPlanes;
    const auto oracle = [](int Y, int Cb, int Cr, int out[3]) {   // the textbook integer BT.601 limited-range conversion
      const int c = Y - 16, d = Cb - 128, e = Cr - 128;
      const auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
      out[0] = cl((298 * c + 409 * e + 128) >> 8);
      out[1] = cl((298 * c - 100 * d - 208 * e + 128) >> 8);
      out[2] = cl((298 * c + 516 * d + 128) >> 8);
    };
    // a 4 x 2 picture: chroma 2 x 1; pixels x 0..1 share chroma texel 0, x 2..3 texel 1. Rows padded (pitch 8 / 4).
    uint8_t yb[16] = {16, 82, 145, 235, 0, 0, 0, 0, 100, 50, 200, 30, 0, 0, 0, 0};   // pitch 8: row 0 = yb[0..3], row 1 = yb[8..11]
    uint8_t cb[8] = {128, 90, 0, 0, 0, 0, 0, 0};    // pitch 4, one row (2 used)
    uint8_t cr[8] = {128, 240, 0, 0, 0, 0, 0, 0};
    uint8_t al[16] = {255, 255, 255, 255, 0, 0, 0, 0, 10, 20, 30, 40, 0, 0, 0, 0};
    YuvPlanes p;
    p.w = 4, p.h = 2;
    p.y = yb, p.y_pitch = 8;
    p.cb = Plant("swap_uv") ? cr : cb, p.cb_pitch = 4;
    p.cr = Plant("swap_uv") ? cb : cr, p.cr_pitch = 4;
    p.a = al, p.a_pitch = 8;
    std::vector<uint8_t> out;
    const bool ok = rf::c::YuvaToRgba(p, out) && out.size() == 4 * 2 * 4;
    bool vals = ok;
    std::string why;
    for (uint32_t y = 0; ok && y < 2; ++y)
      for (uint32_t x = 0; x < 4; ++x) {
        const int Yv = yb[y * 8 + x], Cb = x < 2 ? 128 : 90, Cr = x < 2 ? 128 : 240;
        int want[3];
        oracle(Yv, Cb, Cr, want);
        const uint8_t* px = &out[(size_t(y) * 4 + x) * 4];
        for (int k = 0; k < 3; ++k)
          if (std::abs(int(px[k]) - want[k]) > 2) { vals = false, why += " (" + std::to_string(x) + "," + std::to_string(y) + ") ch" + std::to_string(k) + " " + std::to_string(px[k]) + " want " + std::to_string(want[k]); }
      }
    Check(vals, "yuv.values", why);
    // the chroma planes are half size: pixel x = 2 takes chroma texel 1 (90 / 240), pixel x = 1 chroma texel 0 (128 / 128)
    std::vector<uint8_t> out2;
    YuvPlanes q = p;
    uint8_t cbw[8] = {90, 128, 0, 0, 0, 0, 0, 0}, crw[8] = {240, 128, 0, 0, 0, 0, 0, 0};   // the planted producer: the two chroma texels handed over in the other order
    if (Plant("chroma_swapped")) q.cb = cbw, q.cr = crw;
    const bool ok2 = rf::c::YuvaToRgba(q, out2);
    int a1[3], a2[3];
    oracle(82, 128, 128, a1);   // pixel (1, 0): chroma texel 0
    oracle(145, 90, 240, a2);   // pixel (2, 0): chroma texel 1
    const uint8_t* p1 = &out2[4 * 1];
    const uint8_t* p2 = &out2[4 * 2];
    bool half = ok2;
    for (int k = 0; ok2 && k < 3; ++k) half = half && std::abs(int(p1[k]) - a1[k]) <= 2 && std::abs(int(p2[k]) - a2[k]) <= 2;
    Check(half, "yuv.chroma_halved", ok2 ? ("px1 " + std::to_string(p1[0]) + "," + std::to_string(p1[1]) + "," + std::to_string(p1[2]) + " want " + std::to_string(a1[0]) + "," + std::to_string(a1[1]) + "," + std::to_string(a1[2])) : "refused");
    Check(ok && out[3] == 255 && out[7] == 255 && out[4 * 4 + 3] == 10 && out[4 * 4 + 4 * 3 + 3] == 40, "yuv.alpha_plane", "alpha " + std::to_string(ok ? out[3] : 0));
    YuvPlanes bad = p;
    bad.y_pitch = 2;   // narrower than the picture
    std::vector<uint8_t> o3;
    Check(!rf::c::YuvaToRgba(bad, o3), "yuv.refuses_bad_pitch");
    YuvPlanes noa = p;
    noa.a = nullptr;
    Check(rf::c::YuvaToRgba(noa, o3) && o3[3] == 255 && o3[4 * 5 + 3] == 255, "yuv.opaque_without_alpha");
  }
  {   // the feed: keyed by the screen key; screens 800 and 801 show the same decoded frame (content id 11): ONE conversion, shared; 802 shows another
    FakePlanes src;
    rf::c::YuvScreenFeed feed(&src);
    uint32_t w = 0, h = 0;
    const uint8_t *r800 = nullptr, *r801 = nullptr, *r802 = nullptr;
    const bool a = feed.Picture(800, 5, w, h, r800), b = feed.Picture(801, 5, w, h, r801), c = feed.Picture(802, 5, w, h, r802);
    Check(a && b && c && w == 4 && h == 2 && r800 && r801 && r802, "feed.pictures", "a " + std::to_string(int(a)) + std::to_string(int(b)) + std::to_string(int(c)));
    Check(feed.converted() == 2 && feed.shared() == 1 && r800 == r801 && r800 != r802, "feed.shared", "converted " + std::to_string(feed.converted()) + " shared " + std::to_string(feed.shared()));
    Check(r800 && r802 && std::memcmp(r800, r802, 4 * 2 * 4) == 0, "feed.same_pixels");
  }

  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
