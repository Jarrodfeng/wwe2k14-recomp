// The scene front-end (Session 101, lane 6; docs\renderer\SCENE_FRONTEND.md): takes the scene record's tables (scene_record.h, in memory)
// and produces frames for the renderer (src\renderer\renderer.h). Assets are registered once with our loaders' host formats
// (assets::Model: floats), materials are bound BY NAME to the hand-written effects (src\renderer\shaders\fx\), their named constants
// are filled by name (fx_tables.h). No game memory, no D3D stream, no binder of game draws.
//
// Includes the renderer and the asset model only (no viewer, no SDK).
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/asset_model.h"
#include "../keep_stream.h"   // S103 PVR: WWE2K14_STREAM()
#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"
#include "scene_record.h"
#include "wrk_join.h"   // S103 WRK
#include "char_effects.h"  // S101 lane 13: the character path
#include "post_pass.h"     // S101 lane 34: the post pass
#include "stage_hook.h"    // S103 lane S: stage hooks, live targets, maps by role
#include "live_screen_rule.h"   // S103 SCX: LiveScreenWhy (the grab invalidation reasons)
#include "draw_lists.h"    // S103 lane G: the kept draw lists
#include "see_through_order.h"  // S103 lane F: the see-through order
#include "particle_tables.h"    // S103 lane F: the particle tables (record ids 50-51)

