// Lane E (Session 103): the maps the colour pass reads, made by OUR passes from the scene record, before the colour pass, and handed to it by role
// (stage_hook.h LiveMaps; docs\renderer\FRAME_CONTRACT.md sections 5 and 8; docs\renderer\LIVE_MAPS.md). No stream anchors, no stream copies: the inputs are
// the record's tables (MapsInput), the outputs are our images in LiveMaps.
//
// One StageHook at Stage::Maps (LiveMapsHook). Each map lives in its own source file:
//   live_map_shadow.cpp    the wrestlers' / casters' shadow map (ShadowMap): casters from the record's objects + palettes, the light from the caster pass states
//   live_map_circles.cpp   the circle blob shadows (Circles): the record's CircleShadow rows + the disc mesh
//   (plane, rim, pre-light, sweat: not built yet; MapsCounts::not_built names them, see the page)
// Includes the renderer, the asset model and the record only: no game, no SDK, no viewer.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>   // S103 L9
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/asset_model.h"
#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"
#include "light_state_rows.h"
#include "live_maps_e2.h"   // S103 E2: the plane, rim, pre-light and sweat maps' inputs and state
#include "record_io.h"
#include "scene_record.h"
#include "rows_circle.h"   // S106 G4-2
#include "stage_hook.h"

