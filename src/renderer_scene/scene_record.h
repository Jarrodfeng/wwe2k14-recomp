// The scene record, version 1 (Session 101, arc A-1; docs\renderer\SCENE_RECORD.md): the state of one scene at one instant, in our
// terms. Three sources write it (the viewer's timeline from the files, the live game's engine hooks, a recording of either); the scene
// front-end (src\renderer_scene\) draws it with assets it loads itself, materials bound by NAME. No game or SDK includes.
// On disk it is flat POD tables (like the frame file, adapter_frame.h): a header, a section directory, then the sections.
#pragma once
#include <atomic>
#include <cstdint>

#include "../renderer_adapter/names.h"  // DrawClass

namespace rdr_scene {

// S103 DTP: --wwe2k14_rdr_detached_pieces (default on; start_settings.cpp stores it). Off = a piece the game stopped copying the owner's rows into is a hole again (the old picture: the pad stays on the arm).
inline std::atomic<bool> g_detached_pieces{true};

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

// S103 lane C (docs/renderer/FRAME_CONTRACT.md 8; lane C owns ids 38-41, but 40 and 41 are the crowd run markers above (S102 T19), so lane C uses 38 and 39 only).
// 38 kScModelLights: per-model light sets as the game's rig rewrote them this frame (the entrance rig rewrites a model's sets every frame) and the set each source mesh uses
//    (FrontEnd ModelLights). 39 kScStageFx: the stage's live parts in one table by kind: screen pictures (the tron / wall / mini screens), the crowd camera flashes, and the
//    evaluated world of a moving arena slot. Both optional; a record without them is byte-identical to today's; an unknown layout drops THAT section only (c_errors).
inline constexpr uint32_t kScModelLights = 38;
inline constexpr uint32_t kScStageFx = 39;
inline constexpr uint8_t kModelLightsLayout = 1, kStageFxLayout = 1;
enum ModelLightKind : uint8_t { kMlSet = 0, kMlMesh = 1 };
struct ModelLightRow {
  uint8_t kind = 0;      // ModelLightKind
  uint8_t layout = kModelLightsLayout;
  uint16_t mesh = 0;     // kMlMesh: the model's source mesh (the front-end's Mesh::src order)
  uint32_t asset = 0;    // index into kScAssets (Model): the model the row is for
  int32_t set = 0;       // kMlSet: the set's game id (LightSet::id); kMlMesh: index among this asset's kMlSet rows in table order, -1 = the file's choice
  uint32_t flags = 0;    // kMlSet: bit 0 = pcol valid (the cube is built from the previous frame's colours)
  float dir[12] = {}, col[12] = {}, sky[3] = {}, ground[3] = {}, fall[4] = {}, wt[4] = {}, hl_dir[3] = {}, hl_col[3] = {}, pcol[12] = {};
};
static_assert(sizeof(ModelLightRow) == 240, "ModelLightRow is a flat POD table");
enum StageFxKind : uint8_t { kFxScreen = 1, kFxFlash = 2, kFxSlotWorld = 3, kFxShow = 4, kFxVisMark = 5, kFxVisSkip = 6, kFxCar = 7 };   // S103 CAR: kFxCar = a created arena (200-249) stage's part indices (car_arena_rows.h). S103 WSL: kFxVisMark = one per frame while a stage exists (seq = stage generation, pad = stage, flags & 1 = the mask is valid, slot = count); kFxVisSkip = a slot the game's look gate skips (stage_slot_vis.h). S103 SCF: kFxShow = the game's show bit of a screen key 800-809 or an art slot 301-304 (stage_show.h; slot = the key / slot, flags & 0x200 = shown)
struct StageFxRow {
  uint8_t kind = 0;      // StageFxKind
  uint8_t layout = kStageFxLayout;
  uint16_t slot = 0;     // kFxScreen: the arena key 800-809; kFxSlotWorld: the stage slot
  uint32_t movie = 0xFFFFFFFFu;  // kFxScreen: index into kScMovies (where the picture comes from)
  uint32_t seq = 0;      // kFxScreen: the picture's serial (a new number = new pixels: the feed is asked for them)
  uint32_t w = 0, h = 0; // kFxScreen: the picture's size
  uint32_t flags = 0;
  float pos[3] = {};     // kFxFlash: the centre (the game's space)
  float scale = 0.f;     // kFxFlash: size
  float color[4] = {1, 1, 1, 1};  // kFxFlash: colour (rgb, w = the flash's life fraction 0..1)
  float alpha = 1.f;     // kFxFlash: c185.x
  float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // kFxSlotWorld: VS c76..c78 as the packer cuts them (ArenaWorld::world's rows)
  uint32_t pad = 0;      // S103 C2: kFxSlotWorld: the slot's model object (guest address; the cross-check's key against kScArenaWorlds), appliers ignore it
};
static_assert(sizeof(StageFxRow) == 112, "StageFxRow is a flat POD table");
// S103 C3: StageFxRow::flags of a kFxFlash row (the live writer, src\scene_live\flash_fx_core.h). Appliers test only kFxfGameScale; the rest is census / provenance.
// S103 E7: a kFxScreen row with this flag (w = h = 0) says the game plays no movie: the screen shows the arena file's own picture (the applier drops its live texture).
inline constexpr uint32_t kFxfScreenIdle = 0x100;
enum StageFxFlashFlags : uint32_t {
  kFxfGameScale = 1,    // scale is the game's multiplier of the sprite model's own coordinates (item +40), not a world half-size
  kFxfDrawDone = 2,     // the item's done flag was set by the draw's own last-frame mark (counter == life)
  kFxfLightSet = 4,     // the game takes the flash colour from light set 500 (the row colour is the manager's block, a stand-in)
  kFxfColorUnread = 8,  // the colour block was not readable: white
};

// S103 lane L (45; lane C's range 42-45, docs\renderer\STAGE_LIGHTING.md): the stage's LOOK as the game's entrance rig leaves it. Two kinds in one table, both optional (a record without
// the table is today's: the match look, the sets by id as the light-set contributor read them). kSlEntrance: flags bit 0 = an entrance is running (the game then draws the ring / prop and
// the yBG_Default / yBG_UVScroll classes with their light-map variant, FrontEnd entrance + entrance_bg), bit 1 = the stage light-map page is the entrance's (fade_lightmap00 + pools)
// not the match's. kSlRigSlot: rig slot `rig_slot` (1 = the arena's sets, 2 = the crowd's, 3 = the ring / props': docs\formats\entrance_rig.md rule 1-2) with the values the rig LEFT in its
// light object AFTER the set update (the record's kScLights are read before it: arena_hooks.h reads post sub_827F8730), world-space directions, colours as stored (may exceed 1).
inline constexpr uint32_t kScStageLook = 45;
inline constexpr uint8_t kStageLookLayout = 1;
// S103 L2 (STAGE_LIGHTING.md class B): kSlPool = one floor pool of the entrance page (dir[0] = world x, dir[1] = world z, dir[2] = half size in world units, col[0..2] = the tint); kSlSpot = the follow spot
// (same fields; flags bit 0 kSlfAtEntrant = centred on the entrant's root (the record's actor:slot:0 world x / z), dir[0..1] then unused). Rows are the page's inputs; the page itself is composed by us.
enum StageLookKind : uint8_t { kSlEntrance = 1, kSlRigSlot = 2, kSlPool = 3, kSlSpot = 4,
                                kSlPhase = 5,   // S103 D1: flags = the scene phase word 0x836F3940 read at the latch (the stage-slot draw's read rule: phase == 1 = the match); one row a frame, absent = not read
                                // S103 MBC: the SAME row also carries, read at the same latch: rig_slot = participants + 1 (0 = unknown: the count is only valid on the game's slot-loop path, inputs_core.h ReadPlaneStructure), dir[0] = ring gate word [0x836F39A0] + 1 (0 = unknown). Helpers: PhaseRowParticipants / PhaseRowRingGate (below).
                                kSlPageClock = 6,
                                kSlCardFactor = 7,
                                // S103 TNB: one row per entrance light track the game binds to a character (manager word mgr + 148 + 4 i in 0..8 and the character's skeleton read): rig_slot = i + 1 (track i = 0..9), flags = slot + 1 | 0x100 when that character's act is 25000 (census: bound by the body act, else by YAT2 code 208),
                                // dir[0] = x, dir[1] = z, dir[2] = y of [[0x836F38D0 + 4 slot] + 176] (the game's own position, sub_825BACE0 0x825BAECC). A per-frame word, not route data. Absent = the track is not bound (or no read): the rig's own row.
                                kSlTrackBind = 8,
                                // S103 CLRB (docs\agents\s103\CLR.md 3): the game's main-scene clear colour [[0x83929A68] + 240 / 244 / 248 / 252] (R, G, B, A, each 0..255) read at the latch: flags bit 0 = read, col[0..3] = R, G, B, A / 255; one row a frame, absent = not read (record_frame: black)
                                kSlClearColor = 9 };   // S103 B3: the game's card factor word [0x8376383C] read at the latch: dir[0] = (float)word * 0.001 (sub_827F22C8 applies it to the card technique's c184.z only when > 0); flags bit 0 = the word was read; one row a frame, absent = not read (the front end counts it)
// S103 B3c: the last known kind (record_io.cpp skips kinds above it) and the route kinds (the rows that carry the entrance's light route; the others are per-frame words: phase, page clock, card factor).
inline constexpr uint8_t kSlLast = kSlClearColor;
static_assert(kSlLast == 9, "a new stage-look row kind: set kSlLast and decide in IsRouteKind whether it is route data (record_io.cpp, stage_look.h read both)");
inline constexpr bool IsRouteKind(uint8_t k) { return k >= kSlEntrance && k <= kSlSpot; }
static_assert(kSlSpot < kSlPhase, "route kinds come first; the per-frame words follow");
enum StageLookFlags : uint32_t { kSlfEntrance = 1, kSlfEntrancePage = 2, kSlfAtEntrant = 1 };  // (kSlfAtEntrant is a kSlSpot row's own bit 0)
struct StageLookRow {
  uint8_t kind = 0;      // StageLookKind
  uint8_t layout = kStageLookLayout;
  uint16_t rig_slot = 0; // kSlRigSlot: 1 arena, 2 crowd, 3 ring / props
  uint32_t flags = 0;    // kSlEntrance: StageLookFlags
  float dir[12] = {}, col[12] = {}, sky[3] = {}, ground[3] = {};
};
// S103 MBC: the phase row's extra words (see kSlPhase). -1 = unknown (an older record, a failed read, the count off the slot-loop path).
inline int32_t PhaseRowParticipants(const StageLookRow& r) { return r.kind == kSlPhase && r.rig_slot > 0 ? int32_t(r.rig_slot) - 1 : -1; }
inline int32_t PhaseRowRingGate(const StageLookRow& r) { return r.kind == kSlPhase && r.dir[0] >= 1.f ? int32_t(r.dir[0]) - 1 : -1; }
// S103 RML (F4): the same phase row's dir[1] = the plane module state + 1 (1 = the singleton [0x83924758] is null, 2 = it exists; 0 = unknown / an older record). The ring mat's pass is the game's sub_827ECB60 on THIS latch frame:
// off (the light pass) = module absent, or [0x836F39A0] != 0. -1 = unknown (the mat stays plain, counted). Carried in the record so the mirror path and a transition frame pair with their own latch, not the newest one.
inline int32_t PhaseRowPlaneModule(const StageLookRow& r) { return r.kind == kSlPhase && r.dir[1] >= 1.f ? int32_t(r.dir[1]) - 1 : -1; }
inline int32_t PlaneGameOffOf(int32_t module_state, int32_t ring_gate) { return module_state == 0 ? 1 : (module_state == 1 && ring_gate >= 0) ? (ring_gate != 0 ? 1 : 0) : -1; }   // 1 = sub_827ECB60 == 0, 0 = != 0, -1 unknown
static_assert(sizeof(StageLookRow) == 128, "StageLookRow is a flat POD table");

// S103 lane D4 (46 / 47; lane D's range 46-48, docs\renderer\CROWD_PROPS_LATCH.md "Lane D4"): what no other table carries of the crowd. 46 kScCrowdProps = the signs and foam hands (the two per-member
// loops sub_82518950 / sub_82518EB8: pass 1 the 32 bone-held props, pass 2 the 96 free ones), one row per prop the game would draw; 47 kScCrowdSingles = the single standing people
// (sub_8276E7B0: skinned bodies placed one at a time). Both optional (a record without them is byte-identical to today's); an unknown layout / kind drops THAT section only (c_errors). Written by a
// latch reader through src\renderer_scene\crowd_prop_rows.h (the append API checks every row), drawn by LiveCrowd (live_crowd.cpp) from the record alone.
inline constexpr uint32_t kScCrowdProps = 46;
inline constexpr uint32_t kScCrowdSingles = 47;
inline constexpr uint8_t kCrowdPropLayout = 2, kCrowdSingleLayout = 1;   // S103 CSF: CrowdPropRow layout 2 = 68 bytes (file_key); a layout-1 record's section fails closed (size and layout checks drop it, counted)
// S103 SG (Q2 / V2): kind = WHICH LOOP, by act code: 1 = a sign (sub_82518950, act 14006 / 14007, draw at 0x82518D60), 2 = a foam hand (sub_82518EB8, act 14127 / 14128, draw at 0x825192D0).
// Not a technique: BOTH loops pick T_Audience_LightMap iff the phase word [0x836F3940] == 4, else T_Audience (0x82518A20 / 0x82518EF0), the same choice as the stands in the same frame.
enum CrowdPropKind : uint8_t { kCpSign = 1,   // a sign (one mesh, key 40000)
                               kCpHand = 2 };  // a foam hand (mesh 40999 + h per hand type b: model_key)
enum CrowdPropFlags : uint32_t {
  kCpfFree = 1,      // pass 2: a free prop (the world is plain memory, no bone multiply)
  kCpfBone = 2,      // pass 1: a prop held by a person (world = bone matrix x the element's matrix, composed by the reader)
  kCpfHidden = 4,    // listed but not shown (the loop's own gates failed); counted, never drawn
  kCpfInPlace = 8,   // S103 SG2: the stream draws this prop in place this frame (a walked frame's row); a live applier skips it (LiveCrowdOptions::skip_in_place_props), the mirror draws it
};
struct CrowdPropRow {
  uint8_t kind = 0;        // CrowdPropKind
  uint8_t layout = kCrowdPropLayout;
  uint16_t variant = 0;    // the style byte (sign: elt+18 / the free entry's style) or the hand type b (elt+19 / the free entry's style); census only
  uint32_t model_key = 0;  // the pac entry inside the arena's audience set (entry 20000 of PACH 0): sign 40000; hand 40999 + h (S103 SG: static_SetUpHandSignBoard sub_8250B1E8); 0 = the kind's default (DefaultKey)
  uint32_t flags = 0;      // CrowdPropFlags
  uint32_t picture = 0;    // the texture key word the reader resolved for slot 0 (the guest texture handle of slot 0: a foam hand print is a file image, S103 FHB); 0 = not read. Census only until a picture source exists [U]
  float c[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};   // the draw's VS constants c76-c78 (row r = column r of the world matrix), exactly as the loop transposes them into the registers
  uint32_t file_key = 0;   // S103 CSF (layout 2): a SIGN's file key u32([[0x83767C5C] + 16] + 4 b) = its picture's entry id in pac\audience.pac PACH 0 (sub_825E59F0 SetSignBoardTexGroup); 0 = none / a hand / not read; >= 65000 = the game's runtime texture (never a file picture)
};
static_assert(sizeof(CrowdPropRow) == 68, "CrowdPropRow is a flat POD table"); 
struct CrowdSingleRow {
  uint8_t kind = 1;        // 1 = a body (the only kind)
  uint8_t layout = kCrowdSingleLayout;
  uint8_t type = 0;        // the person type 0..19 (census; the asset names the body)
  uint8_t pad = 0;
  uint32_t asset = 0;      // index into kScAssets: the body ("pac\bg\bgNN.pac" + entry "20000 + type", LOD 0), as a CrowdRun::asset names it
  uint32_t id = 0;         // string: a stable name ("crowd:single:N"); 0 = none
  uint32_t bones_first = 0, bones_count = 0;   // into kScBones: the person's skin palette (0 count = the stored pose, counted)
  uint32_t flags = 0;      // kCsfHidden
  float c[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};   // the person's VS constants c76-c78 (CrowdMember::c's form)
};
static_assert(sizeof(CrowdSingleRow) == 72, "CrowdSingleRow is a flat POD table");
enum CrowdSingleFlags : uint32_t { kCsfHidden = 1 };

// S103 lane D6 (48; lane D's range 46-48): the pictures of the signs / foam hands (CrowdPropRow::picture = the key). The foam hand print is a FILE image (audience-set entry 42000 + h - 1, DXT1 64x128; S103 FHB); a sign is DXT1 64x64 and a latch-time reader of the guest's
// texture object (src\scene_live\guest_texture.h: registry handle -> wrapper -> header -> fetch dwords -> pixel memory, untiled, endian-swapped) hands the blocks over: block-compressed, mip 0, row-major, little-endian. Optional; an unknown
// layout / a bad size drops THIS section only (c_errors). One row per distinct key a shown prop row names.
inline constexpr uint32_t kScCrowdPictures = 48;
inline constexpr uint8_t kCrowdPictureLayout = 1;
inline constexpr uint32_t kCrowdPictureBytes = 4096;   // the biggest picture (the 64x128 foam hand, DXT1)
enum CrowdPictureFormat : uint32_t { kCpfBc1 = 18, kCpfBc2 = 19, kCpfBc3 = 20 };   // the game's format codes (the adapter's FormatOf)
struct CrowdPictureRow {
  uint32_t key = 0;          // the texture key word (CrowdPropRow::picture)
  uint16_t width = 0, height = 0;
  uint32_t format = 0;       // CrowdPictureFormat
  uint32_t nbytes = 0;       // bytes used of \bytes: ((width + 3) / 4) * ((height + 3) / 4) * (8 or 16)
  uint8_t layout = kCrowdPictureLayout, pad[3] = {};
  uint8_t bytes[kCrowdPictureBytes] = {};
};
static_assert(sizeof(CrowdPictureRow) == 4116, "CrowdPictureRow is a flat POD table");
// A row is valid only for a block-compressed format the renderer knows and EXACTLY the bytes of its size (the upload copies the image's full size: a short row would be read past its end).
inline bool CrowdPictureValid(const CrowdPictureRow& r) {
  if (r.layout != kCrowdPictureLayout || !r.width || !r.height || r.width > 4096 || r.height > 4096) return false;
  if (r.format != kCpfBc1 && r.format != kCpfBc2 && r.format != kCpfBc3) return false;
  const uint32_t bpb = r.format == kCpfBc1 ? 8u : 16u;
  return r.nbytes <= kCrowdPictureBytes && r.nbytes == ((uint32_t(r.width) + 3) / 4) * ((uint32_t(r.height) + 3) / 4) * bpb;
}

// S105 A12: the liquid items' pictures (section 53) get their OWN row, sized for the biggest blood decal (256x256 DXT1 = 32 KB; the blood textures of chCommon.pac PACH 0 entry 10 are 64x128 .. 256x256, 4-32 KB). The crowd's row
// (CrowdPictureRow, section 48) and every crowd reader stay as they were: the 4096-byte cap is the foam hand's, never a limit of the 360.
inline constexpr uint8_t kLiquidPictureLayout = 1;
inline constexpr uint32_t kLiquidPictureBytes = 32768;   // 256x256 DXT1
struct LiquidPictureRow {
  uint32_t key = 0;          // the texture key word (LiquidItemRow::picture = the item's wrapper address)
  uint16_t width = 0, height = 0;
  uint32_t format = 0;       // CrowdPictureFormat
  uint32_t nbytes = 0;       // bytes used of \bytes: ((width + 3) / 4) * ((height + 3) / 4) * (8 or 16)
  uint8_t layout = kLiquidPictureLayout, pad[3] = {};
  uint32_t hash[2] = {};     // PictureHashOf (width, height, format, bytes): made ONCE when the hook reads the texture; the picture cache compares it instead of hashing 32 KB a frame (review F3)
  uint8_t bytes[kLiquidPictureBytes] = {};
};
static_assert(sizeof(LiquidPictureRow) == 32796, "LiquidPictureRow is a flat POD table");
inline uint64_t PictureHashOf(uint16_t width, uint16_t height, uint32_t format, const uint8_t* bytes, size_t n) {   // FNV-1a over the header words and the blocks (the cache's own rule)
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const uint8_t* p, size_t k) { for (size_t i = 0; i < k; ++i) h = (h ^ p[i]) * 1099511628211ull; };
  const uint32_t head[3] = {width, height, format};
  mix(reinterpret_cast<const uint8_t*>(head), sizeof head), mix(bytes, n);
  return h;
}
inline void LiquidPictureSeal(LiquidPictureRow& r) {   // sets hash[] from the row's own fields
  const uint64_t h = PictureHashOf(r.width, r.height, r.format, r.bytes, r.nbytes);
  r.hash[0] = uint32_t(h), r.hash[1] = uint32_t(h >> 32);
}
inline bool LiquidPictureValid(const LiquidPictureRow& r) {
  if (r.layout != kLiquidPictureLayout || !r.width || !r.height || r.width > 4096 || r.height > 4096) return false;
  if (r.format != kCpfBc1 && r.format != kCpfBc2 && r.format != kCpfBc3) return false;
  const uint32_t bpb = r.format == kCpfBc1 ? 8u : 16u;
  return r.nbytes <= kLiquidPictureBytes && r.nbytes == ((uint32_t(r.width) + 3) / 4) * ((uint32_t(r.height) + 3) / 4) * bpb;
}
// One name for the picture cache (crowd_picture_cache.h): each row type is valid by its own rule.
inline bool PictureValid(const CrowdPictureRow& r) { return CrowdPictureValid(r); }
inline bool PictureValid(const LiquidPictureRow& r) { return LiquidPictureValid(r); }
inline uint64_t PictureHash(const CrowdPictureRow& r) { return PictureHashOf(r.width, r.height, r.format, r.bytes, r.nbytes); }
inline uint64_t PictureHash(const LiquidPictureRow& r) { return uint64_t(r.hash[0]) | (uint64_t(r.hash[1]) << 32); }   // sealed at the read (LiquidPictureSeal), never recomputed per frame

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

