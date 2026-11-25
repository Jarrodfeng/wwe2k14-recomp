// body-scale: none (checks the inputs / stamps, builds no wrestler picture)  [S103 RGF: body_scale_feed_lint.py]
// inputs_unit: the offline proof of S103 lane A's input capture (docs\renderer\INPUTS_BOARD.md). 0 game runs, no SDK, no game memory.
//   inputs_unit [--plant NAME] [--write-record PATH]
// It runs the SHIPPED code: the stamping (src\scene_live\inputs_core.h over scene_frame.h / node_switch.h), the post settings reader and its derived constants (post_settings.h), the latch
// contributor (inputs_contrib.h) against a fake guest, and the record file code (record_io: the three new sections, a write -> read -> write round trip, an older reader that skips them).
// THE ORACLE for the settings is the layout lane PR read from the listings (post_effects.md: S + byte offsets, the c20 / c21 formulas), built here by hand from numbers the "game" wrote, never
// from the reader's own tables. THE ORACLE for the decision is the contract (FRAME_CONTRACT.md 3): a stamp for j - 1 satisfies a lag-1 input and not a lag-0 one; a missing input keeps InputsMissing.
// A plant is a FAULT in the thing under test (the fake guest or the reference the check compares with); each must make a NAMED check fail (exit code 1 when any check failed):
//   lag0_is_lag1     the decision accepts the previous frame's stamp for a lag-0 input                -> "stamp.lag0_rejects_old"
//   stamp_next       the capture is stamped for frame j + 1                                            -> "stamp.same_frame"
//   skip_unreadable  an unreadable S still stamps the input                                            -> "post.unreadable_no_stamp"
//   word_order       the fake guest hands S over without the big-endian order                          -> "post.words"
//   alpha_off        MdlAlpha lives at [model+248] in the fake guest (the reader reads +252)           -> "model.alpha"
//   drop_missing     the decision ignores a missing input                                              -> "decide.missing_input"
//   lose_section     the round trip loses the model rows                                               -> "record.roundtrip"
//   cam_slot1        the main camera sits in slot 1 of the real 2-slot table, the shadow camera in slot 0 -> "camera.eye"
//   deferred_read    a deferred (unlatched) Emit is allowed to read guest memory                        -> "emit.deferred_no_read"
//   sentinel0        a never-stamped input starts at 0 (a lag-1 input then passes at frame 1)           -> "stamp.fresh_not_satisfied"
//   mode_ignored     the latch ignores --wwe2k14_rdr_scene = 0                                           -> "mode.off_reads_nothing"
//   pal_decoy        [skeleton+200] holds the decoy palette's address (the reader must follow +200)     -> "palette.values"
//   pal_stamp_bad    an unreadable palette still stamps kInWrestlerPalette                               -> "palette.unreadable_no_stamp"
//   dead_refeed      the latch reader ignores the update hook's liveness (a wrestler who left is fed forever) -> "wrestler.dead_leaves"
//   pal_nocount_stamps a kept skeleton with no count still lets the palette input stamp               -> "palette.nocount_no_stamp"
//   param_stride16   the fake guest lays the override entries out 16 bytes apart (the game: 20)       -> "params.entries"
//   param_read_slot0 the fake guest puts the stale double-buffer slot where the reader reads (word 0), the live value in the source pointer (word 1) -> "params.live_source"
//   pass_mode_off    the fake guest keeps the char pass mode at [model+612] / [+624] (the reader reads +616 / +620)    -> "model.pass_mode"
//   param_size_count the fake guest claims 64 bytes for a 16-byte value (an over-read of the controller record)   -> "params.live_source"
//   param_part_swap  the fake guest swaps the per-part tables of parts 1 and 2                         -> "params.parts"
//   plane_swap       the fake guest swaps the camera and fit blocks of the plane module                  -> "plane.matrices"
//   plane_dup        the walk's block is appended next to the latch's instead of replacing it (the old code)   -> "plane.no_duplicate"
//   plane_no_gate    the reader ignores [module+40] (a disabled module is read as current)               -> "plane.stale_refused"
//   param_name_hash  the reader treats entry word 2 as the name itself, not a pointer to it                  -> "params.names"
//   param_models_list the contributor reads the models list, not the live targets (a dead model's tables are read) -> "params.liveness_list"
//   param_half_null  a half-null part table (tables null, counts set) is skipped as "no tables"             -> "params.half_null"
//   a3_no_prelatch   the wrestlers contributor does not run the palette read on the record path with no latch   -> "a3.palette_no_latch"
//   a3_slot_off      the fake stage keeps its slot models 4 bytes further on (the reader: stage + 6164)       -> "a3.arena_models"
//   a3_stamp_lag     the arena input is stamped from the inputs side, one frame behind the stage take                  -> "a3.arena_same_frame"
//   a3_cache_off     the fake character's component cache [char+3112] is empty and the list holds a decoy    -> "a3.circle_cache"
//   a3_ground_flag  the fake guest swaps the two in-ring ground constants (sub_826088A8 picks +4 when the flag is 0)  -> "a3.ground_ring"
//   a3_vp_plain     the fake guest stores module+2240 already transposed (the upload transposes it)           -> "a3.circle_module"
//   a3_stage_same    the arena does not change while the old stage object is freed (the guard cannot fire)               -> "a3.stage_changed_no_stamp"
//   a3_node_off     the fake module has [module+22696] == 0 (the render entry's off test) while rows are expected  -> "a3.circle_rows"
//   a3_mesh_always  the mesh-known test always says yes (rows without a mesh stamp bit 8)                               -> "a3.circle_no_mesh_no_stamp"
//   a3_proxy_swap   the board keeps the proxy words in guest byte order (the copy is not swapped)                      -> "a3.proxy_words"
//   a3_proxy_stamp_early the plane-constants input is stamped without the node's copy for the frame                      -> "a3.proxy_stamp_needs_copy"
//   a3_light_slot   the install is recorded on another thread slot than the model's (slots mixed)                       -> "a3.light_slot_isolation"
//   a3_light_nostamp_empty the light input is stamped with an empty learned table                                       -> "a3.light_empty_no_stamp"
//   a3_light_consumer_ignored the light input is stamped although no consumer reads the model map                          -> "a3.light_no_consumer_no_stamp"
//   a4_future_fresh the future-dated entry is dated at the hook frame                                                  -> "a4.light_future_stale"
//   a4_flag_ignored the stamp switch is on for the flag-off stamp test                                                  -> "a4.light_flag_off_no_stamp"
//   a4_moved_hardcoded bit 4 reports moved while the switch is off                                                      -> "a4.light_moved_follows_flag"
//   a4_hole_filled  the model the frame draws without an entry is given one                                            -> "a4.light_hole_not_ok"
//   a4_stale_fresh  the stale entry is confirmed at the hook frame instead of long before it                            -> "a4.light_stale_refused"
//   a5_ever_ignored the hole classification ignores the table's memory                                                -> "a5.hole_lapsed_vs_never"
//   a5_ask_all the unemitted slot model is asked too                                                                    -> "a5.only_emitted_asked"
//   a5_emitted_hole_dropped the emitted model without an entry is dropped from the asked set                            -> "a5.emitted_without_entry_is_hole"
//   a5_offscreen_asked the off-screen model is asked too                                                               -> "a5.offscreen_not_asked"
//   a5_default_filled a model drawn with no install is given one                                                       -> "a5.no_install_stays_hole"
//   a3_light_no_hold the planned refresh hold is not set before the oldest entry expires                                -> "a3.light_planned_hold"
//   a3_no_derived_head the update hook reported no derived palette (nothing to compare: the mismatch cannot be seen)       -> "a3.palette_derived_mismatch_withholds"
//   a3_piece_unowned  the piece is not linked to its owner body (so it falls back to "any body alive")                   -> "a3.piece_owner_dead_not_fed"
//   a3_walked_as_ours the stored decision of the record's hook frame is Ours while the walk recorded nothing             -> "a3.circle_latch_only_frames"
//   a3_light_direct_ignored the model is attached under another slot than the one whose direct install it follows       -> "a3.light_direct_install_object"
//   a3_default_vec   the default vector the game copies for a zero matrix address is unreadable in the fake guest         -> "a3.circle_default_vec"
//   light_off44      the light objects' block starts at object+44 in the fake guest (reader: +48)        -> "lights.block_offset"
//   light_remap_id   the set id word is not masked to 16 bits in the reference                           -> "lights.remap_ids"
//   light_lose_sec   the round trip loses the light rows                                                 -> "lights.roundtrip"
//   b4x_long_life    S103 B4X: a once-"proven" entry outlives kMaxAge (no long life any more)                                   -> "b4x.no_long_life_for_any_entry"
//   b4x_noid_walk_learned S103 B4X: a slot with no l(N) is walk-learned only (the old rule): a culled one lapses                -> "b4x.culled_no_id_slot_never_lapses"
//   b4x_f4_not_asked  S103 B4X F4: a kept skeleton with no character slot is NOT asked under synthesis (it has an entry here)  -> "b4x.f4_no_slot_skeleton_asked_with_synthesis"
//   b4x_f1_le_read    S103 B4X F1: an le( token is read like l(                                                                  -> "b4x.f1_le_lg_unread"
//   b4x_f2_default_replaces S103 B4X F2: a default-100 slot replaces a fresh learned entry                                      -> "b4x.f2_default_never_replaces_learned"
//   b4x_f3_no_row_none S103 B4X F3: a slot < 399 with no row has no default                                                    -> "b4x.f3_no_row_default"
//   lh_old_hold      S103 LH: a default-100 slot whose learned entry equals the computed one is NOT re-dated (the old rule: it ages, plans a refresh walk) -> "lh.default_equal_entry_plans_no_hold"
//   b4x_f5_oldest_all S103 B4X F5: an entry nobody asked about gates the refresh plan                                           -> "b4x.f5_unasked_entry_plans_no_hold"
//   l33_strict_derived S103 L33F: the old strict derived guard (every mismatch withholds bit 3)                  -> "l33.mismatch_walk_equal_accepted"
//   unrequire_ignored S103 L33F: --wwe2k14_rdr_scene_unrequire applies nothing                                    -> "unrequire.drops_named_bits_only"
//   l33_no_evidence  S103 L33F review F3: an OURS-frame mismatch is submitted without the walk evidence for that body  -> "l33.mismatch_ours_no_evidence_refused"
//   handed_ignored   S110 CUTCROWD: the scene camera is always the composer's copy +1488 / +1552 (the old read)          -> "handed.gated_takes_block"
//   handed_any_block S110 CUTCROWD: any non-zero engine block is taken (not only the main manager's two)               -> "handed.other_block_live"
//   handed_mirror_taken S110 CUTCROWD: a mirrored block (negative determinant) is taken                                 -> "handed.mirror_live"
//   handed_fov_derived S110 CUTCROWD: the FOV is always derived from the block's radians (never the live word)          -> "handed.equal_is_live"
//   handed_invalid_taken S110 CUTCROWD: a block that fails the record's tests is taken (no camera instead of the live)   -> "handed.invalid_block_live"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_arena_world.h"
#include "../../src/renderer_scene/rows_circle.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/renderer_scene/rows_plane.h"
#include "../../src/renderer_scene/rows_quad2d.h"
#include "../../src/scene_live/inputs_contrib.h"
#include "../../src/scene_live/draw_camera_core.h"   // S103 WC R5
#include "../../src/scene_live/hold_switch.h"   // S103 L1C: the run-time hold
#include "../../src/scene_live/arena_world_contrib.h"   // S103 A3: the arena clock under OURS
#include "../../src/scene_live/stage_fx_contrib.h"      // S103 A3: the stage object's slot models (the arena object list)
#include "../../src/scene_live/circle_contrib.h"      // S103 A3 (S6): the walk's circle contributor (the merge)
#include "../../src/scene_live/plane_proxy.h"        // S103 A3 (S8): the proxy list copy
#include "../../src/scene_live/light_index_core.h"   // S103 A3 (bit 4)
#include "../../src/scene_live/slot_light_token.h"   // S103 B4X fixes: the slot text's l / le / lg tokens and the loader default
#include "../../src/renderer_scene/live_maps_e2.h"   // S103 PB: BuildE2Input over a no-draw plane frame
#include "../../src/scene_live/plane_contrib.h"   // S103 A2: the walk's plane contributor (the emit order and the merge)
#include "../../src/scene_live/wrestler_contrib.h"   // S103 A2: the REAL wrestlers contributor (liveness, mirror compare)
#include "../../src/scene_live/model_release.h"   // S108 RDS: what the sub_82BD3B40 hook runs

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;

static_assert(kScPostParams == 29 && kScModelRuntime == 30 && kScInputStamps == 31 && kScCount >= 32, "ids 29-31 are lane A's (FRAME_CONTRACT.md 8)");
static_assert(sizeof(PostParamsRow) == 760 && sizeof(ModelRuntimeRow) == 32 && sizeof(InputStampRow) == 16, "the layouts the record carries");

// bit-8 review 2: each address constant of circle_points.h derived from the (lis, displacement) pair of the instruction that loads it (generated listings, read this session), independent of the header's own value.
// lis rD,HI loads HI << 16 (HI is signed in the listing); the displacement is signed.
constexpr uint32_t Lis(int32_t hi, int32_t disp) { return uint32_t((int64_t(hi) << 16) + disp); }
static_assert(scene_inputs::kGhZero == Lis(-32231, -18228) && scene_inputs::kGhOne == Lis(-32231, -18240), "sub_82812600 0x82812620 / sub_82612700 0x826127E4: lis -32231, lfs -18228 / -18240");
static_assert(scene_inputs::kGhRingHalf == Lis(-32254, -13600), "sub_82812600 0x8281264C: lis -32254, lfs -13600");
static_assert(scene_inputs::kGhC0 == Lis(-32254, -10912) && scene_inputs::kGhC1 == Lis(-32255, 11664) && scene_inputs::kGhC2 == Lis(-32256, 20312), "sub_82612700 0x82612738-0x82612744");
static_assert(scene_inputs::kGhC3 == Lis(-32254, 3332) && scene_inputs::kGhC4 == Lis(-32254, 3328) && scene_inputs::kGhC5 == Lis(-32254, -12532), "sub_82612700 0x82612754-0x82612768");
static_assert(scene_inputs::kGhC6 == Lis(-32254, -12960), "sub_82612700 0x826127A0: lis -32254, lfs -12960");
static_assert(scene_inputs::kGhRingFlag == Lis(-31882, 31984) && scene_inputs::kGhRingTable == Lis(-31902, 9328), "sub_826088A8 0x826088A8-0x826088B4");
static_assert(scene_inputs::kGhGateObj == Lis(-31882, 13636), "sub_824E6FC8 0x824E6FC8-0x824E6FCC");
static_assert(scene_inputs::kGhModeObj == Lis(-31881, -21772), "sub_827D7578 0x827D7578-0x827D757C");
static_assert(scene_inputs::kGhTabPtr == Lis(-31889, 11316) && scene_inputs::kGhTabCount == Lis(-31889, 11320), "sub_822C3440 0x822C3464-0x822C3474");
static_assert(scene_inputs::kCircleDefaultVec == Lis(-32249, -3376), "sub_82812E90 0x82812F90-0x82812F98");
static_assert(scene_inputs::kHandleTab1 == Lis(-31853, -25960) && scene_inputs::kHandleTab2 == Lis(-31853, -5740), "sub_82756710 0x82756710-0x82756718 / sub_82756780 0x82756780-0x82756788");
static_assert(scene_inputs::kShadowModulePtr == Lis(-31854, 18248), "sub_822A7608 0x822A7618 lis r31,-31854 ; 0x822A761C lwz r3,18248(r31)");

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- the fake guest: big-endian bytes at guest addresses
struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;   // guest word address (4-aligned) -> value (host order)
  bool readable = true;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutF(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); words[a] = b; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    if (!readable) return false;
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      uint32_t be = scene_inputs::Swap32(it->second);   // the guest is big-endian
      if (Plant("word_order") && n > 4) be = it->second;   // the planted guest forgets the order for the S block
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
};
static Guest g_guest;
static size_t submitted_union = 0;
static std::vector<uint32_t> g_raw;   // the palette reader's scratch
static bool ReadThunk(uint32_t a, void* d, size_t n) { return g_guest.Read(a, d, n); }

static constexpr uint32_t kS = 0x40001000;                     // where the fake S lives
static constexpr uint32_t kM0 = 0x40002000, kM1 = 0x40002400;  // two fake models

static void BuildCameras();
static void BuildGuest() {
  g_guest = Guest{};
  g_guest.Put(post_settings::kGuestPointer, kS);
  for (uint32_t i = 0; i < kPostSettingsWords; ++i) g_guest.Put(kS + 4 * i, 0);
  // The "game" writes S as post_effects.md describes: flags are words, the DoF block floats.
  g_guest.Put(kS + 0, 1);                 // post on
  g_guest.Put(kS + 4, 1);                 // DoF on
  g_guest.PutF(kS + 8, 0.9980f);          // S8  (c20.z)
  g_guest.PutF(kS + 12, 0.5000f);         // S12
  g_guest.PutF(kS + 16, 0.9990f);         // S16
  g_guest.PutF(kS + 20, 0.8000f);         // S20 (c20.w, the far cap)
  g_guest.PutF(kS + 24, 0.7500f);         // S24 (c21.x)
  g_guest.PutF(kS + 28, 0.9950f);         // S28 (c21.y)
  g_guest.PutF(kS + 32, 0.0050f);         // S32 (c21.z)
  g_guest.Put(kS + 176, 1);               // bloom on
  g_guest.PutF(kS + 188, 0.30f);          // c31
  g_guest.PutF(kS + 192, 0.45f);          // c32
  g_guest.PutF(kS + 200, 1.25f);          // star angle
  g_guest.Put(kS + 656, 1);               // fog on
  g_guest.PutF(kS + 676, 0.10f);
  g_guest.PutF(kS + 680, 0.20f);
  g_guest.PutF(kS + 684, 0.30f);
  // MdlAlpha lives at [model+252] (sub_82C13C88 0x82C13D7C: lfs f1,252(r30)); the word next door (+248) is a decoy the reader must not take. The planted guest swaps the two.
  const uint32_t at = Plant("alpha_off") ? 248 : 252, decoy = Plant("alpha_off") ? 252 : 248;
  g_guest.PutF(kM0 + at, 1.0f);
  g_guest.PutF(kM1 + at, 0.375f);
  g_guest.PutF(kM0 + decoy, 7.0f);
  g_guest.PutF(kM1 + decoy, 7.0f);
  // WRK (WRK.md 2d): the char pass mode = [model+616] (0 / 2) and [model+620]; kM0 = (2, 0), kM1 = (0x41, 1): an insane word = no flag. The planted guest shifts both by 4 bytes.
  const uint32_t pm = Plant("pass_mode_off") ? 612 : 616, pm2 = Plant("pass_mode_off") ? 624 : 620;
  g_guest.Put(kM0 + pm, 2), g_guest.Put(kM0 + pm2, 0), g_guest.Put(kM1 + pm, 0x41), g_guest.Put(kM1 + pm2, 1);
  BuildCameras();
}
// The engine's current-camera table (shadows.md 2): slots 0-2 hold the main camera A, slot 3 holds a shadow light's camera B (transient). A at (100, -50, 300) looking by the rotation below.
static constexpr uint32_t kCamA = 0x40003000, kCamB = 0x40004000;
static void PutCam(uint32_t obj, const float view[16], float zn, float zf) {
  for (int i = 0; i < 16; ++i) g_guest.PutF(obj + 240 + 4 * i, view[i]);   // the game layout: view +240 (sub_829352D8 +EC), proj +176 (sub_82BB3748 +13C)
  for (int i = 0; i < 16; ++i) g_guest.PutF(obj + 176 + 4 * i, (i == 5 ? 2.0f : i == 0 ? 1.125f : i == 10 ? 1.0001f : i == 11 ? 1.0f : i == 14 ? -0.2f : 0.f));
  g_guest.PutF(obj + 500, zn);
  g_guest.PutF(obj + 504, zf);
}
static void BuildCameras() {
  // row-vector world -> view: p_view = p * M3 + t; M3 = a 90 degree turn about Y; t = -(eye * M3) = (300, 50, -100) for the eye (100, -50, 300)
  const float va[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 300, 50, -100, 1};
  const float vb[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -5, -6, -7, 1};
  PutCam(kCamA, va, 10.f, 20000.f);
  PutCam(kCamB, vb, 1.f, 500.f);
  // the REAL table (shadows.md 2, caster_core.h kProductSlots): TWO pointers at 0x83929A40 (slots 0, 1), then the override words at +8, +12. The scene camera is slot 0; slot 1 holds a shadow light's camera.
  const bool slot1 = Plant("cam_slot1");
  g_guest.Put(scene_inputs::kCamTable + 0, slot1 ? kCamB : kCamA);
  g_guest.Put(scene_inputs::kCamTable + 4, slot1 ? kCamA : kCamB);
  g_guest.Put(scene_inputs::kCamTable + 8, 1);    // override words: never read
  g_guest.Put(scene_inputs::kCamTable + 12, 1);
  // S103 A2 (merge with lane S): the scene camera the latch reads is the composer's copy ([0x836F3AA8] +1488 view, +1552 FOV degrees): the same main camera A.
  constexpr uint32_t kComposer = 0x40007000;
  g_guest.Put(scene_inputs::kCamMgrPtr, kComposer);
  for (int i = 0; i < 16; ++i) g_guest.PutF(kComposer + scene_inputs::kCamMgrView + 4 * i, va[i]);
  g_guest.PutF(kComposer + scene_inputs::kCamMgrFov, 60.f);
}
static void ModelsThunk(std::vector<uint32_t>& out) { out = {kM0, kM1}; }

static bool Near(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps * (1.f + std::fabs(b)); }