namespace rdr_scene {

// Planted faults (every one must fail its check on the rig; the controls pass).
struct MapsFaults {
  bool transpose_light = false;   // the light view-projection is handed to the shader transposed (the shadow lands in the wrong place)
  bool no_casters = false;        // the caster list is empty (nothing is drawn into the shadow map)
  bool circles_wrong_frame = false;  // the circle rows come from another frame's record (the stamp rule must refuse them)
  bool scale_prm = false;         // the shadow range (ShadowPrm c168) is scaled by 2: every stored distance is wrong by a factor
  bool circles_all_stamps = false;  // the one-stamp rule is off and every entry is duplicated with stamp - 1 (catch-up entries): every character is drawn twice
  // S103 E3: the fitted light camera's plants (shadow_fit.h ShadowFitFaults); the fit is used for the whole run of a check that names one
  bool fit_view_not_light = false;   // the caster matrix is built from the main camera's forward vector instead of the light's
  bool fit_key_not_negated = false;  // the key vector is used as the travel direction (the shadow falls toward the light)
  bool fit_no_bounds = false;        // the casters' bounds are not measured (the zoom is the minimum on every scene)
  bool circles_no_mesh = false;      // S103 E4: the record's disc mesh is dropped: the circle map must become a counted HOLE (nothing drawn, never an empty map)
  // S103 PMS (the props map the floor reads): each must fail its bar in tools\scene\pms_check.py
  bool props_reader_mapb = false;    // the old binding: the floor is handed the WRESTLERS' light (TexProjLight c68-c71 and ShadowPrm of map B): the S98 'long stretched shadows on the floor'
  bool props_bias_order = false;     // the bias is applied in the other order of the product (to the components of each register, not to the registers)
  bool props_no_blobs = false;       // the blobs are drawn into their own image again (role Circles): no props map is handed to the floor
  // S103 MBC (RMP.md 2): each must fail its bar (tools\scene\mapb_check.py / weapon_plan unit)
  bool mapb_wrestlers_only = false;  // the props are never drawn into map B (today's set: the past behaviour re-found)
  bool mapb_casts_gm = false;        // the cast tag is ignored: every opaque non-wrestler object (gm sets, blood, stage parts) casts into map B
  // S103 PM2 (the props map's prop casters): each must fail its bar (tools\scene\pm2_check.py / the weapon_plan unit)
  bool props_no_props = false;       // the props map is made without its prop casters (PMS's blobs-only map: the past behaviour re-found)
  bool props_viewdepth_row2 = false; // the casters' ViewDepth is the product's row 2 (the clip z) instead of row 3 (the floor's ViewLight[2]): the stored depth no longer matches the receiver's
  bool props_casts_gm = false;       // the props map's cast tag is ignored for gm sets / blood (they cast into it)
  bool Any() const { return props_viewdepth_row2 || props_no_props || props_casts_gm || mapb_wrestlers_only || mapb_casts_gm || transpose_light || no_casters || circles_wrong_frame || scale_prm || circles_all_stamps || fit_view_not_light || fit_key_not_negated || fit_no_bounds || circles_no_mesh || props_reader_mapb || props_bias_order || props_no_blobs; }
  static bool Parse(const std::string& list, MapsFaults& f, std::string& error);  // "a,b"
};

// What the record gives the maps (built by BuildMapsInput from one SceneTables; the hook reads it, never the record).
struct MapsInput {
  // the wrestlers' shadow light: the state the MAP pass entry of the first wrestler (a call with a palette, not the collapse-A one) read
  bool have_light = false;
  const char* light_why = "";             // why not, or where it came from
  CasterRows rows = {};                   // VS c56-59 ViewProj, c54 ViewDepth, c64-67 Trapezoidal, PS c168 ShadowPrm (light_state_rows.h BuildCasterRows)
  WrestlerCameraRows cam = {};            // TexProjLight c68-71 (the readers' projection), light eye
  uint32_t map_w = 512, map_h = 512;      // the shadow map size the record knows (LightState::map_wh, clamped 64..4096); have_light is false when it is unknown or out of range
  uint32_t circle_w = 512, circle_h = 512;   // the circle map's own size (the props map's: 512, SHADOW_PROMOTION_MAP 1.1); the record does not carry it
  bool has_caster_records = false;        // the record has kScCasterCalls at all (false: the writers did not run: "inputs missing")
  bool has_circle_records = false;        // the record has kScCircleShadows at all
  bool light_degenerate = false;          // S103 PMC2: the record carries a kPlDegenerate props-light row: the game's light is degenerate this frame, its props pass draws nothing (PMH.md); NOT a missing input
  // S103 PMS: the props map's floor ShadowPrm (PS c168-c170) from the colour-call light state that holds the props camera (props_map_rows.h PrmFromRecord); invalid = a counted hole, never a constant
  bool props_prm_valid = false;
  float props_prm[3][4] = {};
  const char* props_prm_why = "";
  uint32_t light_stamp = 0, circle_stamp = 0;   // the one stamp each input was taken at
  // the circle blobs of THIS frame
  std::vector<CircleShadow> circles;      // ONE stamp's entries, at most one per slot (see BuildMapsInput)
  uint32_t circles_rejected_stamp = 0;    // entries in the record stamped for another frame (or a second entry of a slot)
  uint32_t circles_incomplete = 0;        // an entry whose ViewProj / ShadowParam the writer could not read (never drawn)
  bool have_circle_mesh = false;
  CircleMesh circle_mesh = {};
  uint64_t guest_frame = 0;               // the record's frame
  E2Input e2;                             // S103 E2: the plane, rim, pre-light and sweat inputs (live_maps_e2.h)
  // S103 E3: the wrestlers' shadow light camera RECOMPUTED (shadow_fit.h) when the record has no caster pass entry (the game's walk is skipped), or always when `force_fit`.
  bool force_fit = false;                 // use the fitted camera even when a caster state is in the record (the offline check, the landing's frames)
  bool fit_key_valid = false;             // the wrestlers' key light vector was found
  bool fit_key_told = false;              // ... and it was TOLD to the rig (a fixture: the record carries no light rows), not read from the record
  float fit_key[3] = {};
  const char* fit_key_why = "";           // where the key came from, or why there is none
  float fit_main_forward[3] = {};         // the view_not_light plant only
  bool casters_expected = false;          // the record has a wrestler actor: a shadow map is expected
  bool shadow_hole = false;               // review 2: the shadow map is expected, would be fitted, and its key light is missing: an INPUT is missing (DrawLive counts the frame a miss)
};
// What the rig may tell the hook for an input the record does not carry yet (OFFLINE ONLY; the live path reads the record).
struct MapsFitOptions {
  bool force_fit = false;
  bool have_told_key = false;
  float told_key[3] = {};                 // the wrestlers' KeyLightVec (VS c12 of a wrestler colour draw)
};
// A record holds the entries of the frame it describes (stamp = header - 1 measured, SF1 run 7147) and may hold older catch-up entries (g-1, g-2: SKIP_GAME_WORK_MAP 1351). ONE stamp is chosen per record:
// header - 1 when present, else the newest at or below the header; never a future one. kStampWindow = how far below the header an entry may be.
inline constexpr uint32_t kStampWindow = 4;
// `other` = another record's tables whose circle rows are used instead of this record's (the circles_wrong_frame plant); null = this record's.
void BuildMapsInput(const SceneTables& t, const MapsFaults& faults, const SceneTables* other, MapsInput& out);
void BuildMapsInput(const SceneTables& t, const MapsFaults& faults, const SceneTables* other, MapsInput& out, const MapsFitOptions& fit);   // S103 E3

struct MapsCounts {   // per frame (reset by Record at the Maps stage). RENDER THREAD ONLY: Record, counts() and CensusLine() are all called by the thread that records the frame
  uint32_t guest_casters = 0;        // S103 CS1B: of casters_objects, the guest models (ModelCache::PutGuest: the editor body, mods, created superstars).
  uint32_t casters_objects = 0;      // record objects drawn into the shadow map
  uint32_t casters_meshes = 0;       // mesh draws
  uint32_t casters_skipped_no_model = 0, casters_skipped_hidden = 0, casters_skipped_not_char = 0;
  uint32_t circles_drawn = 0, circles_rejected_stamp = 0, circles_incomplete = 0;
  const char* props_skip = "";       // S103 PMS: why no PropsMap was handed over this frame ("" = handed over, or the switch is off)
  uint32_t maps_made = 0;            // entries in LiveMaps this frame
  const char* shadow_skip = "";      // why the shadow map was not made this frame ("" = made)
  const char* circles_skip = "";
  bool props_degenerate = false;     // S103 PMC2: this frame's props map was not made because the game's light is degenerate (the game draws none): counted apart from holes
  // S103 E3
  bool shadow_fitted = false;        // the shadow camera was fitted by shadow_fit.h this frame (not read from a caster state)
  float fit_radius = 0.f, fit_scale = 0.f;   // the casters' bounding radius and the squeeze
  uint32_t fit_samples = 0;          // skinned sample points measured for the bounds
  float fit_center[3] = {}, fit_lo[3] = {}, fit_hi[3] = {};   // the measured box (game space; y is DOWN there: the floor is fit_hi[1])
  // S103 MBC: map B's prop casters (RMP.md 2). props cast = tagged objects drawn (meshes counted apart); skipped_mode1 = the game's mode 1 (scene word 1 with >= 3 participants: wrestlers only); skipped_hole = the gate could not be read
  // (phase word / participant count unknown: wrestlers only, counted); skipped_no_model = the posed copy is not resident / not kept; out_of_fit = tagged casters whose bounds lie wholly outside the fitted sphere (only counted with a fitted camera)
  uint32_t mapb_props_cast = 0, mapb_props_meshes = 0, mapb_skipped_mode1 = 0, mapb_skipped_hole = 0, mapb_skipped_no_model = 0, mapb_out_of_fit = 0, mapb_skipped_piece43 = 0, mapb_near_clip = 0;
  uint32_t mapb_fam_weapon = 0, mapb_fam_event = 0, mapb_fam_stage = 0, mapb_fam_unsure = 0;   // casters by family (unsure = ids 59 / 68 / 71 / 96: cast, category unknown)
  bool mapb_ring_gate = false;       // [0x836F39A0] was set this frame: the game's map B would also cast category 24 (the ring): NOT built, a counted hole
  const char* mapb_mode = "";        // "0" / "1" / "hole: ..." / "off"
  // S103 PM2: the props map's prop casters (live_map_props.cpp DrawPropCasters; PMC.md). cast = tagged objects drawn (meshes apart); by family (unsure = ids 59 / 68 / 71 / 96); skipped_no_model = the posed copy is not resident;
  // skipped_piece43 = ids 2064 / 2556 (category 43: the props pass never draws it); near_clip = a caster whose nearest sample is nearer the light than the map's near distance (the depth clamp takes it onto the near plane);
  // skipped_no_light = casters existed but no props light / map was made this frame (a counted hole, the PropsMap hole's reason is in props_skip). why = "" when drawn, or the reason none were.
  uint32_t props_cast = 0, props_meshes = 0, props_fam_weapon = 0, props_fam_event = 0, props_fam_stage = 0, props_fam_unsure = 0, props_skipped_no_model = 0, props_skipped_piece43 = 0, props_near_clip = 0, props_skipped_no_light = 0;
  const char* props_cast_why = "";
  uint32_t holes = 0;                // maps that were NOT made for lack of an input this frame (each with its reason in the *_skip string): never a silent empty map
};

struct ChainState;   // S103 L7 (live_maps_chain.h)

class LiveMapsHook final : public StageHook {
 public:
  // The model of a front-end asset index (SceneFrame::objects[].asset), or null: the casters' geometry is built from it once (the same assets::Model the front-end was given).
  using ModelProvider = std::function<const assets::Model*(uint32_t asset)>;
  LiveMapsHook(rdr::Renderer& renderer, ModelProvider provider);
  ~LiveMapsHook() override;
  LiveMapsHook(const LiveMapsHook&) = delete;
  LiveMapsHook& operator=(const LiveMapsHook&) = delete;

