// The scene record, version 1 (Session 101, arc A-1; docs\renderer\SCENE_RECORD.md): the state of one scene at one instant, in our
// terms. Three sources write it (the viewer's timeline from the files, the live game's engine hooks, a recording of either); the scene
// front-end (src\renderer_scene\) draws it with assets it loads itself, materials bound by NAME. No game or SDK includes.
// On disk it is flat POD tables (like the frame file, adapter_frame.h): a header, a section directory, then the sections.
#pragma once
#include <cstdint>

#include "../renderer_adapter/names.h"  // DrawClass

namespace rdr_scene {

inline constexpr uint32_t kSceneMagic = 0x4E435352;  // "RSCN"
inline constexpr uint32_t kSceneVersion = 1;

// Space: world units and axes of the GAME (the matrices the engine gives its draws); a source in another space converts once, at the
// source (the viewer's "Y up, +Z toward the stage" space is converted by its backend; the conversion is lodged in SCENE_RECORD.md).
// Matrices: 16 floats, the layout of assets::Matrix (row vectors, p' = p * M, translation in [12..14]).
struct Mat4 { float m[16]; };

enum class Source : uint8_t { Viewer = 0, Live = 1, Recording = 2 };

enum SceneSectionId : uint32_t {
  kScStrings = 1,     // char[]: NUL-separated UTF-8; a name field = byte offset + 1 (0 = none)
  kScCamera,          // Camera[1]
  kScAssets,          // AssetRef[]
  kScObjects,         // Object[]
  kScBones,           // Mat4[]: skin palettes, objects index ranges of it
  kScParams,          // ParamValue[]: named material values that differ from the file's
  kScLights,          // Light[]
  kScSprites,         // Sprite[] (2D layer)
  kScTexts,           // Text[]
  kScMovies,          // Movie[]
  kScTargets,         // Target[]: the surfaces the scene asks for, by role (names.h SurfaceRole)
  kScEngineObjects,   // uint32[] parallel to Object[]: the game's engine object address (guest) of each object, 0 = none; Live records only.
                      // S101 contract (WRESTLER_PROMOTION_MAP.md section 2): the pairing key between a record object and a stream draw's
                      // DrawOrigin::object. Additive: older readers skip it; absent = all 0.
  kScLightStates,     // LightState[]: S101 lane 109 (LS0, PER_DRAW_LIGHTS.md section 3): the deduplicated render-camera light states R of the frame. Optional.
  kScLightRefs,       // LightRef[]: per model call (object, ordinal) the index into kScLightStates. Optional; absent = no per-draw light information.
  kScArenaWorlds,     // ArenaWorld[]: S101 lane 130 (docs\renderer\ARENA_OURS.md "D3 gaps"): the evaluated world of the arena's engine objects (placed 390-397, moving pieces)
                      // and the stage clock, each stamped with the drawing thread's guest frame. Optional; absent = none recorded (an older record).
  kScCircleShadows = 16,  // lane 136: CircleShadow[]: per character, ShadowParam + ViewProj + the 21 body points (vertex constants c0, c4-c7, c8-c28). Optional. (15 is lane 130's.)
  kScCircleMesh,          // lane 136: CircleMesh[0 or 1]: the disc mesh in the stream's host vertex layout. Optional
  kScPlaneFrames,         // lane 139: PlaneFrame[]: the plane shadow module's per-frame constants (SHADOW_PLANE.md). Optional
  kScPlaneDraws,          // lane 139: PlaneDraw[]: the plane chain's per-draw part and the blur block. Optional
  kScRingRows,            // lane 194 (Ring D3, SKIP_GAME_WORK_MAP.md 16.3 R0): RingRows[]: per ring model call, the node rows the packer reads at the call. Optional
  kScRingNodes,           // lane 194: RingNodeRows[]: the node-row pool the RingRows entries index (12 floats a node = VS c76..c78 of the node). Optional
  kScCasterCalls,         // lane 202 (Caster D3 E0, SKIP_GAME_WORK_MAP.md 17.7): CasterCall[]: per caster pass entry (map / plane / rim) the camera state, palette pointer / count and the pass id. Optional
  kScCasterStates,        // lane 202: LightState[]: the deduplicated render-camera records R as read AT THE PASS ENTRY, the CasterCall entries index it. Optional
  kScCasterNodes,         // lane 217 (SKIP_GAME_WORK_MAP.md 17.9): uint32[]: the per-node hash pool the CasterCall entries index (FNV-1a 32 of each palette matrix as read at the entry; node k of a call = pool[node_first + k]). Optional
  kScCasterExtras,        // lane 221 (SKIP_GAME_WORK_MAP.md 17.10): CasterExtra[], PARALLEL to kScCasterCalls (same count, same order): the rim pass's camera (the thread slot's current-camera product), RimLightDepth and the map-A collapse point. Optional
  kScCasterMats,          // lane 226 (SKIP_GAME_WORK_MAP.md 17.11, E5): Mat4[]: the palette matrices a PLANE pass entry read from [model+200] (the plane proxies are in no other record); a CasterCall with kCcMats indexes it by node_first
  kScSphereOcc,           // lane 242 (SKIP_GAME_WORK_MAP.md 18.8, H3): SphereOccRow[]: per wrestler skeleton the 'CCSO' component's two spheres as sub_826DF8E0 left them (P, V = the stream's VS c168-c171) + its table. Optional
  kScCount
};

struct SceneHeader {
  uint32_t magic = kSceneMagic;
  uint32_t version = kSceneVersion;
  uint32_t section_count = 0;
  uint8_t source = 0;          // Source
  uint8_t pad[3] = {};
  uint64_t guest_frame = 0;    // live / recording: the game's frame; viewer: its own frame counter
  double time_s = 0;           // scene time (motion frames / 60 in the viewer)
  uint32_t address = 0;        // string: where (go-anywhere address), 0 = unknown
  uint32_t flags = 0;
};

struct SectionEntry {
  uint32_t id = 0;
  uint32_t element_size = 0;
  uint64_t count = 0;
  uint64_t offset = 0;  // from the start of the record
};

struct Camera {
  Mat4 view;             // world -> view
  Mat4 proj;             // view -> clip, the game's convention (lodged in the page)
  float eye[3] = {0, 0, 0};
  float fov_y_deg = 0;   // 0 = take proj as given
  float near_z = 0, far_z = 0;
  float jitter[2] = {0, 0};  // pixels, for DLSS / TAA (0 = none)
  uint32_t cut = 0;      // 1 = this frame is a camera cut (no blending / motion vectors across it)
  uint32_t flags = 0;    // CameraFlags (S101 lane 1: the v1 pad word, now declared; 0 = view and proj as above, same layout)
};
// S101 lane 1 (A-1 v2 candidate, kept inside v1: the word was the zero pad, so old records read as 0 = separate matrices).
enum CameraFlags : uint32_t {
  kCamViewIsViewProj = 1,  // `view` holds the combined world -> clip matrix and `proj` is the identity (the viewer's writer);
                           // a reader that needs the view alone (eye space, fog depth) must use `eye` / `fov_y_deg`, not `view`
};

// What to load, by name. The front-end loads each asset once with our loaders (src\assets\) from the player's own files.
enum class AssetKind : uint8_t { Model = 0, Texture = 1, Font = 2, Movie = 3 };
struct AssetRef {
  uint8_t kind = 0;      // AssetKind
  uint8_t pad[3] = {};
  uint32_t pac = 0;      // string: path under game_data\extracted ("pac\\ch\\CH001.PAC")
  uint32_t entry = 0;    // string: entry name or number in the pac ("" = the whole pac's model)
  uint32_t variant = 0;  // attire / skin / LOD index where the asset has them
};

enum ObjectFlags : uint32_t {
  kObjVisible = 1,
  kObjCastsShadow = 2,
  kObjSkinned = 4,       // bones_first / bones_count index kScBones
  kObjMirrorX = 8,       // the arena's mirrored twin (50001 "r"): drawn with the mirror (assets.md "An arena pac")
  kObjTurned = 16,       // the arena's turned twin (50001 "y")
};

// One thing to draw: an instance of an asset, with its class and its live values.
struct Object {
  uint32_t asset = 0;            // index into kScAssets
  rdr_adapter::DrawClass klass = rdr_adapter::DrawClass::ModelPart;
  uint8_t layer = 0;             // draw-order group within the class (arena slot order from 50001, 0 otherwise)
  uint16_t pad = 0;
  uint32_t flags = kObjVisible;  // ObjectFlags
  uint32_t id = 0;               // string: a stable name for the object across frames ("actor:ent", "arena:slot:12", "crowd:card:301")
  Mat4 world;
  uint32_t bones_first = 0, bones_count = 0;  // into kScBones (kObjSkinned)
  uint32_t params_first = 0, params_count = 0;  // into kScParams: values that override the file's
  uint32_t mesh_mask_lo = 0xFFFFFFFFu, mesh_mask_hi = 0xFFFFFFFFu;  // which of the model's first 64 meshes are shown (attire pieces)
  uint32_t light_set = 0xFFFFFFFFu;  // the GAME's light set id (= some Light::set), looked up BY ID, never a table index; or none
                                     // (S101 lane 1, after the lane 28 review: one id space shared by every contributor's Light rows)
  float tint[4] = {1, 1, 1, 1};      // fades (object alpha), 1 = none
};

// A material value by name (the effect's named parameter, g_*), for one object. Unset parameters take the file's value.
struct ParamValue {
  uint32_t name = 0;      // string: "g_SweatAmount", ...
  float value[4] = {0, 0, 0, 0};
};

enum class LightKind : uint8_t { Directional = 0, Point = 1, Ambient = 2, Hemisphere = 3 };
struct Light {
  uint32_t set = 0;       // light sets group lights the way the game's light set does (docs\formats\light_set.md)
  uint8_t kind = 0;       // LightKind
  uint8_t slot = 0;       // the slot within its set (the game's 4-slot rig)
  uint16_t pad = 0;
  float dir_or_pos[3] = {0, 0, 0};
  float color[3] = {1, 1, 1};  // linear, intensity folded in
  float range = 0;
};

// S101 lane 109 (LS0; docs\renderer\PER_DRAW_LIGHTS.md): the per-draw light state. Every per-draw light constant of a model draw is a verbatim copy of
// one record per thread slot, the "render camera record" R (2208 bytes, the game's global 0x8392DDA4 -> +4 array, index = the thread slot). A LightState
// is what the packer / binder read of R, decoded to host floats, in R's own order. Two-level table so that ~300 model calls a frame share <= 8 states.
inline constexpr uint32_t kLsLightFloats = 224;     // R+0..895 (the install's block, from the light object +48)
// Float offsets inside LightState::light (R byte offset / 4): the named parts of the install block (PER_DRAW_LIGHTS.md 1.1).
inline constexpr uint32_t kLsDir = 4;       // R+16: 5 light directions, 4 floats each (world space; w as stored)
inline constexpr uint32_t kLsCol = 24;      // R+96: 5 light colours rgbw (w = the lobe weight), 4 floats each; light 4 = the KeyLight
inline constexpr uint32_t kLsBlockA = 88;   // R+352: a 4-float block (sky or ground: which is which is [U])
inline constexpr uint32_t kLsBlockB = 92;   // R+368: the other 4-float block
struct LightState {
  float light[kLsLightFloats];   // R+0..895 as floats (word-swapped from the guest)
  float shadow_m[3][16];         // R+912, R+976, R+1040: three 4x4 matrices of the shadow fields
  float shadow_f[16];            // R+1104..1163 (15 floats; [15] = 0): the ShadowPrm source floats
  float shadow_x[2][8];          // R+2144, R+2176: two 32-byte blocks (the packer copies them to the shadow sub-block)
  float map_wh[2];               // the shadow map size, G+208 / G+212 (floats of the word at 0x8392DDA4); 0 = not read
  uint32_t slot;                 // the thread slot the state was read from
  uint32_t flags;                // LightStateFlags
};
enum LightStateFlags : uint32_t { kLsMapKnown = 1 };
static_assert(sizeof(LightState) == 1232, "LightState is a flat POD table");
// One model call (sub_82C04DF0): the engine object (DrawOrigin::object), the 0-based ordinal of this object's model calls in the frame, the state it read.
struct LightRef {
  uint32_t object = 0;
  uint32_t ordinal = 0;
  uint32_t state = 0;            // index into kScLightStates
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) when the call ran: the join to the stream's frame is by this, not the record header
};
static_assert(sizeof(LightRef) == 16, "LightRef is a flat POD table");