// S102 T19 (docs\formats\scene_record_crowd_runs.md): one marker for a run of consecutive crowd members that share kind, person type and LOD group (so also asset and palette).
// A reader expands it into `count` Objects inserted at Objects[at]: class Crowd, layer = `layer`, flags = kObjVisible (| kObjSkinned when `skinned`), asset / bones as here, world =
// the member's 12 constants as rdr_scene::live::crowd::WorldFromConstants makes it, id = "crowd:<kind>:<block>:<member:>N" with N = the member's index, or, for a member with
// no readable index (CrowdMember::index = 0xFFFFFFFF), the next number of the record's per-record ordinal (counted over all runs in order, from 0, "u" prefixed). Every other
// Object field is the default. 28 bytes.
enum CrowdRunKind : uint8_t { kRunBlock = 0, kRunFloor = 1, kRunCard = 2 };
struct CrowdRun {
  uint32_t at = 0;            // index in the expanded Objects[] where the run's members start (runs are in nondecreasing `at`)
  uint32_t first_member = 0;  // into kScCrowdMembers
  uint32_t count = 0;
  uint32_t asset = 0;         // index into kScAssets
  uint32_t bones_first = 0, bones_count = 0;
  uint8_t kind = 0;           // CrowdRunKind
  uint8_t block = 0;          // the person type (the id's TT)
  uint8_t layer = 0;          // Object::layer
  uint8_t skinned = 0;        // 1 = kObjSkinned set, bones as above
};
static_assert(sizeof(CrowdRun) == 28, "CrowdRun is a flat POD table");
struct CrowdMember {
  float c[12];                // the draw's VS constants c76-c78: row r is column r of the world matrix (rdr_scene::live::crowd::WorldFromConstants)
  uint32_t index;             // the member's index in its type's array, or 0xFFFFFFFF = none readable
};
static_assert(sizeof(CrowdMember) == 52, "CrowdMember is a flat POD table");
inline constexpr uint32_t kCrowdNoIndex = 0xFFFFFFFFu;

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
enum LightStateFlags : uint32_t { kLsMapKnown = 1, kLsSynthetic = 2 };   // kLsSynthetic (S103 A8): written from the latch by wrestler_light_latch.h, not a walked R: its shadow fields are zero; d3::RecordIndex::Add skips its refs
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
enum CircleFlags : uint32_t { kCircleHaveViewProj = 1, kCircleHaveParam = 2, kCircleFromLatch = 0x100 };   // 0x100 (S103 A3): the row was read at the latch, not recorded by the walk; circle_contrib.h replaces it when the walk recorded the same (frame, slot)
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
enum PlaneKind : uint32_t { kPlaneMask = 0, kPlaneBlur = 1, kPlaneProxy = 2 };   // kPlaneProxy (S103 A3, S8): one row per entry of the plane node's proxy list copy (object only; kind decode / plane position [U]); consumers of the walk's mask / blur rows never see it
enum PlaneFrameHave : uint32_t {   // PlaneFrame::have: bit set = the value was uploaded THIS frame (cleared per frame; never "the last frame's")
  kPlaneHaveFade = 1, kPlaneHaveNormal = 2, kPlaneHaveFit = 4, kPlaneHaveCamera = 8, kPlaneHaveAll = 15,
};
enum PlaneFlags : uint32_t {
  kPlaneFromLatch = 0x100,    // S103 A3 (S8): a PlaneDraw row built at the latch from the proxy list copy (plane_proxy.h), not recorded by the walk; plane_contrib.h drops it when the walk recorded rows of the same frame
  kPlaneNoDraw = 0x200,       // S103 PB: sub_827ED960's x pair collapsed (the game's own 2/0): the plane covers no screen pixel; fit_matrix holds the game's raw inf and kPlaneHaveFit is clear
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
// S103 E5 (lane E's range 49-53; docs\renderer\LIVE_MAPS.md "Lane E5"): two optional latch sections.
//  49 kScPlaneEyes = PlaneEyes[] (one per frame the latch could read the plane module's per-arena tables): the plane mask's EyePos (VS c72) PER PROXY KIND, recomputed from the module's inputs
//     exactly as sub_827ED6D0 does (vector at module+768+16 kind, angle at module+896+4 kind, rotated about y by angle * pi/180 - pi, the result stored at module+992..1000), and PlanePos (c7) = (0, [module+1440], 0, 0).
//     Kinds: 1 = the wrestler proxy, 4..7 = the ring proxy's four points. `have` bit k = eye[k] was recomputed (a kind whose table was not loaded stays 0 and its bit clear: never a guess).
//  50 kScPartClocks = PartClock[] (one per RUN of parts of a stage model with the same clock): the UV-scroll clock VS c186 `Time` = the part's time word [[model+484] + 24 part + 12], which sub_82BD5A98
//     advances by [model+248] every update and wraps at the part's period word (+16). A consumer asks for (object, part) and takes the row with the largest `part` <= the asked one.
inline constexpr uint32_t kScPlaneEyes = 49;
inline constexpr uint32_t kScPartClocks = 50;
inline constexpr uint32_t kPlaneEyesLayout = 1, kPartClockLayout = 1;
enum PlaneEyeKind : uint32_t { kPeWrestler = 1, kPeRing0 = 4, kPeRing3 = 7, kPeKinds = 8 };
enum PlaneEyesFlags : uint32_t { kPeHavePos = 1 };   // plane_pos was read
struct PlaneEyes {
  uint32_t version = kPlaneEyesLayout;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) the latch read the module at
  uint32_t have = 0;             // bit k = eye[k] valid
  uint32_t flags = 0;            // PlaneEyesFlags
  float eye[kPeKinds][4] = {};   // EyePos per module kind (x, y, z, 1)
  float plane_pos[4] = {};       // PlanePos (c7): (0, [module+1440], 0, 0)
};
static_assert(sizeof(PlaneEyes) == 16 + 128 + 16, "PlaneEyes is a flat POD table");
struct PartClock {
  uint32_t version = kPartClockLayout;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits)
  uint32_t object = 0;           // the stage model object (the join key of DrawOrigin::object / kScArenaWorlds)
  uint32_t part = 0;             // the first part this row applies to (rows of one object ascend; it covers the parts up to the next row)
  float time = 0.f;              // c186 for those parts: the part's time word
  float period = 0.f;            // the wrap period (+16): the update subtracts it when time + step exceeds it (sub_82BD5A98 0x82BD5AF4-0x82BD5B04)
  float step = 0.f;              // [model+248]: what one update adds (census; the consumer needs only `time`)
  uint32_t slot = 0xFFFFFFFFu;   // the stage array index of the model (the "arena:slot:K" number, [L] the same index); 0xFFFFFFFF = not from a stage array
};
static_assert(sizeof(PartClock) == 32, "PartClock is a flat POD table");

