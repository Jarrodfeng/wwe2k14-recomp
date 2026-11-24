// flash_fx_unit: the offline proof of S103 lane C3's flash writer (docs\renderer\STAGE_LIVE_WRITERS.md section 4). 0 game runs, no SDK, no game memory.
//   flash_fx_unit [--plant NAME]
// It runs the SHIPPED code (src\scene_live\flash_fx_core.h + flash_fx_contrib.h) against a FAKE GUEST built from the offsets the listings give (flash_fx_core.h header: manager word 0x836F3A7C,
// items at M + 182816 + 4 i, item vtable 0x8200F1AC, fields +4 done / +16 pos / +32 counter / +36 life / +40 scale / +44 visible / +48 pending / +56 colour index, colour blocks M + 179232 + 896 k + 96).
// THE ORACLE is written by hand from named values (the y drop is 40.0, the colour of block k is the table below). A plant is a FAULT in the fake guest or the input; each must make a NAMED check
// fail (exit 1 when any check failed):
//   swapped_yz       the guest keeps y and z swapped (+20 / +24)                      -> "flash.position"
//   array_early      the item array starts one word early (M + 182812)                 -> "flash.array"
//   all_pending      every item still has its pending flag                              -> "flash.live_rule"
//   scale_off        the scale sits at +44 instead of +40                              -> "flash.scale"
//   color_off        the colour index sits at +52 instead of +56                       -> "flash.color"
//   mode_ignored     the latch ignores --wwe2k14_rdr_scene = 0                          -> "mode.off_reads_nothing"
//   lose_flags       the round trip drops the rows' flags                               -> "record.roundtrip"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_stage_fx.h"
#include "../../src/scene_live/flash_fx_contrib.h"
#include "../../src/lever_scene.h"   // S110 FLASHHOLD F2: HoldFactor (pure)

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace ffx = rdr_scene::live::flashfx;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}
static bool Near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }
static int g_mode = 2;

struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutF(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); words[a] = b; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = ffx::Swap32(it->second);
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
};
static Guest g_guest;
static bool ReadThunk(uint32_t a, void* dst, size_t n) { return g_guest.Read(a, dst, n); }

static constexpr uint32_t kM = 0x40100000, kP = 0x40180000, kPassObj = 0x40190000;
static uint32_t ItemAddr(uint32_t i) { return 0x40200000 + i * 0x100; }

struct Spec {
  uint32_t idx;
  float x, y, z, scale;
  uint32_t done, counter, life, visible, pending, drawn, color;
  uint32_t vtable;
};

// The oracle's colour table (what the block k holds): the four measured colours of crowd_flash.md.
static const float kColor[4][4] = {{0.6f, 0.8f, 0.6f, 0.7f}, {0.8f, 6.0f, 0.6f, 0.7f}, {0.6f, 0.6f, 0.8f, 0.7f}, {0.9f, 0.9f, 1.0f, 0.7f}};

// Item words by the listings' offsets. Unspecified words are 0 (readable): the item is 64 bytes.
static void PutItem(const Spec& s) {
  const uint32_t a = ItemAddr(s.idx);
  for (uint32_t o = 0; o < 64; o += 4) g_guest.Put(a + o, 0);
  g_guest.Put(a + 0, s.vtable);
  g_guest.Put(a + 4, s.done);
  g_guest.PutF(a + 16, s.x);
  g_guest.PutF(a + (Plant("swapped_yz") ? 24 : 20), s.y);
  g_guest.PutF(a + (Plant("swapped_yz") ? 20 : 24), s.z);
  g_guest.PutF(a + 28, 1.f);
  g_guest.Put(a + 32, s.counter);
  g_guest.Put(a + 36, s.life);
  g_guest.PutF(a + (Plant("scale_off") ? 44 : 40), s.scale);
  g_guest.Put(a + (Plant("scale_off") ? 40 : 44), s.visible);
  g_guest.Put(a + 48, Plant("all_pending") ? 1u : s.pending);
  g_guest.Put(a + (Plant("color_off") ? 52 : 56), s.color);
  if (!Plant("color_off")) g_guest.Put(a + 52, Plant("not_drawn") ? 0u : s.drawn);   // +52 = the draw's "drawn" flag
}