static void ResetStamps() {
  for (int b = 0; b < 32; ++b) rdr_own::S(rdr_own::Pass::Scene3D).input_frame[b].store(0xFFFFFFFFFFFF0000ull);   // "never stamped" (frame 0 is a legal frame)
  for (int b = 0; b < scene_inputs::kInputCount; ++b) {
    scene_inputs::g_counters.stamps[b].store(0);
    scene_inputs::g_counters.last[b].store(0);
  }
}
// The decision the plants corrupt: `drop_missing` ignores a missing input, `lag0_is_lag1` accepts the previous frame for every input.
static rdr_own::OwnedPassStamp Decide(uint64_t frame) {
  auto st = scene_frame::DecideNow(frame);
  if (Plant("lag0_is_lag1")) {   // the faulty rule: every registered input is judged with lag 1
    rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
    uint32_t have = 0;
    const uint32_t req = s.required_inputs.load();
    for (int b = 0; b < 32; ++b)
      if (((req >> b) & 1u) && (s.input_frame[b].load() == frame || s.input_frame[b].load() == frame - 1)) have |= 1u << b;
    st.inputs_mask = have;
  }
  if (Plant("drop_missing")) st.why_not = uint8_t(rdr_own::WhyNot::None), st.decision = uint8_t(rdr_own::Decision::Ours);
  return st;
}
static int g_mode = 2;

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
  std::string write_record;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
    else if (!std::strcmp(argv[i], "--write-record") && i + 1 < argc) write_record = argv[++i];
  }
  scene_frame::g_mode_cvar.store(&g_mode);
  scene_frame::SetReady(true);
  ResetStamps();
  BuildGuest();

  // ---------------------------------------------------------------- 1. the stamp and the decision
  scene_inputs::RegisterAll();
  scene_frame::RequireInputLag(20, 0);   // a synthetic lag-0 input (every real latch input is lag 2 since S103 CLK2; sphere_occ is lag 1)
  {   // the FRESH state (the session branch's ~0 start): nothing stamped satisfies any input at frame 1 (a 0 start would satisfy every lag-1 input: lane A review 6)
    if (Plant("sentinel0")) for (int b = 0; b < 32; ++b) rdr_own::S(rdr_own::Pass::Scene3D).input_frame[b].store(0);
    const uint32_t have = scene_frame::InputsMask(1);
    Check(have == 0, "stamp.fresh_not_satisfied", "mask 0x" + std::to_string(have));
    // the sphere bit is not required until the sphere hook registers
    scene_inputs::RequireBit(scene_inputs::kInSphereOcc);
    Check(rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load() & (1u << scene_inputs::kInSphereOcc), "stamp.sphere_bit_on_registration");
    rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.fetch_and(~(1u << scene_inputs::kInSphereOcc));
  }
  const uint32_t req = rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load();
  Check(req == ((scene_inputs::AllMask() & ~(1u << scene_inputs::kInSphereOcc)) | (1u << 20)), "stamp.registered", "required mask 0x" + std::to_string(req));
  {   // S103 SMX (HOL2 LP): the census does not count plane_constants as missing where the game built no plane module; every other bit still counts (the planted twin = the old census)
    const uint32_t m = (1u << scene_inputs::kInPlaneConstants) | (1u << scene_inputs::kInCamera);
    static_assert(scene_frame::kCensusPlaneConstantsBit == scene_inputs::kInPlaneConstants, "the census bit is plane_constants");
    const bool twin = Plant("census_plane_bit");
    Check(scene_frame::CensusMissing(m, true, twin) == (1u << scene_inputs::kInCamera) && scene_frame::CensusMissing(m, false, twin) == m && scene_frame::CensusMissing(0, true, twin) == 0, "census.plane_bit_dropped_only_for_no_module", "0x" + std::to_string(scene_frame::CensusMissing(m, true, twin)));
  }
  {   // S103 SMX (HOL2 IP): the stage light-map page row (T_Sprite 0x82BC5688 into a 512x512 offscreen page) is a page row while our own page is on; anything else offscreen stays a pass drop (the twin = the old census)
    rdr_scene::Quad2D r;
    r.pass = rdr_scene::kQpOffscreen, r.site = 0x82BC5688u, r.have = rdr_scene::kQhTarget, r.target_size[0] = 512, r.target_size[1] = 512;
    const bool twin = Plant("page_row_pass");
    rdr_scene::Quad2D o = r;
    o.target_size[0] = 128;   // the replay-thumbnail downscale: not the page
    rdr_scene::Quad2D s = r;
    s.site = 0x82BC3EE8u;
    rdr_scene::Quad2D nt = r;
    nt.have = 0;              // no target read: unknown size, never a page row
    Check(rdr_scene::IsStagePageRow(r, true, true, twin) && !rdr_scene::IsStagePageRow(r, false, true, twin) && !rdr_scene::IsStagePageRow(r, true, false, twin) && !rdr_scene::IsStagePageRow(o, true, true, twin) && !rdr_scene::IsStagePageRow(s, true, true, twin) && !rdr_scene::IsStagePageRow(nt, true, true, twin), "census.stage_page_row_class", "the 512x512 T_Sprite offscreen row only, own page on");
  }
  const uint64_t j = 100;
  // every real input captured for j - 1, the synthetic one for j - 1 too: the decision for j accepts the lag-1 inputs only
  for (int b = 0; b < scene_inputs::kInputCount; ++b) scene_inputs::Capture(b, j - 1);
  rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 20, j - 1);
  {
    const uint32_t have = Decide(j).inputs_mask;
    Check((have & scene_inputs::AllMask()) == (scene_inputs::AllMask() & ~(1u << scene_inputs::kInSphereOcc)), "stamp.lag1_accepts_old", "mask 0x" + std::to_string(have));
    Check(!(have & (1u << 20)), "stamp.lag0_rejects_old", "mask 0x" + std::to_string(have));
  }
  // a capture for frame j satisfies the lag-0 input
  rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 20, j);
  {
    const auto st = Decide(j);
    Check(st.inputs_mask == req && st.decision == uint8_t(rdr_own::Decision::Ours), "stamp.all_present_is_ours", "mask 0x" + std::to_string(st.inputs_mask));
  }
  // the stamp of the capture is the frame it was made for (never the next one)
  {
    ResetStamps();
    scene_inputs::Capture(scene_inputs::kInPostSettings, Plant("stamp_next") ? j + 1 : j);
    const uint64_t got = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInPostSettings].load();
    Check(got == j, "stamp.same_frame", "stamped " + std::to_string(got) + " for " + std::to_string(j));
  }
  // a missing input keeps the decision at InputsMissing (planted twin: drop_missing)
  {
    ResetStamps();
    for (int b = 0; b < scene_inputs::kInputCount; ++b)
      if (b != scene_inputs::kInLightStates) scene_inputs::Capture(b, j - 1);   // everything but one (the latch inputs are stamped at the end of the frame before)
    rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 20, j);
    const auto st = Decide(j);
    Check(st.decision == uint8_t(rdr_own::Decision::GameWalks) && st.why_not == uint8_t(rdr_own::WhyNot::InputsMissing), "decide.missing_input",
          "decision " + std::to_string(st.decision) + " why " + std::to_string(st.why_not));
    // frame j + 1 with the stamps of j: lag 1 inputs pass, the lag-0 one does not (j + 1 wants j + 1)
    const auto st2 = Decide(j + 1);
    Check(!(st2.inputs_mask & (1u << 20)), "decide.old_stamp_not_current", "mask 0x" + std::to_string(st2.inputs_mask));
  }
  // every input has a registered lag and a board id, and the bits are unique
  {
    uint32_t seen = 0;
    bool ok = true;
    for (int b = 0; b < scene_inputs::kInputCount; ++b) {
      ok = ok && scene_inputs::kInputs[b].bit == b && scene_inputs::kInputs[b].name && scene_inputs::kInputs[b].board && !(seen & (1u << b));
      seen |= 1u << b;
    }
    Check(ok && scene_inputs::kInputCount <= 32, "stamp.table", "inputs " + std::to_string(int(scene_inputs::kInputCount)));
  }

  // ---------------------------------------------------------------- 2. the post settings, read from the fake guest
  ResetStamps();
  PostParamsRow post = scene_inputs::ReadPostSettings(ReadThunk, 7);
  {
    bool words = post.flags & kPpRead && post.settings_addr == kS && post.frame == 7;
    for (uint32_t i = 0; i < kPostSettingsWords; ++i) {
      uint32_t want = 0;
      auto it = g_guest.words.find(kS + 4 * i);
      if (it != g_guest.words.end()) want = it->second;
      if (post.s[i] != want) words = false;
    }
    Check(words, "post.words", "flags 0x" + std::to_string(post.flags) + " addr 0x" + std::to_string(post.settings_addr));
  }
  {
    const uint32_t want = kPpRead | kPpOn | kPpDof | kPpBloom | kPpFog;
    Check(post.flags == want, "post.flags", "got 0x" + std::to_string(post.flags) + " want 0x" + std::to_string(want));
  }
  {
    // the combine's constants by hand from the numbers the "game" wrote (post_effects.md 1): c20 = (1/(S24-S12), 1/(S16-S28), S8, S20), c21 = (S24, S28, S32, 0)
    const auto c = post_settings::CombineFrom(post);
    const float c20x = 1.f / (0.7500f - 0.5000f), c20y = 1.f / (0.9990f - 0.9950f);
    const bool ok = Near(c.c20[0], c20x) && Near(c.c20[1], c20y) && Near(c.c20[2], 0.9980f) && Near(c.c20[3], 0.8000f) && Near(c.c21[0], 0.75f) && Near(c.c21[1], 0.995f) &&
                    Near(c.c21[2], 0.005f) && c.c21[3] == 0.f && Near(c.c27[0], 0.10f) && Near(c.c27[1], 0.20f) && Near(c.c27[2], 0.30f) && c.c27[3] == 1.f && Near(c.c31, 0.30f) && Near(c.c32, 0.45f);
    Check(ok, "post.combine", "c20 " + std::to_string(c.c20[0]) + " " + std::to_string(c.c20[1]) + " " + std::to_string(c.c20[2]) + " " + std::to_string(c.c20[3]));
    Check(Near(post_settings::Float(post, post_settings::kStarAngle), 1.25f), "post.star_angle");
  }
  {
    // near blur off: S24 = S12 = 0 gives c20.x = inf, as the game's divide does (post_effects.md 1)
    PostParamsRow off = post;
    off.s[24 / 4] = 0, off.s[12 / 4] = 0;
    const auto c = post_settings::CombineFrom(off);
    Check(std::isinf(c.c20[0]), "post.near_blur_off_inf");
  }
  // an unreadable S: no stamp (the decision stays at InputsMissing), a row that says so (planted: skip_unreadable)
  {
    ResetStamps();
    scene_inputs::ReadFn bad = [](uint32_t, void*, size_t) { return false; };
    live::inputs::InputsContributor c;
    c.SetReader(bad);
    c.SetModels(ModelsThunk);
    live::inputs::InputsContributor::Frame f;
    c.Take(50, f);
    if (Plant("skip_unreadable")) scene_inputs::Capture(scene_inputs::kInPostSettings, 50);
    const uint64_t got = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInPostSettings].load();
    Check(!(f.post.flags & kPpRead) && got != 50 && f.models.empty() && f.models_unreadable == 2, "post.unreadable_no_stamp", "stamp " + std::to_string(got) + " models_unreadable " + std::to_string(f.models_unreadable));
  }

  // ---------------------------------------------------------------- 3. the contributor at a latch, then Emit
  ResetStamps();
  live::inputs::InputsContributor con;
  con.SetReader(ReadThunk);
  con.SetModels(ModelsThunk);
  FrameInfo fi;
  fi.guest_frame = 200;
  const bool latched = con.Latch(fi);
  Check(latched, "contrib.latch");
  Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInPostSettings].load() == 199 && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInModelRuntime].load() == 199,
        "contrib.stamps_at_latch");
  SceneTables t;
  Counters cnt;
  con.Emit(fi, t, cnt);
  {   // S110 ENTRBLACK (entry: InputsContributor::Latch / Emit): the record's scene-fact header bits. No stage word in this guest: nothing read, no bits (never black on a failed read)
    Check((t.header.flags & rdr_scene::kSceneFactMask) == 0, "entrblack.scene_fact_unread_no_bits");
    const Guest keep = g_guest;
    g_guest.Put(0x83763544u, 0x40004000u), g_guest.Put(0x40004000u + 1176, 0);   // a stage, hidden
    for (uint32_t i = 0; i < 9; ++i) g_guest.Put(0x836F38D0u + 4 * i, 0);         // no character
    live::inputs::InputsContributor c2;
    c2.SetReader(ReadThunk);
    c2.SetModels(ModelsThunk);
    FrameInfo f2;
    f2.guest_frame = 300;
    c2.Latch(f2);
    SceneTables t2;
    Counters cn2;
    c2.Emit(f2, t2, cn2);
    Check(rdr_scene::GameDrawsNoScene(t2.header.flags), "entrblack.scene_fact_hidden_written", "flags " + std::to_string(t2.header.flags));
    {   // review 5: the fallback take (game threads running) leaves the bits clear
      live::inputs::InputsContributor::Frame ff;
      c2.Take(300, ff, false);
      Check(!ff.scene_fact_read && !ff.scene_drawn, "entrblack.scene_fact_fallback_take_clear");
    }
    g_guest.Put(0x40004000u + 1176, 1);   // the stage shown
    c2.Latch(f2);
    SceneTables t3;
    c2.Emit(f2, t3, cn2);
    Check(!rdr_scene::GameDrawsNoScene(t3.header.flags) && (t3.header.flags & rdr_scene::kSceneDrawn), "entrblack.scene_fact_shown_written", "flags " + std::to_string(t3.header.flags));
    g_guest = keep;
  }
  {
    const bool ok = t.post_params.size() == 1 && t.model_runtime.size() == 2 && t.model_runtime[0].object == kM0 && t.model_runtime[1].object == kM1 && t.model_runtime[0].frame == 200;
    Check(ok, "contrib.rows", "post " + std::to_string(t.post_params.size()) + " models " + std::to_string(t.model_runtime.size()));
    Check(t.model_runtime.size() == 2 && Near(t.model_runtime[0].alpha, 1.0f) && Near(t.model_runtime[1].alpha, 0.375f) && (t.model_runtime[1].flags & kMrAlphaRead), "model.alpha",
          t.model_runtime.size() == 2 ? ("alpha " + std::to_string(t.model_runtime[0].alpha) + " " + std::to_string(t.model_runtime[1].alpha)) : "no rows");
    Check(t.model_runtime.size() == 2 && (t.model_runtime[0].flags & kMrPassModeRead) && (t.model_runtime[0].reserved & 0xFF) == 2 && ((t.model_runtime[0].reserved >> 8) & 0xFF) == 0 && !(t.model_runtime[1].flags & kMrPassModeRead) && t.model_runtime[1].reserved == 0 && (t.model_runtime[1].flags & kMrAlphaRead),
          "model.pass_mode", "m616 2 / m620 0 with the flag; 0x41 = no flag, alpha kept");
    {   // WRK review R5: one insane word, or one unreadable word, drops the flag for BOTH (a half-read mode is never carried); the row keeps MdlAlpha
      const Guest keep_pm = g_guest;
      ModelRuntimeRow rr;
      g_guest.Put(kM0 + 620, 0x41);   // +616 fine (2), +620 insane
      const bool a = scene_inputs::ReadModelRuntime(ReadThunk, kM0, 5, rr, 3) && !(rr.flags & kMrPassModeRead) && rr.reserved == 0 && (rr.flags & kMrAlphaRead);
      g_guest = keep_pm;
      g_guest.words.erase(kM0 + 620);   // +616 readable, +620 unreadable
      const bool b = scene_inputs::ReadModelRuntime(ReadThunk, kM0, 5, rr, 3) && !(rr.flags & kMrPassModeRead) && rr.reserved == 0 && (rr.flags & kMrAlphaRead);
      g_guest = keep_pm;
      g_guest.Put(kM0 + 372, 2), g_guest.Put(kM0 + 480, 0x40090000);   // WRK_ADV 5b: the part table, 24 bytes per part, word 5 = E+20; part 1 has it set
      for (uint32_t w = 0; w < 12; ++w) g_guest.Put(0x40090000 + 4 * w, 0);
      g_guest.Put(0x40090000 + 24 + 20, 1);
      const bool c = scene_inputs::ReadModelRuntime(ReadThunk, kM0, 5, rr, 3) && (rr.flags & kMrPassModeRead) && ((rr.reserved >> 16) & 0xFF) == 1 && ((rr.reserved >> 24) & 0xFF) == 1 && (rr.reserved & 0xFFFF) == 2;
      g_guest.Put(0x40090000 + 24 + 20, 0);
      const bool d = scene_inputs::ReadModelRuntime(ReadThunk, kM0, 5, rr, 3) && ((rr.reserved >> 16) & 0xFF) == 0 && ((rr.reserved >> 24) & 0xFF) == 1;
      g_guest = keep_pm;
      const bool e = scene_inputs::ReadModelRuntime(ReadThunk, kM0, 5, rr, 0) && !(rr.flags & kMrPassModeRead) && rr.reserved == 0 && (rr.flags & kMrAlphaRead);   // review F5: an arena model / the switch off: MdlAlpha only
      const bool f2 = scene_inputs::ReadModelRuntime(ReadThunk, kM0, 5, rr, 1) && (rr.flags & kMrPassModeRead) && ((rr.reserved >> 24) & 0xFF) == 0;   // bit 0 alone: no part-table read
      Check(a && b && c && d && e && f2, "model.pass_mode_partial", "insane +620 / unreadable +620: no flag, alpha kept");
    }
    {   // S108 RDS part A (entry: InputsContributor::Take, the production order: models_ / targets resolved, then ReadModelRuntime per model). A model the game released stays in the KEPT set (up to 120 Emits) and its block
        // was reused: +252 a plausible float, +372 = 1, +480 = 0x41700001 (the float 15.000001, the run's refused address). The runtime rows come from the latch's targets: the released model is never read.
      const Guest keep_rel = g_guest;
      constexpr uint32_t kRel = 0x40070000;
      g_guest.PutF(kRel + 252, 0.25f), g_guest.Put(kRel + 372, 1), g_guest.Put(kRel + 480, 0x41700001u), g_guest.Put(kRel + 616, 0), g_guest.Put(kRel + 620, 0);
      static uint32_t rel_reads = 0;
      const auto run = [&](bool with_targets, std::vector<uint32_t>& objs) {
        live::inputs::InputsContributor rc;
        rc.SetReader([](uint32_t a, void* d, size_t n) { if (uint64_t(a) + n > kRel && a < kRel + 1024) ++rel_reads; if (a >= 0x41700000u && a < 0x41701000u) ++rel_reads; return g_guest.Read(a, d, n); });
        rc.SetModels([](std::vector<uint32_t>& out) { out = {kM0, kRel}; });   // the kept set: the live body and the released one
        if (with_targets) rc.SetPalettes([](uint64_t, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) { out = {{kM0, 3}}; missing = 0; }, [](uint32_t, uint32_t, const Mat4*, uint64_t) { return 0; });
        rel_reads = 0;
        live::inputs::InputsContributor::Frame fr;
        rc.Take(800, fr, true);
        objs.clear();
        for (const ModelRuntimeRow& r : fr.models) objs.push_back(r.object);
      };
      std::vector<uint32_t> objs;
      run(true, objs);
      Check(objs == std::vector<uint32_t>{kM0} && rel_reads == 0, "rds.released_model_not_read", "rows " + std::to_string(objs.size()) + ", reads of the released block / the wild address " + std::to_string(rel_reads));
      run(false, objs);   // planted twin: the kept set (the old list) reads the released block and makes it a row
      Check(objs.size() == 2 && rel_reads > 0, "rds.twin_kept_set_reads_released", "rows " + std::to_string(objs.size()) + ", reads " + std::to_string(rel_reads));
      {   // RDS review: the game's own branch and order: the character-table targets (SetSlotPalettes), LatchPalettes builds THIS hook's targets, then Take reuses them (ResolveModelList's pal_targets_ branch)
        live::inputs::InputsContributor sc;
        static uint32_t unscoped = 0;   // S108 RDS phase 2: reads made outside a pass scope (PassLines) by LatchPalettes / Take
        sc.SetReader([](uint32_t a, void* d, size_t n) { if (!wwe2k14::safe_pages::PassLines::Current()) ++unscoped; if (uint64_t(a) + n > kRel && a < kRel + 1024) ++rel_reads; if (a >= 0x41700000u && a < 0x41701000u) ++rel_reads; return g_guest.Read(a, d, n); });
        sc.SetModels([](std::vector<uint32_t>& out) { out = {kM0, kRel}; });
        sc.SetSlotPalettes([](uint64_t, uint64_t, const live::inputs::InputsContributor::SlotTable&, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing, uint32_t* stale_visible, bool) {
                             missing = 0;
                             if (stale_visible) *stale_visible = 0;
                             out = {{kM0, 3}};
                           },
                           [](uint32_t, uint32_t, const Mat4*, uint64_t) { return 0; });
        const int keep_mode = g_mode;
        g_mode = 2;
        sc.LatchPalettes(800);
        rel_reads = 0;
        live::inputs::InputsContributor::Frame fr;
        sc.Take(800, fr, true);
        g_mode = keep_mode;
        objs.clear();
        for (const ModelRuntimeRow& r : fr.models) objs.push_back(r.object);
        Check(objs == std::vector<uint32_t>{kM0} && rel_reads == 0, "rds.slot_path_released_model_not_read", "rows " + std::to_string(objs.size()) + ", reads of the released block / the wild address " + std::to_string(rel_reads));
        Check(unscoped == 0 && !wwe2k14::safe_pages::PassLines::Current(), "rds.inputs_pass_scoped", "reads outside a pass scope " + std::to_string(unscoped) + "; the scope outlived the pass: " + (wwe2k14::safe_pages::PassLines::Current() ? "YES" : "no"));
      }
      g_guest = keep_rel;
    }
    {   // S108 RDS class closure: the production function the sub_82BD3B40 hook runs (model_release.h OnModelDestroyed, over the process singletons Wrestlers() / lightidx::The()): a model destroyed between two
        // latches is named by no container afterwards, so the next latch reads nothing of it. (The REX_HOOK_RAW itself needs the SDK: compiled by game_syntax, not here.) Twins: each erase left out in turn.
      constexpr uint32_t kGone = 0x40500000;
      auto& wc = live::Wrestlers();
      std::vector<Mat4> mats(3);
      const auto plant = [&] {
        wc.SubmitWorld(kGone, -1, 3, mats.data(), mats.data(), 10);
        live::lightidx::The().OnLatchEntry(kGone, 0x40600000, 0, 0, 10);
        live::inputs::Inputs().SetSnapForTest({{0x40510000u, 3}, {kGone, 3}}, 10);   // RDS review: the last latch's target copy the off-latch fallback reads
        return wc.NamesModel(kGone) && live::lightidx::The().Has(kGone) && live::inputs::Inputs().SnapNames(kGone);
      };
      const auto gone = [&] {
        std::vector<uint32_t> kept;
        wc.KeptSkeletons(kept);
        return !wc.NamesModel(kGone) && !live::lightidx::The().Has(kGone) && !live::inputs::Inputs().SnapNames(kGone) && std::find(kept.begin(), kept.end(), kGone) == kept.end();
      };
      const bool before = plant();
      wwe2k14::model_release::OnModelDestroyed(kGone);   // the release, between latch 10 and latch 11
      const bool after = gone();
      Check(before && after, "rds.release_forgets_address", std::string("named before ") + (before ? "yes" : "no (the plant did not register)") + ", after " + (after ? "none" : "STILL NAMED"));
      const bool tw_ok = plant() && (wwe2k14::model_release::OnModelDestroyed<true, false>(kGone), !gone()) && plant() && (wwe2k14::model_release::OnModelDestroyed<false, true>(kGone), !gone()) &&
                         plant() && (wwe2k14::model_release::OnModelDestroyed<false, false, true>(kGone), !gone());   // RDS review: + the snapshot's erase left out
      wwe2k14::model_release::OnModelDestroyed(kGone);   // leave the singletons clean
      const bool other_kept = live::inputs::Inputs().SnapNames(0x40510000u);   // only the destroyed address leaves the copy
      live::inputs::Inputs().SetSnapForTest({}, ~0ull);
      Check(tw_ok && gone() && other_kept, "rds.release_twins_keep_address", std::string("twins (the wrestlers' erase / the light erase left out) ") + (tw_ok ? "still name the address: caught" : "FORGOT IT ANYWAY (the check cannot see a missing erase)"));
    }
    {   // S108 RDS phase 1: the per-object copy reads the hull once and keeps the per-field verdicts; at a mapped page end the exact hull reads, the one-byte-wider twin is refused. Page-granular fake (an unset word reads 0).
      static std::set<uint32_t> pages_ok;
      static uint32_t calls = 0;
      const scene_inputs::ReadFn pread = [](uint32_t a, void* d, size_t n) {
        ++calls;
        for (uint64_t p = a >> 12; p <= (uint64_t(a) + n - 1) >> 12; ++p) if (!pages_ok.count(uint32_t(p))) return false;
        for (size_t i = 0; i < n; i += 4) if (!g_guest.Read(a + uint32_t(i), static_cast<uint8_t*>(d) + i, 4)) std::memset(static_cast<uint8_t*>(d) + i, 0, 4);
        return true;
      };
      const Guest keep_pe = g_guest;
      const uint32_t m = 0x40101000u - scene_inputs::kModelRuntimeEnd;   // the hull [m+252, m+624) ends exactly at the end of page 0x40100
      pages_ok = {0x40100u, 0x40200u};
      g_guest.PutF(m + 252, 0.5f), g_guest.Put(m + 372, 1), g_guest.Put(m + 480, 0x40200000u), g_guest.Put(m + 616, 2), g_guest.Put(m + 620, 1), g_guest.Put(0x40200000u + 20, 1);
      ModelRuntimeRow rr;
      calls = 0;
      const bool exact = scene_inputs::ReadModelRuntime(pread, m, 5, rr, 3) && (rr.flags & kMrPassModeRead) && Near(rr.alpha, 0.5f) && ((rr.reserved >> 16) & 0xFF) == 1 && ((rr.reserved >> 24) & 0xFF) == 1;
      const uint32_t exact_calls = calls;
      const bool wide = scene_inputs::ReadModelRuntimeT<true>(pread, m, 5, rr, 3);   // planted twin: one byte past the hull, no per-field fallback
      pages_ok = {0x40200u};   // the model's page unmapped: the copy is refused and the per-field reads refuse too (no row), exactly as before
      const bool gone = !scene_inputs::ReadModelRuntime(pread, m, 5, rr, 3);
      Check(exact && exact_calls == 2 && !wide && gone, "rds.copy_hull_page_end", std::string("exact ") + (exact ? "row" : "none") + " in " + std::to_string(exact_calls) + " reads (was 5 + the table), wide twin " + (wide ? "row (WRONG)" : "refused") + ", unmapped " + (gone ? "none" : "row"));
      g_guest = keep_pe;
    }
    Check(!t.input_stamps.empty(), "contrib.stamp_rows", std::to_string(t.input_stamps.size()) + " rows");
    bool sorted = true, has_post = false;
    for (const auto& r : t.input_stamps) {
      if (r.bit == scene_inputs::kInPostSettings) has_post = r.frame == 199 && r.lag == 2 && r.moved == 1 && r.clock == scene_inputs::kClockHook;   // the HOOK clock: record 200 carries hook frame 199
      if (r.bit >= 32) sorted = false;
    }
    Check(has_post && sorted, "contrib.stamp_row_post");
  }
  {   // a second Emit without a latch for its frame reads now (not latched) and still produces the rows
    SceneTables t2;
    Counters c2;
    FrameInfo f2;
    f2.guest_frame = 201;
    con.Emit(f2, t2, c2);
    Check(t2.post_params.size() == 1 && t2.post_params[0].frame == 201, "contrib.unlatched_reads_now");
  }
  {   // a DEFERRED emit with no latch for its frame must not read guest memory (lane A review 5): it counts stale and writes nothing
    SceneTables t3;
    Counters c3;
    FrameInfo f3;
    f3.guest_frame = 202;
    live::tail_detail::t_deferred_emit = !Plant("deferred_read");
    con.Emit(f3, t3, c3);
    live::tail_detail::t_deferred_emit = false;
    Check(t3.post_params.empty() && t3.model_runtime.empty() && !t3.has_camera && t3.input_stamps.empty() && c3.stale == 1, "emit.deferred_no_read", "stale " + std::to_string(c3.stale));
  }
  {   // the mode gate (review 1): --wwe2k14_rdr_scene = 0 reads nothing and writes nothing, latched or not
    g_mode = Plant("mode_ignored") ? 2 : 0;
    live::inputs::InputsContributor off;
    off.SetReader(ReadThunk);
    off.SetModels(ModelsThunk);
    ResetStamps();
    FrameInfo f4;
    f4.guest_frame = 400;
    const bool latched4 = off.Latch(f4);
    SceneTables t4;
    Counters c4;
    off.Emit(f4, t4, c4);
    FrameInfo f5;
    f5.guest_frame = 401;   // and an unlatched Emit
    off.Emit(f5, t4, c4);
    const bool stamped = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInPostSettings].load() == 399;
    Check(latched4 && t4.post_params.empty() && t4.model_runtime.empty() && !t4.has_camera && t4.input_stamps.empty() && !stamped, "mode.off_reads_nothing");
    g_mode = 2;
  }

  // ---------------------------------------------------------------- 3a. W1 (S103 A2): the skin palette at [skeleton+200], read at the latch and handed to the wrestlers contributor as a consumer palette
  {
    constexpr uint32_t kPal = 0x40005000, kPalDecoy = 0x40006000, kCount = 3;
    // the "game" writes count x 64 bytes of floats: matrix i, element j = 100 * i + j + 0.25 (a hand-made pattern, never the reader's own table)
    for (uint32_t i = 0; i < kCount; ++i)
      for (uint32_t j = 0; j < 16; ++j) {
        g_guest.PutF(kPal + 64 * i + 4 * j, 100.f * i + j + 0.25f);
        g_guest.PutF(kPalDecoy + 64 * i + 4 * j, -1.f);
      }
    g_guest.Put(kM0 + scene_inputs::kPaletteOff, Plant("pal_decoy") ? kPalDecoy : kPal);
    g_guest.Put(kM0 + 196, kPalDecoy);   // the word next door
    std::vector<Mat4> pal;
    const bool ok = scene_inputs::ReadPalette(ReadThunk, kM0, kCount, pal, g_raw);
    bool vals = ok && pal.size() == kCount;
    for (uint32_t i = 0; vals && i < kCount; ++i)
      for (uint32_t j = 0; j < 16; ++j)
        if (!Near(pal[i].m[j], 100.f * i + j + 0.25f)) vals = false;
    Check(vals, "palette.values", ok ? ("m[1][3] " + std::to_string(pal.size() > 1 ? pal[1].m[3] : 0.f)) : "unreadable");
    Check(!scene_inputs::ReadPalette(ReadThunk, kM1, kCount, pal, g_raw) && pal.empty(), "palette.null_ptr", "kM1 has no +200 pointer");
    Check(!scene_inputs::ReadPalette(ReadThunk, kM0, 0, pal, g_raw) && !scene_inputs::ReadPalette(ReadThunk, kM0, scene_inputs::kPaletteMaxNodes + 1, pal, g_raw), "palette.count_bounds");
    {   // a block that runs off the end of what the game wrote, and a NaN word
      Check(!scene_inputs::ReadPalette(ReadThunk, kM0, kCount + 1, pal, g_raw), "palette.short_block");
      g_guest.PutF(kPal + 8, std::nanf(""));
      Check(!scene_inputs::ReadPalette(ReadThunk, kM0, kCount, pal, g_raw) && pal.empty(), "palette.nan_rejected");
      g_guest.PutF(kPal + 8, 2.25f);
    }
    // the latch hook: submits each target, stamps in the hook clock; an unreadable one, or a kept skeleton with no count, does not stamp
    static std::vector<std::pair<uint32_t, uint64_t>> submitted;
    static std::vector<float> last_values;
    static uint32_t g_missing = 0;
    submitted.clear(), last_values.clear();
    ResetStamps();
    live::inputs::InputsContributor pc;
    pc.SetReader(ReadThunk);
    pc.SetPalettes([](uint64_t, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) { out = {{kM0, kCount}}; missing = g_missing; },
                   [](uint32_t m, uint32_t n, const Mat4* skin, uint64_t f) { submitted.push_back({m, f}); last_values.push_back(n ? skin[n - 1].m[15] : -9.f); return 0; });
    g_missing = 0;
    pc.LatchPalettes(300);
    const uint64_t got = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInWrestlerPalette].load();
    Check(submitted.size() == 1 && submitted[0].first == kM0 && submitted[0].second == 299 && Near(last_values[0], 215.25f) && got == 299, "palette.latch_submits",
          "submits " + std::to_string(submitted.size()) + " stamp " + std::to_string(got));
    ResetStamps();
    g_missing = 1;   // a kept skeleton the wrestlers contributor has no count for (own-skeleton piece without a wanted count): read the rest, but no stamp (review 3)
    submitted.clear();
    pc.LatchPalettes(303);
    if (Plant("pal_nocount_stamps")) scene_inputs::Capture(scene_inputs::kInWrestlerPalette, 302);
    Check(submitted.size() == 1 && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInWrestlerPalette].load() != 302, "palette.nocount_no_stamp");
    g_missing = 0;
    ResetStamps();
    g_guest.words.erase(kPal + 64 * 2 + 4 * 15);   // one word of the last matrix unreadable
    submitted.clear();
    pc.LatchPalettes(301);
    if (Plant("pal_stamp_bad")) scene_inputs::Capture(scene_inputs::kInWrestlerPalette, 300);
    const uint64_t got2 = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInWrestlerPalette].load();
    Check(submitted.empty() && got2 != 300, "palette.unreadable_no_stamp", "submits " + std::to_string(submitted.size()) + " stamp " + std::to_string(got2));
    g_mode = 0;
    submitted.clear();
    ResetStamps();
    pc.LatchPalettes(302);
    Check(submitted.empty() && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInWrestlerPalette].load() != 301, "palette.off_reads_nothing");
    g_mode = 2;
    BuildGuest();   // restore the fake guest for the checks below
  }

  // ---------------------------------------------------------------- 3a1. W1 with the REAL wrestlers contributor: liveness (review 1), the mirror compare (review 2), the union with the pending set (review 4)
  {
    constexpr uint32_t kPal = 0x40005000, kCount = 3, kSkel = kM0;
    for (uint32_t i = 0; i < kCount; ++i)
      for (uint32_t j = 0; j < 16; ++j) g_guest.PutF(kPal + 64 * i + 4 * j, 100.f * i + j + 0.25f);
    g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
    static live::WrestlerContributor* W;
    static bool plant_dead;
    plant_dead = Plant("dead_refeed");
    live::WrestlerContributor w;
    W = &w;
    ResetStamps();
    live::inputs::InputsContributor pc;
    pc.SetReader(ReadThunk);
    pc.SetPalettes([](uint64_t hook, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) {
                     std::vector<live::WrestlerContributor::PaletteTarget> t;
                     W->LatchTargets(plant_dead ? 0 : hook, t, missing);   // the planted fault: no liveness (hook 0 makes every skeleton look alive)
                     out.clear();
                     for (const auto& x : t) out.push_back({x.skel, x.count});
                   },
                   [](uint32_t m, uint32_t n, const Mat4* skin, uint64_t f) { return W->SubmitLatchConsumer(m, n, skin, f); });
    w.SetPreLatch([](uint64_t rf, bool) { (void)rf; });   // (the unit drives the pre-latch itself, below)
    std::vector<Mat4> derived(kCount);
    for (size_t di = 0; di < derived.size(); ++di) for (int dj = 0; dj < 16; ++dj) derived[di].m[dj] = 100.f * float(di) + float(dj) + 0.25f;   // = the guest block: nodes 0-58 agree (audit item 2)
    auto frame = [&](uint64_t rec, bool update_hook, bool walk) {
      const uint64_t hook = rec - 1;
      if (update_hook) w.SubmitSkin(kSkel, 0, kCount, derived.data(), hook);
      if (walk) w.SubmitConsumer(kSkel, kCount, derived.data(), hook);
      pc.LatchPalettes(rec);
      FrameInfo fi;
      fi.guest_frame = rec;
      w.Latch(fi);
      SceneTables t;
      Counters c;
      w.Emit(fi, t, c);
      return t.objects.size();
    };
    size_t n = 0;
    for (uint64_t r = 100; r < 110; ++r) n = frame(r, true, false);
    Check(n == 1 && w.ConsumerSubmits() >= 9, "wrestler.live_emits", "objects " + std::to_string(n) + " consumer submits " + std::to_string((unsigned long long)w.ConsumerSubmits()));
    // the update hook stops (the wrestler left) while the latch keeps running: the skeleton must leave after kPruneAfter (+ the liveness window)
    size_t last = 1;
    uint64_t gone_at = 0;
    for (uint64_t r = 110; r < 110 + 2 * live::WrestlerContributor::kPruneAfter + 20; ++r) {
      last = frame(r, false, false);
      if (!last && !gone_at) gone_at = r;
    }
    Check(last == 0 && gone_at != 0 && gone_at <= 110 + live::WrestlerContributor::kPruneAfter + 12, "wrestler.dead_leaves", "gone at " + std::to_string((unsigned long long)gone_at) + " objects " + std::to_string(last));
    // mirror mode: the walk's own copy of the frame is kept, ours is compared (counted equal / differs), never overwritten
    live::WrestlerContributor w2;
    W = &w2;
    for (uint64_t r = 300; r < 303; ++r) w2.SubmitSkin(kSkel, 0, kCount, derived.data(), r - 1);
    std::vector<Mat4> walk(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
      for (uint32_t j = 0; j < 16; ++j) walk[i].m[j] = 100.f * i + j + 0.25f;   // the same bytes the fake guest holds
    w2.SubmitConsumer(kSkel, kCount, walk.data(), 302 + live::WrestlerContributor::kPaletteLead);   // S103 W1: the walk copy of hook 302 content is tagged hook + lead
    pc.LatchPalettes(303);   // hook frame 302: the walk's copy is there and equal
    const bool same_ok = w2.LatchSame() == 1 && w2.LatchDiffers() == 0;
    walk[1].m[5] += 1.f;
    w2.SubmitConsumer(kSkel, kCount, walk.data(), 303 + live::WrestlerContributor::kPaletteLead);   // a new frame whose walk copy differs from the guest block
    pc.LatchPalettes(304);
    const bool diff_ok = w2.LatchDiffers() == 1;
    // and the kept palette is the WALK's (the differing one), not ours
    FrameInfo fi2;
    fi2.guest_frame = 304 + live::WrestlerContributor::kPaletteLead;   // S103 W1: pairs with stream 305, takes tag <= 304
    w2.Latch(fi2);
    SceneTables t2;
    Counters c2;
    w2.Emit(fi2, t2, c2);
    bool kept_walk = false;
    for (const auto& b : t2.bones) if (Near(b.m[5], 106.25f)) kept_walk = true;   // the walk's copy (105.25 + 1), not the guest block's 105.25
    Check(same_ok && diff_ok, "wrestler.mirror_compare", "same " + std::to_string((unsigned long long)w2.LatchSame()) + " differs " + std::to_string((unsigned long long)w2.LatchDiffers()));
    Check(kept_walk, "wrestler.mirror_keeps_walk_copy", "bones " + std::to_string(t2.bones.size()));
    // a skeleton new this frame (only in cur_, never emitted) is read too (union with the pending set)
    live::WrestlerContributor w3;
    W = &w3;
    w3.SubmitSkin(kSkel, 0, kCount, derived.data(), 399);
    submitted_union = 0;
    pc.SetPalettes([](uint64_t hook, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) {
                     std::vector<live::WrestlerContributor::PaletteTarget> t;
                     W->LatchTargets(hook, t, missing);
                     out.clear();
                     for (const auto& x : t) out.push_back({x.skel, x.count});
                     submitted_union = t.size();
                   },
                   [](uint32_t m, uint32_t n, const Mat4* skin, uint64_t f) { return W->SubmitLatchConsumer(m, n, skin, f); });
    pc.LatchPalettes(400);
    Check(submitted_union == 1, "wrestler.pending_is_read", "targets " + std::to_string(submitted_union));
    BuildGuest();
  }

  // ---------------------------------------------------------------- S103 L31B: the latch's targets from the CHARACTER TABLE (character [0x836F38D0 + 4 slot], skeleton [char + 48]), and the stale guard
  // The run-30 case: a body whose update hook (exit of sub_82BD30E8) goes quiet while the game keeps it in its table and keeps changing [skel+200]. New path: still read every latch, the record carries the
  // CURRENT palette. Planted twin `l31b_hook_targets` = the old hook-liveness targets: the body is dropped from the latch (and, with the guard, from the record) while the stamp says the frame is complete.
  {
    constexpr uint32_t kPal = 0x40005000, kCount = 3, kSkel = kM0, kCh = 0x40300000, kPiece = 0x40301000, kPiecePal = 0x40302000;
    auto put_pal = [&](uint32_t pal, float v) {
      for (uint32_t i = 0; i < kCount; ++i)
        for (uint32_t j = 0; j < 16; ++j) g_guest.PutF(pal + 64 * i + 4 * j, v + float(i) * 0.01f + float(j) * 0.0001f);
    };
    static live::WrestlerContributor* W;
    static uint64_t reads;
    static bool old_path;
    old_path = Plant("l31b_hook_targets");
    auto setup = [&](live::WrestlerContributor& w, live::inputs::InputsContributor& pc) {
      W = &w;
      reads = 0;
      pc.SetReader(ReadThunk);
      if (old_path) {
        pc.SetPalettes([](uint64_t hook, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) {
                         std::vector<live::WrestlerContributor::PaletteTarget> t;
                         W->LatchTargets(hook, t, missing);
                         out.clear();
                         for (const auto& x : t) out.push_back({x.skel, x.count});
                       },
                       [](uint32_t m, uint32_t n, const Mat4* skin, uint64_t f) { ++reads; return W->SubmitLatchConsumer(m, n, skin, f); });
      } else {   // the game's wiring (inputs_glue.h)
        pc.SetSlotPalettes([](uint64_t hook, uint64_t record, const live::inputs::InputsContributor::SlotTable& slots, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing,
                              uint32_t* stale_visible, bool count) {
                             live::WrestlerContributor::SlotTable s;
                             for (const auto& x : slots.skels) s.skels.push_back({x.skel, x.slot, x.shown_addr, x.game_shown});
                             for (int i = 0; i < 9; ++i) s.state[i] = slots.state[i];
                             std::vector<live::WrestlerContributor::PaletteTarget> t;
                             W->LatchTargets(hook, &s, t, missing, count);
                             if (stale_visible) *stale_visible = W->StaleVisibleAt(record, &s);
                             out.clear();
                             for (const auto& x : t) out.push_back({x.skel, x.count});
                           },
                           [](uint32_t m, uint32_t n, const Mat4* skin, uint64_t f) { ++reads; return W->SubmitLatchConsumer(m, n, skin, f); });
      }
      w.SetPreLatch([](uint64_t, bool) {});   // the unit drives the pre-latch itself
      if (Plant("l31b_guard_off")) w.SetStaleGuardForTest(false);
      w.SetFaultsForTest((Plant("l31b_pointer_presence") ? live::WrestlerContributor::kFaultPointerPresence : 0u) | (Plant("l31b_drop_invisible") ? live::WrestlerContributor::kFaultDropInvisible : 0u) |
                         (Plant("l31b_no_walk_gate") ? live::WrestlerContributor::kFaultNoWalkGate : 0u) | (Plant("l31b_keep_gone") ? live::WrestlerContributor::kFaultKeepGoneTargets : 0u) |
                         (Plant("cam_upd_shows") ? live::WrestlerContributor::kFaultUpdShows : 0u));   // S107 CAM twin: the old stream-free rule (a posed, un-asked body shows)
    };
    struct Out { size_t objects = 0; uint32_t stale = 0; float bone0 = -1.f; uint64_t stamp = 0; uint64_t reads = 0; bool visible = false; size_t invisible = 0; };
    // walk = the game's draw read the body this frame (the consumer hook sub_82C04DF0: SubmitConsumer). In mode Ours the walk does not run; review C shows a hook-quiet body only while a walk saw it lately.
    auto frame = [&](live::WrestlerContributor& w, live::inputs::InputsContributor& pc, uint64_t rec, bool update_hook, bool walk = false, bool draw_ask = false) {
      const uint64_t hook = rec - 1;
      if (draw_ask) w.NoteDrawAsked(kSkel, hook, [](uint32_t a, uint32_t& v) { auto it = g_guest.words.find(a); if (!g_guest.readable || it == g_guest.words.end()) return false; v = it->second; return true; });   // S103 DRL: the game's draw ask (sub_82C04DF0 raw hook)
      put_pal(kPal, float(rec));   // the game keeps posing the body: [skel+200] holds this frame's palette
      std::vector<Mat4> d(kCount);
      for (uint32_t i = 0; i < kCount; ++i)
        for (uint32_t j = 0; j < 16; ++j) d[i].m[j] = float(rec) + float(i) * 0.01f + float(j) * 0.0001f;
      if (update_hook) w.SubmitSkin(kSkel, 0, kCount, d.data(), hook);
      if (walk) {   // the walk's copy tagged t holds the content the latch read tagged t holds (the latch of record t read frame t's block): value t = rec - 1
        std::vector<Mat4> wk(kCount);
        for (uint32_t i = 0; i < kCount; ++i)
          for (uint32_t j = 0; j < 16; ++j) wk[i].m[j] = float(hook) + float(i) * 0.01f + float(j) * 0.0001f;
        w.SubmitConsumer(kSkel, kCount, wk.data(), hook);
      }
      const uint64_t r0 = reads;
      pc.LatchPalettes(rec);
      FrameInfo fi;
      fi.guest_frame = rec;
      w.Latch(fi);
      SceneTables t;
      Counters c;
      w.Emit(fi, t, c);
      Out o;
      o.objects = t.objects.size(), o.stale = c.stale, o.reads = reads - r0;
      o.stamp = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInWrestlerPalette].load();
      for (size_t k = 0; k < t.objects.size(); ++k)
        if (t.engine_objects.size() > k && t.engine_objects[k] == kSkel && t.objects[k].bones_count) o.bone0 = t.bones[t.objects[k].bones_first].m[0], o.visible = (t.objects[k].flags & kObjVisible) != 0;
      for (const Object& ob : t.objects) o.invisible += (ob.flags & kObjVisible) ? 0 : 1;
      return o;
    };
    auto table = [&](bool occupied) {
      for (uint32_t i = 0; i < 9; ++i) g_guest.Put(live::lightlatch::kCharSlotTable + 4 * i, i == 0 && occupied ? kCh : 0u);
      g_guest.Put(kCh + live::lightlatch::kCharSkeletonOff, kSkel);
      g_guest.Put(kCh + 52, kPiece);   // a decoy next to the skeleton word
      g_guest.Put(kCh + 1208, 1u), g_guest.Put(kCh + 224, 1u), g_guest.Put(kCh + 232, 1u);   // S107 CAM: the three words of the game's shown predicate sub_823CBC50 ([char+232] is also the flag the draw ask reads)
    };
    g_mode = 2;
    {   // 1. the hook goes quiet, the character stays in the table: read every latch, emitted with the CURRENT palette, palettes read == wrestler objects, the stamp given
      BuildGuest();
      g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
      table(true);
      live::WrestlerContributor w;
      live::inputs::InputsContributor pc;
      setup(w, pc);
      ResetStamps();
      for (uint64_t r = 100; r < 110; ++r) frame(w, pc, r, true, true);   // posed and drawn
      bool fresh = true, counted = true, stamped = true;
      uint32_t stale = 0;
      Out o;
      // (records 110..115: the bit-3 guard still holds the quiet hook's last derived heads, which differ from the moving [skel+200]: those reads are refused, the stamp withheld and, in mode Ours, the body
      // dropped, fail-closed; from 116 the heads are out of the window and the latch read stands alone)
      for (uint64_t r = 110; r < 116; ++r) {
        o = frame(w, pc, r, false, false);
        if (o.objects && !Near(o.bone0, float(r - 1))) fresh = false;   // never an OLD palette, even here
      }
      for (uint64_t r = 116; r < 150; ++r) {
        o = frame(w, pc, r, false, r % 30 == 0);   // mode Ours: the game walks only now and then (a sentinel / a hole frame) and sees him then; review C keeps him visible in between
        fresh = fresh && o.objects == 1 && o.visible && Near(o.bone0, float(r - 1));   // the latch of record r - 1 read [skel+200] = frame r - 1's palette (kPaletteLead: record r takes tag <= r - 1)
        counted = counted && o.reads == o.objects;
        stamped = stamped && o.stamp == r - 1;
        stale += o.stale;
      }
      Check(fresh && stale == 0, "l31b.slot_read_after_hook_quiet", "objects " + std::to_string(o.objects) + " bone " + std::to_string(o.bone0) + " stale " + std::to_string(stale) + " (hook quiet " + std::to_string((unsigned long long)w.SlotHookQuiet()) + ")");
      Check(counted, "l31b.palettes_read_eq_objects", "reads " + std::to_string((unsigned long long)o.reads) + " objects " + std::to_string(o.objects));
      Check(stamped && w.SlotHookQuiet() > 0 && w.SlotTargets() >= 40, "l31b.stamp_and_census", "stamp " + std::to_string((unsigned long long)o.stamp) + " " + w.SlotCensus());
      {   // review E: Take (the W4 override tables) asks for the same targets again: it counts nothing
        const uint64_t before = w.SlotTargets(), quiet = w.SlotHookQuiet();
        live::inputs::InputsContributor::Frame tf;
        pc.Take(150, tf);
        Check(w.SlotTargets() == before && w.SlotHookQuiet() == quiet, "l31b.take_counts_nothing", "slot_targets " + std::to_string((unsigned long long)before) + " -> " + std::to_string((unsigned long long)w.SlotTargets()));
      }
    }
    {   // review C: the hook goes quiet and the game's draw does NOT see him (a waiting opponent: the early exit +220 == 0 && +296 == 0): the palette is still read, but he is emitted invisible (a hole:
        // the frame goes to the game) until a walk sees him again; then visible again. Twin l31b_no_walk_gate (table membership alone keeps him visible) fails.
      BuildGuest();
      g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
      table(true);
      live::WrestlerContributor w;
      live::inputs::InputsContributor pc;
      setup(w, pc);
      ResetStamps();
      for (uint64_t r = 100; r < 110; ++r) frame(w, pc, r, true, true);   // posed and drawn
      Out o;
      bool hidden = true;
      for (uint64_t r = 110; r < 200; ++r) {
        o = frame(w, pc, r, false, false);
        if (r - 1 > 108 + live::WrestlerContributor::kQuietSeenWindow && r >= 116) hidden = hidden && o.objects == 1 && !o.visible && o.invisible == 1;   // past the window: kept, invisible
      }
      o = frame(w, pc, 200, false, true);   // a walk sees him (this frame walked)
      o = frame(w, pc, 201, false, false);
      const bool back = o.objects == 1 && o.visible;
      Check(hidden && back && w.QuietUnseen() > 0, "l31b.quiet_unseen_is_hole", "hidden " + std::to_string(hidden) + " back " + std::to_string(back) + " quiet_unseen " + std::to_string((unsigned long long)w.QuietUnseen()));
    }
    {   // S103 DRL: the freeze frame (no update hook, no walk: only the game's draw ASK). A shown body stays visible past the quiet window; the twin with no ask (plant) is quiet_unseen; a HIDDEN
        // character ([char+232] == 0) that is still asked about stays hidden (a draw ask must not revive it).
      const auto rw = [](uint32_t a, uint32_t& v) { auto it = g_guest.words.find(a); if (!g_guest.readable || it == g_guest.words.end()) return false; v = it->second; return true; };
      auto run = [&](uint64_t ask_to, bool shown, uint64_t hide_from, uint64_t from, bool& visible_all, bool& hidden_all, uint64_t& unseen, uint64_t& asked) {   // asks while r < ask_to; the game hides him from hide_from (0 = never); the verdict is read from `from`
        BuildGuest();
        g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
        table(true);
        g_guest.Put(kCh + 232, shown ? 1u : 0u);
        live::WrestlerContributor w;
        live::inputs::InputsContributor pc;
        setup(w, pc);
        ResetStamps();
        for (uint64_t r = 100; r < 110; ++r) frame(w, pc, r, true, true);   // live match: update hook + walk
        visible_all = true;
        hidden_all = true;
        for (uint64_t r = 110; r < 260; ++r) {   // the freeze: nothing updates him; the game keeps asking to draw him
          const bool ask = r < ask_to;
          if (hide_from != 0 && r == hide_from) g_guest.Put(kCh + 232, 0u);   // S107 CAM: the game hides him (the shown predicate sub_823CBC50 goes false: read by the latch)
          const Out o = frame(w, pc, r, false, false, ask);
          if (ask)
            for (int k = 0; k < 3; ++k) w.NoteDrawAsked(kSkel, r - 1, rw);   // ~99 asks a frame: the same frame's further asks cost nothing and stamp nothing more
          if (r >= from) {
            visible_all = visible_all && o.objects == 1 && o.visible;
            hidden_all = hidden_all && o.objects == 1 && !o.visible && o.invisible == 1;
          }
        }
        unseen = w.QuietUnseen();
        asked = w.DrawAsked();
      };
      bool vis = false, hid = false, tv = false, th = false, hv = false, hh = false;
      uint64_t unseen = 0, asked = 0, tunseen = 0, tasked = 0, hunseen = 0, hasked = 0;
      run(260, true, 0, 180, vis, hid, unseen, asked);
      run(0, true, 0, 180, tv, th, tunseen, tasked);
      run(260, false, 0, 180, hv, hh, hunseen, hasked);
      Check(vis && unseen == 0 && asked == 150, "drl.draw_ask_keeps_visible", "visible " + std::to_string(vis) + " quiet_unseen " + std::to_string((unsigned long long)unseen) + " draw_asked " + std::to_string((unsigned long long)asked) + " (once per frame: 150)");
      Check(th && !tv && tunseen > 0 && tasked == 0, "drl.plant_no_ask_quiet_unseen", "hidden " + std::to_string(th) + " visible " + std::to_string(tv) + " quiet_unseen " + std::to_string((unsigned long long)tunseen));
      Check(hh && !hv && hunseen > 0 && hasked == 0, "drl.hidden_body_ask_stays_hidden", "hidden " + std::to_string(hh) + " visible " + std::to_string(hv) + " quiet_unseen " + std::to_string((unsigned long long)hunseen) + " draw_asked " + std::to_string((unsigned long long)hasked));
      bool sv = false, sh = false, pv = false, ph = false;   // review 1: the game STOPS asking about a body it hides / the freeze ends: invisible within the short window (kConsumerFresh), not 60 frames
      uint64_t su = 0, sa = 0, pu = 0, pa = 0;
      run(200, true, 0, 215, sv, sh, su, sa);     // asks stop at 200 (the live walk stamp expired at ~170), he stays shown in the table: quiet by 215
      run(200, true, 200, 215, pv, ph, pu, pa);   // asks stop and the game hides him at 200
      Check(sh && !sv && su > 0, "drl.freeze_end_no_asks_goes_quiet", "hidden " + std::to_string(sh) + " visible " + std::to_string(sv) + " quiet_unseen " + std::to_string((unsigned long long)su));
      Check(ph && !pv && pu > 0, "drl.hidden_no_further_asks_goes_quiet", "hidden " + std::to_string(ph) + " visible " + std::to_string(pv) + " quiet_unseen " + std::to_string((unsigned long long)pu));
      {   // review 5: a slot body the update hook never met (Known lacks it): published by LatchTargets, so the filter passes it; the draw ask counts draw_unknown and creates nothing
        live::WrestlerContributor unk;
        live::WrestlerContributor::SlotTable st;
        st.skels.push_back({0x40001230, 0, 0});
        st.state[0] = live::WrestlerContributor::kSlotOccupied;
        std::vector<live::WrestlerContributor::PaletteTarget> t;
        uint32_t missing = 0;
        unk.LatchTargets(5, &st, t, missing);
        unk.NoteDrawAsked(0x40001230, 5, [](uint32_t, uint32_t& v) { v = 1; return true; });
        unk.NoteDrawAsked(0x40009990, 5, [](uint32_t, uint32_t& v) { v = 1; return true; });   // not published: dropped by the lock-free filter
        const bool counted = unk.SlotCensus().find("unknown 1)") != std::string::npos;
        Check(unk.DrawAsked() == 0 && unk.KnownCount() == 0 && counted, "drl.unknown_skeleton_not_created", "asked " + std::to_string((unsigned long long)unk.DrawAsked()) + " known " + std::to_string((unsigned long long)unk.KnownCount()) + " draw_unknown counted " + std::to_string(counted));
        unk.NoteDrawAsked(0x40001230, 5 + 1000, [](uint32_t, uint32_t& v) { v = 1; return true; });   // review 3: the published list is stale (no latch for > kQuietSeenWindow frames): no match
        Check(unk.SlotCensus().find("unknown 1)") != std::string::npos, "drl.stale_list_ignored", "a list older than the window does not match a reused address");
      }
      {   // BHF1: G1 (sticky slot / address reuse), BH4 6 (a draw-ask hide clears every stamp), B6 (a hide always wins over a concurrent stamp), B7 (a republished position does not inherit "checked")
        constexpr uint32_t kA = 0x40001230, kB = 0x40001330, kShown = 0x4000AAAA;
        std::vector<Mat4> m7(7), m5(5);
        // the guest read: [char+48] (shown - 184) = the skeleton the character owns (`owner`), [char+232] = `flag`
        auto rd = [&](uint32_t flag, uint32_t owner = kA) { return [flag, owner](uint32_t ad, uint32_t& v) { v = ad == kShown - 184 ? owner : flag; return true; }; };
        for (int plant = 0; plant < 3; ++plant) {
          live::WrestlerContributor w;
          w.SetFaultsForTest(plant == 1 ? live::WrestlerContributor::kFaultKeepStaleKnown : plant == 2 ? live::WrestlerContributor::kFaultHideKeepsStamps : 0u);
          w.SubmitSkin(kA, 3, 5, m5.data(), 10);
          w.SubmitSkin(kA, -1, 5, m5.data(), 11);   // the update hook no longer matches a slot at this address
          live::WrestlerContributor::KnownProbe p;
          Check(w.ProbeForTest(kA, p) && (plant == 1 ? p.slot == 3 : p.slot == -1), plant == 1 ? "g1.plant_slot_sticky" : "g1.unmatched_update_clears_slot", "slot after an unmatched (-1) update: " + std::to_string(p.slot) + (plant == 1 ? " (the old rule keeps 3)" : " (cleared)"));
          w.StampDrawForTest(kA, 12);
          w.SubmitSkin(kA, -1, 7, m7.data(), 13);   // another node count at the same address = another object
          w.ProbeForTest(kA, p);
          Check(plant == 1 ? p.draw == 12 : (p.draw == 0 && w.ReuseReset() == 1), plant == 1 ? "g1.plant_no_reset" : "g1.count_change_resets_liveness", "draw stamp after a node-count change: " + std::to_string((unsigned long long)p.draw));
          live::WrestlerContributor::SlotTable st;
          st.skels.push_back({kA, 0, kShown});
          st.state[0] = live::WrestlerContributor::kSlotOccupied;
          std::vector<live::WrestlerContributor::PaletteTarget> t;
          uint32_t missing = 0;
          w.LatchTargets(20, &st, t, missing);
          w.SubmitConsumer(kA, 7, m7.data(), 21);   // a walk stamp + (below) a draw ask stamp
          w.NoteDrawAsked(kA, 21, rd(1));
          w.ProbeForTest(kA, p);
          const bool stamped = p.draw == 21 && p.walk == 21;
          w.NoteDrawAsked(kA, 22, rd(0));   // the game hides him (seen by the draw ask only)
          w.ProbeForTest(kA, p);
          Check(stamped && p.draw == 0 && (plant == 2 ? p.walk == 21 : p.walk == 0), plant == 2 ? "bh4.plant_hide_keeps_walk" : "bh4.draw_hide_clears_every_stamp", "after a draw-ask hide: draw " + std::to_string((unsigned long long)p.draw) + " walk " + std::to_string((unsigned long long)p.walk));
        }
        {   // BH3-4: the published flag address is old and its character now owns ANOTHER skeleton: the word is arbitrary, nothing is stamped (counted unread)
          live::WrestlerContributor w;
          w.SubmitSkin(kA, 0, 5, m5.data(), 10);
          live::WrestlerContributor::SlotTable st;
          st.skels.push_back({kA, 0, kShown});
          st.state[0] = live::WrestlerContributor::kSlotOccupied;
          std::vector<live::WrestlerContributor::PaletteTarget> t;
          uint32_t missing = 0;
          w.LatchTargets(20, &st, t, missing);
          w.NoteDrawAsked(kA, 21, rd(1, 0x40777000u));
          Check(w.DrawAsked() == 0 && w.SlotCensus().find("unread 1") != std::string::npos, "bh3_4.stale_flag_owner_not_trusted", "a character that no longer owns the skeleton: no stamp, draw_unread counted");
          w.NoteDrawAsked(kA, 22, rd(1));
          Check(w.DrawAsked() == 1, "bh3_4.owner_matches_stamps", "the owner word equals the skeleton: the flag is trusted");
        }
        {   // B6: the replay hide arrives (thread started inside the stamp's flag read, blocked on the lock) AFTER the flag read said shown: the hide's clear must follow the stamp
          live::WrestlerContributor w;
          w.SubmitSkin(kA, 0, 5, m5.data(), 10);
          live::WrestlerContributor::SlotTable st;
          st.skels.push_back({kA, 0, kShown});
          st.state[0] = live::WrestlerContributor::kSlotOccupied;
          std::vector<live::WrestlerContributor::PaletteTarget> t;
          uint32_t missing = 0;
          w.LatchTargets(20, &st, t, missing);
          for (int plant = 0; plant < 2; ++plant) {   // plant 1 = kFaultFlagReadUnlocked: the old code reads the flag before the lock
            w.SetFaultsForTest(plant ? live::WrestlerContributor::kFaultFlagReadUnlocked : 0u);
            bool free_during_read = false;
            std::thread other;   // another game thread, at the moment the flag word is read: is the lock free for the replay hide to run?
            w.NoteDrawAsked(kA, 21 + plant, [&](uint32_t ad, uint32_t& v) {
              v = ad == kShown - 184 ? kA : 1;
              if (ad == kShown && !other.joinable()) {
                other = std::thread([&] { free_during_read = w.TryLockForTest(); });
                if (other.joinable()) other.join();
              }
              return true;
            });
            Check(plant ? free_during_read : !free_during_read, plant ? "b6.plant_flag_read_unlocked" : "b6.flag_read_holds_the_lock", std::string("the lock is ") + (free_during_read ? "FREE" : "HELD") + " while the shown flag is read (a replay hide that arrives then must wait for the stamp)");
          }
        }
        {   // B7: the republish moves body B into position 0 on the same frame body A was checked
          live::WrestlerContributor w;
          w.SubmitSkin(kA, 0, 5, m5.data(), 10);
          w.SubmitSkin(kB, 1, 5, m5.data(), 10);
          live::WrestlerContributor::SlotTable sa, sb;
          sa.skels.push_back({kA, 0, 0});
          sa.state[0] = live::WrestlerContributor::kSlotOccupied;
          sb.skels.push_back({kB, 1, 0});
          sb.state[1] = live::WrestlerContributor::kSlotOccupied;
          std::vector<live::WrestlerContributor::PaletteTarget> t;
          uint32_t missing = 0;
          auto yes = [](uint32_t, uint32_t& v) { v = 1; return true; };
          w.LatchTargets(20, &sa, t, missing);
          w.NoteDrawAsked(kA, 21, yes);
          w.LatchTargets(20, &sb, t, missing);   // position 0 now holds B
          w.NoteDrawAsked(kB, 21, yes);
          Check(w.DrawAsked() == 2, "b7.republish_does_not_inherit_checked", "B stamped in the frame A was checked at the same position: draw_asked " + std::to_string((unsigned long long)w.DrawAsked()));
        }
      }
    }
    {   // S107 CAM (docs\research\s107\CAM_EXTRA.md): Create a Moveset's extra slot bodies are POSED by the skeleton update (update hook every frame) but the game never ASKS to draw them: in the stream-free build
        // such a body is invisible (a hole, quiet_unseen), shown only while the game asks (draw_ask). Planted twin cam_upd_shows = the old rule (upd_frame shows him).
      auto run = [&](bool ask, uint64_t hide_from, bool& vis_late, bool& hid_late, bool& vis_before, uint64_t& unseen) {
        BuildGuest();
        g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
        table(true);
        g_guest.Put(kCh + 232, 1u);   // all three words of the shown predicate readable
        live::WrestlerContributor w;
        live::inputs::InputsContributor pc;
        setup(w, pc);
        w.SetNoWalk(true);  // the player build: no command-stream walk
        ResetStamps();
        vis_late = true, hid_late = true, vis_before = true;
        for (uint64_t r = 100; r < 250; ++r) {
          if (hide_from != 0 && r == hide_from) g_guest.Put(kCh + 1208, 0u);   // the game hides him: sub_823CBC50 ([char+1208] && [char+224] && [char+232]) goes false (+232 stays unset-or-set: the ask path alone would not catch it)
          const Out o = frame(w, pc, r, true, false, ask);   // the skeleton update runs EVERY frame; the draw ask only when asked
          if (r >= 120 && (hide_from == 0 || r < hide_from)) vis_late = vis_late && o.objects == 1 && o.visible, hid_late = hid_late && o.objects == 1 && !o.visible && o.invisible == 1;
          if (hide_from != 0 && r == hide_from) vis_before = o.objects == 1 && o.visible;   // the record of the very frame he was hidden in
        }
        unseen = w.QuietUnseen();
      };
      bool v = false, h = false, b = false, tv = false, th = false, tb = false, gv = false, gh = false, gb = false;
      uint64_t unseen = 0, tunseen = 0, gunseen = 0;
      run(false, 0, v, h, b, unseen);    // posed, never asked (the extra editor body)
      run(true, 0, tv, th, tb, tunseen);   // posed and asked
      run(true, 150, gv, gh, gb, gunseen);   // posed and asked, then the game hides him at 150 ([char+1208] = 0) while the asks go on: hidden AT ONCE (not after kConsumerFresh records)
      Check(h && !v && unseen > 0, "cam.posed_unasked_is_hidden", "hidden " + std::to_string(h) + " visible " + std::to_string(v) + " quiet_unseen " + std::to_string((unsigned long long)unseen) + " (a posed body the game never asks to draw is a hole)");
      Check(tv && !th && tunseen == 0, "cam.posed_and_asked_visible", "visible " + std::to_string(tv) + " quiet_unseen " + std::to_string((unsigned long long)tunseen));
      Check(gv && !gb && gunseen > 0, "cam.game_hidden_is_hidden", "visible before the hide " + std::to_string(gv) + " visible in the record of the hide frame " + std::to_string(gb) + " quiet_unseen " + std::to_string((unsigned long long)gunseen) + " (the shown predicate false = hidden at once)");
    }
    {   // review A: presence by SLOT. The body is stale (its block unreadable) and the table's slot 0 holds (a) another pointer (a wrong [char+48] [L]), (b) an unreadable word: in both he is still
        // present: the frame walks (no stamp), never ours without him. Twin l31b_pointer_presence (the pointer rule) gives the stamp.
      bool held = true;
      for (int variant = 0; variant < 2; ++variant) {
        BuildGuest();
        g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
        table(true);
        live::WrestlerContributor w;
        live::inputs::InputsContributor pc;
        setup(w, pc);
        ResetStamps();
        for (uint64_t r = 400; r < 410; ++r) frame(w, pc, r, true, true);
        g_guest.words.erase(kSkel + scene_inputs::kPaletteOff);
        if (variant == 0) g_guest.Put(kCh + live::lightlatch::kCharSkeletonOff, 0x40400000u);   // not the skeleton the hook matched (never sized: not read)
        else g_guest.words.erase(live::lightlatch::kCharSlotTable);                              // slot 0's word unreadable
        Out o;
        for (uint64_t r = 410; r < 440; ++r) {
          o = frame(w, pc, r, false);
          if (r >= 416) held = held && o.objects == 0 && o.stamp != r - 1;
        }
      }
      Check(held, "l31b.presence_by_slot", "held " + std::to_string(held));
    }
    {   // review B: a stale INVISIBLE skeleton (no slot: a piece, a skel:) is still emitted in mode Ours, so rf_wrestlers keeps it as a hole. Twin l31b_drop_invisible drops it.
      BuildGuest();
      table(false);
      live::WrestlerContributor w;
      live::inputs::InputsContributor pc;
      setup(w, pc);
      ResetStamps();
      std::vector<Mat4> d(kCount);
      for (auto& m : d) std::memset(m.m, 0, sizeof m.m);
      for (uint64_t r = 500; r < 510; ++r) {
        w.SubmitSkin(kM1, -1, kCount, d.data(), r - 1);
        frame(w, pc, r, false);
      }
      Out o;
      bool kept = true;
      for (uint64_t r = 510; r < 530; ++r) {
        o = frame(w, pc, r, false);
        kept = kept && o.invisible == 1;
      }
      Check(kept, "l31b.stale_invisible_stays_hole", "invisible " + std::to_string(o.invisible) + " objects " + std::to_string(o.objects));
    }
    {   // S103 AUSF (AUS2 fix 2): a build with NO walk has no "the frame walks" fallback. A stale VISIBLE body the character table still holds is HELD with its kept palette for up to kConsumerFresh records, then dropped (the table
        // still holds him, but the hold is bounded); one that left the table is dropped at once; the stream build (SetNoWalk(false)) keeps today's immediate drop. Planted twin ausf_drop (kFaultNoWalkRefuse = today's rule in a no-walk
        // build): the body vanishes the moment its palette is stale = "l33_ausf.stale_held_bounded" fails.
      for (int variant = 0; variant < 3; ++variant) {   // 0 = no-walk, stays in the table; 1 = no-walk, leaves the table at 412; 2 = the stream build
        BuildGuest();
        g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
        table(true);
        live::WrestlerContributor w;
        live::inputs::InputsContributor pc;
        setup(w, pc);
        w.SetNoWalk(variant != 2);
        if (Plant("ausf_drop")) w.SetFaultsForTest(live::WrestlerContributor::kFaultNoWalkRefuse);
        ResetStamps();
        Out fresh;
        for (uint64_t r = 400; r < 410; ++r) fresh = frame(w, pc, r, true, true, true);   // S107 CAM: the game draws him (asks)
        g_guest.words.erase(kSkel + scene_inputs::kPaletteOff);   // the palette block goes unreadable: no fresh palette from now on
        int held_records = 0;
        bool held_visible = true, held_kept_palette = true;   // review: a held body is VISIBLE and carries the KEPT palette (the last fresh read), never a zero / bind pose
        bool back_to_zero_stays = true, seen_zero = false;
        for (uint64_t r = 410; r < 440; ++r) {
          if (variant == 1 && r == 412) table(false);
          const Out o = frame(w, pc, r, false);
          if (o.objects == 1 && o.stale > 0) ++held_records, held_visible = held_visible && o.visible, held_kept_palette = held_kept_palette && o.bone0 == fresh.bone0 && fresh.bone0 != -1.f;
          else if (o.objects == 1 && o.stale == 0) fresh = o;
          if (o.objects == 0) seen_zero = true;
          else if (seen_zero) back_to_zero_stays = false;
        }
        const Out end = frame(w, pc, 440, false);
        const std::string why = "variant " + std::to_string(variant) + " held_records " + std::to_string(held_records) + " stale_held " + std::to_string((unsigned long long)w.StaleHeld()) + " stale_dropped " + std::to_string((unsigned long long)w.StaleDropped());
        if (variant == 0) Check(held_visible && held_kept_palette && held_records >= 1 && held_records <= int(live::WrestlerContributor::kConsumerFresh) && back_to_zero_stays && end.objects == 0 && w.StaleHeld() == uint64_t(held_records), "l33_ausf.stale_held_bounded", why);
        else if (variant == 1) Check(held_records <= 2 && end.objects == 0 && back_to_zero_stays, "l33_ausf.left_table_dropped", why);   // records 410-411 may still hold him (still in the table); from 412 he is gone
        else Check(held_records == 0 && w.StaleHeld() == 0 && end.objects == 0, "l33_ausf.stream_build_drops_at_once", why);
      }
    }
    {   // S103 AUSF Opus review 2 / 3 / 4: the hold's three limits. variant 0 = a HIDDEN stale body (the game stopped updating / drawing it long ago: not held), 1 = a live stale body whose slot a FRESH body claims (not held: it
        // would take the slot from him), 2 = a held body keeps its spring-hair rows. Twins: ausf_hold_hidden (kFaultHoldAny), ausf_hold_slot (kFaultHoldNoSlotCheck), ausf_hold_extras (kFaultHoldNoExtras).
      for (int variant = 0; variant < 3; ++variant) {
        BuildGuest();
        g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
        table(true);
        live::WrestlerContributor w;
        live::inputs::InputsContributor pc;
        setup(w, pc);
        w.SetNoWalk(true);
        uint32_t fl = 0;
        if (variant == 0 && Plant("ausf_hold_hidden")) fl |= live::WrestlerContributor::kFaultHoldAny;
        if (variant == 1 && Plant("ausf_hold_slot")) fl |= live::WrestlerContributor::kFaultHoldNoSlotCheck;
        if (variant == 2) fl |= live::WrestlerContributor::kFaultHairAnyFrame | (Plant("ausf_hold_extras") ? live::WrestlerContributor::kFaultHoldNoExtras : 0u);
        w.SetFaultsForTest(fl);
        ResetStamps();
        std::vector<Mat4> d(kCount);
        for (auto& m : d) std::memset(m.m, 0, sizeof m.m);
        if (variant == 2) w.SetPieceRowsForTest(kSkel, 7);
        for (uint64_t r = 400; r < 410; ++r) {
          if (variant == 1) w.SubmitSkin(kM1, 0, kCount, d.data(), r - 1);   // the fresh claimant of slot 0 is updating all along
          frame(w, pc, r, true, true, true);   // S107 CAM: asked (the live phase)
        }
        if (variant == 0) for (uint64_t r = 410; r < 440; ++r) frame(w, pc, r, false);   // quiet for 30 records, the palette still READ (fresh each latch): the game stopped updating him
        const uint64_t stale_from = variant == 0 ? 440 : 410;
        g_guest.words.erase(kSkel + scene_inputs::kPaletteOff);
        size_t max_objects = 0;
        int held_records = 0;
        for (uint64_t r = stale_from; r < stale_from + 4; ++r) {
          if (variant == 1) w.SubmitSkin(kM1, 0, kCount, d.data(), r - 1);   // a fresh body resolves to slot 0 (the vest-off swap case)
          const Out o = frame(w, pc, r, false);
          if (variant != 1 || r > stale_from) max_objects = std::max(max_objects, o.objects);   // variant 1: record stale_from itself still has both bodies fresh
          if (o.objects >= 1 && o.stale > 0) ++held_records;
        }
        const std::string why = "variant " + std::to_string(variant) + " max_objects " + std::to_string((unsigned long long)max_objects) + " held_records " + std::to_string(held_records) + " held_with_extras " + std::to_string((unsigned long long)w.HeldWithExtras());
        if (variant == 0) Check(held_records == 0 && w.StaleHeld() == 0, "l33_ausf.hidden_stale_not_held", why);
        else if (variant == 1) Check(max_objects <= 1 && w.StaleHeld() == 0, "l33_ausf.held_never_over_fresh_slot", why);
        else Check(w.StaleHeld() >= 1 && w.HeldWithExtras() == w.StaleHeld(), "l33_ausf.held_keeps_hair_rows", why);
      }
    }
    {   // S103 AUSF Opus review 1: an OFF-latch mismatch (Emit's missed-latch read, game threads running) is refused even with no walk. Twin ausf_offlatch submits it.
      // (driven in the l33 block below through SetOffLatchThread)
    }
    {   // review D: a hook-quiet slot body whose block stays unreadable leaves the targets once it has left the record (kPruneAfter): the stamp comes back (counted slot_gone), never held for
        // kKnownAge records. Twin l31b_keep_gone keeps it a target: the stamp never returns.
      BuildGuest();
      g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
      table(true);
      live::WrestlerContributor w;
      live::inputs::InputsContributor pc;
      setup(w, pc);
      ResetStamps();
      for (uint64_t r = 600; r < 610; ++r) frame(w, pc, r, true, true);
      g_guest.words.erase(kSkel + scene_inputs::kPaletteOff);
      Out o;
      const uint64_t end = 610 + live::WrestlerContributor::kPruneAfter + 20;
      for (uint64_t r = 610; r < end; ++r) o = frame(w, pc, r, false);
      Check(o.stamp == end - 2 && o.objects == 0 && w.SlotGone() > 0, "l31b.gone_slot_target_released", "stamp " + std::to_string((unsigned long long)o.stamp) + " want " + std::to_string((unsigned long long)(end - 2)) + " " + w.SlotCensus());
    }
    {   // 2. the guard: the body is in the table but cannot be read (its palette is gone): in mode Ours it is never written with the old palette, and the frame walks (no stamp)
      BuildGuest();
      g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
      table(true);
      live::WrestlerContributor w;
      live::inputs::InputsContributor pc;
      setup(w, pc);
      ResetStamps();
      for (uint64_t r = 200; r < 210; ++r) frame(w, pc, r, true);
      g_guest.words.erase(kSkel + scene_inputs::kPaletteOff);   // [skel+200] unreadable from now on
      bool none = true, walked = true;
      Out o = frame(w, pc, 210, false);   // record 210 still takes the good read of latch 209
      for (uint64_t r = 211; r < 230; ++r) {
        o = frame(w, pc, r, false);
        none = none && o.objects == 0;
        walked = walked && o.stamp != r - 1;
      }
      Check(none && walked && w.StaleDropped() > 0, "l31b.stale_never_emitted", "objects " + std::to_string(o.objects) + " bone " + std::to_string(o.bone0) + " stamp " + std::to_string((unsigned long long)o.stamp) + " dropped " + std::to_string((unsigned long long)w.StaleDropped()));
      g_mode = 1;   // mirror: unchanged (the stale entry is emitted, counted stale, for the compare)
      o = frame(w, pc, 230, false);
      Check(o.objects == 1 && o.stale == 1, "l31b.mirror_unchanged", "objects " + std::to_string(o.objects) + " stale " + std::to_string(o.stale));
      g_mode = 2;
    }
    {   // 3. the character leaves the table (hook quiet too): dropped in mode Ours, and NOT a reason to walk (the game no longer draws it)
      BuildGuest();
      g_guest.Put(kSkel + scene_inputs::kPaletteOff, kPal);
      table(true);
      live::WrestlerContributor w;
      live::inputs::InputsContributor pc;
      setup(w, pc);
      ResetStamps();
      for (uint64_t r = 300; r < 310; ++r) frame(w, pc, r, true);
      table(false);
      Out o;
      bool ok = true;
      o = frame(w, pc, 310, false);   // record 310 still takes latch 309's good read
      for (uint64_t r = 311; r < 330; ++r) {   // gone from the table: never read again, never emitted, and the frame is still ours
        o = frame(w, pc, r, false);
        // (until its update hook's liveness window closes, hook 308 + kConsumerFresh, the old rule still reads it and the bit-3 guard refuses the moved block: those frames walk, fail-closed)
        const bool want_stamp = r - 1 > 308 + live::WrestlerContributor::kConsumerFresh;
        if (ok && !(o.objects == 0 && (!want_stamp || o.stamp == r - 1))) std::printf("  l31b left_table first bad: record %llu objects %zu stamp %llu\n", (unsigned long long)r, o.objects, (unsigned long long)o.stamp);
        ok = ok && o.objects == 0 && (!want_stamp || o.stamp == r - 1);
      }
      Check(ok, "l31b.left_table_dropped_not_withheld", "objects " + std::to_string(o.objects) + " stamp " + std::to_string((unsigned long long)o.stamp));
    }
    {   // 4. a piece whose owner body is a slot skeleton is alive with it, though the body's update hook is quiet (the old rule: the owner is dead, the piece is not fed)
      BuildGuest();
      table(true);
      live::WrestlerContributor w;
      std::vector<live::WrestlerContributor::PieceRef> pieces;
      live::WrestlerContributor::PieceRef pr;
      pr.model = kPiece, pr.id = 1;
      pieces.push_back(pr);
      w.SubmitModelLink(kSkel, 5001, 0, 0, 0, std::move(pieces));   // the descriptor lists the piece under the body
      std::vector<Mat4> nodes(60);
      for (auto& m : nodes) std::memset(m.m, 0, sizeof m.m);
      for (uint64_t f = 5; f <= 10; ++f) w.SubmitSkin(kSkel, 0, 60, nodes.data(), f);   // the body's hook stopped at 10
      w.WantSkeleton(kPiece, 3);
      w.SubmitConsumer(kPiece, 3, nodes.data(), 99);
      FrameInfo fi;
      fi.guest_frame = 100;
      w.Latch(fi);
      std::vector<live::WrestlerContributor::PaletteTarget> tg;
      uint32_t missing = 0;
      live::WrestlerContributor::SlotTable sl;
      if (!old_path) sl.skels.push_back({kSkel, 0}), sl.state[0] = live::WrestlerContributor::kSlotOccupied;
      w.LatchTargets(99, old_path ? nullptr : &sl, tg, missing);
      bool has_body = false, has_piece = false;
      for (const auto& t : tg) has_body = has_body || t.skel == kSkel, has_piece = has_piece || t.skel == kPiece;
      Check(has_body && has_piece, "l31b.piece_of_slot_body_alive", "body " + std::to_string(has_body) + " piece " + std::to_string(has_piece));
      (void)kPiecePal;
    }
    BuildGuest();
  }

  // ---------------------------------------------------------------- 3a2. W2 (S103 A2): the light objects and the manager's selection words, read at the latch
  {
    constexpr uint32_t kMgr = 0x40010000, kSetsArr = 0x40011000, kIdsArr = 0x40011100, kRemapArr = 0x40011200, kPoolArr = 0x40011300, kObj0 = 0x40020000, kPool0 = 0x40030000, kBlend = 0x40040000, kOvr = 0x40041000;
    constexpr uint32_t kN = 3, kPoolN = 3;   // pool slot 2 is null (sub_827F8808 leaves unused slots null: normal), pool 1 is the same block as set 1
    const uint32_t boff = Plant("light_off44") ? 44 : 48;
    auto put_block = [&](uint32_t obj, float base) {
      for (uint32_t k = 0; k < kLsLightFloats; ++k) g_guest.PutF(obj + boff + 4 * k, base + float(k) + 0.5f);
      g_guest.PutF(obj + 44, -7.f);   // a decoy before the block
    };
    g_guest.Put(scene_inputs::kLightMgrPtr, kMgr);
    g_guest.Put(kMgr + 124, kN), g_guest.Put(kMgr + 112, kSetsArr), g_guest.Put(kMgr + 116, kIdsArr), g_guest.Put(kMgr + 120, kRemapArr);
    g_guest.Put(kMgr + 140, kPoolArr), g_guest.Put(kMgr + 144, kPoolN);
    g_guest.Put(kMgr + 368, kOvr), g_guest.Put(kMgr + 372, 1), g_guest.Put(kMgr + 376, kBlend);
    for (uint32_t i = 0; i < kN; ++i) {
      g_guest.Put(kSetsArr + 4 * i, kObj0 + 0x1000 * i);
      g_guest.Put(kIdsArr + 4 * i, 0x12340000u | (10 + i));   // the high half must be masked off (the arena's set id is the low 16 bits)
      g_guest.Put(kRemapArr + 4 * i, i == 1 ? 0u : 0xFFFFFFFFu);
      put_block(kObj0 + 0x1000 * i, 1000.f * float(i));
    }
    for (uint32_t j = 0; j < kPoolN; ++j) {
      g_guest.Put(kPoolArr + 4 * j, j == 2 ? 0u : kPool0 + 0x1000 * j);
      if (j < 2) put_block(kPool0 + 0x1000 * j, j == 1 ? 1000.f : 5000.f);   // pool 1 repeats set 1's words: one shared row
    }
    put_block(kBlend, 9000.f), put_block(kOvr, 9500.f);
    g_guest.Put(scene_inputs::kLightOverrideWord0, 5), g_guest.Put(scene_inputs::kLightOverrideWord1, 0xFFFFFFFFu), g_guest.Put(scene_inputs::kLightGatePtr, 0);
    std::vector<LightObjectRow> rows;
    scene_inputs::LightReadInfo li;
    const bool ok = scene_inputs::ReadLightObjects(ReadThunk, 200, rows, li);
    auto wf = [](const LightObjectRow& r, uint32_t k) { float f; std::memcpy(&f, &r.words[k], 4); return f; };
    auto ref = [&](uint32_t k) { return ok ? rows[0].words[kLmRefBase + k] : kLightNoRow; };
    Check(ok && rows.size() == 7 && rows[0].kind == kLoManager && rows[1].kind == kLoSet && rows[4].kind == kLoPool && rows[5].kind == kLoBlend && rows[6].kind == kLoOverride && li.unreadable == 0 && li.null_objects == 1 && li.sets == kN &&
              li.pool == kPoolN && li.distinct == 6,
          "lights.rows", ok ? ("rows " + std::to_string(rows.size()) + " distinct " + std::to_string(li.distinct) + " null " + std::to_string(li.null_objects)) : "unreadable");
    Check(ok && li.objects.size() == 7, "lights.objects_all", ok ? ("objects " + std::to_string(li.objects.size())) : "unreadable");   // 3 sets + 2 non-null pool + blend + override: EVERY address, deduped blocks included (review must 5)
    // the block starts at object+48: set 2 word 7 = 2000 + 7 + 0.5 (the decoy at +44 is -7)
    Check(ok && rows.size() == 7 && ref(2) != kLightNoRow && Near(wf(rows[ref(2)], 7), 2007.5f) && Near(wf(rows[ref(0)], 0), 0.5f) && Near(wf(rows[ref(kLightMaxSets + 0)], 223), 5000.f + 223.5f) && (rows[ref(2)].flags & kLoBlockRead), "lights.block_offset",
          ok && rows.size() == 7 ? ("set2[7] " + std::to_string(wf(rows[ref(2)], 7))) : "no rows");
    // identical blocks share one row, a null slot has none
    Check(ok && ref(1) == ref(kLightMaxSets + 1) && ref(1) != ref(0) && ref(kLightMaxSets + 2) == kLightNoRow && ref(kLmRefBlend) == 5 && ref(kLmRefOverride) == 6, "lights.dedupe_null",
          ok ? ("set1 " + std::to_string(ref(1)) + " pool1 " + std::to_string(ref(kLightMaxSets + 1))) : "no rows");
    {   // the per-set id (low 16 bits) and the pool remap + 1, packed in the manager row
      const uint32_t id0 = Plant("light_remap_id") ? 0x1234000Au : 10u;
      const bool good = ok && rows[0].words[kLmSetInfoBase + 0] == (id0 | (0u << 16)) && rows[0].words[kLmSetInfoBase + 1] == (11u | (1u << 16)) && rows[0].words[kLmSetInfoBase + 2] == (12u | (0u << 16));
      Check(good, "lights.remap_ids", ok ? ("set1 info " + std::to_string(rows[0].words[kLmSetInfoBase + 1])) : "no rows");
    }
    Check(ok && rows[0].aux0 == kN && rows[0].aux1 == kPoolN && rows[0].remap == 1 && rows[0].words[kLmOverrideMode0] == 5 && rows[0].words[kLmOverrideMode1] == 0xFFFFFFFFu && rows[0].words[kLmGateObject] == 0 && rows[0].words[kLmGateFlag] == 0 &&
              (rows[0].flags & kLoBlockRead),
          "lights.manager_row");
    {   // the gate object: word 0x836F53D0 -> object, [+152] the flag
      g_guest.Put(scene_inputs::kLightGatePtr, 0x40050000), g_guest.Put(0x40050000 + 152, 1);
      std::vector<LightObjectRow> r2;
      Check(scene_inputs::ReadLightObjects(ReadThunk, 201, r2, li) && r2[0].words[kLmGateObject] == 0x40050000 && r2[0].words[kLmGateFlag] == 1, "lights.gate_flag");
      g_guest.Put(scene_inputs::kLightGatePtr, 0);
    }
    {   // an unread override word defaults to 0xFFFFFFFF (0 is a valid index) and the manager row says it is incomplete (review 7)
      Guest keep = g_guest;
      g_guest.words.erase(scene_inputs::kLightOverrideWord0);
      std::vector<LightObjectRow> r2;
      const bool ok2 = scene_inputs::ReadLightObjects(ReadThunk, 204, r2, li);
      Check(ok2 && r2[0].words[kLmOverrideMode0] == 0xFFFFFFFFu && !(r2[0].flags & kLoBlockRead) && r2[0].words[kLmOverrideMode1] == 0xFFFFFFFFu, "lights.override_unread_default");
      g_guest = keep;
    }
    {   // one object whose block cannot be read: no row, its ref says so, the others are fine, the count is reported
      Guest keep = g_guest;
      g_guest.words.erase(kPool0 + 48 + 4 * 100);
      std::vector<LightObjectRow> r2;
      const bool ok2 = scene_inputs::ReadLightObjects(ReadThunk, 202, r2, li);
      Check(ok2 && li.unreadable == 1 && r2[0].words[kLmRefBase + kLightMaxSets + 0] == kLightNoRow && r2[0].words[kLmRefBase + 1] != kLightNoRow, "lights.unreadable_block", "unreadable " + std::to_string(li.unreadable));
      g_guest = keep;
    }
    {   // a missing, empty or insane manager gives no rows and says which
      Guest keep = g_guest;
      std::vector<LightObjectRow> r2;
      g_guest.Put(kMgr + 124, 0);
      const bool z = !scene_inputs::ReadLightObjects(ReadThunk, 203, r2, li) && li.why == scene_inputs::LightWhy::NoSets && r2.empty();
      g_guest.Put(kMgr + 124, 9999);
      const bool big = !scene_inputs::ReadLightObjects(ReadThunk, 203, r2, li) && li.why == scene_inputs::LightWhy::Insane;
      g_guest.Put(scene_inputs::kLightMgrPtr, 0);
      const bool nul = !scene_inputs::ReadLightObjects(ReadThunk, 203, r2, li) && li.why == scene_inputs::LightWhy::ManagerUnreadable;
      Check(z && big && nul, "lights.manager_sane", "n=0 -> NoSets / n=9999 -> Insane / null manager -> ManagerUnreadable");
      g_guest = keep;
    }
    {   // a bad light section has its OWN error path: lane A's tables survive (review 8)
      SceneTables tl;
      tl.light_objects = rows;
      tl.post_params.push_back(PostParamsRow{});
      std::vector<uint8_t> by = SerializeRecord(tl);
      SceneTables good;
      std::string er;
      const bool g = ParseRecord(by.data(), by.size(), good, er) && good.light_objects.size() == 7 && good.light_errors == 0 && good.post_params.size() == 1;
      tl.light_objects[0].words[kLmRefBase + 0] = 99;   // a ref past the end
      by = SerializeRecord(tl);
      SceneTables bad;
      const bool b = ParseRecord(by.data(), by.size(), bad, er) && bad.light_objects.empty() && bad.light_errors == 1 && bad.post_params.size() == 1 && bad.inputs_errors == 0;
      Check(g && b, "lights.own_error_path", er);
    }
    {   // through the latch contributor, the record file and back; mode Off reads nothing
      ResetStamps();
      live::inputs::InputsContributor lc;
      lc.SetReader(ReadThunk);
      FrameInfo fl;
      fl.guest_frame = 500;
      lc.Latch(fl);
      SceneTables tl;
      Counters cl;
      lc.Emit(fl, tl, cl);
      Check(tl.light_objects.size() == rows.size() && tl.light_objects[0].frame == 500 && tl.light_objects.size() == 7, "lights.contrib_rows", "rows " + std::to_string(tl.light_objects.size()));
      std::vector<uint8_t> by = SerializeRecord(tl);
      SceneTables back;
      std::string er;
      const bool parsed = ParseRecord(by.data(), by.size(), back, er);
      if (Plant("light_lose_sec")) back.light_objects.clear();
      Check(parsed && back.light_objects.size() == tl.light_objects.size() && std::memcmp(back.light_objects.data(), tl.light_objects.data(), tl.light_objects.size() * sizeof(LightObjectRow)) == 0 && SerializeRecord(back) == by,
            "lights.roundtrip", er);
      g_mode = 0;
      live::inputs::InputsContributor off;
      off.SetReader(ReadThunk);
      FrameInfo fo;
      fo.guest_frame = 501;
      off.Latch(fo);
      SceneTables to;
      Counters co;
      off.Emit(fo, to, co);
      Check(to.light_objects.empty(), "lights.off_reads_nothing");
      g_mode = 2;
    }
    BuildGuest();
  }

  // ---------------------------------------------------------------- 3a3. W4 / A4 (S103 A2): a model's run-time parameter override entries
  {
    constexpr uint32_t kB = 0x40060000, kWide = 0x40061000, kTab = 0x40063000, kCnt = 0x40063100, kP1 = 0x40064000, kP2 = 0x40065000, kVal = 0x40066000;
    const uint32_t stride = Plant("param_stride16") ? 16 : 20;
    g_guest.Put(kM0 + 832, kB), g_guest.Put(kM0 + 372, 3);
    g_guest.Put(kB + 1132, kWide), g_guest.Put(kB + 1136, 2), g_guest.Put(kB + 1140, kTab), g_guest.Put(kB + 1144, kCnt);
    // the name strings: 16 bytes each at kNames + 16 * k, big-endian words; "BSSSwtMax" style names
    constexpr uint32_t kNames = 0x40068000;
    auto put_name = [&](uint32_t k, const char* nm) {
      uint8_t b[16] = {};
      std::strncpy(reinterpret_cast<char*>(b), nm, 15);
      for (int w = 0; w < 4; ++w) g_guest.Put(kNames + 16 * k + 4 * w, (uint32_t(b[4 * w]) << 24) | (uint32_t(b[4 * w + 1]) << 16) | (uint32_t(b[4 * w + 2]) << 8) | b[4 * w + 3]);
    };
    put_name(0, "g_fBSSSwtMax"), put_name(1, "g_f4MuscleParam"), put_name(2, "g_fMdlAlpha"), put_name(3, "g_f4SpecialPara"), put_name(4, "g_fTime");
    // WRK F1 (WRK.md 1d.5): word 0 = the 360's double-buffer slot 0 (a STALE value, kStale), word 1 = the SOURCE pointer (the live value), word 4 = the count in floats. The planted twin swaps the two
    // pointers = the old reader (slot 0): params.live_source must then see the stale 999.x value and fail.
    constexpr uint32_t kStale = 0x40067000;
    // WRK_ADV 1c: word 3 = the game's memcpy size in BYTES (the reader's size); word 4 = the count (4 * count is NOT the size for the value-type 1 / 2 names). The twin param_size_count claims 64 bytes everywhere: a reader
    // that does not honour word 3 (or honours it wrongly) reads past a 16-byte value and params.live_source fails.
    auto put_entry = [&](uint32_t base, uint32_t i, uint32_t key, uint32_t val_ptr, uint32_t bytes = 64, uint32_t count = 16) {
      const uint32_t name_ptr = Plant("param_name_hash") ? key : kNames + 16 * (key & 0xF);   // the planted guest keeps a hash-like key in word 2
      const bool slot0 = Plant("param_read_slot0");
      g_guest.Put(base + stride * i + 0, slot0 ? val_ptr : kStale), g_guest.Put(base + stride * i + 4, slot0 ? kStale : val_ptr), g_guest.Put(base + stride * i + 8, name_ptr), g_guest.Put(base + stride * i + 12, Plant("param_size_count") ? 64u : bytes), g_guest.Put(base + stride * i + 16, count);
    };
    for (uint32_t k = 0; k < 16; ++k) g_guest.PutF(kStale + 4 * k, 999.f + float(k));
    // the 20-byte stride: the planted 16-byte layout leaves word 4 of every entry in the next entry's place (the guest words at those addresses are the next entry's)
    put_entry(kWide, 0, 0, kVal), put_entry(kWide, 1, 1, kVal + 0x100);
    put_entry(kP1, 0, 2, kVal + 0x200);
    put_entry(kP2, 0, 3, kVal + 0x300, 4, 4), put_entry(kP2, 1, 4, 0x40FFF000);   // the last one's live pointer is not readable; entry 3 has 16 bytes (count 8 on purpose: the size is word 3, not 4 * word 4): only 4 words are read
    const bool swap = Plant("param_part_swap");
    g_guest.Put(kTab + 0, 0), g_guest.Put(kTab + 4, swap ? kP2 : kP1), g_guest.Put(kTab + 8, swap ? kP1 : kP2);
    g_guest.Put(kCnt + 0, 0), g_guest.Put(kCnt + 4, 1), g_guest.Put(kCnt + 8, 2);
    for (uint32_t k = 0; k < 16; ++k)
      for (uint32_t blk = 0; blk < 4; ++blk) g_guest.PutF(kVal + 0x100 * blk + 4 * k, 7.f + float(blk) * 10.f + float(k));
    std::vector<ParamOverrideRow> rows;
    scene_inputs::ParamReadInfo pi;
    const bool ok = scene_inputs::ReadParamOverrides(ReadThunk, kM0, 600, rows, pi);
    Check(ok && rows.size() == 5 && pi.entries == 5 && pi.value_unreadable == 1 && rows[0].part == kParamModelWide && rows[2].part == 1 && rows[3].part == 2 && rows[4].index == 1, "params.rows", ok ? ("rows " + std::to_string(rows.size())) : "unreadable");
    Check(ok && rows.size() == 5 && rows[1].entry[3] == 64 && rows[1].entry[4] == 16 && rows[1].entry[1] == kVal + 0x100 && rows[3].entry[3] == 4 && rows[3].entry[4] == 4, "params.entries",
          ok && rows.size() == 5 ? ("e[1][3] " + std::to_string(rows[1].entry[3]) + " e[1][4] " + std::to_string(rows[1].entry[4])) : "no rows");
    Check(ok && rows.size() == 5 && std::string(rows[0].name) == "g_fBSSSwtMax" && std::string(rows[1].name) == "g_f4MuscleParam" && std::string(rows[2].name) == "g_fMdlAlpha" && (rows[0].flags & kPoNameRead), "params.names",
          ok && rows.size() == 5 ? ("name0 '" + std::string(rows[0].name) + "'") : "no rows");
    auto vf = [](const ParamOverrideRow& r, int k) { float f; std::memcpy(&f, &r.value[k], 4); return f; };
    Check(ok && rows.size() == 5 && Near(vf(rows[0], 0), 7.f) && Near(vf(rows[1], 15), 7.f + 10.f + 15.f) && Near(vf(rows[2], 3), 7.f + 20.f + 3.f) && Near(vf(rows[3], 0), 37.f) && (rows[3].flags & kPoValueRead) && !(rows[4].flags & kPoValueRead) && rows[4].value[0] == 0, "params.values");
    Check(ok && rows.size() == 5 && Near(vf(rows[0], 0), 7.f) && vf(rows[0], 0) < 900.f && Near(vf(rows[3], 0), 37.f) && rows[3].value[1] == 0, "params.live_source", "the SOURCE value (7.x), never slot 0 (999.x); a 4-byte size reads 1 word, value[1] stays 0 (a 4 * count reader would take 4)");
    Check(ok && rows.size() == 5 && std::string(rows[2].name) == "g_fMdlAlpha" && std::string(rows[3].name) == "g_f4SpecialPara" && std::string(rows[4].name) == "g_fTime", "params.parts", "part 1 holds MdlAlpha, part 2 holds SpecialPara / Time");
    {   // no block: nothing to override, not a failure; an unreadable table: failure, and the rows of that model are dropped
      std::vector<ParamOverrideRow> r2;
      scene_inputs::ParamReadInfo p2;
      const bool nb = scene_inputs::ReadParamOverrides(ReadThunk, kM1, 601, r2, p2);   // kM1 has no [m+832] in the fake guest: unreadable, a FAILURE (the fake has no word there)
      g_guest.Put(kM1 + 832, 0);
      const bool nb0 = scene_inputs::ReadParamOverrides(ReadThunk, kM1, 601, r2, p2) && p2.no_block && r2.empty();
      r2 = rows;
      Guest keep = g_guest;
      g_guest.words.erase(kCnt + 8);   // part 2's count unreadable
      const bool bad = !scene_inputs::ReadParamOverrides(ReadThunk, kM0, 602, r2, p2) && r2.size() == rows.size();   // the failing model's rows are dropped, the earlier ones kept
      g_guest = keep;
      g_guest.Put(kB + 1136, 9999);
      const bool insane = !scene_inputs::ReadParamOverrides(ReadThunk, kM0, 602, r2, p2);
      g_guest = keep;
      g_guest.Put(kB + 1144, Plant("param_half_null") ? 0u : 0u);   // counts array null while the tables array is set: a half-null part table
      std::vector<ParamOverrideRow> r3;
      const bool half = Plant("param_half_null") ? false : !scene_inputs::ReadParamOverrides(ReadThunk, kM0, 603, r3, p2);
      g_guest = keep;
      Check(half, "params.half_null", "tables set, counts null: refused");
      Check(!nb && nb0 && bad && insane, "params.failures", "unreadable fails / null block ok / one bad table drops that model's rows / count 9999 refused");
    }
    {   // the contributor: a live pointer that cannot be read is NO stamp; fixed, it stamps in the hook clock; the model list is the LIVE targets
      g_guest.Put(kM1 + 832, 0);
      Guest keep = g_guest;
      ResetStamps();
      live::inputs::InputsContributor pc;
      pc.SetReader(ReadThunk);
      pc.SetModels(ModelsThunk);
      FrameInfo fp;
      fp.guest_frame = 700;
      pc.Latch(fp);
      SceneTables tp;
      Counters cp;
      pc.Emit(fp, tp, cp);
      const uint64_t unread_stamp = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInMaterialParams].load();
      Check(tp.param_overrides.size() == 5 && unread_stamp != 699, "params.unreadable_value_no_stamp", "rows " + std::to_string(tp.param_overrides.size()) + " stamp " + std::to_string(unread_stamp));
      g_guest.Put(kP2 + 20 * 1 + (Plant("param_read_slot0") ? 0 : 4), kVal + 0x300);   // the last entry's live (source) pointer made readable
      ResetStamps();
      live::inputs::InputsContributor pc2;
      pc2.SetReader(ReadThunk);
      pc2.SetModels(ModelsThunk);
      FrameInfo fq;
      fq.guest_frame = 700;
      pc2.Latch(fq);
      SceneTables tq;
      pc2.Emit(fq, tq, cp);
      const bool stamped = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInMaterialParams].load() == 699;
      Check(tq.param_overrides.size() == 5 && stamped, "params.contrib", "rows " + std::to_string(tq.param_overrides.size()) + " stamp " + std::to_string(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInMaterialParams].load()));
      {   // liveness: the model list holds a dead model (kM1 with tables); the live targets hold only kM0
        g_guest.Put(kM1 + 832, kB), g_guest.Put(kM1 + 372, 0);   // kM1 shares kB's wide table
        live::inputs::InputsContributor pc3;
        pc3.SetReader(ReadThunk);
        pc3.SetModels(ModelsThunk);
        if (!Plant("param_models_list"))
          pc3.SetPalettes([](uint64_t, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) { out = {{kM0, 3}}; missing = 0; }, [](uint32_t, uint32_t, const Mat4*, uint64_t) { return 0; });
        FrameInfo f3;
        f3.guest_frame = 710;
        pc3.Latch(f3);
        SceneTables t3;
        pc3.Emit(f3, t3, cp);
        bool only0 = !t3.param_overrides.empty();
        for (const auto& r : t3.param_overrides) only0 = only0 && r.model == kM0;
        Check(only0, "params.liveness_list", "rows " + std::to_string(t3.param_overrides.size()));
        g_guest.Put(kM1 + 832, 0);
      }
      tq.light_objects.clear();
      std::vector<uint8_t> by = SerializeRecord(tq);
      SceneTables back;
      std::string er;
      const bool parsed = ParseRecord(by.data(), by.size(), back, er);
      Check(parsed && back.param_overrides.size() == 5 && back.param_errors == 0 && std::memcmp(back.param_overrides.data(), tq.param_overrides.data(), 5 * sizeof(ParamOverrideRow)) == 0 && SerializeRecord(back) == by, "params.roundtrip", er);
      if (tq.param_overrides.size() > 1) tq.param_overrides[1].part = 77;   // not a sane part: the section drops with its OWN counter, lane A's tables stay
      by = SerializeRecord(tq);
      SceneTables bad;
      const bool b = ParseRecord(by.data(), by.size(), bad, er) && bad.param_overrides.empty() && bad.param_errors == 1 && bad.inputs_errors == 0;
      Check(b, "params.own_error_path", er + " perr " + std::to_string(bad.param_errors) + " rows " + std::to_string(bad.param_overrides.size()) + " ierr " + std::to_string(bad.inputs_errors));
      (void)keep;
    }
    BuildGuest();
  }

  // ---------------------------------------------------------------- 3a4. S7 (S103 A2): the plane shadow module's per-frame constants at the latch
  {
    constexpr uint32_t kMod = 0x40070000, kHolder = 0x40071000;
    g_guest.Put(scene_inputs::kPlaneModulePtr, kMod);
    g_guest.Put(kMod + scene_inputs::kPlEnabledOff, 1);               // the task's own test: [module+40] != 0
    g_guest.Put(scene_inputs::kPlSuppressWord, 0);                     // word 0x836F39A0 == 0
    g_guest.Put(scene_inputs::kPlCamHolder, kHolder), g_guest.Put(kHolder + scene_inputs::kPlCamHolderOff, 0x40072000);   // [[0x83929A68]+256] != 0
    const uint32_t cam_off = Plant("plane_swap") ? scene_inputs::kPlFitOff : scene_inputs::kPlCameraOff, fit_off = Plant("plane_swap") ? scene_inputs::kPlCameraOff : scene_inputs::kPlFitOff;
    for (uint32_t i = 0; i < 10; ++i) g_guest.PutF(kMod + 68 + 4 * i, 500.f - 50.f * float(i));
    for (uint32_t i = 0; i < 16; ++i) g_guest.PutF(kMod + cam_off + 4 * i, 1000.f + float(i)), g_guest.PutF(kMod + fit_off + 4 * i, 2000.f + float(i));
    g_guest.PutF(kMod + 64, -1.f), g_guest.PutF(kMod + 108, -2.f);   // decoys just outside the fade array
    PlaneFrame pf;
    const bool ok = scene_inputs::ReadPlaneFrame(ReadThunk, 799, pf) == scene_inputs::PlaneWhy::Ok;
    Check(ok && pf.frame == 799 && pf.have == kPlaneHaveAll && Near(pf.fade[0], 500.f) && Near(pf.fade[9], 50.f) && Near(pf.plane_normal[1], -1.f), "plane.fade", ok ? ("fade0 " + std::to_string(pf.fade[0]) + " fade9 " + std::to_string(pf.fade[9])) : "unreadable");
    Check(ok && Near(pf.camera[0], 1000.f) && Near(pf.camera[15], 1015.f) && Near(pf.fit_matrix[0], 2000.f) && Near(pf.fit_matrix[15], 2015.f), "plane.matrices", ok ? ("camera0 " + std::to_string(pf.camera[0]) + " fit0 " + std::to_string(pf.fit_matrix[0])) : "unreadable");
    {   // refusals: each of the game's own conditions, apart (a stale module is never a current frame, review 2)
      using W = scene_inputs::PlaneWhy;
      Guest keep = g_guest;
      PlaneFrame p2;
      g_guest.Put(scene_inputs::kPlaneModulePtr, 0);
      const bool nul = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == W::NoModule;
      g_guest = keep;
      g_guest.Put(kMod + scene_inputs::kPlEnabledOff, 0);
      const bool dis = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == (Plant("plane_no_gate") ? W::Ok : W::Disabled);
      g_guest = keep;
      g_guest.Put(scene_inputs::kPlSuppressWord, 1);
      const bool sup = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == W::Suppressed;
      g_guest = keep;
      g_guest.Put(kHolder + scene_inputs::kPlCamHolderOff, 0);
      const bool nocam = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == W::NoCamera;
      g_guest = keep;
      g_guest.words.erase(kMod + 1312 + 60);
      const bool cut = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == W::ReadFail;
      g_guest = keep;
      g_guest.PutF(kMod + 68, std::nanf(""));
      const bool nan = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == W::NonFiniteFade;
      g_guest = keep;
      Check(nul && cut && nan, "plane.refusals", "null module / a block cut short / a NaN");
      {   // S103 CE (bit 9): the old `Unreadable` split: each reason counts apart; the first bad block is handed back for the dev log. The twin counts a NaN in the fit as a failed read.
        scene_inputs::PlaneBadBlock bad;
        g_guest.PutF(kMod + scene_inputs::kPlFitOff, std::nanf(""));
        const W wf = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2, &bad);
        Check(wf == (Plant("ce_fit_as_readfail") ? W::ReadFail : W::NonFiniteFit) && bad.n == 16 && Near(bad.v[1], 2001.f), "ce.plane_nan_fit_counts_nonfinite_fit", "NaN at +1312: NonFiniteFit with the fit block");
        g_guest = keep;
        g_guest.PutF(kMod + scene_inputs::kPlCameraOff + 4 * 5, std::nanf(""));
        const bool nc = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == W::NonFiniteCamera;
        g_guest = keep;
        g_guest.Put(kHolder - 0 + 0, 0);   // (decoy: the holder word itself is at 0x83929A68)
        g_guest.Put(scene_inputs::kPlCamHolder, 0x100);
        const bool hn = scene_inputs::ReadPlaneFrame(ReadThunk, 1, p2) == W::HolderNull;
        g_guest = keep;
        Check(nc && hn, "ce.plane_other_reasons", "NaN in the camera block: NonFiniteCamera; holder < 0x10000: HolderNull");
        {   // S103 PB: the game's x-collapse (sub_827ED960 divides 2 / 0) = "no plane draw this frame": stamped with kPlaneNoDraw; any other non-finite fit stays withheld
          constexpr float kInf = std::numeric_limits<float>::infinity();
          const float run36[16] = {kInf, 0.f, 0.f, kInf, 0.f, 2.99299f, 0.f, 1.99299f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};   // run 36's dump row for row
          auto put_fit = [&](const float* v) { for (uint32_t i = 0; i < 16; ++i) g_guest.PutF(kMod + scene_inputs::kPlFitOff + 4 * i, v[i]); };
          put_fit(run36);
          PlaneFrame o;
          W r = scene_inputs::ReadPlaneFrame(ReadThunk, 5365, o);
          if (Plant("pb_offscreen_withheld")) r = W::NonFiniteFit;
          Check(r == W::OffScreenFit && (o.flags & kPlaneNoDraw) && o.have == (kPlaneHaveAll & ~kPlaneHaveFit) && std::isinf(o.fit_matrix[0]) && std::isinf(o.fit_matrix[3]) && o.frame == 5365, "pb.offscreen_fit_stamps",
                "run 36's dump (inf 0 0 inf / 0 2.99299 0 1.99299 / identity): OffScreenFit, kPlaneNoDraw, no kPlaneHaveFit");
          float left[16];
          std::memcpy(left, run36, sizeof left);
          left[3] = -kInf;
          put_fit(left);
          PlaneFrame ol;
          W rl = scene_inputs::ReadPlaneFrame(ReadThunk, 1, ol);
          Check(rl == W::OffScreenFit && (ol.flags & kPlaneNoDraw), "pb.offscreen_left", "row 0 = (inf, 0, 0, -inf): the left side collapses the same way");
          float nan_y[16], nan_x1[16], inf_x_only[16];
          std::memcpy(nan_y, run36, sizeof nan_y), std::memcpy(nan_x1, run36, sizeof nan_x1), std::memcpy(inf_x_only, run36, sizeof inf_x_only);
          nan_y[5] = std::nanf(""), nan_x1[1] = std::nanf(""), inf_x_only[3] = 0.f;
          bool withheld = true;
          for (const float* v : {nan_y, nan_x1, inf_x_only}) {
            put_fit(v);
            PlaneFrame on;
            W rn = scene_inputs::ReadPlaneFrame(ReadThunk, 1, on);
            if (Plant("pb_any_nonfinite_nodraw")) rn = W::OffScreenFit;
            if (rn != W::NonFiniteFit || (on.flags & kPlaneNoDraw)) withheld = false;
          }
          Check(withheld, "pb.other_nonfinite_withheld", "NaN at fit[5], NaN at fit[1], inf at fit[0] alone: NonFiniteFit, never a no-draw frame");
          g_guest = keep;
        }
      }
      Check(dis && sup && nocam, "plane.stale_refused", "[module+40] == 0 / word 0x836F39A0 != 0 / [[0x83929A68]+256] == 0 each refuse");
    }
    {   // the live emit order: inputs registered first, so ITS Emit runs before the plane contributor's. One frame per stamp; the walk's block wins; equal / differs counted (review 1)
      FrameInfo fp;
      fp.guest_frame = 800;
      live::inputs::InputsContributor pc;
      pc.SetReader(ReadThunk);
      pc.Latch(fp);
      SceneTables tp;
      Counters cp;
      pc.Emit(fp, tp, cp);
      Check(tp.plane_frames.size() == 1 && tp.plane_frames[0].frame == 800, "plane.latch_frame", "frames " + std::to_string(tp.plane_frames.size()));
      live::plane::PlaneContributor plane;
      plane.Latch(fp);
      PlaneFrame walk = tp.plane_frames[0];
      walk.flags = 77;   // marks the walk's block
      plane.TestInject(walk);
      plane.Emit(fp, tp, cp);
      if (Plant("plane_dup")) tp.plane_frames.push_back(walk);
      Check(tp.plane_frames.size() == 1 && tp.plane_frames[0].flags == 77 && plane.LatchSame() == 1 && plane.LatchDiffers() == 0, "plane.no_duplicate",
            "frames " + std::to_string(tp.plane_frames.size()) + " same " + std::to_string((unsigned long long)plane.LatchSame()));
      // a walk block that differs from the latch's: counted, the walk's kept
      SceneTables t2;
      pc.Latch(fp);
      pc.Emit(fp, t2, cp);
      live::plane::PlaneContributor plane2;
      plane2.Latch(fp);
      if (t2.plane_frames.empty()) { Check(false, "plane.second_emit_empty", "mode " + std::to_string(g_mode) + " stamps " + std::to_string(t2.input_stamps.size()) + " post " + std::to_string(t2.post_params.size())); return 1; }
      PlaneFrame w2 = t2.plane_frames[0];
      w2.camera[3] += 1.f, w2.flags = 78;
      plane2.TestInject(w2);
      plane2.Emit(fp, t2, cp);
      Check(t2.plane_frames.size() == 1 && t2.plane_frames[0].flags == 78 && plane2.LatchDiffers() == 1 && plane2.LatchSame() == 0, "plane.differs_counted", "differs " + std::to_string((unsigned long long)plane2.LatchDiffers()));
      // a walk frame with another stamp appends
      SceneTables t3;
      pc.Latch(fp);
      pc.Emit(fp, t3, cp);
      live::plane::PlaneContributor plane3;
      plane3.Latch(fp);
      PlaneFrame w3 = t3.plane_frames[0];
      w3.frame = 790;
      plane3.TestInject(w3);
      plane3.Emit(fp, t3, cp);
      {   // S103 PB: a latch block that is the game's no-draw frame keeps its flag when the walk's block (raw inf, have All) replaces it
        SceneTables tn;
        PlaneFrame lat = tp.plane_frames[0];
        lat.flags |= kPlaneNoDraw, lat.have = kPlaneHaveAll & ~kPlaneHaveFit;
        lat.fit_matrix[0] = lat.fit_matrix[3] = std::numeric_limits<float>::infinity();
        tn.plane_frames.push_back(lat);
        live::plane::PlaneContributor planen;
        planen.Latch(fp);
        PlaneFrame wn = lat;
        wn.flags = 0, wn.have = kPlaneHaveAll;   // the walk recorder knows no no-draw
        planen.TestInject(wn);
        planen.Emit(fp, tn, cp);
        if (Plant("pb_merge_drops_nodraw")) tn.plane_frames[0].flags &= ~uint32_t(kPlaneNoDraw);
        Check(tn.plane_frames.size() == 1 && (tn.plane_frames[0].flags & kPlaneNoDraw) && !(tn.plane_frames[0].have & kPlaneHaveFit) && planen.LatchSame() == 1, "pb.merge_keeps_nodraw",
              "frames " + std::to_string(tn.plane_frames.size()) + " flags " + std::to_string(tn.plane_frames.empty() ? 0u : tn.plane_frames[0].flags) + " same " + std::to_string((unsigned long long)planen.LatchSame()));
      }
      {   // S103 PB: BuildE2Input over a no-draw frame: no complete block, but not a hole (no missing line), plane_nodraw set; RecordPlane's early return is linted in inputs_check.py
        SceneTables te;
        te.header.guest_frame = 111;
        PlaneFrame nd = tp.plane_frames[0];
        nd.frame = 110, nd.flags |= kPlaneNoDraw, nd.have = kPlaneHaveAll & ~kPlaneHaveFit;
        nd.fit_matrix[0] = nd.fit_matrix[3] = std::numeric_limits<float>::infinity();
        te.plane_frames.push_back(nd);
        rdr_scene::E2Input in;
        BuildE2Input(te, rdr_scene::E2Faults(), in);
        bool listed = false;
        for (uint32_t i = 0; i < in.plane_missing.n; ++i) listed = listed || std::strstr(in.plane_missing.v[i], "no complete finite block") != nullptr;
        if (Plant("pb_e2_nodraw_hole")) listed = true;
        Check(!in.have_plane && in.plane_nodraw && !listed, "pb.e2_nodraw_no_hole", std::string("have_plane ") + (in.have_plane ? "1" : "0") + " nodraw " + (in.plane_nodraw ? "1" : "0") + " missing-line " + (listed ? "1" : "0"));
      }
      Check(t3.plane_frames.size() == 2, "plane.other_stamp_appends", "frames " + std::to_string(t3.plane_frames.size()));
      std::vector<uint8_t> by = SerializeRecord(tp);
      SceneTables back;
      std::string er;
      Check(ParseRecord(by.data(), by.size(), back, er) && back.plane_frames.size() == 1 && std::memcmp(&back.plane_frames[0], &tp.plane_frames[0], sizeof(PlaneFrame)) == 0, "plane.roundtrip", er);
    }
    BuildGuest();
  }

  // ---------------------------------------------------------------- 3a4b. S103 PLF: why the game built no plane module: the gate bytes G+18 / +19 / +24 and the participant count (SHD.md PLB)
  {
    constexpr uint32_t kP = 0x40090000, G = scene_inputs::kPlGlobal;
    const bool wrong = Plant("plf_wrong_off");   // the planted guest keeps the structure byte at G+18 instead of G+19: a reader that takes it from the wrong offset prints a different structure
    g_guest.Put(G + 16, wrong ? 0x00000100u : 0x00000001u);   // bytes G+16..G+19 (big-endian word): G+19 = 1 (the planted twin: G+18 = 1)
    g_guest.Put(G + 24, 0);                                      // G+24 .. G+27: G+24 = 0
    g_guest.Put(G + scene_inputs::kPlOffMode, 3), g_guest.Put(scene_inputs::kPlNetWord, 0);   // the slot-loop path of sub_827E3520
    g_guest.Put(scene_inputs::kPlPartPtr, kP);
    const uint32_t ids[6] = {5, 9, 0xC800, 7, 0xC800, 0xC800}, st[6] = {0, 2, 0, 1, 3, 0};   // occupied + status 0 / 2 count (slots 0, 1); slot 3 has status 1 (not counted); empty ids never count
    for (uint32_t i = 0; i < 6; ++i)
      g_guest.Put(kP + 148 + 56 * i + 16, ids[i]), g_guest.Put(kP + 148 + 56 * i + 8, st[i]);
    scene_inputs::PlaneStructure ps;
    const bool rd = scene_inputs::ReadPlaneStructure(ReadThunk, ps);
    const std::string txt = rd ? scene_inputs::PlaneStructureText(ps) : "unreadable";
    Check(rd && txt == "structure 0/1/0, participants 2", "plf.structure_and_participants", txt);
    {   // the NoModule latch that triggers the read, and the other-branch tag
      Guest keep = g_guest;
      g_guest.Put(scene_inputs::kPlaneModulePtr, 0);
      PlaneFrame pf;
      const bool nm = scene_inputs::ReadPlaneFrame(ReadThunk, 5, pf) == scene_inputs::PlaneWhy::NoModule;
      g_guest.Put(G + scene_inputs::kPlOffMode, 0);
      scene_inputs::PlaneStructure p2;
      const bool rd2 = scene_inputs::ReadPlaneStructure(ReadThunk, p2);
      Check(nm && rd2 && !p2.loop_path && scene_inputs::PlaneStructureText(p2).find("unconfirmed") != std::string::npos, "plf.other_branch_flagged", rd2 ? scene_inputs::PlaneStructureText(p2) : "unreadable");
      {   // S103 MBC review 1: the count is KNOWN on the other branch too (a match runs the loop there); only the online add-on makes it unknown. A planted reader that used loop_path would fail the first Check.
        g_guest.Put(scene_inputs::kPlOnlineWord, 0);
        scene_inputs::PlaneStructure q0;
        const bool r0 = scene_inputs::ReadPlaneStructure(ReadThunk, q0);
        g_guest.Put(scene_inputs::kPlOnlineWord, 1);
        scene_inputs::PlaneStructure q1;
        const bool r1 = scene_inputs::ReadPlaneStructure(ReadThunk, q1);
        Check(r0 && q0.count_known && !q0.loop_path && q0.participants == 2 && r1 && !q1.count_known, "plf.count_known_other_branch", "mode != 3: known when [0x83929894] == 0, unknown when the online add-on applies");
      }
      g_guest = keep;
      g_guest.words.erase(kP + 148 + 56 * 5 + 8);
      scene_inputs::PlaneStructure p3;
      Check(!scene_inputs::ReadPlaneStructure(ReadThunk, p3), "plf.unreadable_slot_refused", "a slot word cut: no census, never a guess");
      g_guest = keep;
    }
    BuildGuest();
  }

  // ---------------------------------------------------------------- 3a5. S103 A3: W1 on the record path with no latch, the arena object list + input, the clock under OURS, the circle points (S6)
  {
    // ---- W1 with no latch (run 9: "latched 0"): the wrestlers contributor's Emit must run the palette read itself
    {
      constexpr uint32_t kPal = 0x40005000, kCount = 3;
      for (uint32_t i = 0; i < kCount; ++i)
        for (uint32_t j = 0; j < 16; ++j) g_guest.PutF(kPal + 64 * i + 4 * j, 100.f * i + j + 0.25f);
      g_guest.Put(kM0 + scene_inputs::kPaletteOff, kPal);
      static live::WrestlerContributor* W;
      static live::inputs::InputsContributor* PC;
      live::WrestlerContributor w;
      W = &w;
      live::inputs::InputsContributor pc;
      PC = &pc;
      pc.SetReader(ReadThunk);
      pc.SetPalettes([](uint64_t hook, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) {
                       std::vector<live::WrestlerContributor::PaletteTarget> t;
                       W->LatchTargets(hook, t, missing);
                       out.clear();
                       for (const auto& x : t) out.push_back({x.skel, x.count});
                     },
                     [](uint32_t m, uint32_t n, const Mat4* skin, uint64_t f) { return W->SubmitLatchConsumer(m, n, skin, f); });
      if (!Plant("a3_no_prelatch")) w.SetPreLatch([](uint64_t rf, bool lt) { PC->LatchPalettes(rf, lt); });
      std::vector<Mat4> derived(kCount);
      for (size_t di = 0; di < derived.size(); ++di) for (int dj = 0; dj < 16; ++dj) derived[di].m[dj] = 100.f * float(di) + float(dj) + 0.25f;   // = the guest block: nodes 0-58 agree (audit item 2)
      ResetStamps();
      for (uint64_t r = 900; r < 904; ++r) w.SubmitSkin(kM0, 0, kCount, derived.data(), r - 1);
      FrameInfo fi;
      fi.guest_frame = 904;
      SceneTables t;
      Counters c;
      w.Emit(fi, t, c);   // NO Latch: the record path of run 9
      const uint64_t st = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInWrestlerPalette].load();
      Check(w.ConsumerSubmits() >= 1 && st == 903, "a3.palette_no_latch", "submits " + std::to_string((unsigned long long)w.ConsumerSubmits()) + " stamp " + std::to_string((unsigned long long)st));
    }
    // ---- the arena object list (the stage object's slot models), its MdlAlpha / override rows (bit 16), and the arena input (bit 5, stamped by stage_fx's own take: audit item 1)
    {
      constexpr uint32_t kStage = 0x40080000, kA0 = 0x40081000, kA1 = 0x40082000, kB = 0x40083000, kEnt = 0x40084000, kNm = 0x40085000, kVal = 0x40086000;
      const uint32_t at = Plant("a3_slot_off") ? 6168 : 6164;
      for (uint32_t i = 0; i < 399; ++i) g_guest.Put(kStage + at + 4 * i, i == 3 ? kA0 : i == 77 ? kA1 : 0u);
      g_guest.PutF(kA0 + 252, 0.5f), g_guest.PutF(kA1 + 252, 0.25f);
      g_guest.Put(kA0 + 832, 0);   // no override block
      g_guest.Put(kA1 + 832, kB), g_guest.Put(kA1 + 372, 0);   // one model-wide override entry, no parts
      g_guest.Put(kB + 1132, kEnt), g_guest.Put(kB + 1136, 1), g_guest.Put(kB + 1140, 0), g_guest.Put(kB + 1144, 0);
      g_guest.Put(kEnt + 0, kVal), g_guest.Put(kEnt + 4, kVal), g_guest.Put(kEnt + 8, kNm), g_guest.Put(kEnt + 12, 64), g_guest.Put(kEnt + 16, 16);   // WRK: word 1 = the source pointer the reader follows, word 4 = the count
      { const char* nm = "g_fArenaTest"; uint8_t b2[16] = {}; std::strncpy(reinterpret_cast<char*>(b2), nm, 15);
        for (int w = 0; w < 4; ++w) g_guest.Put(kNm + 4 * w, (uint32_t(b2[4 * w]) << 24) | (uint32_t(b2[4 * w + 1]) << 16) | (uint32_t(b2[4 * w + 2]) << 8) | b2[4 * w + 3]); }
      for (uint32_t k = 0; k < 16; ++k) g_guest.PutF(kVal + 4 * k, 3.f + float(k));
      static live::stagefx::StageFxContributor* SF;
      live::stagefx::StageFxContributor sf;
      SF = &sf;
      sf.SetReader(ReadThunk);
      sf.SetStage(kStage);
      sf.SetStamp([](uint64_t rf) { scene_inputs::Capture(scene_inputs::kInArenaWorlds, scene_inputs::HookFrameOfRecord(rf)); });
      std::vector<uint32_t> am;
      const bool ok = sf.StageModels(am);
      Check(ok && am.size() == 2 && am[0] == kA0 && am[1] == kA1, "a3.arena_models", ok ? ("models " + std::to_string(am.size())) : "unreadable");
      // bit 5 is stamped by the stage take OF THE SAME FRAME (inputs latch first, so a stamp made from the inputs side would be a frame old)
      ResetStamps();
      live::stagefx::StageFxContributor::Frame sfr;
      sf.Take(1001, sfr);
      const uint64_t same = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInArenaWorlds].load();
      Check(same == (Plant("a3_stamp_lag") ? 999u : 1000u), "a3.arena_same_frame", "stamp " + std::to_string((unsigned long long)same));
      // a dirty take (the slot table is not published for the arena) stamps nothing
      ResetStamps();
      sf.SetSlotsReady([]() { return false; });
      sf.Take(1002, sfr);
      Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInArenaWorlds].load() != 1001, "a3.arena_ready_dirty");
      sf.SetSlotsReady(nullptr);
      // the contributor reads the arena models (bit 16) from the same list
      ResetStamps();
      live::inputs::InputsContributor pc2;
      pc2.SetReader(ReadThunk);
      pc2.SetArena([](std::vector<uint32_t>& out) { return SF->StageModels(out); });
      FrameInfo fq;
      fq.guest_frame = 1001;
      pc2.Latch(fq);
      SceneTables tq;
      Counters cq;
      pc2.Emit(fq, tq, cq);
      bool a0 = false, a1 = false;
      for (const auto& r : tq.model_runtime) a0 = a0 || (r.object == kA0 && Near(r.alpha, 0.5f)), a1 = a1 || (r.object == kA1 && Near(r.alpha, 0.25f));
      Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInArenaModels].load() == 1000 && a0 && a1, "a3.arena_alpha_stamp", "alpha rows " + std::to_string(tq.model_runtime.size()));
      bool pr = false;
      for (const auto& r : tq.param_overrides) pr = pr || (r.model == kA1 && std::string(r.name) == "g_fArenaTest" && Near(*reinterpret_cast<const float*>(&r.value[1]), 4.f));
      Check(pr, "a3.arena_params", "param rows " + std::to_string(tq.param_overrides.size()));
      // audit item 1: the arena clock row (kAwClock) stays out of OURS frames: with no hook-fed buffer closed the contributor writes no entry (c186 is never frame - origin: J1)
      live::arenaworld::WorldContributor w2;
      w2.tracker.OnStageSetup(500);
      SceneTables tc;
      Counters cc;
      FrameInfo fc;
      fc.guest_frame = 521;
      w2.Emit(fc, tc, cc);
      Check(tc.arena_worlds.empty(), "a3.no_clock_row_under_ours", "entries " + std::to_string(tc.arena_worlds.size()));
    }
    // ---- S6: the circle shadows' body points
    {
      constexpr uint32_t kCh0 = 0x40090000, kCh1 = 0x40091000, kCh2 = 0x40092000, kComp0 = 0x40093000, kComp1 = 0x40094000, kComp0Decoy = 0x40095000, kSk0 = 0x40096000, kSk1 = 0x40097000, kW0 = 0x40098000, kW1 = 0x400A0000;
      constexpr uint32_t kHead = kCh1 + 1152, kN1 = 0x40099000, kN2 = 0x4009A000;
      for (uint32_t i = 0; i < 9; ++i) g_guest.Put(scene_inputs::kCircleSlotTable + 4 * i, i == 0 ? kCh0 : i == 1 ? kCh1 : i == 4 ? kCh2 : 0u);
      for (uint32_t ch : {kCh0, kCh1, kCh2}) g_guest.Put(ch + 1208, 1), g_guest.Put(ch + 224, 1), g_guest.Put(ch + 232, 1);
      g_guest.Put(kCh2 + 1208, 0);   // gated (the game skips it)
      g_guest.Put(kCh0 + 48, kSk0), g_guest.Put(kCh1 + 48, kSk1);
      g_guest.Put(kSk0 + 192, kW0), g_guest.Put(kSk1 + 192, kW1);
      // ch0: the component is cached at [char+3112]; ch1: found in the tagged list (head -> N1 (other tag) -> N2 (the tag), component = [N2+24])
      g_guest.Put(kCh0 + 3112, Plant("a3_cache_off") ? 0u : kComp0), g_guest.Put(kCh1 + 3112, 0);
      g_guest.Put(kCh0 + 1152 + 20, Plant("a3_cache_off") ? 0x400A8000u : 0u);   // the planted list holds a decoy chain
      g_guest.Put(kHead + 20, kN1), g_guest.Put(kHead + 4, 0x11111111), g_guest.Put(kN1 + 20, kN2), g_guest.Put(kN1 + 4, 0x22222222), g_guest.Put(kN2 + 4, scene_inputs::kCompTag), g_guest.Put(kN2 + 24, kComp1), g_guest.Put(kN2 + 20, 0);
      g_guest.Put(0x400A8000 + 4, scene_inputs::kCompTag), g_guest.Put(0x400A8000 + 24, kComp0Decoy), g_guest.Put(0x400A8000 + 20, 0);
      auto put_idx = [&](uint32_t comp, int base) {   // 21 big-endian int16 at comp+40: base + k, with k == 4 set to -1 (unset)
        for (uint32_t w = 0; w < 11; ++w) {
          auto h = [&](uint32_t k) -> uint16_t { return k >= 21 ? 0 : (k == 4 ? uint16_t(int16_t(-1)) : uint16_t(base + int(k))); };
          g_guest.Put(comp + 40 + 4 * w, (uint32_t(h(2 * w)) << 16) | h(2 * w + 1));
        }
      };
      put_idx(kComp0, 0), put_idx(kComp1, 5), put_idx(kComp0Decoy, 9);
      for (uint32_t m = 0; m < 40; ++m)
        for (uint32_t c = 0; c < 4; ++c) g_guest.PutF(kW0 + 64 * m + 48 + 4 * c, 1000.f * float(m) + float(c)), g_guest.PutF(kW1 + 64 * m + 48 + 4 * c, 5000.f + 1000.f * float(m) + float(c));
      std::vector<scene_inputs::CirclePoints> cps;
      scene_inputs::CircleReadInfo ci;
      const bool ok = scene_inputs::ReadCirclePoints(ReadThunk, cps, ci);
      Check(ok && cps.size() == 2 && ci.chars == 2 && ci.gated == 1 && ci.empty_slots == 6 && cps[0].character == kCh0 && cps[1].character == kCh1 && cps[1].slot == 1, "a3.circle_chars",
            ok ? ("chars " + std::to_string(cps.size()) + " gated " + std::to_string(ci.gated)) : "slot table unreadable");
      Check(ok && cps.size() == 2 && Near(cps[0].points[0][0], 0.f) && Near(cps[0].points[7][0], 7000.f) && Near(cps[0].points[7][2], 7002.f) && Near(cps[1].points[0][0], 5000.f + 5000.f) && Near(cps[1].points[20][0], 5000.f + 1000.f * 25.f), "a3.circle_cache",
            ok && cps.size() == 2 ? ("p0.x " + std::to_string(cps[0].points[0][0]) + " p7.x " + std::to_string(cps[0].points[7][0])) : "no chars");
      Check(ok && cps.size() == 2 && !(cps[0].set_mask & (1u << 4)) && (cps[0].set_mask & (1u << 5)) && ci.unset == 2 && ci.points == 40, "a3.circle_unset", "unset " + std::to_string(ci.unset) + " points " + std::to_string(ci.points));
      {   // an unreadable slot table fails; a character without a component is counted, not read
        Guest keep = g_guest;
        g_guest.words.erase(scene_inputs::kCircleSlotTable + 8);
        const bool bad = !scene_inputs::ReadCirclePoints(ReadThunk, cps, ci);
        g_guest = keep;
        g_guest.Put(kCh1 + 3112, 0), g_guest.Put(kHead + 20, 0), g_guest.Put(kHead + 4, 0), g_guest.Put(kHead + 16, 0);
        const bool ok2 = scene_inputs::ReadCirclePoints(ReadThunk, cps, ci);
        Check(bad && ok2 && ci.no_component == 1 && ci.chars == 1, "a3.circle_refusals", "no_component " + std::to_string(ci.no_component));
        g_guest = keep;
      }
    }
    // ---- S6 complete: ShadowParam (c0), ViewProj (c4-c7), the ground height, the merge with the walk's rows, the stamp
    {
      constexpr uint32_t kMod = 0x400B0000, kCirc = 0x400B8000, kCh = 0x400C0000, kComp = 0x400C1000, kSk = 0x400C2000, kW = 0x400C3000, kTab = 0x400D0000, kGateObj = 0x400D8000;
      auto F = [&](uint32_t a, float v) { g_guest.PutF(a, v); };
      F(scene_inputs::kGhZero, 0.f), F(scene_inputs::kGhOne, 1.f), F(scene_inputs::kGhRingHalf, 100.f);
      F(scene_inputs::kGhC0, 400.f), F(scene_inputs::kGhC1, 100.f), F(scene_inputs::kGhC2, 7.f), F(scene_inputs::kGhC3, 900.f), F(scene_inputs::kGhC4, 300.f), F(scene_inputs::kGhC5, 11.f), F(scene_inputs::kGhC6, 50.f);
      const bool sw = Plant("a3_ground_flag");
      F(scene_inputs::kGhRingTable, sw ? 2.5f : 1.5f), F(scene_inputs::kGhRingTable + 4, sw ? 1.5f : 2.5f);   // flag != 0 -> table[0] = 1.5; flag == 0 -> table[1] = 2.5
      g_guest.Put(scene_inputs::kGhRingFlag, 1);
      g_guest.Put(scene_inputs::kGhGateObj, 0);
      g_guest.Put(scene_inputs::kGhModeObj + scene_inputs::kGhModeOff, 5);
      g_guest.Put(scene_inputs::kGhTabPtr, kTab), g_guest.Put(scene_inputs::kGhTabCount, 3);
      for (uint32_t i = 0; i < 3; ++i) {   // ids 4, 5, 6 ; classes 1, 0, 2
        for (uint32_t w = 0; w < 27; ++w) g_guest.Put(kTab + 108 * i + 4 * w, 0);
        g_guest.Put(kTab + 108 * i + 4, (4 + i) << 24);
        g_guest.Put(kTab + 108 * i + 28, i == 0 ? 1u : i == 1 ? 0u : 2u);
      }
      float y = -1;
      auto ground = [&](float x, float z) { g_guest.PutF(kCh + 160, x), g_guest.PutF(kCh + 168, z); return scene_inputs::GroundHeight(ReadThunk, kCh, y) ? y : -999.f; };
      Check(Near(ground(10.f, -20.f), 1.5f), "a3.ground_ring", "flag 1 -> " + std::to_string(y));
      g_guest.Put(scene_inputs::kGhRingFlag, 0);
      Check(Near(ground(10.f, -20.f), sw ? 1.5f : 2.5f), "a3.ground_ring_flag0", "flag 0 -> " + std::to_string(y));
      g_guest.Put(scene_inputs::kGhRingFlag, 1);
      const float r1 = ground(10.f, 250.f), r2 = ground(10.f, 450.f), r3 = ground(10.f, 100.f);
      Check(Near(r1, 3.5f) && Near(r2, 7.f) && Near(r3, 0.f), "a3.ground_ramp", "z250 " + std::to_string(r1) + " z450 " + std::to_string(r2) + " z100 " + std::to_string(r3));
      g_guest.Put(scene_inputs::kGhModeObj + scene_inputs::kGhModeOff, 6);   // class 2: other constants
      const float c2 = ground(10.f, 600.f);
      g_guest.Put(scene_inputs::kGhModeObj + scene_inputs::kGhModeOff, 205);   // 200..249 count as 200: no table entry (ids 4..6) -> class 0
      const float c200 = ground(10.f, 250.f);
      Check(Near(c2, 5.5f) && Near(c200, 3.5f), "a3.ground_class", "class2 " + std::to_string(c2) + " id205 " + std::to_string(c200));
      g_guest.Put(scene_inputs::kGhModeObj + scene_inputs::kGhModeOff, 5);
      const float wide0 = ground(80.f, 250.f);   // |x| > 50: only the listed modes ramp
      g_guest.Put(scene_inputs::kGhModeObj + scene_inputs::kGhModeOff, 54);
      const float wide54 = ground(80.f, 250.f);
      g_guest.Put(scene_inputs::kGhModeObj + scene_inputs::kGhModeOff, 5);
      Check(Near(wide0, 0.f) && Near(wide54, 3.5f), "a3.ground_wide_x", "mode5 " + std::to_string(wide0) + " mode54 " + std::to_string(wide54));
      g_guest.Put(scene_inputs::kGhGateObj, kGateObj), g_guest.Put(kGateObj + scene_inputs::kGhGateOff, 0);
      const float gate0 = ground(10.f, 250.f);
      g_guest.Put(scene_inputs::kGhGateObj, 0);
      Check(Near(gate0, 0.f), "a3.ground_gate");
      {   // a missing constant is a refusal, never a zero
        Guest keep = g_guest;
        g_guest.words.erase(scene_inputs::kGhC6);
        float yy = 0;
        const bool bad = !scene_inputs::GroundHeight(ReadThunk, kCh, yy);
        g_guest = keep;
        Check(bad, "a3.ground_unreadable");
      }
      // ---- the module part: c0 = ([circle+16], y, [module+264], [module+268]); c4-c7 = module+2240 transposed
      g_guest.Put(scene_inputs::kShadowModulePtr, kMod);
      g_guest.Put(kMod + scene_inputs::kModCircle, kCirc), g_guest.Put(kMod + 288, 1), g_guest.Put(kMod + 22696, Plant("a3_node_off") ? 0u : 1u);   // the render entry's two off tests: [module+288] == 2, [module+22696] == 0
      F(kCirc + 16, 0.6f), F(kMod + scene_inputs::kModZ, 12.f), F(kMod + scene_inputs::kModW, 3.f);
      for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c) F(kMod + scene_inputs::kModViewProj + 4 * (r * 4 + c), Plant("a3_vp_plain") ? float(c * 4 + r) : float(r * 4 + c));
      // sub_82813228's two handle lookups (bit-8 review 3b): handle at circle+8 resolves through the table at [0x83929A98] (begin [P+8], end [P+16], 8-byte entries); circle+28 through [0x8392E994]
      constexpr uint32_t kP1 = 0x400B9000, kP2 = 0x400BA000, kE1 = 0x400BB000, kE2 = 0x400BC000;
      g_guest.Put(kCirc + 8, 0x00010005), g_guest.Put(kCirc + 28, 0x00020003);
      g_guest.Put(scene_inputs::kHandleTab1, kP1), g_guest.Put(kP1 + 8, kE1), g_guest.Put(kP1 + 16, kE1 + 8 * 8);
      g_guest.Put(scene_inputs::kHandleTab2, kP2), g_guest.Put(kP2 + 8, kE2), g_guest.Put(kP2 + 16, kE2 + 8 * 8);
      for (uint32_t i = 0; i < 8; ++i) g_guest.Put(kE1 + 8 * i, 0), g_guest.Put(kE1 + 8 * i + 4, 0), g_guest.Put(kE2 + 8 * i, 0), g_guest.Put(kE2 + 8 * i + 4, 0);
      g_guest.Put(kE1 + 8 * 5, 0x00010005), g_guest.Put(kE1 + 8 * 5 + 4, 0x1234);   // entry 5 holds the handle: resolves to 0x1234
      g_guest.Put(kE2 + 8 * 3, 0x00020003), g_guest.Put(kE2 + 8 * 3 + 4, 0x5678);
      scene_inputs::CircleModuleRead mr;
      const bool mok = scene_inputs::ReadCircleModule(ReadThunk, mr) == scene_inputs::CircleModuleState::Ok;
      bool vp = mok;
      for (uint32_t r = 0; vp && r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c) vp = vp && Near(mr.view_proj[r * 4 + c], float(c * 4 + r));   // uploaded row r = memory column r
      Check(mok && vp && Near(mr.xzw[0], 0.6f) && Near(mr.xzw[1], 12.f) && Near(mr.xzw[2], 3.f) && mr.active, "a3.circle_module");
      // ---- one character through the whole reader (the ring-square case: y = table[0] = 1.5)
      for (uint32_t i = 0; i < 9; ++i) g_guest.Put(scene_inputs::kCircleSlotTable + 4 * i, i == 2 ? kCh : 0u);
      g_guest.Put(kCh + 1208, 1), g_guest.Put(kCh + 224, 1), g_guest.Put(kCh + 232, 1), g_guest.Put(kCh + 48, kSk), g_guest.Put(kSk + 192, kW), g_guest.Put(kCh + 3112, kComp);
      for (uint32_t w = 0; w < 11; ++w) g_guest.Put(kComp + 40 + 4 * w, ((2 * w) << 16) | (2 * w + 1));   // indices 0..21 (the last is unused)
      for (uint32_t m = 0; m < 24; ++m)
        for (uint32_t c = 0; c < 4; ++c) F(kW + 64 * m + 48 + 4 * c, 100.f * float(m) + float(c));
      g_guest.PutF(kCh + 160, 10.f), g_guest.PutF(kCh + 168, -20.f);
      std::vector<CircleShadow> rows;
      scene_inputs::CircleReadInfo ci;
      uint32_t noh = 9;
      bool mod_ok = false;
      const bool ok = scene_inputs::ReadCircleShadows(ReadThunk, 1199, rows, ci, noh, mod_ok);
      Check(ok && mod_ok && noh == 0 && rows.size() == 1 && rows[0].frame == 1199 && rows[0].slot == 2 && rows[0].character == kCh && (rows[0].flags & kCircleHaveParam) && Near(rows[0].shadow_param[0], 0.6f) &&
                Near(rows[0].shadow_param[1], 1.5f) && Near(rows[0].shadow_param[2], 12.f) && Near(rows[0].shadow_param[3], 3.f) && Near(rows[0].points[20][0], 2000.f) && Near(rows[0].view_proj[1], 4.f),
            "a3.circle_rows", ok && !rows.empty() ? ("c0.y " + std::to_string(rows[0].shadow_param[1])) : "no rows");
      {   // the render entry's second off test (audit item 3): [module+22696] == 0 draws nothing
        g_guest.Put(kMod + 22696, 0);
        scene_inputs::CircleModuleRead m2;
        const bool read2 = scene_inputs::ReadCircleModule(ReadThunk, m2) == scene_inputs::CircleModuleState::NothingToDraw;
        g_guest.Put(kMod + 22696, Plant("a3_node_off") ? 0u : 1u);
        Check(read2 && !m2.active, "a3.circle_node_off");
      }
      {   // sub_82813228 draws nothing when either handle does not resolve (bit-8 review 3b): a stale handle, an index past the table, or an empty entry
        scene_inputs::CircleModuleRead m3;
        g_guest.Put(kE1 + 8 * 5, 0x00990005);   // the entry no longer holds this handle
        const bool a = scene_inputs::ReadCircleModule(ReadThunk, m3) == scene_inputs::CircleModuleState::NothingToDraw;
        g_guest.Put(kE1 + 8 * 5, 0x00010005);
        g_guest.Put(kCirc + 28, 0x00020009);   // index 9 >= 8 entries
        const bool b = scene_inputs::ReadCircleModule(ReadThunk, m3) == scene_inputs::CircleModuleState::NothingToDraw;
        g_guest.Put(kCirc + 28, 0x00020003);
        g_guest.Put(kE2 + 8 * 3 + 4, 0);       // resolves to 0
        const bool c = scene_inputs::ReadCircleModule(ReadThunk, m3) == scene_inputs::CircleModuleState::NothingToDraw;
        g_guest.Put(kE2 + 8 * 3 + 4, 0x5678);
        Check(a && b && c && scene_inputs::ReadCircleModule(ReadThunk, m3) == scene_inputs::CircleModuleState::Ok, "a3.circle_handles");
      }
      {   // no module / no circle object = nothing to draw (review 6); an unreadable pointer is not
        Guest keep2 = g_guest;
        scene_inputs::CircleModuleRead m4;
        g_guest.Put(scene_inputs::kShadowModulePtr, 0);
        const bool n0 = scene_inputs::ReadCircleModule(ReadThunk, m4) == scene_inputs::CircleModuleState::NothingToDraw;
        g_guest = keep2;
        g_guest.Put(kMod + scene_inputs::kModCircle, 0);
        const bool n1 = scene_inputs::ReadCircleModule(ReadThunk, m4) == scene_inputs::CircleModuleState::NothingToDraw;
        g_guest = keep2;
        g_guest.words.erase(scene_inputs::kShadowModulePtr);
        const bool un = scene_inputs::ReadCircleModule(ReadThunk, m4) == scene_inputs::CircleModuleState::Unreadable;
        g_guest = keep2;
        Check(n0 && n1 && un, "a3.circle_nothing_vs_unreadable");
      }
      {   // the module's own off test: mode 2 draws nothing
        g_guest.Put(kMod + 288, 2);
        std::vector<CircleShadow> r2;
        const bool ok2 = scene_inputs::ReadCircleShadows(ReadThunk, 1199, r2, ci, noh, mod_ok);
        g_guest.Put(kMod + 288, 1);
        Check(ok2 && mod_ok && r2.empty(), "a3.circle_module_off");
      }
      // ---- through the contributor: rows in the record, the stamp, then the walk's tracker merge (equal / differs)
      ResetStamps();
      live::inputs::InputsContributor pc;
      pc.SetReader(ReadThunk);
      pc.SetMeshKnown([]() { return true; });
      FrameInfo fi;
      fi.guest_frame = 1200;
      pc.Latch(fi);
      SceneTables t;
      Counters cnt;
      pc.Emit(fi, t, cnt);
      const uint64_t st = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInCirclePoints].load();
      {   // rows without a known disc mesh are not a valid input (bit-8 review 4); with no rows the mesh does not matter (review 6)
        ResetStamps();
        live::inputs::InputsContributor pm;
        pm.SetReader(ReadThunk);
        pm.SetMeshKnown(Plant("a3_mesh_always") ? +[]() { return true; } : +[]() { return false; });
        pm.Latch(fi);
        const uint64_t nomesh = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInCirclePoints].load();
        Check(nomesh != 1199, "a3.circle_no_mesh_no_stamp", "stamp " + std::to_string((unsigned long long)nomesh));
        ResetStamps();
        Guest keep3 = g_guest;
        g_guest.Put(scene_inputs::kShadowModulePtr, 0);
        live::inputs::InputsContributor pn;
        pn.SetReader(ReadThunk);
        pn.Latch(fi);
        const uint64_t nothing = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInCirclePoints].load();
        g_guest = keep3;
        Check(nothing == 1199, "a3.circle_nothing_stamps", "stamp " + std::to_string((unsigned long long)nothing));
        ResetStamps();
        pc.Latch(fi);   // restore the stamp state the checks below read
      }
      Check(t.circle_shadows.size() == 1 && st == 1199, "a3.circle_stamp", "rows " + std::to_string(t.circle_shadows.size()) + " stamp " + std::to_string((unsigned long long)st));
      live::circles::CircleContributor cc;
      cc.Latch(fi);
      CircleShadow walk = t.circle_shadows[0];
      walk.flags = (walk.flags & ~uint32_t(kCircleFromLatch)) | 0x200;   // the walk's row: no latch flag, marked 0x200
      cc.TestInject(walk);
      cc.Emit(fi, t, cnt);
      Check(t.circle_shadows.size() == 1 && (t.circle_shadows[0].flags & 0x200) && !(t.circle_shadows[0].flags & kCircleFromLatch) && cc.LatchSame() == 1 && cc.LatchOnly() == 0 && cc.WalkOnly() == 0 && cc.LatchDiffers() == 0, "a3.circle_merge_equal", "rows " + std::to_string(t.circle_shadows.size()) + " same " + std::to_string((unsigned long long)cc.LatchSame()));
      SceneTables t2;
      pc.Latch(fi);
      pc.Emit(fi, t2, cnt);
      live::circles::CircleContributor cc2;
      cc2.Latch(fi);
      CircleShadow w2 = t2.circle_shadows[0];
      w2.shadow_param[1] += 1.f;
      cc2.TestInject(w2);
      cc2.Emit(fi, t2, cnt);
      Check(t2.circle_shadows.size() == 1 && cc2.LatchDiffers() == 1, "a3.circle_merge_differs", "differs " + std::to_string((unsigned long long)cc2.LatchDiffers()));
      {   // latch_only / walk_only (bit-8 review 5): a latch row of a frame the walk recorded but of a slot it did not = a blob the game does not draw; a walk row with no latch row; duplicate walk rows never pair
        SceneTables t5;
        pc.Latch(fi);
        pc.Emit(fi, t5, cnt);
        CircleShadow other = t5.circle_shadows[0];
        other.slot = 5;   // the walk drew slot 5 only
        other.flags &= ~uint32_t(kCircleFromLatch);
        live::circles::CircleContributor cc5;
        cc5.Latch(fi);
        cc5.TestInject(other);
        cc5.Emit(fi, t5, cnt);
        Check(cc5.LatchOnly() == 1 && cc5.WalkOnly() == 1 && t5.circle_shadows.size() == 2, "a3.circle_latch_only_walk_only", "latch_only " + std::to_string((unsigned long long)cc5.LatchOnly()) + " walk_only " + std::to_string((unsigned long long)cc5.WalkOnly()));
        SceneTables t6;
        pc.Latch(fi);
        pc.Emit(fi, t6, cnt);
        CircleShadow dup = t6.circle_shadows[0];
        dup.flags &= ~uint32_t(kCircleFromLatch);
        live::circles::CircleContributor cc6;
        cc6.Latch(fi);
        cc6.TestInject(dup), cc6.TestInject(dup);   // the walk recorded the same (frame, slot) twice
        cc6.Emit(fi, t6, cnt);
        Check(cc6.LatchSame() == 1 && cc6.WalkOnly() == 1 && t6.circle_shadows.size() == 2, "a3.circle_duplicate_walk_row", "same " + std::to_string((unsigned long long)cc6.LatchSame()) + " walk_only " + std::to_string((unsigned long long)cc6.WalkOnly()));
      }
      {   // a character whose ground height cannot be read: no stamp
        g_guest.words.erase(scene_inputs::kGhRingTable);
        ResetStamps();
        live::inputs::InputsContributor pc3;
        pc3.SetReader(ReadThunk);
        pc3.SetMeshKnown([]() { return true; });
        pc3.Latch(fi);
        const uint64_t st3 = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInCirclePoints].load();
        Check(st3 != 1199, "a3.circle_no_stamp_without_height");
      }
    }
    // ---- review F2: a replaced stage object gives no stamp; duplicate slot models give one row; an arena failure leaves the wrestlers' stamps
    {
      constexpr uint32_t kStage = 0x400E0000, kA0 = 0x400E1000, kA1 = 0x400E2000;
      for (uint32_t i = 0; i < 399; ++i) g_guest.Put(kStage + 6164 + 4 * i, i == 3 || i == 9 ? kA0 : i == 77 ? kA1 : 0u);   // slots 3 and 9 hold the SAME model
      g_guest.PutF(kA0 + 252, 0.5f), g_guest.PutF(kA1 + 252, 0.25f);
      g_guest.Put(kA0 + 832, 0), g_guest.Put(kA1 + 832, 0);
      static int32_t g_arena;
      g_arena = 1;
      static live::stagefx::StageFxContributor* SF;
      live::stagefx::StageFxContributor sf;
      SF = &sf;
      sf.SetReader(ReadThunk);
      sf.SetArenaFn([]() { return g_arena; });
      sf.SetStamp([](uint64_t rf) { scene_inputs::Capture(scene_inputs::kInArenaWorlds, scene_inputs::HookFrameOfRecord(rf)); });
      sf.SetStage(kStage);   // set under arena 1
      live::stagefx::StageFxContributor::Frame sfr;
      ResetStamps();
      sf.Take(200, sfr);
      std::vector<uint32_t> am;
      sf.StageModels(am);
      Check(am.size() == 2, "a3.stage_dedupe", "models " + std::to_string(am.size()));
      Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInArenaWorlds].load() == 199, "a3.stage_current_stamps");
      // the arena changes: the old stage object is freed (its memory reads garbage); the guard must refuse it BEFORE anything is read through it, and the take stamps nothing
      if (!Plant("a3_stage_same")) g_arena = 2;
      for (uint32_t i = 0; i < 399; ++i) g_guest.Put(kStage + 6164 + 4 * i, 0xDEAD0000u + 16 * i);   // freed: no longer models
      ResetStamps();
      sf.Take(201, sfr);
      std::vector<uint32_t> am2;
      const bool got = sf.StageModels(am2);
      const uint64_t after = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInArenaWorlds].load();
      Check(after != 200 && !got && am2.empty(), "a3.stage_changed_no_stamp", "stamp " + std::to_string((unsigned long long)after) + " models " + std::to_string(am2.size()));
      g_arena = 1;
      sf.SetStage(kStage);
      for (uint32_t i = 0; i < 399; ++i) g_guest.Put(kStage + 6164 + 4 * i, i == 3 ? kA0 : i == 77 ? kA1 : 0u);
      // F4: an unreadable arena model costs bit 16 only; the wrestlers' bits (1, 13) still stamp
      g_guest.words.erase(kA1 + 252);
      ResetStamps();
      live::inputs::InputsContributor pc4;
      pc4.SetReader(ReadThunk);
      pc4.SetModels(ModelsThunk);
      g_guest.Put(kM0 + 832, 0), g_guest.Put(kM1 + 832, 0);
      pc4.SetArena([](std::vector<uint32_t>& out) { return SF->StageModels(out); });
      FrameInfo f4;
      f4.guest_frame = 300;
      pc4.Latch(f4);
      auto stamp = [&](int bit) { return rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].load(); };
      Check(stamp(scene_inputs::kInModelRuntime) == 299 && stamp(scene_inputs::kInMaterialParams) == 299 && stamp(scene_inputs::kInArenaModels) != 299, "a3.arena_failure_isolated",
            "bit1 " + std::to_string((unsigned long long)stamp(scene_inputs::kInModelRuntime)) + " bit13 " + std::to_string((unsigned long long)stamp(scene_inputs::kInMaterialParams)) + " bit14 " + std::to_string((unsigned long long)stamp(scene_inputs::kInArenaModels)));
      g_guest.PutF(kA1 + 252, 0.25f);
      ResetStamps();
      pc4.Latch(f4);
      Check(stamp(scene_inputs::kInArenaModels) == 299, "a3.arena_models_stamp");
    }
    // ---- review notes: an unset circle point / the default vector for a zero matrix address / 42 bytes
    {
      constexpr uint32_t kCh = 0x400F0000, kComp = 0x400F1000, kSk = 0x400F2000;
      for (uint32_t i = 0; i < 9; ++i) g_guest.Put(scene_inputs::kCircleSlotTable + 4 * i, i == 0 ? kCh : 0u);
      g_guest.Put(kCh + 1208, 1), g_guest.Put(kCh + 224, 1), g_guest.Put(kCh + 232, 1), g_guest.Put(kCh + 48, kSk), g_guest.Put(kCh + 3112, kComp);
      g_guest.Put(kSk + 192, 0);   // the world array pointer is null: index 0 -> matrix address 0 -> the default vector
      for (uint32_t w = 0; w < 11; ++w) g_guest.Put(kComp + 40 + 4 * w, 0);   // all 21 indices 0
      if (!Plant("a3_default_vec")) for (int c = 0; c < 4; ++c) g_guest.PutF(scene_inputs::kCircleDefaultVec + 4 * c, 9.f + float(c));
      std::vector<scene_inputs::CirclePoints> cps;
      scene_inputs::CircleReadInfo ci;
      const bool ok = scene_inputs::ReadCirclePoints(ReadThunk, cps, ci);
      Check(ok && cps.size() == 1 && Near(cps[0].points[0][0], 9.f) && Near(cps[0].points[0][3], 12.f) && ci.unset == 0 && ci.unreadable == 0, "a3.circle_default_vec", "unset " + std::to_string(ci.unset) + " unreadable " + std::to_string(ci.unreadable));
    }
    // ---- S8 (audit item 4): the plane node's proxy list copy -> mask rows + the plane-constants stamp
    {
      using namespace rdr_scene::live;
      planeproxy::The().Reset();
      {   // a readable plane module (S7), as the plane block above builds it
        constexpr uint32_t kM = 0x40070000, kH = 0x40071000;
        g_guest.Put(scene_inputs::kPlaneModulePtr, kM), g_guest.Put(kM + scene_inputs::kPlEnabledOff, 1), g_guest.Put(scene_inputs::kPlSuppressWord, 0);
        g_guest.Put(scene_inputs::kPlCamHolder, kH), g_guest.Put(kH + scene_inputs::kPlCamHolderOff, 0x40072000);
        for (uint32_t i = 0; i < 10; ++i) g_guest.PutF(kM + 68 + 4 * i, 1.f);
        for (uint32_t i = 0; i < 16; ++i) g_guest.PutF(kM + scene_inputs::kPlCameraOff + 4 * i, 1.f), g_guest.PutF(kM + scene_inputs::kPlFitOff + 4 * i, 1.f);
      }
      uint32_t raw[planeproxy::kWords];
      for (uint32_t i = 0; i < planeproxy::kWords; ++i) raw[i] = scene_inputs::Swap32(Plant("a3_proxy_swap") ? 0u : (0xA0000000u + 0x100u * i));   // guest byte order (big-endian words)
      if (Plant("a3_proxy_swap")) for (uint32_t i = 0; i < planeproxy::kWords; ++i) raw[i] = 0xA0000000u + 0x100u * i;   // the planted board keeps them unswapped
      planeproxy::The().Note(1000, raw, scene_inputs::Swap32(3));   // the plane node is a DRAW-HALF hook: its note carries the RECORD frame (1000), not the hook frame (999)
      std::vector<planeproxy::Snap> got;
      planeproxy::The().Take(got);
      Check(got.size() == 1 && got[0].frame == 1000 && got[0].count == 3 && got[0].words[2] == 0xA0000200u && got[0].words[31] == 0xA0001F00u, "a3.proxy_words", got.empty() ? "no copy" : ("w2 " + std::to_string(got[0].words[2])));
      // the contributor: rows + stamp, only with S7 read AND the copy of this hook frame
      planeproxy::The().Note(1000, raw, scene_inputs::Swap32(3));
      planeproxy::The().Note(1001, raw, scene_inputs::Swap32(1));   // a NEWER copy: it belongs to the next record and must not be consumed (F1)
      ResetStamps();
      live::inputs::InputsContributor pc;
      pc.SetReader(ReadThunk);
      FrameInfo fp;
      fp.guest_frame = 1000;
      pc.Latch(fp);
      SceneTables t;
      Counters c;
      pc.Emit(fp, t, c);
      const uint64_t st = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInPlaneConstants].load();
      size_t latch_rows = 0;
      for (const PlaneDraw& d : t.plane_draws) latch_rows += (d.flags & kPlaneFromLatch) && d.kind == kPlaneProxy && d.have == 0 && d.frame == 1000 ? 1 : 0;
      Check(st == 999 && latch_rows == 3 && t.plane_draws[0].object == 0xA0000000u, "a3.proxy_rows_and_stamp", "stamp " + std::to_string((unsigned long long)st) + " rows " + std::to_string(latch_rows));
      {   // the newer copy stayed in the board for the next record
        std::vector<planeproxy::Snap> left;
        planeproxy::The().TakeUpTo(1001, left);
        Check(left.size() == 1 && left[0].frame == 1001 && left[0].count == 1, "a3.proxy_newer_copy_kept", "left " + std::to_string(left.size()));
      }
      // no copy for the frame (the node did not run): no stamp, no rows
      ResetStamps();
      live::inputs::InputsContributor pn;
      pn.SetReader(ReadThunk);
      FrameInfo fn;
      fn.guest_frame = 1001;
      if (Plant("a3_proxy_stamp_early")) scene_inputs::Capture(scene_inputs::kInPlaneConstants, 1000);
      pn.Latch(fn);
      Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInPlaneConstants].load() != 1000, "a3.proxy_stamp_needs_copy");
      // the plane contributor drops the latch rows of a frame the walk recorded draws for, keeps them otherwise
      planeproxy::The().Note(1000, raw, scene_inputs::Swap32(2));
      SceneTables tm;
      Counters cm;
      live::inputs::InputsContributor pm;
      pm.SetReader(ReadThunk);
      pm.Latch(fp);
      pm.Emit(fp, tm, cm);
      live::plane::PlaneContributor plane;
      plane.Latch(fp);
      PlaneDraw wd;
      wd.frame = 1000, wd.kind = kPlaneMask, wd.object = 0xB0000000u, wd.have = kPlaneDrawHavePos;
      plane.TestInjectDraw(wd);
      plane.Emit(fp, tm, cm);
      size_t left_latch = 0, walk_rows = 0;
      for (const PlaneDraw& d : tm.plane_draws) (d.flags & kPlaneFromLatch ? left_latch : walk_rows)++;
      Check(left_latch == 0 && walk_rows == 1 && plane.LatchRowsDropped() == 2, "a3.proxy_walk_rows_win", "latch rows left " + std::to_string(left_latch) + " walk " + std::to_string(walk_rows));
    }
    // ---- bit 4: the per-node install table (light_index_core.h) and its kLoModelMap rows
    {
      using namespace rdr_scene::live;
      lightidx::Table tb;
      // the thread slot: tls = [r13], blk = [tls + 1048], slot = blk ? [blk + 32] : 0
      constexpr uint32_t kR13a = 0x400F8000, kTlsA = 0x400F8100, kBlkA = 0x400F8200, kR13b = 0x400F8300, kTlsB = 0x400F8400, kBlkB = 0x400F8500;
      g_guest.Put(kR13a, kTlsA), g_guest.Put(kTlsA + 1048, kBlkA), g_guest.Put(kBlkA + 32, 1);
      g_guest.Put(kR13b, kTlsB), g_guest.Put(kTlsB + 1048, 0);   // a thread with no block: slot 0 (as the game does)
      auto rd = [](uint32_t a, uint32_t& v) { uint32_t raw; if (!g_guest.Read(a, &raw, 4)) return false; v = scene_inputs::Swap32(raw); return true; };
      uint32_t sa = 99, sb = 99;
      const bool oka = lightidx::SlotOf(+rd, kR13a, sa), okb = lightidx::SlotOf(+rd, kR13b, sb);
      Check(oka && okb && sa == 1 && sb == 0, "a3.light_slot_of", "slots " + std::to_string(sa) + " " + std::to_string(sb));
      tb.OnInstall(1, 7, 1);   // slot 1: sub_826203E8(7, 1) -> ...
      tb.OnInstallObject(1, 0xAA10);   // ... which ends in sub_8261FE90(0xAA10)
      tb.OnInstall(0, 3, 1);
      tb.OnInstallObject(0, 0xAA20);
      tb.OnModel(0x1000, Plant("a3_light_slot") ? 0u : 1u, 500);   // a model drawn by slot 1: follows slot 1's install (7), not slot 0's (3)
      tb.OnModel(0x2000, 0, 500);
      tb.OnModel(0x3000, 5, 500);   // a slot that never installed: counted, no entry
      std::vector<lightidx::NodeLight> snap;
      tb.Snapshot(501, snap);
      Check(snap.size() == 2 && snap[0].model == 0x1000 && snap[0].index == 7 && snap[0].mode == 1 && snap[0].object == 0xAA10 && snap[1].model == 0x2000 && snap[1].index == 3 && snap[1].object == 0xAA20 && tb.GetStats().no_install == 1, "a3.light_slot_isolation",
            snap.empty() ? "empty" : ("model0 index " + std::to_string(snap[0].index)));
      tb.OnInstall(1, 9, 1);
      tb.OnInstallObject(1, 0xAA30);
      tb.OnModel(0x1000, 1, 502);   // the same model under another install: counted
      Check(tb.GetStats().changed == 1, "a3.light_changed_counted");
      {   // E4's finding: the wrestler nodes install their EMBEDDED object straight through sub_8261FE90, with no sub_826203E8 before it: the model still gets its object (labelled kNoIndex), never the slot's PREVIOUS install
        lightidx::Table td;
        td.OnInstall(2, 5, 1);
        td.OnInstallObject(2, 0xBB10);                // an indexed install ...
        td.OnInstallObject(2, 0xBB20);                // ... then a direct one (no index hook in between)
        td.OnModel(0x7000, Plant("a3_light_direct_ignored") ? 3u : 2u, 600);
        std::vector<lightidx::NodeLight> sd;
        td.Snapshot(600, sd);
        Check(sd.size() == 1 && sd[0].object == 0xBB20 && sd[0].index == lightidx::kNoIndex && sd[0].mode == lightidx::kNoIndex, "a3.light_direct_install_object", sd.empty() ? "no entry" : ("object " + std::to_string(sd[0].object)));
      }
      tb.Snapshot(502 + lightidx::kMaxAge + 1, snap);   // nothing confirmed 0x2000 for kMaxAge + 2 frames; 0x1000 for kMaxAge + 1: both older than kMaxAge
      Check(snap.empty() && tb.GetStats().expired == 2 && tb.GetStats().size == 0, "a3.light_expires", "left " + std::to_string(snap.size()));
      // the contributor: 80 learned models -> two kLoModelMap rows (74 + 6), the stamp only with a non-empty table and the objects read
      auto& gt = lightidx::The();
      gt.Clear();
      constexpr uint32_t kMgr = 0x40100000, kSets = 0x40101000, kIds = 0x40101100, kRem = 0x40101200;
      g_guest.Put(scene_inputs::kLightMgrPtr, kMgr);
      g_guest.Put(kMgr + 124, 1), g_guest.Put(kMgr + 112, kSets), g_guest.Put(kMgr + 116, kIds), g_guest.Put(kMgr + 120, kRem), g_guest.Put(kMgr + 140, 0), g_guest.Put(kMgr + 144, 0);
      g_guest.Put(kMgr + 368, 0), g_guest.Put(kMgr + 372, 0), g_guest.Put(kMgr + 376, 0);
      g_guest.Put(kSets, 0), g_guest.Put(kIds, 1), g_guest.Put(kRem, 0xFFFFFFFFu);
      for (uint32_t k = 0; k < kLsLightFloats; ++k) g_guest.PutF(0xC0000 + 4 * k, 40.f + float(k));   // the embedded light object the learned table names
      g_guest.Put(scene_inputs::kLightOverrideWord0, 0xFFFFFFFFu), g_guest.Put(scene_inputs::kLightOverrideWord1, 0xFFFFFFFFu), g_guest.Put(scene_inputs::kLightGatePtr, 0);
      ResetStamps();
      live::inputs::InputsContributor pe;
      pe.SetReader(ReadThunk);
      FrameInfo fe;
      fe.guest_frame = 2000;
      pe.Latch(fe);
      const uint64_t empty_st = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load();
      if (Plant("a3_light_nostamp_empty")) scene_inputs::Capture(scene_inputs::kInLightStates, 1999);
      Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() != 1999 && empty_st != 1999, "a3.light_empty_no_stamp");
      gt.OnInstall(0, 4, 1);
      gt.OnInstallObject(0, 0xC0000);
      for (uint32_t i = 0; i < 80; ++i) gt.OnModel(0x50000 + 16 * i, 0, 1999);
      scene_inputs::g_stamp_light_states = true;   // S103 A4: the stamp switch is on, but nobody reads the map yet (F2 (1)) -> no stamp even with a full table
      scene_inputs::g_light_map_consumer = Plant("a3_light_consumer_ignored") ? true : false;
      ResetStamps();
      {
        live::inputs::InputsContributor pg;
        pg.SetReader(ReadThunk);
        pg.Latch(fe);
        Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() != 1999, "a3.light_no_consumer_no_stamp");
      }
      scene_inputs::g_stamp_light_states = true;
      scene_inputs::g_light_map_consumer = true;
      ResetStamps();
      live::inputs::InputsContributor pf;
      pf.SetReader(ReadThunk);
      pf.Latch(fe);
      SceneTables tf;
      Counters cf;
      pf.Emit(fe, tf, cf);
      size_t maps = 0, triples = 0;
      bool first_ok = false;
      for (const LightObjectRow& r : tf.light_objects)
        if (r.kind == kLoModelMap) {
          ++maps, triples += r.aux0;
          if (maps == 1) first_ok = r.aux0 == 56 && r.words[0] == 0x50000 && r.words[1] == 0xC0000 && r.words[2] == 4 && r.words[3] == 1;
        }
      Check(maps == 2 && triples == 80 && first_ok && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() == 1999, "a3.light_model_rows_and_stamp", "maps " + std::to_string(maps) + " triples " + std::to_string(triples));
      {   // F2 (2): every model the latch knows needs an entry (a wrestler skeleton without one = no stamp); F2 (3): an entry near expiry plans a hold
        ResetStamps();
        live::inputs::InputsContributor ph;
        ph.SetReader(ReadThunk);
        ph.SetModels(+[](std::vector<uint32_t>& out) { out = {0x50000u, 0x99990u}; });   // 0x99990 has no entry
        const bool sv_wl_g = rdr_scene::live::wlatch::g_on;
        rdr_scene::live::wlatch::g_on = false;   // S103 B4X: wrestlers are asked only while A8's synthesis is off (b4x.wrestlers_not_asked_with_synthesis)
        ph.Latch(fe);
        rdr_scene::live::wlatch::g_on = sv_wl_g;
        Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() != 1999, "a3.light_missing_model_no_stamp");
        rdr_own::S(rdr_own::Pass::Scene3D).hold_until.store(0);
        gt.Clear();
        gt.OnInstall(0, 4, 1);
        gt.OnInstallObject(0, 0xC0000);
        gt.OnModel(0x50000, 0, 1999 - (lightidx::kMaxAge - 2));   // confirmed kMaxAge - 2 hook frames ago: it would expire in two frames
        live::inputs::InputsContributor pi;
        pi.SetReader(ReadThunk);
        pi.SetModels(+[](std::vector<uint32_t>& out) { out = {0x50000u}; });   // S103 B4X F5: the refresh plan looks at asked entries only: this one is asked (a kept skeleton)
        pi.Latch(fe);
        const uint64_t hold = rdr_own::S(rdr_own::Pass::Scene3D).hold_until.load();
        if (Plant("a3_light_no_hold")) rdr_own::S(rdr_own::Pass::Scene3D).hold_until.store(0);
        Check(rdr_own::S(rdr_own::Pass::Scene3D).hold_until.load() >= 1999 + 3 && hold >= 1999 + 3, "a3.light_planned_hold", "hold_until " + std::to_string((unsigned long long)hold));
        rdr_own::S(rdr_own::Pass::Scene3D).hold_until.store(0);
        gt.Clear();
        gt.OnInstall(0, 4, 1);
        gt.OnInstallObject(0, 0xC0000);
        for (uint32_t i = 0; i < 80; ++i) gt.OnModel(0x50000 + 16 * i, 0, 1999);
      }
      {   // S103 B4X (Q1 class: a stamp waiting on evidence only an earlier OURS frame, or a walk of a model the game may cull, can produce). Bit 4 has no OURS-only rule: a COMPUTED arena-slot entry (also a slot with no
          // l(N): the game's default set 100, sub_824E8E38) is recomputed at every latch (frame = hook), so a model the game culls never lapses; wrestlers are not asked while A8's synthesis is on.
        const auto S = [](uint64_t v) { return std::to_string((unsigned long long)v); };
        const bool sv_ids = rdr_scene::live::lightstamp::g_from_ids, sv_wl = rdr_scene::live::wlatch::g_on;
        auto& hold_until = rdr_own::S(rdr_own::Pass::Scene3D).hold_until;
        constexpr uint32_t kObjM = 0xC0000 - 48;   // the manager's set 0 = the object whose block is at 0xC0000 (an object install copies obj + 48)
        constexpr uint32_t kDefaultSet = 100;      // the loader's default slot set (sub_824E8E38 fills slot words 0..398 (399) with its position first)
        g_guest.Put(kSets, kObjM), g_guest.Put(kIds, kDefaultSet);   // the manager's id list holds set 100 at position 0
        static std::vector<uint32_t> s_models, s_arena;
        static uint32_t s_set = 0xFFFFFFFFu;
        static bool s_expl = true;   // the slot's set is named by the text (false = the loader default)
        const auto latch = [&](uint64_t record_frame, std::string* census) {
          ResetStamps();
          hold_until.store(0);
          live::inputs::InputsContributor pa;
          pa.SetReader(ReadThunk);
          if (!s_models.empty()) pa.SetModels(+[](std::vector<uint32_t>& o) { o = s_models; });
          pa.SetArenaSlots(+[](std::vector<uint32_t>& m, std::vector<uint16_t>& sl) { m = s_arena; sl.assign(s_arena.size(), 5); return true; });
          pa.SetSlotLightSet(+[](uint16_t, bool* e) { if (e) *e = s_expl; return s_set; });
          FrameInfo fl;
          fl.guest_frame = record_frame;
          pa.Latch(fl);
          if (census) census->clear(), pa.Census(*census);
          return rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load();
        };
        constexpr uint32_t kAr = 0x58000, kAf = 0x58300, kWr = 0x58200;
        const auto fresh_table = [&]() {
          gt.Clear();
          gt.OnInstall(0, 4, 1);
          gt.OnInstallObject(0, 0xC0000);
        };
        rdr_scene::live::lightstamp::g_from_ids = true;
        rdr_scene::live::wlatch::g_on = true;
        // (1) a slot model with a text row but no l(N) (set 100 by default) is COMPUTED: never walked, never confirmed (the game culls it), yet its entry is fresh at every latch and never lapses
        s_models.clear(), s_arena = {kAr};
        s_set = Plant("b4x_noid_walk_learned") ? 0xFFFFFFFFu : kDefaultSet;   // the twin: the old rule, a slot with no l(N) is walk-learned only
        fresh_table();
        gt.OnModel(kAf, 0, 1999);   // another model, so the table is not empty (never kAr)
        std::string cen;
        const uint64_t s1 = latch(2000, &cen);
        Check(s1 == 1999, "b4x.culled_no_id_slot_never_lapses", "stamp " + S(s1));
        const uint64_t s1b = latch(2000 + 100000, nullptr);   // a very long run later, still never walked: still stamps, and plans no refresh hold (the computed entry's frame is the hook)
        Check(s1b == 2000 + 100000 - 1 && hold_until.load() < 2000 + 100000 - 1 + 3, "b4x.computed_entry_never_ages", "stamp " + S(s1b) + " hold_until " + S(hold_until.load()));
        // (2) a computed entry carries no keep / proven state: the census has no such counters, and nothing about it needs a walk or an OURS frame
        latch(2000, &cen);
        Check(cen.find("arena_kept") == std::string::npos && cen.find("proven_entries") == std::string::npos, "b4x.no_keep_state_in_census");
        // (3) F4: with A8's synthesis on, a kept skeleton with NO character slot (the referee: no prediction at the latch) is still asked: with no entry it is a hole, with an entry it stamps. (The fake guest has no
        // character slot table, so every kept skeleton here is such a skeleton; the "character wrestlers are not asked" half is the live census: emitted_no_pred vs the asked count.)
        s_set = kDefaultSet, s_expl = true;
        s_models = {kWr};
        rdr_scene::live::wlatch::g_on = true;
        if (Plant("b4x_f4_not_asked")) gt.OnModel(kWr, 0, 1999);   // the twin: it has an entry, so the old "not asked" and the new "asked" read alike: the hole check must fail
        Check(latch(2000, nullptr) != 1999, "b4x.f4_no_slot_skeleton_asked_with_synthesis");
        rdr_scene::live::wlatch::g_on = false;   // synthesis off: asked as before, so a wrestler with no entry is a hole
        if (!Plant("b4x_f4_not_asked")) Check(latch(2000, nullptr) != 1999, "b4x.control_wrestlers_asked_without_synthesis");
        gt.OnModel(kWr, 0, 1999);
        rdr_scene::live::wlatch::g_on = true;
        Check(latch(2000, nullptr) == 1999, "b4x.f4_asked_skeleton_with_entry_stamps");
        // (3b) F2: a default-100 slot (no l(N) in the text) never replaces a FRESH learned entry (the walk's install stays; the census counts the disagreement); with no learned entry it fills the hole
        s_models.clear(), s_arena = {kAr};
        s_set = kDefaultSet, s_expl = Plant("b4x_f2_default_replaces");   // the twin: the same slot named by the text (explicit), so it replaces
        fresh_table();
        gt.OnModel(kAr, 0, 1999);   // learned: index 4 / mode 1; the computed default is index 0 / mode 1: differ
        gt.OnModel(kAf, 0, 1999);
        latch(2000, &cen);
        Check(cen.find("differ_default 1") != std::string::npos && cen.find("filled_default 0") != std::string::npos, "b4x.f2_default_never_replaces_learned", cen.substr(cen.find("light_by_id") == std::string::npos ? 0 : cen.find("light_by_id"), 140));
        fresh_table();
        gt.OnModel(kAf, 0, 1999);   // kAr has no learned entry: the default fills it
        s_expl = false;
        Check(latch(2000, &cen) == 1999 && cen.find("filled_default 1") != std::string::npos, "b4x.f2_default_fills_hole");
        // (3b2) S103 LH: a default-100 slot whose learned entry EQUALS the computed one (index 0 / mode 1 / object 0xC0000) about to expire (confirmed kMaxAge - 1 frames ago) is re-dated by the latch: no planned refresh walk
        // (the stream-free build repeats our last picture on each one), the entry is fresh, and the stamp is made. A learned entry that DIFFERS (index 4) still ages and plans its walk. Twin lh_old_hold: the old rule.
        s_models.clear(), s_arena = {kAr}, s_set = kDefaultSet, s_expl = false;
        rdr_scene::live::lightstamp::g_refresh_default = !Plant("lh_old_hold");
        gt.Clear();
        gt.OnInstall(0, 0, 1);
        gt.OnInstallObject(0, 0xC0000);
        gt.OnModel(kAr, 0, 2000 - lightidx::kMaxAge + 1);   // equal to the computed default; expires in one frame
        const uint64_t sh = latch(2000, &cen);
        Check(hold_until.load() == 0 && sh == 1999 && cen.find("refreshed_default 1") != std::string::npos, "lh.default_equal_entry_plans_no_hold", "hold_until " + S(hold_until.load()) + " stamp " + S(sh) + " " + cen.substr(cen.find("light_by_id") == std::string::npos ? 0 : cen.find("light_by_id"), 160));
        const uint64_t sh2 = latch(2000 + 100000, nullptr);   // far later, never walked: still fresh, still no hold
        Check(sh2 == 2000 + 100000 - 1 && hold_until.load() == 0, "lh.refreshed_entry_never_ages", "hold_until " + S(hold_until.load()) + " stamp " + S(sh2));
        fresh_table();   // index 4 / mode 1: differs from the computed default (index 0)
        gt.OnModel(kAr, 0, 2000 - lightidx::kMaxAge + 1);
        gt.OnModel(kAf, 0, 1999);
        latch(2000, &cen);
        Check(hold_until.load() >= 2000 - 1 + 3 && cen.find("differ_default 1") != std::string::npos && cen.find("refreshed_default 0") != std::string::npos, "lh.differing_entry_still_walks", "hold_until " + S(hold_until.load()));
        rdr_scene::live::lightstamp::g_refresh_default = true;
        // (3c) F5: the refresh plan looks only at ASKED, LEARNED entries: an entry about to expire that nobody asked about plans no hold
        s_models.clear(), s_arena = {kAr}, s_set = kDefaultSet, s_expl = true;
        fresh_table();
        gt.OnModel(kAf, 0, 2000 - lightidx::kMaxAge + 1);   // expires in one frame, not asked (not a stage model, not a kept skeleton)
        if (Plant("b4x_f5_oldest_all")) s_models = {kAf};   // the twin: it is asked (a kept skeleton) while synthesis is off
        rdr_scene::live::wlatch::g_on = !Plant("b4x_f5_oldest_all");
        latch(2000, nullptr);
        Check(hold_until.load() == 0, "b4x.f5_unasked_entry_plans_no_hold", "hold_until " + S(hold_until.load()));
        rdr_scene::live::wlatch::g_on = true;
        s_models.clear();
        // (3d) F1 / F3: the slot text's tokens (slot_light_token.h): l(N) read, le( / lg( unread, the last token wins; a slot < 399 with no row = the default, >= 399 none
        {
          using namespace rdr_scene::live::arena;
          SlotLightText a, b, c, d;
          for (const char* t : {"r", "l(7)"}) ApplyLightToken(t, a);
          for (const char* t : {"l(7)", Plant("b4x_f1_le_read") ? "l(9)" : "le(9)"}) ApplyLightToken(t, b);
          for (const char* t : {"lg(9)", "l(8)"}) ApplyLightToken(t, c);
          for (const char* t : {"l(8)", "lg(9)"}) ApplyLightToken(t, d);
          bool e1 = false, e2 = false, e3 = false;
          const uint32_t s_a = SlotSetOf(&a, 5, &e1), s_b = SlotSetOf(&b, 5, &e2), s_c = SlotSetOf(&c, 5, nullptr), s_d = SlotSetOf(&d, 5, nullptr);
          Check(s_a == 7 && e1 && s_b == kSlotNoSet && !e2 && s_c == 8 && s_d == kSlotNoSet, "b4x.f1_le_lg_unread", S(s_a) + " " + S(s_b) + " " + S(s_c) + " " + S(s_d));
          const uint32_t n1 = SlotSetOf(nullptr, 398, &e3), n2 = SlotSetOf(nullptr, Plant("b4x_f3_no_row_none") ? 0 : 399, nullptr), n3 = SlotSetOf(nullptr, 398, nullptr);
          SlotLightText dflt;   // a row with no l token: default 100, not explicit
          Check(n1 == 100 && !e3 && n2 == kSlotNoSet && n3 == 100 && SlotSetOf(&dflt, 3, &e3) == 100 && !e3, "b4x.f3_no_row_default", S(n1) + " " + S(n2));
        }
        // (4) a LEARNED entry (no id, not computed) lives kMaxAge after its last walked confirmation, whatever its history: no long life, no OURS flag
        s_models.clear();
        s_set = 0xFFFFFFFFu;   // not computed: the learned table is all there is
        fresh_table();
        for (uint32_t f : {100u, 120u, 140u}) gt.OnModel(kAr, 0, f);   // 3 confirmations over 40 hook frames (the old "steady" shape)
        Check(latch(2000, nullptr) != 1999, "b4x.learned_lapses_at_kmaxage_only");   // 1859 frames old: lapsed (a walk must re-confirm it)
        fresh_table();
        for (uint32_t f : {100u, 120u, 140u}) gt.OnModel(kAr, 0, f);
        Check(latch(140 + lightidx::kMaxAge, nullptr) == 140 + lightidx::kMaxAge - 1, "b4x.learned_fresh_within_kmaxage");
        rdr_scene::live::lightstamp::g_from_ids = sv_ids, rdr_scene::live::wlatch::g_on = sv_wl;
        s_models.clear(), s_arena.clear(), s_set = 0xFFFFFFFFu;
        hold_until.store(0);
        g_guest.Put(kSets, 0), g_guest.Put(kIds, 1);
        gt.Clear(), gt.OnInstall(0, 4, 1), gt.OnInstallObject(0, 0xC0000);
        for (uint32_t i = 0; i < 80; ++i) gt.OnModel(0x50000 + 16 * i, 0, 1999);   // the table the next checks expect
      }
      {   // a model the latch does NOT know this frame gets no node-object row (review must 6): the cap is not spent on models that are gone
        SceneTables tk;
        live::inputs::InputsContributor pk;
        pk.SetReader(ReadThunk);
        pk.SetModels(+[](std::vector<uint32_t>& out) { out = {0x12340u}; });   // 0x50000 is NOT in the latch's list
        pk.Latch(fe);
        pk.Emit(fe, tk, cf);
        size_t nobjs = 0;
        for (const LightObjectRow& r : tk.light_objects) nobjs += r.kind == kLoNodeObject ? 1 : 0;
        Check(nobjs == 0, "a3.light_node_rows_known_models_only", "rows " + std::to_string(nobjs));
      }
      {   // the embedded object 0xC0000 is in no manager list: its block is read at the latch as a kLoNodeObject row; an unreadable one withholds the stamp
        for (uint32_t k = 0; k < kLsLightFloats; ++k) g_guest.PutF(0xC0000 + 4 * k, 40.f + float(k));
        ResetStamps();
        SceneTables tn;
        live::inputs::InputsContributor po;
        po.SetReader(ReadThunk);
        po.SetModels(+[](std::vector<uint32_t>& out) { out = {0x50000u}; });   // the latch knows this model THIS frame (review must 6)
        FrameInfo fo;
        fo.guest_frame = 2000;
        po.Latch(fo);
        po.Emit(fo, tn, cf);
        size_t objs = 0;
        bool block_ok = false;
        for (const LightObjectRow& r : tn.light_objects)
          if (r.kind == kLoNodeObject) ++objs, block_ok = r.object == 0xC0000 && (r.flags & kLoBlockRead) && Near(*reinterpret_cast<const float*>(&r.words[3]), 43.f);
        Check(objs == 1 && block_ok && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() == 1999, "a3.light_node_object_rows", "rows " + std::to_string(objs));
        g_guest.words.erase(0xC0000 + 4 * 10);
        ResetStamps();
        live::inputs::InputsContributor pu;
        pu.SetReader(ReadThunk);
        pu.SetModels(+[](std::vector<uint32_t>& out) { out = {0x50000u}; });
        pu.Latch(fo);
        Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() != 1999, "a3.light_node_object_unreadable_no_stamp");
        g_guest.PutF(0xC0000 + 4 * 10, 50.f);
      }
      std::vector<uint8_t> by = SerializeRecord(tf);
      SceneTables back;
      std::string er;
      Check(ParseRecord(by.data(), by.size(), back, er) && back.light_errors == 0 && back.light_objects.size() == tf.light_objects.size(), "a3.light_model_rows_roundtrip", er);
      for (LightObjectRow& r : tf.light_objects) if (r.kind == kLoModelMap) { r.aux0 = 57; break; }
      by = SerializeRecord(tf);
      SceneTables bad;
      Check(ParseRecord(by.data(), by.size(), bad, er) && bad.light_errors == 1 && bad.light_objects.empty(), "a3.light_model_rows_bounds");
      {   // ---- S103 A4: Coverage (holes, stale, future), proven-persistent entries (OURS-confirmed only), the stamp switch
        using lightidx::NodeLight;
        rdr_scene::live::lightstamp::g_on = true;   // the long life exists only with the stamp switch on (checked off below)
        lightidx::Table tc;
        tc.OnInstall(0, 4, 1);
        tc.OnInstallObject(0, 0xD0000);
        tc.OnModel(0x61000, 0, 100);
        tc.OnModel(0x62000, 0, 100);
        if (Plant("a4_hole_filled")) tc.OnModel(0x63000, 0, 100);   // the twin: the hole gets an entry, so the check below must see no hole
        std::vector<NodeLight> sc;
        tc.Snapshot(101, sc);
        const lightidx::Cover cv = lightidx::Coverage(sc, std::vector<uint32_t>{0x61000, 0x62000, 0x63000}, 101);
        Check(!cv.ok() && cv.missing == 1 && cv.stale == 0 && cv.nfirst == 1 && cv.first_missing[0] == 0x63000, "a4.light_hole_not_ok", "missing " + std::to_string(cv.missing));
        // a stale entry (confirmed long before the record's hook frame) is refused, even if the snapshot did not erase it (a record / replay with its own hook frame)
        lightidx::Table tz;
        tz.OnInstall(0, 4, 1);
        tz.OnInstallObject(0, 0xD0000);
        tz.OnModel(0x61000, 0, Plant("a4_stale_fresh") ? 100u + lightidx::kMaxAge : 100u);
        std::vector<NodeLight> sz;
        tz.Snapshot(100, sz);
        const lightidx::Cover cz = lightidx::Coverage(sz, std::vector<uint32_t>{0x61000}, 100 + lightidx::kMaxAge + 1);
        Check(!cz.ok() && cz.stale == 1 && cz.missing == 0, "a4.light_stale_refused", "stale " + std::to_string(cz.stale));
        const lightidx::Cover cf = lightidx::Coverage(sz, std::vector<uint32_t>{0x61000}, 100 + lightidx::kMaxAge);
        Check(cf.ok() || Plant("a4_stale_fresh"), "a4.light_edge_age_fresh", "stale " + std::to_string(cf.stale));
        {   // an entry dated after the hook frame (a clock step-back: load, replay) is stale, in Coverage and in Snapshot; one frame ahead is accepted
          lightidx::Table tf2;
          tf2.OnInstall(0, 4, 1);
          tf2.OnInstallObject(0, 0xD0000);
          tf2.OnModel(0x66000, 0, Plant("a4_future_fresh") ? 100u : 5000u);
          tf2.OnModel(0x66100, 0, 101);
          std::vector<NodeLight> s2;
          const lightidx::Cover c2 = lightidx::Coverage(std::vector<NodeLight>{NodeLight{0x66000, 0xD0000, 4, 1, 5000}, NodeLight{0x66100, 0xD0000, 4, 1, 101}}, std::vector<uint32_t>{0x66000, 0x66100}, 100);
          tf2.Snapshot(100, s2);
          Check(c2.stale == 1 && c2.first_missing[0] == 0x66000 && s2.size() == 1 && s2[0].model == 0x66100, "a4.light_future_stale", "stale " + std::to_string(c2.stale) + " kept " + std::to_string(s2.size()));
        }
        // S103 B4X: NO long life: whatever an entry's history (3 distinct frames over >= 30, one confirmation, a model once seen under another install) it ends kMaxAge hook frames after its last confirmation
        lightidx::Table tp;
        tp.OnInstall(0, 4, 1);
        tp.OnInstallObject(0, 0xD0000);
        for (uint32_t fr : {100u, 115u, 130u}) tp.OnModel(0x6A000, 0, fr);   // the old "proven" shape
        tp.OnModel(0x65000, 0, 131);                                         // one confirmation
        std::vector<NodeLight> sp, sp2;
        tp.Snapshot(130 + lightidx::kMaxAge, sp);   // exactly kMaxAge after the last confirmation of 0x6A000: still there
        Check(sp.size() == 2 && sp[0].model == 0x65000 && sp[1].model == 0x6A000, "a4.light_kmaxage_edge_kept", "kept " + std::to_string(sp.size()));
        tp.Snapshot(131 + lightidx::kMaxAge + 5, sp2);
        if (Plant("b4x_long_life")) sp2.push_back(sp[1]);   // the twin: a once-"proven" entry that outlives kMaxAge
        Check(sp2.empty(), "b4x.no_long_life_for_any_entry", "left " + std::to_string(sp2.size()));
        // the contributor: a fully covered frame stamps only with the stamp switch AND the consumer; with either off it never does
        scene_inputs::g_stamp_light_states = true;
        scene_inputs::g_light_map_consumer = true;
        gt.Clear();
        gt.OnInstall(0, 4, 1);
        gt.OnInstallObject(0, 0xC0000);
        gt.OnModel(0x50000, 0, 1999);
        ResetStamps();
        live::inputs::InputsContributor pz;
        pz.SetReader(ReadThunk);
        pz.SetModels(+[](std::vector<uint32_t>& out) { out = {0x50000u}; });
        pz.Latch(fe);
        Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() == 1999, "a4.light_full_cover_stamps");
        scene_inputs::g_stamp_light_states = Plant("a4_flag_ignored") ? true : false;
        ResetStamps();
        live::inputs::InputsContributor py;
        py.SetReader(ReadThunk);
        py.SetModels(+[](std::vector<uint32_t>& out) { out = {0x50000u}; });
        py.Latch(fe);
        Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInLightStates].load() != 1999, "a4.light_flag_off_no_stamp");
        // the input's `moved` follows the constexpr switch (off in the shipped header)
        Check(scene_inputs::kInputs[scene_inputs::kInLightStates].moved == rdr_scene::live::lightstamp::kStampLightStates && (Plant("a4_moved_hardcoded") ? !scene_inputs::kInputs[scene_inputs::kInLightStates].moved : scene_inputs::kInputs[scene_inputs::kInLightStates].moved), "a4.light_moved_follows_flag");
        {   // ---- S103 A5: holes are classified (lapsed vs never drawn), never filled; a model drawn with NO install has no default light set to inherit
          lightidx::Table ta;
          ta.OnInstall(0, 4, 1);
          ta.OnInstallObject(0, 0xD0000);
          ta.OnModel(0x71000, 0, 100);   // drawn, then not confirmed for longer than kMaxAge
          std::vector<NodeLight> sa;
          const uint32_t later = 100 + lightidx::kMaxAge + 10;
          ta.Snapshot(later, sa);          // the entry is erased here
          std::vector<uint32_t> ev;
          ta.Ever(ev);
          const lightidx::Cover ca = lightidx::Coverage(sa, std::vector<uint32_t>{0x71000, 0x72000}, later, Plant("a5_ever_ignored") ? nullptr : &ev);
          Check(ca.missing == 2 && ca.lapsed == 1 && ca.never == 1 && !ca.ok(), "a5.hole_lapsed_vs_never", "lapsed " + std::to_string(ca.lapsed) + " never " + std::to_string(ca.never));
          ta.Clear();
          std::vector<uint32_t> ev2;
          ta.Ever(ev2);
          Check(ev2.empty(), "a5.ever_cleared_with_table");
          // a model whose slot has had NO install since the stage (sub_826203E8 with a negative index installs nothing, so no light is changed) is NOT given a default entry: it stays a hole and is counted
          lightidx::Table tn;
          if (Plant("a5_default_filled")) tn.OnInstall(0, 4, 1), tn.OnInstallObject(0, 0xD0000);   // the twin: a default install is invented for it
          tn.OnModel(0x73000, 0, 100);
          std::vector<NodeLight> sn;
          tn.Snapshot(100, sn);
          const lightidx::Cover cn = lightidx::Coverage(sn, std::vector<uint32_t>{0x73000}, 100);
          Check(sn.empty() && cn.missing == 1 && tn.GetStats().no_install == 1, "a5.no_install_stays_hole", "missing " + std::to_string(cn.missing));
          // bit 4 asks only about the slot models the record EMITS: slot 7's model is hidden by the look (not emitted), slot 3's is emitted without an entry (a hole), slot 5's is emitted with one
          lightidx::Table te;
          te.OnInstall(0, 4, 1);
          te.OnInstallObject(0, 0xD0000);
          te.OnModel(0x75000, 0, 100);
          std::vector<NodeLight> se;
          te.Snapshot(100, se);
          const std::vector<uint32_t> models{0x73000, 0x75000, 0x77000};
          const std::vector<uint16_t> slots{3, 5, 7};
          const std::vector<uint16_t> emitted = Plant("a5_ask_all") ? std::vector<uint16_t>{3, 5, 7} : std::vector<uint16_t>{3, 5};
          const lightidx::Cover ce = lightidx::Coverage(se, lightidx::FilterEmitted(models, slots, emitted), 100);
          Check(ce.drawn == 2 && ce.missing == 1 && ce.first_missing[0] == 0x73000, "a5.only_emitted_asked", "drawn " + std::to_string(ce.drawn) + " missing " + std::to_string(ce.missing));
          const lightidx::Cover cx = lightidx::Coverage(se, lightidx::FilterEmitted(models, slots, std::vector<uint16_t>{5}), 100);
          Check(cx.ok() || Plant("a5_emitted_hole_dropped"), "a5.all_emitted_covered", "missing " + std::to_string(cx.missing));
          {   // ---- bit 4 over what OUR frame can see: a synthetic row-vector perspective camera at the origin looking down +z (D3D depth), fov 90, aspect 1, near 1, far 100
            const float zn = 1.f, zf = 100.f, q = zf / (zf - zn);
            const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, q, 1, 0, 0, -zn * q, 0};
            const lightidx::Frustum fr = lightidx::FrustumFromViewProj(view, proj);
            lightidx::Sphere on, off, unk, behind;
            on.c[2] = 10.f, on.r = 1.f;                         // straight ahead
            off.c[0] = 500.f, off.c[2] = 10.f, off.r = 1.f;     // far to the right of a 90 degree view
            behind.c[2] = -20.f, behind.r = 1.f;                // behind the camera
            Check(fr.ok && lightidx::SphereMaybeVisible(fr, on) && !lightidx::SphereMaybeVisible(fr, off) && !lightidx::SphereMaybeVisible(fr, behind) && lightidx::SphereMaybeVisible(fr, unk), "a5.frustum_sphere");
            // 0x73000 on screen WITHOUT an entry, 0x75000 on screen with one, 0x77000 off screen without one
            const std::vector<lightidx::Sphere> sp{on, on, Plant("a5_offscreen_asked") ? on : off};
            uint32_t offc = 0;
            const lightidx::Cover cv2 = lightidx::Coverage(se, lightidx::FilterVisible(models, sp, fr, &offc), 100);
            Check(cv2.drawn == 2u && offc == 1u, "a5.offscreen_not_asked", "drawn " + std::to_string(cv2.drawn));
            Check(cv2.missing >= 1 && cv2.first_missing[0] == 0x73000 && !cv2.ok(), "a5.onscreen_without_entry_is_hole", "missing " + std::to_string(cv2.missing));
            const std::vector<lightidx::Sphere> unknown_b(3);   // all bounds unknown: nothing is dropped
            Check(lightidx::FilterVisible(models, unknown_b, fr).size() == 3, "a5.unknown_bounds_asked");
          }
          Check(!lightidx::Coverage(se, lightidx::FilterEmitted(models, slots, Plant("a5_emitted_hole_dropped") ? std::vector<uint16_t>{5} : std::vector<uint16_t>{3}), 100).ok(), "a5.emitted_without_entry_is_hole");
        }
      }
      rdr_scene::live::lightstamp::g_on = false;
      gt.Clear();
      scene_inputs::g_stamp_light_states = false;
      scene_inputs::g_light_map_consumer = false;
      BuildGuest();
    }
    // ---- review F3: the derived-palette guard (bit 3) and piece liveness
    {
      constexpr uint32_t kPal = 0x40005000, kCount = 3;
      for (uint32_t i = 0; i < kCount; ++i)
        for (uint32_t j = 0; j < 16; ++j) g_guest.PutF(kPal + 64 * i + 4 * j, 100.f * i + j + 0.25f);
      g_guest.Put(kM0 + scene_inputs::kPaletteOff, kPal);
      static live::WrestlerContributor* W;
      static live::inputs::InputsContributor* PC;
      static int64_t g_accept = live::WrestlerContributor::kDeltaUnsettled;
      // S103 L33F: the guard-machinery checks below (which head is evidence, the accepted delta, the shift) were written for the strict rule (a mismatch withholds by itself) and run under it (the test fault
      // kFaultStrictDerived); the shipped rule (a mismatch falls through to the walk copy; no walk copy = submitted) has its own checks after them (l33.*). g_walk: 0 no walk copy, 1 equal to the latch read, 2 differs.
      static uint32_t g_faults = live::WrestlerContributor::kFaultStrictDerived;
      static int g_walk = 0;
      static bool g_slot = false;   // S103 L33F: the targets come from the character table (slot 0 holds kM0), as the game's glue does (SetSlotPalettes)
      static uint64_t g_walk_same = 0, g_walk_differs = 0, g_submitted = 0, g_no_evidence = 0, g_nowalk_sub = 0;
      static bool g_nowalk = false;   // S103 AUSF: the contributor runs as the stream-free build
      static std::vector<int> g_pre;   // S103 L33F review F3: earlier WALKED latches of the same body under the same mismatch, each with its walk copy: 1 equal to the latch read, 2 differs
      auto run = [&](bool derived_equal_older, bool updates_in_off_mode, uint64_t& st, uint64_t& mismatch, uint64_t& unchecked, uint64_t only_head_tag = 0, bool head_bad = false, float shift = 0.f, bool apply_shift = true) {   // S103 W1: only_head_tag != 0 = ONE equal derived head at that tag
        live::WrestlerContributor w;
        W = &w;
        w.SetFaultsForTest(g_faults);   // S103 L33F
        w.SetNoWalk(g_nowalk);          // S103 AUSF
        if (g_accept != live::WrestlerContributor::kDeltaUnsettled) w.SetAcceptedHeadDelta(g_accept);
        live::inputs::InputsContributor pc;
        PC = &pc;
        pc.SetReader(ReadThunk);
        pc.SetPalettes([](uint64_t hook, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) {
                         std::vector<live::WrestlerContributor::PaletteTarget> t;
                         if (g_slot) {
                           live::WrestlerContributor::SlotTable st;
                           st.skels.push_back({kM0, 0});
                           st.state[0] = live::WrestlerContributor::kSlotOccupied;
                           W->LatchTargets(hook, &st, t, missing);
                         } else {
                           W->LatchTargets(hook, t, missing);
                         }
                         out.clear();
                         for (const auto& x : t) out.push_back({x.skel, x.count});
                       },
                       [](uint32_t m, uint32_t n, const Mat4* skin, uint64_t f) { return W->SubmitLatchConsumer(m, n, skin, f); });
        std::vector<Mat4> good(kCount), bad(kCount);
        for (size_t i = 0; i < kCount; ++i)
          for (int j = 0; j < 16; ++j) good[i].m[j] = 100.f * float(i) + float(j) + 0.25f, bad[i].m[j] = 7.f;
        uint64_t pre_mm = 0, pre_sub = 0;
        if (!g_pre.empty()) {   // S103 L33F review F3: the walked latches (hooks 999, 1000, ...) that give (or take back) this body's evidence
          g_mode = 2;
          for (size_t k = 0; k < g_pre.size(); ++k) {
            const uint64_t ph = 999 + k;
            w.SubmitSkin(kM0, 0, kCount, bad.data(), ph);   // the derived head of that hook: garbage (the mismatch)
            w.SubmitConsumer(kM0, kCount, (g_pre[k] == 1 ? good : bad).data(), ph + live::WrestlerContributor::kPaletteLead);
            pc.LatchPalettes(ph + 1);
          }
          pre_mm = w.DerivedMismatch(), pre_sub = w.MismatchSubmitted();
        }
        ResetStamps();
        g_mode = updates_in_off_mode ? 0 : 2;   // F7: in mode Off the update hook keeps no derived head
        if (only_head_tag) {
          std::vector<Mat4> head = head_bad ? bad : good;   // S103 W1: shift != 0 = the update-exit palette is BEFORE a sub_82BD3620 shift of (shift, shift, shift) the latch read already has
          for (Mat4& m : head) m.m[12] -= shift, m.m[13] -= shift, m.m[14] -= shift;
          w.SubmitSkin(kM0, 0, kCount, head.data(), only_head_tag);
          if (shift != 0.f && apply_shift) w.ApplyDelta(kM0, shift, shift, shift);   // the existing wwe2k14_skel_shift hook
        } else if (derived_equal_older) {
          w.SubmitSkin(kM0, 0, kCount, good.data(), 1000);   // three frames back: equal
          w.SubmitSkin(kM0, 0, kCount, bad.data(), 1001);    // the newer derived palettes: garbage
          w.SubmitSkin(kM0, 0, kCount, bad.data(), 1002);
        } else {
          w.SubmitSkin(kM0, 0, kCount, bad.data(), 1002);
        }
        g_mode = 2;
        if (g_walk) w.SubmitConsumer(kM0, kCount, (g_walk == 1 ? good : bad).data(), 1003 + live::WrestlerContributor::kPaletteLead);   // S103 L33F: the walk's own copy of the same content (a walked frame)
        pc.LatchPalettes(1004);   // hook frame 1003
        st = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInWrestlerPalette].load();
        mismatch = w.DerivedMismatch() - pre_mm, unchecked = w.DerivedUnchecked();
        g_walk_same = w.MismatchWalkSame(), g_walk_differs = w.MismatchWalkDiffers(), g_submitted = w.MismatchSubmitted() - pre_sub, g_no_evidence = w.MismatchNoEvidence(), g_nowalk_sub = w.MismatchSubmittedNoWalk();
      };
      {  // S103 HGF (XLB items 3 and 4): the latch reads a spring piece's rows ONCE (LatchPalettes' targets are reused by Take for the param targets); the off-latch fallback take reads no table / piece rows and asks no slot targets
        constexpr uint32_t kCh2 = 0x40800000, kNode = 0x40801000, kComp = 0x40802000, kArr = 0x40803000, kMap = 0x40804000, kModel = 0x70002000, kPalB = 0x40805000, kSk2 = 0x70001000;
        const Guest saved_guest = g_guest;
        rdr_scene::g_spring_hair_rows.store(true);
        g_guest = Guest{};
        for (uint32_t i = 0; i < 9; ++i) g_guest.Put(live::lightlatch::kCharSlotTable + 4 * i, i == 0 ? kCh2 : 0u);
        g_guest.Put(kCh2 + live::lightlatch::kCharSkeletonOff, kSk2);
        g_guest.Put(kCh2 + 1152 + 20, kNode), g_guest.Put(kNode + 4, scene_inputs::kSubmTag), g_guest.Put(kNode + 20, 0), g_guest.Put(kNode + 24, kComp);
        g_guest.Put(kComp + 72, kArr), g_guest.Put(kComp + 80, kArr + 48);
        g_guest.Put(kArr + 0, kModel), g_guest.Put(kArr + 16, 20002u << 16), g_guest.Put(kArr + 12, kMap), g_guest.Put(kArr + 24, 1), g_guest.Put(kArr + 28, 1), g_guest.Put(kArr + 36, 0x00010000u);
        for (uint32_t i = 0; i < 11; ++i) g_guest.Put(kMap + 4 * i, i == 3 ? 0xFFFFFFFFu : i);
        g_guest.Put(kModel + 200, kPalB);
        for (uint32_t k = 0; k < 16; ++k) g_guest.PutF(kPalB + 64 * 3 + 4 * k, 7.f + float(k));
        static uint32_t slot_calls, table_reads;
        const auto latch_once = [&](uint32_t faults, uint64_t rec, uint64_t& rows_latch, uint64_t& rows_off, uint32_t& calls_latch, uint32_t& calls_off, uint32_t& tbl_latch, uint32_t& tbl_off) {
          live::inputs::InputsContributor pc;
          pc.SetReader([](uint32_t a, void* d, size_t n) { if (a == live::lightlatch::kCharSlotTable) ++table_reads; for (size_t i = 0; i < n; i += 4) if (!g_guest.Read(a + uint32_t(i), static_cast<uint8_t*>(d) + i, n - i < 4 ? n - i : 4)) std::memset(static_cast<uint8_t*>(d) + i, 0, n - i < 4 ? n - i : 4); return true; });   // S108 RDS: word by word (an unset word reads 0, its set neighbours keep their bytes: a block read of one object must see what the field reads saw)   // an unset guest word reads 0 (like wrestler_emit's fake)
          pc.SetPieceNodeCount([](uint32_t) { return 11u; });
          pc.SetSlotPalettes([](uint64_t, uint64_t, const live::inputs::InputsContributor::SlotTable&, std::vector<live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing, uint32_t* stale_visible, bool) {
                               ++slot_calls;
                               missing = 0;
                               if (stale_visible) *stale_visible = 0;
                               out.clear();
                               out.push_back({kSk2, 3});
                             },
                             [](uint32_t, uint32_t, const Mat4*, uint64_t) { return 0; });
          pc.SetHgfFaultsForTest(faults);
          g_mode = 2;
          slot_calls = table_reads = 0;
          const uint64_t r0 = scene_inputs::g_piece_rows_reads.load();
          pc.LatchPalettes(rec);
          FrameInfo fi;
          fi.guest_frame = rec;
          pc.Latch(fi);
          rows_latch = scene_inputs::g_piece_rows_reads.load() - r0, calls_latch = slot_calls, tbl_latch = table_reads;
          slot_calls = table_reads = 0;
          const uint64_t r1 = scene_inputs::g_piece_rows_reads.load();
          FrameInfo other;
          other.guest_frame = rec + 5;   // never latched: Emit takes the off-latch fallback
          SceneTables t;
          Counters c;
          pc.Emit(other, t, c);
          pc.LatchPalettes(rec + 9, false);   // review 2: the Wrestlers emit fallback's pre-latch, off the latch thread: palettes only (no slot table, no piece rows, no LatchTargets(slots))
          rows_off = scene_inputs::g_piece_rows_reads.load() - r1, calls_off = slot_calls, tbl_off = table_reads;
        };
        uint64_t rl = 0, ro = 0;
        uint32_t cl = 0, co = 0, tl = 0, to = 0;
        latch_once(0, 500, rl, ro, cl, co, tl, to);
        Check(rl == 1 && cl == 1, "hgf.latch_reads_each_piece_once", "ReadPieceRows " + std::to_string((unsigned long long)rl) + " slot-target calls " + std::to_string(cl) + " (table reads " + std::to_string(tl) + ")");
        Check(ro == 0 && co == 0, "hgf.off_latch_take_and_prelatch_read_no_pieces", "ReadPieceRows " + std::to_string((unsigned long long)ro) + " slot-target calls " + std::to_string(co) + " (table reads " + std::to_string(to) + ")");
        latch_once(live::inputs::InputsContributor::kHgfNoTargetReuse, 510, rl, ro, cl, co, tl, to);   // planted twin: the old double read
        Check(rl == 2 && cl == 2, "hgf.twin_double_read_is_seen", "ReadPieceRows " + std::to_string((unsigned long long)rl) + " (the old rule reads twice: the first check above is what stops it)");
        latch_once(live::inputs::InputsContributor::kHgfOffLatchReads, 520, rl, ro, cl, co, tl, to);   // planted twin: the old fallback re-read
        Check(ro == 2 && co == 2, "hgf.twin_off_latch_read_is_seen", "ReadPieceRows " + std::to_string((unsigned long long)ro) + " slot-target calls " + std::to_string(co) + " (the old fallback, Take and the pre-latch, reads and replaces the piece state: the second check above is what stops it)");
        g_guest = saved_guest;
      }
      uint64_t st = 0, mm = 0, un = 0;
      run(false, Plant("a3_no_derived_head"), st, mm, un);   // the planted fault: the update hook's heads never got made, so the mismatch cannot be seen
      Check(st != 1003 && mm == 1, "a3.palette_derived_mismatch_withholds", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm));
      if (!Plant("a3_no_derived_head")) {   // F7: updates seen in mode Off leave no head: nothing is compared (unchecked), no false mismatch
        run(false, true, st, mm, un);
        Check(mm == 0 && un == 1, "a3.derived_head_off_mode", "mismatch " + std::to_string((unsigned long long)mm) + " unchecked " + std::to_string((unsigned long long)un));
      }
      run(true, false, st, mm, un);   // the older head (frame 1000, 3 hook frames back = outside the window of 4?) kConsumerFresh = 4: matched through the ring
      Check(mm == 0 && st == 1003, "a3.palette_derived_older_head_matches", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm));
      g_accept = 1;   // review must 7: the mirror run settled delta +1; the equal head is at delta 3, so it no longer passes
      run(true, false, st, mm, un);
      Check(st != 1003 && mm == 1, "a3.palette_accepted_delta_only", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm));
      g_accept = 3;
      run(true, false, st, mm, un);
      Check(st == 1003 && mm == 0, "a3.palette_accepted_delta_matches");
      // S103 W1: the shipped value (inputs_glue.h): the latch at hook H reads the content tagged H + kPaletteLead, so its equal derived head sits at H - head = -kPaletteLead (run 20 measured -1). The latch's own
      // tagging (hook + lead) leaves this delta alone: the guard compares the PARAM hook with the update hook's tag.
      const int64_t lead = int64_t(live::WrestlerContributor::kPaletteLead);
      g_accept = -lead;
      run(true, false, st, mm, un, 1003 + uint64_t(lead));   // the equal head at the accepted delta: compared, matched
      Check(mm == 0 && un == 0 && st == 1003, "w1.guard_accepts_minus_lead_head", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm) + " unchecked " + std::to_string((unsigned long long)un));
      run(true, false, st, mm, un, 1003 + uint64_t(lead), true);   // a GARBAGE head at the accepted delta: the guard sees it and withholds the stamp
      Check(st != 1003 && mm == 1, "w1.guard_catches_bad_head", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm));
      run(true, false, st, mm, un, 1003, true);   // a stale head (tag = hook, delta 0), garbage: refused as evidence (not compared, counted unchecked): it cannot withhold a good frame
      Check(mm == 0 && un == 1, "w1.guard_ignores_stale_head", "mismatch " + std::to_string((unsigned long long)mm) + " unchecked " + std::to_string((unsigned long long)un));
      // S103 W1 (option a): a sub_82BD3620 shift after the update exit. The latch read (the guest block) already has it; the derived head gets it from ApplyDelta (the wwe2k14_skel_shift hook).
      run(true, false, st, mm, un, 1003 + uint64_t(lead), false, 12.5f, true);
      Check(mm == 0 && un == 0 && st == 1003, "w1.shifted_frame_passes_with_delta", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm));
      run(true, false, st, mm, un, 1003 + uint64_t(lead), false, 12.5f, false);   // planted twin: the old guard (no delta on the head) refuses the same good frame
      Check(st != 1003 && mm == 1, "w1.twin_shifted_frame_without_delta_refused", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm));
      // a stale latch read (the guest block holds the palette of frame j - 5) against the fresh derived head still withholds; the delta cannot launder it
      run(true, false, st, mm, un, 1003 + uint64_t(lead), true, 12.5f, true);
      Check(st != 1003 && mm == 1, "w1.stale_latch_palette_withheld_after_delta", "stamp " + std::to_string((unsigned long long)st) + " mismatch " + std::to_string((unsigned long long)mm));
      run(true, false, st, mm, un, 1003 + uint64_t(lead) - 5, false);   // the head of frame j - 5 is not accepted as evidence (delta -1 only): unchecked, never a pass by itself
      Check(mm == 0 && un == 1, "w1.head_j_minus_5_is_not_evidence", "mismatch " + std::to_string((unsigned long long)mm) + " unchecked " + std::to_string((unsigned long long)un));
      g_accept = lead;   // planted twin: the sign taken the other way round never sees the real head's garbage
      run(true, false, st, mm, un, 1003 + uint64_t(lead), true);
      Check(mm == 0 && un == 1, "w1.twin_wrong_sign_misses_bad_head", "mismatch " + std::to_string((unsigned long long)mm) + " unchecked " + std::to_string((unsigned long long)un));
      g_accept = live::WrestlerContributor::kDeltaUnsettled;
      {   // S103 L33F (run 32: kDerivedMismatch for both wrestlers every match frame, all walked): the shipped rule. A garbage derived head at 1002, the guest block = the good palette.
        const auto S = [](uint64_t v) { return std::to_string((unsigned long long)v); };
        g_faults = Plant("l33_strict_derived") ? live::WrestlerContributor::kFaultStrictDerived : 0u;   // the twin: the old strict rule
        g_walk = 1;   // a walked frame whose walk copy equals the latch read: accepted, stamped, the mismatch counted
        run(false, false, st, mm, un);
        Check(st == 1003 && mm == 1 && g_walk_same == 1 && g_submitted == 0, "l33.mismatch_walk_equal_accepted", "stamp " + S(st) + " mismatch " + S(mm) + " walk_same " + S(g_walk_same));
        g_walk = 0;   // an OURS frame (no walk copy), a body the character table holds, AND an earlier walked frame showed its latch read equal to the walk copy under the mismatch: submitted, stamped, census only
        g_slot = true;
        g_pre = {1};
        run(false, false, st, mm, un);
        Check(st == 1003 && mm == 1 && g_submitted == 1, "l33.mismatch_ours_submitted_counted", "stamp " + S(st) + " mismatch " + S(mm) + " submitted " + S(g_submitted));
        g_pre.clear();   // review F3: no such walked frame yet (no evidence for this body): refused
        if (Plant("l33_no_evidence")) g_faults |= live::WrestlerContributor::kFaultNoMismatchEvidence;   // the twin: submit without the evidence
        run(false, false, st, mm, un);
        Check(st != 1003 && mm == 1 && g_submitted == 0 && g_no_evidence == 1, "l33.mismatch_ours_no_evidence_refused", "stamp " + S(st) + " submitted " + S(g_submitted) + " no_evidence " + S(g_no_evidence));
        g_faults &= ~live::WrestlerContributor::kFaultNoMismatchEvidence;
        g_pre = {1, 2};   // the evidence, then a walked frame whose walk copy DIFFERS: the flag is cleared, the OURS read is refused
        run(false, false, st, mm, un);
        Check(st != 1003 && mm == 1 && g_submitted == 0, "l33.mismatch_evidence_cleared_by_differs", "stamp " + S(st) + " submitted " + S(g_submitted));
        g_pre = {1};
        g_slot = false;   // the evidence, but no character table behind the target (hook-liveness only: e.g. a body that left the table): refused, fail closed (the l31b.left_table case)
        run(false, false, st, mm, un);
        Check(st != 1003 && mm == 1 && g_submitted == 0, "l33.mismatch_ours_not_in_table_refused", "stamp " + S(st) + " submitted " + S(g_submitted));
        g_pre.clear();
        g_slot = true;
        g_faults = 0;
        g_walk = 2;   // the walk copy differs too: both references disagree with the latch read: withheld
        run(false, false, st, mm, un);
        Check(st != 1003 && mm == 1 && g_walk_differs == 1, "l33.mismatch_walk_differs_withheld", "stamp " + S(st) + " walk_differs " + S(g_walk_differs));
        g_faults = live::WrestlerContributor::kFaultStrictDerived;   // the strict rule itself, run on purpose: it rejects both accepted cases above
        g_walk = 1;
        run(false, false, st, mm, un);
        const bool strict_walk = st != 1003;
        g_walk = 0;
        run(false, false, st, mm, un);
        Check(strict_walk && st != 1003 && mm == 1, "l33.strict_rule_rejects_both", "stamp " + S(st));
        // S103 AUSF (AUS2 fix 1): the stream-free build (no walk, so no walk evidence EVER). A mismatch with no walk copy for a body the table holds is SUBMITTED (the draw's own source), counted apart; a body not in
        // the table is still refused. Twin ausf_refuse = today's refusal in that build ("l33_ausf.nowalk_mismatch_submitted" fails: the body leaves the record). The stream build (g_nowalk false) is the l33.* above, unchanged.
        g_nowalk = true;
        g_walk = 0;
        g_pre.clear();
        g_slot = true;
        g_faults = Plant("ausf_refuse") ? live::WrestlerContributor::kFaultNoWalkRefuse : 0u;
        run(false, false, st, mm, un);
        Check(st == 1003 && mm == 1 && g_nowalk_sub == 1 && g_no_evidence == 0 && g_submitted == 0, "l33_ausf.nowalk_mismatch_submitted", "stamp " + S(st) + " nowalk_submitted " + S(g_nowalk_sub) + " no_evidence " + S(g_no_evidence));
        g_slot = false;   // not held by the table: refused even with no walk (fail closed)
        run(false, false, st, mm, un);
        Check(st != 1003 && g_nowalk_sub == 0, "l33_ausf.nowalk_not_in_table_refused", "stamp " + S(st) + " nowalk_submitted " + S(g_nowalk_sub));
        g_slot = true;   // review 1: the same read made OFF the latch thread: refused
        g_faults = Plant("ausf_offlatch") ? live::WrestlerContributor::kFaultHoldOffLatch : 0u;
        live::WrestlerContributor::SetOffLatchThread(true);
        run(false, false, st, mm, un);
        live::WrestlerContributor::SetOffLatchThread(false);
        Check(st != 1003 && g_nowalk_sub == 0 && g_no_evidence == 1, "l33_ausf.offlatch_mismatch_refused", "stamp " + S(st) + " nowalk_submitted " + S(g_nowalk_sub) + " no_evidence " + S(g_no_evidence));
        g_faults = 0;
        g_nowalk = false;   // the stream build: the old refusal, unchanged
        g_faults = 0;
        g_slot = true;
        run(false, false, st, mm, un);
        Check(st != 1003 && g_nowalk_sub == 0 && g_no_evidence == 1, "l33_ausf.stream_build_refusal_unchanged", "stamp " + S(st) + " nowalk_submitted " + S(g_nowalk_sub) + " no_evidence " + S(g_no_evidence));
        g_slot = false;
      }
    }
    {   // piece liveness: a piece is fed only while ITS OWNER body is alive (not merely while some body is)
      live::WrestlerContributor w;
      std::vector<Mat4> nodes(60);
      for (auto& m : nodes) std::memset(m.m, 0, sizeof m.m);
      constexpr uint32_t kA = 0x40200000, kB = 0x40201000, kP = 0x40202000;
      for (uint64_t f = 5; f <= 99; ++f) w.SubmitSkin(kA, 0, 60, nodes.data(), f);   // body A keeps updating
      for (uint64_t f = 5; f <= 10; ++f) w.SubmitSkin(kB, 1, 60, nodes.data(), f);   // body B stopped at hook frame 10
      if (!Plant("a3_piece_unowned")) {
        std::vector<live::WrestlerContributor::PieceRef> pieces;
        live::WrestlerContributor::PieceRef pr;
        pr.model = kP, pr.id = 1;
        pieces.push_back(pr);
        w.SubmitModelLink(kB, 5001, 0, 0, 0, std::move(pieces));   // the game's descriptor lists P under B
      }
      w.WantSkeleton(kP, 3);
      std::vector<Mat4> small(3);
      for (auto& m : small) std::memset(m.m, 0, sizeof m.m);
      w.SubmitConsumer(kP, 3, small.data(), 99);
      FrameInfo fi;
      fi.guest_frame = 100;
      w.Latch(fi);
      std::vector<live::WrestlerContributor::PaletteTarget> tg;
      uint32_t missing = 0;
      w.LatchTargets(99, tg, missing);
      bool has_a = false, has_b = false, has_p = false;
      for (const auto& t : tg) has_a = has_a || t.skel == kA, has_b = has_b || t.skel == kB, has_p = has_p || t.skel == kP;
      Check(has_a && !has_b && !has_p, "a3.piece_owner_dead_not_fed", "A " + std::to_string(has_a) + " B " + std::to_string(has_b) + " P " + std::to_string(has_p));
    }
    // ---- review F4 / F5 / small items
    {
      // F4: on a WALKED frame the walk recorded no circle row but the latch wrote one: latch_only_frames; an OURS frame counts nothing
      live::circles::CircleContributor cc;
      FrameInfo fi;
      fi.guest_frame = 3001;
      cc.Latch(fi);
      SceneTables t;
      CircleShadow l;
      l.flags = kCircleFromLatch;
      t.circle_shadows.push_back(l);
      rdr_own::OwnedPassStamp stamp;
      stamp.pass = uint8_t(rdr_own::Pass::Scene3D);
      stamp.guest_frame = 3000;   // the record's hook frame
      stamp.decision = uint8_t(Plant("a3_walked_as_ours") ? rdr_own::Decision::Ours : rdr_own::Decision::GameWalks);
      rdr_own::g_board.Put(stamp);
      Counters c;
      cc.Emit(fi, t, c);
      Check(cc.LatchOnlyFrames() == 1, "a3.circle_latch_only_frames", "frames " + std::to_string((unsigned long long)cc.LatchOnlyFrames()));
      live::circles::CircleContributor cc2;
      FrameInfo fo;
      fo.guest_frame = 3011;
      cc2.Latch(fo);
      SceneTables t2;
      t2.circle_shadows.push_back(l);
      rdr_own::OwnedPassStamp s2 = stamp;
      s2.guest_frame = 3010, s2.decision = uint8_t(rdr_own::Decision::Ours);
      rdr_own::g_board.Put(s2);
      cc2.Emit(fo, t2, c);
      Check(cc2.LatchOnlyFrames() == 0, "a3.circle_ours_frame_counts_nothing");
      live::circles::CircleContributor cc3;   // mode 1 has no Scene3D stamp for the frame at all: a frame is walked unless a stamp says Ours (review must 3)
      FrameInfo f3;
      f3.guest_frame = 3031;
      cc3.Latch(f3);
      SceneTables t3;
      t3.circle_shadows.push_back(l);
      cc3.Emit(f3, t3, c);
      Check(cc3.LatchOnlyFrames() == 1, "a3.circle_latch_only_frames_mode1");
      // F5: the stage clock is a REGISTERED required input that nothing stamps (moved = false)
      scene_inputs::RegisterAll();
      Check(scene_inputs::kInputs[scene_inputs::kInStageClock].moved && (rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load() & (1u << scene_inputs::kInStageClock)) && scene_inputs::kInputCount == 18, "a3.stage_clock_required_moved");
      {   // S103 L1C: the run-time hold (--wwe2k14_rdr_scene_hold): parsing, a held bit is never stamped, bit 4's hold turns its feeding switches off
        namespace hold = rdr_scene::live::hold;
        std::string bad;
        Check(hold::ParseHoldList("7,17", &bad) == ((1u << 7) | (1u << 17)) && bad.empty(), "hold.parse_bits");
        Check(hold::ParseHoldList("particles, stage_clock;crowd_snapshot") == ((1u << 11) | (1u << 17) | (1u << 10)), "hold.parse_names");
        bad.clear();
        Check(hold::ParseHoldList("") == 0 && hold::ParseHoldList("99 xyz 4", &bad) == (1u << 4) && bad == "99 xyz", "hold.parse_bad_tokens_reported");
        const bool sv_g_on = rdr_scene::live::lightstamp::g_on, sv_ids = rdr_scene::live::lightstamp::g_from_ids, sv_wl = rdr_scene::live::wlatch::g_on, sv_cons = scene_inputs::g_light_map_consumer;
        ResetStamps();
        if (!Plant("hold_ignored")) hold::ApplyHold(1u << scene_inputs::kInCasterEntries);
        scene_inputs::Capture(scene_inputs::kInCasterEntries, 777);
        scene_inputs::Capture(scene_inputs::kInStageClock, 777);
        Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInCasterEntries].load() != 777 && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInStageClock].load() == 777 &&
                  scene_inputs::g_held_drops[scene_inputs::kInCasterEntries].load() >= 1,
              "hold.drops_the_held_stamp_only");
        rdr_scene::live::lightstamp::g_on = rdr_scene::live::lightstamp::g_from_ids = rdr_scene::live::wlatch::g_on = scene_inputs::g_light_map_consumer = true;
        hold::ApplyHold(1u << scene_inputs::kInLightStates);
        Check(!rdr_scene::live::lightstamp::g_on && !rdr_scene::live::lightstamp::g_from_ids && !rdr_scene::live::wlatch::g_on && !scene_inputs::g_light_map_consumer, "hold.bit4_turns_its_feeds_off");
        hold::ApplyHold(0);
        {   // S103 L33F: the dev-only un-require (--wwe2k14_rdr_scene_unrequire): the same parser; the named bits leave the required mask (and stay out after a later registration); a release build ignores it
          auto& rq = rdr_own::S(rdr_own::Pass::Scene3D).required_inputs;
          const uint32_t sv_req = rq.load();
          scene_inputs::RegisterAll();
          const uint32_t before = rq.load();
          const uint32_t m = hold::ParseHoldList("light_states, 3");
          Check(m == ((1u << 4) | (1u << 3)) && (before & m) == m, "unrequire.parse");
          Check(hold::ApplyUnrequire(m, /*dev=*/false) == 0 && rq.load() == before && scene_frame::g_unrequire_mask.load() == 0, "unrequire.ignored_in_release");
          hold::ApplyUnrequire(Plant("unrequire_ignored") ? 0u : m, /*dev=*/true);
          scene_inputs::RegisterAll();   // a registration after the switch must not bring them back
          const uint32_t after = rq.load();
          Check(after == (before & ~m) && after != 0, "unrequire.drops_named_bits_only", "before " + std::to_string(before) + " after " + std::to_string(after));
          hold::ApplyUnrequire(0, true);
          rq.store(sv_req);
        }
        rdr_scene::live::lightstamp::g_on = sv_g_on, rdr_scene::live::lightstamp::g_from_ids = sv_ids, rdr_scene::live::wlatch::g_on = sv_wl, scene_inputs::g_light_map_consumer = sv_cons;
        ResetStamps();
      }
      // small: a manager ref that points at a model-map row is refused; the section's layout is 2
      SceneTables lt;
      LightObjectRow m;
      m.kind = kLoManager, m.frame = 1;
      for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
      LightObjectRow mp;
      mp.kind = kLoModelMap, mp.frame = 1, mp.aux0 = 1;
      m.words[kLmRefBase + 0] = 1;   // set 0 -> row 1 = the map row
      lt.light_objects.push_back(m), lt.light_objects.push_back(mp);
      std::vector<uint8_t> by = SerializeRecord(lt);
      SceneTables back;
      std::string er;
      Check(kLightObjectLayout == 3 && ParseRecord(by.data(), by.size(), back, er) && back.light_errors == 1 && back.light_objects.empty(), "a3.light_ref_to_map_row_refused", er);
      // SlotOf: a thread with no block is census-visible
      constexpr uint32_t kR13 = 0x400F9000, kTls = 0x400F9100;
      g_guest.Put(kR13, kTls), g_guest.Put(kTls + 1048, 0);
      uint32_t slot = 9;
      bool nb = false;
      const bool ok = live::lightidx::SlotOf(+[](uint32_t a, uint32_t& v) { uint32_t raw; if (!g_guest.Read(a, &raw, 4)) return false; v = scene_inputs::Swap32(raw); return true; }, kR13, slot, &nb);
      Check(ok && slot == 0 && nb, "a3.light_slot_null_block_census");
    }
    {   // review must 1: the ONE accessor of the model map; must 8: layout 1 (A2's, older captures) is still accepted as "no map rows"
      LightObjectRow r;
      for (uint32_t k = 0; k < kLightModelsPerRow; ++k) SetModelMapEntry(r.words, k, ModelMapEntry{0x1000u + k, 0x2000u + k, 3u + k, 1u});
      bool ok = true;
      for (uint32_t k = 0; k < kLightModelsPerRow; ++k) {
        const ModelMapEntry e = GetModelMapEntry(r.words, k);
        ok = ok && e.model == 0x1000u + k && e.object == 0x2000u + k && e.index == 3u + k && e.mode == 1u;
      }
      Check(ok, "a3.model_map_accessor");
      SceneTables lt;
      LightObjectRow m;
      m.kind = kLoManager, m.frame = 1, m.layout = 1;
      for (uint32_t i = 0; i < kLmRefCount; ++i) m.words[kLmRefBase + i] = kLightNoRow;
      lt.light_objects.push_back(m);
      std::vector<uint8_t> by = SerializeRecord(lt);
      SceneTables back;
      std::string er;
      Check(ParseRecord(by.data(), by.size(), back, er) && back.light_errors == 0 && back.light_objects.size() == 1, "a3.light_layout1_accepted", er);
      LightObjectRow bad;
      bad.kind = kLoModelMap, bad.frame = 1, bad.layout = 1, bad.aux0 = 1;
      lt.light_objects.push_back(bad);
      by = SerializeRecord(lt);
      SceneTables back2;
      Check(ParseRecord(by.data(), by.size(), back2, er) && back2.light_errors == 1, "a3.light_layout1_with_map_row_refused");
    }
    {   // the census says what blocks the frame: every open input has an owner and is named in the line
      bool owners = true, named = true;
      const std::string line = scene_inputs::CensusLine();
      for (const auto& i : scene_inputs::kInputs)
        if (!i.moved) owners = owners && i.owner && i.owner[0], named = named && line.find(std::string(i.name) + " (" + i.owner + ")") != std::string::npos;
      Check(owners && named && line.find("[blocks the frame:") != std::string::npos, "a3.census_blocks", line.substr(line.find("[blocks the frame:")));
    }
    BuildGuest();
  }

  // ---------------------------------------------------------------- 3b. the render camera (K1)
  {
    Camera cam;
    scene_inputs::CameraInfo ci;
    const bool ok = scene_inputs::ReadCamera(ReadThunk, cam, ci);
    Check(ok && ci.object == kCamA && ci.reason == 0, "camera.slot0", "object " + std::to_string(ci.object) + " reason " + std::to_string(ci.reason));
    Check(ok && Near(cam.eye[0], 100.f) && Near(cam.eye[1], -50.f) && Near(cam.eye[2], 300.f), "camera.eye", ok ? ("eye " + std::to_string(cam.eye[0]) + " " + std::to_string(cam.eye[1]) + " " + std::to_string(cam.eye[2])) : "unreadable");
    Check(ok && cam.near_z == 10.f && cam.far_z == 20000.f && Near(cam.proj.m[5], 2.0f) && Near(cam.view.m[12], 300.f) && cam.flags == 0, "camera.matrices");
    Guest keep = g_guest;
    Camera c2;
    scene_inputs::CameraInfo i2;
    g_guest.Put(scene_inputs::kCamTable, 0);   // no pointer
    Check(!scene_inputs::ReadCamera(ReadThunk, c2, i2) && i2.reason == 1, "camera.null_pointer_rejected");
    g_guest = keep;
    for (int i = 0; i < 16; ++i) g_guest.PutF(kCamA + 240 + 4 * i, 0.f);   // a zero matrix
    Check(!scene_inputs::ReadCamera(ReadThunk, c2, i2) && i2.reason == 3, "camera.zero_matrix_rejected");
    g_guest = keep;
    g_guest.PutF(kCamA + 240, 2.0f);   // a scaled (non-rigid) view
    Check(!scene_inputs::ReadCamera(ReadThunk, c2, i2) && i2.reason == 3, "camera.scaled_view_rejected");
    g_guest = keep;
    g_guest.PutF(kCamA + 176 + 4 * 15, 1.0f);   // not a perspective pattern (m[15] != 0)
    Check(!scene_inputs::ReadCamera(ReadThunk, c2, i2) && i2.reason == 4, "camera.projection_pattern_rejected");
    g_guest = keep;
    ResetStamps();
    live::inputs::InputsContributor cc;
    cc.SetReader(ReadThunk);
    FrameInfo f3;
    f3.guest_frame = 300;
    cc.Latch(f3);
    SceneTables tc;
    Counters c3;
    cc.Emit(f3, tc, c3);
    Check(tc.has_camera && Near(tc.camera.eye[0], 100.f) && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInCamera].load() == 299, "camera.record_and_stamp");
    std::vector<uint8_t> by = SerializeRecord(tc);
    SceneTables back;
    std::string er;
    Check(ParseRecord(by.data(), by.size(), back, er) && back.has_camera && Near(back.camera.eye[2], 300.f) && SerializeRecord(back) == by, "camera.roundtrip", er);

    // ---- S110 CUTCROWD (docs\research\s110\CUTCROWD.md; src\scene_live\handed_camera.h): the record's camera is the block the game HANDED to its renderer, [[0x83929A68] + 256] (view +240, FOV radians
    // +512), read at the latch through the real ReadSceneCamera; the composer's copy +1488 / +1552 is one shot ahead on a gated pass. Forged: the manager, the engine object, the block, the live copy, the image k.
    {
        namespace hd = scene_inputs::handed;
        hd::g_plant = Plant("handed_ignored") ? 1 : Plant("handed_any_block") ? 2 : Plant("handed_mirror_taken") ? 3 : Plant("handed_fov_derived") ? 4 : Plant("handed_invalid_taken") ? 5 : 0;
        constexpr uint32_t kMgr = 0x40003000, kEng = 0x40005000, kOther = 0x40009000;
        const float kDeg = 0.017453292f;   // the image float at 0x82004D70 (q decomp sub_823C79A8 +58)
        const uint32_t kw = hd::WordOf(kDeg);
        const auto view_at = [](float x) {   // a rigid view (identity rotation) with its translation x: eye = (-x, 0, 0)
            std::array<float, 16> v{};
            v[0] = v[5] = v[10] = v[15] = 1.f;
            v[12] = x;
            return v;
        };
        const auto put_live = [](const std::array<float, 16>& v, uint32_t fov_w) {
            for (int i = 0; i < 16; ++i) g_guest.PutF(kMgr + 1488 + 4 * i, v[i]);
            g_guest.Put(kMgr + 1552, fov_w);
        };
        const auto put_block = [&](uint32_t blk, const std::array<float, 16>& v, uint32_t deg_w) {   // what the hand-off writes: the view at +240, fl(deg k) at +512; then the engine's pointer
            for (int i = 0; i < 16; ++i) g_guest.PutF(blk + 240 + 4 * i, v[i]);
            g_guest.Put(blk + 512, hd::RadWordOf(deg_w, kw));
            g_guest.Put(kEng + 256, blk);
        };
        const auto read_cam = [](Camera& c) {
            scene_inputs::CameraInfo ci;
            return scene_inputs::ReadSceneCamera(ReadThunk, c, ci);
        };
        Guest keep3 = g_guest;
        g_guest.Put(scene_inputs::kCamMgrPtr, kMgr);
        g_guest.Put(0x83929A68u, kEng);
        g_guest.PutF(0x82004D70u, kDeg);
        const auto A = view_at(-100.f), B = view_at(-500.f);   // A: eye x = 100 (the old shot, handed), B: eye x = 500 (the new shot, +1488)
        const uint32_t f50 = hd::WordOf(50.f), f40 = hd::WordOf(40.f);
        // a gated pass: the block holds A (either of the manager's two blocks), +1488 already B
        bool gated_ok = true;
        std::string gwhy;
        for (uint32_t k = 0; k < 2; ++k) {
            put_block(kMgr + 48 + 656 * k, A, f50);
            put_live(B, f40);
            Camera ca;
            const bool ok = read_cam(ca);
            gated_ok = gated_ok && ok && Near(ca.eye[0], 100.f) && std::fabs(ca.fov_y_deg - 50.f) < 1e-4f;
            gwhy += " k" + std::to_string(k) + (ok ? (": eye x " + std::to_string(ca.eye[0]) + " fov " + std::to_string(ca.fov_y_deg)) : ": unreadable");
        }
        Check(gated_ok, "handed.gated_takes_block", gwhy);
        // an ungated pass: block == live; the record's bytes are the old read's, the FOV word too. The FOV is one whose radians do NOT give it back by division (fl(r / k) or a neighbour lands on another
        // degrees value with the same radians), so a derived word would differ from the live one.
        uint32_t dw = hd::WordOf(60.f);
        int steps = 0;
        while (steps < 200000 && hd::DegWordFromRad(hd::RadWordOf(dw, kw), kw) == dw) dw = hd::WordOf(std::nextafter(hd::FloatOf(dw), INFINITY)), ++steps;
        put_block(kMgr + 48, B, dw);
        put_live(B, dw);
        Camera cb, cl;
        const bool okb = read_cam(cb);
        g_guest.Put(kEng + 256, 0);   // no block: the old read
        const bool okl = read_cam(cl);
        Check(steps < 200000 && okb && okl && std::memcmp(&cb, &cl, sizeof cb) == 0 && Near(cb.eye[0], 500.f), "handed.equal_is_live",
              "fov " + std::to_string(hd::FloatOf(dw)) + " (searched " + std::to_string(steps) + " ulps); the block read and the live read are the same bytes");
        // not the main manager's block (a sub-view, another manager): the live read; pointer 0 likewise
        put_block(kOther, A, f50);
        put_live(B, f40);
        Camera co, cz;
        const bool oko = read_cam(co);
        g_guest.Put(kEng + 256, 0);
        const bool okz = read_cam(cz);
        Check(oko && okz && Near(co.eye[0], 500.f) && Near(co.fov_y_deg, 40.f) && Near(cz.eye[0], 500.f), "handed.other_block_live", "eye x " + std::to_string(co.eye[0]));
        // a mirrored block (the hand-off's slot +176 path: row 0 negated, with a cull-winding flip the record cannot carry): the live read
        auto M = A;
        for (int i = 0; i < 4; ++i) M[i] = -M[i];
        put_block(kMgr + 48, M, f50);
        put_live(B, f40);
        Camera cm;
        const bool okm = read_cam(cm);
        Check(okm && Near(cm.eye[0], 500.f), "handed.mirror_live", okm ? "eye x " + std::to_string(cm.eye[0]) : "unreadable");
        // a block that fails the record's own tests (a scaled view): the live read, not "no camera"
        auto S = A;
        for (int i = 0; i < 3; ++i) S[i * 4 + i] = 2.f;
        put_block(kMgr + 48, S, f50);
        put_live(B, f40);
        Camera cs;
        const bool oks = read_cam(cs);
        Check(oks && Near(cs.eye[0], 500.f), "handed.invalid_block_live", oks ? "eye x " + std::to_string(cs.eye[0]) : "no camera");
        // the census through the real latch call: three gated latches, then an equal one; the reach words [mgr+1424] and the composer slot's +176 / +184
        if (hd::g_plant == 0) {
            hd::ResetCensus();
            g_guest.Put(kMgr + 1424, 1);
            g_guest.Put(kMgr + 24, 0x40006000), g_guest.Put(kMgr + 28, 2), g_guest.Put(kMgr + 32, 1);
            g_guest.Put(0x40006000 + 272 + 176, 1), g_guest.Put(0x40006000 + 272 + 184, 0);
            put_block(kMgr + 48, A, f50);
            put_live(B, f40);
            for (int i = 0; i < 3; ++i) scene_inputs::HandedCensusLatch(ReadThunk);
            put_block(kMgr + 48, B, f40);
            scene_inputs::HandedCensusLatch(ReadThunk);
            g_guest.Put(kEng + 256, kOther);
            scene_inputs::HandedCensusLatch(ReadThunk);
            const std::string line = hd::CensusLine();
            Check(line.find("latches 5 block 4 differ 3 longest_differ 3 fov_derived 3") != std::string::npos && line.find("other_block 1") != std::string::npos &&
                  line.find("reach(mgr1424 5 slot176 5 slot184 0)") != std::string::npos, "handed.census", line);
            hd::ResetCensus();
        }
        hd::g_plant = 0;
        g_guest = keep3;
    }

    // ---- S103 WC R5 (WNCC_adversary.md R5): the draw thread's camera, read as sub_82BA7748 does: slot = [[[r13]+1048]+32] (0 when [[r13]+1048] is 0), cam = [0x83929A40 + 4 slot], view +240, proj +176
    {
        using live::drawcam::Cam;
        live::drawcam::g_drawcam_plant = Plant("dc_slot_ignored") ? 1 : Plant("dc_age_ignored") ? 2 : Plant("dc_hook_clock") ? 3 : Plant("dc_light_not_skipped") ? 4 : 0;
        constexpr uint32_t kR13 = 0x40001000, kTls = 0x40001100, kBlk = 0x40001200;
        Guest keep2 = g_guest;
        g_guest.Put(kR13, kTls), g_guest.Put(kTls + 1048, kBlk), g_guest.Put(kBlk + 32, 1);   // the thread's block says slot 1: the camera is B's (the table holds A in slot 0, B in slot 1)
        Cam().Reset();
        const bool took = Cam().Note(ReadThunk, kR13, 900);
        Camera dc;
        const bool have = Cam().Get(900, dc);
        Check(took && have && Near(dc.eye[0], 5.f) && Near(dc.eye[1], 6.f) && Near(dc.eye[2], 7.f) && dc.near_z == 1.f && dc.far_z == 500.f, "drawcam.thread_slot", "slot 1 of the thread block -> table entry 1 (camera B): view from +240, proj from +176, near / far from +500 / +504");
        g_guest.Put(kBlk + 32, 0);   // slot 0 now: a NEW frame, camera A
        Cam().Reset();
        Camera da;
        const bool took0 = Cam().Note(ReadThunk, kR13, 901);
        Check(took0 && Cam().Get(901, da) && Near(da.eye[0], 100.f) && Near(da.eye[2], 300.f), "drawcam.slot0", "thread slot 0 -> camera A");
        g_guest.Put(kTls + 1048, 0);   // no block: slot 0 (the game's own rule for a thread without one)
        Cam().Reset();
        Check(Cam().Note(ReadThunk, kR13, 902) && Cam().Get(902, da) && Near(da.eye[0], 100.f), "drawcam.no_block_is_slot0", "[[r13]+1048] == 0 -> slot 0");
        {   // BHF1 B1: two draw threads in one frame, the slot-1 worker FIRST: the latch's camera is the main walk's (slot 0, camera A), never "first come"
            constexpr uint32_t kR13w = 0x40001400, kTlsw = 0x40001500, kBlkw = 0x40001600;
            g_guest.Put(kR13w, kTlsw), g_guest.Put(kTlsw + 1048, kBlkw), g_guest.Put(kBlkw + 32, 1);   // worker: slot 1 -> camera B
            g_guest.Put(kTls + 1048, 0);                                                              // main thread: no block -> slot 0 -> camera A
            Cam().Reset();
            const bool w1 = Cam().Note(ReadThunk, kR13w, 930), m1 = Cam().Note(ReadThunk, kR13, 930), w2 = Cam().Note(ReadThunk, kR13w, 930);
            Camera dm;
            const bool got = Cam().Get(930, dm);
            Check(w1 && m1 && !w2 && got && Near(dm.eye[0], 100.f) && Cam().Stats().fallback_slot == 0, "drawcam.main_slot_wins_over_first_worker", "worker (slot 1) noted first, main (slot 0) second: Get answers camera A (eye 100), not B (eye 5); a repeat call of the worker's slot is the cheap early exit");
            Cam().Reset();
            Cam().Note(ReadThunk, kR13w, 931);
            Camera df;
            const bool gotf = Cam().Get(931, df);
            Check(gotf && Near(df.eye[0], 5.f) && Cam().Stats().fallback_slot == 1, "drawcam.fallback_when_main_slot_has_none", "only the slot-1 worker drew: its camera answers (counted fallback_slot 1), never none");
        }
        g_guest = keep2;
        // the HUD camera in the slot (ortho in +176: m11 = 0, m15 = 1) is never cached; a later call of the same frame with the 3D camera is
        {
            g_guest.Put(kR13, kTls), g_guest.Put(kTls + 1048, 0);
            float hud[16] = {};
            hud[0] = 2.f / 1280.f, hud[5] = -2.f / 720.f, hud[10] = 1.f, hud[15] = 1.f;
            for (int i = 0; i < 16; ++i) g_guest.PutF(kCamA + 176 + 4 * i, hud[i]);
            Cam().Reset();
            const bool hud_taken = Cam().Note(ReadThunk, kR13, 910);
            Camera dh;
            const bool hud_have = Cam().Get(910, dh);
            g_guest = keep2;
            g_guest.Put(kR13, kTls), g_guest.Put(kTls + 1048, 0);
            const bool again = Cam().Note(ReadThunk, kR13, 910);
            Check(!hud_taken && !hud_have && again && Cam().Get(910, dh), "drawcam.hud_ortho_rejected", "the HUD camera (ortho at +176) fails IsPerspective and is not cached; the same frame's next call takes the 3D one");
        }
        g_guest = keep2;
        g_guest.Put(kR13, kTls), g_guest.Put(kTls + 1048, 0);
        Cam().Reset();
        Cam().Note(ReadThunk, kR13, 920);
        Camera dd;
        const bool age0 = Cam().Get(920, dd), age1 = Cam().Get(921, dd), age2 = Cam().Get(922, dd), ahead = Cam().Get(919, dd);
        Check(age0 && age1 && !age2 && !ahead, "drawcam.age", "the camera noted at record-clock frame j answers Get(j) and Get(j + 1), never Get(j + 2), and Get(j - 1) is refused (ahead)");
        // the latch: the scene manager is null (the CAW editor, reason 1): the draw thread's camera is the frame's camera
        g_guest.Put(scene_inputs::kCamMgrPtr, 0);
        ResetStamps();
        Cam().Reset();
        {
            live::inputs::InputsContributor c0;
            c0.SetReader(ReadThunk);
            FrameInfo fx;
            fx.guest_frame = 300;
            c0.Latch(fx);
            SceneTables t0;
            Counters k0;
            c0.Emit(fx, t0, k0);
            Check(!t0.has_camera, "drawcam.latch_without_note_has_no_camera", "scene manager null and no draw-thread camera: no camera (as before)");
        }
        ResetStamps();
        // S103 CCF: Note stamps on the draw-half clock = the record frame (note 300 for record 300); the old test noted 299 and so hid the stale-every-frame bug
        Cam().Note(ReadThunk, kR13, 300);
        {
            live::inputs::InputsContributor c1;
            c1.SetReader(ReadThunk);
            FrameInfo fx;
            fx.guest_frame = 300;
            c1.Latch(fx);
            SceneTables t1;
            Counters k1;
            c1.Emit(fx, t1, k1);
            Check(t1.has_camera && Near(t1.camera.eye[0], 100.f) && t1.camera.near_z == 10.f && rdr_own::S(rdr_own::Pass::Scene3D).input_frame[scene_inputs::kInCamera].load() == 299, "drawcam.latch_fallback", "scene manager null: the latch takes the draw thread's camera (record + stamp)");
        }
        g_guest = keep2;
        {   // S103 CCF: the clocks of the cache, record 300: noted 301 refused (ahead), 298 refused (old), 299 and 300 accepted
            g_guest.Put(kR13, kTls), g_guest.Put(kTls + 1048, 0);
            Camera dz;
            bool got[4] = {};
            const uint64_t notes[4] = {301, 298, 299, 300};
            for (int i = 0; i < 4; ++i) {
                Cam().Reset();
                Cam().Note(ReadThunk, kR13, notes[i]);
                got[i] = Cam().Get(300, dz);
            }
            Check(!got[0] && !got[1] && got[2] && got[3], "drawcam.clock_window", "latch of record 300: camera noted 301 refused (ahead), 298 refused (old), 299 and 300 accepted");
            Cam().Reset();
            Cam().Note(ReadThunk, kR13, 301);
            Cam().Get(300, dz);
            Check(Cam().Stats().ahead == 1 && Cam().Stats().old == 0, "drawcam.ahead_counted", "a camera noted after the latch's record counts as ahead, not old");
        }
        g_guest = keep2;
        {   // S103 CCF: the shadow module's light camera (this + 304 / this + 1312, installed by sub_827E7DF0 into the thread slot) is not the frame's camera; the frame's later main camera is still taken
            constexpr uint32_t kMod = kCamA - live::drawcam::kLightRecordA;   // [0x83924748] = the module, so this + 304 == kCamA (slot 0's object)
            g_guest.Put(kR13, kTls), g_guest.Put(kTls + 1048, 0), g_guest.Put(live::drawcam::kShadowModulePtr, kMod);
            Cam().Reset();
            const bool took_light = Cam().Note(ReadThunk, kR13, 950);
            const uint64_t skipped = Cam().Stats().light_skipped;
            g_guest.Put(scene_inputs::kCamTable + 0, kCamB);   // the shadow render ended: the main camera is back in the slot
            Camera dl;
            const bool took_main = Cam().Note(ReadThunk, kR13, 950) && Cam().Get(950, dl);
            Check(!took_light && skipped == 1 && took_main && Near(dl.near_z, 1.f), "drawcam.light_skipped", "slot holds module + 304: skipped (counted, no read used up), then the main camera of the same frame is taken");
            Check(Cam().CensusText().find("sample{") != std::string::npos && Cam().CensusText().find("light_skipped 1") != std::string::npos, "drawcam.census_sample", "the census names light_skipped and a sample{object, eye, near/far, module, flagA}");
            g_guest.Put(live::drawcam::kShadowModulePtr, 0);
        }
        g_guest = keep2;
        Cam().Reset();
        live::drawcam::g_drawcam_plant = 0;
    }
  }

  // ---------------------------------------------------------------- 4. the record file: write -> read -> write, an older reader
  {
    std::vector<uint8_t> a = SerializeRecord(t);
    SceneTables back;
    std::string err;
    const bool parsed = ParseRecord(a.data(), a.size(), back, err);
    if (Plant("lose_section")) back.model_runtime.clear();
    std::vector<uint8_t> b = SerializeRecord(back);
    Check(parsed && a == b && back.post_params.size() == 1 && back.model_runtime.size() == 2 && back.input_stamps.size() == t.input_stamps.size() && back.inputs_errors == 0, "record.roundtrip",
          parsed ? ("bytes " + std::to_string(a.size()) + (a == b ? " equal" : " DIFFER")) : err);
    if (!write_record.empty()) {
      std::ofstream f(write_record, std::ios::binary);
      f.write(reinterpret_cast<const char*>(a.data()), std::streamsize(a.size()));
    }
    // an older reader does not know ids 29-31: it skips them. Emulated by re-labelling the three sections with an id no reader knows and reading again: the rest of the record is intact.
    std::vector<uint8_t> old = a;
    SceneHeader h;
    std::memcpy(&h, old.data(), sizeof h);
    int relabelled = 0;
    for (uint32_t i = 0; i < h.section_count; ++i) {
      SectionEntry e;
      std::memcpy(&e, old.data() + sizeof h + i * sizeof e, sizeof e);
      if (e.id == kScPostParams || e.id == kScModelRuntime || e.id == kScInputStamps) {
        e.id = 200 + e.id;
        std::memcpy(old.data() + sizeof h + i * sizeof e, &e, sizeof e);
        ++relabelled;
      }
    }
    SceneTables o;
    std::string oerr;
    const bool oparsed = ParseRecord(old.data(), old.size(), o, oerr);
    Check(relabelled == 3 && oparsed && o.post_params.empty() && o.model_runtime.empty() && o.input_stamps.empty() && o.inputs_errors == 0, "record.older_reader_skips", oerr);
    // a section with a layout this reader does not know drops THAT table only (counted), never the record. S103 BHF4 (224d864b5, record_io.cpp ParseRecord: each table is judged on its own; the old clear-all hid every hair / cap / eyelid piece): a bad model_runtime leaves the post row and the stamps
    SceneTables bad = t;
    bad.model_runtime[0].layout = 9;
    std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables bo;
    std::string berr;
    const bool bparsed = ParseRecord(bb.data(), bb.size(), bo, berr);
    Check(bparsed && bo.inputs_errors == 1 && bo.model_runtime.empty() && bo.post_params.size() == t.post_params.size() && bo.input_stamps.size() == t.input_stamps.size(), "record.bad_layout_dropped", berr);
    // Recycle empties the new members
    SceneTables r = t;
    r.Recycle();
    Check(r.post_params.empty() && r.model_runtime.empty() && r.input_stamps.empty() && r.inputs_errors == 0, "record.recycle");
  }

  // ---------------------------------------------------------------- S103 CE: bit 7 (caster_entries): the entrances' slots 7 / 8 carry a key-OFF light state (key direction 0, key colour 0)
  {
    using rdr_scene::wlight::Entry;
    auto make = [](float key_dir_x, float key_dir_z, float key_col) {   // a wrestler-state block: light 0 coloured, key (light 4) as given
      Entry e;
      float f[rdr_scene::kLsLightFloats] = {};
      f[rdr_scene::kLsCol + 0] = 0.08f, f[rdr_scene::kLsCol + 1] = 0.08f, f[rdr_scene::kLsCol + 2] = 0.12f;
      f[rdr_scene::kLsDir + 16] = key_dir_x, f[rdr_scene::kLsDir + 18] = key_dir_z;
      f[rdr_scene::kLsCol + 16] = f[rdr_scene::kLsCol + 17] = f[rdr_scene::kLsCol + 18] = key_col;
      std::memcpy(e.block, f, sizeof f);
      return e;
    };
    const Entry keyed = make(-0.6f, -0.8f, 1.96f), off = make(0.f, 0.f, 0.f), zero_dir_lit = make(0.f, 0.f, 1.5f);
    const std::vector<Entry> entrance = {keyed, keyed, off, off};   // slots 0, 6 keyed; 7, 8 key-off
    const std::vector<Entry> all_off = {off, off};
    namespace wl = rdr_scene::live::wlatch;
    const bool old_rule = Plant("ce_old_key_rule"), off_is_keyed = Plant("ce_keyoff_is_keyed");
    Check(wl::LatchStamps(entrance, 4, true, !old_rule), "ce.entrance_latch_stamps", "{0 keyed, 6 keyed, 7 key-off, 8 key-off}: stamps (the old rule refused: slots 7 / 8 key_not_dir)");
    Check(!wl::LatchStamps(all_off, 2, true, true, off_is_keyed), "ce.all_keyoff_no_stamp", "every slot key-off: nothing for the shadow fit's key: no stamp");
    Check(!wl::EntryUsable(zero_dir_lit, true, Plant("ce_zero_dir_usable")) && !wl::LatchStamps({keyed, zero_dir_lit}, 2, true, true), "ce.zero_dir_lit_key_unusable", "direction 0 with a non-zero key colour stays unusable");
    Check(!wl::LatchStamps(entrance, 5, true) && !wl::LatchStamps(entrance, 4, false), "ce.holes_still_withhold", "an occupied slot with no entry / synthesis off: no stamp");
    // the draw side shares the rule
    float st[rdr_scene::kLsLightFloats];
    std::memcpy(st, off.block, sizeof st);
    float kf[rdr_scene::kLsLightFloats];
    std::memcpy(kf, keyed.block, sizeof kf);
    Check(rdr_scene::LightStateUsable(st) && !rdr_scene::HasKey(st) && rdr_scene::LightStateUsable(kf) && rdr_scene::HasKey(kf), "ce.draw_rule_shared", "LightStateUsable / HasKey (light_install.h)");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