// lane 136 (SH3 slice 1, docs/renderer/SHADOW_CIRCLES.md): the round blob shadows under the characters (T_CircleShadow, sub_82812E90) as the game uploads them. Two optional sections with
// ids in the enum (16 and 17; 15 is lane 130's): 16 = one CircleShadow per character per call, 17 = the one shared disc mesh.
// Entries are stamped with the hook's guest frame and joined to the stream's frame by that stamp, never by the record's index (the +1 pairing is the crowd's, not ours).
inline constexpr uint32_t kCirclePoints = 21;      // uploaded WorldPosArray rows (c8..c28)
inline constexpr uint32_t kCircleMeshVertices = 128, kCircleMeshIndices = 192, kCircleVertexStride = 32;
struct CircleShadow {
  uint32_t frame = 0;          // the hook's guest frame (low 32 bits) when the game uploaded the constants
  uint32_t character = 0;      // the character's engine address (the word in the slot table), 0 = unknown
  uint32_t slot = 0;           // 0..8: the slot in the game's table of 9 characters
  uint32_t flags = 0;          // CircleFlags
  float shadow_param[4] = {};  // c0: x strength, y ground height, z fade height, w fade power
  float view_proj[16] = {};    // c4-c7 (the four uploaded rows)
  float points[kCirclePoints][4] = {};   // c8-c28: x, y, z, 1 of each tracked body point (world space, the game's file space)
};
enum CircleFlags : uint32_t { kCircleHaveViewProj = 1, kCircleHaveParam = 2 };
static_assert(sizeof(CircleShadow) == 16 + 16 + 64 + 16 * kCirclePoints, "CircleShadow is a flat POD table");
struct CircleMesh {
  uint32_t vertex_count = 0, index_count = 0, stride = kCircleVertexStride, flags = 0;
  uint8_t vertices[kCircleMeshVertices * kCircleVertexStride] = {};   // the stream's host layout: float3 position, float blend, u8x4 point ids, float3 disc uv + strength
  uint16_t indices[kCircleMeshIndices] = {};                          // triangle list, host byte order
};
static_assert(sizeof(CircleMesh) == 16 + kCircleMeshVertices * kCircleVertexStride + 2 * kCircleMeshIndices, "CircleMesh is a flat POD table");

