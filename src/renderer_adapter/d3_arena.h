// D3 "skip the object", the arena's contract header (Session 101, lane 138; docs\renderer\SKIP_GAME_WORK_MAP.md sections 2.3 / 2.4 / 8.3). No game or SDK includes: the guest hook
// (scene_live\d3_arena_hook.h), the binder (d3_binder.h), fview and tools\d3\d3_unit_test.cpp all build THIS header.
//
// What it is. For a proven arena object the game never builds its draw: the hook in sub_82C04DF0 (the existing REX_HOOK_RAW in draw_origin.h) writes ONE 0-primitive op-15 record,
// the MARKER, tagged {object, marker, generation}, and a PACKET into a host side table in the same call. The binder, meeting the marker, draws the object's parts from its
// descriptors (what a recorded-in-full frame taught it: the D1 templates plus each part's state, slots and bank), the pass bank at the marker, and the packet's values.
//   marker  = an origin Key { call_site = kMarkerCall, pass_site 0, object, part = generation } (the intern table's own fields; draw_origin.h RecordTag writes it)
//   packet  = PacketTable entry keyed (guest frame, object, ordinal); written BEFORE the skip is decided; the consumer never discards one
//   hole    = a marker whose packet is missing (counted, required 0)
//   dropped = an object the hook could have skipped and recorded in full instead (counted by reason)
// Switch: g_arena_d3 (live --wwe2k14_rdr_arena_d3; fview --d3 arena). Default off on PC and phone. It needs arena_ours + arena_thin + scene records + light states.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "thin_draw.h"
#include "../scene_live/crowd_skip_core.h"   // NowMs: ONE clock for the gate on both sides (the hook asks it, the binder reports with it)

