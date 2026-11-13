// Lane 139 (SH3 slice 2) unit test: the plane shadow chain's record, writer and pure functions. 0 game runs, no GPU. Build: tools\shadow\build_plane_unit.ps1.
//   plane_unit.exe FILE.plane [FILE.plane ...] [--plant NAME]
// FILE = tools\shadow\plane_oracle.py's dump of a capture (Check B, run 7079, Bryan). Everything here is the SHIPPED code: record_io (the sections), plane_hook.h (the writer's tracker), plane_sites.h (the
// committed table of upload sites the tracker accepts AND the oracle driver replays), plane_shadow_core.h (BuildMask / BuildBlur / Matches / the board), plane_oracle.h.
//   A. the record: write -> read -> write is byte-identical; a record without the sections reads as empty; an unknown layout version drops only the plane tables (plane_errors); DropIfBad.
//   B. the writer (half (a) of the acceptance): per frame the tracker's frame block and draws, recomputed through BuildMask / BuildBlur and joined by the board (stamp, object, part, nth), equal the stream's
//      constants bit for bit (c0-c6, c7, c8, c9-c12, c13-c16; the blur's c33, c35, c36-c39, c40), and own every register the oracle names (EyePos, the palette and c34 are not owned: stated).
//   C. the guards: a call whose registers differ from its table row, an unreadable pointer, a foreign return address (the OLD range): counted, nothing recorded; the table covers exactly the owned registers.
//   D. the board: the join by (object, part, nth), blur by nth, a record delivered twice adds nothing, the camera of the previous stamp.
//   E. (lane 205) the blur lane rule: only the lanes the game writes are compared (a NaN leftover in c33.w / c35.w is equal when the bytes are); a differing written lane is still refused.
// --plant NAME (tests only): blur_nan_lane (E), plane_inv_identity, plane_camera_stale, plane_pos_shift, duplicate_record, writer_lr_missing, upload_after_snapshot, foreign_upload_between.
// Each must make check B FAIL (exit 1); a clean run exits 0.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/blend_key.h"  // lane 159 (S2)
#include "../../src/renderer_scene/live_scene.h"
#include "../../src/renderer_scene/plane_oracle.h"
#include "../../src/scene_live/plane_contrib.h"

using namespace rdr_scene;
using namespace rdr_scene::plane;
using rdr_scene::plane::oracle::File;
using rdr_scene::plane::oracle::Frame;

static int g_bad = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_bad;
}

static void RecordChecks() {
  SceneTables t;
  t.header.guest_frame = 77;
  for (uint32_t i = 0; i < 2; ++i) {
    PlaneFrame f;
    f.frame = 70 + i, f.have = kPlaneHaveAll;
    for (int k = 0; k < 10; ++k) f.fade[k] = float(k + i);
    for (int k = 0; k < 16; ++k) f.fit_matrix[k] = float(k) * 0.5f, f.camera[k] = float(k) * 0.25f;
    t.plane_frames.push_back(f);
  }
  for (uint32_t i = 0; i < 3; ++i) {
    PlaneDraw d;
    d.frame = 70 + (i & 1), d.kind = i == 2 ? kPlaneBlur : kPlaneMask, d.object = 0x40000000u + i, d.part = i, d.ordinal = i, d.have = i == 2 ? kPlaneDrawHaveBlur : kPlaneDrawHavePos;
    for (int c = 0; c < 4; ++c) d.plane_pos[c] = float(c + i), d.blur_point[c] = float(c);
    t.plane_draws.push_back(d);
  }
  const std::vector<uint8_t> a = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool parsed = ParseRecord(a.data(), a.size(), back, err);
  Check(parsed, "record.parses", err);
  Check(parsed && a == SerializeRecord(back), "record.round_trip_bytes", "write -> read -> write");
  Check(parsed && back.plane_frames.size() == 2 && back.plane_draws.size() == 3 && std::memcmp(back.plane_frames.data(), t.plane_frames.data(), 2 * sizeof(PlaneFrame)) == 0 &&
            std::memcmp(back.plane_draws.data(), t.plane_draws.data(), 3 * sizeof(PlaneDraw)) == 0, "record.tables_equal", "2 frames, 3 draws");
  Check(sizeof(PlaneFrame) == 200 && sizeof(PlaneDraw) == 160 && kScPlaneFrames == 18 && kScPlaneDraws == 19 && kScCount == 28 && kPlaneLayoutVersion == 1, "record.ids_pinned",
        "enum ids 18 / 19 before kScCount 28 (ring ids 20 / 21 added by lane 194, caster ids 22 / 23 by lane 202, 24 by lane 217, 25 by lane 221, 26 by lane 226, 27 by lane 242), element sizes 200 / 160, layout version 1");
  SceneTables old;
  old.header.guest_frame = 5;
  const std::vector<uint8_t> o = SerializeRecord(old);
  SceneTables oback;
  Check(ParseRecord(o.data(), o.size(), oback, err) && oback.plane_frames.empty() && oback.plane_draws.empty() && oback.plane_errors == 0, "record.old_reads_empty", err);
  SceneTables ver = t;
  ver.plane_draws[1].version = 2;
  const std::vector<uint8_t> vb = SerializeRecord(ver);
  SceneTables vback;
  Check(ParseRecord(vb.data(), vb.size(), vback, err) && vback.plane_frames.empty() && vback.plane_draws.empty() && vback.plane_errors == 1, "record.future_version_dropped",
        "a layout version this reader does not know drops the plane tables only, plane_errors 1");
  std::string verr;
  Check(live::Validate(ver, verr), "record.validate_not_refused", "live::Validate does not refuse a record for its plane tables: " + verr);
  live::plane::Tracker tr;
  Check(live::plane::DropIfBad(ver, &tr) && ver.plane_draws.empty() && tr.GetStats().dropped_tables == 1 && !live::plane::DropIfBad(t, &tr), "record.writer_drops_its_own_bad_tables", "DropIfBad: cleared and counted; a good table stays");
}