  // This frame's inputs (null = none: nothing is made). The pointer is read by the next Record only and cleared at its end: set it again every frame.
  void SetInput(const MapsInput* input, bool repeatable = false) { input_ = input, repeatable_ = repeatable; }   // S103 E3: repeatable = the same input serves every Record until the next SetInput (offline rig only)
  // The front-end released (or replaced) the model of `asset`: its caster geometry is dropped (destroyed a few frames later: no wait). The provider's answer is also compared by pointer every frame.
  void ForgetAsset(uint32_t asset);
  void SetFaults(const MapsFaults& f) { faults_ = f; }
  // S103 MBC: the CPU copy of a posed prop model (weapon / event prop / structure piece) by front-end id, and a serial that moves each time the pose changes (rf::ModelCache::PropCpu). Null = not kept.
  using PropProvider = std::function<const assets::Model*(uint32_t asset, uint32_t& serial)>;
  void SetPropProvider(PropProvider p) { prop_provider_ = std::move(p); }
  // S103 CS1B: true when the front-end model id is a guest model (census `guest casters`; null = none counted).
  using GuestPredicate = std::function<bool(uint32_t asset)>;
  void SetGuestPredicate(GuestPredicate p) { guest_pred_ = std::move(p); }
  // S103 E4: the plane mask is drawn only while something reads MapRole::PlaneShadow (lane B / C's receivers: none yet). The offline drivers set it; the live drawer does not.
  void SetPlaneReader(bool attached) { plane_reader_ = attached; }
  const MapsFaults& faults() const { return faults_; }
  void SetE2Faults(const E2Faults& f) { e2_faults_ = f; }   // S103 E2
  void SetBlockingLoads(bool on) { blocking_loads_ = on; }   // S103 B2F: the offline rigs draw ONE frame: the rope proxy's mesh is waited for instead of arriving a frame later
  void SetGameRoot(const std::string& root) { game_root_ = root; }   // S103 B2F: where the rope proxy's arena pac is read from (the live drawer, the rigs); empty = the proxy is not drawn
  const E2State& e2() const { return *e2_; }                // S103 E2
  // S108 SHOT-A (the subject-shot saver, src\renderer_host\scene_host.cpp): the persistent liquid images that hold paint (the ones the skin reads): the map key, its character's skeleton object, the image and its
  // size. Read-only; call on the thread that runs Record (the host thread) between frames.
  struct LiveLiquidImage { uint32_t map = 0, object = 0; rdr::ImageId image = rdr::kNoImage; uint32_t width = 0, height = 0; };
  void LiveLiquidImages(std::vector<LiveLiquidImage>& out) const;
  const char* Name() const override { return "live_maps"; }
  void Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& builder) override;

