// slot_vis_unit: the offline proof of S103 lane WSL (COV6 gap G5): the game's per-slot LOOK gate (sub_824E7190 0x824E72A8-0x824E7500, src\renderer_scene\stage_slot_vis.h) and the applier's
// generation reset (BH2 G5). 0 game runs, no SDK, no game memory.
//   slot_vis_unit [--plant NAME]
// It runs the SHIPPED code: ReadVisWords (the guest reads, against a FAKE GUEST built from the addresses the listing gives), SlotLookSkipped / BuildVisSkips, the marker + skip rows through the record
// round trip (record_io), VisFromRows and VisAdopt (the applier's keep rule). The ORACLE is written by hand from the listing's branch table: w skipped when (phase == 1 or [3934] or [stage+23112]) and byte
// [3A34] != 1 and [394C] == 0; e only at phase 4; h only with [3970]; g only in the game look (arenas 200..249 keep the 240-247 exceptions); slots 65-70 / 110-126 / 140-156 hidden by sub_822BC3C0 when
// phase != 4 and (game look or [3A34] != 1).
// A plant is a FAULT in the guest / the input of the thing under test; each must make a NAMED check fail (exit 1 when any check failed):
//   w_flag_lost     the stage records are handed over without the w bit (the old behaviour: w never gated)   -> "w.skipped_in_game_look"
//   phase_off       the guest's phase word reads 0 in a match                                                    -> "w.skipped_in_game_look"
//   e_as_match      the guest's phase word reads 1 in an entrance (e hidden only by the match look)             -> "e.drawn_only_at_phase4"
//   gen_ignored     the applier's marker rows all carry generation 0 (a new match keeps the old switches)      -> "adopt.new_generation_clears"
//   lose_vis        the round trip loses the stage rows                                                          -> "record.roundtrip"
//   moving_known_only  the moving-model cull is only rewritten while the mask is valid (the old condition)       -> "moving.dropped_on_new_generation"
//   arena_ignored   the applier adopts a marker of another arena's stage                                         -> "adopt.other_arena_ignored"
//   cycle_all_drawn the cycle skips are never merged (the old behaviour: every slot of a group drawn)           -> "cycle.pick_at_ticks"
//   cycle_pick_last the pick falls to the last slot whatever the tick                                            -> "cycle.pick_at_ticks"
//   tail_polarity   the hide lists apply when their bit is CLEAR (the polarity read wrong)                         -> "tail.hide_list_needs_bit_set"
//   tail_pairs_swapped  the 240-247 words select the other member of each pair                                   -> "tail.pairs_240_247"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_stage_fx.h"
#include "../../src/renderer_scene/stage_slot_vis.h"
#include "../../src/scene_live/stage_fx_contrib.h"

using namespace rdr_scene;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- the fake guest: bytes by guest address (big-endian words), a missing byte = unreadable
struct Guest {
  std::map<uint32_t, uint8_t> b;
  void W(uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[a + uint32_t(i)] = uint8_t(v >> (24 - 8 * i));
  }
  void B(uint32_t a, uint8_t v) { b[a] = v; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i) {
      auto it = b.find(a + uint32_t(i));
      if (it == b.end()) return false;
      p[i] = it->second;
    }
    return true;
  }
};

constexpr uint32_t kStage = 0x40000000u, kTable = 0x50000000u;

struct World {
  uint32_t phase = 1, b3a34 = 0, w3934 = 0, st23112 = 0, w394C = 0, w3970 = 0, arena = 5, g_key = 5;
  uint32_t st22116 = 0, st22120 = 0, st22124 = 0;
  uint32_t tailw[4] = {0, 0, 0, 1};   // S103 CAR: [stage+21200+4i] of a created arena (the 200..249 tail); default skips B = {242, 244, 246} and A_3 = 241, so 240 / 245 / 243 pass the tail
  uint8_t g_byte0 = 0;
  uint8_t rec37 = 0;   // byte +37 of the table record whose key is g_key
};
static Guest Make(const World& w) {
  Guest g;
  uint32_t ph = w.phase;
  if (Plant("phase_off") && ph == 1) ph = 0;
  if (Plant("e_as_match") && ph == 4) ph = 1;
  g.W(kVisPhaseWord, ph);
  g.W(kVisLookByteWord, w.b3a34 << 24);   // lbz: the word's high byte
  g.W(kVisWord3934, w.w3934);
  g.W(kStage + kStageOff23112, w.st23112);
  g.W(kVisWord394C, w.w394C);
  g.W(kVisWord3970, w.w3970);
  g.W(kStage + kStageOffArenaId, w.arena);
  g.W(kStage + kStageOff22116, w.st22116);
  g.W(kStage + kStageOff22120, w.st22120);
  g.W(kStage + kStageOff22124, w.st22124);
  g.W(kStage + 21180, 0), g.W(kStage + 21196, 0);   // S103 CAR: the 200..249 tail words (read only for a created arena): variant 0, no list bit, W = tailw, every list empty
  for (int i = 0; i < 4; ++i) g.W(kStage + 21200 + 4 * uint32_t(i), w.tailw[i]);
  for (uint32_t k = 0; k < 14 * 30 + 30; ++k) g.B(kStage + (k < 14 * 30 ? 21216 + 2 * k : 22056 + 2 * (k - 14 * 30)), 0xFF), g.B(kStage + (k < 14 * 30 ? 21216 + 2 * k : 22056 + 2 * (k - 14 * 30)) + 1, 0xFF);
  for (uint32_t k = 0; k < kCycleMaxGroups; ++k) g.W(kStage + kCycleGroupsAt + 4 * k, 0);   // S103 CAR: no cycle objects (the 16 words the take always reads)
  g.W(kCycleWord835E1924, 1), g.W(kCycleGateBlockWord, 0);
  g.B(kVisGlobalBlock, w.g_byte0);
  g.W(kVisGlobalBlock + 64, w.g_key);
  g.W(kVisArenaTablePtrWord, kTable);
  g.W(kVisArenaTableCountWord, 3);
  for (uint32_t i = 0; i < 3; ++i) {   // three 108-byte records: keys 2, 5 (ours), 7; only the key-5 record may carry byte +37
    const uint8_t key = i == 0 ? 2 : i == 1 ? 5 : 7;
    for (uint32_t k = 0; k < 108; ++k) g.B(kTable + 108 * i + k, 0);
    g.B(kTable + 108 * i + 4, key);
    if (key == 5) g.B(kTable + 108 * i + 37, w.rec37);
    if (key == 2 || key == 7) g.B(kTable + 108 * i + 37, 1);   // another arena's record: must NOT count
  }
  return g;
}