namespace rdr_scene {
struct SceneTables;   // S103 P7: record_io.h
struct PartClock;     // S106 G4-2: a pointer only (SceneFrame::part_clocks); rows_part_clock.h defines it

// S103 L10: how a stage-slot mesh gets the stage light-map page. A bool at an old call site fails to compile (enum class), never silently becomes EntranceLm.
enum class LmMode { Own, EntranceLm, MatchVertexPage, MatchNoPage };   // Own = the material's own rule; EntranceLm = the LM pixel pair (L5 / entrance); MatchVertexPage = a match yBG_Default mesh of the vertex-LM class (page at vertex slot 16 only); MatchNoPage = the other match yBG_Default meshes (no page)

// What the effect chooser may pick from, and what it picked.
struct MaterialWants {
  bool page_available = false;  // S103 L10: the stage page image exists (the fallback when no vertex-LM variant fits: the per-pixel page)
  bool diffuse = false, lightmap = false, normal = false, specmap = false;
  bool spec = false;       // the material asks for a highlight
  bool alpha_test = false;
  bool alpha_mask = false;   // S103 P1: the diffuse alpha is a highlight mask (Material::alpha_mask): the FX_ALPHA_SPEC / FX_ALPHA_GLOSS pair
  bool skinned = false;    // S101 lane 15: a skinned prop chunk (the FX_SKINNED vertex variant)
  bool vertex_lm = false;  // S103 L10: take the `*_vertex_lm_vertex` vertex variant (the page read in the vertex stage, slot 16) with the light-map pixel pair; needs `lightmap`
  bool unlit = false;      // S103 L4: take the audience variant WITHOUT FX_LIT (the far-card quad: its picture is baked lit already)
  int layers = 1;          // S103 LAY: the UVScroll diffuse layer count (1..4) the material draws; the variant must have exactly that many (FX_LAYER2..4)
  bool light_shadow = false;    // S103 RML: the ring mat reads the PROPS map (FX_LIGHT_SHADOW + FX_SHADOW_LIGHT, slot 12): the game's pass 2 / 3 when it built no plane module (sub_82608CB0); never together with screen_shadow
  bool screen_shadow = false;   // S103 L9: the ring mat reads the plane shadow map (FX_SCREEN_SHADOW, slot 12); only a yProp_PSM / yProp_PSMLM material asks for it
  int tech_shows = -1;     // S103 TQF (fallback scoring only): 1 / 0 = the game's table variant shows / does not show the diffuse alpha (ShowsDiffuseAlpha); the stand-in variant must agree (a strong score term); -1 = no preference
  bool cube = false;       // S103 RFL: the material names a reflection cube (sampCubeRefPrm) whose image the front end can bind at pixel slot 8 (a Reflect variant is then fed the file's cube, not the black stand-in)
  bool refmap = false;     // S103 RFL: ... and a reflection-amount map (sampRefMapPrm) for pixel slot 9 (the reflect_map / reflect_mask variants)
  int planar = 0;          // S103 TQ2 (UVS.md): 0 none, 1 = FX_PLANAR_UV (XZ), 2 = + FX_PLANAR_YZ: the yBG_UVScroll SearchLight techniques 8..11 (world-projected uv); the vertex variant must match exactly
  bool change_colour = false;   // S108 THB: an entrance PROP drawn recoloured (the record's section-67 row, kCcProp; its mask is loaded): the pixel variant is the EDIT/STS table's row (the game's model+392 row while model+604 is set), all cc_mask variants: FX_CC_MASK is allowed and slot 12 is the prop's mask (Material::change_mask[0]); needs `technique`
  int technique = -1;      // S103 TQF: the mesh record's technique word (Material::technique) of a yProp_BumpMap / Default / Reflect material: the HARD filter on the pixel variant (the game's table row, technique_pick.h); -1 = unknown / not that class: the sampler scoring only
};
struct FxChoice {
  int vs = -1, ps = -1;    // indices into rdr::fx::kVariants (-1 = none)
  std::string effect;      // the effect the pair belongs to ("yBG_DefaultLM")
  std::string why_not;     // set when vs < 0: the reason, for the coverage list
  bool layer_fallback = false;  // S103 LAY: no multi-layer variant fitted: the one-layer pick
  bool vlm_fallback = false;  // S103 L10: a vertex-LM pick found no variant and took the per-pixel page pair
  int tech_ps = -1;            // S103 TQF: the effect table's pixel variant for this material's technique (registry index), whether or not it could be bound (-1 = not a technique-picked material / no row): the alpha test + blend follows IT, not the stand-in
  bool tech_picked = false;    // S103 TQF: the pixel variant is the technique table's (the game's) pick
  bool tech_fallback = false;  // S103 TQF: the table's variant was not in the table / not bindable for this material: the sampler-scoring pick, counted
  bool cc_row = false;         // S108 THB: the pick is the STS table's cc_mask variant (MaterialWants::change_colour was asked and the table's row was bindable)
  bool Ok() const { return vs >= 0 && ps >= 0; }
};
// Picks the vertex + fragment variants of an effect for a material: the simplest pair whose interpolators match and whose textures and
// vertex inputs the front-end can supply. effect is the material class name as the file stores it ("yBG_Default", "yProp_BumpMap").
FxChoice ChooseEffect(const std::string& effect, const MaterialWants& wants);
// S105 C4: planted twin (the unit sets it): the pairing before the FX_SPECULAR guard, a vertex / fragment pair that disagrees on FX_SPECULAR is let through. Never set in the game.
bool& WideGuardPlant();   // S105 C4 fix: test-only twin (the FX_SPECULAR guard without `renamed > 0`)
bool& LoosePairPlant();
// S103 L5: a stage-slot mesh (part "slot N") of a MATCH the game reads the stage light-map page for (see arena_slot_flags.md "S103 L5"): not a translucent (`a` / `s`) slot, an EntranceLmClass
// effect, not a movie surface. The ring's slots (>= 951) are excluded: the arena-4 table has none above 397 and no stream shows the ring's choice [U]. One rule for the front-end draw and for
// the SubModel page hand-over (rf_c_core.cpp), so a sub-model with no own-LM mesh still gets the page.
bool ArenaMatchLm(const std::string& part, const assets::Material& m);
// S103 L10: a match arena mesh the game gives the page in the VERTEX stage only (yBG_Default, the vertex-LM class, Material::vertex_lm); the other yBG_Default meshes of a match get no page. See arena_lightmap.md "S103 L10".
bool ArenaMatchVertexPage(const std::string& part, const assets::Material& m);
bool ArenaMatchNoPage(const std::string& part, const assets::Material& m);

// What one effect did in a scene: the coverage list (every mesh is either bound to our effect or drawn in the debug colour).
struct EffectCoverage {
  std::string effect;       // the material class name
  std::string bound_to;     // "yBG_DefaultLM: ...": one chosen pair (the first), empty when none
  uint32_t meshes_bound = 0, meshes_unbound = 0;
  std::string reason;       // why unbound
};

struct FrontEndOptions {
  bool textures = true;     // false: every material samples a flat grey (the selftest's planted fault)
  bool plant_one_layer = false;  // S103 LAY planted twin (dev rig only): the old one-layer UVScroll draw
  bool plant_old_alpha_ref = false;  // S103 B2 planted twin (dev rig only): one alpha reference 0.5 for every pass class (the old behaviour)
  bool plant_cutout_noblend = false; // S103 B2 planted twin (dev rig only): an alpha-tested cut-out drawn unblended (the first B2 build)
  bool plant_untested_alpha = false; // S103 B2 planted twin (dev rig only): an arena part with a real texture alpha stays untested (the loader's alpha_test rule only)
  bool plant_no_mirror = false;      // S103 RFL planted twin (dev rig only): the viewer-space draw does not flip the cube lookup (+Y / -Y swapped, every reflection upside down)
  bool plant_cube_unbound = false;   // S103 RFL planted twin (dev rig only): the reflection cube is never bound (the black stand-in + ReflectAlpha 0): the reflection is missing, tools/scene/rfl_unit.cpp must catch it
  bool plant_mat_plain_when_no_module = false;   // S103 RML planted twin (dev rig only): the ring mat stays the plain variant when the game built no plane module (the old binding: no contact shadow on the mat)
  bool plant_vs_prm_ps_rows = false;             // S103 RML planted twin (dev rig only, pms_check.py): the vertex ShadowPrm block gets the pixel rows (the old Resolve)
  bool plant_light_stamp_no_mat = false;         // S103 RML planted twin (dev rig only, frame_rig --lists): the kept draw lists' light stamp leaves out the ring mat's props_reader (the bug of review F1: the mat's TexProjLight goes stale)
  bool plant_mat_reads_mapb = false;             // S103 RML planted twin (dev rig only): the ring mat's light pass reads the wrestlers' map B at slot 12 instead of the props map
  bool debug_unbound = true;  // draw a mesh with no effect in magenta (never silently)
  // S106 PSEED (TOOL ONLY, never set in the game: tools\renderer\pipeline_census.py through frame_rig --census-maps / --census-char-mode): the pipeline census draws a record with the map
  // arms the game has at run time but a stored record may lack. census_maps bits: 1 = a shadow map, 2 = a props map, 4 = a plane-shadow map is present (a white stand-in image, identity matrix,
  // not raw); each is separate (a plane map with no props map is a state the game has). census_char_mode >= 0 replaces the wrestlers' pass mode word (0 or 2) so both pass classes are drawn.
  uint32_t census_maps = 0;
  int census_char_mode = -1;
};

// S101 lane 15 (entrances): the light sets of one model as the entrance rig rewrote them for this frame (the viewer's GpuModel::lights: the model's sets,
// the default rig, and for a belt / carried prop the entrant's set appended LAST by the viewer) and which set each mesh uses (GpuMesh::light: the viewer re-points
// a belt's normal-mapped meshes at the entrant's set, the rest at set 0). Replaces the sets and the per-mesh choice the model was added with (a proposed
// scene-record field: SCENE_RECORD "v2 candidates").
struct ModelLights {
  uint32_t asset = 0;
  std::vector<assets::LightSet> sets;
  std::vector<int16_t> mesh_set;  // per source mesh (Mesh::src order): its index into `sets`; empty = the file's choice (the set the mesh was added with)
};

// S110 CUTGREY (docs\research\s110\CUTGREY_BUILD.md): one crowd camera flash, drawn AFTER the post chain by FrontEnd::DrawFlashesAfterPost (the game draws them after its combine, crisp: render category 52).
// world = the camera-facing sprite's world (row vectors, the viewer's space), centre = its translation (the point whose scene depth hides the sprite whole), rgb = the colour of this flash (the record's colour
// block; white when the block was unreadable), alpha = c185.x, model = the front-end model of the arena's flash sprite (key 31000).
struct FlashSprite {
  int model = -1;
  float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float centre[3] = {0, 0, 0};
  float rgb[3] = {1, 1, 1};
  float alpha = 0.5f;
  // S110 FLASHOCC (census only, never drawn from): the game row this sprite is, for the near-flash log line (front_end_flashes.h)
  uint64_t guest_frame = 0;
  uint32_t seq = 0;
  uint16_t slot = 0xFFFF;
  bool held = false;   // shown from the flash hold (kFxfHeld)
  float scale = 0.f;   // the row's scale (item +40)
};
// S110 FLASHOCC: a flash whose centre is nearer than this (view z, game units) cannot be a crowd flash seen from inside the ring: the spawners place them 1900+ (inner ring) / |x| + |z| >= 1650 from
// the ring centre (FLASHDEPTH.md a5), so from a camera inside the ring (radius < ~400) the nearest crowd flash is ~1200-1500 units away. Counted and logged (FrontEnd::FlashLine), never acted on.
inline constexpr float kFlashNearViewZ = 1500.f;
// TEST ONLY (tools\scene\cutgrey_unit.cpp): the planted twins of the flash draw. pre_post = the flashes are drawn into the scene colour BEFORE the post chain (the old order: the far blur, haze and sepia run
// over them); file_colour = the sprite's own colour is ignored (white); v_flip = the centre's depth texel is read with the picture's rows upside down (the centre test reads the wrong place). The game never sets them.
inline std::atomic<bool> g_plant_flash_pre_post{false}, g_plant_flash_file_colour{false}, g_plant_flash_v_flip{false};
// TEST ONLY (S110 FLASHOCC, cutgrey_unit [OCC]): the centre's depth is fetched from the picture's row 0 (the sky's far plane), not the centre's own row: a body in front of the centre no longer hides the sprite.
inline std::atomic<bool> g_plant_flash_fetch_row0{false};
// TEST ONLY (S110 FLASHFRONT, cutgrey_unit [PIX]): the after-post flash pass keeps its depth test off (CUTGREY's state): a sprite whose centre is visible paints over a nearer body.
inline std::atomic<bool> g_plant_flash_no_ztest{false};

// A frame request: what to draw, from where.
struct SceneFrame {
  Camera camera;                         // view = world -> clip when proj is the identity (the viewer gives its combined matrix; see the page)
  std::vector<Object> objects;
  // S103 EYF: the model alpha (MdlAlpha = [model+252]) of the own-skeleton pieces (hair, caps, glasses, the eyelid / lash layer 20002) that an OWNER body object draws: per owner index in `objects`, per piece id
  // (mesh src_kind 2, src_key == piece). The front end draws such a mesh with this alpha (not at all when 0). A lid-layer mesh (20000-39999, `trans`, a `mabuta` palette) with no row here is not drawn.
  struct PieceAlpha { uint32_t object = 0, piece = 0; float alpha = 0.f; };
  std::vector<PieceAlpha> piece_alphas;
  // S103 W2B (W2_ADV.md R2): (object index in `objects`, piece id) pairs whose spring-hair extra nodes the record could not supply: the object's meshes that skin to those nodes are skipped (counted), the rest draw.
  std::vector<std::pair<uint32_t, uint32_t>> absent_extras;
  // S103 SWF: the record's engine object (the game's skeleton address, kScEngineObjects) of the wrestler bodies in `objects`: (index in `objects`, engine object). `objects` is built by the classes' appliers and is NOT parallel
  // to the record's table, so the join of a body to its liquid map (MapRole::SweatMap, keyed by this address) goes through this list, which the wrestler applier fills where it pushes the body.
  struct ObjectEngine { uint32_t object = 0, engine = 0; int32_t char_mode = -1, char_mode620 = -1, e20_any = -1; };   // S103 WRK: char_mode620 = [model+620], e20_any = some part's E+20 word is set (census only), -1 = not read. S103 YCB: char_mode = the game's model+616 (char +5056; 0..3, technique_pick.h CharPass) of this body when the record carries it, -1 = not read (the per-material mode 0 is used, counted)
  std::vector<ObjectEngine> object_engine;
  std::vector<wrk::Levels> wrinkle_levels;   // S103 WRK: the live face-wrinkle / id-map levels per (body index in `objects`, part), wrk_join.h
  // S108 THR2 (change_colour_draw.h): the Superstar Threads editor's live colours of a recoloured wrestler body: (index in `objects`, the 16 ChangeColor constants (R, G, B, 1) / 255, the row's flags).
  // Filled by rf_wrestlers from the record's section 67; a body with no entry draws its plain row.
  struct ChangeColour { uint32_t object = 0, flags = 0; float constants[64] = {}; };
  std::vector<ChangeColour> change_colours;
  std::vector<ParamValue> params;       // Object::params_first / count index this (names are strings: resolved by the caller into ParamNames)
  std::vector<std::string> param_names;  // parallel to params
  // S101 lane 13 (wrestlers): the skin palettes (Object::bones_first / bones_count index this; one matrix per skeleton node, viewer space, row vectors,
  // the viewer's `Model::skin`) and the evaluated light sets of characters (Object::light_set indexes this; the in-memory form of kScLights).
  std::vector<Mat4> bones;
  std::vector<assets::LightSet> light_sets;
  // S101 lane 15 (entrances): this frame's light sets per model (the entrance rig) and the entrance rule that the game draws the ring / prop classes with
  // their light-map variant (the stage page carries the rig's pools).
  std::vector<ModelLights> model_lights;
  bool entrance = false;
  bool entrance_bg = false;  // the viewer's ent_lm_bg switch: the yBG_Default / yBG_UVScroll classes take the entrance LM variant too (off by default)
  bool viewer_space = true;  // S101 lane 13: the matrices are the viewer's (Y up, the loader's mirror); false = already the game's file space (a live record)
  float card_factor = 0.f;           // S103 B3: the game's card factor [0x8376383C] * 0.001 of the frame (the record's kSlCardFactor row); applied to card-technique models only when > 0
  bool card_factor_valid = false;    // false = the record has no row (a capture made before the row, the viewer): the card gain is unscaled, counted (card_factor_missing)
  int64_t phase_word = -1;   // S103 D1: the scene phase word 0x836F3940 of the frame (the record's kSlPhase row); -1 = not in the record: the read rule takes (entrance ? 4 : 1) [L]
  int32_t plane_game_off = -1;   // S103 RML (F4): 1 = the game's sub_827ECB60 was 0 at this frame's latch (no plane module, or [0x836F39A0] != 0: the ring mat's light pass), 0 = it was not, -1 = unknown (PlaneGameOffOf, scene_record.h)
  int32_t participants = -1, ring_gate = -1;   // S103 MBC: the phase row's participant count and ring gate word [0x836F39A0] (PhaseRowParticipants / PhaseRowRingGate); -1 = unknown: map B casts wrestlers only
  double time_s = 0;
  // S103 E5: the UV-scroll clocks (VS c186 `Time`) per stage slot / part (the record's kScPartClocks; null = none): a mesh of an UV-scroll material whose slot has a row is fed THAT clock instead of time_s.
  const PartClock* part_clocks = nullptr;
  size_t part_clock_count = 0;
  std::vector<FlashSprite> flashes;   // S110 CUTGREY: the crowd camera flashes of this frame (live_flashes.cpp): drawn after the post chain, never scene objects
  uint32_t width = 0, height = 0;
  float clear[4] = {0.12f, 0.14f, 0.18f, 1.0f};
  // S101 lane 34: when set, the scene's colour + depth go through the post pass (post_pass.h) and the picture read back is its output (the display curve is
  // NOT applied: the caller does, as the game does at the present). The scene's size must equal post->width / height.
  const PostParams* post = nullptr;
  // S103 lane S (FRAME_CONTRACT.md 8): the classes' own draws at each stage boundary (stage_hook.h), and lane E's maps by role.
  std::vector<StageHook*> stage_hooks;
  const LiveMaps* maps = nullptr;
  // S103 F: the order of the see-through list (see_through_order.h; docs\renderer\SEE_THROUGH_ORDER.md). Distance = today; the live scene frame sets Game.
  SeeOrder see_order = SeeOrder::Distance;
  // S103 F: the frame's particles (the record's kScParticles tables; null = none). Drawn by LiveParticles at Stage::AfterBlended (live_particles.h).
  const ParticleTables* particles = nullptr;
  // S103 P7 tween (review F1): the record's tables, so an own draw's blend key hashes an object's NAME (Object::id is a byte offset into the record's string blob: not the same between ticks).
  // Set only by BuildFrame (the live draw / the host); null = own draws are not blend-tagged.
  const SceneTables* names = nullptr;
  // S103 CAWB (CAWV Q1, census only): the body probe of a guest-model actor (the CAW editor's body), filled by rf_wrestlers once per 600 frames; FrontEnd prints it with the count of additive items nearer the eye.
  struct BodyProbe {
    bool valid = false;
    uint64_t guest_frame = 0;
    float pos[3] = {0, 0, 0};      // the root palette row's translation (the frame's space)
    float view_z = 0, ndc[2] = {0, 0}, dist = 0;
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};   // the model's bounds
  } body_probe;
};

