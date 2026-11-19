// Offline proof of S103 lane L2d (src\renderer_game\scene_kind.h + its use in scene_frame.h DecideNow + inputs_core.h kCawDrop): every frame kind needs an owner.
// No game, no SDK, no run. Checks (each FAIL line names its check):
//   flat.ours_with_flag        a stage-less frame with whole 2D / movie / particles decides OURS with --wwe2k14_rdr_scene_flat on
//   flat.never_with_flag_off   ... and never with it off (the reason is flag_off, last)
//   flat.hole_falls_back       a flat frame with a 2D hole falls back (2d_holes), also with the switch on
//   flat.reasons               not taken / no origins / count / refused / movie planes / particles not recorded / held, in order; a planes-only refusal is movie_planes
//   flat.not_flat              a stage, a 3D object or a Scene3D stamp = not a flat frame (and never counted under flat)
//   mask.match_unchanged       match and entrance require exactly today's registered mask; a drop for them is refused
//   decide.match_unchanged     the REAL DecideNow: a match frame missing bit 10 walks (InputsMissing), the old `inputs_mask != req` answer, over every single missing bit
//   mask.caw                   CAW = every input but crowd / ring / plane / screens / pools; DecideNow on a CAW-published frame missing only those decides OURS, missing caster_entries walks
//   kind.lag                   a kind published for j or j - 1 is taken; older = Match
//   census.line                the [scene][kind] line: per kind OURS share and first reasons
//   walk.sticky_stage_flat     S103 WB: a record with a stage and 251 objects but NO model-class walk bit is FLAT (twins T1 walked stays not flat, T2 walked + NOT READY stamp stays not flat, plant 6 the old rule must fail); walk.hooks the walk-hook list; walk.kind_mask bit 7 never changes the kind; walk.board OrPad; walk.log the change-driven line
// Planted twins (--plant N, RDR_L2D_PLANTS; tools\scene\l2d_check.py runs each and names the check it must fail):
//   S103 CW1: caw.classifier / caw.hysteresis / caw.line / caw.flat = sequence 76 (through the hysteresis) publishes Caw and the real DecideNow answers READY with the audited drop (crowd / ring rows / plane constants; screens and
//   pools stay required), another sequence is Match, enter after 3 frames / leave at once, a 76 menu with no model walk is FLAT; plant 7 (the classifier unwired) must fail caw.classifier
//   1 a match drop accepted -> mask.match_unchanged / decide.match_unchanged; 2 holes ignored -> flat.hole_falls_back; 3 switch ignored -> flat.never_with_flag_off; 4 lag ignored -> kind.lag; 6 the old rule (a sticky stage is trusted without a walk) -> walk.sticky_stage_flat
//   pwsh -NoProfile -File tools\scene\build_l2d_unit.ps1 [-Plant N]  ->  out\tools\l2d_unit.exe
#define RDR_L2D_PLANTS 1
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../src/scene_live/inputs_core.h"
#include "../../src/renderer_game/scene_skip_core.h"

namespace sk = scene_kind;
namespace sf = scene_frame;
namespace si = scene_inputs;

static int g_fail = 0;
#define CHECK(name, c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s (line %d): %s\n", name, __LINE__, #c); } } while (0)
static bool Has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

static sk::FlatFacts WholeFlat() {
  sk::FlatFacts f;
  f.has_stage = false, f.objects3d = 0, f.scene3d_stamp = false;
  f.twod = sk::TwoD::Whole, f.refused_other = 0, f.movie_rows = 1, f.movie_planes_missing = 0, f.particles = sk::Parts::Whole;
  return f;
}

