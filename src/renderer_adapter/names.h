// Names, version 2 of the frame / capture format (Session 101, arc A1; docs\renderer\NAMES.md). Declare, do not infer: every draw,
// surface and texture carries where it came from, named at its source. Frames without these sections (version 1) open as "unnamed".
// No game or SDK includes: the reader, the binder, rplay, fview, the case ring and the scene front-end all build it.
#pragma once
#include <cstdint>

namespace rdr_adapter {

inline constexpr uint32_t kNamesVersion = 3;  // FrameFileHeader::version for frames that may carry the sections below (3 = + kSecThin, S101 lane 122; older frames just lack it)

// ---- section ids (continue adapter_frame.h SectionId; never reuse a number) --------------------------------------------------------
enum NameSectionId : uint32_t {
  kSecDrawOrigins = 64,    // DrawOrigin[draw count], parallel to kSecDraws
  kSecSurfaceRoles = 65,   // SurfaceRoleEntry[]: images of this frame that have a declared role
  kSecProvenance = 66,     // TextureProvenance[]: images of this frame with a declared source
  kSecStrings = 67,        // char[]: NUL-separated UTF-8 names; a name field = byte offset + 1 (0 = no name)
  kSecWhere = 68,          // FrameWhere[1]: where the game was this frame
  kSecTextRuns = 69,       // S101 U1: TextRun[], one per glyph draw whose run string was read (capture / case ring only); sorted by draw
  kSecThinFacts = 71,      // S101 lane 122 (review 6): ThinDrawFacts[draw count] parallel to kSecDraws (or absent): the raw facts a thin key includes
  kSecD3 = 72,             // lane 138 (D3 skip the object, d3_arena.h): D3FrameCounts[1], the frame's marker / dropped counts (absent = the switch was off)
  kSecThin = 70,           // S101 lane 122 (D1 thin draws, SKIP_GAME_WORK_MAP.md 2.3): ThinFrameStamp[1], the pin-set generation the reader used for this frame (absent = no thin draws)
};

// ---- draw origin ------------------------------------------------------------------------------------------------------------------
// The class a draw belongs to. Promotion (RENDERER_ARC.md 3.2) goes by class.
enum class DrawClass : uint8_t {
  Unnamed = 0,   // no origin reached this draw: counted (the unnamed count is a check)
  ModelPart,     // a part of a 3D model (wrestler, arena, ring, prop): sub_82BDED40 [L: draw_submit.md]
  Crowd,         // the crowd's members and cards
  Sprite,        // a 2D UI sprite / rect
  Text,          // a glyph run
  Movie,         // a Bink plane draw (T_SpriteYUVAlpha) or a movie surface
  Particle,
  Shadow,        // a shadow-map pass draw
  Post,          // a post-chain step
  Clear,         // a clear drawn as geometry
  Other,         // named, but none of the above (the name says what)
  Count
};

inline const char* DrawClassName(DrawClass c) {
  static const char* const k[] = {"unnamed", "model_part", "crowd", "sprite", "text", "movie", "particle", "shadow", "post", "clear", "other"};
  return uint8_t(c) < uint8_t(DrawClass::Count) ? k[uint8_t(c)] : "?";
}

enum DrawOriginFlags : uint16_t {
  kOriginFromSubmit = 1,  // set by a hook at the game's own submit function (declared), not by our matching
  kOriginGuessed = 2,     // our inference (shape / hash table): allowed only with a comment at its source saying why it cannot be declared
};

// The raw facts come from the game (draw_origin.h); klass and technique come from the origin table (origin_table.h: our declared map of
// submit functions, from docs\research\s101\A0-3.md), so tools can re-classify an old capture by re-reading call_site / pass_site.
struct DrawOrigin {
  DrawClass klass = DrawClass::Unnamed;
  uint8_t op = 0;            // the record's opcode: 15 indexed, 19 inline vertices, 68 render-thread callback; 0 = no tag reached the draw
  uint16_t flags = 0;        // DrawOriginFlags
  uint32_t call_site = 0;    // guest address of the recorder call (the submit function's call; the function containing it is the submitter). Op 68 inside a post scope (lane PN): the post EFFECT's call into the queue helper
  uint32_t pass_site = 0;    // model parts: the call of sub_82BDED40 (its function is the scene pass: T_ZPass, T_Lambert, ...); else 0
  uint32_t object = 0;       // guest address of the engine object (model object); 0 = none
  uint32_t part = 0;         // part index within the object. Op 68 inside a post scope (lane PN): the generic queue site (0x82C1F48C / 514 / 674) the recorder was called from
  uint32_t technique = 0;    // string: the technique (T_Lambert, T_ZPass, T_Sprite, ...); 0 = not known
  uint32_t asset = 0;        // string: "pac:entry[:mesh]" the object was loaded from (host_write file ranges); 0 = not resolved
  uint32_t widget = 0;       // string, 2D classes: "screen seq=S menu=M[; element 0xE node 0xN text T]" (NAMES.md "2D draws"); 0 = none
};

// S101 lane 64 (U1, docs\renderer\UI_PROMOTION_MAP.md section 8): the whole string a glyph draw belongs to (the text draw sub_82BD06B8's r4),
// beside the 40-char label in the widget string. `string` is a name field (UTF-8: a code point above 0x7F is its UTF-8 bytes, so the icon codes
// U+E000.. survive); `units` is how many code points the run holds (a surrogate pair counts once; the NUL excluded). Present only for draws of class text, only while labels
// are on (a capture / case ring is active). A run longer than kRunUnits is cut and marked kRunTruncated, never silently.
inline constexpr uint32_t kRunUnits = 512;
enum TextRunFlags : uint32_t {
  kRunTruncated = 1,  // the string was cut at kRunUnits units (or the guest page ended)
};
struct TextRun {
  uint32_t draw = 0;    // index into kSecDraws
  uint32_t string = 0;  // string: the run, UTF-8
  uint32_t units = 0;   // code points, not UTF-16 units
  uint32_t flags = 0;   // TextRunFlags
};

// ---- surface roles ----------------------------------------------------------------------------------------------------------------
// What a render target IS. Scaling, lasting work and pass merging follow the role, never the size or aspect (the 16:9 rule and the
// "ever read" rule in reader.h are what this replaces).
enum class SurfaceRole : uint8_t {
  Unnamed = 0,
  SceneColor, SceneDepth,
  ShadowMap,        // index = cascade / light
  PostStep,         // index = step in the post chain; name says which (bloom, dof, ...)
  FeedbackMap,      // rebuilt each frame from itself (sweat / blood / damage): keeps the game's size, lasts across frames
  AtlasTarget,      // a texture-shaped target the game renders into once and samples later (attire atlas, crowd person pictures)
  MenuPicture,      // a picture the GPU makes for a menu (roster render, arena thumbnail, create preview)
  TronPicture,      // the titantron / stage screen picture
  Ui,               // the 2D layer
  Present,          // what goes to the screen
  Other,
  RimMap,           // S101 lane 86: the rim light map (T_RimLightMap, 512x256) and its chain; NOT a shadow map (it was tagged shadow_map before)
  Count
};

inline const char* SurfaceRoleName(SurfaceRole r) {
  static const char* const k[] = {"unnamed", "scene_color", "scene_depth", "shadow_map", "post_step", "feedback_map", "atlas_target",
                                  "menu_picture", "tron_picture", "ui", "present", "other", "rim_map"};
  return uint8_t(r) < uint8_t(SurfaceRole::Count) ? k[uint8_t(r)] : "?";
}

enum SurfaceRoleFlags : uint8_t {
  kRoleScales = 1,    // follows the render scale (screen images only)
  kRoleLasting = 2,   // its contents are read in a later frame: never dropped by a skipped frame
  kRoleDeclared = 4,  // named where the game creates it (else kRoleGuessed must be set)
  kRoleGuessed = 8,
};

struct SurfaceRoleEntry {
  uint32_t image = 0;        // ImageRecord id
  SurfaceRole role = SurfaceRole::Unnamed;
  uint8_t index = 0;
  uint8_t flags = 0;         // SurfaceRoleFlags
  uint8_t pad = 0;
  uint32_t name = 0;         // string: the game's own name for it where it has one
  uint32_t creator_fn = 0;   // guest address of the function that created / chose it
};

// ---- texture provenance -----------------------------------------------------------------------------------------------------------
// Who owns the newest bytes of a texture: the coherence model, declared per texture.
enum class Provenance : uint8_t {
  Unnamed = 0,
  File,   // loaded from the game's files and never written after: pac + entry + name. A capture may store it as a reference only
  Gpu,    // written by the GPU: a copy / resolve of a surface with a role (source_image)
  Cpu,    // written by a named CPU writer (the movie decoder, a lock / unlock, the attire pack): re-read when that writer runs
  Count
};

inline const char* ProvenanceName(Provenance p) {
  static const char* const k[] = {"unnamed", "file", "gpu", "cpu"};
  return uint8_t(p) < uint8_t(Provenance::Count) ? k[uint8_t(p)] : "?";
}

enum ProvenanceFlags : uint8_t {
  kProvContentByReference = 1,  // this frame / capture holds no bytes for it: load `file` from the player's own game files
  kProvDeclared = 2,
  kProvGuessed = 4,
};

struct TextureProvenance {
  uint32_t image = 0;        // ImageRecord id
  Provenance source = Provenance::Unnamed;
  uint8_t flags = 0;         // ProvenanceFlags
  uint16_t pad = 0;
  uint32_t file = 0;         // string, File: "pac\\path.pac:entry" (relative to game_data\extracted)
  uint32_t file_name = 0;    // string, File: the texture's own name in the pac
  uint32_t source_image = 0; // Gpu: the surface image it was copied from
  uint32_t writer = 0;       // string, Cpu: the writer's name ("movie_decoder", "attire_pack", "lock_unlock:<fn>")
  uint32_t writer_fn = 0;    // Cpu: guest address of the writer
  uint64_t last_write_frame = 0;  // guest frame of the newest write by its owner
};

// ---- where the game was -----------------------------------------------------------------------------------------------------------
struct FrameWhere {
  uint32_t address = 0;      // string: the go-anywhere address (RENDERER_ARC.md 4A), 0 = not known
  uint32_t words = 0;        // string: the same in words
  uint32_t menu_id = 0xFFFFFFFFu;
  uint32_t camera_kind = 0;  // string
  uint64_t match_clock = 0;  // frames, 0 = not in a match
};

// S101 lane 122 (D1 thin draws): what the reader tells the binder about a frame. `generation` is the pin set the reader took its lite decisions from (0 = none);
// the binder releases a key's template only once a frame it binds carries a newer generation without it (thin_draw.h). A draw the reader did not build the rest of carries
// Draw::flags kDrawThin (adapter_frame.h); its geometry offsets / bytes, counts and layout are blank (0 from the live reader; fview's emulation writes thin::kBlankHash / kBlankCount / 0xFFFFFFFF). The shader hashes stay: the program is part of the key.
// The identity facts a thin key includes besides (site, object, part, ordinal, program): free to the reader (the draw call's own arguments and the texture slots it already walked),
// carried for every draw of a frame part so BOTH sides hash the same numbers; a change in any of them makes a new key, which takes the full path at once.
// Live: the raw guest values. fview's emulation of an old capture: index_addr = the draw's index_offset, index_count = Draw::index_count, base_vertex = start_index = 0.
struct ThinDrawFacts {
  uint64_t index_addr = 0;
  uint32_t index_count = 0, base_vertex = 0, start_index = 0;
  uint32_t img0 = 0, img4 = 0, img5 = 0;  // image ids bound in texture slots 0 / 4 / 5 (0 = none)
  // lane 129 review F4: the vertex identity. Live: the draw's vertex fetch constant dwords 0 / 1 (guest address + type bits, size + endian; 0 for inline vertices). fview's emulation of an old capture: the
  // draw's vertex_offset (a content key: it changes when the bytes change) folded to 32 bits, and vertex_bytes. A stream VB re-allocated or swapped under the same IB and images is a new key.
  uint32_t vb_addr = 0, vb_dw1 = 0;
};

// lane 138 (D3 skip the object, SKIP_GAME_WORK_MAP.md 8.3 item 2): the per-frame header counts. `markers` = objects the game did not build (a 0-primitive marker record stands in), `dropped` = objects the
// hook could have skipped and recorded in full (reasons in `why`, d3_arena.h Why order), `generation` = the mark set the hook read, `frame` = the guest frame.
struct D3FrameCounts {
  uint32_t markers = 0, dropped = 0;
  uint32_t generation = 0, flags = 0;   // flags: reserved
  uint64_t frame = 0;
  uint32_t why[16] = {};
};

struct ThinFrameStamp {
  uint32_t generation = 0;
  uint32_t flags = 0;        // reserved
  uint64_t frame = 0;        // the guest frame the proof rule (key hash % K == frame % K) used
  uint32_t lite_draws = 0;   // how many draws of this frame carry kDrawThin (a cross-check for the binder's tally)
  uint32_t pad = 0;
};

}  // namespace rdr_adapter