// S103 lane D (live_crowd): ONE instanced draw of a model's meshes: the members of a body class (or the cards of a type) as instances. The vertices are the
// class's own, posed by the caller for this frame (`posed[k]` = the vertices of the model's k-th kept mesh, the model's own order), pushed as the frame's geometry;
// the instances are the three world rows of each (the game's c76-c78 form: row r = c[4 r .. 4 r + 3], translation in the 4th column). The material is the
// model's (bound by name as for any object), the vertex stage its yAudienceInst twin (FX_INST), the pixel stage the material's own.
struct InstVertex { float pos[3], normal[3], uv[2]; };   // 32 bytes (the instanced layout)
struct InstancedDraw {
  int model = -1;                                        // a front-end model
  const std::vector<std::vector<InstVertex>>* posed = nullptr;  // required: per kept mesh
  const float* worlds = nullptr;                         // 12 floats an instance
  uint32_t count = 0;
  uint8_t group = rdr::kGrpOther;                        // S103 GSTP: the GPU group stamps' group of these draws (rdr::GroupId)
  rdr::ImageId slot0 = rdr::kNoImage;                    // replaces the material's diffuse picture (a card's baked picture)
  const float* view_proj = nullptr;                      // 16 floats, row vectors; null = the stage's
  const float* eye = nullptr;                            // 3 floats; null = the stage's
  bool game_space = true;                                // the model, worlds and camera are in the game's file space (the lights are then fed as the file stores them)
  bool cull_none = false;                                // two-sided (a card)
  bool clamp_slot0 = false;                              // clamp-to-edge sampler for slot 0 (a card's picture: the feet are on its bottom edge)
  bool no_blend = false;                                 // S103 P7 tween (review F2): never tag this draw for the tick blend (a card BAKE shares its model + mesh keys with the LOD-0 members, so it must not pair with them)
  const ModelLights* lights = nullptr;                   // S103 L4: this frame's light sets of the model (the record's evaluated values by id); null = the sets the model was added with (the file's baseline)
  // S103 CB1 (the card atlas, card_atlas_rule.h): a draw into one tile of a shared target. viewport_rect replaces the stage's viewport (x, y, w, h of the render area), scissor_rect (default the viewport) the scissor.
  const rdr::Rect* viewport_rect = nullptr;
  const rdr::Rect* scissor_rect = nullptr;
  uint8_t aniso_override = 0;                            // S103 CB1: slot 0's max anisotropy when non-zero (the atlas's far-card draw: 1x, whatever the setting says now)
  uint32_t tag_salt = 0;                                 // S103 CB1: mixed into the tick-blend key (the card types share one atlas image and one quad model: their draws must stay apart)
  // S103 CB1: input_hash != null = every input of this draw is mixed into *input_hash (state, dynamic state, texture images + samplers, both constant blocks, the posed vertices, the instance rows). dry = ONLY that:
  // nothing is pushed or recorded (the card bake asks "would this draw give the same picture as last frame?" before it opens a pass). The textures' CONTENT is not in it: the caller adds its own page counters.
  uint64_t* input_hash = nullptr;
  bool dry = false;
};
struct InstancedStats { uint32_t draws = 0, instances = 0, meshes_refused = 0, light_mismatch = 0; std::string first_refusal; };