namespace rdr_adapter::d3 {

inline bool g_arena_d3 = false;
inline bool g_ring_d3 = false;     // lane 229 (Ring D3 R3, SKIP_GAME_WORK_MAP.md 16.8): the ring class's own D3 chain (fview --d3 ring; live: --wwe2k14_rdr_ring_d3_live, verify arm by default, the skip arm since lane 289; Android unregistered)
inline bool g_caster_d3 = false;   // lane 217 (E2b, SKIP_GAME_WORK_MAP.md 17.9): the caster class's own D3 chain (fview --d3 casters). INERT live: nothing sets it in the game (Android unregistered), the reader's two caster-marker tests are behind it
inline bool g_wrestlers_d3 = false;   // lane 227 (Wrestlers D3 H2, SKIP_GAME_WORK_MAP.md 18.7): the wrestlers' own D3 chain (fview --d3 wrestlers). INERT live: nothing sets it in the game (Android unregistered); the live reader's marker tests are behind it
// The record route (light / shadow / world registers from the frame's scene records, d3_record.h; lane 152 = D3-F) is built and proven offline (fview --d3-mode record on J1, tools\d3\d3_check.py). Until it was,
// the live binder published an EMPTY mark set and the hook stayed off (lane 138 review A). The switch --wwe2k14_rdr_arena_d3 stays default OFF; its first live run is the milestone check (SKIP_GAME_WORK_MAP.md 11.6).
inline constexpr bool kRecordRouteBuilt = true;   // lane 152: flipped when the offline bars passed (d3_check.py record arms on J1, the plants, Check B / 7079; unit; syntax checks)
inline int64_t NowMs() { return wwe2k14::crowd_skip::NowMs(); }
inline bool g_verify = false;   // lane 152 (10): --wwe2k14_rdr_arena_d3_verify: the hook writes the marker but never skips; default off
inline std::atomic<bool> g_hook_on{false};   // the guest hook's one relaxed load (set with the switch once the whole chain is on)

inline constexpr uint32_t kMarkerCall = 0x0D3A0000u;   // the origin call_site of a marker record (never a guest address); the low byte = the object's keyed ordinal. The marker's origin: object = the object, part = the mark-set generation the hook read, pass_site = the hook's guest frame (the packet's key)
inline constexpr uint32_t kMarkerPass = 0x0D3A0100u;   // the origin pass_site of a marker (no model-part site): the interned key is {kMarkerCall | ordinal, kMarkerPass, object} only; the hook's frame and the mark generation ride in the record's +20 / +24 words (base vertex / start index), which the reader hands to the binder (Draw::vertex_offset / index_offset of a marker: contract in SKIP_GAME_WORK_MAP.md 10.7)
inline constexpr uint32_t kHookToStreamOffset = 1;   // lane 152 [C: J1] stream guest frame - the hook's stamp (arena_world_records.py, ledger.py --light-state); the live record pairing (live.cpp SnapshotKeeper: stream frame j goes with record j-1) says the same
inline constexpr uint32_t kProofK = thin::kProofK;     // an object takes the full path in 1 frame of K (by the object's hash)
inline constexpr uint32_t kNeedCleanFrames = 3;        // crowd_skip_core.h's gate shape: clean frames before the game's work may be dropped
inline constexpr int64_t kStaleMs = 250;               // a report older than this counts as none
inline constexpr uint32_t kMaxStates = 64;             // light_state_hook.h: a frame with more states is not skipped (counted)

inline bool IsMarkerCall(uint32_t call_site) { return (call_site & 0xFFFFFF00u) == kMarkerCall; }
// lane 229 (Ring D3 R3): a ring marker's call_site = kRingMarkerCall | (keyed ordinal & 0xFF); its origin object is the ring model, pass_site kMarkerPass. IsMarkerCall (the arena's) and IsCasterMarkerCall do not accept it, so neither
// chain ever sees a ring marker, and the ring chain's own scans see no other class's marker.
inline constexpr uint32_t kRingMarkerCall = 0x0D3B0000u;
inline bool IsRingMarkerCall(uint32_t call_site) { return (call_site & 0xFFFFFF00u) == kRingMarkerCall; }
// lane 227 (Wrestlers D3 H2): the wrestler class's marker. call_site = kWrMarkerCall | (keyed ordinal & 0xFF); the origin's object is the model, pass_site kMarkerPass. IsMarkerCall (the arena's) does not accept it, so the arena's scans
// never count one, and the wrestler chain's scans count only its own: an object's keyed ordinal is the same pair in both chains' guest ledgers because a wrestler object is never an arena object.
inline constexpr uint32_t kWrMarkerCall = 0x0D3D0000u;
inline bool IsWrestlerMarkerCall(uint32_t call_site) { return (call_site & 0xFFFFFF00u) == kWrMarkerCall; }

// ---- lane 211 (Caster D3 E2): the caster class rides the same chain with its own marker call and pass-aware keys -----------------------------------------------------------------------------
// A caster marker's call_site = kCasterMarkerCall | pass << 8 | (ordinal in that pass & 0xFF); its origin object is the model, pass_site kMarkerPass. IsMarkerCall (the arena's) does not accept it, so the arena's
// scans never see one. The key objects of the class (mark set, packet table, guest ledger) are COMPOSITES of (model, pass): one model draws up to three passes a frame (props / A / B map, plane, rim), each its own
// call sequence, so (model, pass) is the "object" the arena chain already keys by and the ordinal is the index within the pass.
inline constexpr uint32_t kCasterMarkerCall = 0x0D3C0000u;
inline bool IsCasterMarkerCall(uint32_t call_site) { return (call_site & 0xFFFF0000u) == kCasterMarkerCall; }
// lane 246 (16.8 precondition 4): the READER's claim decision, in one place so reader.h (TryD3Marker) and tools\d3\d3_unit_test.cpp call the same code. A marker is claimed (consumed as a marker draw, never built as a draw) when its class's
// chain is on; the arena's marker has always been claimed whenever any chain is (kept). A ring marker NOT claimed would reach DrawCommon as a geometry-store offset (a hole).
inline bool AnyD3Chain() { return g_arena_d3 || g_caster_d3 || g_wrestlers_d3 || g_ring_d3; }
inline bool MarkerClaimed(uint32_t call_site) {
  return IsMarkerCall(call_site) || (g_caster_d3 && IsCasterMarkerCall(call_site)) || (g_wrestlers_d3 && IsWrestlerMarkerCall(call_site)) || (g_ring_d3 && IsRingMarkerCall(call_site));
}
inline bool IsAnyMarkerCall(uint32_t call_site) { return IsMarkerCall(call_site) || IsCasterMarkerCall(call_site) || IsWrestlerMarkerCall(call_site) || IsRingMarkerCall(call_site); }   // the unclaimed-marker census: a tagged draw that was not claimed (required 0)
inline uint32_t CasterMarkerCall(uint32_t pass, uint32_t ordinal) { return kCasterMarkerCall | ((pass & 0xFFu) << 8) | (ordinal & 0xFFu); }
inline uint32_t CasterPassOfMarker(uint32_t call_site) { return (call_site >> 8) & 0xFFu; }
inline uint32_t CasterObjectId(uint32_t object, uint32_t pass) { return object ^ (pass * 0x9E3779B1u); }
// The pass sites of the caster parts (shadow_roles.h): 0x82C04634 inside sub_82C01FE0 (the shadow maps), 0x82BF4D38 inside sub_82BF22C8 (plane mask), 0x82BFBD58 inside sub_82BF9878 (rim map).
// The pass ids are the scene record's CasterPass values (kCpMap 1, kCpPlane 2, kCpRim 3; d3_record.h static_asserts it).
inline constexpr uint32_t kCasterSiteMap = 0x82C04634, kCasterSitePlane = 0x82BF4D38, kCasterSiteRim = 0x82BFBD58;
inline uint32_t CasterPassOfSite(uint32_t pass_site) { return pass_site == kCasterSiteMap ? 1u : pass_site == kCasterSitePlane ? 2u : pass_site == kCasterSiteRim ? 3u : 0u; }
// The draw class of a caster part's origin. Run 7138's capture [C]: the map and plane draws are DrawClass::Shadow (7), the rim draws DrawClass::ModelPart (1); lane 211's synthetic frames used ModelPart for all three. The pass site decides which pass; the class only has to be one of these.
inline bool IsCasterClass(DrawClass c) { return c == DrawClass::ModelPart || c == DrawClass::Shadow; }

// The object's proof frame (the contract: "not a proof frame, every K = 32nd frame by hash, as D1"). The reader's lite rule uses the same function for the keys of a D3 object, so in
// that frame every part of the object takes the full path and is compared.
inline bool ObjectProofFrame(uint32_t object, uint64_t frame, uint32_t k = kProofK) { return thin::IsProofFrame(thin::ObjectProofHash(object), frame, k); }
inline uint64_t MarkKey(uint32_t object, uint32_t ordinal) { return thin::Mix(thin::Mix(0xD3A7, object), ordinal) | 1ull; }

// ---- the faults (planted; fview --d3-fault NAME). The names are the gate's. ----------------------------------------------------------------------------------------------------
enum class Fault : uint8_t {
  None,
  HoleNoPacket,       // a marker is written without its packet
  OrdinalOff,         // the marker names another keyed ordinal than the binder's run count (the call ledger and the binder disagree on what a call is)
  SkipInProofFrame,   // the object is skipped in its proof frame (nothing then compares the marker path with the full path)
  TemplateFirst,      // lane 35's class: one template (the first part's) for every part of an object
  NoMirror,           // the 0x82C0B94C copies drawn with the un-negated camera (Session 97's half arena class)
  MovingStatic,       // the moving pieces drawn at their first world (the template's), not the packet's
  PlacedIdentity,     // the placed slots' world taken from the template's identity rows (ARENA_OURS.md "world differs 5,456")
  JoinByIndex,        // the packet joined by the capture's frame index instead of the guest frame
  Offset0,            // (lane 130's offset0) the packet joined at offset 0: another frame's packet
  RecordSets,         // the light registers taken from a record light set rather than the packet (PER_DRAW_LIGHTS.md 2)
  StaleTemplate,      // the template is never refreshed after the first learn (what the proof frames exist for)
  NoPass,             // the pass registers taken from the template, not from the pass bank
  PassMarker,         // DIAGNOSIS arm (not a plant): the pass registers taken from the marker's own bank (the previous draw's: the library's shadow) instead of the frame's pass bank
  // lane 152 (D3-F, the record route): the plants of the record sources (each must fail its named check)
  StatePrev,          // the light state of the PREVIOUS object's call (lane 109's state_prev_object, at the arena's join)
  RecordOffset0,      // the records joined at hook-to-stream offset 0 instead of +1 (the world stamp off by one)
  NegateDirs,         // the light directions taken negated (c8-c12 xyz) where the rows need no negation
  MirrorDirs,         // the light directions' x negated on the mirrored copies (site B) where the rows need none
  ClockOrigin,        // THE PAST BUG: c186 Time taken as the MotionSetup origin hypothesis (frame - origin from the kAwClock entry), which J1 proved false
  MarkUnsourced,      // a sequence with an UNSOURCED register is marked all the same (the reason is ignored)
  DrawUnsourced,      // (review S1) a marker whose part has an unsourced register is drawn with the zero fill (and such sequences are marked): the check that must turn it into a hole is switched off
  ProofSkew,          // (review S7) the hook's proof-frame test runs on a skewed clock: objects are skipped in their real proof frame and never re-proved
  PacketOnCachePath,  // (review S3) a packet is written for a call that cannot skip (cache path 3 / 4)
  CallIndexFromHook,  // (review S6) the packet's call index is the hook's own count, not the ordinal the light-state tracker will assign (a nested call shifts them)
  RecordLate,         // (review B1) the live pairing as it was: record i reaches the binder only when it binds frame i+1 (the PREVIOUS present's record): every proof misses, nothing may be marked
  SkipUnreadable,     // (16, guest core) the skip is decided before the light state was read: a skip although the state cannot be written
  MarkerCount0,       // (lane 204, Check K1) THE PAST BUG: the marker claimed only when the replayed index count is 0 (the V4 review fix); the replay handler delivers 2, so it never claims
  // lane 211 (Caster D3 E2 / E3, SKIP_GAME_WORK_MAP.md 17.8): the caster plants (each must fail its named check)
  StaleSlots,         // THE PAST BUG of the wrestlers' palette: bone slots past the part's own bone count taken as rows (the don't-care cut off)
  OrdinalMerge,       // map A and map B of one object keyed alike (the pass ordinal dropped from the key)
  KeyByPassWrong,     // the pass dropped from the key: a part drawn in B but not in the props / A map reads the wrong sequence
  CameraPrevFrame,    // the camera run taken one frame old
  PropsCameraForWrestlers,   // the props map's camera used for the wrestlers' maps (the two cameras of 17.2 item 2 swapped)
  CameraViewprojWrong,       // ViewProj taken from M(R+976) instead of M(R+912)
  CatchupRecord,             // (lane 211 SF1) a catch-up record's calls of hook frame f-1 paired with record f's bones (the frame pairing check off)
  FirstMatch,                // (lane 211 SF2) the slot -> bone map takes the FIRST bone whose rows match, ambiguity ignored
  // lane 221 (SKIP_GAME_WORK_MAP.md 17.10)
  RimCameraPrevFrame,        // the rim pass's camera (CasterExtra product) of the previous hook frame's entry: the wrong-frame camera
  RimCameraFromR,            // the rim pass's camera built from the entry's R as for the maps (the lane 211 / 217 rule that failed on 595 rim parts)
  CollapseOff,               // map A's rows are the colour pass's full rows (the collapse not applied)
  CollapseShift,             // the collapsed-slot set read one slot off
  CollapsePointStale,        // the collapse point (pose row 3) of the previous hook frame
  // lane 226 (SKIP_GAME_WORK_MAP.md 17.11, E5): the plane mask's sources
  PlaneCameraPrev,           // c13-c16 (CameraViewProj) from the previous stamp's PlaneFrame
  PlaneFitIdentity,          // c9-c12 (PlaneInv) = identity (the screen fit lost)
  PlanePosShift,             // c7 (PlanePos) moved 40 units
  PlanePosPrevFrame,         // lane 226 follow-up (S1): c7 (PlanePos) from the previous hook frame's entry snapshot of the same call
  LiveGuard,                 // lane 226 follow-up (S1): the core behaves as a LIVE one: a plane call is never marked (its sources are made inside the call; see D3Core::plane_marks_allowed)
  LiveGuardOff,              // PLANT: LiveGuard with the guard removed (a live core marks the plane): the check must see the plane marked
  PlaneMatsPrevFrame,        // the plane entry's palette matrices of the previous hook frame's same call
  // lane 229 (Ring D3 R3, SKIP_GAME_WORK_MAP.md 16.4 / 16.8): the ring plants (each must fail its named check)
  RingAlphaZero,             // c185 MdlAlpha taken as zero where the record route sources it (the skirts differ in the entrance)
  RingCanvasMarked,          // the canvas' own registers (c230-c237, no source) ignored: the canvas is marked all the same
  RingLodFlipOrdinal,        // the per-object call-count guard switched off: rope sequences keyed by ordinal across an LOD flip
  RingDedupeModelR5,         // the ring rows joined by (frame, model, r5) like the arena's tracker: the rope heights of one side collapse to one entry
  RingFillGenerationOff,     // lane 289 PLANT: the emulated hook writes generation + 1000 into the marker: the ordinal fill finds no mark set (fill_missing > 0, the ropes shift, the gate resets)
  RingMergeInstances,        // lane 258 THE PAST BUG (RL1): the ring's run scan closes a run only on another object, so the 12 instances of one model are ONE run (calls = runs differs, 11 of 12 instances' rows unmatched)
  // lane 227 (Wrestlers D3 H2, SKIP_GAME_WORK_MAP.md 18.7)
  BrockUpdatePalette,        // THE PAST BUG (lane 43 / the ledger's Brock rows): the palette taken from the record of the hook frame BEFORE the stamp (the update hook's timing: rows one update old)
  SkelPaletteZero,           // the palette rows of an object the record holds no bones for are composed as ZEROS and the object is marked all the same (the skel actors' rows unsourced but marked)
  SphereFromPrevFrame,       // VS SphereOccP / V c168-c171 taken from the part's last learned frame instead of this frame's value
  TexprojFromColourCall,     // VS c68-c71 TexProjLight taken from the colour call's light state (0 of 1920 reads match it) instead of the wrestler light camera
  PartCountsMissing,         // the marker's parts carry no vertex / index counts: the wrestler core's join fails for the object (about 9,000 draws lost in KC)
  // lane 274 (SKIP_GAME_WORK_MAP.md 10.12): the arena route's call scan
  ArenaMergeInstances,       // THE PAST BUG (the ring's RL1 on the arena chain): ScanRuns merges a model's back-to-back instance calls into ONE run (the arena route's split on a part restart per pass site switched off)
  // lane 290 (SKIP_GAME_WORK_MAP.md 17.14): the caster ledger's plane cap
  CasterPlaneNoGuard,        // lane 299 (review SF2): the caster route has no call-count guard for the plane pass again (the ring proxy's 12 calls change count: ordinal k gets another side's EyePos)
  CasterPlaneCap4,           // THE LEVER PLANT: the plane pass's ledger tracks 4 calls an object again (lane 268's kMaxCalls cap): the ring plane proxy's calls 4-11 get no marker
  Count
};
inline bool ParseFault(const std::string& s, Fault& f) {
  static const struct { const char* n; Fault f; } k[] = {{"", Fault::None}, {"hole_no_packet", Fault::HoleNoPacket}, {"ordinal_off", Fault::OrdinalOff}, {"skip_in_proof_frame", Fault::SkipInProofFrame},
                                                         {"template_first", Fault::TemplateFirst}, {"no_mirror", Fault::NoMirror}, {"moving_static", Fault::MovingStatic},
                                                         {"placed_identity", Fault::PlacedIdentity}, {"join_by_index", Fault::JoinByIndex}, {"offset0", Fault::Offset0},
                                                         {"record_sets", Fault::RecordSets}, {"stale_template", Fault::StaleTemplate}, {"no_pass", Fault::NoPass}, {"pass_marker", Fault::PassMarker},
                                                         {"state_prev_object", Fault::StatePrev}, {"record_offset0", Fault::RecordOffset0}, {"negate_dirs", Fault::NegateDirs}, {"mirror_dirs", Fault::MirrorDirs},
                                                         {"clock_origin", Fault::ClockOrigin}, {"mark_unsourced", Fault::MarkUnsourced}, {"skip_unreadable", Fault::SkipUnreadable}, {"record_late", Fault::RecordLate},
                                                         {"draw_unsourced", Fault::DrawUnsourced}, {"proof_skew", Fault::ProofSkew}, {"packet_on_cache_path", Fault::PacketOnCachePath}, {"call_index_from_hook", Fault::CallIndexFromHook}, {"marker_count0", Fault::MarkerCount0},
                                                         {"stale_slots", Fault::StaleSlots}, {"ordinal_merge", Fault::OrdinalMerge}, {"key_by_pass_wrong", Fault::KeyByPassWrong}, {"camera_prev_frame", Fault::CameraPrevFrame},
                                                         {"props_camera_for_wrestlers", Fault::PropsCameraForWrestlers}, {"camera_viewproj_wrong", Fault::CameraViewprojWrong},
                                                         {"catchup_record", Fault::CatchupRecord}, {"first_match", Fault::FirstMatch},
                                                         {"rim_camera_prev_frame", Fault::RimCameraPrevFrame}, {"rim_camera_from_r", Fault::RimCameraFromR}, {"collapse_off", Fault::CollapseOff},
                                                         {"collapse_shift", Fault::CollapseShift}, {"collapse_point_stale", Fault::CollapsePointStale},
                                                         {"plane_camera_prev", Fault::PlaneCameraPrev}, {"plane_fit_identity", Fault::PlaneFitIdentity}, {"plane_pos_shift", Fault::PlanePosShift},
                                                         {"plane_pos_prev_frame", Fault::PlanePosPrevFrame}, {"live_guard", Fault::LiveGuard}, {"live_guard_off", Fault::LiveGuardOff}, {"plane_mats_prev_frame", Fault::PlaneMatsPrevFrame},
                                                         {"stale_slots_counted", Fault::StaleSlots}, {"late_record", Fault::RecordLate}, {"alpha_zero", Fault::RingAlphaZero}, {"canvas_marked", Fault::RingCanvasMarked}, {"lod_flip_ordinal", Fault::RingLodFlipOrdinal},
                                                         {"dedupe_model_r5", Fault::RingDedupeModelR5}, {"ring_merge_instances", Fault::RingMergeInstances}, {"ring_fill_generation_off", Fault::RingFillGenerationOff},
                                                         {"brock_update_palette", Fault::BrockUpdatePalette}, {"skel_palette_zero", Fault::SkelPaletteZero}, {"sphere_from_prev_frame", Fault::SphereFromPrevFrame},
                                                         {"texproj_from_colour_call", Fault::TexprojFromColourCall}, {"part_counts_missing", Fault::PartCountsMissing},
                                                         {"arena_merge_instances", Fault::ArenaMergeInstances}, {"caster_plane_cap4", Fault::CasterPlaneCap4}, {"caster_plane_no_guard", Fault::CasterPlaneNoGuard}};
  for (const auto& e : k)
    if (s == e.n) return f = e.f, true;
  return false;
}

// lane 204 (Check K1, run 7093): what the REPLAY delivers for the hook's marker record. The hook records the marker as op 15 with r4 = prim 6 (triangle strip) and r7 = 0 (count). The replay handler sub_82BB41E8
// (docs/native/d3d/A_replay_ops.md op 15; src/d3d_layer.h) passes the draw call r7 = tbl[prim].mul * rec+28 + tbl[prim].add from the table at 0x82001B78 (8 bytes a primitive: multiplier, add); entry 6 is (1, 2), so
// the recorded count 0 replays as 1 * 0 + 2 = 2 indices (SKIP_GAME_WORK_MAP 10.7 item 6: "a 2-index strip"). The V4 claim test `index_count == 0` therefore never matched a live marker (K1: tagged but unclassified 24).
inline constexpr uint32_t kMarkerPrim = 6;
inline constexpr uint32_t kMarkerPrimMul = 1, kMarkerPrimAdd = 2;   // table 0x82001B78 entry 6
inline constexpr uint32_t ReplayIndexCount(uint32_t mul, uint32_t recorded, uint32_t add) { return mul * recorded + add; }   // sub_82BB41E8's arithmetic
inline constexpr uint32_t kMarkerReplayIndexCount = ReplayIndexCount(kMarkerPrimMul, 0, kMarkerPrimAdd);   // = 2: the recorded r7 (0) through the handler's arithmetic
// The marker's fixed draw arguments as the replayed draw call carries them: a real draw with a stale tag is passed on, never claimed.
inline bool IsMarkerReplay(uint32_t prim, uint32_t index_count, bool inline_vertices, Fault fault = Fault::None) {
  const uint32_t want = fault == Fault::MarkerCount0 ? 0u : kMarkerReplayIndexCount;
  return prim == kMarkerPrim && index_count == want && !inline_vertices;
}

// ---- the register groups: where a constant of the arena's programs comes from in D3 (SKIP_GAME_WORK_MAP.md 8.2) --------------------------------------------------------------
// Pass     the pass bank at the marker (camera, eye, fog, fades): the library's shadow when the marker is replayed; the mirrored copy 0x82C0B94C = the camera with x negated (draw_submit.md 150-151)
// Template the value of the part's last recorded-in-full frame (material constants, MdlAlpha, MipmapBias, identity worlds of the static slots)
// Light    DifLghCol c0-c4, DifLghVec c8-c12, sky / ground c181 / c182, key light c197 / c199: the light-state record (PER_DRAW_LIGHTS.md), unproven until J1
// Shadow   TexProjLight c68-c71, ViewLight c164-c166, ShadowPrm c168-c170: the light-state record's shadow fields
// World    c76-c138 of a placed / moving program (skinned or bone-id variants): the arena-worlds record (kScArenaWorlds, lane 130)
// Clock    c186 Time: the stage clock (kScArenaWorlds kAwClock)
// Drift    a pass / template register that a recorded-in-full frame showed NOT equal to its source (an animated MatDifCol c189, an A part that carries the mirrored camera): the ledger moves it to
//          the record side; nothing sources it today, so it is withheld in the readiness mode and a live object with one never proves complete
enum class Group : uint8_t { Pass, Template, Light, Shadow, World, Clock, Drift, Count };
inline const char* GroupName(Group g) {
  static const char* const k[] = {"pass", "template", "light", "shadow", "world", "clock", "drift"};
  return uint8_t(g) < uint8_t(Group::Count) ? k[uint8_t(g)] : "?";
}
// `programmed_world`: the variant moves or places its object (a bone-id / skinned program: the placed slots, or a moving piece's world), so c76.. are NOT the identity template.
inline Group GroupOf(uint16_t reg, bool programmed_world) {
  if (reg <= 4 || (reg >= 8 && reg <= 12) || reg == 181 || reg == 182 || reg == 197 || reg == 199) return Group::Light;
  if ((reg >= 68 && reg <= 71) || (reg >= 164 && reg <= 166) || (reg >= 168 && reg <= 170)) return Group::Shadow;
  if (reg == 186) return Group::Clock;
  if (reg >= 76 && reg <= 138) return programmed_world ? Group::World : Group::Template;
  if ((reg >= 56 && reg <= 59) || reg == 72 || reg == 74 || reg == 180 || reg == 184) return Group::Pass;
  return Group::Template;
}
// lane 211: the registers of the caster programs (yShadowMap / yRimLightMap / yPlaneShadowMap). Camera run (ViewProj c56-c59, ViewDepth c54, Trapezoidal c64-c67, RimLightDepth c162, ShadowPrm c168-c170) = the Shadow group (the
// pass-entry camera record); the palette rows c76.. = the World group (the wrestler record's bones through the part's slot -> node map). The plane's own registers (c0-c7, c9-c16, c72) are the Shadow group too and have
// no source until E5. Nothing is a Pass register: a caster pass has no "pass bank" of its own (the pass bank is the colour draws' camera).
// lane 226 (E5): `pass` = the CasterPass (2 = plane). The plane mask's EyePos c72 is a TEMPLATE register (learned in a recorded-in-full call, held, re-proved at every proof frame): its writer is [U] (SHADOW_PLANE.md
// 6.3: a constant per proxy instance, ring (+-1000, -800, 0) / (0, -800, +-1000) by side, wrestler proxy (-450, -1400, 0); 24 of 24 ring and 8 of 8 wrestler-proxy draws of run 7149 carry the same value in
// every sampled frame [C: tools\caster\plane_regs_probe.py]). The sequence key is (object, call ordinal), so each ring side has its own sequence and its own held value.
inline Group GroupOfCaster(uint16_t reg, bool programmed_world, uint8_t pass = 0) {
  if (reg == 72 && pass == 2) return Group::Template;
  if (reg == 54 || (reg >= 56 && reg <= 59) || (reg >= 64 && reg <= 67) || reg == 162 || (reg >= 168 && reg <= 170)) return Group::Shadow;
  if (reg >= 76 && reg <= 138) return programmed_world ? Group::World : Group::Template;
  if (reg <= 16 || reg == 72) return Group::Shadow;   // the plane mask's (EyePos c72, PlanePos c7, Shadow* c0-c6, PlaneInv c9-c12, CameraViewProj c13-c16): E5
  return Group::Template;
}
// lane 227 (Wrestlers D3 H2): the registers of the wrestlers' yCh_ programs. The same classes as the arena's, plus VS c168-c171 SphereOccP / V (the contact spheres, H3): per wrestler per frame, no record source yet (Shadow group,
// named in the census); PS c168-c170 ShadowPrm and VS c68-c71 / c166 are already the Shadow group of GroupOf. `stage`: 0 VS, 1 PS.
inline Group GroupOfWrestler(uint16_t reg, uint32_t stage, bool programmed_world) {
  if (stage == 0 && reg >= 168 && reg <= 171) return Group::Shadow;
  if (reg >= 52 && reg <= 54) return Group::Pass;   // View (VS + PS): shared by every main-pass draw of the frame (308 of 308 in run 7147, frame 100), so the pass bank sources it (PassBank wide)
  if (reg == 162) return Group::Shadow;             // RimLightDepth (near, far, 0, 0): the rim pass's depth range, in the record (CasterExtra kCxDepth); the arena's programs do not read it
  return GroupOf(reg, programmed_world);
}
// Which groups a packet CAN carry once its sources exist (the record fields); `sourced_now` = the ones with a built source (offline: none of the record groups; the emulation's stream mode
// stands in for all of them as "emulated record fields").
inline bool IsRecordGroup(Group g) { return g == Group::Light || g == Group::Shadow || g == Group::World || g == Group::Clock || g == Group::Drift; }

// ---- why an object was recorded in full instead of skipped ---------------------------------------------------------------------------------------------------------------------
enum class Why : uint8_t {
  Skip = 0, SwitchOff, NotOwner, GateNotProven, ProofFrame, NotMarkable, NoSequence, LedgerIncomplete, PacketFailed, TableFull, StateOverflow, Unreadable,
  CachePath3,   // the game would begin recording this model's cached command list in this call (sub_82BD7B50 path 3): skipping would seal an EMPTY cached list
  CachePath4,   // the game replays this model's cached command list (op 81, path 4): not ours to skip
  CachePath2,   // lane 237: the model's cache is being RECORDED ([[model+896]+slot*4] != 0, path 2): the slot's list is redirected into the cache buffer, a marker would be sealed into it (SKIP_GAME_WORK_MAP.md 11.9)
  OuterRecording,   // lane 228 (review S2): a caster pass entered while an OUTER cache recording is open on the thread's slot ([entry+36] != 0, sub_82BB4AE0 0x82BB4B8C / sub_82BB4B98 0x82BB4C34): a marker would be sealed into that cache
  Count
};
inline const char* WhyName(Why w) {
  static const char* const k[] = {"skip", "switch off", "not owner", "gate not proven", "proof frame", "not markable", "no sequence", "ledger incomplete", "packet failed", "table full",
                                  "state overflow", "unreadable", "cache path 3 (game records its cache)", "cache path 4 (game replays its cache)", "cache path 2 (game records its cache)", "outer cache recording open (marker would be sealed into it)"};
  return uint8_t(w) < uint8_t(Why::Count) ? k[uint8_t(w)] : "?";
}
// sub_82BD7B50 is the model's CACHED COMMAND-LIST manager (lane 138 review B; SKIP_GAME_WORK_MAP.md 10.2): which of its four paths this call takes decides whether a skip is safe.
//   1 Fresh1  [model+888]==0 or ([model+600]&1)==0: no cache in use, the model is drawn fresh        -> skippable
//   2 Fresh2  [[model+896]+slot*4]!=0 (local=1): the colour pass between the outer begin and end is being RECORDED into the model's cache buffer -> NOT skippable (lane 237, 11.9)
//   3 Record3 the handle r19=[model+852|848] (by [[model+900]+slot*4]) has [r19]==0: the game begins recording the cache (sub_82BB4AE0), the tail seals it (op 82) -> record in full
//   4 Cached4 [r19]!=0: op 81 replays the cached list (sub_82C24500), returns 1 -> left to the game
enum class CachePath : uint8_t { Fresh1, Fresh2, Record3, Cached4, Unreadable };
struct SkipInputs {
  CachePath path = CachePath::Fresh1;
  bool switch_on = false, owner = false, gate_proven = false, proof_frame = false;
  bool markable = false;          // the object is in the published mark set (every key it drew in its last proof frame pinned, the ledger complete)
  bool packet_ok = false;         // the packet was written (the side table had room, the state read, count <= kMaxStates)
  Why packet_why = Why::PacketFailed;
};
// The decision, in the contract's order (8.3 item 3). `skip_in_proof_frame` is the plant: the proof-frame test is not applied.
inline Why Decide(const SkipInputs& in, Fault fault = Fault::None) {
  if (!in.switch_on) return Why::SwitchOff;
  if (!in.owner) return Why::NotOwner;
  if (!in.gate_proven) return Why::GateNotProven;
  if (in.proof_frame && fault != Fault::SkipInProofFrame) return Why::ProofFrame;
  if (!in.markable) return Why::NotMarkable;
  if (in.path == CachePath::Record3) return Why::CachePath3;
  if (in.path == CachePath::Cached4) return Why::CachePath4;
  if (in.path == CachePath::Fresh2) return Why::CachePath2;   // lane 228: a CASTER pass's path 2 is filed here too (PredictPathCaster returns Fresh2): never skipped, the histograms agree
  if (in.path == CachePath::Unreadable) return Why::Unreadable;
  if (!in.packet_ok) return in.packet_why;
  return Why::Skip;
}

// ---- the gate (crowd_skip_core.h's shape, generalised): the binder reports once per bound frame, the hook asks ------------------------------------------------------------------
inline constexpr uint32_t kLatchResets = 3;      // (review B2) this many resets ...
inline constexpr int64_t kLatchWindowMs = 10000;  // ... inside this window latch D3 off for the session: a systematic hole must not loop skip / hole / reset forever
struct Gate {
  bool latch_enabled = true;                  // (unit test plant `no_latch` turns it off)
  std::atomic<bool> latched{false};
  int64_t reset_ms[kLatchResets] = {};        // the last resets' times (the one reporter thread writes them)
  uint32_t reset_n = 0;
  std::atomic<uint32_t> clean{0};
  std::atomic<int64_t> last_ms{0};
  std::atomic<uint64_t> reports{0}, resets{0};
  // drew = the binder bound the frame with the class on; bad = holes + proof mismatches this frame (a frame with any resets the gate: the game records everything again until 3 clean frames)
  // `hole_bad` = the part of `bad` that is a HOLE-type cause (holes, unconsumed packets, no sequence, ordinal mismatch): only those resets count toward the LATCH. A marked object's proof mismatch resets the gate
  // (it unmarks the object) but never latches the session: real changes (an entrance's light cues, a camera cut) would otherwise latch it. Default: every bad frame counts.
  void Report(bool drew, uint64_t bad, int64_t now_ms, uint64_t hole_bad = ~0ull) {
    {   // lane 295 SF1: silence voids the proof for good (the first report after > kStaleMs must not re-prove from the previous scene's clean frames)
      const int64_t t0 = last_ms.load(std::memory_order_relaxed);
      if (t0 != 0 && now_ms - t0 > kStaleMs) clean.store(0, std::memory_order_relaxed);
    }
    if (drew && bad == 0) {
      const uint32_t c = clean.load(std::memory_order_relaxed);
      if (c < 1000000u) clean.store(c + 1, std::memory_order_relaxed);
    } else {
      if (clean.load(std::memory_order_relaxed) >= kNeedCleanFrames) {
        resets.fetch_add(1, std::memory_order_relaxed);
        if (hole_bad) reset_ms[reset_n++ % kLatchResets] = now_ms;
        if (hole_bad && latch_enabled && reset_n >= kLatchResets) {   // the oldest of the last kLatchResets resets is inside the window: latch
          int64_t oldest = reset_ms[0];
          for (uint32_t i = 1; i < kLatchResets; ++i) oldest = std::min(oldest, reset_ms[i]);
          if (now_ms - oldest <= kLatchWindowMs) latched.store(true, std::memory_order_release);
        }
      }
      clean.store(0, std::memory_order_relaxed);
    }
    last_ms.store(now_ms, std::memory_order_relaxed);
    reports.fetch_add(1, std::memory_order_relaxed);
  }
  bool Proven(int64_t now_ms) const {
    const int64_t t = last_ms.load(std::memory_order_relaxed);
    return !latched.load(std::memory_order_acquire) && t != 0 && now_ms - t <= kStaleMs && clean.load(std::memory_order_relaxed) >= kNeedCleanFrames;
  }
};
inline Gate g_gate;

// ---- the mark set: binder -> hook. The objects (per call ordinal) the binder holds a complete, current sequence for ------------------------------------------------------------------
struct MarkSet {
  uint32_t generation = 0;
  std::vector<uint64_t> keys;   // sorted MarkKey(object, ordinal)
  // lane 289 (the ring SKIP arm): parallel to `keys` (empty for every chain but the ring's): the part triples (thin::TripleHash(site, object, part), one per part DRAW of the sequence, a doubled part twice) of each marked call. A skipped call's
  // draws never reach the stream, so the READER (reader_thin.h Reserve) and the BINDER (thin::ComputeKeys fill) add these to their per-frame ordinal counters at the marker's position; both read this one published list by the generation the marker carries.
  std::vector<std::vector<uint64_t>> parts;
  bool Has(uint64_t k) const { return std::binary_search(keys.begin(), keys.end(), k); }
  const std::vector<uint64_t>* PartsOf(uint64_t k) const {
    if (parts.size() != keys.size()) return nullptr;
    const auto it = std::lower_bound(keys.begin(), keys.end(), k);
    return (it != keys.end() && *it == k) ? &parts[size_t(it - keys.begin())] : nullptr;
  }
};
class MarkPublisher {
 public:
  uint32_t Publish(std::vector<uint64_t> keys) { return PublishSet(std::move(keys), {}); }
  // lane 289: keys with their part-triple lists (the ring chain). The recent sets are kept (kHistory) so a marker finds the set of the generation it carries.
  uint32_t PublishParts(std::vector<std::pair<uint64_t, std::vector<uint64_t>>> entries) {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> keys;
    std::vector<std::vector<uint64_t>> parts;
    keys.reserve(entries.size()), parts.reserve(entries.size());
    for (auto& e : entries) keys.push_back(e.first), parts.push_back(std::move(e.second));
    return PublishSet(std::move(keys), std::move(parts));
  }
  std::shared_ptr<const MarkSet> Acquire() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }
  // The set a marker's generation names (the newest kHistory part-carrying sets; null when it is older or never existed).
  std::shared_ptr<const MarkSet> Find(uint32_t generation) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (current_ && current_->generation == generation) return current_;
    for (const auto& s : hist_)
      if (s->generation == generation) return s;
    return nullptr;
  }
  uint32_t generation() const { return gen_atomic_.load(std::memory_order_acquire); }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    current_.reset();
    hist_.clear();
    generation_ = 0;
    gen_atomic_.store(0, std::memory_order_release);
  }
  static constexpr size_t kHistory = 64;

 private:
  uint32_t PublishSet(std::vector<uint64_t> keys, std::vector<std::vector<uint64_t>> parts) {
    if (parts.empty()) std::sort(keys.begin(), keys.end());   // (PublishParts passes keys already sorted with their parts)
    std::lock_guard<std::mutex> lock(mu_);
    if (current_ && current_->keys == keys && current_->parts == parts) return current_->generation;
    auto set = std::make_shared<MarkSet>();
    set->generation = ++generation_;
    set->keys = std::move(keys);
    set->parts = std::move(parts);
    if (current_ && !current_->parts.empty()) {   // only the part-carrying (ring) sets are kept: the other chains' sets can be large and nobody asks for their history
      hist_.push_back(current_);
      while (hist_.size() > kHistory) hist_.pop_front();
    }
    current_ = std::move(set);
    gen_atomic_.store(generation_, std::memory_order_release);
    return generation_;
  }
  mutable std::mutex mu_;
  std::shared_ptr<const MarkSet> current_;
  std::deque<std::shared_ptr<const MarkSet>> hist_;
  uint32_t generation_ = 0;
  std::atomic<uint32_t> gen_atomic_{0};
};
inline MarkPublisher g_marks;   // the live pair (our render thread publishes, the game thread asks); fview keeps its own