// S103 SWF (lane E's range, ids 51-53; docs\systems\liquid_map.md; docs\research\s103\SWB.md): the wrestlers' sweat / blood "liquid map" inputs, read at the entry of the map's draw node (sub_8279ACD8: r3 = the CCLM component, the
// map object). Three optional sections, written together or not at all:
//  51 kScLiquidMaps     = LiquidMapRow[]    one per map (character) that had a live item or a pending clear request this hook frame, the LAST call of the frame wins
//  52 kScLiquidItems    = LiquidItemRow[]   the live items of those maps (at most 16 each: the pool's cap), a map's items are the rows [item_first, item_first + item_count)
//  53 kScLiquidPictures = LiquidPictureRow[] the items' source textures (sweat 64x64 DXT1; blood 64x128 .. 256x256 DXT1, S105 A12: its own 32 KB row, not the crowd's 4 KB one), one row per distinct key; LiquidItemRow::picture = the row's `key`, 0 = none
// A reader that does not know them skips them (older readers) and a record without them draws every skin dry (never a failure). A bad layout / a non-finite number drops the three together (liquid_errors).
inline constexpr uint32_t kScLiquidMaps = 51, kScLiquidItems = 52, kScLiquidPictures = 53;
inline constexpr uint32_t kLiquidLayout = 1;
enum LiquidMapFlags : uint32_t {
  kLmEnabled = 1,     // map+2572 != 0 (the node's own enable word)
  kLmClear = 2,       // map+136 != 0: the clear request (create, blood level 0, the Swet reset). The hook CONSUMES it (writes 0) when the node is skipped, so a row's bit is an EDGE
  kLmAlt = 4,         // map+200 != 0 (the alt path: never seen set; recorded, not drawn)
  kLmDim = 8,         // sub_8259F0E0(character): character+5076 != 0 || character+5072 != 0 (created wrestlers); fixed at build
  kLmResources = 16,  // sub_82726170 [C]: the map's resources exist (the node draws only then); the map is also 1x1x1 while they are missing
};
struct LiquidMapRow {   // 64 bytes
  uint32_t layout = kLiquidLayout;
  uint32_t frame = 0;          // the hook's guest frame (low 32 bits) of the call that wrote the row
  uint32_t map = 0;            // the map object (guest address): the persistent image's key
  uint32_t object = 0;         // [[map+48]+48]: the character's skeleton object = the record's engine object of its body (the key of every wrestler row); 0 = not read
  uint32_t flags = 0;          // LiquidMapFlags
  uint16_t width = 0, height = 0;   // map+124 / +128 (1024 x 512; 1 x 1 while the resources are missing)
  uint32_t levels = 0;         // map+132 (3)
  uint32_t item_first = 0, item_count = 0;   // into kScLiquidItems
  uint32_t live_count = 0;     // map+88: the live list's count (item_count counts the ones with a drawable flag set)
  uint32_t reserved[6] = {};
};
static_assert(sizeof(LiquidMapRow) == 64, "LiquidMapRow is a flat POD table");
enum LiquidItemFlags : uint32_t { kLiHasSource = 1 /* the source texture was read */ };
// item+60 bits the node's draw loop skips [C SWB 1d: sub_82725E28 bit30, sub_82725E38 bit29, sub_82725E58 bit26]: paused + not drawn, not drawn. (bit31 = alive: a dying item is still drawn once.)
inline constexpr uint32_t kLiStateNotDrawn = 0x40000000u | 0x20000000u | 0x04000000u;
struct LiquidItemRow {   // 72 bytes
  uint32_t layout = kLiquidLayout;
  uint32_t flags = 0;          // LiquidItemFlags
  uint32_t type = 0;           // item+20: 0 blood, 1 sweat ("tear"); >= 2 is recorded and not drawn (the game inherits its constants from the previous item)
  uint32_t state = 0;          // item+60: bit31 alive, bit30 / bit29 paused + not drawn, bit26 not drawn
  uint32_t pass = 0;           // item+144: 0 = T_Normal p0 (sweat), 1 = T_Normal p1 (paste)
  uint32_t picture = 0;        // CrowdPictureRow::key of the source texture (item+64 -> wrapper -> header); 0 = none read
  float local_area[4] = {};    // item+80..92 (VS c205)
  float world_param[4] = {};   // item+112..124 (VS c206): the disc's two corners in the map's UV
  float rect[4] = {};          // item+128..140: the dirty rect in map texels (x0, y0, x1, y1), 32-snapped
};
static_assert(sizeof(LiquidItemRow) == 72, "LiquidItemRow is a flat POD table");