static const float kInf = INFINITY;
static void BuildGuest(bool manager = true, bool pass = true, bool light_set = false, bool loop = true) {
  g_guest.words.clear();
  g_guest.Put(ffx::kLoopGate, loop ? 1u : 0u);
  if (manager) g_guest.Put(ffx::kManagerGlobal, kM);
  g_guest.Put(ffx::kPassGlobal, kP);
  g_guest.Put(kP + 12, pass ? kPassObj : 0u);
  g_guest.Put(ffx::kLightSetFlag, light_set ? 1u : 0u);
  for (uint32_t k = 0; k < 4; ++k)
    for (int c = 0; c < 4; ++c) g_guest.PutF(kM + ffx::kColorBlocks + ffx::kBlockStride * k + ffx::kBlockColors + 4u * uint32_t(c), kColor[k][c]);
  // the item array (24 pointers; slot 5 empty, slots 9..23 empty)
  const uint32_t arr = kM + ffx::kItemArray - (Plant("array_early") ? 4u : 0u);
  for (uint32_t i = 0; i < ffx::kItemCount; ++i) g_guest.Put(arr + 4 * i, 0);
  const uint32_t V = ffx::kItemVtable;
  const Spec specs[] = {
      //   idx x      y       z      scale done ctr life vis pend drawn col vtable
      {0, 100.f, -300.f, 200.f, 0.5f, 0, 1, 2, 1, 0, 1, 1, V},    // live, drawn, colour block 1
      {1, 10.f, -10.f, 10.f, 0.5f, 0, 0, 2, 0, 1, 0, 0, V},       // pending: the visibility test has not run
      {2, 20.f, -20.f, 20.f, 0.5f, 0, 1, 2, 0, 0, 0, 0, V},       // tested, off screen / behind a wrestler: invisible
      {3, -400.f, -120.f, 90.f, 0.5f, 1, 2, 2, 1, 0, 1, 2, V},    // done by the draw's own mark (counter == life) THIS frame: drawn this walk
      {4, 30.f, -30.f, 30.f, 0.5f, 1, 3, 2, 1, 0, 0, 0, V},       // done by the update (counter == life + 1, +52 cleared): drawn earlier, not now
      {6, 40.f, -40.f, 40.f, 0.5f, 0, 1, 2, 1, 0, 1, 0, 0x82001234u},   // not a flash item (vtable read)
      {7, -50.f, -10.f, 70.f, 0.15f, 0, 2, 2, 1, 0, 1, 3, V},     // live, shrunk, colour block 3
      {8, kInf, -10.f, 70.f, 0.5f, 0, 1, 2, 1, 0, 1, 0, V},       // non-finite position
      {9, 50.f, -50.f, 50.f, 0.5f, 0, 1, 2, 1, 0, 0, 0, V},
      {11, 0.f, 0.f, 0.f, 0.f, 0, 0, 0, 0, 0, 0, 0, V},          // never spawned: the constructor's zero scale       // the visibility-test frame: visible, pending cleared, the draw returned before drawing (+52 = 0)
  };
  for (const Spec& s : specs) {
    PutItem(s);
    g_guest.Put(arr + 4 * s.idx, ItemAddr(s.idx));
  }
}

static const StageFxRow* RowOf(const std::vector<StageFxRow>& v, uint16_t slot) {
  for (const StageFxRow& r : v)
    if (r.kind == kFxFlash && r.slot == slot) return &r;
  return nullptr;
}