// The stream's rows by register, as the oracle holds them.
struct Ref {
  float v[64][4] = {};
  bool valid[64] = {};
  void Set(uint32_t reg, const float* r) {
    std::memcpy(v[reg], r, 16);
    valid[reg] = true;
  }
};

static int WriterChecks(const std::string& path, const Faults& faults, const oracle::Plant& plant, bool verbose) {
  File f;
  std::string err;
  if (!oracle::Load(path, f, err)) return std::printf("FAIL  oracle.load  %s\n", err.c_str()), ++g_bad, 1;
  live::plane::Tracker tr;
  oracle::FakeGuest g;
  Board board(faults.duplicate_record);
  uint64_t masks = 0, blurs = 0, rows_compared = 0, incomplete = 0, unjoined = 0, mismatched = 0, stamp_wrong = 0, count_wrong = 0, no_frame = 0, not_all = 0;
  for (const Frame& fr : f.frames) {
    const uint32_t stamp = fr.guest_frame - 1;
    oracle::DriveFrame(tr, g, fr, stamp, plant);
    std::vector<PlaneFrame> frames;
    std::vector<PlaneDraw> draws;
    tr.Take(frames, draws);
    if (draws.size() != fr.masks.size() + fr.blurs.size() || frames.size() != (fr.masks.empty() ? 0u : 1u)) {
      ++count_wrong;
      continue;
    }
    board.Add(frames, draws);
    const PlaneFrame* pf = board.FrameAt(stamp);
    if (!fr.masks.empty() && !pf) ++no_frame;
    if (pf && pf->have != kPlaneHaveAll) ++not_all;
    std::map<uint64_t, uint32_t> nth;
    for (const oracle::MaskDraw& m : fr.masks) {
      const uint32_t n = nth[(uint64_t(m.object) << 32) | m.part]++;
      const PlaneDraw* h = board.Find(stamp, kPlaneMask, m.object, m.part, n);
      if (!h) {
        ++unjoined;
        continue;
      }
      if (h->frame != stamp || h->kind != kPlaneMask) ++stamp_wrong;
      RegTable t;
      BuildMask(pf, *h, faults, board.CameraBefore(stamp), t);
      Ref ref;
      for (uint32_t r = 0; r < 17; ++r) ref.Set(r, m.rows[r]);   // c0-c16, all uploaded now (c7 per proxy, c8 by sub_827EC600)
      uint32_t compared = 0;
      const bool eq = Matches(t, [&](uint32_t reg) -> const float* { return ref.valid[reg] ? ref.v[reg] : nullptr; }, &compared);
      bool complete = true;
      for (uint32_t r = 0; r < 64; ++r)
        if (ref.valid[r] && !t.have[r]) complete = false;
      masks++, rows_compared += compared;
      if (!eq) ++mismatched;
      if (!complete) ++incomplete;
    }
    uint32_t bn = 0;
    for (const auto& b : fr.blurs) {
      const PlaneDraw* h = board.Find(stamp, kPlaneBlur, 0, 0, bn++);
      if (!h) {
        ++unjoined;
        continue;
      }
      RegTable t;
      BuildBlur(*h, t);
      Ref ref;
      for (uint32_t r = 0; r < 8; ++r)
        if (r != 1) ref.Set(33 + r, b.data() + 4 * r);   // c34 is a pixel constant (op 28): not tracked
      uint32_t compared = 0;
      const bool eq = Matches(t, [&](uint32_t reg) -> const float* { return ref.valid[reg] ? ref.v[reg] : nullptr; }, &compared);
      bool complete = true;
      for (uint32_t r = 0; r < 64; ++r)
        if (ref.valid[r] && !t.have[r]) complete = false;
      blurs++, rows_compared += compared;
      if (!eq) ++mismatched;
      if (!complete) ++incomplete;
    }
  }
  char why[420];
  std::snprintf(why, sizeof why, "%zu frames, %llu mask draws, %llu blur draws, %llu rows compared, unjoined %llu, mismatched %llu, incomplete %llu, no frame block %llu, frame block not complete %llu, stamp wrong %llu, count wrong %llu", f.frames.size(),
                (unsigned long long)masks, (unsigned long long)blurs, (unsigned long long)rows_compared, (unsigned long long)unjoined, (unsigned long long)mismatched, (unsigned long long)incomplete,
                (unsigned long long)no_frame, (unsigned long long)not_all, (unsigned long long)stamp_wrong, (unsigned long long)count_wrong);
  const bool ok = masks + blurs > 0 && !unjoined && !mismatched && !incomplete && !no_frame && !not_all && !stamp_wrong && !count_wrong;
  Check(ok, ("writer.rows_equal " + path.substr(path.find_last_of("/\\") + 1)).c_str(), why);
  if (verbose) {
    const live::plane::Stats s = tr.GetStats();
    std::printf("      tracker: calls %llu, mask rows %llu, blur rows %llu, frames %llu, mask draws %llu, blur draws %llu, incomplete frames %llu, orphans %llu, unreadable %llu, overflow %llu\n", (unsigned long long)s.calls,
                (unsigned long long)s.rows_mask, (unsigned long long)s.rows_blur, (unsigned long long)s.frames, (unsigned long long)s.mask_draws, (unsigned long long)s.blur_draws,
                (unsigned long long)s.incomplete_frames, (unsigned long long)s.orphans, (unsigned long long)s.unreadable, (unsigned long long)s.overflow);
  }
  return ok ? 0 : 1;
}