// S103 UP: who asked for a live texture upload (the byte counter's split: page = the stage light-map page, screen = tron / screen pictures, other).
enum class TexTag : uint8_t { Other = 0, Page = 1, Screen = 2, Count };
// S103 UP planted twins (RIG / UNIT ONLY: frame_rig.cpp and the unit set them; the game never does): per_model = AdoptPage refuses, so every model keeps its private page copy and every key change is
// re-sent to each (the old fan-out); no_rejoin = JoinCurrentPage keeps a model in the group it joined first (the old fault: a rebuilt set splits the page into two groups).
inline bool g_plant_page_per_model = false;
inline bool g_plant_page_no_rejoin = false;
// RIG ONLY fallback of the real switch wwe2k14_rdr_page_per_caller (rdr_switches.h, PerFrame, +dev): the rig has no cvars; frame_rig.cpp sets this from its env. The game reads the switch.
inline bool g_rig_page_per_caller = false;
// SCU (docs\research\harness\SCU_contract.md; LOOK: the picture follows the game): a stage mesh whose record word +0x8C has bit 0x2 is drawn with back faces culled, its `r` copy with the
// front face swapped (FrontEnd::Record). The game reads the switch wwe2k14_rdr_stage_cull (rdr_switches.h, PerFrame, default on); this is its fallback before the switch table's first refresh
// and in the offline tools (frame_rig sets it from WWE_RDR_STAGE_CULL=0). false = the stage meshes two-sided (the draw before SCU); screens / crowd keep their own cull.
inline std::atomic<bool> g_stage_cull{true};
// SCU planted twin (TEST ONLY; frame_rig from WWE_RDR_PLANT_STAGE_CULL_NO_FLIP, tools\scene\stage_cull_proof.py): the cull is on but the `r` copy keeps the clockwise front (no winding swap): the mirrored half shows its inside.
inline std::atomic<bool> g_plant_stage_cull_no_flip{false};
// S105 C3 (docs\research\s105\C3.md; LOOK): every model family follows the record cull bit like the stage (the switch wwe2k14_rdr_family_cull, rdr_switches.h, PerFrame, default on); this is its fallback before the table's
// first refresh and in the offline tools (frame_rig: WWE_RDR_FAMILY_CULL=0 is the "off" arm = the draw before). The planted twin (TEST ONLY, WWE_RDR_PLANT_FAMILY_CULL_FLIP): the non-stage families take the swapped front face (a wrong winding).
inline std::atomic<bool> g_family_cull{true};
inline std::atomic<bool> g_plant_family_cull_flip{false};
// S105 SZW (docs\research\harness\SZW_contract.md; LOOK: the picture follows the game): the see-through stage slots (`s` / `a`) write depth unless `z`, every stage draw is alpha-tested GEQUAL 1/255 (a draw whose pixel
// variant has no alpha twin writes NO depth, counted), a `z`-only slot draws blended with no depth write, the `s` / `a` groups draw in the game's slot order. The game reads the switch wwe2k14_rdr_stage_zwrite
// (rdr_switches.h, PerFrame, default on); this is its fallback before the switch table's first refresh and in the offline tools (frame_rig: WWE_RDR_STAGE_ZWRITE=0 is the "off" arm = the draw before).
inline std::atomic<bool> g_stage_zwrite{true};
// S105 SZW planted twin (TEST ONLY; frame_rig from WWE_RDR_PLANT_SZW_NO_TEST, tools\scene\stage_zwrite_proof.py): the depth write lands but the alpha test is NOT forced: a see-through piece cuts its rectangle out of what draws after it.
inline std::atomic<bool> g_plant_szw_no_test{false};
// S108 HAIR planted twins (TEST ONLY; never set by the game, no switch: the game's per-piece rule IS the draw, piece_draw_rule.h). _old = the draw before S108 (a stock piece's translucent mesh blended, untested, no depth write);
// _no_test = the depth write lands with NO discard (the cards cut what draws behind them: the census counts `depth write without a discard`).
inline std::atomic<bool> g_plant_piece_cut_old{false};
inline std::atomic<bool> g_plant_piece_cut_no_test{false};
// S108 HAIR review (TEST ONLY): every piece-cut pair reads as having NO alpha twin (the compiled registry gives every yCh_* pixel variant one, so only this drives the no-twin rule: depth write off + the piece_cut_notwin draw_shortfall).
inline std::atomic<bool> g_plant_piece_cut_no_twin{false};
// S103 RFL (docs\research\s103\RFF.md section A; LOOK: the picture follows the game): the Reflect effects (yBG_Reflect, yProp_Reflect) sample the model's own file cube (sampCubeRefPrm: cube_00 / cube_01 / cube_post / cube_ref,
// DXT1, six faces) at pixel slot 8, scaled by the material's g_fReflectAlpha (PS c208.x); the reflect_add / reflect_mask / reflect_map variants take the 2D amount map at slot 9. ON by default in both builds (the
// start-time switch wwe2k14_rdr_reflections in the stream-free build; the rigs' WWE_RDR_REFLECTIONS=0 is the "before" arm). false = the black cube + ReflectAlpha 0 stand-in of S101 lane 15, exactly.
inline std::atomic<bool> g_reflections{true};   // BOTH-BUILDS: a LOOK, ON in both builds by design (the stream build keeps this default; the stream-free start reads the cvar)
// S103 TQ2 (docs\research\s103\TQ2.md; LOOK: the picture follows the game): the pixel variant of yBG_Default / yBG_BumpMap (+LM) and yDefault materials is the effect table's row for the record's technique word
// (technique_pick.h AllFamily), like TQF's yProp class and RFL's yBG_Reflect. ON by default (the start-time switch wwe2k14_rdr_table_pick_all; the rigs' WWE_RDR_TABLE_PICK_ALL=0 is the "before" arm). false = those families
// keep the sampler scoring + the L5 / P1 fits, and the SearchLight (yBG_UVScroll T8..T11) planar pair is refused too (UVS.md; front_end.cpp UvLayers / SearchLightPlanar / Resolve SL_Scale), exactly as before the lane.
inline std::atomic<bool> g_table_pick_all{true};   // BOTH-BUILDS: a LOOK (header comment above), ON in both builds; the cvar exists only in the OFF build, the stream build keeps the default true (start_settings.cpp:270)
// S103 YCB (docs\research\s103\YCH.md; LOOK: the picture follows the game): the vertex + pixel variants of a wrestler (yCh_*) material are the effect table's row for (technique word, the game's pass rule CharPass), not our
// min-define pick. ON by default (the start-time switch wwe2k14_rdr_char_table_pick; the rigs' WWE_RDR_CHAR_TABLE_PICK=0 is the "before" arm). false = the old pick (shadow / wet bits), exactly as before the lane.
inline std::atomic<bool> g_char_table_pick{true};   // BOTH-BUILDS: a LOOK (header comment above), ON in both builds; the cvar exists only in the OFF build (start_settings.cpp:93), the stream build keeps the default true
inline bool g_wrk_plant_grey = false;   // S103 WRK: the rig's twin (the no-map fallback is the flat grey: alpha 255 tilts the normal by the full level)
// S103 UPL U2: screen pictures are uploaded once per distinct picture and the screens that show the same pixels share ONE image (FrontEnd::UpdateScreens). ON by default (invisible: the same picture). false = the old
// one image per screen, one upload per changed screen (the start-time switch wwe2k14_rdr_screen_share in the stream-free build; the rig's WWE_RDR_SCREEN_SHARE=0 is the "before" arm).
// S103 ENV fix: the CAWB dev plant lives here (front_end.cpp is built into the viewers / tools without rf_c): >= 0 hides every stage mesh copy whose
// slot_no equals it (3 = CAS_moya); -1 = off. Set by start_settings (+dev builds only, --wwe2k14_rdr_hide_stage_slot) and the rig (WWE_RDR_HIDE_STAGE_SLOT).
inline std::atomic<int> g_hide_stage_slot{-1};
inline std::atomic<bool> g_screen_share{true};   // BOTH-BUILDS: invisible and ON in the stream build too (its front end always shares); the stream-free cvar only flips the A/B arm
// S103 RGP (docs/research/s103/RGS.md option 1; 360 HABIT row 12 in docs/renderer/360_HABITS.md): the posed ring pieces are drawn GPU-skinned (the game's own yProp skinned variants through the skinned-prop chunk path:
// one static model per ring key, each instance an object with its node rows in the frame's bones) instead of CPU-posed per instance (PoseMesh + UpdateModelVertices + a posed copy / upload per piece). ON by default in the stream-free build (OFF in the stream build, which has no switch); the
// start-time switch wwe2k14_rdr_ring_gpu_skin (start_settings.cpp) stores it; false = the CPU path (the A/B arm). The rigs store it themselves (WWE_RDR_RING_GPU_SKIN=0).
inline std::atomic<bool> g_ring_gpu_skin{!WWE2K14_STREAM()};
// S105 A1 review F3: RIG-ONLY planted twin (host_rig --a1-plant unscaled): the stream-free host leaves the scene targets unscaled at a render scale != 1 (the bug: the resolution choice did nothing). Never set by the game.
inline std::atomic<int> g_a1_plant_host_unscaled{0};
// S105 A1 review F2: RIG-ONLY planted failure (post_rig --a1-plant alloc): the scaled scene depth "could not be made" (the 4K memory failure): the targets must fall back to unscaled ones, logged once. Never set by the game.
inline std::atomic<int> g_a1_plant_scaled_alloc_fail{0};
// S103 PVR (docs/research/s103/UPF.md fix 1; 360 HABIT row 11 in docs/renderer/360_HABITS.md): the per-frame POSED vertices (re-posed ring pieces and posed weapons: ModelCache::PosedModel; CPU-skinned / cloth
// pieces: Record's cloth block; never the crowd, which already uses the ring) go into the frame's geometry ring (FrameBuilder::PushGeometry, drawn as {kFrameGeometry, offset}) instead of a staging buffer +
// vkCmdCopyBuffer + barrier pair per mesh. A model that stays at rest for more than the frames in flight is written into its own buffer once and drawn from it. STREAM-FREE BUILD ONLY: the default is
// off in the stream build (the start-time switch wwe2k14_rdr_posed_ring is defined and stored only by the stream-free build); the offline rigs store it themselves (the rig's WWE_RDR_POSED_RING=0 is the "before" arm).
inline std::atomic<bool> g_posed_ring{!WWE2K14_STREAM()};
// S103 PVR: the most posed bytes one frame's ring may take (FrontEnd::PosedPush also checks the renderer's real room, minus what the other producers pushed in recent frames: a frame over the ring is DROPPED): beyond it a mesh falls back to the old buffer path (counted).
constexpr uint32_t kPosedRingCap = 12u << 20;
// S103 PVR planted twins (RIG ONLY: frame_rig.cpp sets it from WWE_RDR_PLANT_POSED_RING; the game never does): 1 = every ring offset is one vertex off; 3 = a cloth piece reuses the previous Record's offset and pushes nothing; 4 = the renderer's-room check is left out (a heavy frame is dropped); 2 = a model reuses the previous frame's ring
// offset and pushes nothing (the consumer reads a slot of another frame). Both must make the picture differ (tools/scene/pvr_check.py).
inline std::atomic<int> g_plant_posed_ring{0};
// S103 PVR counters (process totals; the rig and the census read them): meshes / models pushed into the ring, bytes, fallbacks to the buffer path for lack of room.
inline std::atomic<uint64_t> g_posed_ring_pushes{0}, g_posed_ring_bytes{0}, g_posed_ring_overflow{0};
// S103 PF2 (docs/research/s103/RCP.md fix F2): on a device with unified memory (an integrated GPU, a phone: Renderer::UnifiedMemory) a re-posed model keeps kPosedCopies copies of its vertex buffer in host-visible, mapped,
// device-local memory; UpdateModelVertices writes the skinned vertices straight into the next free copy (ONE copy of the bytes: no posed_cpu, no ring push, no staging, no GPU copy) and the draws bind that copy.
// A copy a frame in flight may still read is never written (FramesInFlight / RenderSerial); when none is free the old staged copy into the model's own buffer is used (counted). Discrete cards keep the
// frame-ring path (g_posed_ring). The switch wwe2k14_rdr_posed_direct (default on) needs g_posed_ring on; the rigs store it themselves.
inline std::atomic<bool> g_posed_direct{true};   // BOTH-BUILDS: inert in the stream build (it needs g_posed_ring, which is off there)
// TEST / RIG ONLY: the direct path on a discrete card too (the copies live in host-visible memory the GPU reads over PCIe): pvr_unit and frame_rig set it; the game never does.
inline std::atomic<bool> g_posed_direct_force{false};
// S103 PF2 planted twins (RIG ONLY, tools/scene/pvr_unit.cpp --plant 5..7): 5 = ONE copy per model (every re-pose finds it in flight: must be detected, counted as fallbacks); 6 = one copy AND the in-flight check left
// out (the audit counter g_posed_direct_inflight must catch the write); 7 = the draws bind the copy after the one just written (an older pose: the picture must differ).
inline std::atomic<int> g_plant_posed_direct{0};
constexpr uint32_t kPosedCopies = 4;   // copies per model: a copy written at tick t is rewritten at t + 4, when (one Render a tick, 2 frames in flight) every frame that could read it has finished
// S103 PF2 counters (process totals; the [scene][posed] line and the rig read them): meshes written straight into a mapped copy and their bytes, re-poses that found every copy in flight (the staged fallback),
// copies made, and the AUDIT: writes that went into a copy a frame in flight may still read (always 0 unless the plant is on).
inline std::atomic<uint64_t> g_posed_direct_writes{0}, g_posed_direct_bytes{0}, g_posed_direct_fallbacks{0}, g_posed_direct_made{0}, g_posed_direct_inflight{0}, g_posed_direct_released{0};
constexpr uint64_t kPosedRestRenders = 600;   // a model with no write or draw for this many Renders frees its copies (SweepPosedCopies)
// 8 = (RIG ONLY, with the unit's own 3-renders-per-record loop) nothing is changed in the front end: the unit renders each recorded frame THREE times, more than kRendersPerRecord: Render must count an overrun.
// S103 UPL planted twin (RIG ONLY: frame_rig.cpp sets it from WWE_RDR_PLANT_SCREEN_REUPLOAD; the game never does): every screen is re-sent every frame to its own private image (no serial skip, no share, no
// same-bytes skip): the old per-screen fan-out at its worst. The upload bytes bar of up_check.py must fail on it.
inline bool g_plant_screen_reupload = false;
// One picture of a UpdateScreens batch: the live texture (of the model) that takes it and the RGBA8 pixels (w x h, exactly the texture's own size).
// `id` = the feed's content id of the pixels (rf_c.h ScreenFeed: equal id + size = equal bytes; never 0 here); `rgba` = the bytes, valid for the call.
struct ScreenPic { size_t texture = 0; uint32_t w = 0, h = 0; const uint8_t* rgba = nullptr; uint64_t id = 0; };

