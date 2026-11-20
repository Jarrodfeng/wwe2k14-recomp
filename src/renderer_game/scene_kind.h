// Every frame kind gets an owner (Session 103 lane L2d, landing 2; docs\renderer\LANDING2_PLAN.md 5, claims out\s103\decomp\claims_Q78.jsonl Q8).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure, header-only: no game, SDK, REX or renderer includes (scene_frame.h includes it; tools\scene\l2d_unit.cpp builds it).
//
// Two things live here:
//  1. SCENE KINDS and their REQUIRED-INPUT masks (match / entrance / flat / CAW). The decision (scene_frame.h DecideNow) asks the inputs of the frame's kind, not one
//     all-inputs mask: required(kind) = registered & ~drop(kind). Match and entrance drop NOTHING (their mask is today's, by construction: SetKindDrop refuses them; the
//     unit's twin plants a match drop and must be caught). CAW drops the inputs the editor has no part for (scene_inputs::kCawDrop, inputs_core.h). Flat drops nothing ON THE
//     GAME SIDE: a flat frame has no 3D walk, so no scene hook asks for it; a hook that does ask on a frame published Flat proves the frame is not flat and gets the strictest
//     mask (it walks unless every input is stamped). The flat frame's own owner is item 2.
//     The kind of hook frame j is PUBLISHED by whoever classifies it (PublishKind; lag 1 like the latch inputs: Decide(j) accepts a kind published for j or j - 1). Nothing
//     published (or a stale one) = Match = today's mask: unwired, this file changes no decision.
//  2. The FLAT frame (menus, select, title, boot, load: Q8 [C] "no model submit; the frame = the 2D list + the movie YUV quad + the backdrop particles into image 8"):
//     FlatVerdict decides it on the RENDER side from the record and the 2D layer's own census, because there is no game-side walk to skip. A frame is flat when the record
//     holds no stage (live_scene_draw.cpp HasStage), no 3D object and no Scene3D stamp (no scene hook ran). It is OURS when its 2D rows are whole (the 2D drawer's own
//     check: taken, no hole, origins, row count == the stream's, no refused row), its movie rows have their planes, and its particles are whole - each by its own existing
//     census - AND the dev switch --wwe2k14_rdr_scene_flat is on (default OFF, +dev builds only: g_flat). Exempt from HasStage and from every 3D input.
//     COMPILE_OUT_LIST.md 4 R2: "a frame without a stage is a 2D frame".
//  3. The CENSUS: per scene kind, frames decided, OURS share and the first reasons (game side: WhyNot / the lowest missing input bit; flat: FlatWhy). Its own line
//     `[scene][kind] ...` until L2b2's fallback census merges (then this feeds it).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "node_switch.h"