// S103 I1 (lane I, landing 2; docs\renderer\REMNANT_MAP.md sections 5 and 10): 58 kScQuads2D = Quad2D[] = the 2D layer as OUR record: one row per 2D draw, in call order (`seq`), carrying the numbers the game computed (the four
// vertices, the PosScale / Color / fUseProjMatrix constants) so the layer can be drawn without the stream's draw. This piece covers kQkRect (T_Rect, call site 0x82BC3EE8) and kQkYuvAlpha (T_SpriteYUVAlpha, 0x82BC7E50);
// S103 I2 adds kQkSprite (T_Sprite, 0x82BC5688, the plain and the hue-shifted program); the other kinds are named for the next pieces and a writer never emits them. Written by the taps (src\scene_live\quads2d_tap.h, behind --wwe2k14_rdr_2d_record) and, in the offline rig, from the stream's
// own draws (src\renderer_scene\quads2d_rows.h); drawn by src\renderer_scene\quads2d_draw.* (a pure function of the rows + the pictures). Optional: absent = no rows. A bad row drops THIS section only (Quads2DProblem).
// `have` says which fields were read ([U] means the tap cannot read it yet: blend / alpha test / pass / target stay 0 = unknown until REMNANT_MAP U4 / U6 are traced; the offline rig fills them from the stream's draw state).
// S103 L2a: blend / alpha test now come from the game's per-thread state object at the op-19 (kQhState; quads2d_core.h FillState, R8's code tables); pass / target stay unknown.
// S103 L2a (P0, the id check): 58 is lane I's. FRAME_CONTRACT.md 8 had allocated 58-59 to lane H and 60-64 to lane I; lane H never took an id (no kSc constant in 51-64 but 54-56 particles and this one), and
// 58 is in every 2D record and unit since I1, so the allocation moved, not the id: H 59, I 58 + 60-64 (tools\scene\l2a_unit.cpp [SECTION IDS] checks the table).
inline constexpr uint32_t kScQuads2D = 58;
inline constexpr uint32_t kQuad2DLayout = 5;   // S103 RFX: 5 = + address (324 bytes a row): the game's pixel sampler 0 address code. Layout 4 history:   // S103 I2: 2 = + hsl[4] (212 bytes a row); layout 1 never ran live. S103 P3c: 3 = + proj[16] (276 bytes a row): the matrix-placed rows' c29..c32. S103 SC: 4 = + scissor[4] + scissor_on at the end (320 bytes a row, with the viewport and target size): the clip the game drew the row with (the menu lists clip to their panels); an older layout is refused whole (a row is never read with a guessed scissor)
inline constexpr uint32_t kQuad2DLayout4Bytes = 320;   // S103 RFX: the size of a layout-4 row (no address); ParseRecord reads it as 'address unknown'
inline constexpr uint32_t kQuad2DNotTaken = 0xFFFFFFFFu;   // S103 L2a: SceneTables::quad2d_holes when no live 2D contributor took the frame
enum Quad2DKind : uint8_t { kQkNone = 0, kQkRect = 1, kQkSprite = 2, kQkGlyph = 3, kQkRectGrad = 4, kQkYuv = 5, kQkYuvAlpha = 6, kQkRectColor = 7 /* S105 A8: T_RectColor (sub_82BC3F30, site 0x82BC4404): a rect with a colour per corner; verts[v][3] = the corner's r | g << 8 | b << 16 (0..255 each, an exact float), verts[v][4] = its a (0..255) */, kQkCount };
enum Quad2DPass : uint8_t { kQpUnknown = 0, kQpFinal = 1, kQpSceneColour = 2, kQpOffscreen = 3, kQpOther = 4, kQpCount };
enum Quad2DBlend : uint8_t { kQbUnknown = 0, kQbOff = 1, kQbAlpha = 2 /* SrcAlpha / InvSrcAlpha */, kQbAdd = 3 /* SrcAlpha / One */, kQbOther = 4, kQbCount };
enum Quad2DHave : uint32_t {
  kQhVerts = 1, kQhPosScale = 2, kQhColour = 4, kQhProj = 8, kQhBlend = 16, kQhAlphaTest = 32, kQhPlanes = 64 /* movie: the four plane keys; sprite: the picture key (tex_key[0]) */, kQhTarget = 128, kQhFlush = 256,
  kQhHsl = 512 /* S103 I2: the sprite's hue-shift pixel constant was uploaded (hsl[]): the hue-shifted program */, kQhVariant = 1024 /* S103 I2: another pixel-variant constant (blur / gradation) was uploaded: not a plain sprite */,
  kQhChannel = 2048 /* S103 I3: a GLYPH row: the channel-selector constant c4 (the game's XfmChannelSelector) was known when the glyph was drawn: it is stored in hsl[] (the same 16 bytes, read as the selector) */,
  kQhState = 4096 /* S103 L2a: the game's per-thread state object was read at the op-19 (the `state` byte below; blend / alpha_func / alpha_ref filled from it when its codes are in R8's tables) */,
  kQhMatrix = 8192 /* S103 P3c: the object's 4x4 matrix c29..c32 (proj[16]) was uploaded (op 21, r4 = 29, 4 registers) and is valid for this row: with c28 (use_proj) != 0 the row is MATRIX-PLACED */,
  kQhScissor = 16384 /* S103 SC (layout 4): the scissor state of the draw was read (scissor_on / scissor[]). Without it the row is a HOLE (fail closed: an unknown scissor is never drawn as unclipped; Quad2DRowProblem refuses it) */,
  kQhAddress = 32768 /* S103 RFX (layout 5): the pixel sampler 0 address code was read (address); without it the row clamps (the old rule) */,
  kQhPre3DCat = 65536 /* S105 A4B: a scene-colour row recorded at a frame-walk category below 13 (the first wrestler category): the game draws it BEFORE its 3D (CAF's black quad, category 3); drawn at BeforeOpaque. Not in kQhEverything; old captures read 0 */,
  kQhEverything = 65535,   // (kQhTarget = 128 is also set by a layout-4 row: the target class, viewport and size were read)
};
struct Quad2D {
  uint32_t version = kQuad2DLayout;
  uint32_t frame = 0;            // the guest frame (low 32 bits) the draw was recorded in
  uint32_t seq = 0;              // the call-order ticket (global, ascending; the drawer draws in this order)
  uint32_t site = 0;             // the draw call site: lr - 4 of the op-19 recorder call (0x82BC3EE8 rect, 0x82BC7E50 movie)
  uint32_t object = 0;           // the sprite object (provenance, the join key of DrawOrigin::object)
  uint32_t have = 0;             // Quad2DHave
  uint8_t kind = kQkNone;        // Quad2DKind
  uint8_t pass = kQpUnknown;     // Quad2DPass
  uint8_t blend = kQbUnknown;    // Quad2DBlend
  uint8_t alpha_func = 7;        // VkCompareOp of the alpha test (7 = always); with kQhAlphaTest
  uint8_t prim = 0;              // the op-19 primitive type word (r4), as recorded [U: the value the leaf passes]
  uint8_t nverts = 0;            // vertices read (4 for every kind of this piece)
  uint8_t stride_bytes = 0;      // the op-19 stride (12 rect, 20 movie)
  uint8_t state = 0;             // S103 L2a (was pad, always 0; meaningful only with kQhState, so layout 2 stands): the state object's cache words at the op-19 (quads2d_core.h StateWords): bits 0-3 the game's
                                 // blend code (kStateCodeOutside = a code outside R8's table), bit 4 ZENABLE, 5 ZWRITEENABLE, 6 ALPHABLENDENABLE, 7 ALPHATESTENABLE
  uint32_t target = 0xFFFFFFFFu; // index into Target[] / the image role; 0xFFFFFFFF = unknown
  float alpha_ref = 0.f;         // the alpha test reference
  float use_proj = 0.f;          // fUseProjMatrix (c28.x): must be 0 for a pixel-placed quad
  float verts[4][5] = {};        // x y z u v per corner, as the game computed them (1280x720 root); u v are 0 for a rect
  float pos_scale[4] = {};       // c0: (2/width, 2/height, ...) the game's PosScale
  float color[4] = {};           // c1: the Color constant
  uint32_t tex_key[4] = {};      // the movie planes' handle keys (object +400 / +408 / +416 / +424, word 0), as the tap read them; 0 = none
  uint32_t tex_image[4] = {};    // the planes resolved render-side to a capture image id (the offline rig; the tap leaves 0)
  uint32_t flush_seq = 0;        // movie: the count of plane flushes (sub_827FB540) when the draw was recorded: a draw before its flush is a stale plane
  uint32_t plane_set = 0;        // movie: the display set of the last flush (r3)
  float hsl[4] = {};             // S103 I2: sprite with kQhHsl (S103 I3: a glyph row with kQhChannel holds the channel selector c4 here, all four floats): the pixel constant c2 = (hue shift in turns, chroma added, lightness -1..1, unused 0) [C: listing sub_82BC4890 0x82BC4B08, op 28 reg 2 count 1]
  float proj[16] = {};           // S103 P3c (layout 3): the matrix constants c29..c32 exactly as uploaded: proj[4 * k .. 4 * k + 3] = register 29 + k (sub_82BC2A30 stores the object's matrix TRANSPOSED: register 29 + k = column k of the matrix at object + 336); the game's vertex program: clip[k] = dot(register 29 + k, (x y z 1)); valid with kQhMatrix
  int32_t scissor[4] = {};       // S103 SC (layout 4): with kQhScissor and scissor_on: the rect the game clipped the draw to, x y w h in the guest's 1280x720 target pixels (the game's SetScissorRect left / top / right / bottom: the op-79 record words +16..+28 written by sub_82C2C4B8, as width / height); the drawer intersects it with the target. [C: sub_82CAB280 / reader.h C18: window scissor = viewport ∩ this rect when enabled]
  int32_t viewport[4] = {};      // S103 SC (layout 4): with kQhTarget: the viewport x y w h the draw was recorded with, in GUEST pixels of the colour target it draws into (the game's SetViewport record, op 42, sub_82BCBB68: the D3DVIEWPORT9 ints; after a SetRenderTarget it is the whole surface [U: ADAPTER_CALLS row 1])
  uint32_t target_size[2] = {};  // S103 SC: with kQhTarget: the width / height in guest pixels of the colour target of slot 0 (the surface header's size word, sub_82BB3978 = SetRenderTarget, op 45); `pass` = kQpFinal when it is the 1280x720 frame, kQpOffscreen otherwise (a page / bake target). Unknown = no kQhTarget: the row is a hole
  uint32_t address = 0xFFFFFFFFu; // S103 RFX (layout 5): with kQhAddress: the game's address code 0..7 of pixel sampler 0 (state object +148); 0xFFFFFFFF = not read
  uint32_t scissor_on = 0;       // S103 SC: 1 = the game's scissor test was on for this draw (the state object's cache word +124, set by sub_82C22E08 = vtable slot 26 of the recorder object, which sub_82BB7D00 calls; record id 200); 0 = off (the whole target)
};
static_assert(sizeof(Quad2D) == 324, "Quad2D is a flat POD table");
// S103 SMX (HOL2 IP): a row of the stage light-map PAGE pass: the T_Sprite draw (site 0x82BC5688) into the 512x512 offscreen page (sub_825BAA28 draws page 00 / 01 of the arena's light-map table; the page clock at t = 0 still
// draws page 00 every frame: the Inferno). While our own page is on (stage_lmpage::On) the page is composed by the arena (rf_arena_live.cpp), so the row is not a 2D row the host drops: the census counts it `page_rows`
// and the frame stays 2D-whole. `twin_off` = the planted twin (the old census: the row is a `pass_offscreen` drop). [C: HOL2 IP, log 019 / 017: 600 offscreen rows per 600-frame window]
inline bool IsStagePageRow(const Quad2D& r, bool own_page_on, bool page_ctx, bool twin_off = false) {   // page_ctx (review A3): the record has a kSlPageClock or kSlEntrance stage-look row (a match / entrance with a page); site 0x82BC5688 is the general UI sprite routine, so a menu / CAW 512x512 offscreen row is not a page row
  return !twin_off && own_page_on && page_ctx && r.pass == kQpOffscreen && r.site == 0x82BC5688u && (r.have & kQhTarget) && r.target_size[0] == 512 && r.target_size[1] == 512;
}
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
  kRrRecomputed = 64,     // S103 C3: not a copy of a call: the rope rows our recompute made from the rope grid (src\scene_live\ring_recompute_core.h); call = 3 * side + rope, class = rope_hi stands in (LOD not replicated)
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