// lane 139 (SH3 slice 2, docs/renderer/SHADOW_PLANE.md): the plane shadow chain's constants as the game uploads them (ids 18 / 19 are in the SceneSectionId enum). Two optional sections:
// kScPlaneFrames = PlaneFrame[] (one per guest frame the plane module drew: the module's own constants, shared by every draw of the frame) and kScPlaneDraws = PlaneDraw[] (one per plane-chain draw:
// the per-draw part, and for the blur the whole blur block). Both versioned (kPlaneLayoutVersion), both stamped with the hook's guest frame; a consumer joins by (stamp, kind, object, part, nth) and
// never by the record's index. Fields are NAMED by what they are (rule 6); the hook maps them to the 360's registers (plane_shadow_core.h). A value the writer could not see is not "present".
inline constexpr uint32_t kPlaneLayoutVersion = 1;
enum PlaneKind : uint32_t { kPlaneMask = 0, kPlaneBlur = 1 };
enum PlaneFrameHave : uint32_t {   // PlaneFrame::have: bit set = the value was uploaded THIS frame (cleared per frame; never "the last frame's")
  kPlaneHaveFade = 1, kPlaneHaveNormal = 2, kPlaneHaveFit = 4, kPlaneHaveCamera = 8, kPlaneHaveAll = 15,
};
enum PlaneFlags : uint32_t {
  kPlaneCameraIsColour = 1,   // set by a reader that checked the camera bit-equal to the colour pass's (the writer cannot see the colour pass; the record's Camera has no bit-equal combined matrix)
};
struct PlaneFrame {
  uint32_t version = kPlaneLayoutVersion;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits)
  uint32_t flags = 0;            // PlaneFlags
  uint32_t have = 0;             // PlaneFrameHave
  float fade[10] = {};           // the module's distance / fade array: 500, 300, 40, 1.5, 0.5, 0.35, 0.21, 1.0, 0.0, then the screen-fit weight (module+68..+107; the weight is module+104)
  float plane_normal[4] = {};    // the plane's normal (0, -1, 0, 0)
  float fit_matrix[16] = {};     // the per-frame screen fit of the plane (module+1312), row-major as uploaded
  float camera[16] = {};         // the main camera's view-projection (module+1072), row-major as uploaded
};
static_assert(sizeof(PlaneFrame) == 16 + 40 + 16 + 64 + 64, "PlaneFrame is a flat POD table");
enum PlaneDrawHave : uint32_t { kPlaneDrawHavePos = 1, kPlaneDrawHaveBlur = 2 };   // blur: scale + weight + matrix + point all uploaded this frame
struct PlaneDraw {
  uint32_t version = kPlaneLayoutVersion;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) when the draw was issued
  uint32_t kind = 0;             // PlaneKind
  uint32_t flags = 0;
  uint32_t object = 0;           // mask: the model object (DrawOrigin::object); blur: 0
  uint32_t part = 0;             // mask: the part index; blur: 0
  uint32_t have = 0;             // PlaneDrawHave
  uint32_t ordinal = 0;          // the draw's sequence number in its frame (both kinds): the identity a record delivered twice keeps
  float plane_pos[4] = {};       // mask: the plane's position (0, -120, 0, 0), uploaded per proxy before its draws
  float blur_scale[4] = {};      // blur: the first blur row (x = module+96)
  float blur_weight[4] = {};     // blur: the weight row (x = module+104)
  float blur_matrix[16] = {};    // blur: camera x fit, transposed, as uploaded
  float blur_point[4] = {};      // blur: the fit-point row from the table at 0x83625ADC (640, 0, 640, 1)
};
static_assert(sizeof(PlaneDraw) == 32 + 16 + 16 + 16 + 64 + 16, "PlaneDraw is a flat POD table");
// S101 lane 130: section kScArenaWorlds, layout 1 (68 bytes). Two kinds of entry, both stamped with `frame` = the guest frame read on the DRAWING thread when the
// hook ran (low 32 bits); a consumer joins by that stamp, never by the record index (map section 8: the stream reads record i - 1, a lag a live consumer cannot wait for).
//  kAwObject: the model object `sub_82C04DF0` was called with (`object`, the join key to DrawOrigin::object) and what the packer reads for it: `world` = VS c76..c78 as the
//             packer cuts them from the 4x4 at r5 + 64 (n - 1) (row k of `world` = column k of that matrix: (m[k], m[4+k], m[8+k], m[12+k]); listing 0x82C0A038-0x82C0A090). r5 is
//             the call's own palette pointer (the skinning matrices, world x inverse bind, NOT the node world); n = `slot` = the 1-based node index of the first entry of part 0's
//             bone list (0xFFFF = not read). `origin` = ArenaWorldObjFlags.
//  kAwClock : the stage clock of that frame (the scalar VS c186 `Time` holds for the UV-scrolling surfaces): `clock` = frame - `origin`, `origin` = the guest frame the stage
//             was set up in (CStage::MotionSetup exit). Only written when the origin is known; [U] whether the game's own counter is this difference (the J1 check).
struct ArenaWorld {
  uint8_t kind = 1;              // ArenaWorldKind
  uint8_t layout = 1;            // the layout of this entry (a writer that changes the fields bumps it; a reader refuses a layout it does not know)
  uint16_t slot = 0xFFFF;
  uint32_t object = 0;
  uint32_t frame = 0;
  uint32_t origin = 0;           // kAwClock: the stage set-up frame; kAwObject: ArenaWorldObjFlags
  float clock = 0.f;             // kAwClock: frame - origin
  float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // kAwObject: c76..c78 (3 rows of 4); identity by default
};
enum ArenaWorldKind : uint8_t { kAwObject = 1, kAwClock = 2 };
enum ArenaWorldObjFlags : uint32_t { kAwForeignPalette = 1,  // r5 was not [model+200] (another caller's array, e.g. sub_82C13EC8): still what the packer read
                                     kAwNodeAssumed = 2 };   // the part descriptor was not readable: node 1 assumed (counted node_assumed)