// S110 FLASHHOLD (flash_hold_core.h): the hold of each flash row for k guest frames. A game flash is two rows on two guest frames: counter 2 (0.5), counter 3 (0.15).
// The plant "hold_never_releases" is a fault in the output (the last shown row stays after the hold ended): the k = 2 sequence check must catch it.
static StageFxRow GameRow(uint16_t slot, uint32_t counter, float scale) {
  StageFxRow r;
  r.kind = kFxFlash;
  r.slot = slot;
  r.seq = counter;
  r.scale = scale;
  return r;
}
static std::vector<StageFxRow> g_prev_out;
static std::vector<StageFxRow> Hold(ffx::HoldState& st, uint64_t f, uint32_t k, std::vector<StageFxRow> in) {
  std::vector<StageFxRow> out;
  ffx::HoldRows(in, f, k, st, out);
  if (Plant("hold_never_releases") && out.empty()) out = g_prev_out;   // the fault: nothing releases the last row
  g_prev_out = out;
  return out;
}
// a string of the scales shown on a slot per frame, "-" for none
static std::string Scales(const std::vector<std::vector<StageFxRow>>& frames, uint16_t slot) {
  std::string s;
  for (const auto& fr : frames) {
    const StageFxRow* r = RowOf(fr, slot);
    s += r ? (Near(r->scale, 0.5f) ? "5 " : Near(r->scale, 0.15f) ? "1 " : "? ") : "- ";
  }
  return s;
}
static void RunHoldCases() {
  const StageFxRow a0 = GameRow(3, 2, 0.5f), a1 = GameRow(3, 3, 0.15f);
  {   // k = 2: the game gives row 0 at f0, row 1 at f1, nothing after: 0.5 0.5 0.15 0.15 then nothing
    ffx::HoldState st;
    g_prev_out.clear();
    std::vector<std::vector<StageFxRow>> fr;
    fr.push_back(Hold(st, 100, 2, {a0}));
    fr.push_back(Hold(st, 101, 2, {a1}));
    for (uint64_t f = 102; f < 106; ++f) fr.push_back(Hold(st, f, 2, {}));
    const std::string got = Scales(fr, 3);
    Check(got == "5 5 1 1 - - ", "hold.k2_sequence", got);
  }
  {   // k = 1: byte-for-byte pass-through, also the same row twice; and it resets the state
    ffx::HoldState st;
    std::vector<StageFxRow> in = {a0, GameRow(5, 2, 0.5f)}, out;
    ffx::HoldRows({a0}, 100, 2, st, out);   // arm a hold
    out.clear();
    const uint32_t held = ffx::HoldRows(in, 101, 1, st, out);
    bool ok = held == 0 && out.size() == in.size() && std::memcmp(out.data(), in.data(), in.size() * sizeof(StageFxRow)) == 0;
    std::vector<StageFxRow> after;
    ffx::HoldRows({}, 102, 2, st, after);   // the hold was reset: nothing of the old flash comes back
    Check(ok && after.empty(), "hold.k1_passthrough", "held " + std::to_string(held) + " after " + std::to_string(after.size()));
  }
  {   // the same guest frame asked twice gives the same output and does not advance the hold (Emit's re-take)
    ffx::HoldState st;
    std::vector<StageFxRow> o1, o2, o3, o4;
    ffx::HoldRows({a0}, 100, 2, st, o1);
    ffx::HoldRows({a1}, 101, 2, st, o2);
    ffx::HoldRows({}, 101, 2, st, o3);   // the re-take: the game rows are not given again
    ffx::HoldRows({}, 102, 2, st, o4);
    const bool ok = o2.size() == 1 && o3.size() == 1 && std::memcmp(&o2[0], &o3[0], sizeof(StageFxRow)) == 0 && o4.size() == 1 && Near(o4[0].scale, 0.15f);
    Check(ok, "hold.same_frame_twice", "o3 " + std::to_string(o3.size()) + " o4 " + std::to_string(o4.size()));
  }
  {   // a respawn in the slot mid-hold restarts the hold: the new flash's row 0 at f2 shows f2, f3 (the old row 1 is dropped)
    ffx::HoldState st;
    g_prev_out.clear();
    std::vector<std::vector<StageFxRow>> fr;
    fr.push_back(Hold(st, 100, 2, {a0}));
    fr.push_back(Hold(st, 101, 2, {a1}));
    fr.push_back(Hold(st, 102, 2, {a0}));   // respawn: counter 2 again
    fr.push_back(Hold(st, 103, 2, {a1}));
    for (uint64_t f = 104; f < 107; ++f) fr.push_back(Hold(st, f, 2, {}));
    const std::string got = Scales(fr, 3);
    Check(got == "5 5 5 5 1 1 - ", "hold.respawn_mid_hold", got);
  }
  {   // two slots at different phases are independent
    ffx::HoldState st;
    g_prev_out.clear();
    const StageFxRow b0 = GameRow(8, 2, 0.5f), b1 = GameRow(8, 3, 0.15f);
    std::vector<std::vector<StageFxRow>> fr;
    fr.push_back(Hold(st, 100, 2, {a0}));
    fr.push_back(Hold(st, 101, 2, {a1, b0}));
    fr.push_back(Hold(st, 102, 2, {b1}));
    for (uint64_t f = 103; f < 106; ++f) fr.push_back(Hold(st, f, 2, {}));
    const std::string sa = Scales(fr, 3), sb = Scales(fr, 8);
    Check(sa == "5 5 1 1 - - " && sb == "- 5 5 1 1 - ", "hold.two_slots", sa + "| " + sb);
  }
  {   // the flag clears mid-flash (k 2 -> 1): the game's rows pass through, the held row does not stick
    ffx::HoldState st;
    g_prev_out.clear();
    std::vector<std::vector<StageFxRow>> fr;
    fr.push_back(Hold(st, 100, 2, {a0}));
    fr.push_back(Hold(st, 101, 2, {a1}));
    fr.push_back(Hold(st, 102, 1, {}));   // cleared: the game shows nothing now
    fr.push_back(Hold(st, 103, 1, {}));
    Check(Scales(fr, 3) == "5 5 - - ", "hold.flag_clears_no_stuck_row", Scales(fr, 3));
    ffx::HoldState s2;   // and a flash begun after the clear starts clean
    std::vector<StageFxRow> o;
    ffx::HoldRows({a0}, 110, 2, s2, o);
    ffx::HoldRows({}, 111, 1, s2, o);
    o.clear();
    ffx::HoldRows({a0}, 112, 2, s2, o);
    Check(o.size() == 1 && Near(o[0].scale, 0.5f), "hold.restart_after_clear");
  }
  {   // the held count: rows from the hold, not from the game this frame (f0: 0, f1: row 0 held while the game gives row 1 -> the row shown is the held row 0: 1)
    ffx::HoldState st;
    std::vector<StageFxRow> o;
    const uint32_t h0 = ffx::HoldRows({a0}, 100, 2, st, o);
    const uint32_t h1 = ffx::HoldRows({a1}, 101, 2, st, o);
    const uint32_t h2 = ffx::HoldRows({}, 102, 2, st, o);
    Check(h0 == 0 && h1 == 1 && h2 == 1, "hold.held_count", std::to_string(h0) + std::to_string(h1) + std::to_string(h2));
    Check(o.size() == 3 && !(o[0].flags & kFxfHeld) && (o[1].flags & kFxfHeld) && (o[2].flags & kFxfHeld), "hold.held_flag");   // S110 FLASHOCC: a held row says so (the near-flash census line)
  }
}

