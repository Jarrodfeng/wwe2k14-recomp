// Part of crowd_snapshot_unit.cpp (S103 lane S2): HOLD BY PRESENCE for bit 10 (crowd_snapshot_core.h Presence / ApplyPresence / PresencePolicy, presence_rules.h g_crowd). Included by the unit after its helpers
// (Check, Fresh, Record, Draw, Palette, Member, g_stamps). The contributor is the SHIPPED crowd_contrib.cpp; the latch reader is a small forged one (S2Read) whose snapshot is whole except for what the
// case puts in it (a shown / hidden sign, a holder the game draws / hides / we do not model, a picture). Twins (crowd_snapshot_check.py): presence_ignored, presence_lod_absent, presence_partial_kept.
#pragma once

static int g_s2_case = 0;   // 0 nothing shown, 1 a shown sign, 2 a sign the game's gate hides, 3 a holder the game draws, 4 a holder above LOD 0 (kShLod: drawn, not modelled), 5 a holder the gate hides, 6 a picture, 7 a holder only the reader's screen-test port hides (S103 C1)
static void S2World(float* c) {
  for (int i = 0; i < 12; ++i) c[i] = 0.f;
  for (int r = 0; r < 3; ++r) c[r * 4 + r] = 1.f;
  c[3] = 5.f;
}
static uint32_t S2Read(crowd::LatchSnapshot& out) {
  out.members.push_back(Member(0, 0, 0));
  out.members.push_back(Member(0, 0, 1));
  if (g_s2_case == 1 || g_s2_case == 2) {
    crowd::PropObs p;
    p.site = 0, p.index = 7;
    S2World(p.c);
    p.hidden = g_s2_case == 2 ? crowd::kPhDistance : crowd::kPhNone;
    out.props.push_back(p);
  }
  if ((g_s2_case >= 3 && g_s2_case <= 5) || g_s2_case == 7) {
    crowd::SingleObs s;
    s.hidden = g_s2_case == 3 ? crowd::kShNone : g_s2_case == 4 ? crowd::kShLod : g_s2_case == 7 ? crowd::kShScreen : crowd::kShFlags;
    s.world_kind = 1, s.bind_ok = true;
    S2World(s.c);
    out.singles.push_back(s);
  }
  if (g_s2_case == 6) {
    crowd::PictureObs q;
    q.key = 0x1234, q.width = 4, q.height = 4, q.why = 0;
    out.pictures.push_back(q);
  }
  return 0;
}
// One OURS frame (the crowd entry skipped, the latch reads S2Read) under the given switch; returns the stamps made.
static int S2Ours(crowd::Contributor& c, int p, int kase, bool presence) {
  Fresh(c, p);
  c.SetLatchReader(&S2Read);
  Palette(c, 0, 0);
  g_s2_case = kase;
  rdr_scene::live::presence::g_crowd.store(presence);
  c.NoteOursSkip();
  Record(c, 101, true);
  rdr_scene::live::presence::g_crowd.store(false);
  return g_stamps;
}
// One WALKED frame (the walk's draw, then the latch compares the reader with it).
static int S2Walked(crowd::Contributor& c, int p, int kase, bool presence) {
  Fresh(c, p);
  c.SetLatchReader(&S2Read);
  Palette(c, 0, 0);
  g_s2_case = kase;
  rdr_scene::live::presence::g_crowd.store(presence);
  Draw(c, {Member(0, 0, 0), Member(0, 0, 1)});
  Record(c, 101, true);
  rdr_scene::live::presence::g_crowd.store(false);
  return g_stamps;
}

// S103 C1 (R14-4): one WALKED frame whose crowd draw drew a sign (crowd_hooks.h OnOp21 at a prop site: NoteProp AND NoteUnmodelled) plus `extra` other unmodelled op 21s.
static int S2WalkedProp(crowd::Contributor& c, int p, uint32_t extra, bool presence) {
  Fresh(c, p);
  c.SetLatchReader(&S2Read);
  Palette(c, 0, 0);
  g_s2_case = 0;
  rdr_scene::live::presence::g_crowd.store(presence);
  c.BeginDraw(0);
  c.NoteMember(Member(0, 0, 0)), c.NoteMember(Member(0, 0, 1));
  float w[12];
  S2World(w);
  c.NoteProp(0, w), c.NoteUnmodelled();
  for (uint32_t i = 0; i < extra; ++i) c.NoteUnmodelled();
  c.EndDraw();
  Record(c, 101, true);
  rdr_scene::live::presence::g_crowd.store(false);
  return g_stamps;
}

