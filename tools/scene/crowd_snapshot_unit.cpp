// crowd_snapshot_unit: the offline proof of S103 lane D3 (required input bit 10, `crowd_snapshot`; docs\renderer\INPUTS_A3.md). 0 game runs, no SDK, no game memory.
//   crowd_snapshot_unit [--plant NAME]
// It runs the SHIPPED code: crowd_contrib.cpp (TakeInputs / SnapLocked / NoteVerdict) over crowd_snapshot_core.h, with the real scene_inputs::Capture as the stamp. The hooks are driven the way the game's
// are (BeginDraw / NoteMember / EndDraw / SetPalette / SetArena), then Latch + Emit (the latch path) or Emit alone (the no-latch path).
// A plant is a FAULT in the shipped code (RDR_CROWD_SNAP_PLANTS) that must make a NAMED check fail:
//   stale_stamped     a stale (repeated) snapshot is stamped            -> "snap.stale_no_stamp"
//   unreadable_ignored an unreadable member (index unknown) is ignored  -> "snap.unreadable_no_stamp"
//   record_frame_clock the stamp is for the record's frame, not its hook frame -> "snap.hook_clock"
//   mode_off_stamps   Mode Off judges and stamps anyway                 -> "snap.off_costs_nothing"
//   palette_ignored   a member's missing palette is ignored             -> "snap.palette_required"
//   crowd_leads       the crowd takes its own latch's snapshot (one period ahead of the record's camera) -> "period.crowd_takes_previous_latch" (S111 CUTCROWD2)
//   crowd_pairs_off   a mode-Off latch feeds the crowd's pairing (the camera's is fed only with the mode on) -> "period.mode_switch_pairs_with_camera" (S111 CUTCROWD2 review)
#ifndef RDR_CROWD_SNAP_PLANTS
#error "build with -DRDR_CROWD_SNAP_PLANTS (crowd_snapshot_check.py does): the contributor and this test must agree on the class layout"
#endif
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/scene_live/crowd_contrib.h"
#include "../../src/scene_live/inputs_core.h"
#include "../../src/scene_live/crowd_latch_reader.h"

using namespace rdr_scene;
using namespace rdr_scene::live;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& detail = "") {
  if (!ok) {
    ++g_fail;
    std::printf("FAIL %s %s\n", name, detail.c_str());
  } else {
    std::printf("ok   %s\n", name);
  }
}

static bool g_mode_on = true;
static uint64_t g_last_stamp = 0;
static int g_stamps = 0;
static bool Active() { return g_mode_on; }
static void Stamp(uint64_t hook) {
  scene_inputs::Capture(scene_inputs::kInCrowdSnapshot, hook);
  g_last_stamp = hook;
  ++g_stamps;
}

static crowd::MemberObs Member(uint32_t block, uint32_t lod, uint32_t index, bool card = false) {
  crowd::MemberObs m;
  m.block = block, m.lod = card ? crowd::kCardLayer : lod, m.index = index, m.card = card;
  for (int r = 0; r < 3; ++r) m.c[r * 4 + r] = 1.f;
  m.c[3] = 10.f * float(index);
  return m;
}
static void Palette(crowd::Contributor& c, uint32_t type, uint32_t lod) {
  float mats[2 * 16] = {};
  for (int k = 0; k < 2; ++k) mats[k * 16] = mats[k * 16 + 5] = mats[k * 16 + 10] = mats[k * 16 + 15] = 1.f;
  c.SetPalette(0, type, lod, mats, 2);
}
// A crowd draw as the game's hook brackets it.
static void Draw(crowd::Contributor& c, const std::vector<crowd::MemberObs>& ms, uint32_t other = 0) {
  c.BeginDraw(0);
  for (const auto& m : ms) c.NoteMember(m);
  for (uint32_t i = 0; i < other; ++i) c.NoteUnmodelled();
  c.EndDraw();
}
static uint32_t (*g_rd)(crowd::LatchSnapshot&) = nullptr;   // the latch reader every test contributor gets (main sets it: forged manager memory)
static void Fresh(crowd::Contributor& c, int plant) {
  c.Reset();
  c.SetLatchReader(g_rd);
  c.SetArena(5);
  c.SetStampSink({&Active, &Stamp});
  c.SetSnapPolicy({});
  c.snap_plant = plant;
  g_mode_on = true, g_stamps = 0, g_last_stamp = 0;
}
// One record: with the latch (the game threads parked) or without (the no-latch path).
static void Record(crowd::Contributor& c, uint64_t frame, bool latch) {
  live::FrameInfo fi;
  fi.guest_frame = frame;
  SceneTables t;
  live::Counters cnt;
  if (latch) c.Latch(fi);
  c.Emit(fi, t, cnt);
}