// S110 FLASHHOLD review F2: the hold factor k = round(presented updates per second / the game's own rate for the scene), as one table.
// The plant "factor_word60" is the first build's rule (k = 2 only when the lever flag is set and the word is 60): the 120 rows must fail it.
static uint32_t Factor(bool lever, uint32_t game_word, uint32_t frame_word, int32_t applied_hz) {
  if (Plant("factor_word60")) return lever && frame_word == 60 ? 2u : 1u;
  return wwe2k14::HoldFactor(lever, game_word, frame_word, applied_hz);
}
static void RunFactorAndContribHoldCases() {
  {
    struct Row { bool lever; uint32_t game_word, frame_word; int32_t applied_hz; uint32_t want; const char* what; };
    const Row rows[] = {
        {true, 60, 60, 0, 2, "our skip at 60"},
        {true, 60, 120, 120, 4, "our skip at step 120"},
        {false, 30, 60, 120, 2, "a stock 30 scene scaled to 60"},
        {false, 60, 120, 120, 2, "a real 60 match at step 120"},
        {false, 60, 60, 0, 1, "a plain 60 match"},
        {false, 30, 30, 0, 1, "word 30 unscaled"},
        {false, 60, 30, 30, 1, "a match at step 30 (word kept 60, 30 a second)"},
        {true, 30, 30, 0, 1, "our flag with a real halve (word 30)"},
        {false, 0, 0, 0, 1, "nothing seen yet"},
    };
    bool ok = true;
    std::string why;
    for (const Row& r : rows) {
      const uint32_t k = Factor(r.lever, r.game_word, r.frame_word, r.applied_hz);
      if (k != r.want) ok = false, why += std::string(" ") + r.what + ": k " + std::to_string(k) + " want " + std::to_string(r.want) + ";";
    }
    Check(ok, "hold.factor_table", why);
  }
  {   // k = 4 (our skip at step 120): each game row 4 guest frames, then nothing
    ffx::HoldState st;
    g_prev_out.clear();
    const StageFxRow a0 = GameRow(3, 2, 0.5f), a1 = GameRow(3, 3, 0.15f);
    std::vector<std::vector<StageFxRow>> fr;
    fr.push_back(Hold(st, 100, 4, {a0}));
    fr.push_back(Hold(st, 101, 4, {a1}));
    for (uint64_t f = 102; f < 110; ++f) fr.push_back(Hold(st, f, 4, {}));
    const std::string got = Scales(fr, 3);
    Check(got == "5 5 5 5 1 1 1 1 - - ", "hold.k4_sequence", got);
  }
  {   // the production order: Latch -> Take -> BuildFlashRows -> HoldRows with the glue's factor (SetHoldFn), one flash in slot 10 (life 3: drawn at counter 2, then 3 + done)
    struct Step { uint32_t done, counter; float scale; };
    const Step steps[] = {{0, 2, 0.5f}, {1, 3, 0.15f}, {1, 3, 0.15f}, {1, 3, 0.15f}, {1, 3, 0.15f}};
    ffx::FlashFxContributor con;
    con.SetReader(ReadThunk);
    if (!Plant("hold_fn_unwired")) con.SetHoldFn(+[]() -> uint32_t { return 2u; });
    std::vector<std::vector<StageFxRow>> fr;
    uint64_t frame = 900;
    for (const Step& s : steps) {
      BuildGuest();
      const uint32_t arr = kM + ffx::kItemArray;
      for (uint32_t i = 0; i < ffx::kItemCount; ++i) g_guest.Put(arr + 4 * i, 0);   // only slot 10
      PutItem({10, 5.f, -5.f, 5.f, s.scale, s.done, s.counter, 3, 1, 0, 1, 0, ffx::kItemVtable});
      g_guest.Put(arr + 40, ItemAddr(10));
      FrameInfo f;
      f.guest_frame = frame++;
      con.Latch(f);
      SceneTables t;
      Counters c;
      con.Emit(f, t, c);
      fr.push_back(t.stage_fx);
    }
    BuildGuest();
    const std::string got = Scales(fr, 10);
    Check(got == "5 5 1 1 - ", "hold.contrib_take", got + " held " + std::to_string(con.Stats().held));
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
  scene_frame::g_mode_cvar.store(&g_mode);
  BuildGuest();

  std::vector<StageFxRow> rows;
  ffx::FlashCensus cn;
  ffx::FlashState st;
  const uint32_t made = ffx::BuildFlashRows(ReadThunk, 300, rows, cn, st);

  {   // the array: items 0, 3 and 7 are the live ones, and only they make rows
    const bool ok = made == 3 && rows.size() == 3 && RowOf(rows, 0) && RowOf(rows, 3) && RowOf(rows, 7);
    Check(ok, "flash.array", "rows " + std::to_string(rows.size()));
  }
  {   // the live rule, one reason each
    bool ok = made == 3 && cn.pending == 1 && cn.invisible == 1 && cn.done == 0 && cn.not_drawn == 2 && cn.last_frame == 1 && cn.not_item == 1 && cn.bad_values == 1 && cn.unspawned == 1 && cn.empty_slots == 14;
    {   // F1: the same latch again with the SAME state: the done item (3) was seen done, it is frozen now: no ghost row
      std::vector<StageFxRow> again;
      ffx::FlashCensus c2;
      ffx::BuildFlashRows(ReadThunk, 301, again, c2, st);
      ok = ok && again.size() == 2 && !RowOf(again, 3) && c2.done == 1;
    }
    Check(ok, "flash.live_rule", "not_drawn " + std::to_string(cn.not_drawn) + " pending " + std::to_string(cn.pending) + " invisible " + std::to_string(cn.invisible) + " done " + std::to_string(cn.done) + " last " + std::to_string(cn.last_frame) +
                                     " not_item " + std::to_string(cn.not_item) + " bad " + std::to_string(cn.bad_values) + " empty " + std::to_string(cn.empty_slots));
  }
  {   // the position: x, y - 40, z in the game's space (the translation row of the sprite world; the double at 0x8200F2F0)
    const StageFxRow* a = RowOf(rows, 0);
    const StageFxRow* b = RowOf(rows, 3);
    const StageFxRow* c = RowOf(rows, 7);
    const bool ok = a && b && c && Near(a->pos[0], 100.f) && Near(a->pos[1], -340.f) && Near(a->pos[2], 200.f) && Near(b->pos[0], -400.f) && Near(b->pos[1], -160.f) && Near(b->pos[2], 90.f) &&
                    Near(c->pos[0], -50.f) && Near(c->pos[1], -50.f) && Near(c->pos[2], 70.f);
    Check(ok, "flash.position", a ? ("a (" + std::to_string(a->pos[0]) + ", " + std::to_string(a->pos[1]) + ", " + std::to_string(a->pos[2]) + ")") : "no row");
  }
  {   // the scale is the item's multiplier, flagged as such; the alpha is the measured 0.5
    const StageFxRow* a = RowOf(rows, 0);
    const StageFxRow* c = RowOf(rows, 7);
    const bool ok = a && c && Near(a->scale, 0.5f) && Near(c->scale, 0.15f) && (a->flags & kFxfGameScale) && Near(a->alpha, 0.5f);
    Check(ok, "flash.scale", a && c ? ("a " + std::to_string(a->scale) + " c " + std::to_string(c->scale)) : "no row");
  }
  {   // the colour of the item's block (index at +56), the life fraction in w, the draw-done provenance flag
    const StageFxRow* a = RowOf(rows, 0);
    const StageFxRow* b = RowOf(rows, 3);
    const StageFxRow* c = RowOf(rows, 7);
    const bool ok = a && b && c && Near(a->color[0], 0.8f) && Near(a->color[1], 6.0f) && Near(a->color[2], 0.6f) && Near(b->color[0], 0.6f) && Near(b->color[2], 0.8f) && Near(c->color[0], 0.9f) &&
                    Near(c->color[2], 1.0f) && Near(a->color[3], 0.5f) && (b->flags & kFxfDrawDone) && !(a->flags & kFxfDrawDone);
    Check(ok, "flash.color", a ? ("a rgb " + std::to_string(a->color[0]) + " " + std::to_string(a->color[1]) + " " + std::to_string(a->color[2])) : "no row");
  }
  {   // a missing value is no row + a counted reason: no manager, no pass object, an unreadable item
    std::vector<StageFxRow> r2;
    ffx::FlashCensus c2;
    BuildGuest(false, true);
    ffx::FlashState s2;
    const uint32_t n1 = ffx::BuildFlashRows(ReadThunk, 301, r2, c2, s2);
    BuildGuest(true, false);
    const uint32_t n2 = ffx::BuildFlashRows(ReadThunk, 302, r2, c2, s2);
    BuildGuest(true, true, false, false);   // [0x8392B948] = 0: the loop does not run
    const uint32_t n0 = ffx::BuildFlashRows(ReadThunk, 300, r2, c2, s2);
    BuildGuest();
    g_guest.words.erase(ItemAddr(0) + 56);   // item 0 half readable
    const uint32_t n3 = ffx::BuildFlashRows(ReadThunk, 303, r2, c2, s2);
    Check(n0 == 0 && c2.no_loop == 1 && n1 == 0 && n2 == 0 && n3 == 2 && c2.no_manager == 1 && c2.no_pass == 1 && c2.unreadable == 1, "flash.missing_is_counted",
          "n " + std::to_string(n1) + std::to_string(n2) + std::to_string(n3) + " no_manager " + std::to_string(c2.no_manager) + " no_pass " + std::to_string(c2.no_pass) + " unreadable " + std::to_string(c2.unreadable));
    BuildGuest(true, true, true);   // the light-set path: rows carry the stand-in flag
    std::vector<StageFxRow> r3;
    ffx::FlashCensus c3;
    ffx::FlashState s3;
    ffx::BuildFlashRows(ReadThunk, 304, r3, c3, s3);
    Check(!r3.empty() && (r3[0].flags & kFxfLightSet) && c3.light_set_frames == 1, "flash.light_set_flag");
    BuildGuest();
  }
  {   // F1 / F2: one flash's whole life, latched every frame (slot 10, life 3): spawn -> test -> draw x3 (the last one marks done) -> idle x2. Rows ONLY on the drawn frames.
    struct Step { uint32_t done, counter, visible, pending, drawn; bool row; const char* what; };
    const Step steps[] = {{0, 0, 0, 1, 0, false, "spawned"},   {0, 1, 1, 0, 0, false, "test frame (no draw)"}, {0, 2, 1, 0, 1, true, "draw 1"}, {0, 3, 1, 0, 1, true, "draw 2"},
                          {1, 3, 1, 0, 1, true, "draw 3 + done mark"}, {1, 3, 1, 0, 1, false, "idle (frozen)"}, {1, 3, 1, 0, 1, false, "idle (frozen)"}};
    ffx::FlashState life;
    bool ok = true;
    std::string why;
    uint32_t frame = 700;
    for (const Step& stp : steps) {
      BuildGuest();
      const uint32_t arr = kM + ffx::kItemArray - (Plant("array_early") ? 4u : 0u);
      for (uint32_t i = 0; i < ffx::kItemCount; ++i) g_guest.Put(arr + 4 * i, 0);   // only slot 10
      PutItem({10, 5.f, -5.f, 5.f, 0.5f, stp.done, stp.counter, 3, stp.visible, stp.pending, (Plant("test_drawn") && stp.pending == 0 && stp.counter == 1) ? 1u : stp.drawn, 0, ffx::kItemVtable});
      g_guest.Put(arr + 40, ItemAddr(10));
      std::vector<StageFxRow> r;
      ffx::FlashCensus c;
      ffx::BuildFlashRows(ReadThunk, frame++, r, c, life);
      if ((r.size() == 1) != stp.row) { ok = false; why += std::string(" ") + stp.what + ": rows " + std::to_string(r.size()); }
    }
    Check(ok, "flash.lifecycle", why);
  }
  BuildGuest();
  {   // the contributor: latched rows reach the record; the mode gate: --wwe2k14_rdr_scene = 0 reads nothing and writes nothing
    ffx::FlashFxContributor con;
    con.SetReader(ReadThunk);
    FrameInfo f;
    f.guest_frame = 400;
    const bool latched = con.Latch(f);
    SceneTables t;
    Counters c;
    con.Emit(f, t, c);
    Check(latched && t.stage_fx.size() == 3 && c.objects == 3, "contrib.latch_emit", "rows " + std::to_string(t.stage_fx.size()));
    g_mode = Plant("mode_ignored") ? 2 : 0;
    ffx::FlashFxContributor off;
    off.SetReader(ReadThunk);
    FrameInfo f2;
    f2.guest_frame = 600;
    const bool l2 = off.Latch(f2);
    SceneTables t2;
    Counters c2;
    off.Emit(f2, t2, c2);
    FrameInfo f3;
    f3.guest_frame = 601;   // and an unlatched Emit
    off.Emit(f3, t2, c2);
    Check(l2 && t2.stage_fx.empty() && off.Stats().frames == 0, "mode.off_reads_nothing", "rows " + std::to_string(t2.stage_fx.size()) + " frames " + std::to_string(off.Stats().frames));
    g_mode = 2;
  }
  {   // the record round trip: the rows (flags included) survive the file the offline applier reads
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.stage_fx = rows;
    if (Plant("lose_flags"))
      for (StageFxRow& r : t.stage_fx) r.flags = 0;
    const std::string path = "flash_fx_unit_roundtrip.rsc";
    std::string err;
    SceneTables back;
    bool ok = WriteRecord(path, t, err) && ReadRecord(path, back, err);
    ok = ok && back.stage_fx.size() == rows.size();
    for (size_t i = 0; ok && i < rows.size(); ++i) ok = std::memcmp(&back.stage_fx[i], &rows[i], sizeof(StageFxRow)) == 0;
    Check(ok, "record.roundtrip", err);
    std::remove(path.c_str());
  }
  RunHoldCases();
  RunFactorAndContribHoldCases();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
