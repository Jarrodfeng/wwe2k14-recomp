// In-place draw replacement: the shared seam of every class promoted DRAW BY DRAW (Session 101 contract, main session; one seam for the arena
// docs\renderer\ARENA_PROMOTION_MAP.md section 4 and the wrestlers docs\renderer\WRESTLER_PROMOTION_MAP.md section 4, instead of one per class).
// The crowd keeps its own seam (crowd_hook.h: one injection at the first crowd draw, the stream's crowd draws dropped).
//
// What "in place" means: the stream's draw keeps its place in the frame, its target, program, fixed and dynamic state, every constant the hook
// does not name and every texture slot the hook does not name (light-map page, shadow maps, cube maps, sweat / damage maps: whatever the game
// made at run time). The hook supplies only what it owns: the vertex and index streams, the vertex layout, the file textures it names, and (the
// wrestlers) a run of vertex-constant rows it overrides (the skin palette). Everything is checked per draw against the stream draw; a refusal
// keeps that draw the stream's and is counted by reason. No game or SDK includes.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../renderer/api.h"
#include "../renderer/frame_builder.h"
#include "blend_key.h"  // lane 159 (S2)
#include "capture_reader.h"
#include "names.h"

namespace rdr_adapter {

// The stage / model-part colour sites of sub_82C04DF0 [C: Check B 7051 capture, both design readers]: 0x82C0A950 draws a part's stored copy;
// 0x82C0B94C draws the mirrored `r` twin of an arena slot (ViewProj c56-c59 with x negated). Origin pass_site values.
inline constexpr uint32_t kModelPartColourSite = 0x82C0A950u;
inline constexpr uint32_t kModelPartMirrorSite = 0x82C0B94Cu;
// S101 lane 99 (SHADOW_PROMOTION_MAP.md section 4): the two shadow-pass part-draw sites. 0x82C04634 = T_ShadowMap (the props map and the wrestler maps A / B), 0x82BF4D38 = T_PlaneShadowMap (the plane mask's proxies).
inline constexpr uint32_t kShadowMapSite = 0x82C04634u;
inline constexpr uint32_t kPlaneShadowSite = 0x82BF4D38u;

// One candidate draw, as the binder knows it before it builds the draw (after the variant choice and the layout, before State / Textures /
// Geometry / Draw).
struct ReplaceIn {
  const LoadedFrame* frame = nullptr;
  const LoadedFrame* join_frame = nullptr;      // lane 228: the REAL bound frame whose draws the hook's per-frame object tables were built from (always set; `frame` is a D3 caster marker's mini frame while one is drawn). Object joins (a wrestler by its colour draws) must ask this one
  const Draw* draw = nullptr;
  uint32_t draw_index = 0;
  const DrawOrigin* origin = nullptr;           // class, call_site, pass_site, object, part (frame.OriginOf(draw_index))
  int vs = -1, ps = -1;                         // rdr::fx::kVariants indices the binder chose
  uint32_t vertex_count = 0, index_count = 0;   // the stream draw's own counts (the per-draw check)
  const rdr::TextureSet* textures = nullptr;    // the stream draw's slots (checks; run-time slots stay)
  const uint8_t* vs_block = nullptr;            // the stream draw's packed vertex constants (Binder::PackConstants' layout), read only
  uint32_t vs_block_bytes = 0;
  // The draw's captured vertex / index bytes by offset (content joins, verify). Empty: not available.
  std::function<bool(uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out)> read_geometry;
  uint64_t vertex_offset = 0, index_offset = 0;
  uint32_t vertex_stride = 0;
  // The stream draw's own geometry description (lane 41 review 1): a hook that keeps the stream's layout / topology / index width (the
  // wrestlers: the file's strip in the stream's host layout) copies these into ReplaceOut; a hook with its own buffers sets its own.
  uint16_t stream_layout = 0;
  uint8_t stream_topology = 0;
  uint8_t stream_index_32bit = 0;
  // lane 151: the stream draw's packed PIXEL constants (Binder::PackBlock's layout), read only; set only when the hook says NeedsFragmentBlock().
  const uint8_t* fs_block = nullptr;
  uint32_t fs_block_bytes = 0;
  // lane 151: a capture image id (a frame texture slot's / copy's `image`) -> the renderer's image id the binder uses for it (what ReplaceOut::slots names); also only for a hook that says NeedsFragmentBlock().
  std::function<rdr::ImageId(uint32_t)> map_image;
};

// What replaces the stream draw's parts. Anything not set here is the stream draw's.
// The three geometry-description fields start UNSET (the sentinels below): a Replace that returns true with any of them still unset is a
// refusal counted as Mismatch (never a silent "the stream's layout with our buffer": a stride mismatch reads past the buffer, and the device
// has no robustBufferAccess). Copy them from ReplaceIn::stream_* to keep the stream's.
inline constexpr uint16_t kReplaceUnsetLayout = 0xFFFFu;
inline constexpr uint8_t kReplaceUnset8 = 0xFFu;
struct ReplaceOut {
  rdr::BufferRange vertex{};                    // ours, game space (no Y mirror)
  rdr::BufferRange index{};                     // ours (kNoBuffer: not indexed, index_count vertices)
  uint32_t index_count = 0;
  uint8_t index_32bit = kReplaceUnset8;         // 0 / 1, must be set
  uint16_t vertex_layout = kReplaceUnsetLayout; // must be set: ours (built by name from the variant's inputs) or ReplaceIn::stream_layout
  uint8_t topology = kReplaceUnset8;            // VkPrimitiveTopology, must be set; primitive restart follows the stream draw's state
  // A texture slot the hook replaces. The hook's image must be one the program samples in that slot (a set slot the program does not read
  // is counted, not applied), and the stream draw must bind an image there (an empty stream slot: refused as Texture: no sampler to use).
  // view_format / swizzle: Undefined / identity = the hook image's own; the stream slot's are NOT kept (they describe the game's texture).
  struct Slot {
    bool set = false;
    rdr::ImageId image = rdr::kNoImage;
    rdr::Format view_format = rdr::Format::Undefined;
    uint8_t swizzle[4] = {0, 0, 0, 0};
  };
  Slot slots[rdr::kMaxTextures];                // set: our image in that slot (its sampler stays the stream's)
  // Vertex-constant rows to override in the packed block (e.g. the wrestler skin palette from the scene record): `rows` float4 rows starting at
  // register `first_register`, copied over the stream's block by the binder (a copy of the block; the stream's bytes are never written). 0 rows:
  // the stream's constants unchanged.
  uint32_t first_register = 0, rows = 0;
  const float* row_data = nullptr;              // rows * 4 floats, owned by the hook until the next Replace call
  // S101 UI contract (UI_PROMOTION_MAP.md 4, lane U0): true = the stream's geometry stays (vertex / index streams, layout, topology, count, index width: every
  // geometry field above is ignored and may stay unset); only `slots` (and rows, if any) apply. For a class that owns a draw's textures, not its shape (the UI's
  // 2D quads). A yes with this set and no slot applied is counted as replaced all the same (the hook decided the draw is ours).
  bool keep_stream_geometry = false;
  // S101 lane 89 (UT, UI_PROMOTION_MAP.md 7 P2b): with keep_stream_geometry, the draw's vertex bytes are THESE (one frame's copy: the binder pushes them into the frame's geometry
  // right away, so the hook may reuse the buffer on its next call). Layout, topology, index and count stay the stream's; the byte count must equal the stream draw's vertex_bytes
  // (otherwise the binder keeps the stream's vertices). For a class that owns a 2D quad's shape per draw (our glyph quads), not a static mesh.
  const uint8_t* inline_vertices = nullptr;
  uint32_t inline_vertex_bytes = 0;
  // S101 lane 131 (MovieOurs, MOVIE_OURS.md): the draw is made with OUR program instead of the stream's. The stream draw keeps its place, target, fixed and dynamic state (blend, depth, cull,
  // viewport) and its texture slots as bound (plus any `slots` the hook sets); the program, vertex layout, topology, vertex count and BOTH constant blocks are the hook's. No vertex or index
  // buffer is bound (a non-indexed draw of `count` vertices: the program builds its corners from gl_VertexIndex). The stream's constants are not packed for it and it is never tweened.
  // The blocks are copied by the binder before the next Replace call. `program` / `vertex_layout` are rdr::Renderer AddProgram / AddVertexLayout ids the hook made itself.
  struct OwnDraw {
    bool set = false;
    uint16_t program = 0, vertex_layout = 0;
    uint8_t topology = 3;
    uint32_t count = 0;
    const uint8_t* vs_constants = nullptr;
    uint32_t vs_bytes = 0;
    const uint8_t* fs_constants = nullptr;
    uint32_t fs_bytes = 0;
    // lane 159 (SHADOW_PLANE.md): our program and constants on the STREAM's geometry. The stream draw's vertex / index streams, vertex layout, topology, index width and counts all stay (the
    // program must declare the stream variant's vertex inputs at the same locations and types); `vertex_layout`, `topology` and `count` are ignored. The stream's constants are still not
    // packed for it and it is not tweened. For a class that owns a draw's program and constants but not its mesh (the plane shadow's proxies and blur quad).
    bool stream_geometry = false;
    // lane 255 (SHADOW_PLANE.md "Lane 255"): our program and constants block on OUR mesh: the binder's casters + plane join sets it (never a hook): the geometry is ReplaceOut's own fields (vertex, index, index_count,
    // vertex_layout, topology, index_32bit) and the primitive restart stays the stream's. Exclusive with stream_geometry.
    bool hook_geometry = false;
    // lane 159 (review S2): byte ranges of `vs_constants` the 120 Hz frame pump may interpolate between two game ticks (a shape that follows a bone). The binder keys such a draw like a
    // stream draw (shaders, geometry, occurrence) XOR a salt for own draws, so a draw owned this tick and the stream's the next never pair blocks of different layouts. None: never interpolated.
    rdr::BlendRange blend[4] = {};
    uint32_t blend_count = 0;
  } own;
};

// Why a candidate stayed the stream's (counted per hook).
enum class KeepWhy : uint8_t { NotReady, Unjoined, Mismatch, Variant, Texture, Palette, NotOurs, Count };
inline const char* KeepWhyName(KeepWhy w) {
  static const char* const k[] = {"not ready", "unjoined", "mismatch", "variant", "texture", "palette", "not ours"};
  return uint8_t(w) < uint8_t(KeepWhy::Count) ? k[uint8_t(w)] : "?";
}

class DrawReplaceHook {
 public:
  virtual ~DrawReplaceHook() = default;
  virtual const char* Name() const = 0;          // "arena", "wrestlers": the census line's tag ([rdr][NAME])
  // Cheap: is this draw one this class might replace (origin class / pass site / object)? The binder asks the hooks in registration order;
  // the first that claims gets Replace. Must not allocate.
  virtual bool Claims(const DrawOrigin& origin) const = 0;
  // Once per frame, before its first claimed draw: is there a usable record and are the assets uploaded? False: every claimed draw of this
  // frame stays the stream's (a fallback frame).
  virtual bool FrameReady(const LoadedFrame& frame) = 0;
  // One claimed draw. True: `out` is filled, the binder applies it. False: the draw stays the stream's; `why` says why. KeepWhy::NotOurs (and a
  // hook whose FrameReady said no) passes the draw to the next registered hook that claims it (classes sharing a site: arena and wrestlers);
  // every other refusal is final.
  virtual bool Replace(const ReplaceIn& in, ReplaceOut& out, KeepWhy& why) = 0;
  // A bound frame with nothing this hook claimed (a load screen; key the release on the record, not on draws: the crowd's review F3).
  // FrameReady / NoClaimFrame run once per Bind, not once per guest frame (a frame split at a readback is bound twice; an isolate
  // replays one frame many times): count frames, do not time out by them.
  virtual void NoClaimFrame() {}
  // lane 151: true = ReplaceIn::fs_block is filled for this hook's claimed draws (packing it costs a block copy a draw, so only a hook that reads pixel constants asks).
  virtual bool NeedsFragmentBlock() const { return false; }
  // lane 122 (D1 thin draws): counts the times the hook destroyed the buffers / images its Replace results name (an arena change). A thin template is valid only for the epoch it was learned in. 0 = never changes.
  virtual uint32_t AssetEpoch() const { return 0; }
  // lane 122 (review 2): objects whose join the hook dropped (a failed per-draw check): any thin template of those objects is void. Seq changes when there is something to take.
  // lane 129 review F2 / F6 (thin draws): the thin binder is a consumer of a hook's asset facts. WantJoinDrops(true): the hook records dropped joins and per-part facts only when asked (nothing is kept for nobody).
  // BufferReleaseSeq / TakeReleasedBuffers: the ids of the single buffers the hook destroyed (a part swept), so only the templates naming them go. TestReleaseOne: TEST ONLY (fview --thin-release-part).
  virtual void WantJoinDrops(bool) {}
  virtual uint32_t BufferReleaseSeq() const { return 0; }
  virtual void TakeReleasedBuffers(std::vector<uint64_t>&) {}
  virtual void TestReleaseOne() {}
  virtual uint32_t JoinDropSeq() const { return 0; }
  virtual void TakeJoinDrops(std::vector<uint32_t>&) {}
  // lane 227 (Wrestlers D3 H2): the part's own palette length (the file's `palette_count`: the number of bone slots c76 + 3k the part's vertices can address; registers past it are never read). 0 = unknown (no joined model for the
  // object, or the part is not in it). Read only; the binder's D3 chain asks it for the don't-care slots.
  virtual uint32_t PartPaletteCount(uint32_t /*object*/, uint32_t /*part*/) const { return 0; }
  // S101 lane 100: the hook's own account of WHY it kept draws, logged after every census window (live never reaches the end-of-run report: the process hard-exits). Empty = none.
  virtual std::string WindowDetail() const { return {}; }
  // lane 255: the binder joined this hook's own draw with the shadow casters' mesh (both switches on). Counted by the hook ('joined' in its census).
  virtual void NoteMask(bool /*joined_with_casters_mesh*/) {}   // (was NoteJoined) called by the binder once per replaced plane-site mask draw with the binder's own result
  // lane 255 (TEST ONLY, fview --replace-fault plane.join_double / plane.join_drop): 0 none, 1 the binder asks this hook for a joined draw twice, 2 it drops every 7th joined draw (neither claimed nor kept).
  virtual uint32_t JoinFault() const { return 0; }
};

// Per hook, one census line per window: "[rdr][NAME] ours frames N, fallback F, replaced R, kept K (not ready a, unjoined b, ...), ms T".
struct ReplaceCounters {
  uint64_t ours_frames = 0, fallback_frames = 0;
  uint64_t persist_only = 0;   // lane 239: frames bound as a skipped frame's lasting work (never shown): counted apart, not in ours / fallback
  uint64_t replaced = 0;
  uint64_t joined = 0;   // lane 255: draws made with the casters' mesh AND the plane hook's program (counted on the plane hook's slot)
  uint64_t kept[uint8_t(KeepWhy::Count)] = {};
  double ms = 0;
};

// S101 lane 41 / ring contract: the switches of the classes promoted draw by draw (moved here from binder.h). A hook added with Binder::AddReplaceHook names
// its switch; all default off on PC and phone (live: --wwe2k14_rdr_arena_ours / _wrestlers_ours / _ring_ours, all need --wwe2k14_scene_records; fview / rplay:
// --arena-ours / --wrestlers-ours / --ring-ours). g_replace_test is fview's test hook only (--replace-test). The ring is registered after the arena and the wrestlers.
inline bool g_arena_ours = false, g_wrestlers_ours = false, g_replace_test = false;
inline std::string g_caw_cache_root;  // lane 153: the game's cache root / caw (--cache_root, the same place the shader and pipeline caches live; run_list points it at out\bench for test runs): the edit pacs' counts index is cached there. Empty = no cache
inline bool g_caw_ours = false;  // lane 153 (N3): the Create a Superstar editor's wrestler by us in place (caw_core.h); no scene record needed; live --wwe2k14_rdr_caw_ours, fview --caw-ours
inline bool g_ring_ours = false;
inline bool g_flash_ours = false;  // S101 flash contract (PARTICLES_PROMOTION_MAP.md 3 / 9 lane F): the crowd flashes (pass site 0x82C139FC) from bgNN.pac 31000; live --wwe2k14_rdr_flash_ours, fview / rplay --flash-ours
inline bool g_screens_ours = false;  // S101 lane 115: the stage screens (pass site 0x82C0F4BC, keys 801-807 of the arena pac, geometry only); live --wwe2k14_rdr_screens_ours, fview / rplay --screens-ours
inline bool g_crowd_props_ours = false;  // S101 lane 133: the crowd's sign / foam-hand draws (call sites 0x82518D60 / 0x825192D0, the two per-member loops) by us in place from the arena pac's audience set; fate crowd_ours; live --wwe2k14_rdr_crowd_props_ours (needs scene records, for the arena), fview --crowd-props (rplay: not wired)
inline bool g_props_ours = false;  // S101 lane 84: the props of other pacs (commentary table, ring steps, announcer microphone; pass site 0x82C0A950, after the ring); live --wwe2k14_rdr_props_ours, fview / rplay --props-ours
inline bool g_shadow_casters_ours = false;  // S101 lane 99: our caster geometry in the shadow passes (sites 0x82C04634 / 0x82BF4D38; needs the wrestlers' and / or the ring's joins); live --wwe2k14_rdr_shadow_casters_ours, fview / rplay --shadow-casters-ours
inline bool g_small_passes_ours = false;  // lane 135: the wrestlers' sweat map (yLiquidMap) and skin pre-light (yPreLight) passes drawn by us in place (fate 18); live --wwe2k14_rdr_small_passes_ours, fview / rplay --small-passes-ours; needs no scene record
inline bool g_rim_ours = false;  // lane 124: our caster geometry in the rim light map (T_RimLightMap, site 0x82BFBD58; the wrestlers' joins); live --wwe2k14_rdr_rim_ours, fview / rplay --rim-ours
inline bool g_rim_chain_ours = false;  // lane 151: the rim light map's coverage reductions (call 0x82C1F48C) and blur (call 0x82814080) drawn by our own programs on the stream's draws (fate 21); live --wwe2k14_rdr_rim_chain_ours, fview / rplay --rim-chain-ours; needs no scene record and no game file
inline bool g_plane_shadow_ours = false;  // lane 139 (SH3 slice 2): the plane shadow chain's constants (mask T_PlaneShadowMap site 0x82BF4D38, blur call 0x827EEC84) from the scene record's PlaneDraw entries; live --wwe2k14_rdr_plane_shadow_ours, fview / rplay --plane-shadow-ours
inline bool g_circles_ours = false;  // lane 136 (SH3 slice 1): the circle blob shadows (T_CircleShadow, call site 0x828131E4) drawn from the scene record's CircleShadow entries; live --wwe2k14_rdr_circles_ours, fview / rplay --circles-ours
inline bool g_ui_text_ours = false;  // S101 lane 89 (UT): the game's text drawn by us (glyph quads laid out by our pen rule, UI_PROMOTION_MAP.md 6 / 7); live --wwe2k14_rdr_ui_text_ours, fview / rplay --ui-text-ours; registered before the UI's
inline bool g_ui_ours = false;  // S101 UI contract: the 2D draws' textures (UI_PROMOTION_MAP.md); live --wwe2k14_rdr_ui_ours, fview / rplay --ui-ours; registered last
inline bool g_movie_ours = false;  // S101 lane 131: the movie planes (T_SpriteYUVAlpha, call site 0x82BC7E50) drawn by our quad and our YUV shader; live --wwe2k14_rdr_movie_ours, fview / rplay --movie-ours; needs no scene record and no game file; registered before the UI's

}  // namespace rdr_adapter