class FrontEnd {
 public:
  rdr::Renderer& renderer() { return renderer_; }   // S103 D2: the renderer this front-end draws with (a class applier's hook makes its own images)
  explicit FrontEnd(rdr::Renderer& renderer, const FrontEndOptions& options = FrontEndOptions());
  ~FrontEnd();

  // Registers a model (an arena or wrestler) as an asset: uploads its textures and geometry once, remembers its materials by name.
  // Returns the asset index objects refer to. Warnings (a texture that could not be uploaded) are appended.
  // S103 lane D: `shared` (optional) = pictures by name + size kept by the caller across models (a crowd class shares the stage light map and its type's picture with the other
  // classes): a texture found there is not made again, and the model does NOT own its images (the caller destroys them after releasing every model that uses them).
  int AddModel(const assets::Model& model, std::vector<std::string>& warnings, std::unordered_map<std::string, rdr::ImageId>* shared = nullptr);
  // S103 lane D: a model a stage hook draws itself (a crowd class): the generic object loop never draws it, whatever frame.objects says.
  void SetHookOwned(int model, bool owned = true);
  void SetModelGroup(int model, uint8_t group);   // S103 GSTP: the GPU group stamps' group of this model's draws (rdr::GroupId); the owner names it, the label rule (model_group.h) is the fallback
  void SetUnlit(int model, bool unlit = true);   // S103 L4: this model takes the unlit audience variant (MaterialWants::unlit); call before its first draw
  // S103 B: the skeleton node count of a registered model (0 = unknown / not a character): a palette of another size is not this model's (rf_wrestlers.cpp).
  uint32_t ModelNodeCount(int model) const;
  // S103 W2B: the body entry's own node count, and the (piece id, node) tags of the bones pieces added after it (the spring-hair chains); the record's palette must carry exactly these after the body's rows (rf_wrestlers.cpp).
  uint32_t ModelMainNodeCount(int model) const;
  const std::vector<std::pair<uint32_t, uint32_t>>& ModelExtraTags(int model) const;
  // S103 W2B (review D / F): per extra bone (parallel to ModelExtraTags): its parent node in the model, its bind position in FILE space (the game's), and corr^-1 when the piece's bind differs from the body's chain.
  struct ModelExtraInfo { int32_t parent = -1; float pos[3] = {0, 0, 0}; bool has_corr = false; float corr_inv[16] = {}; };
  const ModelExtraInfo* ModelExtraInfoOf(int model, size_t e) const;
  const std::vector<uint32_t>& ModelExtraPieceNodes(int model) const;   // review G: parallel to ModelExtraTags: the node count of the piece entry each extra came from   // null = out of range
  bool ModelBounds(int model, float bmin[3], float bmax[3]) const;   // S103 CAWB: the model's bounds (census only)
  // S103 PZF2 (EB): does this registered model hold a drawable mesh of the wrestler piece entry `piece_id` (src_kind 2, src_key == id)? rf_wrestlers' draw-time check of a piece the owner is said to draw.
  bool ModelHasPieceMesh(int model, uint32_t piece_id) const;
  bool ModelHasAnyPieceMesh(int model) const;   // S103 SMF (review A4): the model holds a mesh of some wrestler piece (src_kind 2): a body that could own an unlinked own-skeleton piece (a referee / manager model holds none)
  void ReleaseModel(int model);  // the GPU must be idle (the caller waits)
  static std::string ImageKey(const assets::Texture& t);   // S103 CTX2: AddModel's `shared` map key of a texture (name | size | format | bytes)
  void ReleaseImage(rdr::ImageId id);                      // S103 CTX2: destroy one image the caller owns (an AddModel `shared` entry); the renderer defers it
  // S103 R (record_frame.h's cache): a model that shares the images of `image_owner` (release the sharers before the owner); an in-place vertex rewrite of a static model
  // (the posed crowd bodies); a CPU-side state replace from a skeleton Model (the arena's light sets and placements). See front_end.cpp.
  int AddModelSharing(const assets::Model& model, int image_owner, std::vector<std::string>& warnings);
  bool UpdateModelVertices(int model, const assets::Model& posed);
  bool UpdateModelState(int model, const assets::Model& state);
  // S103 V1: the stage visibility's rejected copies, by stage slot (bit 1 = the base copy is not drawn, bit 2 = the mirrored / turned copy is not drawn; the vector may be shorter than the slots). A per-frame toggle with
  // no state re-apply: the draw loop skips the instance (the instance numbering of the kept draws is unchanged). Empty = nothing culled. Returns true when the set changed.
  bool SetStageCull(int model, const std::vector<uint8_t>& by_slot);
  // S103 SCF: the movie-key slots (800 + bit) of `model` that are shown with no movie this frame: they draw the live frame grab (the one pre-post scene copy), not their own picture. true = changed.
  bool SetIdleScreens(int model, uint16_t mask);
  // S101 lane 15: a live texture of a model (an RGBA8 texture the viewer rewrites: tron video frames, the stage light-map page) gets a new picture.
  bool UpdateTexture(int model, size_t texture, uint32_t w, uint32_t h, const uint8_t* rgba, TexTag tag = TexTag::Other);
  uint64_t texture_updates() const { return texture_updates_; }
  // S103 UPL U2: a frame's screen pictures as ONE batch (TexTag::Screen). Pictures with equal bytes go to one image that every screen showing them is bound to (a screen already on an image that holds the
  // bytes costs nothing); an image still shown by a screen that is NOT in this batch is never overwritten (that screen keeps its picture: a free image or a new one takes the write). ok[i] = picture i was
  // taken (the validity rules of UpdateTexture). UpdateTexture with TexTag::Screen is a batch of one. With g_screen_share off: one UpdateTexture per picture, as before.
  bool UpdateScreens(int model, const ScreenPic* pics, size_t n, bool* ok);
  uint64_t screen_pics() const { return scr_pics_; }          // pictures asked for (every one that passed the validity rules)
  uint64_t screen_shared() const { return scr_shared_; }      // pictures served with NO upload: an image already held their bytes (another screen's picture, or this screen's own)
  uint64_t screen_moved() const { return scr_moved_; }        // times a screen was re-bound to another image (a bind reset: rare in steady frames)
  uint64_t screen_batches() const { return scr_batches_; }    // batches that took a picture
  uint64_t screen_rebind_batches() const { return scr_rebind_batches_; }   // of them, batches that re-bound a screen (a steady playing movie should have almost none)
  uint64_t screen_images() const { return scr_images_; }      // screen images alive (the models' pools: the textures' own + the extras the share made)
  // S103 UP: bytes / copies sent by UpdateTexture / UpdatePage, by caller (the split of the image upload bytes), and the page sends the shared image / its same-bytes guard made unnecessary.
  uint64_t texture_update_bytes(TexTag t) const { return tex_bytes_[size_t(t)]; }
  uint64_t texture_update_count(TexTag t) const { return tex_count_[size_t(t)]; }
  uint64_t page_skips_key() const { return page_skip_key_; }
  uint64_t page_skips_hash() const { return page_skip_hash_; }   // "same bytes": a new key whose pixels equal the last sent (the guard: memcmp against the kept copy)
  // S103 UP: ONE stage light-map page image per page group (an arena set; the game has one page, image 388, bound by every draw). AdoptPage points the model's page texture `texture` at the group's
  // image (the first adopter's own image becomes it; later adopters' private copies are destroyed, or left to their owner when the model's images are external). A model adopts BEFORE its first draw
  // (its bound caches are dropped anyway). Returns false when it cannot (planted twin, borrowed images, not a 1-mip RGBA8 live texture, an external image with no group yet).
  bool AdoptPage(int model, size_t texture, uint64_t group);
  // The group whose page a crowd model joins when it first uploads (set by the arena's page update each frame; 0 = none). The arena applier sets it to 0 when it retires a set and to the live set's
  // group right after EnsureArena succeeds (a retired set's group must never be the one a new crowd model joins).
  void SetCurrentPageGroup(uint64_t group) { page_group_current_ = group; }
  uint64_t current_page_group() const { return page_group_current_; }
  // The models AddModel makes next take the group's page image instead of making (and uploading) a private 1 MiB copy of their page texture (rf_c_core.cpp Register sets it around Put; 0 = off).
  void SetBuildPageGroup(uint64_t group) { page_group_build_ = group; }
  // A crowd model (images owned elsewhere) joins the current group's image; the crowd calls it for EVERY slot before it uploads (its own bookkeeping dedups by image id: a sibling's upload must not
  // stand in for this model's adoption). A model in ANOTHER (retired) group leaves it (the reference goes back, its own image returns) and joins the current one. False = not in the current group
  // (no group yet / cannot); the model then keeps its own image.
  bool JoinCurrentPage(int model, size_t texture);
  // The key the group image behind (model, texture) holds now; ~0 = the texture is not on a group image (the caller's own bookkeeping holds).
  uint64_t PageKeyOf(int model, size_t texture) const;
  uint64_t PageGroupOf(int model) const;   // the page group the model sits on (0 = none / not live)
  uint32_t page_share_models() const { uint32_t n = 0; for (const auto& kv : page_shares_) n += uint32_t(kv.second.refs); return n; }   // models on a shared page image now (the planted twin reads 0)
  uint32_t page_groups() const { return uint32_t(page_shares_.size()); }   // live page groups (one arena set live = 1; a rebuilt set's old group lives until its models are released)
  uint64_t page_share_violations() const { return page_share_violations_; }   // AddModelSharing from an owner on a group page (refused; see AddModelImpl)
  // A page upload keyed `key` (the caller's key: 0 match page, 1 base, else the pools hash...). A model in a group: skipped when the group's image already holds `key`, or when the bytes equal the
  // last sent bytes (memcmp against a kept copy); else ONE upload to the shared image. With the +dev switch wwe2k14_rdr_page_per_caller ON every caller sends into the same image (the old byte cost
  // exactly: the Surface flips it within one run). A model not in a group: a plain UpdateTexture (tag Page).
  bool UpdatePage(int model, size_t texture, uint64_t key, uint32_t w, uint32_t h, const uint8_t* rgba);
  // S103 B4F: the GPU image behind a model's texture (0 = none); equal ids = shared (the crowd's classes share their stage page).
  uint64_t TextureImage(int model, size_t texture) const;

