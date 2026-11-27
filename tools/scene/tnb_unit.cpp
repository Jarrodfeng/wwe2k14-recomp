// body-scale: none (checks the entrance page's pool / spot rows, builds no wrestler picture)  [S103 RGF: body_scale_feed_lint.py]
// tnb_unit (S103 TNB, docs\agents\s103\TEN.md 4): the entrance floor pools, the follow spot and the partner / companion disks follow the GAME'S PER-TRACK BINDING (manager words mgr + 148 + 4 i), not "the one entering slot".
//   tnb_unit [--plant NAME]
// Production order, the real code at every step: a fake big-endian guest (the page task runner, the phase word, the light manager with the pool object, the character table 0x836F38D0, the track words) ->
// the LATCH contributor (InputsContributor::Latch: ReadLightObjects writes the track words, ReadTrackBinds the bound characters, Derive decides the frame has rows) -> Emit into a SceneTables -> the RECORD
// (SerializeRecord / ParseRecord: the record the stream-free build writes and reads) -> the CONSUMER (stage_look::Pools over the PARSED record). The oracle is hand-written from the listing (sub_825BACE0: a track
// bound to a slot 0..8 is drawn at [[0x836F38D0 + 4 b] + 176] x / z when the phase word is 4, else at its own rig row), never from the code's own tables. Exit 1 on a failure.
// A plant is a fault in the thing under test; each must make a NAMED check fail:
//   entrant_only   the switch is off (the old rule: one entering slot gates the stamp, the spot sits on the entrant)  -> "tnb.partner_disk_at_bound"
//   bind_ignored   the latch is handed a phase that is not 4 (the bindings never move a track)                          -> "tnb.phase4_disks_at_bound"
//   key_ignored    the keys are dropped at the latch (a partner's track is a plain 375 pool)                            -> "tnb.partner_is_disk_by_key"
//   page_from_lights  S111 PAGEPOOLS: the record without its page rows (the page derived from the model-class light objects, latch r - 1) -> "pagepools.page_of_own_latch"
// (S111 PAGEPOOLS: the page is derived at the LATCH from its own reads and rides the record as kSlPage / kSlPool / kSlSpot rows, so the plants hand the fault to the latch, not to the parsed record.)
//   size_ignored   CODE: Derive drops the size ints (a disk falls to the default 375 pool)                                -> "tnb.disk_half_from_size"
//   pos_first      CODE: Derive checks a track's position before its tint (the old order)                                -> "tnb.dark_track_bad_position_ok"
//   size_words     the guest keeps the size ints at +184 (the latch reads +188)                                         -> "tnb.size_words_carried"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/renderer_scene/rows_stage_look.h"
#include "../../src/renderer_scene/stage_look.h"
#include "../../src/scene_live/inputs_contrib.h"

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace si = scene_inputs;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;   // guest word address (4-aligned) -> value (host order)
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutF(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); words[a] = b; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = si::Swap32(it->second);   // the guest is big-endian
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
};
static Guest g;
static bool ReadThunk(uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }
static int g_mode = 2;

// ---- the fixture: addresses are the guest's (the game's globals), everything else is a made-up heap address
static constexpr uint32_t kOwner = 0x40100000, kGroups = 0x40600000, kTasks = 0x40700000, kPage = 0x40300000;   // the page task runner (sub_82C2FEB8's layout, fd_clock_unit.cpp)
static constexpr uint32_t kMgr = 0x40010000, kSetsArr = 0x40011000, kIdsArr = 0x40011100, kRemapArr = 0x40011200, kPoolArr = 0x40011300, kSetObj = 0x40020000, kPoolObj = 0x40030000;
static constexpr uint32_t kCharTable = 0x836F38D0, kPhaseW = 0x836F3940;
static constexpr uint64_t kDiskKey = 0x1122334455667788ull;