// ---- S103 lane A (docs\renderer\INPUTS_BOARD.md): inputs the game computes at run time, captured OUTSIDE the scene walks and written into the record. Ids 29-31; additive (an older reader skips them).
// kScPostParams: the post settings object S (the guest's `[0x8392DD88]`, post_effects.md "The settings object S"): its first 704 bytes as host-order words, read at the frame's latch (the game threads
// parked, after every update task of the frame, before the next one), NOT inside the post node's walk (sub_82C1E840). Every post effect reads its flag and numbers from S by byte offset; word i =
// S + 4 * i. The named offsets and the combine's constants derived from them (c20 / c21 / c27 / c31 / c32) are in src\scene_live\post_settings.h. One row per record.
inline constexpr uint32_t kPostParamsLayout = 1;
inline constexpr uint32_t kPostSettingsWords = 176;   // S+0 .. S+699: covers the DoF block (+8..+32), the flags (+176..+184), the glare angle (+200), the film / defocus / emboss / retro words (+208..+632), the fog block (+656..+692)
enum PostParamsFlags : uint32_t {
  kPpRead = 1,        // S was readable at the latch (without it the row's words are all 0 and a reader must treat the frame's post inputs as missing)
  kPpOn = 2,          // S+0 non-zero: the post module draws at all
  kPpDof = 4,         // S+4
  kPpBloom = 8,       // S+176
  kPpTonemap = 16,    // S+180
  kPpNight = 32,      // S+184 (FX_NIGHT [L])
  kPpFog = 64,        // S+656
  kPpMonotone = 128,  // S+208
  kPpDefocus = 256,   // S+560
  kPpEmboss = 512,    // S+592
  kPpRetro = 1024,    // S+604
  kPpAfterImage = 2048, // S+224 (S103 FXF: yAfterImage, the stun / comeback / signature / finisher ghost)
  // S103 PST (COV9_story.md 2e / 2f): the antialiasing word and the halation / flare / radial-blur words. The halation ones are census-only (no S-rooted writer found: dead unless a block copy fills them).
  kPpAntialias = 4096,  // S+280 (T_Antialiasing, effect 0)
  kPpHala300 = 8192,    // S+300 (effect 5's switch, sub_82C2A540; NOT halation; census only)
  kPpHala304 = 16384,   // S+304 (halation master, ctor 1): no longer set by FlagsOf (S103 HCN: a raw ctor default, always 1)
  kPpHala308 = 32768,   // S+308 (halation trigger, HALA): no longer set by FlagsOf (the gate is kPpHalation)
  kPpRadial = 65536,    // radial blur RUNS: halation gate && S+524 > 0 (S103 HCN, HRB.md 1; was the raw S+524 word)
  kPpHalation = 131072, // effect 6 (halation) RUNS: S+0 && S+304 && S+308 (S103 HCN, HRB.md 1 [C]; census only, not drawn)
};
struct PostParamsRow {
  uint32_t layout = kPostParamsLayout;
  uint32_t flags = 0;                  // PostParamsFlags
  uint64_t frame = 0;                  // the guest frame of the latch that read S (= the record's own frame)
  uint32_t settings_addr = 0;          // the guest address S was read from ([0x8392DD88] at the latch; 0 = not set up yet)
  // S103 FXF: the 'gGsf' node's moment-tint index at the latch (sub_825B7A38, component +16 = index), as code = index + 2: 1 = index -1 = the arena's OWN monotone row (the 30 Years sepia), >= 2 = a moment tint
  // (index >= 0), 0 = not latched (an older record, or no gGsf node ran this frame). It was padding: layout 1 stays, an older writer's 0 reads as "unknown".
  uint32_t gsf_code = 0;
  uint32_t s[kPostSettingsWords] = {};  // S's words, host order (floats are bit patterns)
  // S103 FXF (review fixes 2 / 3 / 6): words beyond S: [kFxGsfT] the gGsf node's blend t (float bits), [kFxArena..+3] the arena's own monotone row as the node wrote it for index -1 (flag, Cb, Cr, Alpha; all 0 = not
  // known), [kFxDispW / kFxDispH] the game's display size [[0x83929A68]+300 / +304] (0 = unreadable: the reader falls back to 1280 x 720 and counts it). A record written before this change has a different
  // element size and its post row is dropped by the reader (inputs_errors): only live / test rows ever carried one.
  uint32_t fx[8] = {};
};
inline constexpr uint32_t kFxGsfT = 0, kFxArena = 1, kFxDispW = 5, kFxDispH = 6, kFxRate = 7;   // S103 FXH: kFxRate = [0x8392DC58] (the game frame rate, a float, bit pattern; 0 = unreadable)
static_assert(sizeof(PostParamsRow) == 24 + 4 * kPostSettingsWords + 32, "PostParamsRow is a flat POD table");