  // Draws the frame into an internal RGBA8 colour image (and depth) of frame.width x frame.height and reads it back as RGBA8.
  bool RenderNoWait(const SceneFrame& frame, uint32_t renders, std::string& error);   // S103 PF2, RIG ONLY: Record + Finish + `renders` x RenderFrame, no WaitIdle, no read-back (the host's tween shape: 2 Renders per Record)
  bool RecordAndDiscard(const SceneFrame& frame, bool note_dropped, std::string& error);   // S103 CB1, RIG ONLY (front_end.cpp)
  // S105 A1: out_w / out_h given = the read-back's real size (frame size x the render scale when the targets are scaled); null = the frame's size is required (the old contract).
  bool RenderToPixels(const SceneFrame& frame, std::vector<uint8_t>& rgba, std::string& error, uint32_t* out_w = nullptr, uint32_t* out_h = nullptr);
  // S105 A1: the scene targets (colour, depth, the grab, the post output) carry kUsageScaled: drawn at the player's resolution (the renderer's render_scale), not at the logical 1280x720.
  // Default off (the stream build's binder owns its own scaling; the offline rigs opt in per job); the stream-free host turns it on when render_scale != 1 (live_scene_draw.cpp EnsureFrontEnd).
  // Changing it remakes the targets at the next frame.
  void SetScaledTargets(bool on) { scaled_targets_ = on; }
  bool scaled_targets() const { return scaled_targets_; }
  // S105 A1: the real created sizes of the scene targets (log line / tests): false when the image is not made.
  bool TargetExtent(rdr::ImageId image, uint32_t& w, uint32_t& h, bool& scaled) const { return renderer_.ImageExtent(image, w, h, scaled); }
  const PostPass* post_pass_for_test() const { return post_pass_.get(); }   // S105 A1 (tests): the chain's images
  rdr::ImageId scene_colour_image() const { return color_; }
  rdr::ImageId scene_depth_image() const { return depth_; }
  // S103 lane S (FRAME_CONTRACT.md 8): records the frame into the CALLER's builder and targets (the live render thread's frame). Never begins,
  // finishes, waits or reads back. With frame.post set, the post output is post_output() (at the targets' size). RenderToPixels = Begin +
  // Record into the front-end's own images + Finish + wait + read back, so every offline tool is unchanged.
  bool Record(const SceneFrame& frame, rdr::FrameBuilder& builder, const LiveTargets& targets, std::string& error);
  rdr::ImageId post_output() const { return post_out_; }
  // S103 M (N3): the live screen copy is last frame's scene; when the live path did not draw the frame before (a walked stretch), it is stale: the next match-look frame must not show it.
  void InvalidateLiveScreen(LiveScreenWhy why) {   // S103 SCX: only a scene / arena / size change; counted when it took a valid grab away
    if (live_valid_) NoteLiveScreenInvalid(why);
    live_valid_ = false;
  }
  // WGS PAR1, TEST ONLY (frame_rig.cpp: a served job drops the grab like a one-shot process starts without one; the game never calls these): forget the grab without a census count
  void DropGrabForTest() { live_valid_ = false; }
  void PrimeGrabForTest(const uint8_t* rgba, uint32_t w, uint32_t h);   // a known picture sampled through the grab's own binding; null = cleared (front_end.cpp)
  // SCU (TEST ONLY: frame_rig --depth-out, tools\scene\stage_cull_proof.py): keep the scene depth stored (not transient) from the next frame on, and read the last frame's depth back
  // (4 raw bytes a texel, the device's depth format: D32F or X8_D24). Never called by the game.
  void KeepDepthForTest() { want_depth_stored_ = true; }
  bool ReadDepthForTest(std::vector<uint8_t>& raw, uint32_t& w, uint32_t& h);
  // S103 lane S: the front-end's own scene images at w x h for a live Record() (made once per size; the GPU is waited for only when the size
  // or the post choice changes). `post` = the frame will run the post pass (a sampled depth and the post output are made).
  LiveTargets OwnTargets(uint32_t w, uint32_t h, bool post, bool keep_depth = false);   // keep_depth (S103 PRT fix 2): the frame copies the scene depth (soft particles): the depth image must be stored, not transient. Sticky upward once; never makes the post output
  // S103 lane G (SCENE_PACING.md 3b): attach the kept draw lists (null = today: every draw's constants are resolved from scratch). The caller owns the lists
  // and calls DrawLists::Census() when it likes; Record does BeginFrame / EndFrame itself.
  void SetDrawLists(DrawLists* lists) { draw_lists_ = lists; }
  // S103 lane D: records one instanced draw into the caller's builder, inside a stage hook (the colour and depth targets are set). Returns the draws made;
  // a mesh that cannot be drawn instanced (no twin variant, an unsupported input) is counted in `stats` with the first reason, never drawn another way.
  uint32_t DrawInstanced(const InstancedDraw& draw, const StageCtx& ctx, rdr::FrameBuilder& fb, InstancedStats& stats);
  std::string InstancedRefusal(int model);   // S103 D2: "" or the reason DrawInstanced would refuse a mesh of this model (frame-independent checks)

  // The coverage of everything drawn since the last Reset: per material class name.
  std::vector<EffectCoverage> Coverage() const;
  std::string CoverageText() const;
  void ResetCoverage();
  uint64_t validation_errors() const { return renderer_.validation_errors(); }
  // S103 EYF: the last Record's piece-alpha decisions (the offline proof reads them): meshes of a piece row skipped at alpha 0, drawn with its alpha (> 0), and lid-layer meshes with no row skipped.
  struct PieceMeshStats { uint32_t skipped_alpha0 = 0, drawn = 0, lid_no_row_skipped = 0; };
  const PieceMeshStats& piece_mesh_stats() const { return piece_stats_; }
  // S108 HAIR census (one Record; the line `[scene][hair_cut]`): draws = the emitted draws of stock-piece translucent meshes (piece_draw_rule.h PieceCut; each of the two passes counts), cut = of those, alpha-tested
  // GEQUAL 128/255 WITH depth write (the game's state), notwin = draws whose pair has no alpha twin (blended, untested, no depth write; a draw_shortfall per pair), nodisc = a depth write with no discard (0 unless the TEST-ONLY plant).
  struct HairCutCensus {
    uint32_t draws = 0, cut = 0, notwin = 0, nodisc = 0;
    bool operator==(const HairCutCensus& o) const { return draws == o.draws && cut == o.cut && notwin == o.notwin && nodisc == o.nodisc; }
  };
  const HairCutCensus& hair_cut_census() const { return hc_; }   // the last Record's (the offline proof reads it)
  uint64_t wet_bound() const { return wet_bound_; }   // S103 SWF
  uint64_t wet_lost_shadow() const { return wet_lost_shadow_; }
  uint64_t wet_no_variant() const { return wet_no_variant_; }
  // S103 YCB: the wrestler materials' table pick (char_effects.h ChooseCharEffectTable), counted per distinct bind: table picks / cube rows bound as their `pass | 8` sibling (the interim until the slot-11 term) /
  // rows blocked for the wrinkle map (slot 5 unsettled) / every other no-table fallback (the old min-define pick); WRESTLER DRAWS (not distinct binds) whose model+616 mode the record did not carry (mode 0 assumed).
  uint64_t char_table_picked() const { return char_table_picked_; }
  uint64_t char_cube_analytic() const { return char_cube_analytic_; }
  uint64_t char_wrinkle_blocked() const { return char_wrinkle_blocked_; }
  uint64_t char_nontable_fallback() const { return char_nontable_fallback_; }
  uint64_t char_mode_unknown() const { return char_mode_unknown_; }
  // S103 WRK census (per wrestler DRAW unless noted): pass mode, wrinkle draws and whether their live levels arrived. One line, printed by live_scene_draw.cpp.
  struct WrkCensus {
    uint64_t m0 = 0, m2 = 0, m_other = 0, m_unknown = 0, m620[5] = {};   // m620[4] = not carried
    uint64_t e20_set = 0, e20_clear = 0, e20_unknown = 0, e20_would_fire = 0;
    uint64_t wrinkle_draws = 0, levels_missing = 0, wkl_nonzero = 0, idmap_nonzero = 0;
    uint64_t uv_degenerate = 0;   // draws whose wrinkle rectangle is degenerate (z == x or w == y: the x4 group): no wrinkle depth (wkl null, (0,0,1,1) fed); the id-map levels are still joined (S103 XLF); not counted in levels_missing
    uint64_t slot_fallback = 0;   // per distinct BIND: a wrinkle variant bound with the grey stand-in (the material has no wrinkles_n)
  };
  const WrkCensus& wrk_census() const { return wrk_; }
  std::string WrkCensusLine() const;
  uint64_t wet_unknown() const { return wet_unknown_; }   // S103 L9: skin / gear draws the game draws with its sweat / blood variants whose amount the record does not carry (kept on today's variant)
  uint64_t card_factor_missing() const { return card_factor_missing_; }   // S103 B3: Records with no card factor (no row, or the word 0): the card quad drew unscaled
  uint64_t identity_nodes_total() const { return identity_nodes_total_ + identity_nodes_frame_; }   // S103 CAWS (read by the unit / rig)
  uint64_t time_frozen() const { return time_frozen_; }   // S103 RCL: draws of a Time-reading (UVScroll) material with no clock row; their VS c186 Time stays 0 (the live record sets no scene time)
  uint64_t plane_wanted_plain() const { return plane_wanted_plain_; }   // S103 MSF
  // S103 RML (rig only): the ring mat's RESOLVED constant blocks (every draw bound to the plane / props pass), by field name, for tools\scene\rml_check.py (against the stream's op constants).
  struct MatProbeField { std::string name; uint32_t vec4s = 0; std::vector<float> v; };
  struct MatProbe { bool props_reader = false, plane = false, floor = false; rdr::ImageId slot12 = rdr::kNoImage; std::string vs, ps; std::vector<MatProbeField> vs_f, ps_f; };
  void SetMatProbe(bool on) { mat_probe_on_ = on; }
  void ClearMatProbes() { mat_probes_.clear(), mat_floor_probed_ = 0; }   // S103 RML (rig only)
  void SetPropsNudge(float v) { props_nudge_ = v; }   // S103 RML (rig only, frame_rig --lists): added to the props map's matrix[0] at Record, so the map moves between frames (the kept lists must follow it)
  const std::vector<MatProbe>& mat_probes() const { return mat_probes_; }
  uint64_t props_mat_bound() const { return props_mat_bound_; }   // S103 RML: ring-mat draws bound to the props-map pass (light_shadow)
  // S103 RFL: the reflection census line (cubes loaded: name + size; materials bound to a real cube / without one; reflect draws; counted fallbacks). One string, no newline.
  std::string ReflectLine() const;
  // S110 RDRPREP F3: the crowd camera flashes (after the post chain): sprites GIVEN to the frame (the arena's rows, one per flash), sprites DRAWN (not behind the camera, with a mesh), sprites skipped for want of a sampled depth. One string, no newline.
  std::string FlashLine() const;
  uint64_t flash_near() const { return flash_near_; }          // S110 FLASHOCC (tests)
  float flash_nearest() const { return flash_nearest_; }
  uint64_t reflect_draws() const { return reflect_draws_; }
  uint32_t reflect_fallbacks() const { return reflect_fallbacks_; }
  uint32_t reflect_cubes() const { return reflect_cubes_; }
  uint64_t plane_bound() const { return plane_bound_; }   // S103 L9: draws bound to the plane-shadow reader (the ring mat)
  uint64_t d1_rule() const { return d1_rule_; }   // S103 D1: stage-slot draws whose pair the read rule chose (read_rule.h)
  uint64_t d1_old() const { return d1_old_; }     // S103 D1: stage-slot draws left on the old heuristics (no table / no pass / unsupported pair / WWE_RDR_D1_OLD)
  uint64_t pms_site(int i) const { return pms_site_[i]; }   // S103 PMS (rig): 0 reader, 1 matrix, 2 use_shadow, 3 pick key, 4 ctx matrix + prm, 5 slot image
  rdr::ImageId pms_slot12() const { return pms_slot12_; }
  uint64_t props_bound() const { return props_bound_; }           // S103 PMS: floor draws (yBG_* FX_SHADOW, slot 12) bound to the props map
  uint64_t d1_shadow_nomap() const { return d1_shadow_nomap_; }   // S103 PMS: floor receivers whose map (the props map; map B with the switch off) was missing: drawn without +2, counted
  uint64_t shadow_raw_bound() const { return shadow_raw_bound_; }   // S103 L6: character draws bound to the shadow reader whose map is raw (no blur chain yet)

