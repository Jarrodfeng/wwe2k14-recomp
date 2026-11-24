// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {

// lane 202 (Caster D3 phase 1, docs/renderer/SKIP_GAME_WORK_MAP.md 17.3 E0 / 17.7): one record per CASTER PASS ENTRY. The three caster passes are model-class methods called from a draw node through a
// vtable slot: sub_82C01FE0 (T_ShadowMap: the props map, wrestler map A, map B), sub_82BF22C8 (T_PlaneShadowMap) and sub_82BF9878 (T_RimLightMap); all take r3 = the model, r4 = the pass key, r5 = the
// palette pointer or 0 (0 = [model+200]) [C: listing 0x82C01FF4-0x82C02008]. The camera R is read at the entry (the same thread-slot record the colour pass's LightState reads); `LightRef` has no pass field, so the
// pass id is here. Stamped with the hook's guest frame (a consumer joins by that stamp, never by the record index). The key is (pass, object, ordinal): `ordinal` = the index among ALL this object's
// entries of THAT PASS in the frame (map A and map B are the object's ordinals 0 and 1 of the map pass), counted for every entry the hook sees.
inline constexpr uint32_t kCasterCallsLayout = 1;
inline constexpr uint32_t kCasterNoState = 0xFFFFFFFFu;
inline constexpr uint32_t kCasterMaxNodes = 96;   // a palette above this is truncated in the hash (kCcCapped, counted)
enum CasterPass : uint8_t { kCpNone = 0, kCpMap, kCpPlane, kCpRim, kCpCount };
enum CasterCallFlags : uint32_t {
  kCcR5Zero = 1,        // r5 was 0: the palette is [model+200] (the colour pass's convention)
  kCcForeignR5 = 2,     // r5 was neither 0 nor [model+200]
  kCcNoPalette = 4,     // the model is not a skeleton the wrestler contributor counts nodes for (props, pieces): n_nodes 0, no hash
  kCcUnreadable = 8,    // the palette bytes (or [model+200]) could not be read: n_nodes 0, no hash
  kCcCapped = 16,       // count above kCasterMaxNodes: the hash covers the first kCasterMaxNodes nodes
  kCcNoState = 32,      // R could not be read or the frame's state table was full: state = kCasterNoState
  kCcCollapseUnread = 64,   // the map-A collapse flag word could not be read: collapse = 0
  kCcNullBlock = 128,   // [[r13]+1048] was null: slot 0, as the game does
  kCcNodeHashes = 256,  // lane 217: the call has n_nodes per-node hashes in kScCasterNodes starting at node_first
  kCcMats = 512,        // lane 226 (E5): the call has n_nodes palette matrices in kScCasterMats starting at node_first (plane passes only; mutually exclusive with kCcNodeHashes)
};
struct CasterCall {
  uint8_t kind = 1;              // 1 = a pass entry (the only kind in layout 1)
  uint8_t layout = uint8_t(kCasterCallsLayout);
  uint8_t pass = 0;              // CasterPass
  uint8_t collapse = 0;          // the map-A collapse flag (sub_82BA7808: the word at 0x83929A50 + 4 * slot) read at the entry: 1 = map A is being drawn
  uint32_t object = 0;           // the model address (DrawOrigin::object)
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) when the entry ran
  uint32_t ordinal = 0;          // index among ALL this object's entries of this pass in the frame
  uint32_t state = kCasterNoState;   // index into kScCasterStates
  uint32_t key = 0;              // r4: the pass key ([model+368][part] must equal it for a part to be drawn)
  uint32_t r5 = 0;               // the palette pointer the call carried (0 = [model+200])
  uint32_t palette = 0;          // the pointer the passes read (r5, or [model+200] when r5 == 0)
  uint16_t n_nodes = 0;          // the palette node count (the wrestler contributor's WantCount); 0 = none
  uint16_t pad = 0;
  uint32_t flags = 0;            // CasterCallFlags
  uint32_t map_a[3] = {};        // [model+408], [model+412], [model+416]: the map-A collapse inputs (listing 0x82C03CE8-0x82C03DA8) [C], read for the map pass only
  uint32_t node_first = 0;       // lane 217: index into kScCasterNodes of node 0's hash (valid only with kCcNodeHashes; the call has n_nodes hashes). Was `pad2` in lane 202's records (always 0, flag clear)
  uint64_t pal_hash = 0;         // FNV-1a 64 of the palette's n_nodes 64-byte matrices as read at the entry (host floats' bytes); 0 = none
};
static_assert(sizeof(CasterCall) == 64, "CasterCall is a flat POD table");
// lane 221 (SKIP_GAME_WORK_MAP.md 17.10): what the pass entry reads BESIDES R. One per CasterCall, parallel (kScCasterExtras). [C] listings: the rim cached-list twin sub_82BD8A60 (0x82BD8C08) and the body's
// sub_82BE6350 (0x82BEEF74) pack c56-59 from sub_82BA7718 = the 4x4 product at 0x839299C0 + 64 * slot (the thread slot's CURRENT camera, which is the MAIN camera during the rim pass), transposed; c162 =
// (float [G+300], float [G+304], 0, 0) with G = word 0x8392DDA4 (twin 0x82BD8B14-0x82BD8B88); the map-A collapse row is (0, 0, 0, t) with t = row 3 of the pose matrix [[model+192] + 64 * [model+408]] (0x82C03D94-0x82C03DDC).
inline constexpr uint32_t kCasterExtraLayout = 1;
inline constexpr uint32_t kCameraProductBase = 0x839299C0;   // sub_82BA7718: the product of slot s is the 64 bytes at kCameraProductBase + 64 * s [C listing 0x82BA7718-0x82BA7744]
enum CasterExtraFlags : uint32_t {
  kCxProduct = 1,    // product[] was read (the current camera's 4x4 at the entry)
  kCxDepth = 2,      // depth[] was read (G + 300, G + 304)
  kCxPose = 4,       // pose_t[] was read (map pass with a collapse flag and [model+408] >= 0 only)
  kCxPlanePos = 8,   // lane 226 follow-up (S1): PLANE pass only: pose_t[] holds the plane register file's c7 (PlanePos) at the pass ENTRY (the tracker's snapshot taken without the part draw having to run)
};
struct CasterExtra {
  uint32_t layout = kCasterExtraLayout;
  uint32_t flags = 0;          // CasterExtraFlags
  float product[16] = {};      // the 4x4 at kCameraProductBase + 64 * slot, 16 host floats in memory order (row-major as the game stores it)
  float depth[2] = {};         // [G + 300], [G + 304]: RimLightDepth c162 = (depth[0], depth[1], 0, 0)
  float pose_t[4] = {};        // row 3 of the pose matrix [model+408] of [model+192]: the map-A collapse point
};
static_assert(sizeof(CasterExtra) == 8 + 64 + 8 + 16, "CasterExtra is a flat POD table");

}  // namespace rdr_scene