static uint8_t g_recs[kSlotVisSlots * kStageRecordSize];
static void Rec(uint32_t slot, uint8_t b6) {
  uint8_t* r = g_recs + slot * kStageRecordSize;
  std::memset(r, 0, kStageRecordSize);
  r[9] = 0x80;
  r[6] = b6;
  if (Plant("w_flag_lost")) r[6] = uint8_t(b6 & ~2u);
}
static void Records() {
  std::memset(g_recs, 0, sizeof g_recs);
  for (uint32_t s = 0; s < kSlotVisSlots; ++s) Rec(s, 0);
  Rec(110 + 0, 0x02);   // 110 is also in the hide range: use 130 / 135 for the plain w cases
  Rec(130, 0x02);       // w
  Rec(135, 0x02);       // w
  Rec(131, 0x00);       // plain
  Rec(216, 0x01);       // e
  Rec(223, 0x08);       // g
  Rec(300, 0x09);       // e + g
  Rec(65, 0x00);        // hide range
  Rec(70, 0x00);
  Rec(71, 0x00);        // just outside
  Rec(126, 0x00);
  Rec(127, 0x00);
  Rec(140, 0x00);
  Rec(156, 0x00);
  Rec(157, 0x00);
  Rec(240, 0x08), Rec(242, 0x08), Rec(243, 0x08), Rec(245, 0x08);
  Rec(150, 0x04);       // h
}