  const LiveMaps& maps() const { return maps_; }   // SceneFrame::maps points at this
  const MapsCounts& counts() const { return counts_; }
  // The images the maps are drawn into (for the offline sheets).
  rdr::ImageId shadow_image() const { return shadow_color_; }
  rdr::ImageId plane_map_image() const { return e2_->plane_blur_b; }   // S103 L9: the plane shadow map the ring mat reads (256, after the chain), for the offline sheets
  rdr::ImageId plane_mask_image() const { return e2_->plane_color; }   // S103 L9: the raw 512 mask
  rdr::ImageId circles_image() const { return circles_color_; }
  rdr::ImageId props_image() const { return circles_color_; }   // S103 PMS: the props map IS the circle image (blobs in blue); the rig reads it as ours_props.png
  std::string CensusLine() const;   // "[scene][maps] frames N, shadow made M, circles drawn C, ..." (strings built here, never per draw)

 private:
  struct CasterMesh {
    uint32_t vb_offset = 0, ib_offset = 0, index_count = 0, sv_offset = 0;
    int src = 0;                    // Object::mesh_mask bit (the front-end's source mesh ordinal)
    bool chunk = false;             // skinned chunk: `nodes` + the skin stream are valid
    std::vector<uint16_t> nodes;    // palette slot -> skeleton node
    uint32_t sample_first = 0, sample_count = 0;   // S103 E3: this piece's bounds samples in CasterModel::samples
  };
  struct SamplePt { float p[3]; uint16_t slot; };   // S103 E3: a bind-space vertex and the palette slot of its first bone (the bounds are measured with the dominant bone only)
  struct CasterModel {
    const assets::Model* source = nullptr;   // the model it was built from (a different pointer from the provider = rebuilt)
    std::vector<CasterMesh> meshes;
    std::vector<SamplePt> samples;           // S103 E3: every 8th vertex of each piece
    rdr::BufferId vb = rdr::kNoBuffer, ib = rdr::kNoBuffer, svb = rdr::kNoBuffer;
    uint64_t last_used = 0;   // S103 MBC review 3: the frame (frames_) a prop caster was last asked for; prop_cache_sweep.h retires the stale
    uint32_t serial = 0, vb_bytes = 0, ib_bytes = 0;   // S103 MBC: a prop's pose serial and its buffers' sizes (a re-pose of the same shape rewrites vb in place)
  };
  const CasterModel* CasterOf(uint32_t asset, bool prop = false);   // S103 MBC: prop = a tagged weapon / event prop / piece (opaque meshes of any effect, rigid), kept in prop_casters_
  void ReleaseCasters();
  void RetireModel(CasterModel& m);                       // buffers go to the graveyard
  void Retire(rdr::BufferId b);
  void Retire(rdr::ImageId i);
  void SweepGraveyard();                                  // destroys what has waited kGraveFrames frames (Record, render thread)
  static constexpr uint64_t kGraveFrames = 4;
  struct Grave { uint64_t frame; rdr::BufferId buffer; rdr::ImageId image; };
  std::vector<Grave> graveyard_;
  std::vector<assets::Matrix> slots_scratch_;             // per-mesh scratch, kept across meshes
  std::vector<float> rows_scratch_;
  std::vector<uint8_t> block_scratch_;