 private:
  struct ModelData;
  struct Bound;
  ModelData& Mod(int id);
  int AddModelImpl(const assets::Model& model, std::vector<std::string>& warnings, int image_owner, std::unordered_map<std::string, rdr::ImageId>* shared);  // S103 R + D
  const Bound& BindMaterial(ModelData& m, int material, bool skinned, LmMode lm_mode = LmMode::Own, bool shadow = false, bool wet = false, int char_pass = -1, bool light = false, bool change_colour = false);   // S108 THR2: change_colour = a recoloured body's draw (char_pass >= 0 only): the STS row + the masks (change_colour_draw.h)   // S103 YCB: char_pass >= 0 = a wrestler material bound to the game's table row for (technique, pass) (technique_pick.h CharPass; the cache key is the pass, `shadow` / `wet` are then read from the pick); !Ok falls back to the old pick  // S103 SWF: wet = the liquid-map reader of a skin material (char_effects.h ChooseCharEffect)  // S103 L10: lm_mode 0 = the material own rule, 1 = the entrance LM variant (was the bool ent_lm), 2 = a MATCH arena mesh the game gives the page in the VERTEX stage only (Material::vertex_lm), 3 = a MATCH arena mesh with no page at all  // S103 L6: shadow = the FX_SHADOW reader variant of a character material  // (S101 lane 13: skinned picks the vertex variant of a character)
  void EnsureTargets(uint32_t w, uint32_t h, bool wait_idle = true);   // S103 P: wait_idle = false on the live path (the renderer's deferred DestroyImage covers the old images)
  rdr::ImageId MakeTexture(const assets::Texture& t, std::string& warning);
  uint16_t ProgramFor(int vs, int ps, bool alpha);
  void ReloadPrograms(const std::vector<int>& changed);  // S101 lane24: the live shader reload changed SPIR-V (shader_reload.h): forget every program and every material's binding
  uint16_t LayoutFor(int vs);
  uint16_t InstLayoutFor(int vs);  // S103 lane D: InstVertex (binding 0) + the world rows per instance (binding 1)
  std::map<int, uint16_t> inst_layouts_;
  std::vector<uint8_t> inst_block_;  // S103 lane D: DrawInstanced's constant block scratch

  // S110 CUTGREY (front_end_flashes.h, #included by front_end.cpp): the flash sprites onto the finished picture.
  struct FlashMesh {
    rdr::BufferId vb = rdr::kNoBuffer, ib = rdr::kNoBuffer;
    uint32_t vb_offset = 0, ib_offset = 0, index_count = 0;
    rdr::ImageId map = rdr::kNoImage;
    float amb[4] = {1, 1, 1, 1}, dif[4] = {1, 1, 1, 1};   // the material's MatAmbCol / MatDifCol
  };
  bool FlashMeshesOf(int model, std::vector<FlashMesh>& out);
  uint32_t DrawFlashesAfterPost(const SceneFrame& frame, rdr::FrameBuilder& fb, rdr::ImageId colour, rdr::ImageId depth, const StageCtx& sctx);
  bool flash_made_ = false, flash_nodepth_logged_ = false;
  uint64_t flash_given_ = 0, flash_drawn_ = 0, flash_nodepth_ = 0;   // S110 RDRPREP F3: sprites given to the after-post draw / drawn by it / skipped because the depth was not sampled
  uint64_t flash_near_ = 0; float flash_nearest_ = 0.f; uint32_t flash_near_lines_ = 0;   // S110 FLASHOCC: drawn sprites with centre view z < kFlashNearViewZ, the smallest centre view z seen (0 = none), near lines logged (cap 32)
  uint16_t flash_program_ = 0, flash_layout_ = 0;
  rdr::ImageId flash_depth_ = rdr::kNoImage;   // S110 FLASHFRONT: the scene depth's copy the centre read samples (the scene depth itself is the flash pass's attachment)
  uint32_t flash_depth_w_ = 0, flash_depth_h_ = 0;
  bool flash_depth_scaled_ = false;