// Stamps every bit of `stamped` for hook frame f - 1 (every input is lag 1) and decides f.
static rdr_own::OwnedPassStamp DecideWith(uint64_t f, uint32_t stamped) {
  for (int b = 0; b < si::kInputCount; ++b)
    if ((stamped >> b) & 1u) si::Capture(b, f - 1);
  return sf::DecideNow(f);
}
static bool IsOurs(const rdr_own::OwnedPassStamp& s) { return s.decision == uint8_t(rdr_own::Decision::Ours); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) sk::g_plant = std::atoi(argv[i + 1]);
  if (sk::g_plant) std::printf("plant %d\n", sk::g_plant);

  // ---- the flat frame verdict
  {
    sk::g_flat.store(true);
    CHECK("flat.ours_with_flag", sk::FlatVerdict(WholeFlat()) == sk::FlatWhy::Ours);
    sk::FlatFacts nomovie = WholeFlat();
    nomovie.movie_rows = 0;   // a menu without a movie
    CHECK("flat.ours_with_flag", sk::FlatVerdict(nomovie) == sk::FlatWhy::Ours);
    sk::FlatFacts empty_parts = WholeFlat();
    empty_parts.particles = sk::Parts::Whole;
    CHECK("flat.ours_with_flag", sk::IsFlatFrame(empty_parts));
    sk::g_flat.store(false);
    CHECK("flat.never_with_flag_off", sk::FlatVerdict(WholeFlat()) == sk::FlatWhy::FlagOff);
    CHECK("flat.never_with_flag_off", sk::FlatVerdict(nomovie) != sk::FlatWhy::Ours);
    sk::g_flat.store(true);

    sk::FlatFacts hole = WholeFlat();
    hole.twod = sk::TwoD::Holes;
    CHECK("flat.hole_falls_back", sk::FlatVerdict(hole) == sk::FlatWhy::TwoDHoles);

    auto with = [](void (*set)(sk::FlatFacts&)) { sk::FlatFacts f = WholeFlat(); set(f); return sk::FlatVerdict(f); };
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.twod = sk::TwoD::NotTaken; }) == sk::FlatWhy::TwoDNotTaken);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.twod = sk::TwoD::NoOrigins; }) == sk::FlatWhy::TwoDNoOrigins);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.twod = sk::TwoD::CountDiffers; }) == sk::FlatWhy::TwoDCountDiffers);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.twod = sk::TwoD::Refused, f.refused_other = 2; }) == sk::FlatWhy::TwoDRefused);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.twod = sk::TwoD::Refused, f.refused_other = 0, f.movie_planes_missing = 1; }) == sk::FlatWhy::MoviePlanes);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.movie_planes_missing = 1; }) == sk::FlatWhy::MoviePlanes);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.particles = sk::Parts::NotRecorded; }) == sk::FlatWhy::ParticlesNotRecorded);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.particles = sk::Parts::Held; }) == sk::FlatWhy::ParticlesHeld);
    CHECK("flat.reasons", with([](sk::FlatFacts& f) { f.twod = sk::TwoD::Holes, f.particles = sk::Parts::Held; }) != sk::FlatWhy::ParticlesHeld || sk::g_plant == 2);   // the first reason wins
    CHECK("flat.reasons", sk::FlatVerdict(sk::FlatFacts{}) == sk::FlatWhy::NotFlatStage);   // default facts = a 3D frame (fail closed)

    CHECK("flat.not_flat", with([](sk::FlatFacts& f) { f.has_stage = true; }) == sk::FlatWhy::NotFlatStage);
    CHECK("flat.not_flat", with([](sk::FlatFacts& f) { f.objects3d = 3; }) == sk::FlatWhy::NotFlatObjects);
    CHECK("flat.not_flat", with([](sk::FlatFacts& f) { f.scene3d_stamp = true; }) == sk::FlatWhy::NotFlatHook);
    sk::FlatFacts stage = WholeFlat();
    stage.has_stage = true;
    CHECK("flat.not_flat", !sk::IsFlatFrame(stage));
  }

  // ---- masks over the real registration (inputs_core.h RegisterAll + RegisterSceneMasks)
  static const int mode2 = 2;
  sf::g_mode_cvar.store(&mode2);
  si::RegisterAll();
  sf::SetReady(true);
  const uint32_t reg = rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load();
  const uint32_t all = si::AllMask();
  CHECK("mask.match_unchanged", reg == (all & ~(1u << si::kInSphereOcc)));   // today's mask: every input but the sphere occluders (registered with their hook)
  // a hostile drop for match / entrance must be refused (plant 1 accepts it)
  const bool took_match = sk::SetKindDrop(sk::Kind::Match, 1u << si::kInCrowdSnapshot);
  const bool took_entr = sk::SetKindDrop(sk::Kind::Entrance, 1u << si::kInStagePools);
  CHECK("mask.match_unchanged", !took_match && !took_entr);
  CHECK("mask.match_unchanged", sk::RequiredFor(sk::Kind::Match, reg) == reg);
  CHECK("mask.match_unchanged", sk::RequiredFor(sk::Kind::Entrance, reg) == reg);
  CHECK("mask.match_unchanged", si::SceneMask(sk::Kind::Match) == all && si::SceneMask(sk::Kind::Entrance) == all);
  CHECK("mask.match_unchanged", sk::RequiredFor(sk::Kind::Flat, reg) == reg);   // a hook on a flat-published frame gets the strictest mask

  CHECK("mask.caw_dev_only", si::SceneMask(sk::Kind::Caw) == all);   // without the switch (the stream build's default) a Caw frame needs every input
  sk::g_caw_enabled.store(true);   // (start_settings.cpp: always in the stream-free build, dev && --wwe2k14_rdr_scene_caw in the stream build)
  // S103 CW1 audit (inputs_core.h kCawDrop): exactly crowd / ring rows / plane constants are dropped; the two inputs the editor feeds (screens, pools) and everything else stay required. Written out here
  // independently of the constant so a drifting kCawDrop fails.
  const uint32_t caw_want = all & ~((1u << si::kInCrowdSnapshot) | (1u << si::kInRingRows) | (1u << si::kInPlaneConstants));
  CHECK("mask.caw", si::SceneMask(sk::Kind::Caw) == caw_want);
  CHECK("mask.caw", (si::SceneMask(sk::Kind::Caw) >> si::kInStageScreens) & 1u && (si::SceneMask(sk::Kind::Caw) >> si::kInStagePools) & 1u);   // fed in the editor: not dropped
  CHECK("mask.caw", (si::SceneMask(sk::Kind::Caw) >> si::kInCasterEntries) & 1u);   // the shadow module + rim entries stay required
  CHECK("mask.caw", (si::SceneMask(sk::Kind::Caw) >> si::kInCamera) & 1u);

  // ---- the REAL DecideNow
  {
    uint64_t f = 100;
    // match: every registered input stamped -> OURS (nothing published = match)
    CHECK("decide.match_unchanged", sk::KindFor(f) == sk::Kind::Match);
    const rdr_own::OwnedPassStamp mours = DecideWith(f, reg);
    CHECK("decide.match_unchanged", IsOurs(mours));
    CHECK("decide.match_unchanged", mours.pad == 0);   // a match stamp is byte-identical to before
    // every single missing bit walks as InputsMissing, exactly the old `inputs_mask != req`
    // The OLD rule (before L2d) computed from what the TEST stamped, independent of what DecideNow returns: OURS iff every registered input was stamped. The decision must agree on every subset
    // (single missing bits, pairs, everything), so a mask that wrongly drops one bit disagrees with it.
    for (int b = 0; b < si::kInputCount; ++b) {
      if (!((reg >> b) & 1u)) continue;
      f += 2;
      const uint32_t stamped = reg & ~(1u << b);
      const rdr_own::OwnedPassStamp st = DecideWith(f, stamped);
      const bool old_ours = (stamped == reg);
      CHECK("decide.match_unchanged", IsOurs(st) == old_ours && !old_ours && st.why_not == uint8_t(rdr_own::WhyNot::InputsMissing) && st.inputs_mask == stamped);
      for (int b2 = b + 1; b2 < si::kInputCount; ++b2) {
        if (!((reg >> b2) & 1u)) continue;
        f += 2;
        const uint32_t two = reg & ~(1u << b) & ~(1u << b2);
        CHECK("decide.match_unchanged", IsOurs(DecideWith(f, two)) == (two == reg));
      }
    }
    // the same frames as an ENTRANCE (published for the frame before): unchanged
    f += 2;
    sk::PublishKind(f - 1, sk::Kind::Entrance);
    CHECK("decide.match_unchanged", !IsOurs(DecideWith(f, reg & ~(1u << si::kInCrowdSnapshot))));
    // CAW: missing only the parts the editor has none of -> OURS; missing caster_entries -> walks
    f += 2;
    sk::PublishKind(f - 1, sk::Kind::Caw);
    const uint32_t caw_drop = all & ~caw_want;
    const rdr_own::OwnedPassStamp cours = DecideWith(f, reg & ~caw_drop);
    CHECK("mask.caw", IsOurs(cours));
    CHECK("mask.caw", cours.pad == uint8_t(sk::Kind::Caw));   // the kind travels in the stamp (the render side's freshness check asks the same mask)
    f += 2;
    sk::PublishKind(f - 1, sk::Kind::Caw);
    const rdr_own::OwnedPassStamp cst = DecideWith(f, reg & ~caw_drop & ~(1u << si::kInCasterEntries));
    CHECK("mask.caw", !IsOurs(cst) && cst.why_not == uint8_t(rdr_own::WhyNot::InputsMissing));
    // a match frame (nothing published for it any more) missing the CAW drops still walks
    f += 4;
    CHECK("decide.match_unchanged", !IsOurs(DecideWith(f, reg & ~caw_drop)));
  }

  // ---- the render side's freshness check (live.cpp SceneOurs asks scene_kind::StaleBits with the Scene3D stamp's pad byte)
  {
    const uint32_t caw_drop = all & ~caw_want;
    const uint32_t ok_caw = reg & ~caw_drop;   // a CAW record stamps only what the editor has
    CHECK("freshness.kind", sk::StaleBits(0, reg, reg) == 0);                                   // match, all fresh
    CHECK("freshness.kind", sk::StaleBits(0, reg, ok_caw) == caw_drop);                         // match needs every input: the CAW-only stamps are stale
    CHECK("freshness.kind", sk::StaleBits(uint8_t(sk::Kind::Caw), reg, ok_caw) == 0);           // a CAW record is fresh with only its own parts
    CHECK("freshness.kind", sk::StaleBits(uint8_t(sk::Kind::Caw), reg, ok_caw & ~(1u << si::kInCasterEntries)) == (1u << si::kInCasterEntries));
    CHECK("freshness.kind", sk::StaleBits(uint8_t(sk::Kind::Entrance), reg, ok_caw) == caw_drop);
    CHECK("freshness.kind", sk::StaleBits(200, reg, ok_caw) == caw_drop);                       // an unknown kind byte = the strictest (match) mask
    // the copy bug the L2dF review found: a stamp copied WITHOUT its pad reads as match, so a CAW record is called stale (here: the CAW record's own bits are NOT enough under kind 0)
    CHECK("freshness.kind", sk::StaleBits(0, reg, ok_caw) != sk::StaleBits(uint8_t(sk::Kind::Caw), reg, ok_caw));
    sk::g_caw_enabled.store(false);
    CHECK("freshness.kind", sk::StaleBits(uint8_t(sk::Kind::Caw), reg, ok_caw) == caw_drop);    // the CAW drop off = match's mask
    sk::g_caw_enabled.store(true);
  }

  // ---- flat frames in the integration (live.cpp SceneOurs: IsFlatFrame -> FlatVerdict -> Apply2DAllowed)
  {
    sk::g_flat.store(true);
    sk::FlatFacts np = WholeFlat();
    np.particles = sk::Parts::NotRecorded;   // the menu backdrop's particles are not recorded yet: today's live value
    CHECK("flat.no_apply_unless_ours", sk::IsFlatFrame(np) && sk::FlatVerdict(np) == sk::FlatWhy::ParticlesNotRecorded && !sk::Apply2DAllowed(sk::FlatVerdict(np)));
    CHECK("flat.no_apply_unless_ours", sk::Apply2DAllowed(sk::FlatVerdict(WholeFlat())));
    sk::g_flat.store(false);
    CHECK("flat.no_apply_unless_ours", !sk::Apply2DAllowed(sk::FlatVerdict(WholeFlat())));   // switch off: whole everything, still not applied
    sk::g_flat.store(true);
    for (const sk::TwoD t : {sk::TwoD::NotTaken, sk::TwoD::Holes, sk::TwoD::NoOrigins, sk::TwoD::CountDiffers, sk::TwoD::ScanRefused}) {
      sk::FlatFacts f = WholeFlat();
      f.twod = t;
      CHECK("flat.no_apply_unless_ours", !sk::Apply2DAllowed(sk::FlatVerdict(f)));
    }
    sk::FlatFacts scan = WholeFlat();
    scan.twod = sk::TwoD::ScanRefused;
    CHECK("flat.reasons", sk::FlatVerdict(scan) == sk::FlatWhy::TwoDScanRefused && std::string(sk::FlatWhyName(sk::FlatWhy::TwoDScanRefused)) == "2d_scan_refused");
    CHECK("flat.no_apply_unless_ours", !sk::Apply2DAllowed(sk::FlatWhy::NotFlatStage));   // a 3D frame never takes the flat path
    sk::g_flat.store(false);
  }

  // ---- S103 WB: the scene-walk bit (PM_post_match_menu.md 5)
  {
    // The planted FlatFacts of the post-match menu: the record keeps a stage and 251 arena objects, no model-class hook asked (walked = false), the 2D rows are whole.
    auto menu = [](bool walked, bool stamp_not_ready) {
      sk::FlatFacts f = WholeFlat();
      f.has_stage = true, f.objects3d = 251;
      f.scene3d_stamp = stamp_not_ready ? sk::StampMakes3D(uint8_t(rdr_own::Decision::GameWalks), uint8_t(rdr_own::WhyNot::NotReady)) : false;   // a NOT READY stamp is no 3D walk by itself
      sk::ApplyWalk(walked, f);
      return f;
    };
    CHECK("walk.sticky_stage_flat", sk::IsFlatFrame(menu(false, false)));
    CHECK("walk.sticky_stage_flat", sk::FlatVerdict(menu(false, false)) != sk::FlatWhy::NotFlatStage && sk::FlatVerdict(menu(false, false)) != sk::FlatWhy::NotFlatObjects);
    sk::g_flat.store(true);
    CHECK("walk.sticky_stage_flat", sk::FlatVerdict(menu(false, false)) == sk::FlatWhy::Ours);   // it reaches the 2D checks and, whole, is OURS
    sk::g_flat.store(false);
    CHECK("walk.sticky_stage_flat", sk::FlatVerdict(menu(false, false)) == sk::FlatWhy::FlagOff);
    // T1: the same record with a walk stays a stage frame (a match frame never goes flat).
    CHECK("walk.T1", sk::FlatVerdict(menu(true, false)) == sk::FlatWhy::NotFlatStage && !sk::IsFlatFrame(menu(true, false)));
    // T2: a walk whose stamp is NOT READY (a missing asset) stays not flat, even if the record shows no stage at all.
    CHECK("walk.T2", sk::FlatVerdict(menu(true, true)) == sk::FlatWhy::NotFlatStage);
    {
      sk::FlatFacts g = WholeFlat();   // no stage, no objects, a walk with a NOT READY stamp (stageless load that the game is walking)
      sk::ApplyWalk(true, g);
      CHECK("walk.T2", sk::FlatVerdict(g) == sk::FlatWhy::NotFlatHook);
    }
    // T3 is plant 6 (the old rule): l2d_check.py must see walk.sticky_stage_flat fail.
    // The pad byte: bit 7 is the walk bit and never changes the kind (every reader masks).
    CHECK("walk.kind_mask", sk::KindOfPad(uint8_t(sk::Kind::Caw) | sk::kWalkBit) == uint8_t(sk::Kind::Caw) && sk::WalkOfPad(uint8_t(sk::kWalkBit)) && !sk::WalkOfPad(uint8_t(sk::Kind::Flat)));
    CHECK("walk.kind_mask", sk::kWalkBit > sk::kKindMask && (sk::kWalkBit & sk::kKindMask) == 0 && int(sk::Kind::Count) <= 8);
    // The walk-hook list: exactly Model, Crowd, RingColour. ModelRecord (runs in match-setup menus), Particles (every menu) and Post are NOT in it.
    int n = 0;
    for (int i = 0; i < ::scene_skip::kHookCount; ++i) n += ::scene_skip::IsWalkHook(::scene_skip::Hook(i)) ? 1 : 0;
    CHECK("walk.hooks", n == 3 && ::scene_skip::IsWalkHook(::scene_skip::Hook::Model) && ::scene_skip::IsWalkHook(::scene_skip::Hook::Crowd) && ::scene_skip::IsWalkHook(::scene_skip::Hook::RingColour));
    CHECK("walk.hooks", !::scene_skip::IsWalkHook(::scene_skip::Hook::ModelRecord) && !::scene_skip::IsWalkHook(::scene_skip::Hook::Particles) && !::scene_skip::IsWalkHook(::scene_skip::Hook::Post));
    // The board: OrPad sets the bit on the stamp of that frame only, never makes one, and the stamp's decision is unchanged.
    {
      rdr_own::g_board.OrPad(rdr_own::Pass::Scene3D, 9000, sk::kWalkBit);   // no stamp yet: nothing happens
      CHECK("walk.board", rdr_own::g_board.Take(9000).empty());
      rdr_own::OwnedPassStamp s;
      s.pass = uint8_t(rdr_own::Pass::Scene3D), s.guest_frame = 9001, s.pad = uint8_t(sk::Kind::Entrance);
      rdr_own::g_board.Put(s);
      rdr_own::g_board.OrPad(rdr_own::Pass::Scene3D, 9001, sk::kWalkBit);
      rdr_own::g_board.OrPad(rdr_own::Pass::Scene3D, 9002, sk::kWalkBit);
      const auto t = rdr_own::g_board.Take(9001);
      CHECK("walk.board", t.size() == 1 && sk::WalkOfPad(t[0].pad) && sk::KindOfPad(t[0].pad) == uint8_t(sk::Kind::Entrance) && rdr_own::g_board.Take(9002).empty());
    }
    // The log line: change-driven; asks going to 0 print once; a steady frame prints nothing; the sequence counts as a change.
    {
      ::scene_skip::walk_log::State st;
      std::string line;
      namespace wl = ::scene_skip::walk_log;
      for (uint64_t f = 100; f < 104; ++f) wl::Note(f, ::scene_skip::Hook::Model), wl::Note(f, ::scene_skip::Hook::Crowd), wl::Note(f, ::scene_skip::Hook::RingColour);
      CHECK("walk.log", wl::StepLog(st, 102, 9, line) && Has(line, "[scene][walk] frame 100 seq 9 model_asks 1 crowd_asks 1 ring_asks 1"));
      CHECK("walk.log", !wl::StepLog(st, 103, 9, line));   // same state: no line
      CHECK("walk.log", !wl::StepLog(st, 104, 9, line) && !wl::StepLog(st, 105, 9, line));   // frames 102, 103: still walking
      CHECK("walk.log", wl::StepLog(st, 106, 9, line) && Has(line, "frame 104 seq 9 model_asks 0 crowd_asks 0 ring_asks 0"));   // frame 104 had no ask: the walk stopped (one line)
      CHECK("walk.log", !wl::StepLog(st, 107, 9, line));
      CHECK("walk.log", wl::StepLog(st, 108, 75, line) && Has(line, "frame 106 seq 75"));   // the sequence changed
    }
  }

  // ---- S103 CW1: the CAW classifier (sequence id 76 = "Create a Superstar Ex Menu Sequence") through the hysteresis, over the REAL DecideNow.
  {
    sk::g_caw_enabled.store(true);
    const uint32_t drop = si::kCawDrop, ok_caw = reg & ~drop;
    sk::Classifier c;
    CHECK("caw.classifier", sk::RawKindOfSequence(76) == sk::Kind::Caw && sk::RawKindOfSequence(9) == sk::Kind::Match && sk::RawKindOfSequence(0) == sk::Kind::Match && sk::RawKindOfSequence(75) == sk::Kind::Match);
    // enter: 76 twice is not yet Caw (the kind stays Match: a Caw-mask frame walks); the third consecutive 76 applies and publishes. Every decision below is on its OWN frame (DecideNow latches per frame).
    CHECK("caw.hysteresis", c.Step(9000, 76) == sk::Kind::Match && c.Step(9001, 76) == sk::Kind::Match);
    CHECK("caw.hysteresis", sk::KindFor(9002) == sk::Kind::Match && !IsOurs(DecideWith(9002, ok_caw)));   // two frames in: still match's mask
    CHECK("caw.classifier", c.Step(9002, 76) == sk::Kind::Caw && c.enters == 1);
    CHECK("caw.classifier", sk::KindFor(9002) == sk::Kind::Caw && sk::KindFor(9003) == sk::Kind::Caw);   // published for 9002, taken for 9002 and 9003 (the lag)
    const rdr_own::OwnedPassStamp ours = DecideWith(9003, ok_caw);
    CHECK("caw.classifier", IsOurs(ours) && ours.pad == uint8_t(sk::Kind::Caw));   // sequence 76 + the editor's stamps (no crowd / ring / plane) = READY with the Caw drop mask
    CHECK("caw.line", Has(sk::CawEnterLine(9002, 76, si::kCawDrop, c.enters), "[scene][kind] caw enter #1 at frame 9002 (sequence 76, drop mask 0x"));
    CHECK("caw.classifier", c.Step(9004, 76) == sk::Kind::Caw && c.enters == 1);   // staying in Caw publishes again, counts no second entry
    CHECK("caw.classifier", !IsOurs(DecideWith(9005, ok_caw & ~(1u << si::kInStagePools))));   // a fed input is still required: pools missing walks
    // a stray frame (one non-76) leaves at once and the next 76 starts the count again
    // (review fix 2) leaving is NOT one frame late: Step(F = 9004, Caw applied) then Step(F + 1, 9): KindFor(F + 1) is Match at once (the lag rule would take the F publish for F + 1 without the clear)
    CHECK("caw.leave", c.Step(9005, 9) == sk::Kind::Match && c.leaves == 1);
    CHECK("caw.leave", sk::KindFor(9005) == sk::Kind::Match && sk::KindFor(9006) == sk::Kind::Match);
    CHECK("caw.line", Has(sk::CawLeaveLine(9005, 9, c.leaves), "[scene][kind] caw leave #1 at frame 9005 (sequence 9)"));
    CHECK("caw.hysteresis", sk::KindFor(9008) == sk::Kind::Match);                                    // nothing published for 9008: leaving needs no publish (the old Caw publish has aged out)
    CHECK("caw.hysteresis", !IsOurs(DecideWith(9008, ok_caw)));                                          // a sequence other than 76 = Match = every input required
    CHECK("caw.hysteresis", c.Step(9007, 76) == sk::Kind::Match);                                        // the count restarts
    // a Match frame with everything stamped is untouched by the classifier
    CHECK("caw.classifier", IsOurs(DecideWith(9030, reg)) && sk::KindFor(9030) == sk::Kind::Match);
    // the stream build's default (g_caw_enabled off): a published Caw frame needs every input
    sk::g_caw_enabled.store(false);
    sk::PublishKind(9049, sk::Kind::Caw);
    CHECK("caw.classifier", !IsOurs(DecideWith(9050, ok_caw)) && IsOurs(DecideWith(9060, reg)));
    sk::g_caw_enabled.store(true);
    // 76 with NO 3D model stamps goes flat (a CAS menu page with no body): the flat verdict is kind-blind; the sticky stage / objects of an earlier scene do not make it 3D (ApplyWalk: no model hook asked)
    sk::g_flat.store(true);
    sk::FlatFacts menu = WholeFlat();
    menu.has_stage = true, menu.objects3d = 251;
    sk::ApplyWalk(false, menu);
    CHECK("caw.flat", sk::IsFlatFrame(menu) && sk::FlatVerdict(menu) == sk::FlatWhy::Ours && sk::Apply2DAllowed(sk::FlatVerdict(menu)));
    // with a model hook asking (the body on screen) the same record is a 3D frame and never flat, whatever the kind
    sk::FlatFacts body = WholeFlat();
    body.has_stage = true, body.objects3d = 251;
    sk::ApplyWalk(true, body);
    CHECK("caw.flat", !sk::IsFlatFrame(body) && sk::FlatVerdict(body) == sk::FlatWhy::NotFlatStage);
    sk::g_flat.store(false);
  }

  // ---- the hysteresis helper
  {
    sk::Hysteresis h;
    CHECK("kind.hysteresis", h.Step(sk::Kind::Caw) == sk::Kind::Match);
    CHECK("kind.hysteresis", h.Step(sk::Kind::Caw) == sk::Kind::Match);
    CHECK("kind.hysteresis", h.Step(sk::Kind::Caw) == sk::Kind::Caw);          // the third consecutive publish applies
    CHECK("kind.hysteresis", h.Step(sk::Kind::Match) == sk::Kind::Match);      // Match / Entrance at once
    CHECK("kind.hysteresis", h.Step(sk::Kind::Caw) == sk::Kind::Match && h.Step(sk::Kind::Flat) == sk::Kind::Match && h.Step(sk::Kind::Caw) == sk::Kind::Match);   // an interruption restarts the count
    CHECK("kind.hysteresis", h.Step(sk::Kind::Entrance) == sk::Kind::Entrance);
  }

  // ---- the lag
  {
    sk::PublishKind(500, sk::Kind::Caw);
    CHECK("kind.lag", sk::KindFor(500) == sk::Kind::Caw);
    CHECK("kind.lag", sk::KindFor(501) == sk::Kind::Caw);
    CHECK("kind.lag", sk::KindFor(502) == sk::Kind::Match);
    CHECK("kind.lag", sk::KindFor(499) == sk::Kind::Match);
    sk::PublishKind(0, sk::Kind::Flat);
    CHECK("kind.lag", sk::KindFor(0) == sk::Kind::Flat && sk::KindFor(1) == sk::Kind::Flat);
  }

  // ---- the census line
  {
    (void)sk::CensusLine(true);   // drain what DecideNow counted above
    sk::NoteFlat(sk::FlatWhy::Ours);
    sk::NoteFlat(sk::FlatWhy::TwoDHoles);
    sk::NoteFlat(sk::FlatWhy::TwoDHoles);
    sk::NoteFlat(sk::FlatWhy::FlagOff);
    sk::NoteFlat(sk::FlatWhy::NotFlatStage);   // a 3D frame: not counted under flat
    sk::NoteGame(sk::Kind::Match, uint8_t(rdr_own::WhyNot::InputsMissing), (1u << 10) | (1u << 12));
    sk::NoteGame(sk::Kind::Match, uint8_t(rdr_own::WhyNot::None), 0);
    sk::NoteGame(sk::Kind::Caw, uint8_t(rdr_own::WhyNot::Hold), 0);
    const std::string l = sk::CensusLine(true);
    std::printf("%s\n", l.c_str());
    CHECK("census.line", Has(l, "match ours 1/2 (50.0%) first: bit 10 1"));
    CHECK("census.line", Has(l, "entrance -"));
    CHECK("census.line", Has(l, "flat ours 1/4 (25.0%) first: 2d_holes 2, flag_off 1"));
    CHECK("census.line", Has(l, "caw ours 0/1 (0.0%) first: hold 1"));
    CHECK("census.line", Has(sk::CensusLine(true), "match -; entrance -; flat -; caw -"));   // drained
  }

  if (g_fail) std::printf("l2d_unit: %d FAILED\n", g_fail);
  else std::printf("l2d_unit: all checks passed\n");
  return g_fail ? 1 : 0;
}