  // S103 E3: the casters' bounding sphere from sampled, skinned vertices (game space); false when no caster is resident. `samples_out` = points measured.
  bool MeasureCasters(const SceneFrame& frame, float center[3], float& radius, uint32_t& samples_out);
  bool MeasureRimDepth(const SceneFrame& frame, const float vp[16], float& near_w, float& far_w, uint32_t& samples_out);   // S103 L7: the depth range of the ON-SCREEN casters' sample points along the camera
  bool MeasuredOnce(const SceneFrame& frame, float center[3], float& radius, uint32_t& samples_out);   // review 6: one measurement per Record (shadow and rim share it)
  struct Measured { bool tried = false, ok = false; float c[3] = {}; float r = 0.f; uint32_t n = 0; } meas_;
  bool RecordShadow(const StageCtx& ctx, rdr::FrameBuilder& fb);
  bool RecordCircles(const StageCtx& ctx, rdr::FrameBuilder& fb);
  // S103 PMS (live_map_props.cpp): the circle image handed to the floor as MapRole::PropsMap (matrix / prm from the circle entry and the props light state); false = not handed over (props_skip says why)
  bool PublishProps(const MapsInput& in);
  // S103 PM2 (live_map_props.cpp): will the props map be handed to the floor this frame (the switches, the props light known)? The prop casters are drawn into it only then.
  bool PropsMapWanted(const MapsInput& in) const;
  struct PropCaster { const CasterModel* model; const Object* obj; };
  // The tagged (kObjCastsProps) visible objects of the frame whose posed copy is resident, with their caster geometry (CasterOf prop); counts what was skipped. Empty = no depth attachment is needed.
  void CollectPropCasters(const SceneFrame& frame, const MapsInput& in, std::vector<PropCaster>& out);
  // Draws them into the circle image's pass (targets + clear are set by RecordCircles): the props light = the circle entry's Trapezoidal x ViewProj (c4-c7) with an identity trapezoid, ViewDepth = its row 3 (the
  // floor's ViewLight[2]), ShadowPrm c168 = the props light's near / far (MapsInput::props_prm); returns the meshes drawn.
  uint32_t DrawPropCasters(const SceneFrame& frame, const MapsInput& in, const std::vector<PropCaster>& casters, rdr::FrameBuilder& fb, uint32_t dyn_id);
  bool EnsureShadowPrograms();   // S103 PM2: the yShadowMap programs / layouts, made once (RecordShadow and DrawPropCasters share them)
  bool EnsureImages(uint32_t w, uint32_t h, bool circles);
  // S103 E2: one source file each (live_map_rim.cpp, live_map_plane.cpp, live_map_prelight.cpp, live_map_sweat.cpp)
  bool RecordRim(const StageCtx& ctx, rdr::FrameBuilder& fb);
  bool FitRimInput(const SceneFrame& frame, CasterRows& out_rows, const char*& why);   // S103 E3: the rim camera + depth range recomputed from the frame camera and the casters
  bool RecordPlane(const StageCtx& ctx, rdr::FrameBuilder& fb);
  // S103 B2F (live_map_rope.cpp): the ropes' shadow proxy (ring entry 956) drawn into the mask just set up by RecordPlane (same targets / dynamic / constants); returns the draws made
  uint32_t DrawRopeProxies(rdr::FrameBuilder& fb, const E2Input& in, const float* head, uint32_t dyn_id, uint32_t tex_id, uint32_t fs_const);
  bool RecordPreLight(const StageCtx& ctx, rdr::FrameBuilder& fb);
  bool RecordSweat(const StageCtx& ctx, rdr::FrameBuilder& fb);
  // S103 L7: the filter chains behind the raw shadow and rim maps (live_map_chain.cpp): the maps this frame made are replaced by their filtered images and say raw = false
  void ChainMaps(rdr::FrameBuilder& fb);
  std::unique_ptr<ChainState> chain_;
  std::unique_ptr<E2State> e2_;   // S103 E2
  E2Faults e2_faults_;            // S103 E2
  std::string game_root_;         // S103 B2F
  bool blocking_loads_ = false;   // S103 B2F