// ---- the packet and its side table ----------------------------------------------------------------------------------------------------------------------------------------------
// Live, the packet's content IS the scene record: the light-state ref (object, ordinal, state) and the arena-worlds entry stamped with the hook's guest frame, so the table only has to
// prove that the marker's packet exists. Offline (fview), the "emulated record fields" are carried here: per part, the stream's banks (the registers of the record groups are read from them).
struct PartFields {
  std::vector<uint32_t> vs, ps;   // the stream's 256 x 4 dwords for the vertex / pixel bank (empty: live)
};
struct Packet {
  uint64_t frame = 0;
  uint32_t object = 0, ordinal = 0;
  uint32_t call = 0;   // lane 152: the call's index among ALL this object's calls in the frame (the light-state ref's ordinal); `ordinal` is the keyed ordinal (the arena-worlds entry's index)
  std::vector<PartFields> parts;   // OFFLINE ONLY, never a source in Mode::Record: the stream's own banks, the truth the composed marker parts are compared with
};
struct TableStats {
  uint64_t put = 0, taken = 0, full = 0, evicted_unconsumed = 0, missing = 0;
};
class PacketTable {
 public:
  explicit PacketTable(size_t capacity = 4096) : capacity_(capacity) {}
  void SetCapacity(size_t n) { capacity_ = n; }
  static uint64_t Key(uint64_t frame, uint32_t object, uint32_t ordinal) { return thin::Mix(thin::Mix(thin::Mix(0x9AC4, frame), object), ordinal); }
  // Producer (the game thread): false = no room (the hook then records the object in full: Why::TableFull).
  bool Put(Packet p) {
    std::lock_guard<std::mutex> lock(mu_);
    if (map_.size() >= capacity_) {
      ++st_.full;
      return false;
    }
    const uint64_t k = Key(p.frame, p.object, p.ordinal);
    order_.push_back({p.frame, k});
    map_[k] = std::make_shared<Packet>(std::move(p));
    ++st_.put;
    return true;
  }
  // Consumer (the binder): the packet for a marker, or null (a HOLE: the caller counts it). A packet is taken once.
  std::shared_ptr<const Packet> Take(uint64_t frame, uint32_t object, uint32_t ordinal) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(Key(frame, object, ordinal));
    if (it == map_.end()) return ++st_.missing, nullptr;
    auto p = std::move(it->second);
    map_.erase(it);
    ++st_.taken;
    return p;
  }
  // Frames older than `frame` - keep are dropped (a packet no marker came for: counted, not a hole).
  void Evict(uint64_t frame, uint64_t keep = 8) {
    std::lock_guard<std::mutex> lock(mu_);
    while (!order_.empty() && order_.front().first + keep < frame) {
      if (map_.erase(order_.front().second)) ++st_.evicted_unconsumed;
      order_.erase(order_.begin());
    }
  }
  TableStats stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return st_;
  }
  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
    order_.clear();
    st_ = {};
  }

 private:
  mutable std::mutex mu_;
  size_t capacity_;
  std::unordered_map<uint64_t, std::shared_ptr<Packet>> map_;
  std::vector<std::pair<uint64_t, uint64_t>> order_;
  TableStats st_;
};
inline PacketTable g_packets;   // the live pair; fview keeps its own

