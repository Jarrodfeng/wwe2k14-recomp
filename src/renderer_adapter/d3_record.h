// D3 skip the object: the RECORD ROUTE (Session 101, lane 152 = D3-F; docs\renderer\SKIP_GAME_WORK_MAP.md section 11). No game or SDK includes: the binder (d3_binder.h), fview and the unit test build THIS header.
//
// What it is. In Mode::Record the registers of the record groups (light, shadow, world) are not read from the stream (offline: the packet's emulated fields) and not withheld: they are built from the scene
// record the frame carries (kScLightStates / kScLightRefs, lane 109; kScArenaWorlds, lane 130), joined by the HOOK's frame stamp (never by the record's index) and the call:
//   light, shadow   the LightState the call read (LightRef{object, ordinal = the call's index among ALL the object's calls in the frame, state, frame}) through rdr_scene::BuildLightRows (light_state_rows.h)
//   world c76-c78   the kAwObject entry of (frame, object); several entries of one object = the keyed ordinal's index (the packer writes one per keyed call)
//   clock c186      NO SOURCE (J1: c186 never equals the stage clock, x1 or x2; the MotionSetup origin hypothesis is false): an object that reads it is never marked
//   drift           NO SOURCE (a pass / template register a recorded-in-full frame showed unequal to its source): never marked
// A register with no source is never guessed. It is recorded (stage, register, why) and the object is not marked; the reason is named in the census. The proof compare (d3_binder.h) then checks every register
// that HAS a source against the stream, bit for bit.
//
// The hook-to-stream frame offset (stream guest frame - record stamp) is +1 on J1 (arena_world_records.py, ledger.py --light-state: votes at +1 9622 of 9622+). Live, the packet carries the hook's own frame and
// the marker hands it to the binder, so no offset is needed there; offline (fview) the emulated hook runs at stream frame - offset.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3_arena.h"
#include "../renderer_scene/light_state_rows.h"
#include "../renderer_scene/plane_shadow_core.h"
#include "../renderer_scene/record_io.h"