  rdr::Renderer& renderer_;
  FrontEndOptions options_;
  int layer_fallbacks_ = 0;   // S103 LAY: multi-layer picks that fell back to one layer (printed)
  uint32_t reflect_cubes_ = 0;                    // S103 RFL: cube textures (file DDS, 6 faces) uploaded as cube images
  std::string reflect_cube_names_;                // S103 RFL: their names + sizes (the first distinct ones)
  // S105 SZW census (one Record, stage-slot draws by the game's rule; the line `[scene][szw]`): game = draws the game's rule gives a depth write (an `s` / `a` slot without `z`, a blended screen piece), ours = of the
  // draws of any `s` / `a` / `z` / screen-piece mesh, the ones whose final state writes depth, nodisc = of those, the ones with NO discard (must be 0 with the switch on), notwin = game draws whose pixel variant has no alpha twin
  // (they keep depth write off), notwin_names = those variants. Reset at the start of a Record, logged when the numbers change.
  struct SzwCensus {
    uint32_t game = 0, ours = 0, nodisc = 0, notwin = 0, order_items = 0;
    std::set<std::string> notwin_names;
    bool operator==(const SzwCensus& o) const { return game == o.game && ours == o.ours && nodisc == o.nodisc && notwin == o.notwin && order_items == o.order_items; }
  };
  SzwCensus szw_, szw_logged_;
  int szw_lines_ = 0;
  HairCutCensus hc_, hc_logged_;   // S108 HAIR (the struct and its meaning: hair_cut_census above)
  int hc_lines_ = 0;
  uint32_t reflect_binds_ = 0, reflect_no_cube_ = 0, reflect_fallbacks_ = 0;   // S103 RFL: reflective materials bound to the file cube / bound with no cube to feed (black, alpha 0) / whose game variant could not be bound (counted fallback)
  uint64_t reflect_draws_ = 0;                    // S103 RFL: draws that sampled a real file cube with ReflectAlpha fed
  int bgrefl_picked_ = 0, bgrefl_fallbacks_ = 0;   // S103 RFL: yBG_Reflect / ReflectLM materials bound by the technique table / by the counted fallback (apart from TQF's yProp counters)
  int tech_picked_ = 0, tech_fallbacks_ = 0;   // S103 TQF: yProp materials bound by the technique table / by the counted sampler-scoring fallback (printed)
  std::vector<ModelData*> models_;
  std::deque<std::string> names_;
  std::map<uint64_t, uint16_t> programs_;
  std::map<int, uint16_t> layouts_;
  std::unique_ptr<PostPass> post_pass_;                // S101 lane 34 (made on first use; destroyed with the front-end, before the renderer)
  rdr::ImageId post_out_ = rdr::kNoImage;              // S101 lane 34: the post pass's output at the scene's size
  bool depth_sampled_ = false, want_depth_sampled_ = false;  // S101 lane 34
  uint32_t logged_target_w_ = 0, logged_target_h_ = 0;
  int logged_target_scaled_ = -1;   // S105 A1 review F5: the last size / flag the [scene][targets] line printed
  bool scaled_targets_ = false, targets_scaled_ = false;     // S105 A1: the setting / what the current scene targets were made with
  bool depth_stored_ = false, want_depth_stored_ = false;    // S103 PRT fix 2: a non-transient depth for the soft-particle depth copy (no post output made for it)
  rdr::ImageId white_ = rdr::kNoImage, grey_ = rdr::kNoImage, color_ = rdr::kNoImage, depth_ = rdr::kNoImage;
  rdr::ImageId black_ = rdr::kNoImage, neutral_normal_ = rdr::kNoImage, black_cube_ = rdr::kNoImage;  // S101 lane 13: all-zero layers / rim map; a normal map with no bump and half gloss
  struct CubeEntry { rdr::ImageId image = rdr::kNoImage; float scale[4] = {1, 1, 1, 1}; uint64_t used = 0; };   // S103 E8: used = the tick of the last CubeFor (LRU)
  uint64_t cube_tick_ = 0, cube_built_ = 0, cube_evicted_ = 0;   // S103 E8: the LRU clock and the census
  std::unordered_map<uint64_t, CubeEntry> cubes_;  // the ambient cube of a light set (char_effects.h BuildAmbientCube), by LightSetKey
  const CubeEntry& CubeFor(const assets::LightSet& ls);
  uint32_t target_w_ = 0, target_h_ = 0;
  // S101 lane 257: the stage's big screen (stage slot 800) shows the previous frame's scene colour in the match look (the viewer's rule, renderer.cpp "live screen"):
  // live_screen_ is that copy at the target's size, live_valid_ says a frame has been copied into it, in_seed_ marks the extra frame that seeds it (a still shows the screen's picture).
  rdr::ImageId live_screen_ = rdr::kNoImage;
  bool live_valid_ = false, in_seed_ = false;
  rdr::ImageId grab_prime_ = rdr::kNoImage;   // WGS PAR1 (TEST ONLY): PrimeGrabForTest's picture
  uint32_t grab_prime_w_ = 0, grab_prime_h_ = 0;
  uint64_t texture_updates_ = 0;  // S101 lane 15
  uint64_t tex_bytes_[size_t(TexTag::Count)] = {}, tex_count_[size_t(TexTag::Count)] = {};   // S103 UP
  uint64_t page_skip_key_ = 0, page_skip_hash_ = 0;
  uint64_t scr_pics_ = 0, scr_shared_ = 0, scr_moved_ = 0, scr_images_ = 0;   // S103 UPL U2
  uint64_t scr_batches_ = 0, scr_rebind_batches_ = 0, scr_batch_no_ = 0;
  void ResetModelBindings(ModelData& m, int model);   // S103 UPL: a model's texture-slot image changed: bound materials hold the old id (bound reset, rule_bound / rule_pick clear, kept draw lists released; model < 0 = the caller releases those)
  int ScrPrivateEntry(ModelData& m, int model, size_t texture);   // S103 UPL: a pool entry only `texture` shows (a direct write must not touch a shared picture)
  bool UpdateTextureDirect(int model, size_t texture, uint32_t w, uint32_t h, const uint8_t* rgba, TexTag tag);   // the one-image-per-texture write (the old UpdateTexture body)
  struct PageShare { rdr::ImageId image = rdr::kNoImage; int refs = 0; uint64_t key = 0; std::vector<uint8_t> sent; uint32_t w = 0, h = 0; };   // sent = the bytes last uploaded (the same-bytes guard)
  std::unordered_map<uint64_t, PageShare> page_shares_;   // group -> its one page image
  uint64_t page_group_current_ = 0, page_group_build_ = 0, page_share_violations_ = 0;
  bool PosedPush(rdr::FrameBuilder& fb, const void* data, uint32_t bytes, uint32_t& offset);   // S103 PVR: one push under the per-frame posed budget + the renderer's real room; false (counted) = use the buffer path
  void NoteRingFrame(const rdr::FrameBuilder& fb);   // S103 PVR: at the end of a Record: what the OTHER producers pushed this frame (the room a later frame must leave them)
  uint64_t posed_epoch_ = 0;
  uint32_t posed_bytes_ = 0, other_est_ = 0, const_est_ = 0;
  std::map<uint64_t, uint32_t> plant_cloth_prev_;   // PLANT 3 (rig only): the cloth pieces' ring offsets of the previous Record
  bool PosedDirectOn() const;   // S103 PF2: the direct path is wanted here (g_posed_ring && g_posed_direct && a unified-memory device or the test force)
  bool PosedDirectWrite(ModelData& m, const assets::Model& model);   // S103 PF2: the skinned vertices straight into the next free mapped copy; false = no copy free (counted) or none could be made: the caller takes the other paths
  rdr::BufferId PosedVertexBuffer(ModelData& m);   // S103 PF2: the buffer a draw of this model reads its vertices from (the current copy, marked in use by this frame's renders, or `vb`)
  void SweepPosedCopies();   // S103 PF2: frees the copies of models at rest for kPosedRestRenders (every 60th Record)
  void NotePosedCensus();   // S103 PF2: every 600 Records, one [scene][posed] line (window deltas of the counters)
  uint64_t posed_census_[6] = {};   // the totals at the last line
  uint32_t posed_census_frames_ = 0;
  bool PosedRingBase(rdr::FrameBuilder& fb, ModelData& m, uint32_t& base);   // S103 PVR: this frame's ring offset of a model's posed vertex block (pushed once per frame on the first draw); false = draw from the model's own buffer (made current first when the ring had no room)
  void LeavePage(ModelData& m, bool restore);   // S103 UP: gives the model's reference on its group page back (destroyed with the last); restore = put the model's own image back (a borrowed model that stays alive)
  uint64_t shadow_raw_bound_ = 0;  // S103 L6
  uint64_t d1_rule_ = 0, d1_old_ = 0, d1_shadow_nomap_ = 0, d1_skin_standin_ = 0, d1_no_pass_ = 0, d1_phase_guess_ = 0;   // S103 D1: draws chosen by the read rule / left on the old path / counted stand-ins
  const Bound* BindRule(ModelData& m, int material, bool skinned, int vs, int ps, bool shadow);   // S103 D1
  uint64_t plane_wanted_plain_ = 0;   // S103 MSF (RML: only the plain case: no plane map and not the light pass)
  uint64_t props_mat_bound_ = 0;      // S103 RML
  bool mat_probe_on_ = false;         // S103 RML (rig only)
  float props_nudge_ = 0.f;           // S103 RML (rig only)
  MapImage props_nudged_;             // S103 RML (rig only): the copy Record reads when props_nudge_ != 0
  MapImage census_maps_[3];           // S106 PSEED (tool only): the stand-in shadow / props / plane maps (options_.census_maps)
  rdr::ImageId mat_probe_slot12_ = rdr::kNoImage;   // S103 RML (rig only)
  bool mat_probe_live_ = false;       // S103 RML (rig only): the vertex block of the current draw was probed, so its pixel block is too
  uint32_t mat_floor_probed_ = 0;     // S103 RML (rig only): floor receivers probed (the first 4)
  std::vector<MatProbe> mat_probes_;  // S103 RML (rig only)
  uint64_t plane_bound_ = 0;       // S103 L9
  uint64_t props_bound_ = 0;       // S103 PMS
  uint64_t pms_site_[6] = {};      // S103 PMS: the six re-pointed floor sites, each counted when it ran on the props map (rig census)
  rdr::ImageId pms_slot12_ = rdr::kNoImage;   // S103 PMS: the image the last floor draw bound at slot 12 (the rig compares it with the hook's props image)
  void ApplyLmRow(ModelData& m);   // S103 B3
  void SetCardFactor(bool valid, float factor);
  float card_factor_ = 0.f;
  uint64_t card_factor_missing_ = 0;
  uint64_t time_frozen_ = 0;       // S103 RCL
  PieceMeshStats piece_stats_;   // S103 EYF
  uint64_t identity_nodes_frame_ = 0, identity_nodes_total_ = 0, identity_lines_ = 0;  // S103 CAWS: skin-node lookups >= the palette size (drawn with identity), this frame / all frames / lines printed
  WrkCensus wrk_;   // S103 WRK
  uint64_t char_table_picked_ = 0, char_cube_analytic_ = 0, char_wrinkle_blocked_ = 0, char_nontable_fallback_ = 0, char_mode_unknown_ = 0;   // S103 YCB
  uint64_t wet_unknown_ = 0;       // S103 L9
  uint64_t wet_lost_shadow_ = 0;   // S103 SWF: a skin with a map and a shadow map but no wet+shadow pair: drawn dry + shadow
  uint64_t wet_bound_ = 0, wet_no_variant_ = 0;   // S103 SWF: skin draws bound to the wet variant (the wrestler had a liquid map) / skin draws that had a map and no wet pair of the material's effect (drawn dry, counted)
  std::map<std::string, EffectCoverage> coverage_;
  std::set<std::string> missing_fields_;
  rdr::FrameBuilder builder_;
  DrawLists* draw_lists_ = nullptr;  // S103 lane G (not owned)
  int debug_vs_ = -1, debug_ps_ = -1;
};

}  // namespace rdr_scene