// kScModelRuntime: values of the model object the colour pass reads at the call, read from the object at the latch (the same moment as S). Per known wrestler model (the skeleton object = the model,
// WRESTLER_PROMOTION_MAP.md section 2; `object` is the pairing key of kScEngineObjects). MdlAlpha (VS c185) = the float at [model+252] (the vtable slots 13 / 19 / 31 pass `lfs f1,252(model)` to
// sub_82C04DF0: sub_82C13C88 0x82C13D7C, sub_82C13A80 0x82C13C14, sub_82C13DA8 0x82C13EA0 [C listings]). More words join through the reserved ids 32-34 when their sources are traced.
inline constexpr uint32_t kModelRuntimeLayout = 1;
enum ModelRuntimeFlags : uint32_t {
  kMrAlphaRead = 1,   // [model+252] was readable
  kMrPassModeRead = 2,   // WRK: `reserved` = [model+616] | [model+620] << 8 | (any part's E+20 != 0) << 16 | (E+20 table read) << 24 (the char pass mode; WRK_ADV 5b); 0 = unknown
};
struct ModelRuntimeRow {
  uint32_t layout = kModelRuntimeLayout;
  uint32_t flags = 0;       // ModelRuntimeFlags
  uint32_t object = 0;      // the model's guest address (the record's engine object)
  uint32_t pad = 0;
  uint64_t frame = 0;       // the latch's guest frame
  float alpha = 1.f;        // MdlAlpha
  uint32_t reserved = 0;
};
static_assert(sizeof(ModelRuntimeRow) == 32, "ModelRuntimeRow is a flat POD table");