// ---- the per-frame counts the capture header carries (names.h kSecD3 / D3FrameCounts) and the census -----------------------------------------------------------------------------------
struct FrameCounts {
  uint32_t markers = 0, dropped = 0;
  uint32_t why[uint8_t(Why::Count)] = {};
};
// The guest side's running tally for the frame in progress (the hook adds, the reader stamps the frame and resets it at the frame boundary).
struct GuestTally {
  std::atomic<uint32_t> markers{0}, dropped{0};
  std::atomic<uint32_t> why[uint8_t(Why::Count)] = {};
  // lane 237: CUMULATIVE counters for the census (the reader's TakeAndReset zeroes the per-frame ones above every frame, so the census read 0): never reset.
  std::atomic<uint64_t> cum_markers{0}, cum_dropped{0}, cum_calls{0}, cum_path[5] = {};   // cum_path: CachePath order (Fresh1, Fresh2, Record3, Cached4, Unreadable)
  std::atomic<uint64_t> cum_why[uint8_t(Why::Count)] = {};
  void NotePath(CachePath p) { cum_calls.fetch_add(1, std::memory_order_relaxed), cum_path[uint8_t(p) < 5 ? uint8_t(p) : 4].fetch_add(1, std::memory_order_relaxed); }
  // lane 152 (item 17, the calls = runs census): keyed calls the hook saw (skipped or recorded in full), by hook frame, in a small ring. The binder compares the frame's total with its keyed runs.
  struct KeyedSlot {
    std::atomic<uint64_t> frame{~0ull};
    std::atomic<uint32_t> n{0};
  };
  static constexpr uint32_t kKeyedRing = 256;   // lane 301: widened from 64 (the live queue is 4-8 deep, but a frame with no keyed call used to leave no slot at all)
  KeyedSlot keyed_ring[kKeyedRing];
  void SetKeyedCalls(uint64_t frame, uint32_t n) {   // lane 274 (the real-calls arm, fview only): the frame's keyed-call count as the CAPTURE's records say it (LightRefs), instead of one per emulated run
    KeyedSlot& s = keyed_ring[frame % kKeyedRing];
    s.frame.store(frame, std::memory_order_relaxed);
    s.n.store(n, std::memory_order_relaxed);
  }
  void NoteKeyedCall(uint64_t frame) {
    KeyedSlot& s = keyed_ring[frame % kKeyedRing];
    if (s.frame.load(std::memory_order_relaxed) != frame) {
      s.frame.store(frame, std::memory_order_relaxed);
      s.n.store(0, std::memory_order_relaxed);
    }
    s.n.fetch_add(1, std::memory_order_relaxed);
  }
  // lane 301: the hook SAW this frame (any call): its slot exists with n = 0 until a keyed call is noted, so a frame whose hook calls were all not-keyed is still compared (the binder's keyed runs must then be 0 too).
  void TouchFrame(uint64_t frame) {
    KeyedSlot& s = keyed_ring[frame % kKeyedRing];
    if (s.frame.load(std::memory_order_relaxed) != frame) {
      s.frame.store(frame, std::memory_order_relaxed);
      s.n.store(0, std::memory_order_relaxed);
    }
  }
  // lane 301 (10.12 5(b) / 11.10 (3)): the objects whose call took cache path 4 (a replayed list: the hook records no part, the binder sees the list's runs) per hook frame, so calls = runs leaves their runs out. Bounded: a
  // frame past kCachedMax objects sets the overflow flag and is not compared (counted in cached_overflow).
  static constexpr uint32_t kCachedMax = 128;
  struct CachedSlot {
    std::atomic<uint64_t> frame{~0ull};
    std::atomic<uint32_t> n{0};
    std::atomic<bool> overflow{false};
    std::atomic<uint32_t> obj[kCachedMax];
  };
  std::unique_ptr<CachedSlot[]> cached_ring{new CachedSlot[kKeyedRing]};   // lane 301 review B1: on the heap (~140 KB; GuestTally lives by value in D3Core, which lives by value in the Binder, a stack local of the render thread)
  std::atomic<uint64_t> cached_overflow{0};
  void NoteCachedObject(uint64_t frame, uint32_t object) {
    CachedSlot& s = cached_ring[frame % kKeyedRing];
    if (s.frame.load(std::memory_order_relaxed) != frame) {
      s.n.store(0, std::memory_order_relaxed);
      s.overflow.store(false, std::memory_order_relaxed);
      s.frame.store(frame, std::memory_order_relaxed);
    }
    const uint32_t n = s.n.load(std::memory_order_relaxed);
    if (s.overflow.load(std::memory_order_relaxed)) return;   // N4: a frame already over the bound stops scanning
    for (uint32_t i = 0; i < n && i < kCachedMax; ++i)
      if (s.obj[i].load(std::memory_order_relaxed) == object) return;
    if (n >= kCachedMax) {
      if (!s.overflow.exchange(true, std::memory_order_relaxed)) cached_overflow.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    s.obj[n].store(object, std::memory_order_relaxed);
    s.n.store(n + 1, std::memory_order_release);
  }
  // true when the slot holds this frame and has not overflowed; the objects are listed into `out` (cleared first). False: nothing noted for the frame (no cached call) or overflow: `overflow` tells which.
  bool CachedObjects(uint64_t frame, std::vector<uint32_t>& out, bool* overflow = nullptr) const {
    out.clear();
    const CachedSlot& s = cached_ring[frame % kKeyedRing];
    if (overflow) *overflow = false;
    if (s.frame.load(std::memory_order_relaxed) != frame) return false;
    if (s.overflow.load(std::memory_order_relaxed)) {
      if (overflow) *overflow = true;
      return false;
    }
    const uint32_t n = s.n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n && i < kCachedMax; ++i) out.push_back(s.obj[i].load(std::memory_order_relaxed));
    return true;
  }
  // false: the ring holds nothing for that frame (the hook was off, or the slot was reused)
  bool KeyedCalls(uint64_t frame, uint32_t& n) const {
    const KeyedSlot& s = keyed_ring[frame % kKeyedRing];
    if (s.frame.load(std::memory_order_relaxed) != frame) return false;
    n = s.n.load(std::memory_order_relaxed);
    return true;
  }
  void Note(Why w) {
    why[uint8_t(w)].fetch_add(1, std::memory_order_relaxed);
    cum_why[uint8_t(w)].fetch_add(1, std::memory_order_relaxed);
    if (w == Why::Skip) cum_markers.fetch_add(1, std::memory_order_relaxed);
    else if (w != Why::SwitchOff && w != Why::NotMarkable && w != Why::NotOwner) cum_dropped.fetch_add(1, std::memory_order_relaxed);
    if (w == Why::Skip) markers.fetch_add(1, std::memory_order_relaxed);
    else if (w != Why::SwitchOff && w != Why::NotMarkable && w != Why::NotOwner) dropped.fetch_add(1, std::memory_order_relaxed);   // eligible objects the game recorded in full
  }
  FrameCounts TakeAndReset() {
    FrameCounts c;
    c.markers = markers.exchange(0, std::memory_order_relaxed);
    c.dropped = dropped.exchange(0, std::memory_order_relaxed);
    for (uint8_t i = 0; i < uint8_t(Why::Count); ++i) c.why[i] = why[i].exchange(0, std::memory_order_relaxed);
    return c;
  }
};
inline GuestTally g_tally;