#include "crowd_snapshot_latch_part.h"
#include "../../src/scene_live/presence_rules.h"   // S103 S2
#include "crowd_snapshot_s2_part.h"                 // S103 S2: hold by presence
#include "crowd_snapshot_period_part.h"             // S111 CUTCROWD2: record r carries latch r - 1's snapshot

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) plant = argv[i + 1];
  int p = 0, lp = 0;
  if (plant == "stale_stamped") p = 1;
  else if (plant == "unreadable_ignored") p = 2;
  else if (plant == "record_frame_clock") p = 3;
  else if (plant == "mode_off_stamps") p = 4;
  else if (plant == "palette_ignored") p = 5;
  else if (plant == "unobserved_ignored") p = 6;
  else if (plant == "yaw_sign") lp = 1;
  else if (plant == "card_mask") lp = 2;
  else if (plant == "remap_ignored") lp = 3;
  else if (plant == "card_gate_ignored") lp = 4;
  else if (plant == "floor_not_transposed") lp = 5;
  else if (plant == "prop_mul_order") lp = 6;
  else if (plant == "floor_near_cut_ignored") lp = 7;
  else if (plant == "prop_distance_ignored") lp = 11;     // S103 D5
  else if (plant == "prop_eye_composer") scene_inputs::handed::g_plant = 1;   // S110 CUTCROWD: the props' eye from the composer's copy +1488 (not the handed block)
  else if (plant == "prop_texture_ignored") lp = 12;
  else if (plant == "prop_exit_ignored") lp = 13;
  else if (plant == "prop_global_ignored") lp = 14;
  else if (plant == "single_rot_order") lp = 15;
  else if (plant == "single_parity") lp = 16;
  else if (plant == "single_chain_order") lp = 17;   // S103 D6
  else if (plant == "soft_scope_off") lp = 18;   // S103 D8
  else if (plant == "entry_ring_ignored") lp = 19;
  else if (plant == "single_forward_any_class") lp = 20;   // S103 C1 (R15)
  else if (plant == "single_screen_ignored") lp = 21;
  else if (plant == "single_lod_as_type") lp = 22;
  else if (plant == "floor_lod_group_ignored") lp = 23;
  else if (plant == "unread_hole_as_other") lp = 24;
  else if (plant == "prop_frames_unmodelled") p = 14;
  else if (plant == "single_screen_nan_hides") lp = 25;   // S103 C1 review
  else if (plant == "single_entity_unchecked") lp = 26;
  else if (plant == "floor_type_byte_skip") lp = 27;
  else if (plant == "hand_map_ignored") lp = 28;   // S103 SG: every hand draws 41016 (the pre-SG rule)
  else if (plant == "hand_attire_ignored") lp = 29;   // S103 SG: sub_82506E30 not replicated (idx 0 always)
  else if (plant == "single_data_count_skips_block") lp = 30;   // S103 SN: the data object's count read at [y+24] (no +12 indirection)
  else if (plant == "screen_scale_ignored") lp = 31;   // S105 A2: the screen tests ignore the widening scale (the 16:9 ports)
  else if (plant == "screen_hidden_absent") crowd::g_presence_plant = 4;
  else if (plant == "picture_handle_unchecked") rdr_scene::live::guest_tex::g_tex_plant = 1;
  else if (plant == "picture_tiled_ignored") rdr_scene::live::guest_tex::g_tex_plant = 2;
  else if (plant == "picture_endian_ignored") rdr_scene::live::guest_tex::g_tex_plant = 3;
  else if (plant == "picture_packed_ignored") rdr_scene::live::guest_tex::g_tex_plant = 4;   // S108 CAP2: mip 0 of a packed-tail texture read from the tail's corner (its offset ignored)
  else if (plant == "picture_fail_leaks") rdr_scene::live::guest_tex::g_tex_plant = 5;
  else if (plant == "sign_guest_read_despite_file") rdr_scene::live::crowd::g_sign_file_plant = 1;   // S103 CSF: the pre-CSF rule (every sign picture read back from the guest, whatever the file holds)
  else if (plant == "picture_huge_pitch_unbounded") rdr_scene::live::guest_tex::g_tex_plant = 10;   // S103 CSF review A: no padding bound, a bad pitch is read
  else if (plant == "sign_stale_key_slot") rdr_scene::live::crowd::g_sign_file_plant = 2;   // S103 CSF review 1: keys[b] (stale after the re-map)
  else if (plant == "sign_check_skipped") rdr_scene::live::crowd::g_sign_file_plant = 3;   // S103 CSF review 2: the one-time FNV check passes everything
  else if (plant == "picture_wrong_stride") rdr_scene::live::guest_tex::g_tex_plant = 9;   // S103 CSF review C: the padding is not stepped over
  else if (plant == "picture_linear_padded_cap") rdr_scene::live::guest_tex::g_tex_plant = 8;   // S103 CSF: the pre-CSF linear test (padded extent against the payload cap)
  else if (plant == "rows_hidden_ignored") p = 7;
  else if (plant == "rows_kind_swapped") p = 8;
  else if (plant == "walked_props_dropped") p = 15;   // S103 SG2: the pre-SG2 rule: a walked frame's reader props never reach the record
  else if (plant == "end_frame_no_latch") p = 9;   // S103 D7
  else if (plant == "end_frame_only_crowd") p = 10;
  else if (plant == "entry_read_ignored") p = 11;   // S103 D9
  else if (plant == "pal_hist_wrong_ring") p = 12;
  else if (plant == "entry_stale_consumed") p = 13;
  else if (plant == "presence_ignored") crowd::g_presence_plant = 1;   // S103 S2
  else if (plant == "presence_lod_absent") crowd::g_presence_plant = 2;
  else if (plant == "presence_partial_kept") crowd::g_presence_plant = 3;
  else if (plant == "crowd_leads") p = 16;   // S111 CUTCROWD2
  else if (plant == "crowd_pairs_off") p = 17;   // S111 CUTCROWD2 review: a mode-Off latch feeds the crowd's pairing
  else if (!plant.empty()) { std::printf("unknown plant %s\n", plant.c_str()); return 2; }

  crowd::Contributor& c = crowd::The();
  crowd::latch::g_latch_plant = lp;
  ForgeManager(g_forge);
  g_rd = &ForgeRead;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--shipped")) {   // the shipped value of kCrowdOursWhole (a build WITHOUT -DRDR_CROWD_OURS_WHOLE): a whole snapshot is judged whole and its Capture is withheld
      Fresh(c, 0);
      Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
      Draw(c, {Member(0, 0, 0)});
      Record(c, 100, true);
      const bool ok = !crowd::kCrowdOursWhole && g_stamps == 0 && c.SnapCensus().withheld == 1 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::Whole)] == 1 &&
                      scene_inputs::g_counters.stamps[scene_inputs::kInCrowdSnapshot].load() == 0;
      Check(ok, "shipped.capture_withheld", "stamps " + std::to_string(g_stamps));
      // S103 S2: with the presence switch on, a whole latch-built frame with nothing shown stamps although kCrowdOursWhole is false; a shown sign still holds it
      Check(S2Ours(c, 0, 0, true) == 1 && c.PresenceStamped() == 1, "shipped.presence_stamps", "stamps " + std::to_string(g_stamps));
      Check(S2Ours(c, 0, 1, true) == 0, "shipped.presence_holder_holds", "stamps " + std::to_string(g_stamps));
      std::printf(g_fail ? "FAILED\n" : "PASSED\n");
      return g_fail ? 1 : 0;
    }

  // ---- registration facts
  Check(scene_inputs::kInputs[scene_inputs::kInCrowdSnapshot].moved, "snap.moved_flag");
  Check(scene_inputs::kInputs[scene_inputs::kInCrowdSnapshot].lag == 2 && scene_inputs::kInputs[scene_inputs::kInCrowdSnapshot].bit == 10, "snap.bit10_lag2");
  for (uint64_t f : {0ull, 1ull, 2ull, 100ull, 123456ull})
    if (crowd::SnapHookFrame(f) != scene_inputs::HookFrameOfRecord(f)) Check(false, "snap.clock_same_as_inputs", std::to_string(f));
  Check(crowd::SnapHookFrame(100) == 99 && crowd::SnapHookFrame(0) == 0, "snap.clock_values");

  // ---- A: a whole snapshot, latch path: stamped for the hook frame (record 100 -> hook 99)
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  const std::vector<crowd::MemberObs> whole = {Member(0, 0, 0), Member(0, 1, 1), Member(1, 0, 0), Member(1, 3, 2, true)};
  Draw(c, whole);
  {   // review F2: the stamp is made at the LATCH, before any Emit (a deferred Emit would land after the update thread is released)
    live::FrameInfo fi;
    fi.guest_frame = 100;
    c.Latch(fi);
    Check(g_stamps == 1, "snap.stamp_at_latch", "stamps " + std::to_string(g_stamps));
    SceneTables t;
    live::Counters cnt;
    c.Emit(fi, t, cnt);
  }
  Check(g_stamps == 1 && g_last_stamp == 99, "snap.hook_clock", "stamps " + std::to_string(g_stamps) + " last " + std::to_string((unsigned long long)g_last_stamp));
  Check(scene_inputs::g_counters.last[scene_inputs::kInCrowdSnapshot].load() == 99, "snap.capture_reached_inputs");
  Check(c.SnapCensus().stamped == 1 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::Whole)] == 1, "snap.census_whole");

  // ---- B: a record made WITHOUT a latch cannot prove anything (a deferred Emit must not read the guest): no stamp, counted
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  Draw(c, whole);
  Record(c, 101, false);
  Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::NoLatchRead)] == 1, "snap.no_latch_no_stamp", "stamps " + std::to_string(g_stamps));

  // ---- C: a stale snapshot (no new crowd draw): the repeat Emit writes it, but it is NOT a stamp
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  Draw(c, whole);
  Record(c, 100, false);   // (with the latch every active frame is fresh; staleness is the no-latch path's)
  Record(c, 101, false);   // repeat 1
  Record(c, 102, false);   // repeat 2 (the last repeated frame)
  Record(c, 103, false);   // repeat 3: skipped
  Record(c, 104, false);
  Check(g_stamps == 0, "snap.stale_no_stamp", "stamps " + std::to_string(g_stamps));
  Check(c.SnapCensus().why[uint32_t(crowd::SnapWhy::Stale)] == 4, "snap.stale_counted", std::to_string((unsigned long long)c.SnapCensus().why[uint32_t(crowd::SnapWhy::Stale)]));

  // ---- D: an unreadable member (its record pointer fitted no array): no stamp, counted
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  std::vector<crowd::MemberObs> bad = whole;
  bad[2].index = crowd::kUnknown;
  Draw(c, bad);
  Record(c, 100, true);
  Check(g_stamps == 0, "snap.unreadable_no_stamp", "stamps " + std::to_string(g_stamps));
  Check(c.SnapCensus().why[uint32_t(crowd::SnapWhy::UnreadableIndex)] == 1 || p == 2, "snap.unreadable_counted");

  // ---- E: a missing palette: no stamp; a card / floor person needs only its type's LOD 0 palette
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0);   // no (type 0, LOD 1)
  Draw(c, whole);
  Record(c, 100, true);
  Check(g_stamps == 0, "snap.palette_required", "stamps " + std::to_string(g_stamps));
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0);
  Draw(c, {Member(0, 0, 0), Member(1, 3, 1, true)});   // LOD 0 only + a card: whole without any LOD 1 palette
  Record(c, 100, true);
  Check(g_stamps == 1, "snap.card_uses_lod0_palette");

  // ---- F: a person of a population the contributor does not record (signs / foam hands): not whole, unless the policy says so
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  Draw(c, whole, 3);
  Record(c, 100, true);
  Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::Unmodelled)] == 1, "snap.unmodelled_blocks");
  Fresh(c, p);
  crowd::SnapPolicy loose;
  loose.unmodelled_blocks = false;
  c.SetSnapPolicy(loose);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  Draw(c, whole, 3);
  Record(c, 100, true);
  Check(g_stamps == 1, "snap.unmodelled_policy_off");

  // ---- G: no arena / no members / a non-finite world / out-of-range block
  Fresh(c, p);
  c.SetArena(-1);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  Draw(c, whole);
  Record(c, 100, true);
  Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::NoArena)] == 1, "snap.no_arena");
  Fresh(c, p);
  Draw(c, {});
  Record(c, 100, true);
  Check(g_stamps == 1, "snap.empty_is_whole", "stamps " + std::to_string(g_stamps));   // review F5: a walked draw with no member and nothing unread
  Fresh(c, p);
  c.SetArena(-1);
  Record(c, 100, true);   // the first active latch: no history, no stamp
  Record(c, 102, true);   // no walk, no skip: the crowd entry was not reached (a menu): empty and whole, with no arena (S111 CUTCROWD2: 2 ahead, so the record takes THIS latch's snapshot, not 100's)
  Check(g_stamps == 1 && c.LatchCensus().absent == 1, "snap.absent_crowd_is_whole");
  Fresh(c, p);
  Draw(c, {}, 2);
  Record(c, 100, true);
  Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::Unmodelled)] == 1, "snap.empty_with_unmodelled");
  // review F1: floor people the observer is off for (the default) are on screen and not read: not whole
  Fresh(c, p);
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  c.BeginDraw(0);
  for (const auto& m : whole) c.NoteMember(m);
  c.NoteUnobserved();
  c.EndDraw();
  Record(c, 100, true);
  Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::Unmodelled)] == 1, "snap.floor_unobserved_blocks", "stamps " + std::to_string(g_stamps));
  Fresh(c, p);
  Palette(c, 0, 0);
  std::vector<crowd::MemberObs> nan = {Member(0, 0, 0)};
  nan[0].c[3] = std::nanf("");
  Draw(c, nan);
  Record(c, 100, true);
  Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::BadWorld)] == 1, "snap.bad_world");
  Fresh(c, p);
  Draw(c, {Member(20, 0, 0)});
  Record(c, 100, true);
  Check(g_stamps == 0 && c.SnapCensus().why[uint32_t(crowd::SnapWhy::BadRange)] == 1, "snap.bad_range");

  // ---- H: Mode Off costs nothing: nothing judged, nothing counted, nothing stamped
  Fresh(c, p);
  g_mode_on = false;
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  Draw(c, whole);
  Record(c, 100, true);
  Draw(c, whole);
  Record(c, 101, false);
  Check(g_stamps == 0 && c.SnapCensus().judged == 0, "snap.off_costs_nothing", "stamps " + std::to_string(g_stamps) + " judged " + std::to_string((unsigned long long)c.SnapCensus().judged));

  // ---- I: no sink (the offline tools): nothing happens
  Fresh(c, p);
  c.SetStampSink({});
  Palette(c, 0, 0), Palette(c, 1, 0), Palette(c, 0, 1);
  Draw(c, whole);
  Record(c, 100, true);
  Check(g_stamps == 0 && c.SnapCensus().judged == 0, "snap.no_sink");

  LatchTests(c, p);   // S103 D3b
  S2PresenceTests(c, p);   // S103 S2
  PeriodTests(c, p);   // S111 CUTCROWD2

  // ---- J: the census line
  Fresh(c, p);
  std::string line;
  c.Census(line);
  Check(line.find("crowd_snapshot withheld 0 stamped 0/0") != std::string::npos && line.find("no_palette") != std::string::npos, "snap.census_line", line);

  std::printf(g_fail ? "FAILED %d check(s)\n" : "PASSED\n", g_fail);
  return g_fail ? 1 : 0;
}