static void S2PresenceTests(crowd::Contributor& c, int p) {
  using crowd::SnapWhy;
  Check(crowd::kSingleHiddenLod == crowd::kShLod, "s2.lod_code_agrees");
  // switch OFF: an OURS frame's latch-built snapshot stays latch_partial (the shipped rule before S2)
  Check(S2Ours(c, p, 0, false) == 0 && c.SnapCensus().why[uint32_t(SnapWhy::LatchPartial)] == 1, "s2.switch_off_unchanged", "stamps " + std::to_string(g_stamps));
  // switch ON, nothing shown: whole, stamped for the hook frame
  Check(S2Ours(c, p, 0, true) == 1 && g_last_stamp == 100 && c.SnapCensus().why[uint32_t(SnapWhy::Whole)] == 1, "s2.nothing_shown_stamps", "stamps " + std::to_string(g_stamps));
  // a shown sign: not whole (holder_present), no stamp; counted in the census
  Check(S2Ours(c, p, 1, true) == 0 && c.SnapCensus().why[uint32_t(SnapWhy::HolderPresent)] == 1 && c.LastPresence().props == 1, "s2.holder_no_stamp", "stamps " + std::to_string(g_stamps));
  {
    std::string line;
    c.Census(line);
    Check(line.find("holder_present 1") != std::string::npos && line.find("holder_frames 1 (shown props 1") != std::string::npos, "s2.census_holder_line", line.substr(line.find("crowd_snapshot")));
  }
  // a sign the game's own gate hides is not present
  Check(S2Ours(c, p, 2, true) == 1, "s2.hidden_sign_stamps", "stamps " + std::to_string(g_stamps));
  // a holder the game draws: present
  Check(S2Ours(c, p, 3, true) == 0 && c.LastPresence().singles == 1, "s2.shown_holder_no_stamp", "stamps " + std::to_string(g_stamps));
  // a holder above LOD 0 (drawn by the game, not modelled by us): present, never assumed absent
  Check(S2Ours(c, p, 4, true) == 0, "s2.lod_holder_present", "stamps " + std::to_string(g_stamps));
  // a holder the gate hides: not present
  Check(S2Ours(c, p, 5, true) == 1, "s2.gated_holder_stamps", "stamps " + std::to_string(g_stamps));
  // S103 C1 review fix 2: a holder only the reader's screen-test port hides is PRESENT (fail closed) until the port is trusted (--wwe2k14_rdr_scene_crowd_screen_trust)
  Check(S2Ours(c, p, 7, true) == 0, "s2.screen_hidden_holder_present", "stamps " + std::to_string(g_stamps));
  rdr_scene::live::presence::g_crowd_screen_trust.store(true);
  Check(S2Ours(c, p, 7, true) == 1, "s2.screen_hidden_holder_trusted", "stamps " + std::to_string(g_stamps));
  rdr_scene::live::presence::g_crowd_screen_trust.store(false);
  // a sign picture read for the frame: present
  Check(S2Ours(c, p, 6, true) == 0, "s2.picture_no_stamp", "stamps " + std::to_string(g_stamps));
  // walked frames: the same rule (a walked frame's stamp lets the next frame go OURS)
  Check(S2Walked(c, p, 0, true) == 1, "s2.walked_nothing_shown_stamps", "stamps " + std::to_string(g_stamps));
  Check(S2Walked(c, p, 1, true) == 0 && c.SnapCensus().why[uint32_t(SnapWhy::HolderPresent)] == 1, "s2.walked_holder_no_stamp", "stamps " + std::to_string(g_stamps));
  {   // (last: the presence census counters are not reset by Fresh) S103 C1 (R14-4, run 32: every withheld frame read `unmodelled`, holder_present 0, although they were the sign frames): the label, never the stamp
    const int on = S2WalkedProp(c, p, 0, true);
    const bool on_ok = on == 0 && c.SnapCensus().why[uint32_t(SnapWhy::HolderPresent)] == 1 && c.SnapCensus().why[uint32_t(SnapWhy::Unmodelled)] == 0;
    Check(on_ok, "s2.prop_frame_holder_present", "stamps " + std::to_string(on) + " holder_present " + std::to_string(c.SnapCensus().why[uint32_t(SnapWhy::HolderPresent)]));
    const int off = S2WalkedProp(c, p, 0, false);
    Check(off == 0 && c.SnapCensus().why[uint32_t(SnapWhy::Unmodelled)] == 1, "s2.prop_frame_switch_off_unmodelled");
    const int more = S2WalkedProp(c, p, 1, true);   // another unmodelled op 21 besides the sign: still unmodelled
    Check(more == 0 && c.SnapCensus().why[uint32_t(SnapWhy::Unmodelled)] == 1, "s2.prop_frame_other_stays_unmodelled");
  }
}