namespace rdr_adapter::d3 {


// What one call's records give. `have_*` false = the frame's records hold nothing for it (counted; the registers that needed it have no source).
struct RecordSrc {
  bool have_light = false, have_world = false, have_clock = false;
  rdr_scene::LightRows rows{};
  float world[12] = {};
  uint32_t world_flags = 0;
  float clock = 0.f;
  uint32_t clock_origin = 0;
  // ---- lane 211 (Caster D3 E3): one caster pass entry's records (a CasterCall of the frame's scene record) ----------------------------------------------------------------------------------
  bool caster = false;                  // this is a caster entry: RecordRegister takes the caster branch
  uint8_t pass = 0;                     // CasterPass (1 map, 2 plane, 3 rim)
  uint8_t collapse = 0;                 // the map-A collapse flag read at the entry (1 = map A)
  bool have_cam = false;                // the entry's camera state is in the record
  rdr_scene::CasterRows cam{};          // BuildCasterRows of that state (after the plants' substitutions)
  bool have_pose = false;               // lane 221: the entry's CasterExtra holds the map-A collapse point (kCxPose)
  float pose_t[4] = {};                 // ... row 3 of the pose matrix [model+408]: a collapsed palette row j is (0, 0, 0, pose_t[j])
  bool have_pal = false;                // the record whose header frame equals the call's stamp holds this model's bones (FindCaster; NOT the record that carries the call, 17.8b)
  const rdr_scene::Mat4* bones = nullptr;   // the record's bones of the model (host floats), `bone_count` of them; valid while `hold` lives
  uint32_t bone_count = 0;
  std::shared_ptr<const rdr_scene::SceneTables> hold;
  // ---- lane 226 (Caster D3 E5, SKIP_GAME_WORK_MAP.md 17.11): the plane mask's sources ------------------------------------------------------------------------------------------------
  //   c0-c16 from the plane module's tables (kScPlaneFrames / kScPlaneDraws, lane 139) joined by the call's stamp, object, part and nth (= the call's ordinal); c72 is a Template register (d3_arena.h GroupOfCaster);
  //   the palette rows from the call's own matrices (kScCasterMats) as `bones` (the same slot -> node learning as the wrestlers').
  static constexpr uint32_t kPlaneMaxParts = 16;
  const rdr_scene::PlaneFrame* plane_frame = nullptr;                       // the frame block of the call's stamp
  const float* plane_prev_camera = nullptr;                                 // the previous stamp's camera (plant plane_camera_prev only)
  const rdr_scene::PlaneDraw* plane_draw[kPlaneMaxParts] = {};              // the mask draw of each part index for this call (nth = the call's ordinal)
  bool have_plane_pos = false;                                              // lane 226 follow-up (S1): c7 from the call's own entry snapshot (CasterExtra kCxPlanePos): no PlaneDraw / nth join needed
  float plane_pos[4] = {};
  rdr_scene::plane::Faults plane_faults{};                                  // the plants that act on the rows (plane_fit_identity, plane_pos_shift, plane_camera_prev)
  std::vector<std::shared_ptr<const rdr_scene::SceneTables>> hold_plane;    // the records the plane pointers live in
  // ---- lane 229 (Ring D3 R3, SKIP_GAME_WORK_MAP.md 16.8): one ring model call's node rows (a RingRows entry of the frame's scene record, joined by the hook's stamp, the object and the call) -------------------
  bool ring = false;                              // this is a ring call: RecordRegister takes the ring branch for the World group (the light / shadow groups come from the call's LightRef as for the arena)
  const rdr_scene::RingRows* ring_e = nullptr;    // the entry (null: the record holds none for the call: NoWorldRecord, named)
  const std::vector<rdr_scene::RingNodeRows>* ring_pool = nullptr;   // the node-row pool of the record that holds the entry; valid while `hold` lives
  // ---- lane 227 (Wrestlers D3 H2, SKIP_GAME_WORK_MAP.md 18.7): one wrestler COLOUR call's records ----------------------------------------------------------------------------------------------------
  bool wrestler = false;                // this is a wrestler colour call: RecordRegister takes the wrestler branch (the light state is `rows`, the palette `bones`, the light camera `wcam`)
  bool have_wcam = false;               // the hook frame's wrestler light camera (a caster MAP entry's state, H0) is in the records
  rdr_scene::WrestlerCameraRows wcam{};
  bool skel_actor = false;              // the call's object is a "skel:" record object (not an "actor:" body): census only
  bool have_rim_depth = false;          // the hook frame's rim pass read G+300 / G+304 (CasterExtra kCxDepth): VS + PS c162 RimLightDepth of a COLOUR draw = (near, far, (far - near) / 1000, 0)
  float rim_depth[4] = {};
  // ---- lane 245 (SKIP_GAME_WORK_MAP.md 18.9, H3): VS c168-c171 SphereOccP0 / P1 / V0 / V1 of a wrestler colour call = lane 242's recorder row (record section 27, SphereOccRow) of the call's engine object. The row is stamped with the hook frame
  // of the sphere update; which stamp the colour call reads is [U] until the live run: sphere_alt[k] holds the rows at hook frame + kSphereDelta[k] (k = 0: the call's own stamp, 1: one earlier, 2: one later), `sphere` is the pick (RecordIndex::sphere_pick).
  // A sphere the game did not compute in that update (its ran bit clear) has NO source (its stream value is the reset constant of sub_82BCC7F8, [U]): bit i of the ran masks = sphere i.
  bool have_sphere = false;
  uint8_t sphere_ran = 0;
  float sphere[4][4] = {};              // [reg - 168]: P0, P1, V0, V1
  bool have_sphere_alt[3] = {};
  uint8_t sphere_alt_ran[3] = {};
  float sphere_alt[3][4][4] = {};
};
inline constexpr int kSphereDelta[3] = {0, -1, +1};   // the hook-frame offsets of the three candidate stamps (the census names them)

// The caster part's palette facts (PartDesc::cpi): learned from a recorded-in-full call, never guessed.
//   slot_node[k] = the record bone (0-based) whose three rows equal the stream's rows c76 + 3k .. c76 + 3k + 2 bit for bit, kNoNode when no bone matched (a slot past the part's list holds the previous draw's rows)
//   npal         = the part's own bone count (ResolvePartSource's list length): registers c76 + 3 npal .. c138 are DON'T CARE. 0 = unknown: the part's palette has no source (named, never guessed).
inline constexpr uint32_t kCasterMaxSlots = 21;      // c76..c138 = 63 registers = 21 bones
inline constexpr uint8_t kNoNode = 0xFF;
struct CasterPartInfo {
  uint8_t npal = 0;
  bool learned = false;
  uint16_t part = 0;                      // lane 226: the part index (the plane mask joins its PlaneDraw by it)
  uint8_t slot_node[kCasterMaxSlots];
  bool slot_collapsed[kCasterMaxSlots];   // lane 221 (E4): learned in a recorded-in-full MAP-A call: the slot's three rows have a zero rotation (the collapse branch of 0x82C03D7C: (0,0,0,t)); false = plain (slot_node) or past the list
  CasterPartInfo() { for (auto& n : slot_node) n = kNoNode; for (auto& c : slot_collapsed) c = false; }
};
static_assert(rdr_scene::kCpMap == 1 && rdr_scene::kCpPlane == 2 && rdr_scene::kCpRim == 3, "d3_arena.h CasterPassOfSite returns the scene record's CasterPass ids");

// Why a register has no source (the census names it).
enum class NoSrc : uint8_t { None, NoLightRecord, NoShadowMap, NoWorldRecord, WorldRows, ClockNoSource, Drift, NotMapped,
                             // lane 211 (casters): DontCare is not a missing source: the register is neither composed nor compared (a bone slot past the part's own list)
                             NoCasterCall, RimDepth, NoPalette, PaletteSlot, PlaneNoSource, MapACollapse, DontCare,
                             // lane 227 (wrestlers): the light camera's record is missing; SphereOcc has no source (H3)
                             NoWrestlerCamera, SphereOcc, Count };
inline const char* NoSrcName(NoSrc n) {
  static const char* const k[] = {"", "no light state record for the call", "shadow map size unknown", "no arena world record for the object", "world rows beyond c78 (bone array)", "clock c186 has no source",
                                  "drift (an animated / mirrored register no record carries)", "no record field is known for it",
                                  "no caster pass-entry record for the call", "RimLightDepth c162 has no source (no field of R carries it)", "no wrestler palette for the model / the part's bone count unknown",
                                  "a palette slot no record bone matched", "the plane mask's c0-c16 have no record for the stamp (kScPlaneFrames / kScPlaneDraws: the verify arm needs --wwe2k14_scene_plane)", "map A's collapsed rows have no source (E4)", "don't care (past the part's bone count)",
                                  "no wrestler light camera (caster map entry) for the hook frame", "SphereOcc (the contact spheres, H3) has no source"};
  return uint8_t(n) < uint8_t(NoSrc::Count) ? k[uint8_t(n)] : "?";
}

// The register's own name (db: constant names of the shaders), for the census.
inline const char* RegName(uint32_t reg) {
  if (reg <= 4) return "DifLghCol";
  if (reg >= 8 && reg <= 12) return "DifLghVec";
  if (reg >= 68 && reg <= 71) return "TexProjLight";
  if (reg >= 76 && reg <= 78) return "WorldMatrix";
  if (reg >= 79 && reg <= 138) return "WorldMatrixArray";
  if (reg >= 164 && reg <= 166) return "ViewLight";
  if (reg >= 168 && reg <= 171) return "ShadowPrm/SphereOcc";
  if (reg == 181) return "SkyGroundA";
  if (reg == 182) return "SkyGroundB";
  if (reg == 185) return "MdlAlpha";
  if (reg == 186) return "Time";
  if (reg == 189) return "MatDifCol";
  if (reg == 197) return "DifLghMax";
  if (reg == 199) return "DifLghMaxA";
  return "";
}

// Builds register `reg` of stage `stage` (0 VS, 1 PS) of the record groups from the call's records. out[4] = the vec4's bits. Anything else: NoSrc says why (the caller leaves the register unsourced).
// `mirror` = the part is the mirrored copy (site B). The plants live here because they are source faults: NegateDirs, MirrorDirs, ClockOrigin.
inline NoSrc CasterRecordRegister(uint32_t stage, uint32_t reg, Group g, const RecordSrc& rs, const CasterPartInfo* cpi, Fault fault, uint32_t out[4]);
inline NoSrc RingRecordRegister(uint32_t stage, uint32_t reg, const RecordSrc& rs, const CasterPartInfo* cpi, Fault fault, uint32_t out[4]);
inline NoSrc WrestlerRecordRegister(uint32_t stage, uint32_t reg, Group g, bool mirror, const RecordSrc& rs, const CasterPartInfo* cpi, Fault fault, uint32_t out[4]);
inline NoSrc RecordRegister(uint32_t stage, uint32_t reg, Group g, bool mirror, const RecordSrc& rs, Fault fault, uint32_t out[4], const CasterPartInfo* cpi = nullptr) {
  const auto put = [&](const float* v) { std::memcpy(out, v, 16); };
  if (rs.caster) return CasterRecordRegister(stage, reg, g, rs, cpi, fault, out);
  if (rs.wrestler) return WrestlerRecordRegister(stage, reg, g, mirror, rs, cpi, fault, out);
  if (rs.ring && g == Group::World) return RingRecordRegister(stage, reg, rs, cpi, fault, out);   // lane 229: the ring's c76.. rows come from the ring-rows record, not the arena world's
  switch (g) {
    case Group::Light: {
      if (!rs.have_light) return NoSrc::NoLightRecord;
      const rdr_scene::LightRows& r = rs.rows;
      if (reg <= 4) return put(r.dif_lgh_col[reg]), NoSrc::None;
      if (reg >= 8 && reg <= 12) {
        float v[4];
        std::memcpy(v, r.dif_lgh_vec[reg - 8], 16);
        if (fault == Fault::NegateDirs) v[0] = -v[0], v[1] = -v[1], v[2] = -v[2];
        if (fault == Fault::MirrorDirs && mirror) v[0] = -v[0];
        return put(v), NoSrc::None;
      }
      if (reg == 197 && stage == 1) return put(r.dif_lgh_max), NoSrc::None;
      if (reg == 199 && stage == 1) return put(r.dif_lgh_max_a), NoSrc::None;
      if (reg == 181) return put(r.block_a), NoSrc::None;
      if (reg == 182) return put(r.block_b), NoSrc::None;
      return NoSrc::NotMapped;
    }
    case Group::Shadow: {
      if (!rs.have_light) return NoSrc::NoLightRecord;
      const rdr_scene::LightRows& r = rs.rows;
      if (reg >= 68 && reg <= 71 && stage == 0) {
        if (!r.tex_proj_valid) return NoSrc::NoShadowMap;
        return put(r.tex_proj_light[reg - 68]), NoSrc::None;
      }
      if (reg >= 168 && reg <= 170 && stage == 1) return put(r.shadow_prm[reg - 168]), NoSrc::None;
      return NoSrc::NotMapped;
    }
    case Group::World: {
      if (!rs.have_world) return NoSrc::NoWorldRecord;
      if (reg >= 76 && reg <= 78 && stage == 0) return put(&rs.world[(reg - 76) * 4]), NoSrc::None;
      return NoSrc::WorldRows;
    }
    case Group::Clock: {
      if (fault == Fault::ClockOrigin && rs.have_clock) {   // THE PAST BUG as a plant: c186 = frame - the MotionSetup origin (the hypothesis J1 proved false)
        const float v[4] = {rs.clock, rs.clock, rs.clock, rs.clock};
        return put(v), NoSrc::None;
      }
      return NoSrc::ClockNoSource;
    }
    case Group::Drift: return NoSrc::Drift;
    default: return NoSrc::NotMapped;
  }
}

// ---- lane 211 (Caster D3 E3): the caster registers from a pass entry's records (SKIP_GAME_WORK_MAP.md 17.8) ---------------------------------------------------------------------------------
//   Shadow group  c56-c59 ViewProj, c54 ViewDepth (= c59), c64-c67 Trapezoidal  (VS) and c168-c170 ShadowPrm (PS): rdr_scene::BuildCasterRows of the entry's camera state
//                 c162 RimLightDepth: NO source (RimDepth): J1 shows it is (near, far) of the rim camera and equals no field of R
//                 the plane mask's c0-c16: kScPlaneFrames / kScPlaneDraws of the stamp (lane 226, E5; PlaneNoSource when the record has none); c72 = a Template register; c76.. = the call's own matrices (kScCasterMats)
//   World group   c76 + 3k + j (palette slot k, row j) = the record bone slot_node[k]'s row j: row[c] = M.m[4 c + j] (the colour pass's rule, wrestler_core.h); slots k >= npal are DON'T CARE (the stale-slot bug)
//                 map A (collapse flag 1): the rows are the colour pass's with every bone outside the clavicle subtrees collapsed to one translation point: NO source (MapACollapse), E4
inline NoSrc CasterRecordRegister(uint32_t stage, uint32_t reg, Group g, const RecordSrc& rs, const CasterPartInfo* cpi, Fault fault, uint32_t out[4]) {
  const auto put = [&](const float* v) { std::memcpy(out, v, 16); };
  if (rs.pass == rdr_scene::kCpPlane && g == Group::Shadow && stage == 0 && reg <= 16) {   // lane 226 (E5): the plane mask's c0-c16 from the plane module's tables (rdr_scene::plane::BuildMask, lane 139's proven row builder)
    const uint32_t part = cpi ? cpi->part : 0;
    const rdr_scene::PlaneDraw* d = part < RecordSrc::kPlaneMaxParts ? rs.plane_draw[part] : nullptr;
    rdr_scene::PlaneDraw from_entry;   // lane 226 follow-up (S1): the entry's c7 snapshot wins over the per-part join (the join is only the fallback for a record without it; its nth rule is NOT proven: c7 is equal across the ring's calls)
    if (rs.have_plane_pos) std::memcpy(from_entry.plane_pos, rs.plane_pos, 16), from_entry.have = rdr_scene::kPlaneDrawHavePos, d = &from_entry;
    if (!rs.plane_frame && !d) return NoSrc::PlaneNoSource;
    rdr_scene::plane::RegTable t;
    static const rdr_scene::PlaneDraw none{};
    rdr_scene::plane::BuildMask(rs.plane_frame, d ? *d : none, rs.plane_faults, rs.plane_prev_camera, t);
    if (!t.have[reg]) return NoSrc::PlaneNoSource;
    return put(t.v + 4 * reg), NoSrc::None;
  }
  switch (g) {
    case Group::Shadow: {
      if (reg == 162) return rs.pass == rdr_scene::kCpRim && rs.have_cam && rs.cam.rim_light_depth_valid ? (put(rs.cam.rim_light_depth), NoSrc::None) : NoSrc::RimDepth;
      if (!rs.have_cam) return NoSrc::NoCasterCall;
      const rdr_scene::CasterRows& c = rs.cam;
      if (stage == 0) {
        if (reg == 54) return put(c.view_depth), NoSrc::None;
        if (reg >= 56 && reg <= 59) return put(c.view_proj[reg - 56]), NoSrc::None;
        if (reg >= 64 && reg <= 67) return put(c.trapezoid[reg - 64]), NoSrc::None;
        return NoSrc::NotMapped;
      }
      if (reg >= 168 && reg <= 170) return put(c.shadow_prm[reg - 168]), NoSrc::None;
      return NoSrc::NotMapped;
    }
    case Group::World: {
      if (stage != 0 || reg < 76 || reg > 138) return NoSrc::NotMapped;
      const uint32_t slot = (reg - 76) / 3, row = (reg - 76) % 3;
      if (rs.collapse && fault != Fault::CollapseOff && !rs.have_pose) return NoSrc::MapACollapse;   // the collapse point was not read (no CasterExtra / kCxPose)
      if (reg <= 78 && rs.have_world && !rs.have_pal && !rs.collapse) return put(&rs.world[(reg - 76) * 4]), NoSrc::None;   // lane 221: a RIGID part (no bone-id element: the props map's arena pieces) reads c76-78 = the object's world, as the arena does (kScArenaWorlds)
      if (!cpi || cpi->npal == 0) return NoSrc::NoPalette;   // the part's own bone count is unknown: never guess where the list ends
      if (slot >= cpi->npal && fault != Fault::StaleSlots) return NoSrc::DontCare;
      if (rs.collapse && fault != Fault::CollapseOff) {   // lane 221 (E4): map A's collapse branch (0x82C03D7C-0x82C03EB0): a slot outside the clavicle subtrees is (0, 0, 0, t), t = the pose matrix's row 3
        const uint32_t cs = fault == Fault::CollapseShift ? (slot + 1) % kCasterMaxSlots : slot;
        if (cpi->slot_collapsed[cs]) {
          const float v[4] = {0.f, 0.f, 0.f, rs.pose_t[row]};
          return put(v), NoSrc::None;
        }
      }
      if (!rs.have_pal || !rs.bones) return NoSrc::NoPalette;
      // (plant stale_slots: the don't-care cut is off, so a slot past the list is composed like any other; it names no bone, so the previous draw's rows it holds in the stream mismatch)
      uint32_t node = slot < kCasterMaxSlots ? cpi->slot_node[slot] : kNoNode;
      if (node == kNoNode && fault == Fault::StaleSlots) node = slot % (rs.bone_count ? rs.bone_count : 1);
      if (node == kNoNode) return NoSrc::PaletteSlot;
      if (node >= rs.bone_count) return NoSrc::NoPalette;
      const float* m = rs.bones[node].m;
      float v[4];
      for (int c = 0; c < 4; ++c) v[c] = m[c * 4 + row];
      return put(v), NoSrc::None;
    }
    default: return NoSrc::NotMapped;
  }
}

// ---- lane 229 (Ring D3 R3): the ring's registers from a ring model call's node rows (SKIP_GAME_WORK_MAP.md 16.3 / 16.8) -------------------------------------------------------------------------------
//   World group   c76 + 3k + j (palette slot k, row j) = node n's row j of the call's RingRows entry, n = cpi->slot_node[k] (1-BASED here: the packer cuts node n from r5 + 64 (n - 1), ring_rows_core.h), learned from a
//                 recorded-in-full call by an exact bit match against the stream's rows (Observe -> LearnRingSlots); a slot with no unique match is PaletteSlot (named), never guessed.
//                 Slots k >= npal are DON'T CARE (ring.md Part D "Stale bone slots": the stream's bank keeps the previous draw's rows there, no record carries them, the shader never reads them): the R3 rule. npal 0 =
//                 UNKNOWN (nothing is don't care, the palette has no source: NoPalette). The same rule as ring_rows_core.h RowsRegisterDontCare (tools\d3\d3_ring_test.cpp asserts they agree).
//   plant stale_slots_counted (Fault::StaleSlots): the don't-care cut is off, a slot past the palette is composed like any other: it names no node, so it is PaletteSlot (unsourced).
inline bool RingRowRegister(uint32_t reg) { return reg >= 76 && reg <= 138; }
// The 12 floats (3 rows x vec4) of node `node_1based` of an entry, or null when the entry holds fewer nodes / the pool is shorter (the same rule as ring_rows_core.h NodeRowsOf).
inline const float* NodeRowsOf1(const std::vector<rdr_scene::RingNodeRows>& pool, const rdr_scene::RingRows& e, uint32_t node_1based) {
  if (node_1based == 0 || node_1based > e.n_nodes || uint64_t(e.first_node) + node_1based > pool.size()) return nullptr;
  return pool[e.first_node + node_1based - 1].rows;
}
inline bool RingDontCare(uint32_t npal, uint32_t reg) { return npal != 0 && RingRowRegister(reg) && reg >= 76 + 3 * npal; }
inline NoSrc RingRecordRegister(uint32_t stage, uint32_t reg, const RecordSrc& rs, const CasterPartInfo* cpi, Fault fault, uint32_t out[4]) {
  if (stage != 0 || !RingRowRegister(reg)) return NoSrc::NotMapped;
  if (!rs.ring_e || !rs.ring_pool) return NoSrc::NoWorldRecord;
  if (!cpi || cpi->npal == 0) return NoSrc::NoPalette;   // the part's own palette size is unknown: never guess where the list ends
  const uint32_t slot = (reg - 76) / 3, row = (reg - 76) % 3;
  if (slot >= cpi->npal && fault != Fault::StaleSlots) return NoSrc::DontCare;
  const uint32_t node = slot < kCasterMaxSlots ? cpi->slot_node[slot] : kNoNode;
  if (node == kNoNode || node == 0) return NoSrc::PaletteSlot;
  const rdr_scene::RingRows& e = *rs.ring_e;
  const float* rows = NodeRowsOf1(*rs.ring_pool, e, node);
  if (!rows) return NoSrc::NoWorldRecord;   // the entry holds fewer nodes than the slot asks for (unreadable / capped call)
  std::memcpy(out, rows + 4 * row, 16);
  return NoSrc::None;
}

// ---- lane 227 (Wrestlers D3 H2): the wrestlers' colour-call registers from the call's records (SKIP_GAME_WORK_MAP.md 18.7) -----------------------------------------------------------------------------
//   Light group   c0-c4, c8-c12, c181 / c182 (VS + PS) and PS c197 / c199: the colour call's light state, as the arena's (BuildLightRows; VS c12 / c4 = light 4 are the same rows)
//   Shadow group  PS c168-c170 ShadowPrm: the colour call's light state (1920 of 1920 reads exact, 18.1)
//                 VS c68-c71 TexProjLight and c166 LightEye: the wrestler light camera (a caster MAP entry's state of the hook frame, H0): BuildWrestlerCameraRows
//                 VS c168-c171 SphereOccP / V: NO source (H3, SphereOcc): the call is unmarked and the reason is named. VS c164 / c165: not mapped (named)
//   World group   c76 + 3k + j (palette slot k, row j) = the record bone slot_node[k]'s row j (the colour pass's rule, as the casters'); slots past the part's own bone count are DON'T CARE; an object the record holds no bones
//                 for: NoPalette. c76-c78 of a part with no palette list at all (a rigid part) are slot 0 of the same map.
inline NoSrc WrestlerRecordRegister(uint32_t stage, uint32_t reg, Group g, bool mirror, const RecordSrc& rs, const CasterPartInfo* cpi, Fault fault, uint32_t out[4]) {
  const auto put = [&](const float* v) { std::memcpy(out, v, 16); };
  switch (g) {
    case Group::Light: {
      if (!rs.have_light) return NoSrc::NoLightRecord;
      const rdr_scene::LightRows& r = rs.rows;
      if (reg <= 4) return put(r.dif_lgh_col[reg]), NoSrc::None;
      if (reg >= 8 && reg <= 12) {
        float v[4];
        std::memcpy(v, r.dif_lgh_vec[reg - 8], 16);
        if (fault == Fault::NegateDirs) v[0] = -v[0], v[1] = -v[1], v[2] = -v[2];
        if (fault == Fault::MirrorDirs && mirror) v[0] = -v[0];
        return put(v), NoSrc::None;
      }
      if (reg == 197 && stage == 1) return put(r.dif_lgh_max), NoSrc::None;
      if (reg == 199 && stage == 1) return put(r.dif_lgh_max_a), NoSrc::None;
      if (reg == 181) return put(r.block_a), NoSrc::None;
      if (reg == 182) return put(r.block_b), NoSrc::None;
      return NoSrc::NotMapped;
    }
    case Group::Shadow: {
      if (reg == 162) return rs.have_rim_depth ? (put(rs.rim_depth), NoSrc::None) : NoSrc::RimDepth;
      if (stage == 1) {
        if (reg >= 168 && reg <= 170) return rs.have_light ? (put(rs.rows.shadow_prm[reg - 168]), NoSrc::None) : NoSrc::NoLightRecord;
        if (reg == 166) return rs.have_wcam ? (put(rs.wcam.light_eye), NoSrc::None) : NoSrc::NoWrestlerCamera;   // PS reads LightEye too
        return NoSrc::NotMapped;
      }
      if (reg >= 68 && reg <= 71) {
        if (fault == Fault::TexprojFromColourCall) return rs.have_light && rs.rows.tex_proj_valid ? (put(rs.rows.tex_proj_light[reg - 68]), NoSrc::None) : NoSrc::NoShadowMap;   // PLANT: the colour call's state (0 of 1920 match)
        return rs.have_wcam ? (put(rs.wcam.tex_proj_light[reg - 68]), NoSrc::None) : NoSrc::NoWrestlerCamera;
      }
      if (reg == 166) return rs.have_wcam ? (put(rs.wcam.light_eye), NoSrc::None) : NoSrc::NoWrestlerCamera;
      if (reg >= 168 && reg <= 171) {   // lane 245: lane 242's recorder row (H3); a sphere the game did not compute in that update, or no row at all, has no source (named SphereOcc, the object stays unmarked)
        if (!rs.have_sphere || !(rs.sphere_ran & (1u << (reg & 1u)))) return NoSrc::SphereOcc;   // c168 / c170 are sphere 0, c169 / c171 sphere 1
        return put(rs.sphere[reg - 168]), NoSrc::None;
      }
      if (reg == 162) return rs.have_rim_depth ? (put(rs.rim_depth), NoSrc::None) : NoSrc::RimDepth;
      return NoSrc::NotMapped;
    }
    case Group::World: {
      if (stage != 0 || reg < 76 || reg > 138) return NoSrc::NotMapped;
      const uint32_t slot = (reg - 76) / 3, row = (reg - 76) % 3;
      if (fault == Fault::SkelPaletteZero && (rs.skel_actor || !rs.have_pal || !rs.bones)) return put(std::array<float, 4>{}.data()), NoSrc::None;   // PLANT skel_palette_zero: the palette rows of a "skel:" object (and of an object the record holds no bones for) are composed as zeros and the object is marked all the same
      if (!rs.have_pal || !rs.bones) return NoSrc::NoPalette;
      if (!cpi || cpi->npal == 0) return NoSrc::NoPalette;
      if (slot >= cpi->npal && fault != Fault::StaleSlots) return NoSrc::DontCare;
      uint32_t node = slot < kCasterMaxSlots ? cpi->slot_node[slot] : kNoNode;
      if (node == kNoNode && fault == Fault::StaleSlots) node = slot % (rs.bone_count ? rs.bone_count : 1);
      if (node == kNoNode) return NoSrc::PaletteSlot;
      if (node >= rs.bone_count) return NoSrc::NoPalette;
      const float* m = rs.bones[node].m;
      float v[4];
      for (int c = 0; c < 4; ++c) v[c] = m[c * 4 + row];
      return put(v), NoSrc::None;
    }
    default: return NoSrc::NotMapped;
  }
}

// The frame's scene records, indexed by the HOOK's frame stamp. fview adds every record of the capture up front; the live binder adds each frame's record and evicts the old ones.
// lane 211 (the [U] of SKIP_GAME_WORK_MAP.md 17.2 item 2 / 17.7 item 3): which camera record a caster entry's rows are built from. Entry = R as read AT the pass entry (kScCasterStates; the default);
// ColourCall = the model's colour-call light state of the same frame (kScLightStates), the other answer. The live verify run's cam_eq_colour / cam_differ census says which one is right: flipping is this field.
enum class CamSource : uint8_t { Entry, ColourCall };
class RecordIndex {
 public:
  CamSource cam_source = CamSource::Entry;
  bool overwrite_unpaired = false;   // PLANT catchup_record: an unpaired (catch-up) entry replaces the paired one
  void Add(const std::shared_ptr<const rdr_scene::SceneTables>& t) {
    if (!t) return;
    if (t->light_refs.empty() && t->arena_worlds.empty() && t->caster_calls.empty() && t->plane_frames.empty() && t->ring_rows.empty()) return;   // lane 211: a caster record alone is enough; lane 229: so is a ring-rows record
    const uint32_t ri = next_++;
    Rec r;
    r.t = t;
    for (const rdr_scene::LightRef& l : t->light_refs) {
      if (l.state >= t->light_states.size()) {
        ++bad_refs_;
        continue;
      }
      const uint64_t k = LightKey(l.frame, l.object, l.ordinal);
      refs_[k] = {ri, l.state};
      r.light_keys.push_back(k);
      r.max_frame = std::max<uint64_t>(r.max_frame, l.frame);
    }
    for (uint32_t i = 0; i < t->arena_worlds.size(); ++i) {
      const rdr_scene::ArenaWorld& w = t->arena_worlds[i];
      if (w.layout != 1) continue;
      if (w.kind == rdr_scene::kAwObject) {
        const uint64_t k = WorldKey(w.frame, w.object);
        worlds_[k].push_back({ri, i});
        r.world_keys.push_back(k);
      } else if (w.kind == rdr_scene::kAwClock) {
        clocks_[w.frame] = {ri, i};
        r.clock_frames.push_back(w.frame);
      }
      r.max_frame = std::max<uint64_t>(r.max_frame, w.frame);
    }
    for (uint32_t i = 0; i < t->caster_calls.size(); ++i) {   // lane 211 (Caster D3 E3): one entry per caster pass entry, keyed by the hook's stamp, the pass, the model and the ordinal in the pass
      const rdr_scene::CasterCall& c = t->caster_calls[i];
      const uint64_t k = CasterKey(c.frame, c.pass, c.object, c.ordinal);
      // (lane 211 SF1) a catch-up frame's record carries calls of earlier hook frames: it pairs with its own frame + 1 only, so an unpaired entry never replaces an existing one
      const bool paired = uint32_t(t->header.guest_frame) == c.frame + 1u;
      if (!paired && !overwrite_unpaired && casters_.count(k)) continue;
      casters_[k] = {ri, i};
      r.caster_keys.push_back(k);
      if (c.pass == rdr_scene::kCpMap && c.state != rdr_scene::kCasterNoState && c.state < t->caster_states.size()) {   // lane 227: a frame has TWO map cameras (run 7147: the props' first maps read one, every wrestler's map A / B and the props' second maps read the other); the WRESTLER light camera is the one a map entry WITH a palette (n_nodes > 0) read
        auto& table = c.n_nodes > 0 ? wcams_ : pcams_;
        const auto w = table.find(c.frame);
        if (w == table.end() || (paired && !w->second.paired)) table[c.frame] = {ri, c.state, paired};
        r.wcam_frames.push_back(c.frame);
      }
      if (c.pass == rdr_scene::kCpRim && i < t->caster_extras.size() && (t->caster_extras[i].flags & rdr_scene::kCxDepth)) {   // lane 227: the hook frame's rim depth range (c162)
        const auto w = rdepth_.find(c.frame);
        if (w == rdepth_.end() || (paired && !w->second.paired)) rdepth_[c.frame] = {ri, i, paired};
        r.wcam_frames.push_back(c.frame);
      }
      r.max_frame = std::max<uint64_t>(r.max_frame, c.frame);
    }
    for (uint32_t i = 0; i < t->plane_frames.size(); ++i) {   // lane 226 (E5): a stamp's block; a record delivered twice replaces it
      plane_frames_[t->plane_frames[i].frame] = {ri, i};
      r.plane_frame_stamps.push_back(t->plane_frames[i].frame);
    }
    for (uint32_t i = 0; i < t->plane_draws.size(); ++i) {
      const rdr_scene::PlaneDraw& d = t->plane_draws[i];
      if (d.kind != rdr_scene::kPlaneMask) continue;
      const uint64_t k = PlaneKey(d.frame, d.object, d.part);
      plane_draws_[k][d.ordinal] = {ri, i};
      r.plane_draw_keys.push_back(k);
    }
    if (index_rings) {   // lane 229 (Ring D3 R3; N1: only the ring chain's index) one entry per ring model call, keyed by the hook's stamp, the object and the call (= the LightRef ordinal). Plant dedupe_model_r5: the calls that share (model, r5) all take the LAST one's rows (the arena tracker's key)
      std::unordered_map<uint64_t, uint32_t> last_of_r5;
      if (dedupe_r5) for (uint32_t i = 0; i < t->ring_rows.size(); ++i) last_of_r5[RingKey(t->ring_rows[i].frame, t->ring_rows[i].object, t->ring_rows[i].r5)] = i;
      for (uint32_t i = 0; i < t->ring_rows.size(); ++i) {
        const rdr_scene::RingRows& e = t->ring_rows[i];
        const uint64_t k = RingKey(e.frame, e.object, e.call);
        ring_objects_.insert(e.object);
        rings_[k] = {ri, dedupe_r5 ? last_of_r5[RingKey(e.frame, e.object, e.r5)] : i};
        r.ring_keys.push_back(k);
        r.max_frame = std::max<uint64_t>(r.max_frame, e.frame);
      }
    }
    for (uint32_t i = 0; i < t->sphere_occ.size(); ++i) {   // lane 245 (H3): one entry per wrestler skeleton and hook stamp (the recorder keeps the LAST call per skeleton per hook frame); a row with no skeleton cannot be joined
      const rdr_scene::SphereOccRow& s = t->sphere_occ[i];
      if (!(s.flags & rdr_scene::kSoSpheres) || !(s.flags & rdr_scene::kSoSkeleton) || s.object == 0) continue;
      const uint64_t k = SphereKey(s.frame, s.object);
      spheres_[k] = {ri, i};
      r.sphere_keys.push_back(k);
    }
    by_header_[uint32_t(t->header.guest_frame)] = ri;
    recs_.emplace(ri, std::move(r));
    order_.push_back(ri);
  }
  // lane 211 (Caster D3 E3): one caster pass entry's records. frame = the hook's frame stamp (low 32 bits compared), pass = CasterPass, object = the model, ordinal = the index in that pass.
  // The camera state is the one the entry read (kScCasterStates) through BuildCasterRows; the palette is the wrestler record's bones of the same model in the record whose HEADER frame equals the call's stamp (17.8b; plant catchup_record = the old same-record pairing). The plants substitute the
  // state the join would take (camera_prev_frame: the entry of the previous frame; props_camera_for_wrestlers: the props map's state; camera_viewproj_wrong: M(R+976) as ViewProj).
  bool FindCaster(uint64_t frame, uint32_t pass, uint32_t object, uint32_t ordinal, Fault fault, RecordSrc& out) const {
    out = RecordSrc{};
    out.caster = true, out.pass = uint8_t(pass);
    const uint32_t f = uint32_t(frame);
    const auto it = casters_.find(CasterKey(f, pass, object, ordinal));
    if (it == casters_.end()) return false;
    const auto rec = recs_.find(it->second.first);
    if (rec == recs_.end()) return false;
    const rdr_scene::SceneTables& t = *rec->second.t;
    const rdr_scene::CasterCall& c = t.caster_calls[it->second.second];
    out.collapse = c.collapse;
    uint32_t state_ri = it->second.first, state = c.state;
    if (fault == Fault::CameraPrevFrame) {   // PLANT: the camera run is the previous frame's entry
      const auto pv = casters_.find(CasterKey(f - 1, pass, object, ordinal));
      if (pv != casters_.end() && recs_.count(pv->second.first)) state_ri = pv->second.first, state = recs_.at(state_ri).t->caster_calls[pv->second.second].state;
    }
    if (fault == Fault::PropsCameraForWrestlers && c.n_nodes > 0) {   // PLANT: a wrestler's map takes the props map's camera (a call of the same frame and pass with no palette)
      for (const rdr_scene::CasterCall& o : t.caster_calls)
        if (o.frame == c.frame && o.pass == c.pass && o.n_nodes == 0 && o.state != rdr_scene::kCasterNoState) {
          state_ri = it->second.first, state = o.state;
          break;
        }
    }
    const auto srec = recs_.find(state_ri);
    if (cam_source == CamSource::ColourCall) {   // the [U] of 17.2 item 2, the other answer: R at the entry is the colour call's R -> the model's first LightRef state of the frame
      for (const rdr_scene::LightRef& lr : t.light_refs)
        if (lr.frame == f && lr.object == object && lr.state < t.light_states.size()) {
          rdr_scene::LightState st = t.light_states[lr.state];
          if (fault == Fault::CameraViewprojWrong) std::memcpy(st.shadow_m[0], st.shadow_m[1], sizeof st.shadow_m[0]);
          out.cam = rdr_scene::BuildCasterRows(st), out.have_cam = true;
          break;
        }
    } else if (srec != recs_.end() && state != rdr_scene::kCasterNoState && state < srec->second.t->caster_states.size()) {
      const uint64_t ck = (uint64_t(state_ri) << 32) | state | (uint64_t(fault == Fault::CameraViewprojWrong) << 31);
      auto cr = crows_cache_.find(ck);
      if (cr == crows_cache_.end()) {
        rdr_scene::LightState st = srec->second.t->caster_states[state];
        if (fault == Fault::CameraViewprojWrong) std::memcpy(st.shadow_m[0], st.shadow_m[1], sizeof st.shadow_m[0]);
        cr = crows_cache_.emplace(ck, rdr_scene::BuildCasterRows(st)).first;
      }
      out.cam = cr->second, out.have_cam = true;
    }
    // lane 221 (SKIP_GAME_WORK_MAP.md 17.10): the entry's CasterExtra (parallel to the calls). The rim pass takes its camera and RimLightDepth from it (the main camera is NOT in R); map A takes its collapse point.
    // Plants: rim_camera_prev_frame / collapse_point_stale read the SAME key's extra of the previous hook frame; rim_camera_from_r leaves the camera as built from R.
    const auto extra_of = [&](const decltype(casters_)::const_iterator& at) -> const rdr_scene::CasterExtra* {
      const auto r2 = recs_.find(at->second.first);
      if (r2 == recs_.end()) return nullptr;
      const auto& tt = *r2->second.t;
      return at->second.second < tt.caster_extras.size() ? &tt.caster_extras[at->second.second] : nullptr;
    };
    const rdr_scene::CasterExtra* xt = extra_of(it);
    if ((fault == Fault::RimCameraPrevFrame && pass == rdr_scene::kCpRim) || (fault == Fault::CollapsePointStale && pass == rdr_scene::kCpMap) || (fault == Fault::PlanePosPrevFrame && pass == rdr_scene::kCpPlane)) {
      const auto pv = casters_.find(CasterKey(f - 1, pass, object, ordinal));
      xt = pv != casters_.end() ? extra_of(pv) : nullptr;
    }
    if (const auto wi = worlds_.find(WorldKey(f, object)); wi != worlds_.end() && !wi->second.empty()) {   // lane 221: a rigid caster part's world (the arena's record; the first entry: a prop's world does not change between its calls of a frame)
      const auto wr = recs_.find(wi->second.front().first);
      if (wr != recs_.end() && wi->second.front().second < wr->second.t->arena_worlds.size()) std::memcpy(out.world, wr->second.t->arena_worlds[wi->second.front().second].world, sizeof out.world), out.have_world = true;
    }
    if (xt) {
      if (out.have_cam && pass == rdr_scene::kCpRim && fault != Fault::RimCameraFromR) rdr_scene::ApplyCasterExtra(out.cam, *xt, true);
      if ((xt->flags & rdr_scene::kCxPlanePos) && pass == rdr_scene::kCpPlane) std::memcpy(out.plane_pos, xt->pose_t, sizeof out.plane_pos), out.have_plane_pos = true;   // lane 226 follow-up (S1)
      if ((xt->flags & rdr_scene::kCxPose) && pass == rdr_scene::kCpMap) std::memcpy(out.pose_t, xt->pose_t, sizeof out.pose_t), out.have_pose = true;
    }
    // The wrestler record's bones of this model. [C: live run 7138's zip, tools\caster\caster_zip_probe.py] the bones that equal the stream's palette rows of stream frame g (hook stamp g-1) are in the record whose
    // HEADER frame is g-1 = the call's stamp, NOT in the record that carries the call (header g): 6,120 of the rim draws' 6,426 slots (95 %) and 29,407 colour slots equal a bone of record index g-1, against 0 of the rim's and 8,485 colour slots (about 29 % of 29,407, 20 % of the 43,029 possible)
    // in record g (tools\caster\caster_zip_probe.py on run 7138's zip). So the bones come from the record with header == c.frame (by_header_). This is consistent with, but does NOT explain, the live pal_equal 0 / pal_differ 9375: the entry hash also fails against record g-1's first 96 bones (0 of 30 calls), cause [U] (plant catchup_record = the old same-record pairing; run through --d3-fault outside the test rig it gets only that half, because overwrite_unpaired is set by the rig).
    std::shared_ptr<const rdr_scene::SceneTables> bt = rec->second.t;
    if (fault != Fault::CatchupRecord) {
      bt.reset();
      const auto hb = by_header_.find(c.frame);
      if (hb != by_header_.end()) {
        const auto br = recs_.find(hb->second);
        if (br != recs_.end()) bt = br->second.t;
      }
    }
    if (pass == rdr_scene::kCpPlane) {   // lane 226 (E5)
      FindPlane(f, object, ordinal, fault, out);
      // the call's own palette matrices (kScCasterMats): the plane proxies are in no other record. Plant plane_mats_prev_frame: the same call of the previous hook frame.
      const rdr_scene::CasterCall* pc = &c;
      std::shared_ptr<const rdr_scene::SceneTables> mt = rec->second.t;
      if (fault == Fault::PlaneMatsPrevFrame) {
        const auto pv = casters_.find(CasterKey(f - 1, pass, object, ordinal));
        pc = nullptr;
        if (pv != casters_.end() && recs_.count(pv->second.first)) mt = recs_.at(pv->second.first).t, pc = &mt->caster_calls[pv->second.second];
      }
      if (pc && (pc->flags & rdr_scene::kCcMats) && uint64_t(pc->node_first) + pc->n_nodes <= mt->caster_mats.size() && pc->n_nodes > 0)
        out.bones = &mt->caster_mats[pc->node_first], out.bone_count = pc->n_nodes, out.have_pal = true, out.hold = mt;
      return true;
    }
    for (size_t k = 0; bt && k < bt->engine_objects.size() && k < bt->objects.size(); ++k) {
      if (bt->engine_objects[k] != object) continue;
      const rdr_scene::Object& o = bt->objects[k];
      if (o.bones_count == 0 || uint64_t(o.bones_first) + o.bones_count > bt->bones.size()) continue;
      out.bones = &bt->bones[o.bones_first], out.bone_count = o.bones_count, out.have_pal = true, out.hold = bt;
      break;
    }
    return true;
  }
  // lane 226 (E5): the plane tables of one plane call: the stamp's frame block and, for each part of the object, the mask draw whose rank among the part's draws of the stamp equals the call's ordinal.
  void FindPlane(uint32_t f, uint32_t object, uint32_t ordinal, Fault fault, RecordSrc& out) const {
    out.plane_faults.inv_identity = fault == Fault::PlaneFitIdentity, out.plane_faults.plane_pos_shift = fault == Fault::PlanePosShift, out.plane_faults.camera_stale = fault == Fault::PlaneCameraPrev;
    const auto keep = [&](uint32_t ri) {
      const auto r = recs_.find(ri);
      if (r == recs_.end()) return static_cast<const rdr_scene::SceneTables*>(nullptr);
      for (const auto& h : out.hold_plane)
        if (h == r->second.t) return h.get();
      out.hold_plane.push_back(r->second.t);
      return r->second.t.get();
    };
    if (const auto pf = plane_frames_.find(f); pf != plane_frames_.end())
      if (const rdr_scene::SceneTables* t = keep(pf->second.first); t && pf->second.second < t->plane_frames.size()) out.plane_frame = &t->plane_frames[pf->second.second];
    if (fault == Fault::PlaneCameraPrev) {   // plant: the newest block OLDER than the stamp that has a camera
      uint32_t best = 0;
      bool have = false;
      for (const auto& kv : plane_frames_)
        if (kv.first < f && (!have || kv.first > best)) best = kv.first, have = true;
      if (have)
        if (const rdr_scene::SceneTables* t = keep(plane_frames_.at(best).first); t && plane_frames_.at(best).second < t->plane_frames.size()) out.plane_prev_camera = t->plane_frames[plane_frames_.at(best).second].camera;
    }
    const uint32_t nth = ordinal;
    for (uint32_t part = 0; part < RecordSrc::kPlaneMaxParts; ++part) {
      const auto pd = plane_draws_.find(PlaneKey(f, object, part));
      if (pd == plane_draws_.end()) continue;
      uint32_t rank = 0;
      for (const auto& e : pd->second) {
        if (rank++ != nth) continue;
        if (const rdr_scene::SceneTables* t = keep(e.second.first); t && e.second.second < t->plane_draws.size()) out.plane_draw[part] = &t->plane_draws[e.second.second];
        break;
      }
    }
  }
  // lane 229 (Ring D3 R3): one ring model call's node rows. frame = the hook's stamp (low 32 bits compared), object = the model, call = the index among ALL the object's calls of the frame. false = the record holds no entry
  // for the call (counted by the caller as a missing record; the World registers then have NoWorldRecord).
  bool FindRing(uint64_t frame, uint32_t object, uint32_t call, RecordSrc& out) const {
    out.ring = true, out.ring_e = nullptr, out.ring_pool = nullptr;
    const auto it = rings_.find(RingKey(uint32_t(frame), object, call));
    if (it == rings_.end()) return false;
    const auto rec = recs_.find(it->second.first);
    if (rec == recs_.end() || it->second.second >= rec->second.t->ring_rows.size()) return false;
    out.hold = rec->second.t;
    out.ring_e = &rec->second.t->ring_rows[it->second.second], out.ring_pool = &rec->second.t->ring_nodes;
    return true;
  }
  // lane 258: the number of ring model calls the game's hook ran in hook frame `frame` = the RingRows entries stamped with it (one per call; the offline hook tally of --d3-hook-from-record). 0 = the records hold none.
  uint32_t RingCallsOfFrame(uint64_t frame, std::unordered_set<uint32_t>* objects = nullptr) const {
    uint32_t n = 0;
    const uint32_t f = uint32_t(frame);
    for (const auto& kv : recs_) {
      const rdr_scene::SceneTables& t = *kv.second.t;
      for (const rdr_scene::RingRows& e : t.ring_rows) {
        if (e.frame != f) continue;
        const auto it = rings_.find(RingKey(e.frame, e.object, e.call));
        if (it != rings_.end() && it->second.first == kv.first) ++n, objects ? (void)objects->insert(e.object) : (void)0;   // counted once: the record the join points at
      }
    }
    return n;
  }
  bool dedupe_r5 = false;   // PLANT dedupe_model_r5 (set before Add)
  bool index_rings = false; // N1: set only by RingChain: the arena's and the casters' indexes do not carry the ring entries
  size_t ring_entries() const { return rings_.size(); }
  bool IsRingObject(uint32_t object) const { return ring_objects_.count(object) != 0; }   // lane 274 (S2): a model the ring-rows recorder ever named (the ring class: 13 models, never erased)
  // lane 227 (Wrestlers D3 H2): one wrestler colour call's records: the light state (Find), the hook frame's wrestler light camera (the first caster MAP entry's state, H0) and the object's bones from the record
  // whose HEADER frame equals the call's stamp (the same pairing as the casters', 17.8b). Returns have_light. Plants: camera_prev_frame (the camera of the previous hook frame), brock_update_palette (the bones
  // of the record one header frame older: the update hook's timing, THE PAST BUG of the ledger's Brock rows), state_prev_object (Find's, through the binder).
  bool FindWrestler(uint64_t frame, uint32_t object, uint32_t call, uint32_t keyed, Fault fault, RecordSrc& out) const {
    const bool ok = Find(frame, object, call, keyed, out);
    out.wrestler = true;
    const uint32_t f = uint32_t(frame);
    const uint32_t cf = fault == Fault::CameraPrevFrame ? f - 1 : f;
    const auto& camtab = fault == Fault::PropsCameraForWrestlers ? pcams_ : wcams_;   // PLANT: the props map's camera for the wrestlers
    if (const auto w = camtab.find(cf); w != camtab.end()) {
      const auto rec = recs_.find(w->second.ri);
      if (rec != recs_.end() && w->second.state < rec->second.t->caster_states.size()) {
        const uint64_t ck = (uint64_t(w->second.ri) << 32) | w->second.state;
        auto cr = wcam_cache_.find(ck);
        if (cr == wcam_cache_.end()) cr = wcam_cache_.emplace(ck, rdr_scene::BuildWrestlerCameraRows(rec->second.t->caster_states[w->second.state])).first;
        out.wcam = cr->second, out.have_wcam = cr->second.valid;
      }
    }
    if (const auto d = rdepth_.find(cf); d != rdepth_.end()) {
      if (const auto rec = recs_.find(d->second.ri); rec != recs_.end() && d->second.state < rec->second.t->caster_extras.size()) {
        const rdr_scene::CasterExtra& x = rec->second.t->caster_extras[d->second.state];
        out.rim_depth[0] = x.depth[0], out.rim_depth[1] = x.depth[1], out.rim_depth[3] = 0.f, out.have_rim_depth = true;
        { volatile float dz = x.depth[1] - x.depth[0]; out.rim_depth[2] = dz / 1000.f; }   // [C: run 7147, 36 of 36 sampled wrestler-frames, scratch probe, 18.7] the COLOUR pass's c162 = (near, far, (far - near) / 1000, 0); the rim pass's own c162 has z = 0
      }
    }
    for (int k = 0; k < 3; ++k) {   // lane 245 (H3): lane 242's recorder rows of the call's object at the candidate stamps; none = no source (SphereOcc, named)
      const auto sp = spheres_.find(SphereKey(f + uint32_t(kSphereDelta[k]), object));
      if (sp == spheres_.end()) continue;
      const auto rec = recs_.find(sp->second.first);
      if (rec == recs_.end() || sp->second.second >= rec->second.t->sphere_occ.size()) continue;
      const rdr_scene::SphereOccRow& row = rec->second.t->sphere_occ[sp->second.second];
      out.have_sphere_alt[k] = true;
      out.sphere_alt_ran[k] = uint8_t(((row.flags & rdr_scene::kSoSphere0Ran) && row.count >= 1 ? 1u : 0u) | ((row.flags & rdr_scene::kSoSphere1Ran) && row.count >= 2 ? 2u : 0u));
      for (int i = 0; i < 2; ++i) std::memcpy(out.sphere_alt[k][i], row.p[i], 16), std::memcpy(out.sphere_alt[k][2 + i], row.v[i], 16);
    }
    if (sphere_pick >= 0 && sphere_pick < 3 && out.have_sphere_alt[sphere_pick]) {
      out.have_sphere = true, out.sphere_ran = out.sphere_alt_ran[sphere_pick];
      std::memcpy(out.sphere, out.sphere_alt[sphere_pick], sizeof out.sphere);
    }
    const uint32_t hf = fault == Fault::BrockUpdatePalette ? f - 1 : f;
    if (const auto hb = by_header_.find(hf); hb != by_header_.end()) {
      if (const auto br = recs_.find(hb->second); br != recs_.end()) {
        const std::shared_ptr<const rdr_scene::SceneTables>& bt = br->second.t;
        for (size_t k = 0; k < bt->engine_objects.size() && k < bt->objects.size(); ++k) {
          if (bt->engine_objects[k] != object) continue;
          const rdr_scene::Object& o = bt->objects[k];
          if (o.bones_count == 0 || uint64_t(o.bones_first) + o.bones_count > bt->bones.size()) continue;
          out.bones = &bt->bones[o.bones_first], out.bone_count = o.bones_count, out.have_pal = true, out.hold = bt;
          out.skel_actor = bt->Str(o.id).rfind("skel:", 0) == 0;
          break;
        }
      }
    }
    return ok;
  }
  // One call's records. frame = the hook's frame stamp (low 32 bits compared), call = the call's index among all the object's calls, keyed = the keyed ordinal. Returns have_light.
  bool Find(uint64_t frame, uint32_t object, uint32_t call, uint32_t keyed, RecordSrc& out) const {
    out = RecordSrc{};
    const uint32_t f = uint32_t(frame);
    if (auto it = refs_.find(LightKey(f, object, call)); it != refs_.end()) {
      const auto rec = recs_.find(it->second.first);
      if (rec != recs_.end()) {
        out.rows = Rows(it->second.first, it->second.second, *rec->second.t);
        out.have_light = true;
      }
    }
    if (auto it = worlds_.find(WorldKey(f, object)); it != worlds_.end() && !it->second.empty()) {
      const auto& ents = it->second;
      const auto& pick = ents[keyed < ents.size() ? keyed : ents.size() - 1];   // [U] one entry per keyed call; fewer entries than keyed calls = the last one (a plant-free guess the proof compare checks)
      const auto rec = recs_.find(pick.first);
      if (rec != recs_.end()) {
        const rdr_scene::ArenaWorld& w = rec->second.t->arena_worlds[pick.second];
        std::memcpy(out.world, w.world, sizeof out.world);
        out.world_flags = w.origin;
        out.have_world = true;
      }
    }
    if (auto it = clocks_.find(f); it != clocks_.end()) {
      const auto rec = recs_.find(it->second.first);
      if (rec != recs_.end()) {
        const rdr_scene::ArenaWorld& w = rec->second.t->arena_worlds[it->second.second];
        out.clock = w.clock, out.clock_origin = w.origin, out.have_clock = true;
      }
    }
    return out.have_light;
  }
  // Live: keep only the newest records.
  void Evict(uint64_t frame, uint64_t keep_records = 8) {   // (review S8) the newest 8 records are enough (stream frame j reads the stamps of record j); `frame` is unused
    (void)frame;
    while (order_.size() > keep_records) {
      const auto it = recs_.find(order_.front());
      if (it == recs_.end()) {
        order_.pop_front();
        continue;
      }
      for (uint64_t k : it->second.light_keys) refs_.erase(k);
      for (uint64_t k : it->second.world_keys) worlds_.erase(k);   // (a key shared by two records loses both: stamps are unique per record in practice)
      for (uint32_t c : it->second.clock_frames) clocks_.erase(c);
      for (uint64_t k : it->second.caster_keys) casters_.erase(k);   // lane 211
      for (uint32_t s : it->second.plane_frame_stamps)   // lane 226: only the entries this record owns
        if (auto pf = plane_frames_.find(s); pf != plane_frames_.end() && pf->second.first == it->first) plane_frames_.erase(pf);
      for (uint64_t k : it->second.plane_draw_keys)
        if (auto pd = plane_draws_.find(k); pd != plane_draws_.end()) {
          for (auto e = pd->second.begin(); e != pd->second.end();) e = e->second.first == it->first ? pd->second.erase(e) : std::next(e);
          if (pd->second.empty()) plane_draws_.erase(pd);
        }
      for (uint64_t k : it->second.ring_keys) rings_.erase(k);       // lane 229
      for (uint64_t k : it->second.sphere_keys)                       // lane 245: only the entries this record owns
        if (auto sp = spheres_.find(k); sp != spheres_.end() && sp->second.first == it->first) spheres_.erase(sp);
      for (uint32_t f : it->second.wcam_frames) {   // lane 227
        if (const auto w = wcams_.find(f); w != wcams_.end() && w->second.ri == it->first) wcams_.erase(w);
        if (const auto w = pcams_.find(f); w != pcams_.end() && w->second.ri == it->first) pcams_.erase(w);
        if (const auto w = rdepth_.find(f); w != rdepth_.end() && w->second.ri == it->first) rdepth_.erase(w);
      }
      if (auto hb = by_header_.find(uint32_t(it->second.t->header.guest_frame)); hb != by_header_.end() && hb->second == it->first) by_header_.erase(hb);
      for (auto rc = crows_cache_.begin(); rc != crows_cache_.end();) rc = (uint32_t(rc->first >> 32) == it->first) ? crows_cache_.erase(rc) : std::next(rc);
      for (auto rc = wcam_cache_.begin(); rc != wcam_cache_.end();) rc = (uint32_t(rc->first >> 32) == it->first) ? wcam_cache_.erase(rc) : std::next(rc);   // lane 227 (review S1)
      for (auto rc = rows_cache_.begin(); rc != rows_cache_.end();) rc = (uint32_t(rc->first >> 32) == it->first) ? rows_cache_.erase(rc) : std::next(rc);
      recs_.erase(it);
      order_.pop_front();
    }
  }
  size_t records() const { return recs_.size(); }
  // lane 274: the real calls of the object in a hook frame = its LightRefs (one per sub_82C04DF0 call, ordinals 0..n-1). 0 = none recorded.
  uint32_t RealCalls(uint32_t frame, uint32_t object) const {
    uint32_t n = 0;
    while (refs_.count(LightKey(frame, object, n))) ++n;
    return n;
  }
  size_t light_refs() const { return refs_.size(); }
  size_t world_entries() const { return worlds_.size(); }
  size_t caster_entries() const { return casters_.size(); }
  size_t wrestler_cameras() const { return wcams_.size(); }
  size_t sphere_rows() const { return spheres_.size(); }   // lane 245
  int sphere_pick = 0;   // lane 245: which candidate stamp (index into kSphereDelta) is THE source of c168-c171; 0 = the call's own hook stamp [U until the live run's census]
  uint64_t bad_refs() const { return bad_refs_; }