static void GuardChecks() {
  using namespace rdr_scene::live::plane;
  // the table: every row is ours, the OLD range is not, and the rows cover exactly the owned registers
  bool all_ours = true;
  std::set<uint32_t> mask_regs, blur_regs;
  for (const Site& s : kSites) {
    all_ours = all_ours && Tracker::IsOurs(s.lr);
    for (uint32_t r = 0; r < s.rows; ++r) (s.chain == Chain::Mask ? mask_regs : blur_regs).insert(s.reg + r);
  }
  Check(all_ours && kSiteCount == 15, "guard.is_ours_covers_every_site", "IsOurs true for all 15 table rows");
  const std::set<uint32_t> want_mask = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, want_blur = {33, 35, 36, 37, 38, 39, 40};
  Check(mask_regs == want_mask && blur_regs == want_blur, "guard.table_covers_owned_registers", "mask c0-c16, blur c33 / c35 / c36-c39 / c40 (no EyePos, palette, c34)");
  Check(!Tracker::IsOurs(0x82BF24C0u) && !Tracker::IsOurs(0x82BF22CCu) && !Tracker::IsOurs(0x82BF4DE8u) && !Tracker::IsOurs(0x827EC62Cu) && FindSite(0x827EC6DCu) && FindSite(0x827EE248u), "guard.old_range_is_not_ours",
        "no LR of sub_82BF22C8's engine-parameter slots is accepted");
  Tracker t;
  oracle::FakeGuest g;
  const ReadBytes rd = [&g](uint32_t a, void* dst, size_t n) { return g.Read(a, dst, n); };
  const float rows[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  g.PutFloats(0x1000, rows, 16);
  t.OnConstants(0x827EC6F0u, 14, 0x1000, 4, 5, rd);      // the camera's row, but the wrong register
  t.OnConstants(0x827EC6F0u, 13, 0x1000, 3, 5, rd);      // the right register, the wrong count
  t.OnConstants(0x82BF24C0u, 13, 0x1000, 4, 5, rd);      // a foreign return address
  t.OnConstants(0x827EC6F0u, 13, 0x9999000, 4, 5, rd);   // unreadable
  const Stats s = t.GetStats();
  std::vector<PlaneFrame> fr;
  std::vector<PlaneDraw> d;
  t.OnPartDraw(0x40000000u, 1, 5);
  t.Take(fr, d);
  Check(s.orphans == 3 && s.unreadable == 1 && s.rows_mask == 0 && fr.size() == 1 && fr[0].have == 0 && d.size() == 1 && d[0].have == 0, "guard.counted_not_recorded", "3 orphans, 1 unreadable, no row recorded, the frame block owns nothing");
  Tracker o;
  for (uint32_t i = 0; i < kMaxPending + 5; ++i) o.OnPartDraw(0x40000000u, i, 7);
  Check(o.GetStats().overflow == 5, "guard.overflow_counted", "kMaxPending + 5 part draws");
  // "present" means written THIS frame: rows from frame 5 are not frame 6's
  Tracker p;
  oracle::MaskDraw m;
  for (uint32_t r = 0; r < 17; ++r)
    for (int c = 0; c < 4; ++c) m.rows[r][c] = float(r + c);
  oracle::Frame one;
  one.masks.push_back(m);
  oracle::DriveFrame(p, g, one, 5);
  std::vector<PlaneFrame> a5;
  std::vector<PlaneDraw> d5;
  p.Take(a5, d5);
  p.OnPartDraw(m.object, m.part, 6);   // frame 6 draws without the module's uploads
  std::vector<PlaneFrame> a6;
  std::vector<PlaneDraw> d6;
  p.Take(a6, d6);
  Check(a5.size() == 1 && a5[0].have == kPlaneHaveAll && a6.size() == 1 && a6[0].have == 0 && d6.size() == 1 && d6[0].have == 0 && p.GetStats().incomplete_frames == 1, "guard.present_is_this_frame",
        "frame 6 has no rows of frame 5's: have 0, counted incomplete");
  // lane 226 follow-up (S1): the PASS ENTRY snapshot. A frame whose part draw never runs (a skipped call) still gets its frame block and c7 from the register file the uploads fed.
  Tracker q;
  float entry_pos[4] = {};
  bool entry_ok = false;
  oracle::Plant ep;
  ep.entry_pos = entry_pos, ep.entry_ok = &entry_ok;
  oracle::DriveFrame(q, g, one, 8, ep);
  std::vector<PlaneFrame> a8;
  std::vector<PlaneDraw> d8;
  q.Take(a8, d8);
  bool pos_eq = entry_ok;
  for (int c = 0; c < 4; ++c) pos_eq = pos_eq && entry_pos[c] == m.rows[7][c];
  bool cam_eq = a8.size() == 1 && a8[0].have == kPlaneHaveAll;
  for (int c = 0; c < 16 && cam_eq; ++c) cam_eq = a8[0].camera[c] == m.rows[13 + c / 4][c % 4];
  Check(pos_eq && cam_eq && d8.empty(), "guard.pass_entry_snapshot", "no part draw ran: the frame block is complete (camera = c13-c16), c7 returned, no PlaneDraw made");
  float none[4] = {};
  Tracker q2;
  Check(!q2.OnPassEntry(9, none), "guard.pass_entry_not_this_frame", "no upload this frame: c7 is not reported");
}

static void BoardChecks() {
  Board b;
  std::vector<PlaneFrame> frames;
  std::vector<PlaneDraw> draws;
  const auto mk = [&](uint32_t frame, uint32_t kind, uint32_t obj, uint32_t part, uint32_t ord, float tag) {
    PlaneDraw d;
    d.frame = frame, d.kind = kind, d.object = obj, d.part = part, d.ordinal = ord, d.plane_pos[0] = tag, d.have = kPlaneDrawHavePos;
    draws.push_back(d);
  };
  mk(10, kPlaneMask, 0xA, 1, 0, 1.f);
  mk(10, kPlaneMask, 0xA, 1, 1, 2.f);   // the second draw of the same part (the second proxy instance)
  mk(10, kPlaneMask, 0xB, 0, 2, 3.f);
  mk(10, kPlaneBlur, 0, 0, 3, 4.f);
  mk(10, kPlaneBlur, 0, 0, 4, 5.f);
  mk(11, kPlaneMask, 0xA, 1, 0, 6.f);
  PlaneFrame f10, f11;
  f10.frame = 10, f11.frame = 11, f10.have = f11.have = kPlaneHaveAll, f10.camera[0] = 1.f, f11.camera[0] = 2.f;
  frames = {f10, f11};
  b.Add(frames, draws);
  b.Add(frames, draws);   // a record delivered twice adds nothing
  const PlaneDraw* h0 = b.Find(10, kPlaneMask, 0xA, 1, 0);
  const PlaneDraw* h1 = b.Find(10, kPlaneMask, 0xA, 1, 1);
  const PlaneDraw* hb = b.Find(10, kPlaneMask, 0xB, 0, 0);
  const PlaneDraw* bl1 = b.Find(10, kPlaneBlur, 99, 99, 1);
  Check(h0 && h1 && hb && bl1 && h0->plane_pos[0] == 1.f && h1->plane_pos[0] == 2.f && hb->plane_pos[0] == 3.f && bl1->plane_pos[0] == 5.f && !b.Find(10, kPlaneMask, 0xA, 1, 2) && !b.Find(12, kPlaneMask, 0xA, 1, 0),
        "board.join_object_part_nth", "same part twice, blur by nth, missing stamp / nth -> null");
  Check(b.draw_count() == 6, "board.record_twice_adds_nothing", "6 draws held after the same record twice");
  const float* cam = b.CameraBefore(11);
  Check(cam && cam[0] == 1.f && !b.CameraBefore(10), "board.camera_before", "the previous stamp's camera");
  uint32_t newest = 0;
  Check(b.newest_stamp(newest) && newest == 11, "board.newest_stamp", "11");
  Board dup(true);   // the plant duplicate_record's identity: it must lose the second instance
  dup.Add(frames, draws);
  Check(dup.Find(10, kPlaneMask, 0xA, 1, 1) == nullptr, "board.duplicate_record_plant_loses_instance", "(a control of the plant: by (kind, object, part) the second instance is folded away)");
}

// lane 159 (review S2): the 120 Hz blend key of an own draw. The pump pairs two ticks' draws by key and checks only counts and bytes, so the key of an own draw must differ from the stream draw's
// (a draw refused one tick and owned the next has two block layouts), velocity rebinds from scene draws, and it must be deterministic. The ranges are pinned to the shaders' std140 layouts.
static void BlendKeyChecks() {
  using rdr_adapter::BlendKeyBase;
  const uint64_t vs = 0x1122334455667788ull, ps = 0x99AABBCCDDEEFF00ull, vo = 4096, io = 8192;
  const uint64_t stream = BlendKeyBase(vs, ps, vo, io, false, false), own = BlendKeyBase(vs, ps, vo, io, false, true), vel = BlendKeyBase(vs, ps, vo, io, true, false);
  Check(stream != own, "blend.own_salted", "an own draw never shares the stream draw's key");
  Check(stream != vel && own != BlendKeyBase(vs, ps, vo, io, true, true), "blend.velocity_apart");
  Check(own == BlendKeyBase(vs, ps, vo, io, false, true) && own != BlendKeyBase(vs, ps, vo + 16, io, false, true), "blend.deterministic_and_geometry_keyed");
  // the mask block (rdr_plane_mask.vert): EyePos 0, PlanePos 16, Fade[2] 32, PlaneInv[4] 64, CameraViewProj[4] 128, Bools[2] 192, palette 224..1231; blur block: BlurStep 0, PlaneMatrix 16..79, PlaneScale 80, Axis 96
  const uint32_t mask_blocks[3][2] = {{0, 32}, {64, 128}, {224, 1008}}, mask_bytes = 1232, blur_range[2] = {16, 64}, blur_bytes = 112;
  bool ok = true;
  for (const auto& r : mask_blocks) ok = ok && r[0] + r[1] <= mask_bytes;
  ok = ok && mask_blocks[2][0] + mask_blocks[2][1] == mask_bytes && blur_range[0] + blur_range[1] <= blur_bytes;
  Check(ok, "blend.ranges_inside_blocks");
}

// E (lane 205, check K3): the blur's lane rule. sub_827ECD10 stores ONE float into r1+80 and uploads the 16-byte row from it (c33: 0x827ECDA8-0x827ECDB0, c35: 0x827ECDD0-0x827ECDE8), so lanes y z w are
// stack leftovers. The stream and the record carry the same bytes, but a NaN leftover fails a float ==. A run of 600 blur draws whose c33.w and c35.w are 0xFFFFFFFF on both sides (and y, z leftovers
// that differ between the two sides): the fixed code replaces all of them, the plant blur_nan_lane (today's comparator) refuses all. The control: the written lane x differs -> refused, with or without the plant.
static void BlurLaneChecks(const Faults& faults) {
  const uint32_t kNaN = 0xFFFFFFFFu;
  const auto run = [&](const Faults& flt, bool x_differs, bool leftovers_differ) {
    uint32_t equal = 0;
    for (uint32_t i = 0; i < 600; ++i) {
      PlaneDraw d;
      d.kind = kPlaneBlur, d.have = kPlaneDrawHaveBlur, d.ordinal = i;
      const float x = 1.0f + 0.001f * float(i);
      d.blur_scale[0] = x, d.blur_weight[0] = 0.5f + 0.001f * float(i);
      std::memcpy(&d.blur_scale[3], &kNaN, 4), std::memcpy(&d.blur_weight[3], &kNaN, 4);
      for (int k = 0; k < 16; ++k) d.blur_matrix[k] = float(k) * 0.25f + float(i);
      d.blur_point[0] = 640.f, d.blur_point[2] = 640.f, d.blur_point[3] = 1.f;
      Ref ref;
      ref.Set(33, d.blur_scale), ref.Set(35, d.blur_weight);
      for (uint32_t r = 0; r < 4; ++r) ref.Set(36 + r, d.blur_matrix + 4 * r);
      ref.Set(40, d.blur_point);
      if (x_differs) ref.v[33][0] += 0.125f;                 // a WRITTEN lane
      if (leftovers_differ) ref.v[33][1] = 3.0f, ref.v[35][2] = -7.0f;   // lanes the game never wrote
      RegTable t;
      BuildBlur(d, t, flt);
      uint32_t n = 0;
      equal += Matches(t, [&](uint32_t reg) -> const float* { return ref.valid[reg] ? ref.v[reg] : nullptr; }, &n) && n == 7;
    }
    return equal;
  };
  Faults none = faults;
  none.blur_radius = false;
  const uint32_t same = run(none, false, true);
  Check(same == 600, "blur.nan_lanes_replaced", std::to_string(same) + " of 600 blur draws verify with c33.w = c35.w = 0xFFFFFFFF on both sides and leftover y / z lanes that differ (the plant blur_nan_lane: 0)");
  Faults radius = faults;
  radius.blur_radius = true;   // the record's x scaled 8x: the written lane really differs
  const uint32_t refused_x = run(none, true, false);
  Check(refused_x == 0, "blur.written_lane_still_refused", std::to_string(600 - refused_x) + " of 600 refused when the x lane (the one the game writes) differs");
  const uint32_t refused_radius = run(radius, false, false);
  Check(refused_radius == 0, "blur.radius_still_refused", std::to_string(600 - refused_radius) + " of 600 refused under the blur_radius fault (the record's tap step 8x the stream's)");
}

int main(int argc, char** argv) {
  std::vector<std::string> files;
  std::string plant;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else files.push_back(a);
  }
  Faults faults;
  oracle::Plant dplant;
  std::string err;
  if (plant == "writer_lr_missing") dplant.drop_lr = 0x827EC6DCu;
  else if (plant == "upload_after_snapshot") dplant.upload_after_snapshot = true;
  else if (plant == "foreign_upload_between") dplant.foreign_between = true;
  else if (plant == "foreign_ignored") dplant.foreign_ignored = true;   // the control: exit 0
  else if (!plant.empty() && !Faults::Parse(plant, faults, err)) return std::printf("%s\n", err.c_str()), 2;
  if (plant.empty()) {
    RecordChecks();
    BlendKeyChecks();
    GuardChecks();
    BoardChecks();
  }
  BlurLaneChecks(faults);
  if (files.empty()) return std::printf("FAIL  no oracle files given\n"), 2;
  for (const std::string& f : files) WriterChecks(f, faults, dplant, plant.empty());
  std::printf("%s: %d failed\n", g_bad ? "FAILED" : "OK", g_bad);
  return g_bad ? 1 : 0;
}