// kScLightObjects (S103 A2, W2 / L1-L4; docs\renderer\INPUTS_A2.md): RECOMPUTE's first half. The light objects the game's update task writes (sub_827F8F48: the 48 arena sets `(mgr+112)[i]`, the pool `(mgr+140)[j]`, the blend
// `mgr+376`, the override `mgr+368`; all four are POINTERS [C sub_827F6828 `lwz r3,368(r31)`, review]) and the words sub_827F6828 / sub_826203E8 select with, read at the frame's latch (the game threads parked), NOT in a model walk.
// ONE manager row (kind kLoManager) and one row per DISTINCT readable block: `words` = the 896 bytes at object+48 (the block an install copies into R, PER_DRAW_LIGHTS.md 1.1: R+16 = 5 directions, R+96 = 5 colours rgbw,
// R+352 / R+368 the two blocks) as host-order words (floats are bit patterns; kLsLightFloats = 224 of them, the order of LightState::light). Objects with an identical block share one row (most arena sets are the same black
// set); null object slots (sub_827F8808 leaves unused pool / set slots null, normal) have no row. The manager row's `words` map the objects to rows and carry the selection inputs (LightManagerWords).
inline constexpr uint32_t kLightObjectLayout = 3;   // 3 (S103 TNB): the manager row's words carry the entrance track bindings / sizes / keys (kLmTrackBind..; flag kLoTracksRead); a layout < 3 row with the flag is refused. 2 (S103 A3): kLoModelMap / kLoNodeObject rows exist. Layouts 1 and 2 (older captures) are still ACCEPTED, as "no map rows" / "no track words" (a layout-1 section with a map row is refused)
enum LightObjectKind : uint32_t { kLoSet = 0, kLoPool = 1, kLoBlend = 2, kLoOverride = 3, kLoManager = 4, kLoModelMap = 5, kLoNodeObject = 6 };
// kLoModelMap (S103 A3, bit 4; light_index_core.h): `aux0` quadruples (model object, LIGHT OBJECT it was drawn under, raw install index or 0xFFFFFFFE for a direct install, mode) in words[0 .. 4 * aux0 - 1], learned from
// the walk: the install each model's draw follows. One entry per model (the last install wins). At most kLightModelsPerRow per row. kLoNodeObject: a light object that appears in the map but not in the manager's lists
// (the wrestler nodes' EMBEDDED objects at node+3936 / +2736): `object` = its address, words = the 896-byte block read at the latch, like a manager object's.
inline constexpr uint32_t kLightModelsPerRow = 56;
inline constexpr uint32_t kNodeObjectMax = 64;   // kLoNodeObject rows per record
// The ONE accessor of the model map (review must 1): every writer and every reader goes through it, so a layout change cannot be missed by a reader that indexes words by hand.
// Entry k of a kLoModelMap row: model object, the light object its draw followed, the raw install index (kNoIndex 0xFFFFFFFE = a direct install) and the mode.
inline constexpr uint32_t kLightMapWords = 4;   // words per entry
struct ModelMapEntry {
  uint32_t model, object, index, mode;
};
inline ModelMapEntry GetModelMapEntry(const uint32_t* words, uint32_t k) { return ModelMapEntry{words[kLightMapWords * k], words[kLightMapWords * k + 1], words[kLightMapWords * k + 2], words[kLightMapWords * k + 3]}; }
inline void SetModelMapEntry(uint32_t* words, uint32_t k, const ModelMapEntry& e) {
  words[kLightMapWords * k] = e.model, words[kLightMapWords * k + 1] = e.object, words[kLightMapWords * k + 2] = e.index, words[kLightMapWords * k + 3] = e.mode;
}
static_assert(kLightModelsPerRow * kLightMapWords <= kLsLightFloats, "a model-map row must fit its words");
enum LightObjectFlags : uint32_t { kLoBlockRead = 1,   // a block row: the 896 bytes were read; the manager row: every selection word was read
                                   kLoTracksRead = 2 };  // S103 TNB (the manager row, layout 3): the 200 bytes at mgr+148 were read into kLmTrackBind / kLmTrackSize / kLmTrackKey (without the flag those words are 0 and mean NOTHING: 0 would read as slot 0)