// ---- lane 228 (Caster D3, the live seam; SKIP_GAME_WORK_MAP.md 17.12): the caster class's live pair. The caster hooks (scene_live\d3_caster_hook.h, on the three pass entries) and the live binder's caster chain share these; the
// ARENA's globals above are not used by it (self-contained: d3::CasterChain points its packets / marks / gate / tally here in SetD3CasterLive). Nothing sets g_caster_hook_on unless --wwe2k14_rdr_caster_d3 turned the chain on.
inline bool g_caster_verify = false;                 // --wwe2k14_rdr_caster_d3_verify: markers are written, the original always runs, the binder compares (the arena's g_verify)
inline std::atomic<bool> g_caster_hook_on{false};    // the caster hooks' one relaxed load
inline bool g_caster_plane = false;                  // lane 268: --wwe2k14_rdr_caster_d3_plane (default off): the PLANE pass joins the caster cut (marks published live, a marked plane call no longer withdrawn). Needs --wwe2k14_scene_plane (the c0-c16 tables). Verify arm first; see SKIP_GAME_WORK_MAP.md 17.13
inline PacketTable g_cpackets;
inline MarkPublisher g_cmarks;
inline Gate g_cgate;
inline GuestTally g_ctally;

// ---- lane 246 (Ring D3, the live seam; SKIP_GAME_WORK_MAP.md 16.9): the ring class's live set. The ring hook (scene_live\d3_ring_hook.h, inside the existing REX_HOOK_RAW on sub_82C04DF0) and the live binder's ring chain share these;
// the ARENA's globals (g_packets / g_marks / g_gate / g_tally / g_hook_on) are not used by it (d3::RingChain points its packets / marks / gate / tally here in SetD3RingLive). g_ring_d3 (above) is the R3 CHAIN switch; the cvar
// --wwe2k14_rdr_ring_d3 is the R2 RECORD switch (lane 194) and is NOT wired to either (16.8 N5). Nothing sets g_ring_hook_on unless --wwe2k14_rdr_ring_d3_live turned the chain on.
inline bool g_ring_verify = false;                  // --wwe2k14_rdr_ring_d3_live_verify: markers are written, the original always runs, the binder compares (the arena's g_verify)
inline std::atomic<uint64_t> g_ring_fills{0}, g_ring_fill_missing{0};   // lane 289: the READER's ordinal fills (reader.h TryD3Marker) and the markers whose mark-set generation / key it could not find (required 0); read by RingHookStats::Line
inline std::atomic<bool> g_ring_hook_on{false};     // the ring hook's one relaxed load
inline PacketTable g_rpackets;
inline MarkPublisher g_rmarks;
inline Gate g_rgate;
inline GuestTally g_rtally;
// The class table of 16.8 precondition 6, as the hook last read it: the 13 ring model pointers of the object at [0x836F4C74] (scene_live\ring_rows_core.h kClassTable). The hook claims a call whose model is one of them for the ring
// chain and the arena's hook never sees it; the binder filters its two `calls = runs` guards by the SAME table (IsLiveRingModel), so each guard counts each call exactly once.
inline constexpr uint32_t kRingModelSlots = 13;
inline std::atomic<uint32_t> g_ring_models[kRingModelSlots] = {};
inline bool IsLiveRingModel(uint32_t object) {
  if (!object) return false;
  for (uint32_t i = 0; i < kRingModelSlots; ++i)
    if (g_ring_models[i].load(std::memory_order_relaxed) == object) return true;
  return false;
}
// ---- lane 245 (Wrestlers D3, the live seam; SKIP_GAME_WORK_MAP.md 18.9): the wrestler class's live pair. The wrestler scope (scene_live\d3_wrestler_hook.h, on the colour call sub_82C04DF0) and the live binder's wrestler chain
// share these; the ARENA's, the ring's and the casters' globals are not used by it (self-contained: d3::WrestlerChain points its packets / marks / gate / tally here in SetD3WrestlersLive). Nothing sets
// g_wrestlers_hook_on unless --wwe2k14_rdr_wrestlers_d3 turned the chain on (live.cpp). g_wrestlers_d3 (above) is the class switch the decision and the binder read.
inline bool g_wrestlers_verify = false;                 // --wwe2k14_rdr_wrestlers_d3_verify: markers are written, the original always runs, the binder compares (the arena's g_verify)
inline std::atomic<bool> g_wrestlers_hook_on{false};    // the wrestler scope's one relaxed load
inline PacketTable g_wpackets;
inline MarkPublisher g_wmarks;
inline Gate g_wgate;
inline GuestTally g_wtally;