  rdr::Renderer& renderer_;
  ModelProvider provider_;
  const MapsInput* input_ = nullptr;
  bool repeatable_ = false;   // S103 E3
  bool plane_reader_ = std::getenv("WWE_RDR_NO_PLANE_READER") == nullptr;   // S103 L9: the ring mat reads MapRole::PlaneShadow now (front_end.cpp), so the mask is drawn; WWE_RDR_NO_PLANE_READER=1 = the old behaviour (the planted twin)
  MapsFaults faults_;
  LiveMaps maps_;
  MapsCounts counts_;
  uint64_t inputs_missing_frames_ = 0;
  uint64_t frames_ = 0, shadow_made_ = 0, circles_made_ = 0, circle_draws_ = 0, caster_draws_ = 0, props_made_ = 0, props_holes_ = 0, props_degenerate_ = 0;   // S103 PMC2: props_degenerate_ = frames whose props map was not made because the game's light is degenerate (not holes);   // S103 PMS: props_*

  std::unordered_map<uint32_t, std::unique_ptr<CasterModel>> casters_;   // by asset index
  std::unordered_map<uint32_t, std::unique_ptr<CasterModel>> prop_casters_;   // S103 MBC: by asset index, tagged props only
  PropProvider prop_provider_;
  GuestPredicate guest_pred_;   // S103 CS1B
  uint32_t guest_casters_last_ = 0;   // S103 CS1B F5: guest casters of the last frame whose shadow was drawn (the census prints it: a half-rate skipped frame leaves it)
  friend struct LiveMapsTestAccess;   // S103 CS1B: tools\scene\cs1b_unit.cpp reaches MeasureCasters / casters_
  // images
  rdr::ImageId shadow_color_ = rdr::kNoImage, shadow_depth_ = rdr::kNoImage;
  uint32_t shadow_w_ = 0, shadow_h_ = 0;
  rdr::ImageId circles_color_ = rdr::kNoImage;
  uint32_t circles_w_ = 0, circles_h_ = 0;
  rdr::ImageId props_depth_ = rdr::kNoImage;   // S103 PM2: the props pass's depth (transient, cleared each frame the casters draw), the circle image's size
  // programs / layouts (made on first use)
  int vs_skinned_ = -1, vs_single_ = -1, fs_shadow_ = -1, vs_circle_ = -1, fs_circle_ = -1;
  uint16_t prog_skinned_ = 0, prog_single_ = 0, prog_circle_ = 0, layout_skinned_ = 0, layout_single_ = 0, layout_circle_ = 0;
  bool shadow_ready_ = false, circle_ready_ = false;
  // S103 SPB (SP2 M3): the LOW tier's half-rate shadow maps (start_settings.h PlayerQuality::shadow_half). shadow_drawn_w_/h_ = the size of the last map a frame DREW (0 = none: the image holds nothing to
  // show again); shadow_half_n_ counts frames while the lever is on (odd ones keep the last drawn map).
  uint32_t shadow_drawn_w_ = 0, shadow_drawn_h_ = 0;
  uint64_t shadow_half_n_ = 0;
  // the circle disc mesh (uploaded once, again when the record's mesh differs)
  rdr::BufferId circle_vb_ = rdr::kNoBuffer, circle_ib_ = rdr::kNoBuffer;
  CircleMesh circle_mesh_ = {};
  bool circle_mesh_valid_ = false;
};

}  // namespace rdr_scene