inline constexpr uint32_t kLightMaxSets = 64, kLightMaxPool = 32;
inline constexpr uint32_t kLightNoRow = 0xFFFFFFFFu;
// kLoManager `words` layout. The selection rule is the game's own (sub_826203E8 0x82620424-0x8262043C, sub_827F6828):
enum LightManagerWords : uint32_t {
  kLmOverrideMode0 = 0,   // word 0x83622668: the index installed for mode 0 when >= 0 (static data; writers [U]); 0xFFFFFFFF when unread (0 is a valid index)
  kLmOverrideMode1 = 1,   // word 0x8362266C: the same for mode 1
  kLmGateObject = 2,      // word 0x836F53D0: a pointer; non-null with [+152] non-zero skips the mode override
  kLmGateFlag = 3,        // [[0x836F53D0] + 152] (0 when the pointer is null or unreadable)
  kLmRefBase = 8,         // + k: the row index (into the section) holding object k's block, kLightNoRow = null / unreadable. k: set i -> i; pool j -> kLightMaxSets + j; blend -> kLmRefBlend; override -> kLmRefOverride
  kLmRefBlend = kLightMaxSets + kLightMaxPool, kLmRefOverride = kLmRefBlend + 1, kLmRefCount = kLmRefOverride + 1,
  kLmSetInfoBase = kLmRefBase + kLmRefCount,   // + i (i < n): set i's id (low 16 bits, (mgr+116)[i] & 0xFFFF) | (uint16 of (mgr+120)[i] + 1) << 16: the pool remap + 1 (0 = -1 = no remap)
  // S103 TNB (docs\agents\s103\TEN.md 2 / 4): the entrance page's per-track words of the manager, read as ONE 200-byte block at mgr+148 (sub_825BACE0 reads them with sub_827F65C8 / sub_827F65D8; sub_827F65B8 writes the binding).
  // Track i = 0..9 = rig track 50 + i = pool-object track t = 7 + i. Words are host order (swapped at the read); a key is a u64 stored high word first.
  kLmTrackBind = kLmSetInfoBase + kLightMaxSets,   // + i: [mgr + 148 + 4 i] the character slot the track follows (an int; 0..8 = a slot of the table 0x836F38D0; anything else = unbound)
  kLmTrackSize = kLmTrackBind + 10,                // + i: [mgr + 188 + 4 i] (words 47 + i), + 10 + i: [mgr + 228 + 4 i] (words 57 + i): the sprite size ints of a resolved key (the map to a world half size is [U]: carried, not used)
  kLmTrackKey = kLmTrackSize + 20,                 // + 2 i (high), + 2 i + 1 (low): the u64 at [mgr + 268 + 8 i], the track's sprite key (0 = none: the object's default sprite)
  kLmWordsUsed = kLmTrackKey + 20,
};
inline constexpr uint32_t kLightTracks = 10;
static_assert(kLmWordsUsed <= kLsLightFloats, "the manager row's words must fit one row");
struct LightObjectRow {
  uint32_t layout = kLightObjectLayout;
  uint32_t flags = 0;       // LightObjectFlags
  uint64_t frame = 0;       // the latch's guest frame
  uint32_t kind = 0;        // LightObjectKind of the FIRST object that has this block (the others are in the manager row's refs)
  uint32_t index = 0;       // kLoSet: i in (mgr+112); kLoPool: j in (mgr+140); else 0
  uint32_t object = 0;      // the guest address the words were read from (0 for the manager row)
  int32_t remap = -1;       // kLoManager: mgr+372 (non-zero: id 10010 selects the override object); else -1
  uint32_t aux0 = 0;        // kLoManager: the set count n = mgr+124; else 0
  uint32_t aux1 = 0;        // kLoManager: the pool count (mgr+144); else 0
  uint32_t words[kLsLightFloats] = {};
};
static_assert(sizeof(LightObjectRow) == 40 + 4 * kLsLightFloats, "LightObjectRow is a flat POD table");

// kScParamOverrides (S103 A2, W4; INPUTS_A2.md 4): a wrestler model's run-time material parameters. The walk (sub_82C04DF0) takes a parameter's value from the file unless the model's own override table holds an entry with the
// parameter's NAME; only then it records the parameter BY REFERENCE (sub_82BDADA0) to the entry's live pointer [C listing 0x82C07A94-0x82C07AF4 / 0x82C081F0-0x82C08218; run 5 trace: the by-reference sources sit inside the model's own
// storage, +57 KB..+92 KB from the model; BSSSwtMax and MuscleParam were the dynamic names seen]. The override tables, from B = [model+832]: model-wide `[B+1132]`, 20-byte entries, count `[B+1136]`; per part p (0 .. [model+372] - 1)
// `[[B+1140] + 4p]`, count `[[B+1144] + 4p]`. Entry: word 0 = the instance buffer (the 360's 2-slot double buffer, slot 0 is STALE: never read), word 1 = the SOURCE pointer (the live value: the file value, or a relinked controller record's +20),
// word 2 = a `const char*` to the parameter's NAME (sub_82C35118 copies 15 bytes of it into a frame buffer and the walk compares two 8-byte words with the descriptor's name [C review of part 2]; it is NOT a hash),
// word 3 = the byte size the game copies per frame (4 for a whole-register value type, else 4 * word 4), word 4 = the count in floats [C WRK_ADV 1a / 1c].
// A row = one entry as the latch read it: the 20 entry bytes, the NAME (15 characters + NUL) and min(word 3, 64) BYTES at the SOURCE pointer (the rest of `value` is 0). Layout 3 (128 bytes, WRK): the value comes from word 1 (layout 2 read the
// stale slot 0 at word 0, 64 bytes); layout 1 held the name pointer's value only. Older sections are dropped by ParamOverridesProblem (fail closed).
inline constexpr uint32_t kParamOverrideLayout = 3;
inline constexpr uint32_t kParamModelWide = 0xFFFFFFFFu;   // ParamOverrideRow::part of a model-wide entry
enum ParamOverrideFlags : uint32_t { kPoValueRead = 1, kPoNameRead = 2 };
struct ParamOverrideRow {
  uint32_t layout = kParamOverrideLayout;
  uint32_t flags = 0;         // ParamOverrideFlags
  uint64_t frame = 0;         // the latch's guest frame
  uint32_t model = 0;         // the model object (the record's engine object)
  uint32_t part = kParamModelWide;   // the part index p, or kParamModelWide
  uint32_t index = 0;         // the entry's index in its table
  uint32_t entry[5] = {};     // the 20-byte entry, host order
  char name[16] = {};         // the parameter's name (entry[2] points at it), NUL-terminated; empty unless kPoNameRead
  uint32_t value[16] = {};    // entry[3] bytes (the game's memcpy size, max 64) at the SOURCE pointer entry[1] (WRK_ADV 1c; floats are bit patterns); 0 unless kPoValueRead
};
static_assert(sizeof(ParamOverrideRow) == 128, "ParamOverrideRow is a flat POD table");

// kScInputStamps: per registered scene input (src\scene_live\inputs_core.h), the guest frame it was last captured for, in the clock the decision uses (scene_frame.h). `lag` is the lag the input was registered
// with (1 = known only after the walks: the decision accepts the previous frame's stamp). 16 bytes.
struct InputStampRow {
  uint8_t bit = 0;
  uint8_t lag = 0;
  uint8_t moved = 0;        // 1 = a reader outside the walks feeds this input; 0 = registered, no capture yet (the decision answers InputsMissing)
  uint8_t clock = 0;        // the clock `frame` is in: 1 = the HOOK clock (wwe2k14::g_guest_frame; record j carries hook frame j - 1), the one the scene decision uses
  uint32_t reserved = 0;
  uint64_t frame = 0;
};
static_assert(sizeof(InputStampRow) == 16, "InputStampRow is a flat POD table");
// S103 lane S (FRAME_CONTRACT.md 3): kScOwnedPasses rows, the same 16 bytes as rdr_own::OwnedPassStamp (node_switch.h; this header does not include it).
struct OwnedStamp {
  uint8_t pass = 0, decision = 0, why_not = 0, pad = 0;   // rdr_own::Pass, Decision (1 = Ours), WhyNot
  uint32_t inputs_mask = 0;
  uint64_t guest_frame = 0;
};
static_assert(sizeof(OwnedStamp) == 16, "OwnedStamp is rdr_own::OwnedPassStamp's layout");
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