namespace scene_kind {

// Append only (the census and the unit index by it).
enum class Kind : uint8_t { Match = 0, Entrance = 1, Flat = 2, Caw = 3, Count };
inline constexpr int kKinds = int(Kind::Count);
inline const char* KindName(Kind k) {
  static const char* const n[] = {"match", "entrance", "flat", "caw"};
  return int(k) < kKinds ? n[int(k)] : "?";
}

#ifdef RDR_L2D_PLANTS
// Test only (tools\scene\l2d_unit.cpp): 1 = SetKindDrop accepts a match drop (a match input silently not required); 2 = the flat verdict ignores the 2D holes;
// 3 = the flat verdict ignores the switch; 4 = KindFor ignores the lag (a kind published for j - 1 is not taken: every frame reads Match); 5 = Apply2DAllowed says yes to a non-Ours flat frame; 6 = ApplyWalk trusts the record without a walk (S103 WB, the old rule); 7 = the CAW classifier is unwired (Classifier::Step never publishes: S103 CW1); 8 = leaving Caw does not clear the publish (one frame late); 9 = the classifier's hysteresis is N = 1; 10 = an OURS frame with no walk bit stays OURS (S105 A4, the old rule); 11 = a demoted frame keeps its Scene3D stamp as 3D evidence (S105 A4 review F1)
inline int g_plant = 0;
#define L2D_PLANT(n) (::scene_kind::g_plant == (n))
#else
#define L2D_PLANT(n) false
#endif

// ---- 1. per-kind required masks
inline std::atomic<uint32_t> g_kind_drop[kKinds] = {};   // bits the kind does not need (0 = every registered input)
// Match and entrance must stay today's mask: a drop for them is refused (returns false).
inline bool SetKindDrop(Kind k, uint32_t drop) {
  if (int(k) >= kKinds) return false;
  if ((k == Kind::Match || k == Kind::Entrance || k == Kind::Flat) && !L2D_PLANT(1)) return drop == 0;   // flat: see the header (a hook on a flat frame gets the strictest mask)
  g_kind_drop[int(k)].store(drop, std::memory_order_relaxed);
  return true;
}
// The CAW drop (S103 CW1): ON in the stream-free build (start_settings.cpp), where the Classifier below publishes Caw while the game's sequence id is 76; in the stream build it stays `dev && --wwe2k14_rdr_scene_caw`
// (nothing publishes Caw there). The drop list was audited against the game's code (inputs_core.h kCawDrop). Tests set it directly.
inline std::atomic<bool> g_caw_enabled{false};
inline uint32_t KindDrop(Kind k) {
  if (int(k) >= kKinds || (k == Kind::Caw && !g_caw_enabled.load(std::memory_order_relaxed))) return 0u;
  return g_kind_drop[int(k)].load(std::memory_order_relaxed);
}
inline uint32_t RequiredFor(Kind k, uint32_t registered) { return registered & ~KindDrop(k); }
// The render side's freshness check (live.cpp SceneOurs): which required inputs of the record's kind (the Scene3D stamp's pad byte) are not stamped fresh. `ok` = the bits the record's own stamps carry for its hook frame or later.
inline uint32_t StaleBits(uint8_t kind_byte, uint32_t registered, uint32_t ok) { return RequiredFor(Kind(kind_byte), registered) & ~ok; }

// The kind of a hook frame, published by the classifier (frame << 3 | kind). 0 = nothing published.
inline std::atomic<uint64_t> g_published{0};
inline void PublishKind(uint64_t hook_frame, Kind k) { g_published.store(((hook_frame + 1) << 3) | uint64_t(uint8_t(k) & 7u), std::memory_order_relaxed); }
// Decide(j) takes a kind published for j or j - 1 (latch lag 1); anything else (none, stale, a future frame) = Match, today's mask.
// S103 NR: the scene phase word 0x836F3940 == 4 inside the in-game sequence 9 (an entrance is running; S103 PHS: 4 alone is also the editor and the menu sequences), set once per presented frame by step_rate.h OnFrameInner (the same read as g_entrances). It is the kind of a frame
// NOBODY published (nothing publishes today): runs 34 / 35 counted every entrance frame as `match`. Entrance and match require the SAME inputs (SetKindDrop refuses a drop for both), so this
// changes the census and the stamp's pad byte, never a decision.
inline std::atomic<bool> g_phase_entrance{false};
inline Kind KindFor(uint64_t frame) {
  const Kind unpublished = g_phase_entrance.load(std::memory_order_relaxed) ? Kind::Entrance : Kind::Match;
  const uint64_t w = g_published.load(std::memory_order_relaxed);
  if (!w) return unpublished;
  const uint64_t f = (w >> 3) - 1;   // stored + 1 so frame 0 is publishable
  const uint8_t k = uint8_t(w & 7u);
  if (k >= kKinds) return unpublished;
  if (f == frame || (!L2D_PLANT(4) && frame >= 1 && f == frame - 1)) return Kind(k);
  return unpublished;
}

// The hysteresis every PublishKind caller passes its raw per-frame guess through, so one stray frame cannot drop a mask. A kind that DROPS inputs (Caw, and any later one) applies only after kHysteresisN
// consecutive same-kind publishes; Match and Entrance apply at once (they drop nothing: leaving a dropping kind is always safe).
inline constexpr uint32_t kHysteresisN = 3;
struct Hysteresis {
  Kind applied = Kind::Match;
  Kind pending = Kind::Match;
  uint32_t run = 0;
  Kind Step(Kind raw, uint32_t n = kHysteresisN) {
    if (raw == Kind::Match || raw == Kind::Entrance) return applied = raw, pending = raw, run = 0, applied;
    if (raw == pending) ++run;
    else pending = raw, run = 1;
    if (run >= n) applied = raw;
    return applied;
  }
};

// S103 CW1 (docs\research\s103\PCW_caw_wrestler.md C1, PCW_adversary.md 3 / 4): the CLASSIFIER. The CAW scene is the game's sequence id 76, "Create a Superstar Ex Menu Sequence" (sub_833979B8 builds it with
// r4 = 76; the id is [[[0x839400F8]] + 48]: load_probe.h CurrentSequence and step_rate.h OnFrameInner read that chain) [C]. 76 is the WHOLE CAS menu sequence, not "a body is on screen" [L]: the kind only chooses
// the required-input mask. Whether a frame is a picture of ours stays the hooks' business: a Model-class hook asking = a 3D walk (kWalkBit); none asking = a flat menu frame (FlatVerdict is kind-blind: a Caw kind
// never makes a frame 3D and never exempts it from the 2D / movie / particle checks). Every other sequence (and an unreadable one, 0) is Match; an entrance is the phase word's (KindFor).
inline constexpr uint32_t kCawSequence = 76;
inline Kind RawKindOfSequence(uint32_t seq) { return seq == kCawSequence ? Kind::Caw : Kind::Match; }
struct Classifier {
  Hysteresis h;
  uint64_t enters = 0;   // transitions into an applied Caw (the log prints a line per change)
  uint64_t leaves = 0;
  // `hook_frame`: the frame the game's clock reads when the sequence was read (step_rate.h OnFrameInner: the presented frame). Publishes only while Caw is applied: Match / Entrance are published by NOBODY (an
  // unpublished or stale kind reads as Match / Entrance in KindFor), so leaving Caw takes effect on the next frame with no publish at all.
  Kind Step(uint64_t hook_frame, uint32_t seq) {
    const Kind before = h.applied;
    const Kind now = h.Step(RawKindOfSequence(seq), L2D_PLANT(9) ? 1u : kHysteresisN);   // plant 9: N = 1 (one 76 frame applies Caw)
    if (now == Kind::Caw && before != Kind::Caw) ++enters;
    if (now != Kind::Caw && before == Kind::Caw) {
      ++leaves;
      // Leaving is immediate: KindFor accepts a kind published for frame - 1 (the latch lag), so the last Caw publish (frame G - 1) would still answer decision G. Clear it.
      if (!L2D_PLANT(8)) g_published.store(0, std::memory_order_relaxed);   // plant 8: the publish is left to age out (one frame late)
    }
    if (now == Kind::Caw && !L2D_PLANT(7)) PublishKind(hook_frame, Kind::Caw);
    return now;
  }
};
// The line logged on every entry into Caw (change-driven): the census's own prefix, so `[scene][kind] caw` finds both. `drop` = the mask the decision then drops (KindDrop(Caw): 0 when g_caw_enabled is off).
inline std::string CawLeaveLine(uint64_t frame, uint32_t seq, uint64_t leaves) {
  char b[128];
  std::snprintf(b, sizeof b, "[scene][kind] caw leave #%llu at frame %llu (sequence %u)", (unsigned long long)leaves, (unsigned long long)frame, seq);
  return b;
}
inline std::string CawEnterLine(uint64_t frame, uint32_t seq, uint32_t drop, uint64_t enters) {
  char b[160];
  std::snprintf(b, sizeof b, "[scene][kind] caw enter #%llu at frame %llu (sequence %u, drop mask 0x%x)", (unsigned long long)enters, (unsigned long long)frame, seq, drop);
  return b;
}

// ---- 2. the flat frame
// --wwe2k14_rdr_scene_flat (live.cpp sets it at start: +dev builds only; tests set it directly). Default false: no flat frame is ever OURS.
inline std::atomic<bool> g_flat{false};

// The 2D layer's verdict for the frame, as the 2D drawer's own check gives it (live.cpp Draw2DOurs, LANDING2_PLAN P1: the same order).
enum class TwoD : uint8_t { Whole = 0, NotTaken, Holes, NoOrigins, CountDiffers, Refused, ScanRefused };   // ScanRefused: the stream scan (q2d::ScanStream) refused a target / interleaved / blend pairing (L2dF)
// The particles' verdict (presence_rules.h ParticleVerdict for the scene node; the menu backdrop emitters' own verdict when they are recorded). NotRecorded = no verdict
// for this frame: the frame shows particles we have no rows for, so it is NOT whole (fail closed).
enum class Parts : uint8_t { NotRecorded = 0, Whole, Held };
// S103 MP: the flat frame's particle verdict from the record's particle tables (pure: the live.cpp call passes the table facts). `empty_marker` = the status row carries kPiEmptyPass (the node did not draw: no call in the
// frame, or +3084 / +3088 == 0 / null camera), `batches` = batches in the record, `reasons` = the status row's reasons (0 = no row), `screen` = a screen-camera row is in the record.
// Whole: the marker, no batch, and no reason other than the marker (an EMPTY pass is whole). Held: batches that the node drew with its screen camera (recorded, but the flat drawer for them is not built, and blend code 0 /
// texture naming are still counted reasons). Anything else (no verdict row, a batch without a screen camera, a reason with no batch) stays NotRecorded: fail closed.
inline Parts PartsOf(bool empty_marker, uint32_t batches, uint32_t reasons, bool screen) {
  constexpr uint32_t kInfo = 8u | 256u;   // rdr_scene::kPiInfoMask = kPiPivot | kPiEmptyPass (particle_tables.h; the unit and the live.cpp call site assert the equality)
  if (empty_marker && !batches && !screen && (reasons & ~kInfo) == 0) return Parts::Whole;
  if (screen) return Parts::Held;   // (a screen-camera row with no batch: the node ran but nothing of a known class was read: still not a whole drawn pass)
  return Parts::NotRecorded;
}
struct FlatFacts {
  // the 3D half (LiveSceneDraw::FlatFacts3D fills it from the record it last built)
  bool has_stage = true;          // live_scene_draw.cpp HasStage (an arena-family object in the record)
  uint32_t objects3d = 1;         // 3D objects in the record (any family) - a flat frame has none
  bool scene3d_stamp = true;      // a Scene3D decision is in the record (a scene hook ran: the frame has a 3D walk)
  // the flat half (the 2D drawer's census of this frame)
  TwoD twod = TwoD::NotTaken;
  uint32_t refused_other = 0;     // refused 2D rows other than a movie row's missing planes (twod == Refused only when > 0)
  uint32_t movie_rows = 0;        // movie YUV rows in the frame's 2D list
  uint32_t movie_planes_missing = 0;   // of those, rows refused for their planes (the P4 plane resolver)
  Parts particles = Parts::NotRecorded;
};
// The FIRST failing reason, in this order. NotFlat* = the frame is a 3D frame (decided game side: no flat census). Ours = draw the flat frame from our rows.
enum class FlatWhy : uint8_t {
  Ours = 0, NotFlatStage, NotFlatObjects, NotFlatHook,
  TwoDNotTaken, TwoDHoles, TwoDNoOrigins, TwoDCountDiffers, TwoDScanRefused, TwoDRefused, MoviePlanes, ParticlesNotRecorded, ParticlesHeld,
  FlagOff,   // everything whole: the frame WOULD be ours with --wwe2k14_rdr_scene_flat (last, so a run with the switch off still names what blocks each frame)
  Count
};
inline const char* FlatWhyName(FlatWhy w) {
  static const char* const n[] = {"ours", "not_flat_stage", "not_flat_objects", "not_flat_hook", "2d_not_taken", "2d_holes", "2d_no_origins", "2d_count_differs", "2d_scan_refused", "2d_refused",
                                  "movie_planes", "particles_not_recorded", "particles_held", "flag_off"};
  return uint8_t(w) < uint8_t(FlatWhy::Count) ? n[uint8_t(w)] : "?";
}
// The ONE gate for leaving the stream's 2D out of a flat frame (live.cpp SceneOurs -> Apply2D): only an Ours verdict. Any other flat frame keeps its UI from the stream (a menu whose 2D is skipped but not drawn would lose it).
inline bool Apply2DAllowed(FlatWhy w) {
#ifdef RDR_L2D_PLANTS
  if (g_plant == 5 && w != FlatWhy::NotFlatStage && w != FlatWhy::NotFlatObjects && w != FlatWhy::NotFlatHook) return true;   // twin: Apply2D on a non-Ours flat frame
#endif
  return w == FlatWhy::Ours;
}
// S103 FLAT F1 (stream-free): does ONE Scene3D stamp make its frame a 3D frame? Game sequence 9 (CGameMainSequence) holds the in-game load and other non-3D pages, and post / plane / prelight / shadow / rim
// run there too, so "a scene hook asked" is no evidence of a 3D walk. A stamp counts only when its decision is OURS, or when it was refused for a reason other than NOT READY. NOT READY is the stageless case
// (READY needs a stage in the record: live_scene_draw.cpp kTermNoStage; a stage with a missing asset / hole is not flat anyway by has_stage / objects3d, and a record the drawer did not build is never flat:
// FlatFacts3D). `plant_old` = the old rule (any stamp).
inline bool StampMakes3D(uint8_t decision, uint8_t why_not, bool plant_old = false) {
  if (plant_old) return true;
  return decision == uint8_t(rdr_own::Decision::Ours) || why_not != uint8_t(rdr_own::WhyNot::NotReady);
}
// S103 WB (docs\research\s103\PM_post_match_menu.md 3, 4): the SCENE-WALK bit. The Scene3D stamp's pad byte holds the kind in bits 0-2 (PublishKind) and, in the stream-free build, bit 7 = "a model-class scene hook
// (Model / Crowd / Ring, scene_skip_core.h kWalkHook) asked in this hook frame": the 3D scene is on screen exactly when those walks run. The contributors keep recording what is loaded (a sticky arena after a
// match); only the flat verdict stops trusting them when no walk ran. Every reader of the kind masks with KindOfPad.
inline constexpr uint8_t kWalkBit = 0x80u;
inline constexpr uint8_t kKindMask = 0x07u;
inline uint8_t KindOfPad(uint8_t pad) { return uint8_t(pad & kKindMask); }
inline bool WalkOfPad(uint8_t pad) { return (pad & kWalkBit) != 0; }
// S105 A4 (F1 / F2 of review_A4.md): an OURS frame (stream-free: READY alone) whose Scene3D stamp had NO walk bit for kRecords consecutive records is not ours: the record's stage is a leftover (a sticky arena after Create
// a Finisher / a match; the menu case lasts hundreds of frames, one record missing its bit mid-match must not go black). Such a frame is DEMOTED: it takes the host's not-ours branch and its Scene3D stamp is dropped
// as 3D evidence (DropStampWhenDemoted), so FlatFacts3D / ApplyWalk see a stage-less, stamp-less frame and IsFlatFrame says flat. `force_ours` = the rig's seam (never demoted, resets the run).
// `k` is a parameter only for the unit's old-rule twin (k = 1 = the first build of this piece); L2D_PLANT(10) = the older rule still (never demoted).
struct WalkGate {
  static constexpr uint32_t kRecords = 3;
  uint32_t run = 0;   // consecutive records with no walk bit
  bool Demote(bool walked, bool force_ours, uint32_t k = kRecords) {
    if (walked || force_ours) return run = 0, false;
    if (run < 0xFFFFu) ++run;
    return !L2D_PLANT(10) && run >= k;
  }
};
// A demoted frame's Scene3D stamp is no evidence of a 3D walk (it is an OURS stamp from READY alone). `plant_old` = the first build of this piece (the stamp stays: the frame is "the game walked", Hold3D / black_no_3d).
inline void DropStampWhenDemoted(FlatFacts& f, bool demoted, bool plant_old = false) {
  if (demoted && !plant_old && !L2D_PLANT(11)) f.scene3d_stamp = false;
}
// The ONE step both FlatFacts3D and the unit run: no walk = the record's stage and 3D objects are leftovers (a sticky snapshot), not a picture. `plant_old` = the old rule (the record is trusted).
// A walk with a missing asset (a NOT READY stamp) is still a walk: it also counts as a Scene3D stamp, so it is never flat.
inline void ApplyWalk(bool walked, FlatFacts& f, bool plant_old = false) {
  if (walked) { f.scene3d_stamp = true; return; }
  if (plant_old || L2D_PLANT(6)) return;   // plant 6 (l2d_unit): the old rule
  f.has_stage = false;
  f.objects3d = 0;
}
inline bool IsFlatFrame(const FlatFacts& f) { return !f.has_stage && f.objects3d == 0 && !f.scene3d_stamp; }
inline FlatWhy FlatVerdict(const FlatFacts& f) {
  if (f.has_stage) return FlatWhy::NotFlatStage;
  if (f.objects3d) return FlatWhy::NotFlatObjects;
  if (f.scene3d_stamp) return FlatWhy::NotFlatHook;
  switch (f.twod) {
    case TwoD::Whole: break;
    case TwoD::NotTaken: return FlatWhy::TwoDNotTaken;
    case TwoD::Holes: if (!L2D_PLANT(2)) return FlatWhy::TwoDHoles; break;
    case TwoD::NoOrigins: return FlatWhy::TwoDNoOrigins;
    case TwoD::CountDiffers: return FlatWhy::TwoDCountDiffers;
    case TwoD::ScanRefused: return FlatWhy::TwoDScanRefused;
    case TwoD::Refused: if (f.refused_other) return FlatWhy::TwoDRefused; break;   // only planes refused: the movie reason below names it
    default: return FlatWhy::TwoDNotTaken;
  }
  if (f.movie_planes_missing) return FlatWhy::MoviePlanes;
  if (f.particles == Parts::NotRecorded) return FlatWhy::ParticlesNotRecorded;
  if (f.particles != Parts::Whole) return FlatWhy::ParticlesHeld;
  if (!g_flat.load(std::memory_order_relaxed) && !L2D_PLANT(3)) return FlatWhy::FlagOff;
  return FlatWhy::Ours;
}

// ---- 3. the census (lock-free; one counter per kind per reason code)
// Codes: 0 = OURS; 1-15 = a game-side WhyNot other than InputsMissing; 16 + b = InputsMissing, b = the lowest missing required bit (b < 32); 48 + FlatWhy (flat verdicts).
inline constexpr int kCodeInputs = 16, kCodeFlat = 48, kCodes = 64;
struct Census {
  std::atomic<uint64_t> n[kKinds][kCodes] = {};
};
inline Census g_census;
inline int GameCode(uint8_t why_not, uint32_t missing) {
  if (why_not == uint8_t(rdr_own::WhyNot::None)) return 0;
  if (why_not == uint8_t(rdr_own::WhyNot::InputsMissing) && missing) {
    int b = 0;
    while (!((missing >> b) & 1u)) ++b;
    return kCodeInputs + b;
  }
  return why_not < kCodeInputs ? int(why_not) : kCodeInputs - 1;
}
inline void NoteGame(Kind k, uint8_t why_not, uint32_t missing) {
  if (int(k) < kKinds) g_census.n[int(k)][GameCode(why_not, missing)].fetch_add(1, std::memory_order_relaxed);
}
// The flat verdict of a render-side frame: counted under Flat only when the frame IS flat (a 3D frame's verdict is the game side's).
inline void NoteFlat(FlatWhy w) {
  if (w == FlatWhy::NotFlatStage || w == FlatWhy::NotFlatObjects || w == FlatWhy::NotFlatHook) return;
  g_census.n[int(Kind::Flat)][w == FlatWhy::Ours ? 0 : kCodeFlat + int(w)].fetch_add(1, std::memory_order_relaxed);
}
inline std::string CodeName(int c) {
  static const char* const why[] = {"ours", "switch_off", "not_ready", "inputs_missing", "hold", "latched", "no_inputs_registered"};
  if (c == 0) return "ours";
  if (c < kCodeInputs) return c < 7 ? why[c] : "why" + std::to_string(c);
  if (c < kCodeFlat) return "bit " + std::to_string(c - kCodeInputs);
  return FlatWhyName(FlatWhy(c - kCodeFlat));
}
// "[scene][kind] match ours 540/600 (90.0%) first: bit 10 50, hold 10; entrance -; flat ours 0/300 (0.0%) first: 2d_holes 200, flag_off 100; caw -"
// Reasons by frames, most first (at most 4 per kind). Drains the window when `drain`.
inline std::string CensusLine(bool drain = true) {
  std::string s = "[scene][kind]";
  for (int k = 0; k < kKinds; ++k) {
    uint64_t v[kCodes], total = 0;
    for (int c = 0; c < kCodes; ++c) total += (v[c] = drain ? g_census.n[k][c].exchange(0, std::memory_order_relaxed) : g_census.n[k][c].load(std::memory_order_relaxed));
    s += (k ? "; " : " ");
    s += KindName(Kind(k));
    if (!total) { s += " -"; continue; }
    char b[96];
    std::snprintf(b, sizeof b, " ours %llu/%llu (%.1f%%)", (unsigned long long)v[0], (unsigned long long)total, 100.0 * double(v[0]) / double(total));
    s += b;
    bool any = false;
    for (int shown = 0; shown < 4; ++shown) {
      int best = -1;
      for (int c = 1; c < kCodes; ++c)
        if (v[c] && (best < 0 || v[c] > v[best])) best = c;
      if (best < 0) break;
      s += any ? ", " : " first: ";
      s += CodeName(best) + " " + std::to_string((unsigned long long)v[best]);
      v[best] = 0;
      any = true;
    }
  }
  return s;
}

}  // namespace scene_kind
