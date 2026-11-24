// The scene record, version 1 (Session 101, arc A-1; docs\renderer\SCENE_RECORD.md): the state of one scene at one instant, in our
// terms. Three sources write it (the viewer's timeline from the files, the live game's engine hooks, a recording of either); the scene
// front-end (src\renderer_scene\) draws it with assets it loads itself, materials bound by NAME. No game or SDK includes.
// On disk it is flat POD tables (like the frame file, adapter_frame.h): a header, a section directory, then the sections.
#pragma once
#include <atomic>
#include <cstdint>

#include "../renderer_adapter/names.h"  // DrawClass

namespace rdr_scene {


inline constexpr uint32_t kSceneMagic = 0x4E435352;  // "RSCN"
inline constexpr uint32_t kSceneVersion = 1;
// S103 SVO: SceneHeader::flags bit 0. The record was made on the guest thread while goto's `scene:` stage push was playing (stage_view::g_playing, stamped only when the OFF host switch --wwe2k14_rdr_stage_view is on: stage_view::g_host_enabled, never the stream build's g_enabled). The OFF host
// then leaves the 2D rows out of the picture: the game puts the Create a Story editor's full-screen red background over the scene in program mode 2 (docs\agents\s103\CSC.md). The host reads this stamp, never the live flag.
inline constexpr uint32_t kSceneStagePlaying = 1u;
// S105 A3: SceneHeader::flags bits 1 / 2 = the game shows a scene camera (cam_gate.h: kCamGateRead / kCamGateShown). Append-only: an older record has both 0 (= not read = drawn as before).
// S102 T19: a record that carries crowd run markers (kScCrowdRuns / kScCrowdMembers) is written as version 2; a record without them is still version 1, byte for byte
// as before. A reader accepts both (ParseRecord expands the markers into the plain member objects at once; docs\formats\scene_record_crowd_runs.md).
inline constexpr uint32_t kSceneVersionRuns = 2;
inline constexpr uint32_t kScCrowdRuns = 40;     // S102 T19: CrowdRun[]: one marker per run of same-type stands members. Version 2 only. Optional
inline constexpr uint32_t kScCrowdMembers = 41;  // S102 T19: CrowdMember[]: what differs per member (the world's 12 constants, the index); the runs index it

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
  kScOwnedPasses,         // S102 P0 / P4P5 (OWN_THE_PASS.md 6): OwnedPassStamp[]: one per pass decided this frame (node_switch.h). Optional; absent = every pass is the game's
  kScPostParams,          // S103 A (29; docs\renderer\INPUTS_BOARD.md): PostParamsRow[]: the post settings object S as the frame's latch read it (outside the post node's walk). Optional
  kScModelRuntime,        // S103 A (30): ModelRuntimeRow[]: per known wrestler model, the model-object words the colour pass reads at the call (MdlAlpha = [model+252]). Optional
  kScInputStamps,         // S103 A (31): InputStampRow[]: per registered scene input, the guest frame it was last captured for (src\scene_live\inputs_core.h). Optional
  kScLightObjects,        // S103 A2 (32; docs\renderer\INPUTS_A2.md): LightObjectRow[]: the game's light objects (sets, pool, blend, override) and the manager's selection words as the latch read them (W2 / L1-L4). Optional
  kScParamOverrides,      // S103 A2 (33; docs\renderer\INPUTS_A2.md section 4): ParamOverrideRow[]: the model's run-time parameter OVERRIDE entries (name key + live value) read at the latch (W4 / A4). Optional
  // 34 is reserved for lane A2: more wrestler runtime values once their sources are traced (INPUTS_BOARD.md [U] rows: W4 / A4 parameter blocks, W5 sweat)
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
  kCamProjFromFov = 2,     // S103 S: `proj` is not given: the reader builds it from fov_y_deg, the target's aspect and near_z / far_z (0 = the reader's default)
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
  kObjForceBlend = 64,   // S103 CGF: a slot-32 stage structure draw (alpha = the call's f1 = MdlAlpha): every mesh of it is alpha-blended with the object's alpha even when its material is opaque (depth write as the material's)
  kObjNoDepthWrite = 32768,   // S108 BLD2: the game draws this object with z write OFF whatever its materials say (the floor blood splats: sub_82698770 0x82698898..0x826988E0, sub_82BB7AD8 = 0)
  kObjDetachedPiece = 128, // S103 DTP: an invisible own-skeleton piece whose SUBM owner-copy flag (instance +24) the game set to 0 (the People's Elbow ELBW object takes the pad: docs\research\s103\PEP.md): the game poses the piece from its OWN palette now.
                           // Carries pad (piece id) / layer (owner slot + 1) as kObjDrawnByOwner does. The owner's merged mesh stops drawing it (alpha-0 row, rf_wrestlers pre-pass) and the piece is drawn from its own guest rows.
  kObjCastsMapB = 256,   // S103 MBC (RMP.md 2): a weapon / event prop / structure piece the game casts into the wrestlers' shadow map (map B) in mode 0 (PMC section 4's filter, set by rf_props.cpp ApplyWeaponRows); never set on wrestlers (they cast by their own meshes)
  kObjMapBEvent = 512, kObjMapBStage = 1024, kObjMapBUnsure = 2048, kObjMapBPiece43 = 4096,   // S103 MBC: the caster's family, for the census only (with kObjCastsMapB): event prop / structure piece / ids 59 68 71 96 (category unknown: cast and counted); neither of the first two = a manager weapon
  kObjCastsProps = 8192, // S103 PM2 (PMC.md): a weapon / event prop / structure piece the game casts into the PROPS map (the floor's shadow map; weapons::CastsPropsMap): set by rf_props.cpp ApplyWeaponRows; the family bits above ride along
  kObjOwnRowsPiece = 16384, // S105 A5a: a linked own-skeleton piece of a GUEST-ROW owner (a created superstar's hair / necklace / braces): invisible, carries pad (piece id) / layer (owner slot + 1) like kObjDetachedPiece, and its palette is its OWN latch read
                         // ([model+200]: the game copied the owner's rows into it, proved per record by the contributor's copy proof, own_rows_rule.h). rf_wrestlers draws it from its own guest rows and lights it with the OWNER's state.
                         // NEVER part of the EYF pre-pass mask (kObjDrawnByOwner | kObjDetachedPiece): a row keyed by this piece id on a guest owner would match guest part N (src_key = part index).
  kObjDrawnByOwner = 32, // S103 NRBF: an invisible own-skeleton piece (hair, caps, glasses) that its OWNER body model draws (the game copies the owner's rows into it by name): not a hole, no model of its own.
                         // S103 PZF2 (EB): such an object also carries the piece's id in Object::pad (u16) and its owner's character slot + 1 in Object::layer; 0 in either = unknown (an older record): the draw side
                         // treats that as a hole. rf_wrestlers counts it drawn-by-owner only when the owner's resident model has a mesh of that piece id.
};

// One thing to draw: an instance of an asset, with its class and its live values.
struct Object {
  uint32_t asset = 0;            // index into kScAssets
  rdr_adapter::DrawClass klass = rdr_adapter::DrawClass::ModelPart;
  uint8_t layer = 0;             // draw-order group within the class (arena slot order from 50001, 0 otherwise)
  uint16_t pad = 0;              // 0 for most objects; kObjDrawnByOwner: the piece id (below); S103 RGP: a GPU-skinned ring piece: its instance + 1 (the front end's kept-draw instance key, front_end.cpp it.inst)
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