static void BuildGuest(bool size_at_184) {
  g = Guest{};
  // the page object in task group 11 (entrance word set: an ENTRANCE frame)
  g.Put(0x83930050u, kOwner);
  g.Put(kOwner + 32, kGroups), g.Put(kOwner + 40, kGroups + 16 * 35);
  for (uint32_t gi = 0; gi < 35; ++gi) {
    const uint32_t t = kTasks + 0x1000 * gi;
    g.Put(kGroups + 16 * gi + 0, t), g.Put(kGroups + 16 * gi + 4, t + 64), g.Put(kGroups + 16 * gi + 8, gi == 11 ? t + 4 : t);
  }
  g.Put(kTasks + 0x1000 * 11, kPage);
  g.Put(0x8201F294u, 0x8201F294u), g.Put(0x8201F298u, si::kPageUpdateFn);   // the object's vtable word (any 0x82...) and the vtable's slot 1 = sub_825BA448
  g.Put(kPage, 0x8201F294u), g.Put(kPage + 88, 1), g.PutF(kPage + 108, 12.f), g.Put(kPage + 112, 1);
  g.Put(kPhaseW, 4);
  // the light manager: one arena set remapped to pool 1 (info = remap + 1 = 2: the entrance marker), three pool slots, the rig's pool object in slot 2
  g.Put(si::kLightMgrPtr, kMgr);
  g.Put(kMgr + 124, 1), g.Put(kMgr + 112, kSetsArr), g.Put(kMgr + 116, kIdsArr), g.Put(kMgr + 120, kRemapArr), g.Put(kMgr + 140, kPoolArr), g.Put(kMgr + 144, 3);
  g.Put(kMgr + 368, 0), g.Put(kMgr + 372, 0), g.Put(kMgr + 376, 0);
  g.Put(kSetsArr, kSetObj), g.Put(kIdsArr, 121), g.Put(kRemapArr, 1);
  g.Put(kPoolArr, 0), g.Put(kPoolArr + 4, 0), g.Put(kPoolArr + 8, kPoolObj);
  for (uint32_t k = 0; k < kLsLightFloats; ++k) g.Put(kSetObj + 48 + 4 * k, 0), g.Put(kPoolObj + 48 + 4 * k, 0);
  g.Put(si::kLightOverrideWord0, 0xFFFFFFFFu), g.Put(si::kLightOverrideWord1, 0xFFFFFFFFu), g.Put(si::kLightGatePtr, 0);
  // the rig's own rows of the pool object (sub_827F8808's tail, stride 16): position row at R + 416 + 16 i, tint at R + 736 + 16 i (R = object + 48)
  const float rig_x[10] = {0.f, 300.f, -480.f, -490.f, -500.f, -510.f, -520.f, 111.f, -540.f, -550.f}, rig_z[10] = {7500.f, 400.f, -1386.f, -1379.f, -1372.f, -1365.f, -1358.f, 222.f, -1344.f, -1337.f};
  for (uint32_t i = 0; i < 10; ++i) {
    g.PutF(kPoolObj + 48 + 416 + 16 * i, rig_x[i]), g.PutF(kPoolObj + 48 + 416 + 16 * i + 8, rig_z[i]);
    const bool lit = i == 0 || i == 1 || i == 2 || i == 3 || i == 7;
    for (uint32_t c = 0; c < 3; ++c) g.PutF(kPoolObj + 48 + 736 + 16 * i + 4 * c, lit ? 0.5f : 0.f);
  }
  // the manager's per-track words (sub_827F65C8 / 65D8): bindings at +148 (track 0 -> slot 2, track 7 -> slot 4), sizes at +188 / +228, keys (u64) at +268
  for (uint32_t i = 0; i < 10; ++i) {
    g.Put(kMgr + 148 + 4 * i, i == 0 ? 2u : i == 7 ? 4u : 0xFFFFFFFFu);
    const bool dk = i == 0 || i == 7;   // the disks' key resolves to a 32 x 32 sprite (size ints 32 / 32): half 32 x 3000 / 512 = 187.5
    g.Put(kMgr + (size_at_184 ? 184 : 188) + 4 * i, dk ? 32u : 100 + i), g.Put(kMgr + 228 + 4 * i, dk ? 32u : 200 + i);
    const uint64_t key = (i == 0 || i == 7) ? kDiskKey : 0;
    g.Put(kMgr + 268 + 8 * i, uint32_t(key >> 32)), g.Put(kMgr + 268 + 8 * i + 4, uint32_t(key));
  }
  // the character table: slot 2 = the leader (act 25000 = 0x61A8, step 5), slot 4 = the partner (act 25004); each [char + 176] points at the skeleton's node-2 translation
  for (uint32_t b = 0; b < 9; ++b) {
    const uint32_t ch = 0x40200000u + 0x1000 * b, ar = 0x40800000u + 0x100 * b, pos = 0x40400000u + 0x100 * b;
    g.Put(::kCharTable + 4 * b, ch), g.Put(ch + 28, ar), g.Put(ch + 176, pos);   // the file's own constant: rdr_scene::kCharTable (cam_gate.h, same address) is in scope through `using namespace rdr_scene` and made the name ambiguous
    g.Put(ar, b == 2 ? 0x61A80500u : b == 4 ? 0x61AC0500u : 0u);
    g.PutF(pos, b == 2 ? 440.f : b == 4 ? -880.f : 10.f * float(b)), g.PutF(pos + 4, 10.f), g.PutF(pos + 8, b == 2 ? 2200.f : b == 4 ? 1760.f : 5.f * float(b)), g.PutF(pos + 12, 1.f);
  }
}

