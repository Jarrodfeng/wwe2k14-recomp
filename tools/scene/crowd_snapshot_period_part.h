// Part of crowd_snapshot_unit.cpp (S111 CUTCROWD2, docs\research\s111\CUTCROWD2.md): record r carries the crowd snapshot of latch r - 1, the latch whose scene camera it carries (latch_period.h; the camera's
// pairing is inputs_contrib.h Take, cam_period_). Included by the unit after crowd_snapshot_s2_part.h: it uses Check, Fresh, Draw, Member, Palette, g_stamps, g_last_stamp, S2Read and g_s2_case.
// Each walk draws one stands member whose index names its period (world x = 10 * period). The camera of the same record is a period::OfPalette fed the period, as the scene camera is fed its latch read.
// Twin crowd_leads (snap_plant 16): the crowd takes its own latch's snapshot (the rule before CUTCROWD2) -> "period.crowd_takes_previous_latch" fails.
// Twin crowd_pairs_off (snap_plant 17): a mode-Off latch feeds the crowd's pairing (the camera's is not fed then) -> "period.mode_switch_pairs_with_camera" fails.
#pragma once

static void PeriodTests(crowd::Contributor& c, int p) {
  Fresh(c, p);
  c.SetLatchReader(&S2Read);   // a reader that reads (walked frames are then proved: latch_ok), nothing shown
  g_s2_case = 0;
  Palette(c, 0, 0);
  ::scene_inputs::period::OfPalette<uint64_t> cam;   // the scene camera's pairing
  struct Step {
    uint64_t frame;
    uint32_t other;   // persons the walk drew that we do not model (1 = this latch's snapshot is not whole)
    int stamps;       // stamps the record must make: the verdict of the snapshot it CARRIES
  };
  // 400: the first latch (own read); 401: latch 400's (whole); 402: latch 401's (unmodelled, no stamp); no latch 403 (a gap); 404: its own; 405: latch 404's.
  const Step steps[] = {{400, 0, 1}, {401, 1, 1}, {402, 0, 0}, {404, 0, 1}, {405, 0, 1}};
  bool pair_ok = true, stamp_ok = true;
  std::string paired, stamped;
  for (const Step& s : steps) {
    Draw(c, {Member(0, 0, uint32_t(s.frame))}, s.other);   // walk s.frame
    live::FrameInfo fi;
    fi.guest_frame = s.frame;
    SceneTables t;
    live::Counters cnt;
    const int before = g_stamps;
    c.Latch(fi);
    c.Emit(fi, t, cnt);
    const uint64_t camera = cam.Pick(s.frame, s.frame);
    const uint64_t crowd_period = t.objects.size() == 1 ? uint64_t(t.objects[0].world.m[12] / 10.f + 0.5f) : 0;
    pair_ok = pair_ok && crowd_period == camera;
    paired += " r" + std::to_string((unsigned long long)s.frame) + " crowd " + std::to_string((unsigned long long)crowd_period) + " camera " + std::to_string((unsigned long long)camera);
    const int made = g_stamps - before;
    stamp_ok = stamp_ok && made == s.stamps && (made == 0 || g_last_stamp == s.frame - 1);
    stamped += " r" + std::to_string((unsigned long long)s.frame) + " " + std::to_string(made) + "@" + std::to_string((unsigned long long)g_last_stamp);
  }
  Check(pair_ok, "period.crowd_takes_previous_latch", paired);
  Check(stamp_ok, "period.verdict_of_the_carried_snapshot", stamped);
  std::string line;
  c.Census(line);
  const size_t at = line.find("crowd_period:");
  Check(line.find("crowd_period: latches 5 previous 3 own_first 1 own_gap 1") != std::string::npos, "period.census", at == std::string::npos ? "no crowd_period" : line.substr(at, 80));
  // The mode switch (CUTCROWD2 review): the scene camera is picked only on a latch with the mode on (inputs_contrib.h Latch runs Take, so cam_period_.Pick, only then), so the crowd's pairing is fed
  // under the same condition. 600 on: both their own (first); 601 Off: no record camera, the crowd takes its own and does not feed the pairing (no stamp: Mode Off judges nothing);
  // 602 on: the camera's latch before is 600, a gap: both their own; 603: both latch 602's. Twin crowd_pairs_off (snap_plant 17): the Off latch feeds the crowd's pairing -> record 602 carries
  // the Off latch 601's crowd under camera 602, and the Off latch stamps latch 600's verdict.
  Fresh(c, p);
  c.SetLatchReader(&S2Read);
  g_s2_case = 0;
  Palette(c, 0, 0);
  ::scene_inputs::period::OfPalette<uint64_t> cam2;   // the scene camera's pairing, fed as inputs_contrib.h Latch feeds it (mode on only)
  struct Switch {
    uint64_t frame;
    bool on;
    int stamps;
  };
  const Switch sw[] = {{600, true, 1}, {601, false, 0}, {602, true, 1}, {603, true, 1}};
  bool sw_ok = true;
  std::string sw_paired;
  for (const Switch& s : sw) {
    g_mode_on = s.on;
    Draw(c, {Member(0, 0, uint32_t(s.frame))});   // walk s.frame
    live::FrameInfo fi;
    fi.guest_frame = s.frame;
    SceneTables t;
    live::Counters cnt;
    const int before = g_stamps;
    c.Latch(fi);
    c.Emit(fi, t, cnt);
    const int made = g_stamps - before;
    sw_ok = sw_ok && made == s.stamps;
    sw_paired += " r" + std::to_string((unsigned long long)s.frame) + (s.on ? "" : " off") + " stamps " + std::to_string(made);
    if (!s.on) continue;   // Mode Off: the inputs latch takes nothing, the record has no camera
    const uint64_t camera = cam2.Pick(s.frame, s.frame);
    const uint64_t crowd_period = t.objects.size() == 1 ? uint64_t(t.objects[0].world.m[12] / 10.f + 0.5f) : 0;
    sw_ok = sw_ok && crowd_period == camera;
    sw_paired += " crowd " + std::to_string((unsigned long long)crowd_period) + " camera " + std::to_string((unsigned long long)camera);
  }
  g_mode_on = true;
  Check(sw_ok, "period.mode_switch_pairs_with_camera", sw_paired);
  std::string line2;
  c.Census(line2);
  const size_t at2 = line2.find("crowd_period:");
  Check(line2.find("crowd_period: latches 3 previous 1 own_first 1 own_gap 1") != std::string::npos, "period.mode_switch_census", at2 == std::string::npos ? "no crowd_period" : line2.substr(at2, 80));
}