static std::vector<uint16_t> Skips(const World& w, bool* known = nullptr) {
  Guest g = Make(w);
  SlotVisWords vw;
  const bool ok = ReadVisWords([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, vw);
  if (known) *known = ok;
  std::vector<uint16_t> out;
  if (ok) BuildVisSkips(g_recs, vw, out);
  return out;
}
static bool Has(const std::vector<uint16_t>& v, uint32_t s) {
  for (uint16_t x : v)
    if (x == s) return true;
  return false;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
  Records();

  // ---------------------------------------------------------------- 1. w
  {
    World w;   // a match: phase 1, nothing else set
    auto s = Skips(w);
    Check(Has(s, 130) && Has(s, 135) && !Has(s, 131), "w.skipped_in_game_look", "the match look skips the w slots and only them");
    w.w394C = 1;
    s = Skips(w);
    Check(!Has(s, 130), "w.drawn_with_394C", "[0x836F394C] != 0 draws w");
    w = World();
    w.b3a34 = 1;
    s = Skips(w);
    Check(!Has(s, 130), "w.drawn_with_3A34", "byte [0x836F3A34] == 1 draws w");
    w = World();
    w.phase = 4;
    s = Skips(w);
    Check(!Has(s, 130), "w.drawn_outside_game_look", "the entrance look (phase 4, nothing else) draws w");
    w.w3934 = 1;   // [0x836F3934] makes it the game look even at phase 4
    s = Skips(w);
    Check(Has(s, 130), "w.3934_is_game_look", "[0x836F3934] != 0 is a game look word");
    w = World();
    w.phase = 4;
    w.st23112 = 5;
    s = Skips(w);
    Check(Has(s, 130), "w.stage23112_is_game_look", "[stage+23112] != 0 is a game look word");
  }
  // ---------------------------------------------------------------- 2. e
  {
    bool ok = true;
    std::string why;
    for (uint32_t ph : {0u, 1u, 2u, 3u, 5u}) {
      World w;
      w.phase = ph;
      if (!Has(Skips(w), 216)) ok = false, why += " phase" + std::to_string(ph) + "-drawn";
    }
    World w;
    w.phase = 4;
    if (Has(Skips(w), 216)) ok = false, why += " phase4-skipped";
    Check(ok, "e.drawn_only_at_phase4", why);
  }
  // ---------------------------------------------------------------- 3. g and h
  {
    World w;   // match = game look: g drawn
    auto s = Skips(w);
    Check(!Has(s, 223), "g.drawn_in_game_look", "");
    w.phase = 4;
    s = Skips(w);
    Check(Has(s, 223) && Has(s, 240) && Has(s, 245), "g.skipped_in_entrance_look", "arena 5: no exceptions (sub_822A7240 false)");
    w.arena = 210;   // 200..249: the exceptions
    w.st22120 = 1, w.st22116 = 0, w.st22124 = 0;
    s = Skips(w);
    Check(!Has(s, 240) && Has(s, 242) && Has(s, 245), "g.exception_22120", "slots 240 / 241 drawn by [stage+22120]");
    w.st22116 = 1;
    w.st22120 = 0;
    s = Skips(w);
    Check(Has(s, 240) && !Has(s, 245) && Has(s, 242), "g.exception_22116", "slots 244-247 drawn by [stage+22116]");
    w.st22116 = 0;
    w.st22124 = 1;
    w.tailw[0] = 1;   // the tail then skips A_0 = 243, not 242: the exception itself is what lets 242 through
    s = Skips(w);
    Check(!Has(s, 242) && Has(s, 243) && Has(s, 240) && Has(s, 245), "g.exception_22124", "slots 242 / 243 pass [stage+22124]: 242 drawn (W_0 != 0), 243 then hidden by the tail (A_0)");
    w.tailw[0] = 0;
    s = Skips(w);
    Check(Has(s, 242) && !Has(s, 243), "g.exception_22124_b", "W_0 == 0: 243 passes the exception and the tail, 242 is B_0"); 
    World h;   // h: only with [3970]
    s = Skips(h);
    bool ok = Has(s, 150);
    h.w3970 = 1;
    ok = ok && !Has(Skips(h), 150);
    Check(ok, "h.needs_3970", "");
  }
  // ---------------------------------------------------------------- 4. the hide ranges (sub_822BC3C0)
  {
    World w;
    w.g_byte0 = 27;   // G byte in 25..31
    auto s = Skips(w);
    bool ok = Has(s, 65) && Has(s, 70) && !Has(s, 71) && Has(s, 110) && Has(s, 126) && !Has(s, 127) && Has(s, 140) && Has(s, 156) && !Has(s, 157);
    Check(ok, "range.hidden_in_game_look", "65-70 / 110-126 / 140-156 hidden by the G byte");
    w.phase = 4;
    Check(!Has(Skips(w), 65), "range.not_at_phase4", "phase 4 draws them");
    w = World();
    w.rec37 = 1;   // the arena record's byte +37
    Check(Has(Skips(w), 65), "range.by_record_37", "key 5's record byte +37 hides them");
    w.g_key = 210;   // 200..249 remaps to 200: no record with key 200 -> not found
    Check(!Has(Skips(w), 65), "range.200_remap_no_record", "key 210 looks up 200: no record, not hidden");
    w = World();
    w.rec37 = 0;
    Check(!Has(Skips(w), 65), "range.not_when_clear", "key 5 with byte +37 == 0 hides nothing (other keys' bytes do not count)");
    w.b3a34 = 1;   // phase 1 is a game look: !r28 not needed
    w.rec37 = 1;
    Check(Has(Skips(w), 65), "range.game_look_ignores_3A34", "r30 alone opens the test");
  }
  // ---------------------------------------------------------------- 6. a word the guest cannot read: nothing is claimed
  {
    World w;
    Guest g = Make(w);
    g.b.erase(kVisWord394C + 1);
    SlotVisWords vw;
    Check(!ReadVisWords([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, vw) && !vw.known, "words.fail_closed", "one unreadable word = known false");
    Guest g2 = Make(w);
    SlotVisWords v2;
    Check(!ReadVisWords([&g2](uint32_t a, void* d, size_t n) { return g2.Read(a, d, n); }, 0, v2), "words.no_stage", "no stage = nothing read");
  }
  // ---------------------------------------------------------------- 6b. S110 BARRICADE (site 6): the game's head of sub_824E7190 (0x824E71AC-0x824E71F0): the per-slot SHOW bit
  {
    World w;
    Guest g = Make(w);
    SlotVisWords vw;
    ReadVisWords([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, vw);
    uint8_t recs[kSlotVisSlots * kStageRecordSize];
    std::memcpy(recs, g_recs, sizeof recs);
    auto set9 = [&](uint32_t s, uint8_t b8, uint8_t b9) { recs[s * kStageRecordSize + 8] = b8, recs[s * kStageRecordSize + 9] = b9; };
    set9(131, 0x00, 0x80);   // shown
    set9(132, 0x00, 0x00);   // show bit clear (the chairs' hide, the story hooks)
    set9(133, 0x00, 0x88);   // bit 0x08 of byte +9
    set9(134, 0x80, 0x80);   // +8 bit 0x80 (a cycle group's slot: never drawn by the stage pass)
    std::vector<uint16_t> sk;
    BuildVisSkips(recs, vw, sk);
    Check(!Has(sk, 131) && Has(sk, 132) && Has(sk, 133) && Has(sk, 134), "show.bit_clear_skipped", "a slot whose byte +9 bit 0x80 is clear (or bit 0x08 set, or +8 bit 0x80) is skipped; a shown one is not");
    // a cycle slot still follows CycleSkips: listed slots 295 / 296 carry the builder's +8 bit 0x80; the picked one is drawn, the other skipped
    set9(295, 0x80, 0x80), set9(296, 0x80, 0x80);
    BuildVisSkips(recs, vw, sk);
    CycleGroup cg;
    cg.n = 2, cg.slot[0] = 295, cg.slot[1] = 296, cg.thr[0] = 100, cg.thr[1] = 200, cg.tick = 50;   // pick = 295
    CycleWords cw;
    cw.phase = vw.phase;
    CycleSkips({cg}, cw, recs, sk);
    Check(!Has(sk, 295) && Has(sk, 296) && Has(sk, 132), "show.cycle_slot_follows_cycle", "the picked cycle slot is drawn although its +8 bit is set, the other is skipped, the show-bit skips stay");
  }
  // ---------------------------------------------------------------- 7. the rows (round trip) and the applier's keep rule
  {
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    StageVis v;
    v.marker = true, v.known = true, v.gen = 7, v.stage = kStage;
    World w;
    v.skipped = Skips(w);
    VisToRows(v, t.stage_fx);
    if (Plant("lose_vis")) t.stage_fx.clear();
    const std::string path = "slot_vis_unit_roundtrip.rsc";
    std::string err;
    SceneTables back;
    bool ok = WriteRecord(path, t, err) && ReadRecord(path, back, err);
    StageVis got;
    bool valid = false;
    ok = ok && VisFromRows(back.stage_fx, got, valid) && valid && got.gen == 7 && got.stage == kStage && got.skipped == v.skipped && !v.skipped.empty();
    Check(ok, "record.roundtrip", err);
    std::remove(path.c_str());

    // the applier: generation 1 learns a list and show bits; an unread frame keeps them; generation 2 starts empty
    StageVis keep;
    StageShow show;
    show.known = true, show.keys = 0x3FF, show.art = 0xF;
    auto frame = [&](uint32_t gen, bool valid_, std::vector<uint16_t> sk) {
      StageVis n;
      n.marker = true, n.known = valid_, n.gen = Plant("gen_ignored") ? 0 : gen, n.stage = kStage;
      if (valid_) n.skipped = std::move(sk);
      std::vector<StageFxRow> rows;
      VisToRows(n, rows);
      StageVis rn;
      bool rv = false;
      if (VisFromRows(rows, rn, rv)) VisAdopt(keep, show, std::move(rn), rv);
    };
    frame(1, true, {130, 135});
    show.known = true, show.keys = 0x3FF, show.art = 0xF;   // the same frame's kFxShow rows (ApplyShowMasks reads them after the generation check)
    const bool learned = keep.known && keep.skipped.size() == 2 && show.known;
    frame(1, false, {});
    const bool kept = keep.known && keep.skipped.size() == 2 && show.known;
    Check(learned && kept, "adopt.unread_keeps_last", "an unread frame of the same stage keeps the list and the show bits");
    frame(2, false, {});   // a NEW stage, words unread: nothing of the old match survives
    Check(!keep.known && keep.skipped.empty() && !show.known && keep.gen == 2, "adopt.new_generation_clears", "a new stage generation drops the old skip list and the old show bits");
    frame(2, true, {131});
    Check(keep.known && keep.skipped.size() == 1 && keep.skipped[0] == 131, "adopt.valid_replaces", "");
  }
  // ---------------------------------------------------------------- 8. WSL fix round: the old look rule as a mask, the moving models, the arena in the marker, the table walk, the generation order
  {
    const std::vector<std::pair<int, uint8_t>> b6 = {{216, 0x01}, {223, 0x08}, {300, 0x09}, {130, 0x02}};   // e, g, e+g, w
    StageVis none;   // nothing read
    std::vector<uint8_t> c;
    VisCull(c, none, true, false, b6);   // match look, words unread: the loader's rule as a mask
    Check(c.size() > 216 && c[216] == 3 && c[223] == 0 && c[300] == 0 && c[130] == 0, "fallback.match_look", "e-only hidden, g and e+g and w drawn");
    c.clear();
    VisCull(c, none, true, true, b6);    // entrance look
    Check(c.size() > 223 && c[216] == 0 && c[223] == 3 && c[300] == 0, "fallback.entrance_look", "g-only hidden, e drawn");
    StageVis known;
    known.marker = known.known = true, known.skipped = {130};
    c.clear();
    VisCull(c, known, true, false, b6);
    Check(c.size() > 130 && c[130] == 3 && c[216] == 0, "fallback.known_mask_wins", "a valid game mask replaces the old rule");
    c.clear();
    VisCull(c, known, false, false, b6);   // the switch off: the old rule only
    Check(c.size() > 216 && c[130] == 0 && c[216] == 3, "switch.off_is_old_rule", "wwe2k14_rdr_slot_look false");

    // 3. a moving one-slot model drops the previous generation's cull when a new generation starts (marker, mask not yet valid)
    StageVis keep;
    StageShow show;
    auto rows_of = [&](uint32_t gen, bool valid, uint32_t arena, std::vector<uint16_t> sk) {
      StageVis n;
      n.marker = true, n.known = valid, n.gen = gen, n.stage = kStage, n.arena = arena;
      if (valid) n.skipped = std::move(sk);
      std::vector<StageFxRow> r;
      VisToRows(n, r);
      return r;
    };
    VisAdoptRows(rows_of(1, true, 5, {130}), 5, keep, show);
    VisAdoptRows(rows_of(2, false, 5, {}), 5, keep, show);   // a new match: marker, words not read yet
    const bool gets = Plant("moving_known_only") ? keep.known : VisMovingGetsCull(false, keep);
    std::vector<uint8_t> mv;
    VisCull(mv, keep, true, false, {});
    Check(gets && (mv.empty() || mv[130] == 0), "moving.dropped_on_new_generation", "the moving model is rewritten with the new generation's (empty) cull");
    Check(VisMovingGetsCull(true, StageVis()) && !VisMovingGetsCull(false, StageVis()), "moving.art_slots_always", "an art-slot model is always rewritten; others only once a generation was seen");

    // 5. the marker names the stage's arena; the applier adopts only its own
    StageVis k5;
    StageShow s5;
    const bool other = Plant("arena_ignored") ? true : VisAdoptRows(rows_of(1, true, 7, {130}), 5, k5, s5);   // the plant: the applier adopts it
    Check(!other && !k5.marker && !k5.known, "adopt.other_arena_ignored", "a stage of arena 7 is not adopted by arena 5");
    Check(VisAdoptRows(rows_of(1, true, 5, {130}), 5, k5, s5) && k5.known && k5.skipped.size() == 1, "adopt.own_arena", "");
    StageVis unread;
    unread.marker = true, unread.known = true, unread.arena = kVisArenaUnread;
    Check(!VisIsForArena(unread, 5) && !VisIsForArena(unread, -1), "adopt.unread_arena_refused", "an unreadable [stage+1216] is never adopted");

    // 7. the arena table is not walked when rule 1 cannot apply (phase 4, or no game look with [3A34] == 1)
    {
      World w;
      w.phase = 4;
      Guest g = Make(w);
      for (uint32_t k = 0; k < 108; ++k) g.b.erase(kTable + 108 + k);   // the table is unreadable
      SlotVisWords vw;
      Check(ReadVisWords([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, vw) && vw.known && !vw.hide_ranges, "table.skipped_at_phase4", "no table read at phase 4");
      World w2;
      w2.phase = 2, w2.b3a34 = 1;   // r31 false, r30 false, r28 true: rule 1 cannot apply
      Guest g2 = Make(w2);
      for (uint32_t k = 0; k < 108; ++k) g2.b.erase(kTable + 108 + k);
      SlotVisWords v2;
      Check(ReadVisWords([&g2](uint32_t a, void* d, size_t n) { return g2.Read(a, d, n); }, kStage, v2) && v2.known, "table.skipped_without_game_look", "no table read when !r30 && r28");
      World w3;   // a match: the table IS needed, an unreadable one fails closed
      Guest g3 = Make(w3);
      for (uint32_t k = 0; k < 108; ++k) g3.b.erase(kTable + 108 + k);
      SlotVisWords v3;
      Check(!ReadVisWords([&g3](uint32_t a, void* d, size_t n) { return g3.Read(a, d, n); }, kStage, v3), "table.needed_in_match", "an unreadable table in a match = known false");
    }

    // 4. the contributor: a new stage object bumps the generation (before the swap), the same stage does not, and a take pairs the stage with its generation and its arena
    {
      World w;
      w.arena = 5;
      Guest g = Make(w);
      for (uint32_t s = 0; s < kSlotVisSlots * kStageRecordSize; ++s) g.B(kStage + kStageSlotRecordsAt + s, g_recs[s]);
      static Guest* gp;
      gp = &g;
      static int mode = 2;   // scene_frame mode "on" (the cvar the latch reads)
      scene_frame::g_mode_cvar.store(&mode);
      rdr_scene::live::stagefx::StageFxContributor con;
      con.SetReader(+[](uint32_t a, void* d, size_t n) { return gp->Read(a, d, n); });
      const uint32_t g0 = con.StageGen();
      con.SetStage(kStage);
      const uint32_t g1 = con.StageGen();
      con.SetStage(kStage);   // the same stage: no new generation
      const uint32_t g1b = con.StageGen();
      rdr_scene::live::FrameInfo fi;
      fi.guest_frame = 10;
      con.Latch(fi);
      SceneTables t;
      rdr_scene::live::Counters cn;
      con.Emit(fi, t, cn);
      StageVis got;
      bool valid = false;
      const bool have = VisFromRows(t.stage_fx, got, valid);
      Check(g1 == g0 + 1 && g1b == g1, "gen.bumped_on_new_stage", "a new stage object bumps, the same one does not");
      Check(have && got.stage == kStage && got.gen == g1 && got.arena == 5, "take.pairs_stage_gen_arena", "");
      con.SetStage(kStage + 0x1000);
      Check(con.StageGen() == g1 + 1, "gen.bumped_on_swap", "");
    }
  }
  // ---------------------------------------------------------------- 9. S103 CAR (BCA G2): the cycle groups, the game's one-slot pick (sub_83420EA0)
  {
    using Pairs = std::vector<std::pair<uint16_t, uint16_t>>;   // (slot, ticks)
    // writes one cycle object at `obj` into the stage's array slot k; thresholds are CUMULATIVE like the game's (sub_83420CB0)
    auto put = [&](Guest& g, uint32_t k, uint32_t obj, const Pairs& st, uint16_t tick) {
      g.W(kStage + kCycleGroupsAt + 4 * k, obj);
      g.W(obj, kCycleVt0), g.W(obj + 24, kCycleVt24), g.W(obj + 40, kCycleVt40), g.W(obj + 164, kStage);
      g.B(obj + 104, uint8_t(st.size()));
      uint32_t cum = 0;
      for (uint32_t i = 0; i < st.size(); ++i) {
        cum += st[i].second;
        g.B(obj + 56 + 2 * i, uint8_t(st[i].first >> 8)), g.B(obj + 57 + 2 * i, uint8_t(st[i].first));
        g.B(obj + 106 + 2 * i, uint8_t(cum >> 8)), g.B(obj + 107 + 2 * i, uint8_t(cum));
      }
      g.B(obj + 154, uint8_t(cum >> 8)), g.B(obj + 155, uint8_t(cum));
      g.B(obj + 156, uint8_t(tick >> 8)), g.B(obj + 157, uint8_t(tick));
    };
    // what the take computes for a guest: the cycle skips (read + merged). Planted twins: "all drawn" (never merged) and "pick = last" (the pick falls to the last slot whatever the tick)
    auto cyc_skips = [&](Guest& g, bool* ok = nullptr) {
      std::vector<CycleGroup> grp;
      CycleWords cw;
      const bool good = ReadCycleGroups([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, grp, cw);
      if (ok) *ok = good;
      std::vector<uint16_t> out;
      if (Plant("cycle_pick_last"))
        for (CycleGroup& x : grp) x.tick = 0xFFFF;
      if (good && !Plant("cycle_all_drawn")) CycleSkips(grp, cw, g_recs, out);
      return out;
    };
    const Pairs a0 = {{295, 250}, {296, 35}, {297, 35}, {298, 250}};      // arena 0 (BCA data), length 570
    const Pairs a52 = {{295, 250}, {296, 35}, {297, 250}, {296, 35}};     // arena 52: 296 listed twice, 298 not in the group
    auto drawn_of = [&](const std::vector<uint16_t>& sk, const Pairs& st) {
      std::vector<uint32_t> d;
      for (const auto& x : st)
        if (!Has(sk, x.first) && std::find(d.begin(), d.end(), x.first) == d.end()) d.push_back(x.first);
      return d;
    };
    const struct { uint16_t tick, want; } cases[] = {{0, 295}, {249, 295}, {250, 296}, {285, 297}, {320, 298}, {569, 298}};
    bool all = true;
    std::string why;
    for (const auto& c : cases) {
      World w;
      Guest g = Make(w);
      put(g, 0, 0x60000000u, a0, c.tick);
      const auto d = drawn_of(cyc_skips(g), a0);
      const bool ok = d.size() == 1 && d[0] == c.want;
      all = all && ok;
      if (!ok) why += " tick " + std::to_string(c.tick) + " drew " + std::to_string(d.size()) + " (first " + std::to_string(d.empty() ? 0 : d[0]) + ", want " + std::to_string(c.want) + ");";
    }
    Check(all, "cycle.pick_at_ticks", "arena 0: tick 0 / 249 -> 295, 250 -> 296, 285 -> 297, 320 / 569 -> 298, exactly one drawn" + why);
    {
      World w;
      Guest g = Make(w);
      put(g, 0, 0x60000000u, a52, 260);   // 250 <= 260 < 285: slot 296 (index 1)
      auto d = drawn_of(cyc_skips(g), a52);
      Check(d.size() == 1 && d[0] == 296, "cycle.arena52_twice", "296 listed twice is drawn when it is the pick");
      Guest g2 = Make(w);
      put(g2, 0, 0x60000000u, a52, 0);
      d = drawn_of(cyc_skips(g2), a52);
      Check(d.size() == 1 && d[0] == 295, "cycle.arena52_other_pick", "296 listed twice is skipped when it is not the pick");
    }
    {   // the skips merged into the look skips: ascending, unique; a slot both look-skipped and cycle-skipped is listed once
      World w;
      Guest g = Make(w);
      put(g, 0, 0x60000000u, a0, 0);
      std::vector<CycleGroup> grp;
      CycleWords cw;
      ReadCycleGroups([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, grp, cw);
      std::vector<uint16_t> m = {130, 296, 300};
      CycleSkips(grp, cw, g_recs, m);
      Check(m == std::vector<uint16_t>({130, 296, 297, 298, 300}), "cycle.merge_sorted_unique", "296 once, 295 (the pick) not added");
    }
    {   // the whole-group skip [[0x83767B74]+368] != 0 hides the pick too; a zero word / a foreign object / a wrong stage back-pointer is skipped, never fatal
      World w;
      Guest g = Make(w);
      put(g, 0, 0x60000000u, a0, 0);
      g.W(kCycleGateBlockWord, 0x61000000u), g.W(0x61000000u + kCycleGateOff, 1);
      const auto d = drawn_of(cyc_skips(g), a0);
      Check(d.empty(), "cycle.whole_group_skip", "[[0x83767B74]+368] != 0: no slot of the group is drawn");
      Guest g2 = Make(w);
      put(g2, 3, 0x60000000u, a0, 0);   // index 3 (words 0-2 stay empty: never stop at the first 0)
      put(g2, 5, 0x62000000u, a0, 0);
      g2.W(0x62000000u + 164, kStage + 4);   // a foreign back-pointer: not ours
      std::vector<CycleGroup> grp;
      CycleWords cw;
      ReadCycleGroups([&g2](uint32_t a, void* dd, size_t n) { return g2.Read(a, dd, n); }, kStage, grp, cw);
      Check(grp.size() == 1, "cycle.foreign_and_empty_words", "index 0-2 empty, index 3 read, index 5 (wrong stage) skipped");
    }
    {   // fail closed: a valid object whose thresholds are unreadable = the read fails (the take is unread); no group = true
      World w;
      Guest g = Make(w);
      put(g, 0, 0x60000000u, a0, 0);
      g.b.erase(0x60000000u + 107);
      bool ok = true;
      cyc_skips(g, &ok);
      Check(!ok, "cycle.unread_fails_closed", "an unreadable word of a valid object = false");
      Guest g2 = Make(w);
      bool ok2 = false;
      cyc_skips(g2, &ok2);
      Check(ok2, "cycle.no_groups_ok", "an arena without cycle groups reads true with no skips");
    }
    {   // S103 CAR fix 1 / 2: the cycle rule owns the cycled slots; two passes (a pick another group lists as a non-pick stays drawn)
      World w;
      Guest g = Make(w);
      put(g, 0, 0x60000000u, a0, 0);   // pick 295
      std::vector<CycleGroup> grp;
      CycleWords cw;
      ReadCycleGroups([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, grp, cw);
      std::vector<uint16_t> m = {295, 130};   // the look gate already skipped the picked 295
      CycleSkips(grp, cw, g_recs, m);
      Check(!Has(m, 295) && Has(m, 296) && Has(m, 130), "cycle.owns_listed_slots", "a look-gate skip of the picked cycled slot is dropped (the game's stage pass never looks at it)");
      CycleGroup g2 = grp[0];   // a second group that lists the first group's pick (295) as a NON-pick (its own pick is 296)
      g2.n = 2, g2.slot[0] = 296, g2.slot[1] = 295, g2.thr[0] = 100, g2.thr[1] = 200, g2.tick = 50;   // pick = index 0 = 296
      std::vector<CycleGroup> two = {grp[0], g2};
      std::vector<uint16_t> m2;
      CycleSkips(two, cw, g_recs, m2);
      Check(!Has(m2, 295) && !Has(m2, 296) && Has(m2, 297) && Has(m2, 298), "cycle.two_pass_picks_stay_drawn", "295 (group 1's pick) and 296 (group 2's pick) are both drawn although each lists the other");
    }
    {   // S103 CAR fix 3 / 4: a valid mask that skips none of a group's slots = the group was not read: the first-slot fallback; a created arena's unread mask hides the tail's slots
      StageVis known;
      known.marker = known.known = true, known.skipped = {130};
      std::vector<uint8_t> c;
      VisCull(c, known, true, false, {}, {{295, 296, 297, 298}});
      Check(c.size() > 298 && c[295] == 0 && c[296] == 3 && c[297] == 3 && c[298] == 3 && c[130] == 3, "cycle.known_mask_unread_group_fallback", "mask valid, group slots none skipped: only 295 shown");
      known.skipped = {130, 296, 297, 298};
      c.clear();
      VisCull(c, known, true, false, {}, {{295, 296, 297, 298}});
      Check(c[295] == 0 && c[296] == 3, "cycle.known_mask_read_group_untouched", "the mask's own skips stand");
      StageVis none;
      c.clear();
      VisCull(c, none, true, false, {}, {}, true);
      Check(c.size() > 247 && c[240] == 3 && c[247] == 3 && c[40] == 3 && c[94] == 3 && c[29] == 3 && c[88] == 3 && c[100] == 0, "tail.unread_hides_custom", "arena 200..249, mask unknown: 240-247 and both ring-side variants hidden");
      c.clear();
      VisCull(c, none, true, false, {}, {}, false);
      Check(c.empty() || (c.size() > 247 && c[240] == 0), "tail.unread_ordinary_untouched", "an ordinary arena hides nothing extra");
    }
    {   // the loader's fallback: every cycled slot but each group's first (the tick-0 state)
      const std::vector<int> h = CycleFallbackHide({{295, 296, 297, 298}});
      std::vector<uint8_t> c;
      StageVis none;
      VisCull(c, none, true, false, {}, {{295, 296, 297, 298}});
      Check(h == std::vector<int>({296, 297, 298}) && c.size() > 298 && c[295] == 0 && c[296] == 3 && c[298] == 3, "cycle.fallback_first_slot", "words unread: only the first slot of the group is shown");
    }
  }
  // ---------------------------------------------------------------- 10. S103 CAR (BCA G1 piece 1): the 200..249 tail of sub_824E7190 (0x824E74F0-0x824E765C)
  {
    // a created arena's stage words: [stage+21180], [+21196], W at +21200, the 14 hide lists (sorted u16, 0xFFFF ends) at +21216 + 60 c, the always list at +22056
    struct Tail {
      uint32_t w21180 = 0, bits = 0, w[4] = {0, 0, 0, 0};
      std::vector<std::vector<uint16_t>> lists = std::vector<std::vector<uint16_t>>(14);
      std::vector<uint16_t> always;
    };
    auto put_list = [](Guest& g, uint32_t at, const std::vector<uint16_t>& l) {
      for (uint32_t k = 0; k < 30; ++k) {
        const uint16_t v = k < l.size() ? l[k] : 0xFFFFu;
        g.B(at + 2 * k, uint8_t(v >> 8)), g.B(at + 2 * k + 1, uint8_t(v));
      }
    };
    auto skips = [&](const Tail& t, uint32_t arena) {
      World w;
      w.arena = arena;
      Guest g = Make(w);
      g.W(kStage + 21180, t.w21180), g.W(kStage + 21196, Plant("tail_polarity") ? ~t.bits : t.bits);
      for (int i = 0; i < 4; ++i) g.W(kStage + 21200 + 4 * uint32_t(i), Plant("tail_pairs_swapped") ? (t.w[i] ? 0u : 1u) : t.w[i]);
      for (int c = 0; c < 14; ++c) put_list(g, kStage + 21216 + 60 * uint32_t(c), t.lists[size_t(c)]);
      put_list(g, kStage + 22056, t.always);
      SlotVisWords vw;
      const bool ok = ReadVisWords([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, vw);
      std::vector<uint16_t> out;
      if (ok) BuildVisSkips(g_recs, vw, out);
      return out;
    };
    Tail t;
    auto s = skips(t, 200);
    Check(Has(s, 40) && Has(s, 41) && Has(s, 42) && Has(s, 91) && Has(s, 94) && !Has(s, 29) && !Has(s, 35) && !Has(s, 81) && !Has(s, 88) && !Has(s, 43) && !Has(s, 90) && !Has(s, 95), "tail.ring_side_variant_0",
          "[stage+21180] == 0: 40-42 and 91-94 skipped, 29-35 / 81-88 drawn");
    t.w21180 = 1;
    s = skips(t, 200);
    Check(Has(s, 29) && Has(s, 35) && Has(s, 81) && Has(s, 88) && !Has(s, 28) && !Has(s, 36) && !Has(s, 80) && !Has(s, 89) && !Has(s, 40) && !Has(s, 42) && !Has(s, 91) && !Has(s, 94), "tail.ring_side_variant_1",
          "[stage+21180] != 0: 29-35 and 81-88 skipped, 40-42 / 91-94 drawn");
    // 240-247: W_i != 0 skips A_i = {243, 245, 247, 241}, W_i == 0 skips B_i = {242, 244, 246, 240}
    t = Tail();
    t.w[0] = 1, t.w[2] = 1;
    s = skips(t, 200);
    Check(Has(s, 243) && Has(s, 244) && Has(s, 247) && Has(s, 240) && !Has(s, 242) && !Has(s, 245) && !Has(s, 246) && !Has(s, 241), "tail.pairs_240_247", "W = {1, 0, 1, 0}: 243, 244, 247, 240 skipped; 242, 245, 246, 241 drawn");
    t.w[0] = 0, t.w[1] = 1, t.w[2] = 0, t.w[3] = 1;
    s = skips(t, 200);
    Check(Has(s, 242) && Has(s, 245) && Has(s, 246) && Has(s, 241) && !Has(s, 243) && !Has(s, 244) && !Has(s, 247) && !Has(s, 240), "tail.pairs_240_247_flipped", "W = {0, 1, 0, 1}: 242, 245, 246, 241 skipped");
    // the hide lists: list c skips its slots only when bit c of [stage+21196] is SET (0x824E7608-0x824E7618: slw / and / addic -1 / subfe = "bit != 0")
    t = Tail();
    t.lists[3] = {131, 132}, t.lists[5] = {133};
    t.always = {134};
    t.bits = 1u << 3;
    s = skips(t, 200);
    Check(Has(s, 131) && Has(s, 132) && !Has(s, 133) && Has(s, 134), "tail.hide_list_needs_bit_set", "bit 3 set: list 3 (131, 132) skipped, list 5 (133, bit clear) drawn, the always list (134) skipped");
    t.bits = 0;
    s = skips(t, 200);
    Check(!Has(s, 131) && !Has(s, 132) && !Has(s, 133) && Has(s, 134), "tail.hide_list_bit_clear", "no bit set: only the always list skips");
    // an ordinary arena never reads the tail: the same words skip nothing extra
    t = Tail();
    t.w21180 = 1, t.bits = 0x3FFF;
    t.lists[0] = {131};
    t.always = {134};
    s = skips(t, 5);
    Check(!Has(s, 29) && !Has(s, 81) && !Has(s, 131) && !Has(s, 134) && !Has(s, 242), "tail.ordinary_arena_untouched", "arena 5: the 200..249 tail is not read and skips nothing");
    {   // sub_824E47C8's binary search: a full list of 30 (no terminator), the ends, a miss between entries
      uint16_t full[30];
      for (int i = 0; i < 30; ++i) full[i] = uint16_t(10 + 3 * i);
      Check(ListHas(full, 10) && ListHas(full, 97) && ListHas(full, 43) && !ListHas(full, 11) && !ListHas(full, 98) && !ListHas(full, 9), "tail.list_search_full", "30 entries, no terminator");
      const uint16_t none[30] = {0xFFFF};
      Check(!ListHas(none, 0) && !ListHas(none, 0xFFFF), "tail.list_search_empty", "an empty list holds nothing");
    }
    // the g-slot exceptions flow into the tail: slot 242 (st22124 != 0, a g slot outside the game look) is hidden by the tail when W_0 == 0 (B_0 = 242)
    {
      World w;
      w.arena = 200, w.phase = 4;   // the entrance: r30 false, r31 true
      w.st22124 = 1;
      Guest g = Make(w);
      for (int i = 0; i < 4; ++i) g.W(kStage + 21200 + 4 * uint32_t(i), 0);   // W = 0: B = {242, 244, 246, 240}
      g.W(kStage + 21180, 0), g.W(kStage + 21196, 0);
      for (int c = 0; c < 14; ++c) put_list(g, kStage + 21216 + 60 * uint32_t(c), {});
      put_list(g, kStage + 22056, {});
      SlotVisWords vw;
      const bool ok = ReadVisWords([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, vw);
      std::vector<uint16_t> out;
      if (ok) BuildVisSkips(g_recs, vw, out);
      Check(ok && Has(out, 242) && !Has(out, 243), "tail.g_exception_reaches_tail", "g slots 242 / 243 pass the exception, then 242 is hidden by W_0 == 0");
    }
    {   // fail closed: a created arena whose tail words are unreadable = known false
      World w;
      w.arena = 200;
      Guest g = Make(w);
      for (uint32_t k = 0; k < 4; ++k) g.b.erase(kStage + 21180 + k);   // the tail words unreadable
      SlotVisWords vw;
      Check(!ReadVisWords([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, vw), "tail.unread_fails_closed", "arena 200 with no tail words = known false");
    }
  }
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