enum class Differ : uint8_t { Program, Group0, Count };   // (placeholder: the proof compare names register groups)
struct D3Counters {
  uint64_t frames = 0;               // frames bound with the switch on
  uint64_t markers = 0;              // marker draws met
  uint64_t drawn = 0;                // part draws synthesized and drawn from descriptors
  uint64_t holes = 0;                // markers without a packet (required 0)
  uint64_t no_sequence = 0;          // markers whose (object, ordinal) the binder holds no sequence for (a hole of the other kind: required 0)
  uint64_t dropped_markers = 0;      // markers whose draws were lost (a hole, a template gone): the part draws that did not happen
  uint64_t learned = 0;              // part descriptors learned / refreshed
  uint64_t objects_complete = 0;     // objects whose ledger was complete at their last proof frame (in the mark set at the end)
  uint64_t proof_objects = 0;        // object proof compares
  uint64_t proof_parts = 0;          // part compares in proof frames
  uint64_t proof_mismatch_parts = 0; // parts whose composed bank / state differed from the stream's (by group below)
  uint64_t by_group[uint8_t(Group::Count)] = {};   // register mismatches by group in proof compares
  uint64_t state_differ = 0;         // parts whose fixed state / dynamic state / slots differed from the descriptor
  uint64_t gate_resets = 0;
  uint64_t reg_reads = 0;            // registers composed (the variant's ranges) in sourced mode
  uint64_t reg_withheld = 0;         // of them taken from a record group the packet does not yet carry
  uint64_t draws_would_differ = 0;   // parts that read a withheld register whose stream value is not blank (readiness mode)
  uint64_t parts_total = 0;
  uint64_t marker_ordinal_mismatch = 0;   // markers whose call_site ordinal is not the binder's keyed-run count for the object (a call the ledger and the binder count differently)
  uint64_t unconsumed = 0;           // packets evicted without a marker taking them (counted as holes: the hook wrote a packet for a skip that never arrived)
  uint64_t pass_fallback = 0;        // pass registers the frame's pass bank had no value for (taken from the template: reported, never silent)
  uint64_t seq_len_differs = 0;      // markers whose sequence length differs from the stream's draw count for that object this frame (a part list that changes frame to frame)
  uint64_t would_differ_by_group[uint8_t(Group::Count)] = {};
  // lane 152 (D3-F)
  uint64_t proof_mismatch_marked = 0;   // (11) proof mismatches of objects that WERE in the mark set: the gate counts these only (an unmarked object's mismatch only keeps it unmarked)
  uint64_t record_missing = 0;          // markers / proof compares whose frame's records held no light state ref for the call (Mode::Record)
  uint64_t calls_checked = 0, calls_differ = 0;   // (17) frames where the hook's keyed calls were compared with the binder's keyed runs, and those that differed
  uint64_t keyed_draws = 0;             // draws of keyed arena runs seen (recorded in full) + synthesized from markers: the share base
  uint64_t verify_markers = 0, verify_parts = 0, verify_mismatch_parts = 0, verify_unsourced_parts = 0, verify_len_differs = 0;   // (10) the verify arm: markers written, nothing dropped, stream draws compared with the sequence
  uint64_t proof_overdue = 0;           // (review S7) sequences marked-eligible whose last proof compare is older than 2 x K frames: unmarked (the live detector of a skewed hook-to-reader offset)
  uint64_t latched = 0;                 // (review B2) 1 = the gate latched D3 off for the session
  uint64_t marker_unclaimed = 0;        // lane 204: (offline emulation) markers the claim decision (IsMarkerReplay) refused: required 0 (the live reader counts d3_marker_unclaimed in the names census)
  uint64_t palette_unknown_parts = 0;   // lane 258 (ring): parts captured (learn / refresh) whose palette size is still unknown (a lite draw before any full frame gave it): their c76-c138 reads are NoPalette
  uint64_t ring_refused = 0;            // lane 274 (S2): complete sequences of RING models the arena chain refused to mark (this Publish; the census names them in `not marked`)
  uint64_t marked_unsourced = 0;        // sequences in the mark set that carry an unsourced register (required 0; the plant mark_unsourced). lane 301: refreshed from the CURRENT parts at every Publish (the proof's unsourced map, or drift learned since the proof)
  // lane 301 (SKIP_GAME_WORK_MAP.md 11.10): the marked sequence is a frozen, certified version. `drift_after_mark` = full frames that changed a COMPLETE sequence (drift learned, part list changed): it left the mark set at once and the last proven
  // parts serve the markers in flight. `unsourced_after_mark` = markers whose SEQUENCE carries a register no record can give (drift, not mapped, palette): with the freeze this must stay 0; `record_missing` is now only a light / world / clock
  // record that is really absent. `frozen_markers` = markers drawn from the frozen (proven) parts.
  uint64_t drift_after_mark = 0, unsourced_after_mark = 0, frozen_markers = 0, marked_unsourced_peak = 0;   // peak: the largest `marked_unsourced` any Publish saw (the end-of-run value alone can read 0 after the fact)
};