static_assert(sizeof(ArenaWorld) == 68, "ArenaWorld is a flat POD table");

// lane 194 (Ring D3, docs/renderer/SKIP_GAME_WORK_MAP.md 16.3 R0 / 16.7): the ring's per-call node rows. One RingRows per ring model call (a call of sub_82C04DF0 whose model address is in the
// table at [0x836F4C74]); `first_node` / `n_nodes` index kScRingNodes. Unlike kScArenaWorlds the entry is NOT deduplicated by (model, r5): a rope side's three heights share one r5 buffer that the
// game rewrites between heights, so the rows are copied AT THE CALL and every call has its own entry. Stamped with the hook's guest frame (a consumer joins by that stamp, never by the record index).
// The join key is (frame, object, call) with `call` = the index among ALL the object's model calls of the frame (= the LightRef ordinal of that call); `keyed` = the index among the object's
// RECORDED entries of the frame (the sequence ordinal a binder will use). The game's palette is 64 bytes a node (4x4, translation in m[12..14]); the pool holds the rows of each node as the packer cuts them.
inline constexpr uint32_t kRingRowsLayout = 1;
inline constexpr uint32_t kRingMaxNodes = 96;      // a call with more nodes is truncated (kRrCapped, counted); the ring's biggest is the skirt at 66
enum RingClass : uint8_t { kRcNone = 0, kRcRopeHi, kRcRopeLo, kRcProxy, kRcCoverHi, kRcCoverLo, kRcTurnbuckle, kRcPostHi, kRcPostLo, kRcCanvas, kRcSkirt, kRcCount };
enum RingRowsFlags : uint32_t {
  kRrForeignR5 = 1,       // r5 was not [model+200] (the rope context buffer, sub_82C13EC8's caller array): still what the packer reads
  kRrCountForwarded = 2,  // the node count came through [this+220] (sub_82BD3AB8's forward; vtable slot 2 verified to be the same function)
  kRrCountUnknown = 4,    // the node count could not be read: n_nodes = 0, no rows
  kRrCapped = 8,          // count above kRingMaxNodes: only the first kRingMaxNodes rows are kept
  kRrUnreadable = 16,     // the rows at r5 were not (fully) readable: n_nodes = 0, no rows
  kRrAlphaUnread = 32,    // [model+252] (the f1 the vtable slot 13 passes) was not readable; alpha = 0
};
struct RingRows {
  uint8_t kind = 1;              // 1 = a call's rows (the only kind in layout 1)
  uint8_t layout = uint8_t(kRingRowsLayout);
  uint8_t klass = 0;             // RingClass of the model address
  uint8_t pad = 0;
  uint32_t object = 0;           // the model address (DrawOrigin::object)
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) when the call ran
  uint32_t call = 0;             // index among ALL the object's model calls of the frame (the LightRef ordinal)
  uint32_t keyed = 0;            // index among this object's recorded RingRows of the frame
  uint32_t r5 = 0;               // the palette pointer the call carried (guest address; the rope buffer's identity)
  uint32_t first_node = 0;       // into kScRingNodes
  uint16_t n_nodes = 0;
  uint16_t pad2 = 0;
  uint32_t flags = 0;            // RingRowsFlags
  float alpha = 0.f;             // [model+252]: the MdlAlpha c185 source [L] (SKIP_GAME_WORK_MAP.md 16.1: the proof compare decides)
};
static_assert(sizeof(RingRows) == 40, "RingRows is a flat POD table");
struct RingNodeRows {
  float rows[12];                // VS c76..c78 of the node: row k = (m[k], m[4+k], m[8+k], m[12+k]) of the node's 4x4
};
static_assert(sizeof(RingNodeRows) == 48, "RingNodeRows is a flat POD table");
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
// lane 242 (SKIP_GAME_WORK_MAP.md 18.8, H3): the wrestlers' sphere occluders. The formula is settled offline (tools\skip\sphere_check.py: 7,854 draws a capture, centre within 1.1e-4, axis 6e-7, strength 1.4e-6)
// but three of its inputs are in no record: the character's sphere TABLE (bone roles -> nodes, offsets, angles, radius, scale, blend: loaded from the character's data), the move's CONTACT record
// (sub_8265B470; it blends the centre toward a point of another bone) and the WORLD node matrices (the record holds the skinning matrices). So the recorder takes the RESULT: at the exit of
// sub_826DF8E0 ('CCSO' update, this = the component) it copies the component's own output arrays [this+48] (P, 16 bytes a sphere) and [this+52] (V) = exactly what sub_82BCC888 stores and
// sub_82BE7AC0 uploads to VS c168-c171 (P0 = c168, P1 = c169, V0 = c170, V1 = c171), the 52-byte table rows [this+44] for the census, and the skeleton [[this+32]+48] (the record's engine object).
inline constexpr uint32_t kSphereOccLayout = 1;
inline constexpr uint32_t kSphereMax = 2;           // the component's count is 2 (sub_826E0030); a larger count is refused
inline constexpr uint32_t kSphereRecWords = 13;     // 52 bytes a table row
enum SphereOccFlags : uint32_t {
  kSoSpheres = 1,    // P / V were read from the component's output arrays
  kSoTable = 2,      // table[] was read
  kSoSkeleton = 4,   // object = [[this+32]+48] was read (else 0)
  kSoSphere0Ran = 8, // the game computed sphere 0 in this call (its table row has both nodes >= 0); else p[0] / v[0] are the previous call's, not this frame's
  kSoSphere1Ran = 16,
};
struct SphereOccRow {
  uint32_t layout = kSphereOccLayout;
  uint32_t flags = 0;                          // SphereOccFlags
  uint32_t object = 0;                         // the skeleton's guest address = the record's engine object (0 = unreadable)
  uint32_t frame = 0;                          // the hook's guest frame (low 32 bits), never the record's
  uint32_t count = 0;                          // spheres read (<= kSphereMax)
  uint32_t component = 0;                      // `this` of sub_826DF8E0 (the 'CCSO' component), for the census
  uint32_t character = 0;                      // [this+32]
  uint32_t calls = 0;                          // sub_826DF8E0 calls of this skeleton in this hook frame (the row is the LAST one)
  float p[kSphereMax][4] = {};                 // sphere centre xyz + radius (VS c168, c169), host floats
  float v[kSphereMax][4] = {};                 // axis xyz + strength (VS c170, c171)
  uint32_t table[kSphereMax][kSphereRecWords] = {};   // the 52-byte rows as guest words (big-endian converted to host order; floats are bit patterns)
};
static_assert(sizeof(SphereOccRow) == 32 + 64 + 104, "SphereOccRow is a flat POD table");
// 2D layer (the UI class, C1). Positions in the layout's own units (the game's 1280 x 720 root, A0-4) and an anchor, so our pass can
// draw it at the display's resolution for any screen shape.
enum class Anchor : uint8_t { Stretch = 0, Center, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
struct Sprite {
  uint32_t texture = 0;    // index into kScAssets (Texture), or 0xFFFFFFFF for a surface by role (target)
  uint32_t target = 0xFFFFFFFFu;  // index into kScTargets when the sprite shows a GPU picture
  uint32_t widget = 0;     // string: the widget / layout node name
  float rect[4] = {0, 0, 0, 0};   // x, y, w, h in layout units
  float uv[4] = {0, 0, 1, 1};
  float color[4] = {1, 1, 1, 1};
  float rotation = 0;
  uint16_t order = 0;      // draw order within the 2D layer
  uint8_t anchor = 0;      // Anchor
  uint8_t blend = 0;       // 0 alpha, 1 additive, 2 opaque, 3 premultiplied (S101 lane 23: 3 is new, the pass reads it; older records never hold it)
};

struct Text {
  uint32_t utf8 = 0;       // string: the text itself (from the game's string table, A0-4)
  uint32_t string_id = 0;  // the game's string id where known (40000-79999 from string.pac), 0 = none
  uint32_t font = 0;       // index into kScAssets (Font)
  uint32_t widget = 0;     // string
  float pos[2] = {0, 0};   // layout units
  float size = 0;          // glyph height, layout units
  float color[4] = {1, 1, 1, 1};
  uint16_t order = 0;
  uint8_t anchor = 0;
  uint8_t align = 0;       // 0 left, 1 centre, 2 right
};

enum class MovieShow : uint8_t { FullScreen = 0, OnTarget = 1, OnSprite = 2 };
struct Movie {
  uint32_t asset = 0;          // index into kScAssets (Movie): the .bik's path
  uint8_t show = 0;            // MovieShow
  uint8_t has_alpha = 0;
  uint16_t pad = 0;
  uint32_t target = 0xFFFFFFFFu;  // OnTarget: into kScTargets (the tron picture); OnSprite: into kScSprites
  uint32_t planes[4] = {};     // live: image ids of the decoded Y / cR / cB / A planes (the game's Bink decoder keeps decoding)
  double movie_time_s = 0;
};

// A surface the scene needs, by role (names.h). The front-end owns its size: screen roles follow the render scale, others keep theirs.
struct Target {
  rdr_adapter::SurfaceRole role = rdr_adapter::SurfaceRole::Unnamed;
  uint8_t index = 0;
  uint16_t pad = 0;
  uint32_t name = 0;          // string
  uint32_t width = 0, height = 0;  // 0 = the screen's size (scaled)
};

}  // namespace rdr_scene
