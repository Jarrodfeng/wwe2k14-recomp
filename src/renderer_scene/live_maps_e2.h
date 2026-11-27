// Lane E2 (Session 103): the inputs and the state of the plane, rim, pre-light and sweat maps (live_map_plane.cpp, live_map_rim.cpp, live_map_prelight.cpp, live_map_sweat.cpp). Continues lane E
// (live_maps.h); docs\renderer\LIVE_MAPS.md section "E2". Everything here is read from the scene record: never from the stream. An input the record does not carry is NAMED in `*_missing`
// (lane A moves it), the map is then not made and the census says why.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowd_picture_cache.h"   // S103 SWF: live::PictureCache (the liquid items' source textures)
#include "light_state_rows.h"
#include "live_map_rope.h"   // S103 B2F
#include "record_io.h"
#include "scene_record.h"
#include "rows_liquid.h"   // S106 G4-2
#include "rows_snapshot.h"   // S108 SHOT-A: LiquidImageRow
#include "rows_pictures.h"
#include "rows_plane.h"
#include "stage_hook.h"
#include "../renderer/renderer.h"

namespace rdr_scene {

// Planted faults of the E2 maps (each must fail its check on the rig).
struct E2Faults {
  bool rim_shadow_camera = false;     // the rim pass is drawn from the shadow light's camera instead of the entry's main-camera product (kCxProduct)
  bool rim_clear_black = false;       // S103 B1F: the map is cleared to (0,0,0,0) as before the fix (the game's is (1,0,1,1) off the casters)
  bool rim_no_depth = false;          // RimLightDepth (c162) is left (0, 0): the depth fade is dead
  bool plane_identity_fit = false;    // the plane's screen fit (PlaneInv, c9-c12) is the identity: the mask is not fitted to the screen
  bool plane_stale_camera = false;    // the plane's camera is the previous frame's
  bool prelight_wrong_light = false;  // the cube is baked with the light directions negated
  bool prelight_no_faces = false;     // only face 0 is drawn
  bool sweat_no_items = false;        // the item list is empty (the map stays as it was)
  bool sweat_flip = false;            // the items' disc corners are mirrored
  bool sweat_unbound = false;         // S103 SWF: the liquid maps are made and kept but never handed to the skin (the twin of the old behaviour: every skin dry)
  bool sweat_no_prev = false;         // S103 SWF: the items are drawn without the snapshot of the map under them (the sweat shader reads an empty previous map: the amount never accumulates)
  bool plane_eye_ring = false;        // S103 E4: the wrestler proxy is drawn with the RING proxy's eye point (1000, -800, 0) instead of its own (-450, -1400, 0)
  bool plane_rope_drop_side = false;  // S103 B2F: the rope proxies of ONE side (the first ring eye kind present) are not drawn: exactly 3 of the 12 lines go
  bool plane_rope_swap_eyes = false;  // S103 B2F: two opposite sides' ropes are drawn with each other's eye: their lines move
  bool plane_rope_none = false;       // S103 B2F: no rope proxy at all (the state before this lane)
  bool Any() const { return rim_clear_black || rim_shadow_camera || rim_no_depth || plane_identity_fit || plane_stale_camera || prelight_wrong_light || prelight_no_faces || sweat_no_items || sweat_flip || sweat_unbound || sweat_no_prev || plane_eye_ring || plane_rope_drop_side || plane_rope_swap_eyes || plane_rope_none; }
  static bool Parse(const std::string& list, E2Faults& f, std::string& error);
};

// One pre-light slot's constants: the draw's own pixel bank (PRELIGHT_DIRECT.md section 8.1 PreLightK; yPreLight_sss_fragment c8-c11 DifLghVec, c0-c3 DifLghCol, c198, c16-c19, c199, c204-c209).
struct PreLightSlotInput {
  uint32_t light_set = 0;          // the light set id the slot bakes (10010, the first character's set, 10, 11, 14, 12, 13 ...)
  float dif_lgh_vec[4][4] = {};    // c8-c11
  float dif_lgh_col[4][4] = {};    // c0-c3
  float dif_lgh_max_d[4] = {};     // c198
  float dif_lgh_prm[4][4] = {};    // c16-c19
  float dif_lgh_max_a[4] = {};     // c199
  float sss_lobe_a[3][4] = {};     // c204-c206
  float sss_lobe_b[3][4] = {};     // c207-c209
};
// S103 SWF: one wrestler's liquid map this frame, as the record carries it (kScLiquidMaps + its kScLiquidItems rows); the pictures are the record's kScLiquidPictures rows (E2Input::liquid_pictures).
struct LiquidMapIn {
  LiquidMapRow row;
  std::vector<LiquidItemRow> items;
};
// S108 SHOT-A: a saved record's image of one map (section 69; `bytes` points into the record's section 70, which outlives the frame).
struct LiquidImageIn {
  LiquidImageRow row;
  const uint8_t* bytes = nullptr;
};

// A short list of input names that are missing (fixed storage: no allocation per frame).
struct NameList {
  const char* v[8] = {};
  uint32_t n = 0;
  void Add(const char* s) { if (n < 8) v[n++] = s; }
  const char* First() const { return n ? v[0] : ""; }
};

inline constexpr float kPlaneEyeWrestler[4] = {-450.f, -1400.f, 0.f, 1.f};
inline constexpr float kPlaneEyeRing[4] = {1000.f, -800.f, 0.f, 1.f};   // one of the ring proxy's four points (the plant)
inline constexpr float kPlanePosDefault[4] = {0.f, -120.f, 0.f, 0.f};

struct E2Input {
  // rim: the main camera as the rim pass entry read it, the depth range, the casters = the shadow map's
  bool have_rim = false;
  const char* rim_why = "";
  CasterRows rim_rows = {};
  uint32_t rim_w = 512, rim_h = 256;       // the game's rim map size (RIM_MAP_OURS.md: image 1189, 512x256 RGBA8)
  uint32_t rim_calls = 0;                  // rim pass entries of the record
  uint32_t rim_stamp = 0;                  // the ONE stamp the rim entry was read at
  // plane: the module's per-frame constants and the per-draw part (kScPlaneFrames / kScPlaneDraws), the proxy palettes (kScCasterMats)
  bool have_plane = false;
  bool plane_nodraw = false;               // S103 PB: the chosen plane frame is the game's no-draw frame (kPlaneNoDraw: its screen fit collapsed, the plane covers no pixel): not a hole, no map is made
  const char* plane_why = "";
  PlaneFrame plane_frame = {};
  PlaneFrame plane_prev = {};              // the previous stamp's block (the stale-camera plant)
  bool have_plane_prev = false;
  float plane_pos[4] = {};                 // PlanePos (c7): from the plane calls' CasterExtra::pose_t (kCxPlanePos), or told
  bool have_plane_pos = false, plane_pos_from_record = false;
  uint32_t plane_stamp = 0;                // the ONE stamp the plane tables were read at
  uint32_t plane_calls = 0;                // plane pass entries of the record (the proxies)
  NameList plane_missing;                  // inputs the record does not carry (lane A)
  // EyePos (VS c72): no reader at its writer yet, so no record carries it. The offline rig is TOLD it (maps_draw --told), as it is told which character stands in a slot; live it is lane A's reader.
  bool plane_eye_told = false;
  // S103 E4: the plane mask's EyePos is a per-object-class CONSTANT, not camera data [C: brock_losswin f110 stream, 32 mask draws: the wrestler proxy (-450, -1400, 0, 1), the ring proxy one of (+-1000, -800, 0) / (0, -800, +-1000); PLANE_OURS_WHOLE.md]; PlanePos is (0, -120, 0, 0) in all 32 draws [C same frame].
  // The rig's --const-eye applies them as a fixture (flagged); BuildE2Input does not (review F2: per-arena data, one arena of evidence).
  bool plane_eye_const = false, plane_pos_const = false;   // OFFLINE FIXTURE ONLY (maps_draw --const-eye): brock_losswin's arena values; BuildE2Input never sets them
  float plane_eye[4] = {};
  // S103 E5: the record's PlaneEyes (kScPlaneEyes, read at the latch from the plane module's tables): the eye per module kind (1 = the wrestler proxy, 4..7 = the ring's), and PlanePos. plane_eye = eye[1] then.
  bool plane_eye_record = false;           // plane_eye came from the record
  uint32_t plane_eye_have = 0;             // bit k = plane_eye_kind[k] valid
  float plane_eye_kind[kPeKinds][4] = {};
  uint32_t plane_eyes_stamp = 0;           // the ONE stamp the eyes were read at
  bool plane_told = false;                 // plane_frame / plane_draws came from the rig's told file, not the record
  RopeInput rope;                          // S103 B2F: the ring ropes' node rows (kScRingRows / kScRingNodes) and the arena: the ropes' shadow proxy is drawn from them
  // pre-light: the record has no section for it yet: the list stays empty and `prelight_missing` says so
  std::vector<PreLightSlotInput> prelight_slots;
  // S103 SWF: the sweat / blood liquid maps (kScLiquidMaps / Items / Pictures, written by src\scene_live\liquid_*.h at the sweat node's entry). Empty = a dry record (nothing live, or an older record): not a missing input.
  std::vector<LiquidMapIn> liquid;
  std::vector<LiquidImageIn> liquid_images;   // S108 SHOT-A: section 69: a map with an image starts FROM it and its frame's items are not painted again (the image already holds them); empty for every live record
  std::vector<const LiquidPictureRow*> liquid_pictures;   // only the pictures an item of `liquid` names: POINTERS into the record's kScLiquidPictures rows (the record outlives the frame; no 32 KB copies)
  NameList prelight_missing, sweat_missing;
};
void BuildE2Input(const SceneTables& t, const E2Faults& faults, E2Input& out);

// Per-hook state of the E2 maps (images, programs): defined here so live_maps.h only holds a pointer.
struct E2State {
  // rim
  rdr::ImageId rim_color = rdr::kNoImage, rim_depth = rdr::kNoImage;
  uint32_t rim_w = 0, rim_h = 0;
  int rim_vs_skinned = -1, rim_vs_single = -1, rim_fs = -1;
  uint16_t rim_prog_skinned = 0, rim_prog_single = 0, rim_layout_skinned = 0, rim_layout_single = 0;
  bool rim_ready = false;
  // plane
  rdr::ImageId plane_color = rdr::kNoImage, plane_depth = rdr::kNoImage;
  uint32_t plane_w = 0, plane_h = 0;
  uint16_t plane_prog = 0, plane_layout_skinned = 0;
  bool plane_ready = false;
  // S103 L9: the plane chain behind the mask: the 256 box downscale (kept as the first blur pass's source), the two blur passes, our own blur program over a fullscreen quad of plane coordinates
  rdr::ImageId plane_half = rdr::kNoImage, plane_blur_a = rdr::kNoImage, plane_blur_b = rdr::kNoImage;
  rdr::BufferId blur_vb = rdr::kNoBuffer;
  uint16_t blur_prog = 0, blur_layout = 0;
  bool blur_ready = false;
  // S103 L14: the game's 512 -> 256 downscale is yDownScale_box4 (taps at +-1 texel of the 512 = a centred 4x4 box), not the 2x2 box a linear Copy gives
  int ds_vs = -1, ds_fs = -1;
  uint16_t ds_prog = 0, ds_layout = 0;
  bool ds_ready = false;
  // pre-light: one cube strip image per slot (384x64: six 64x64 faces side by side, as the game's strip), the cube itself (6 layers) is the copy of it
  std::vector<rdr::ImageId> prelight_strips;
  rdr::BufferId pl_vb = rdr::kNoBuffer, pl_ib = rdr::kNoBuffer;
  uint32_t pl_index_count = 0;
  uint16_t pl_prog = 0, pl_layout = 0;
  int pl_vs = -1, pl_fs = -1;
  bool prelight_ready = false;
  // sweat (S103 SWF, live_map_sweat.cpp): one persistent liquid map per character, painted from the game's own items (docs\systems\liquid_map.md)
  rdr::ImageId sweat_color = rdr::kNoImage;   // (the offline tools read it: the last map painted)
  bool sweat_ready = false;
  struct LiquidState {
    rdr::ImageId image = rdr::kNoImage;   // 1024 x 512 RGBA8, 3 mips: x blood, y sweat, z gloss (fx_skin.glsl); lives until the wrestler leaves
    uint16_t w = 0, h = 0;
    uint8_t levels = 1;
    bool painted = false;                 // something was drawn since the last clear: only then is it handed to the skin (a dry map keeps the dry variant, byte for byte)
    bool dim = false;
    uint32_t object = 0;                  // the character's skeleton object = the record's engine object of its body: the key the front-end joins by
    uint32_t last_row_frame = 0;          // the hook frame of the last row applied (a frame drawn twice by the offline rig paints once)
    uint64_t seen = 0;                    // the last hook call (Record count) whose frame held `object`
    uint64_t items_drawn = 0, clears = 0;
    uint32_t clear_gen = 0;               // the last clear generation seen (LiquidMapRow::reserved[0]); a changed one clears even when the row that carried the request was dropped
    bool pending_clear = false;           // a clear that could not be applied (no resources yet): kept until a row can
    uint32_t rebuild_gen = 0;             // S108 BLD3: the last rebuild package generation applied (LiquidMapRow::reserved[1]); the hook re-sends a package until this is acknowledged
  };
  std::unordered_map<uint32_t, LiquidState> liquid;   // by the guest's map object
  rdr::ImageId liquid_prev = rdr::kNoImage;           // the snapshot of the map under the item rects (the sweat shader reads the previous map at slot 0; a render target cannot be read while it is written)
  rdr::BufferId liquid_disc_vb = rdr::kNoBuffer, liquid_disc_ib = rdr::kNoBuffer, liquid_quad_vb = rdr::kNoBuffer;
  uint32_t liquid_disc_indices = 0;
  int liq_vs_local = -1, liq_vs_base = -1, liq_fs_sweat = -1, liq_fs_sweat_dim = -1, liq_fs_paste = -1, liq_fs_copy = -1, liq_fs_copy_dim = -1;
  int liq_fs_copy_q4 = -1;                            // S111 LIQUID4BIT: copy_prev rounding each channel to 1 / 15 (the game's resolve into its k_4_4_4_4 map texture; docs\systems\liquid_map.md section 5a)
  uint16_t liq_prog_sweat = 0, liq_prog_sweat_dim = 0, liq_prog_paste = 0, liq_prog_copy = 0, liq_prog_copy_dim = 0, liq_prog_copy_q4 = 0, liq_layout = 0;
  bool liquid_ready = false;
  std::unique_ptr<live::PictureCache> liquid_pics;
  // per frame (reset by RecordSweat) and totals (census)
  uint32_t liq_maps_frame = 0, liq_items_frame = 0, liq_published = 0;
  uint64_t liq_frames = 0, liq_maps_total = 0, liq_items_total = 0, liq_clears = 0, liq_pruned = 0;
  uint64_t liq_images_applied = 0, liq_skip_image = 0;   // S108 SHOT-A: saved images uploaded / frame rows left unpainted because their map came from an image
  uint64_t liq_skip_no_pic = 0, liq_skip_type = 0, liq_skip_state = 0, liq_skip_rect = 0, liq_skip_resources = 0, liq_skip_repeat = 0, liq_no_object = 0, liq_unmatched = 0, liq_no_variant = 0, liq_skip_size = 0;
  // S108 BLD2: the game's rebuild draw (sub_8279A328; liquid_plan.h): rows, its sweat discs (mask B), its region quads (blood R / sweat G), rows with nothing to draw, rebuild rows / regions without a picture (shortfall)
  int liq_vs_plain = -1, liq_fs_tint = -1, liq_fs_tint_dim = -1;
  uint16_t liq_prog_tint = 0, liq_prog_tint_dim = 0;
  rdr::BufferId liquid_region_vb = rdr::kNoBuffer;   // the unit quad of the plain vertex shader: position = uv = the corner
  uint64_t liq_regions_mirrored = 0, liq_discs_mirrored = 0, liq_items_mirrored = 0;   // S108 BLD4: drawn with a mirrored box (the game's own look) / live items held back for it (a counted hole)
  uint64_t liq_alt_rows = 0, liq_rebuild_drawn = 0, liq_regions_drawn = 0, liq_alt_empty = 0, liq_alt_no_pic = 0, liq_alt_late = 0;   // (S108 BLD3: liq_alt_rows = packages applied, liq_alt_late = of them on a later row)
  // counters (relaxed; census strings built at print time)
  uint64_t rim_made = 0, plane_made = 0, prelight_made = 0, sweat_made = 0;
  uint32_t rim_objects = 0, rim_meshes = 0, plane_draws = 0, prelight_slots = 0, sweat_items = 0;
  const char* rim_skip = "";
  const char* plane_skip = "";
  uint64_t plane_blank_frames = 0, plane_blank_passes = 0;   // S103 SLV: frames the plane chain was left out because its ground square is entirely behind the camera (plane_blank_rule.h, SLV2: never the weight alone), and the passes that saved (the mask only: the rest of the chain never ran in such a frame)
  uint64_t plane_seen = 0, plane_no_caster_frames = 0, plane_chain_failed_frames = 0;   // S103 MSF: RecordPlane calls with a reader attached (made vs read), and the two late skips by their own reason
  uint64_t plane_mask_passes = 0, plane_mask_empty = 0;   // S103 CHS C4: mask passes recorded (SetTargets + Clear + the caster draws), and those that ended with no draw at all (a Clear-only pass whose map no reader gets: "no caster meshes drawn"). A counter only: the pass stays on
  uint64_t plane_nodraw_frames = 0;   // S103 PB: frames the plane pass was skipped because the game drew none (kPlaneNoDraw)
  std::unique_ptr<RopeProxyState> rope;   // S103 B2F: the rope plane-shadow proxy (ring entry 956): mesh, buffers, this frame's notes
  bool plane_not_read = false;   // S103 E4: the plane pass was not drawn because nothing reads its map (not a missing input: not a hole)
  const char* prelight_skip = "";
  const char* sweat_skip = "";
};

}  // namespace rdr_scene