static void ResetStamps() {
  for (int b = 0; b < 32; ++b) rdr_own::S(rdr_own::Pass::Scene3D).input_frame[b].store(0xFFFFFFFFFFFF0000ull);
  for (int b = 0; b < si::kInputCount; ++b) si::g_counters.stamps[b].store(0), si::g_counters.last[b].store(0);
}

// the latch -> Emit -> record -> parse of the current guest, as the game's frame goes
struct Run {
  SceneTables emitted, parsed;
  std::string census;
  bool parse_ok = false;
  uint64_t pools_stamp = 0;
};
static Run Go(uint64_t frame) {
  Run r;
  ResetStamps();
  live::inputs::InputsContributor c;
  c.SetReader(ReadThunk);
  FrameInfo fi;
  fi.guest_frame = frame;
  c.Latch(fi);
  Counters cn;
  c.Emit(fi, r.emitted, cn);
  c.Census(r.census);
  r.pools_stamp = rdr_own::S(rdr_own::Pass::Scene3D).input_frame[si::kInStagePools].load();
  std::vector<uint8_t> by = SerializeRecord(r.emitted);
  std::string er;
  r.parse_ok = ParseRecord(by.data(), by.size(), r.parsed, er);
  return r;
}

static constexpr float kSnap = stage_look::kSpotSnap;
static float Snap(float v) { return std::round(v / kSnap) * kSnap; }
static bool Near(float a, float b) { return std::fabs(a - b) < 0.6f; }
using assets::stage_pools::Pool;
static std::string Desc(const std::vector<Pool>& v) {
  std::string s = std::to_string(v.size()) + ":";
  for (const Pool& p : v) s += std::string(" ") + (p.disk ? "D" : "P") + "(" + std::to_string(int(p.x)) + "," + std::to_string(int(p.z)) + ")";
  return s;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
  scene_frame::g_mode_cvar.store(&g_mode);
  scene_frame::SetReady(true);
  rdr_scene::entrance_tracks::g_on.store(!Plant("entrant_only"));
  stage_pool_words::g_plant_pos_first = Plant("pos_first"), stage_pool_words::g_plant_size_ignored = Plant("size_ignored");   // code faults in Derive (the other twins mutate the guest / the record)

  // ---- A. phase 4: track 0 -> slot 2 (the leader), track 7 -> slot 4 (the partner); the leader's act is 25000, the partner's 25004. The gate no longer needs ONE entering slot: slot 2 alone is in act 25000 here, so that stays true;
  // scenario D below makes it false.
  BuildGuest(Plant("size_words"));
  if (Plant("bind_ignored")) g.Put(kPhaseW, 3);   // S111 PAGEPOOLS: the page is derived at the latch, so the fault is handed to the latch
  if (Plant("key_ignored"))
    for (uint32_t i = 0; i < 10; ++i) g.Put(kMgr + 268 + 8 * i, 0), g.Put(kMgr + 268 + 8 * i + 4, 0);
  Run a = Go(300);
  Check(a.parse_ok && a.emitted.light_objects.size() >= 2 && a.parsed.light_objects.size() == a.emitted.light_objects.size(), "tnb.record_roundtrip", "latch -> Emit -> record -> parse");
  const LightObjectRow* m = a.parsed.light_objects.empty() ? nullptr : &a.parsed.light_objects[0];
  Check(m && m->layout == kLightObjectLayout && (m->flags & kLoTracksRead) && int32_t(m->words[kLmTrackBind + 0]) == 2 && int32_t(m->words[kLmTrackBind + 7]) == 4 && int32_t(m->words[kLmTrackBind + 3]) == -1, "tnb.bind_words_in_record",
        m ? ("flags " + std::to_string(m->flags) + " bind0 " + std::to_string(int32_t(m->words[kLmTrackBind + 0]))) : "no manager row");
  Check(m && m->words[kLmTrackSize + 0] == 32 && m->words[kLmTrackSize + 9] == 109 && m->words[kLmTrackSize + 10] == 32 && m->words[kLmTrackSize + 19] == 209, "tnb.size_words_carried", m ? ("size0 " + std::to_string(m->words[kLmTrackSize + 0])) : "no manager row");
  Check(m && m->words[kLmTrackKey + 0] == uint32_t(kDiskKey >> 32) && m->words[kLmTrackKey + 1] == uint32_t(kDiskKey) && m->words[kLmTrackKey + 14] == uint32_t(kDiskKey >> 32) && m->words[kLmTrackKey + 15] == uint32_t(kDiskKey) && m->words[kLmTrackKey + 12] == 0, "tnb.key_words_in_record");
  size_t nbind = 0;
  for (const StageLookRow& r : a.parsed.stage_look) nbind += r.kind == kSlTrackBind;
  Check(nbind == 2 || Plant("entrant_only"), "tnb.two_bind_rows", std::to_string(nbind) + " kSlTrackBind rows");
  Check(a.pools_stamp == 299 || Plant("entrant_only"), "tnb.pools_stamped_on_the_derive", "kInStagePools stamp " + std::to_string((unsigned long long)a.pools_stamp));
  std::vector<Pool> pv;
  bool inc = false;
  stage_look::Pools(a.parsed, pv, &inc);
  // the page: track 0 the leader's disk at slot 2 (440, 2200), track 1 a pool at its own row (the 5 rigs that key it; drawn now), tracks 2 and 3 pools at their rig rows, track 7 a DISK at slot 4 (-880, 1760)
  const bool shape = pv.size() == 5 && pv[0].disk && !pv[1].disk && !pv[2].disk && !pv[3].disk && pv[4].disk;
  Check(!inc && shape && Near(pv[0].x, Snap(440.f)) && Near(pv[0].z, Snap(2200.f)) && Near(pv[4].x, Snap(-880.f)) && Near(pv[4].z, Snap(1760.f)), "tnb.partner_disk_at_bound", Desc(pv));
  Check(shape && Near(pv[1].x, 300.f) && Near(pv[1].z, 400.f) && pv[1].half == 375.f, "tnb.track1_drawn", Desc(pv));
  Check(shape && Near(pv[2].x, -480.f) && Near(pv[2].z, -1386.f) && Near(pv[3].x, -490.f) && Near(pv[3].z, -1379.f), "tnb.pools_at_rig_rows", Desc(pv));
  Check(shape && pv[4].disk && pv[0].half == 187.5f && pv[4].half == 187.5f, "tnb.partner_is_disk_by_key", Desc(pv));   // same key as track 0 = a disk
  Check(shape && pv[0].half == 32.f * 3000.f / 512.f && pv[1].half == 375.f && pv[2].half == 375.f, "tnb.disk_half_from_size", Desc(pv));   // half = size int x 3000 / 512 (the 32 px sprite: 187.5), an unkeyed track the default 375 (TEN_ADV 3b / 3c)
  Check(shape && Near(pv[0].x, Snap(440.f)) && Near(pv[4].x, Snap(-880.f)), "tnb.phase4_disks_at_bound", Desc(pv));
  Check(a.census.find("[scene][stage] tracks bound 2 (by act 1 / code208 1), disks 2, pools 3, track1 drawn 1, unbound spot 0, disk_size_unknown 0") != std::string::npos || Plant("entrant_only"), "tnb.census_line",
        a.census.substr(a.census.find("[scene][stage]") == std::string::npos ? 0 : a.census.find("[scene][stage]"), 160));
  {   // the entrance row carries track 0's slot + 1 (the offline fallback's slot; the draw reads the bindings)
    uint32_t rs = 0;
    for (const StageLookRow& r : a.parsed.stage_look)
      if (r.kind == kSlEntrance) rs = r.rig_slot;
    Check(rs == 3 || Plant("entrant_only"), "tnb.entrance_row_slot", "rig_slot " + std::to_string(rs));
  }

  // ---- B. the same frame at a phase that is not 4: every track at its OWN rig row (the leader's spot at the rig's (0, 7500), the partner at (111, 222))
  BuildGuest(false);
  g.Put(kPhaseW, 3);
  Run b = Go(301);
  {
    std::vector<Pool> pb;
    stage_look::Pools(b.parsed, pb, &inc);
    // outside phase 4 the keys, sizes and bindings are ignored: every tinted track is the default sprite (a 375 pool) at its own row (TEN_ADV 3b)
    const bool sh = pb.size() == 5 && !pb[0].disk && !pb[4].disk && pb[0].half == 375.f && pb[4].half == 375.f;
    Check(!inc && sh && Near(pb[0].x, 0.f) && Near(pb[0].z, 7500.f) && Near(pb[4].x, 111.f) && Near(pb[4].z, 222.f), "tnb.phase_not4_rig_rows", Desc(pb));
    Check(b.census.find("disks 0, pools 5, track1 drawn 1, unbound spot 1") != std::string::npos || Plant("entrant_only"), "tnb.census_unbound_spot", b.census.substr(b.census.find("[scene][stage]") == std::string::npos ? 0 : b.census.find("[scene][stage]"), 160));
  }

  // ---- C. phase 4, but the body motion UNBOUND track 0 (YAT2 code 208 with p1 == 0 writes -1): the spot is drawn at the rig's own row, not on the entrant (the old rule kept it on the entrant)
  BuildGuest(false);
  g.Put(kMgr + 148, 0xFFFFFFFFu);
  Run c = Go(302);
  {
    std::vector<Pool> pc;
    stage_look::Pools(c.parsed, pc, &inc);
    const bool sh = pc.size() == 5 && pc[0].disk && pc[4].disk;
    Check(!inc && sh && Near(pc[0].x, Snap(0.f)) && Near(pc[0].z, Snap(7500.f)) && Near(pc[4].x, Snap(-880.f)), "tnb.unbound_spot_at_rig_row", Desc(pc));
    Check(c.census.find("tracks bound 1 (by act 0 / code208 1)") != std::string::npos && (c.census.find("unbound spot 1") != std::string::npos || Plant("entrant_only")), "tnb.census_unbound_spot_c", "");
  }

  // ---- D. no character is in act 25000 (a bank whose body act differs): the old gate (one unique entering slot) would refuse the frame and the decision would walk; the new gate is the derive alone
  BuildGuest(false);
  g.Put(0x40800000u + 0x100 * 2, 0x61AE0500u);   // slot 2's act is 25006
  Run d = Go(303);
  Check(d.pools_stamp == 302, "tnb.gate_follows_derive_not_slot", "kInStagePools stamp " + std::to_string((unsigned long long)d.pools_stamp));

  // ---- F. F1 (review): a frame that the consumer's Entrance(t) takes (the set is remapped) but the page object does not call an entrance (entrance word 0): the bind rows must still be there at phase 4 with the switch on;
  // with the switch off the record is the OLD one on every frame (no flag, no bind rows)
  {
    const bool was = rdr_scene::entrance_tracks::g_on.load();
    BuildGuest(false);
    g.Put(kPage + 88, 0);
    rdr_scene::entrance_tracks::g_on.store(true);
    Run on = Go(304);
    size_t nb_on = 0;
    for (const StageLookRow& r : on.parsed.stage_look) nb_on += r.kind == kSlTrackBind;
    rdr_scene::entrance_tracks::g_on.store(false);
    Run off = Go(305);
    size_t nb_off = 0;
    for (const StageLookRow& r : off.parsed.stage_look) nb_off += r.kind == kSlTrackBind;
    rdr_scene::entrance_tracks::g_on.store(was);
    Check(nb_on == 2 && !on.parsed.light_objects.empty() && (on.parsed.light_objects[0].flags & kLoTracksRead), "tnb.bind_rows_on_non_entrance_frame", std::to_string(nb_on) + " bind rows with the switch on");
    Check(nb_off == 0 && !off.parsed.light_objects.empty() && !(off.parsed.light_objects[0].flags & kLoTracksRead), "tnb.switch_off_is_the_old_record", std::to_string(nb_off) + " bind rows");
    stage_pool_words::Tracks tr;
    Check(!stage_look::TracksOf(off.parsed, tr), "tnb.switch_off_consumer_takes_old_rule");
  }

  // ---- G. track 7 keyed while track 0 is NOT (a different sprite): drawn as a pool of its own size (187.5 from 32 px), not a disk; non-square counted (w 32, h 16); keyed_other 1
  BuildGuest(false);
  g.Put(kMgr + 268 + 8 * 7, 0x0A0B0C0Du), g.Put(kMgr + 268 + 8 * 7 + 4, 0x01020304u);   // track 7's key differs from track 0's
  g.Put(kMgr + 268, 0), g.Put(kMgr + 272, 0);   // track 0: no key
  g.Put(kMgr + 228 + 4 * 7, 16u);
  Run gk = Go(306);
  {
    std::vector<Pool> p2;
    stage_look::Pools(gk.parsed, p2, &inc);
    Check(!inc && p2.size() == 5 && !p2[0].disk && p2[0].half == 375.f && !p2[4].disk && p2[4].half == 187.5f && Near(p2[4].x, -880.f), "tnb.keyed_without_track0_is_pool", Desc(p2));
    Check(gk.census.find("keyed_other 1, nonsquare 1") != std::string::npos, "tnb.census_keyed_other_nonsquare", gk.census.substr(gk.census.find("keyed_other") == std::string::npos ? 0 : gk.census.find("keyed_other"), 40));
  }

  // ---- H. F4: a DARK track (tint 0) with a NaN position: the game tests the tint first and never reads the position, so the frame stands
  BuildGuest(false);
  g.PutF(kPoolObj + 48 + 416 + 16 * 4, std::nanf(""));   // track 4 is dark
  Run hk = Go(307);
  Check(hk.pools_stamp == 306, "tnb.dark_track_bad_position_ok", "kInStagePools stamp " + std::to_string((unsigned long long)hk.pools_stamp));

  // ---- E. an older record: a layout-2 manager row without the flag has no track words (the consumer takes the old rule: no TracksOf); with the flag it is refused
  {
    SceneTables o = a.parsed;
    if (!o.light_objects.empty()) {
      LightObjectRow& mm = o.light_objects[0];
      mm.layout = 2, mm.flags &= ~kLoTracksRead;
      for (uint32_t i = 0; i < 50; ++i) mm.words[kLmTrackBind + i] = 0;   // an older writer left these 0: bind word 0 would read as slot 0
      stage_pool_words::Tracks tr;
      Check(!stage_look::TracksOf(o, tr), "tnb.old_record_no_tracks", "TracksOf on a layout-2 manager row");
      mm.flags |= kLoTracksRead;
      std::vector<uint8_t> by = SerializeRecord(o);
      SceneTables back;
      std::string er;
      Check(ParseRecord(by.data(), by.size(), back, er) && back.light_objects.empty() && back.light_errors == 1, "tnb.layout2_with_flag_refused", er);
    }
  }
  // ---- I. S111 PAGEPOOLS (docs\research\s111\PAGEPOOLS.md): the stage PAGE is walk-time (the page node sub_825BB0E0 runs on the record frame's clock: page_node_clock +1 324 of 324), the light objects
  // model-class (CUTREACH: record r carries latch r - 1's). ONE contributor over latches 400..403: track 2's pool moves in period 401. Record 401's page shows 401's pool while its light objects still carry
  // 400's block. Plant page_from_lights: the record without its page rows (the page derived from the model-class light objects, the CUTREACH pairing) -> "pagepools.page_of_own_latch".
  // The page's GATE (S111 PAGEPOOLS review): sub_825BB0E0 draws page 00 + the track sprites exactly when the page object's entrance word [obj+88] != 0 (0x825BB21C: [this+72], this = obj+16), not when "some
  // set is remapped" (the look's rule: tnb F above is a frame with the remap and the word 0). Period 402: the word ends, the remap stays -> the base / match page; period 403: the word is set again, the remap
  // ends -> the entrance page with that latch's pools -> "pagepools.page_gate_is_entrance_word" (the remap rule fails it in both directions).
  {
    BuildGuest(false);
    live::inputs::InputsContributor pc;
    pc.SetReader(ReadThunk);
    const auto rec = [&](uint64_t frame) {
      ResetStamps();
      FrameInfo fi;
      fi.guest_frame = frame;
      pc.Latch(fi);
      SceneTables e, p;
      Counters cn;
      pc.Emit(fi, e, cn);
      std::vector<uint8_t> by = SerializeRecord(e);
      std::string er;
      if (!ParseRecord(by.data(), by.size(), p, er)) p = SceneTables();
      if (Plant("page_from_lights"))
        p.stage_look.erase(std::remove_if(p.stage_look.begin(), p.stage_look.end(), [](const StageLookRow& r) { return r.kind == kSlPage || r.kind == kSlPool || r.kind == kSlSpot; }), p.stage_look.end());
      return p;
    };
    const SceneTables r400 = rec(400);
    g.PutF(kPoolObj + 48 + 416 + 16 * 2, -700.f);   // period 401: track 2's pool moves (its rig row x)
    const SceneTables r401 = rec(401);
    g.Put(kPage + 88, 0);                           // period 402: the page object's entrance word ends; the remap stays (the look still says entrance)
    const SceneTables r402 = rec(402);
    g.Put(kPage + 88, 1), g.Put(kRemapArr, 0xFFFFFFFFu);   // period 403: the word is set again, the remap ends (-1 = no remap)
    const SceneTables r403 = rec(403);
    std::vector<Pool> p400, p401, p402, p403;
    bool i0 = false, i1 = false, i2 = false, i3 = false;
    stage_look::Pools(r400, p400, &i0), stage_look::Pools(r401, p401, &i1), stage_look::Pools(r402, p402, &i2), stage_look::Pools(r403, p403, &i3);
    const uint32_t* blk401 = stage_pool_words::PoolBlock(r401.light_objects);
    float lx = 0.f;
    if (blk401) std::memcpy(&lx, &blk401[stage_pool_words::kPosWord0 + 4 * 2], 4);
    Check(blk401 && Near(lx, -480.f) && stage_look::Entrance(r403), "pagepools.lights_stay_model_class",
          "record 401 light block track 2 x " + std::to_string(lx) + " (want latch 400's -480), record 403 Entrance " + std::to_string(int(stage_look::Entrance(r403))) + " (want 1: latch 402's remap)");
    Check(!i0 && !i1 && p400.size() == 5 && p401.size() == 5 && Near(p400[2].x, -480.f) && Near(p401[2].x, -700.f) && stage_look::PageEntrance(r401),
          "pagepools.page_of_own_latch", "record 400 " + Desc(p400) + ", 401 " + Desc(p401) + " (want track 2 at -700)");
    Check(!i2 && !i3 && p402.empty() && !stage_look::PageEntrance(r402) && stage_look::PageEntrance(r403) && p403.size() == 5 && Near(p403[2].x, -700.f), "pagepools.page_gate_is_entrance_word",
          "record 402 (word 0, remapped) " + Desc(p402) + " page " + std::to_string(int(stage_look::PageEntrance(r402))) + " (want 0: the base page), 403 (word 1, no remap) " + Desc(p403) + " page " +
              std::to_string(int(stage_look::PageEntrance(r403))) + " (want 5, track 2 at -700)");
  }
  std::printf("%s  tnb_unit: %d check(s), %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