 private:
  struct Rec {
    std::shared_ptr<const rdr_scene::SceneTables> t;
    std::vector<uint64_t> light_keys, world_keys;
    std::vector<uint32_t> clock_frames;
    std::vector<uint64_t> caster_keys;   // lane 211
    std::vector<uint32_t> plane_frame_stamps;   // lane 226
    std::vector<uint64_t> plane_draw_keys;       // lane 226
    std::vector<uint64_t> ring_keys;     // lane 229
    std::vector<uint32_t> wcam_frames;   // lane 227
    std::vector<uint64_t> sphere_keys;   // lane 245
    uint64_t max_frame = 0;
  };
  static uint64_t SphereKey(uint32_t frame, uint32_t object) { return thin::Mix(thin::Mix(0x5F3E, frame), object); }
  static uint64_t LightKey(uint32_t frame, uint32_t object, uint32_t ordinal) { return thin::Mix(thin::Mix(thin::Mix(0x1157, frame), object), ordinal); }
  static uint64_t WorldKey(uint32_t frame, uint32_t object) { return thin::Mix(thin::Mix(0xA57D, frame), object); }
  static uint64_t CasterKey(uint32_t frame, uint32_t pass, uint32_t object, uint32_t ordinal) { return thin::Mix(thin::Mix(thin::Mix(thin::Mix(0xCA57, frame), pass), object), ordinal); }
  static uint64_t RingKey(uint32_t frame, uint32_t object, uint32_t call) { return thin::Mix(thin::Mix(thin::Mix(0x4195, frame), object), call); }
  const rdr_scene::LightRows& Rows(uint32_t ri, uint32_t state, const rdr_scene::SceneTables& t) const {
    const uint64_t k = (uint64_t(ri) << 32) | state;
    auto it = rows_cache_.find(k);
    if (it == rows_cache_.end()) it = rows_cache_.emplace(k, rdr_scene::BuildLightRows(t.light_states[state])).first;
    return it->second;
  }
  uint32_t next_ = 0;
  uint64_t bad_refs_ = 0;
  std::unordered_map<uint32_t, Rec> recs_;
  std::deque<uint32_t> order_;
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> refs_;
  std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> worlds_;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> clocks_;
  // lane 226 (E5): the plane tables by stamp. plane_frames_[stamp] = (record, index); plane_draws_[(stamp, object, part)] = ordinal -> (record, index) (the nth draw of a part is the nth ordinal)
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> plane_frames_;
  std::unordered_map<uint64_t, std::map<uint32_t, std::pair<uint32_t, uint32_t>>> plane_draws_;
  static uint64_t PlaneKey(uint32_t frame, uint32_t object, uint32_t part) { return thin::Mix(thin::Mix(thin::Mix(0x9A7E, frame), object), part); }
  std::unordered_map<uint32_t, uint32_t> by_header_;   // lane 211: the record's header frame (low 32 bits) -> record (the bones of a call stamped s are in the record with header s)
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> casters_;   // lane 211: CasterKey -> (record, CasterCall index)
  std::unordered_set<uint32_t> ring_objects_;   // lane 274 (S2)
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> rings_;     // lane 229: RingKey -> (record, RingRows index)
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> spheres_;   // lane 245: SphereKey(stamp, skeleton) -> (record, SphereOccRow index)
  struct WCam { uint32_t ri = 0, state = 0; bool paired = false; };
  std::unordered_map<uint32_t, WCam> wcams_, pcams_, rdepth_;   // (rdepth_: state = the CasterCall index of the rim entry whose extra holds the depth)
    // lane 227: hook frame stamp -> the record and caster-state index of that frame's wrestler light camera (a map entry with a palette) / of the props' first map camera (the plant props_camera_for_wrestlers)
  mutable std::unordered_map<uint64_t, rdr_scene::WrestlerCameraRows> wcam_cache_;
  mutable std::unordered_map<uint64_t, rdr_scene::CasterRows> crows_cache_;
  mutable std::unordered_map<uint64_t, rdr_scene::LightRows> rows_cache_;
};

// A register that has no source, as the census names it: "VS c186 Time: clock c186 has no source".
inline std::string UnsourcedName(uint32_t key) {
  const uint32_t reg = key & 0xFFFF, stage = (key >> 16) & 1, why = key >> 24;
  char text[160];
  const char* n = RegName(reg);
  std::snprintf(text, sizeof text, "%s c%u%s%s (%s)", stage ? "PS" : "VS", reg, *n ? " " : "", n, NoSrcName(NoSrc(why)));
  return text;
}
inline uint32_t UnsourcedKey(NoSrc why, uint32_t stage, uint32_t reg) { return (uint32_t(why) << 24) | (stage << 16) | reg; }
// lane 245: the census of the [U] hook-frame offset of the sphere rows: one VS register c168-c171 of a proof compare, the stream's value against the recorder's row at each candidate stamp (only a sphere the game computed). reads =
// registers with at least one candidate row; norow = registers with none; eq[k] = registers equal bit for bit at offset kSphereDelta[k].
// lane 245 (reviewer SF1): the sphere stamp is [U] until a live run, and a wrong pick would fail every skin-body proof and show markers 0 without exercising the seam. Render thread only: once >= kSpherePickReads register
// reads were counted, adopt the candidate between +0 (index 0) and -1 (index 1) that is bit-equal on >= 99 % of the reads while the other is below 99 %; otherwise undecided (-1), the pick stays and the counting goes on. (+1 can hardly exist live: record j holds
// stamps up to j-1.) Proof compares stay bit-exact whatever is picked: the pick only decides which row the MARKER is composed from.
inline constexpr uint64_t kSpherePickReads = 64;
inline int ChooseSpherePick(uint64_t reads, const uint64_t eq[3]) {   // -1 = undecided (too few reads, or both / neither candidate is equal)
  if (reads < kSpherePickReads) return -1;
  const bool ok0 = eq[0] * 100 >= reads * 99, ok1 = eq[1] * 100 >= reads * 99;
  if (ok0 && !ok1) return 0;
  if (ok1 && !ok0) return 1;
  return -1;
}
inline void NoteSphereOffsets(uint64_t& reads, uint64_t& norow, uint64_t eq[3], const RecordSrc& rs, uint32_t reg, const uint32_t* truth) {
  const uint32_t i = reg & 1u;
  bool any = false;
  for (int k = 0; k < 3; ++k) {
    if (!rs.have_sphere_alt[k] || !(rs.sphere_alt_ran[k] & (1u << i))) continue;
    any = true;
    if (std::memcmp(rs.sphere_alt[k][reg - 168], truth, 16) == 0) ++eq[k];
  }
  any ? ++reads : ++norow;
}

}  // namespace rdr_adapter::d3
