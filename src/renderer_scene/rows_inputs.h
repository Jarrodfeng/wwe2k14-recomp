// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>
#include "rows_lights.h"

namespace rdr_scene {


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

}  // namespace rdr_scene