inline std::string CensusLine(const D3Counters& k, uint64_t skippable_objects, uint64_t pinned_objects) {
  char text[1000];
  std::snprintf(text, sizeof text,
                "[rdr][d3] arena markers %llu, part draws %llu, holes %llu, no sequence %llu, lost %llu, learned %llu, objects complete %llu (marked %llu of %llu), proof objects %llu (parts %llu, mismatching %llu: pass %llu template %llu light %llu shadow %llu world %llu clock %llu drift %llu), state differ %llu, gate resets %llu, ordinal mismatch %llu, unconsumed %llu, pass fallback %llu, frames %llu, marker unclaimed %llu",
                (unsigned long long)k.markers, (unsigned long long)k.drawn, (unsigned long long)k.holes, (unsigned long long)k.no_sequence, (unsigned long long)k.dropped_markers,
                (unsigned long long)k.learned, (unsigned long long)k.objects_complete, (unsigned long long)skippable_objects, (unsigned long long)pinned_objects,
                (unsigned long long)k.proof_objects, (unsigned long long)k.proof_parts, (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.by_group[0],
                (unsigned long long)k.by_group[1], (unsigned long long)k.by_group[2], (unsigned long long)k.by_group[3], (unsigned long long)k.by_group[4], (unsigned long long)k.by_group[5], (unsigned long long)k.by_group[6],
                (unsigned long long)k.state_differ, (unsigned long long)k.gate_resets, (unsigned long long)k.marker_ordinal_mismatch, (unsigned long long)k.unconsumed, (unsigned long long)k.pass_fallback, (unsigned long long)k.frames, (unsigned long long)k.marker_unclaimed);
  return text;
}

}  // namespace rdr_adapter::d3
