// Route 2's source reader (Session 103, lane F; docs\formats\particle_items.md): reads the game's live particle entries / items through a guest-memory
// interface and expands the billboards ITSELF against OUR camera, into a ParticleSnapshot (particle_contrib.h). No stream value, no recorder call.
// Pure: no game or SDK includes. The game build supplies GuestMem (big-endian reads); the offline test supplies a synthetic memory.
//
// Evidence [C] = PC run 8, probe "particle items" (wwe2k14_077.log, Brock's entrance, 849 lines) or a named listing address; [L] = reasoned from a listing;
// [U] = not settled. Lane F3 (listings of sub_82A0D0E0 / sub_82A0DAC8 / sub_82A0AED8 / sub_82A0C758 / sub_82756710 / sub_82756780, image constants read with
// tools\analysis\image_words.py) added: the quad class, the 3x3 pivot grid, the handle tables, the texture / blend / sampler / ZOffset reads. What is NOT confirmed
// is named at kBlendCodesConfirmed and TextureNamer below.
#pragma once
#include <cstdint>
#include <string>

#include "particle_contrib.h"

namespace rdr_scene::live::particles {

// CONTRACT: Read32 returns the big-endian word at `guest` in host order, and false for any address range that is not readable. The reader itself refuses a 4-byte range that wraps mod 2^32.
// WIRING: the seam must pass the GAME-space view matrix to BasisFromView (never the viewer-space S * view).
struct GuestMem {
  virtual ~GuestMem() = default;
  virtual bool Read32(uint32_t guest, uint32_t& out) const = 0;  // false = unreadable
};

// The camera's axes in the game's space (row-vector convention: p' = p * V, so right = column 0 of V's 3x3, up = column 1, forward = column 2).
struct CameraBasis {
  float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};
};
CameraBasis BasisFromView(const float view[16]);

// The file a particle texture handle names (pac + entry, as an AssetRef would). The slot-33 table entry is a HANDLE {id, cached pointer} (the op-43 record's two words:
// docs\formats\particle_items.md); which prtcls.pac sprite-directory texture it is can only be found by content (tools\particles\tex_owner.py hashed mip 0 of the captured image
// against every DDS in every pac), so the implementation is the game side's and is NOT written in this lane: a null namer, or a false return, leaves the key without a texture
// (counted tex_unresolved, kPiTexture, the white stand-in). [U]
// CONTRACT: `cached_ptr` is INFORMATIONAL only. Its writer is unknown (the same class as the +152 / +160 caches: it may be filled by a replay step that does not run under OURS),
// so the namer must resolve by `handle_id` and never depend on `cached_ptr` being valid or non-zero.
struct TextureNamer {
  virtual ~TextureNamer() = default;
  virtual bool Name(uint32_t handle_id, uint32_t cached_ptr, std::string& pac, std::string& entry) const = 0;
};

inline constexpr uint32_t kSpriteItemVtable = 0x82057280u;   // [C] the camera-facing sprite item (slot 3 = sub_82A0D0E0, image word 0x8205728C)
inline constexpr uint32_t kQuadItemVtable = 0x820572A0u;     // [C] the oriented quad item (slot 3 = sub_82A0DAC8, image word 0x820572AC); its own matrix per key
// S103 PRT (PNS.md): kMaxEntries = 255 is the game's own array bound (side arrays are 255 bytes, ctor 0x82254B00 +10C; the largest bank in prtcls.pac has 243) [C]; kMaxKeysPerItem 512: bank 4119 effect 0 has 300 particles [C file].
inline constexpr uint32_t kMaxEntries = 255, kMaxItemsPerEntry = 64, kMaxKeysPerItem = 512;  // sanity caps (counted when hit)
inline constexpr uint32_t kThirdItemVtable = 0x8205725Cu;   // S103 PRT [L basis, PNS.md 3.5]: the third class (mode 1, draw sub_82A0E3D8, bank 0104): read as a sprite with roll 0 (cs = 1, sn = 0)
inline constexpr uint32_t kMaxTextureIndex = 4096;           // sanity bound on the slot-33 table index (the table's length is not read: [U])
inline constexpr float kHalfExtentPerSize = 10.f;            // [C: sub_82A0C758: corners (pivot +-1.0) * 10.0, constants 1.0 @0x8218B8C0 and 10.0 @0x82192140; the log agrees: size 20 -> 200]
inline constexpr float kZOffsetPerInt = 0.001f;              // [C: 0x82A0D380 / 0x82A0DD5C: c200.x = (int32 at *[[o48 + 28 * idx + 108]]) * the float 0.001 @0x82033C7C]

// The handle tables (sub_82756710 for the +156 handle, sub_82756780 for +148): a global holds a manager; entries are 8 bytes {id, pointer} from [mgr+8] to [mgr+16];
// the handle's low 16 bits index them and the full id must match. [C: the listings; the log's ids 0x000F0007 / 0x0059002C fit]. The game's draw refreshes the cache words
// (+152 / +160) itself; under OURS the draw does not run, so the reader does the lookup (never trusts the cache).
inline constexpr uint32_t kHandleTableGlobal156 = 0x83929A98u, kHandleTableGlobal148 = 0x8392E994u;

// The item's blend code (*(*(item+48)) + 196, passed to sub_82BB7630 only for 0 / 1 / 2): 0 = alpha, 1 = additive, 2 = reverse-subtract, all [C] from the setter (S103 PF / F5, the
// line below). Only a code above 2 is reported (kPiBlend) and holds the pass.
inline constexpr bool kBlendCodesConfirmed = true;  // S103 PF: codes 0 / 1 / 2 are all proven (2 = reverse-subtract [C]: R8 -> op-3 handler -> setter sub_82CA90E0 rlwimi into RB_BLENDCONTROL bits 5-7, value 4 = kRevSubtract); the pass still reports kPiBlend for any code above 2
inline uint8_t BlendOfCode(uint32_t code) { return code == 1 ? kParticleAdditive : code == 2 ? kParticleReverseSubtract : kParticleAlpha; }   // S103 F5: from the setter itself (sub_82C23168), see BlendCodeProven
// S103 F4 (run 22, wwe2k14_080.log, Bryan's entrance): 16 of 16 sampled sprite items (4 frames x 4 items, the same descriptor shape) read *(*(item+48))+196 == 1, one texture handle
// (091903BD). The stream of the same route (out/s102_t13/bryan_cut.zip: 79 of 237 frames scanned every 3rd, 2,084 particle draws) holds ONE particle blend state, blend[0] = {enable 1,
// src_color 6 (SRC_ALPHA), dst_color 1 (ONE), op 0, src_alpha 6, dst_alpha 1, op 0, mask 15} with image 1247 on every draw. So code 1 = additive [C] FOR THIS EMITTER (Bryan's entrance pyro). The pairing is by
// GEOMETRY (Opus review): the probe's 16 items are 4 items x 4 frames (the probe caps the first 4 calls per function) of a different run than bryan_cut, but every particle draw in bryan_cut (40 frames
// checked, read-only) is one of exactly four first-vertex shapes, (-200,-200,0), (-40,-40,0), (268.7,-297.0,0), (-297.0,-268.7,0), 260 draws each, vertex colour 0xFEFEFE: the probed items' sizes
// (20x20, 4x4, 2x40 at +-0.785398). Codes 0 / 2 / above 2 were never read in a log (F5: 0 = alpha is now [C] from the writer, below): 0 = alpha was [L] (the rest state each method ends with; the menu and Kane's smoke are SrcAlpha / InvSrcAlpha), 2 is [U];
// also open: whether the code is the technique's own pass state.
// S103 F5 (S103_FABLE_REVIEW 1.6: ask who WRITES a value): the code is DATA, not a run-time state. The emitter loader sub_82A10760 reads the 48-byte header of each PRTC 'rootchk' block into
// the emitter at +192 (sub_82A10EE8) and byte-swaps it in place (sub_82A0C470): emitter+192 = block+8 (MODE), **emitter+196 = block+12 = the blend code** (the draw reads *(*(item+48))+196),
// +200 = block+16 (the 16-byte name, "Light" in the run-22 dump), +216 / +218 = block+32 / +34 (max / command count) [C: listing 0x82A107A0-0x82A107C4 + 0x82A0C470-0x82A0C5C0 + the
// lhz 216 / 218 reads of the same function]. tools\scene\f5_blend_codes.py reads that u32 for all 78,523 emitters of the player's prtcls.pac: ONLY 0 / 1 / 2 occur (3,555 / 74,894 / 74),
// and the file says what each family is: Light / Fire / FireW / Bo / Hanabi emitters hold 1; Smoke / Glass / Solt / Hahen hold 0 (Smoke 1,427 of 2,129); 2 only on the 72 "SmokeB" / "FireF-B"
// emitters and one Ring ("Hosya"). Kane's bank 0107: Smoke_UP / Smoke_UP_Dark / Smoke (the Smoke0002 sprites the run-071 capture drew with SrcAlpha / InvSrcAlpha, prtcls_pac.md "Drawing")
// = 0, Fire01 / Fire02 / Light = 1. So: **1 = additive [C]** (Bryan: 16 of 16 items, geometry-matched to the stream's one additive state; the "Light" emitter is 1 in the file), **0 = alpha [C]**
// (the writer's value on the emitters whose draws the stream shows as SrcAlpha / InvSrcAlpha, Kane's smoke: the join is by emitter, not by frame), **2 = [U]** (no draw joined; 72 emitters; the
// device setter sub_82BB7630 vtable slot 4 was not decoded: drawn as alpha and reported kPiBlend).
// The setter's own table (Fable review 2 Q2 lead, decoded offline): the thread's +1036 object is the command RECORDER (built on the stack by sub_82C13A80 via sub_82C244A0, whose vtable word is
// lis -32249 / addi -14640 = 0x8206C6D0 [C listing 0x82C244B8-0x82C244C4]); slot 4 = 0x82C23168 [C image word 0x8206C6E0]. It maps the code to three state writes (sub_82C28798(buf, reg, value),
// regs 80 / 72 / 76): code 0 = (80:0, 72:6, 76:7), 1 = (0, 6, 1), 2 = (4, 6, 1) [C listing 0x82C231B0-0x82C23390]. With 72 = source factor 6 = SRC_ALPHA and 76 = dest factor (7 = INV_SRC_ALPHA, 1 = ONE; the
// stream's two known states fix this: code 0 alpha, code 1 additive [C]), **code 2 = SRC_ALPHA / ONE with reg 80 = 4**: reg 80 is the blend OP by elimination (0 for 0 and 1), 4 = reverse-subtract
// in the Xenos encoding [L]. So code 2 is a subtractive blend [L, not paired with a draw]: still reported kPiBlend.
// S103 PF: code 2 is proven too [C: probe claims_S1G1.jsonl G1 rows: R8 -> the op-3 handler -> setter sub_82CA90E0 rlwimi into RB_BLENDCONTROL bits 5-7; value 4 = kRevSubtract]. Only codes above 2 stay unproven (none occur in the files).
inline bool BlendCodeProven(uint32_t code) { return code <= 2; }

// The 3x3 pivot anchor grid [C: sub_82A0C758 0x82A0CB44-0x82A0CBC4, constants 1.0 @0x8218B8C0, 0.0 @0x8218B8CC, -1.0 @0x82004734 read from the image]: the code is the halfword at
// *(*(item+48)) + 226; the corners are ((cx -+ 1) * 10, (cy -+ 1) * 10). Codes 0..8 = (+1,-1) (0,-1) (-1,-1) (+1,0) (0,0) (-1,0) (+1,+1) (0,+1) (-1,+1); anything above 8 = (0,0).
struct PivotShift { float x, y; };
PivotShift PivotOfCode(uint32_t code);

// The byte at the item's 28-byte record pointer is handed to sub_82BB7B20 (a sampler vcall, slot 28, sampler 0) as {0 -> 2, 1 -> 5, 2 -> 6, 3 -> 1}; above 3 the game makes no call.
// Which sampler state slot 28 sets is [U] (nothing shown; the values 2 / 5 / 6 / 1 are only consistent with a wrap/address code, not proven).
// Returns 0 for a byte the game ignores.
inline uint8_t SamplerValueOfByte(uint32_t b) { return b == 0 ? 2 : b == 1 ? 5 : b == 2 ? 6 : b == 3 ? 1 : 0; }

// Test only: 1 = the sprite half-extent is the size itself (the scale fault), 2 = the entry gate is ignored, 3 = the pivot grid is ignored (every item drawn centred),
// 4 = a quad is placed with the camera's axes instead of its own matrix. S103 PRT: 6 = the old 64-entry cap, 7 = a flat soft sprite is not drawn hard, 8 = the game's entry list is ignored.
// S103 F4: 5 = every blend code counts as proven (the blend-proof fault). S111 PARTZ: 9 = the entry's +220 word is ignored (every entry depth tested).
inline int g_reader_plant = 0;

struct ReaderStats {
  uint64_t entries = 0, items = 0, keys = 0;
  uint64_t entries_gated = 0;      // the entry's own draw gate said no (the game would not draw it)
  uint64_t items_other_class = 0;  // an item whose vtable is neither the sprite nor the quad one (the third method 0x82A0E3D8): not read, [U]
  uint64_t unreadable = 0, over_cap = 0;
  uint64_t over_entries = 0, over_items = 0, over_keys = 0, over_trail = 0, items_third = 0;   // S103 PRT: over_cap split by site (each also adds to over_cap); the third-class items read
  uint64_t items_skipped = 0, items_unresolved = 0, entries_no_items = 0, node_idle = 0;  // the game's own early outs (slot 15 null, a handle that does not resolve, null item array, node idle)
  uint64_t non_finite = 0, alpha_zero = 0, pivot_not_centred = 0;                        // keys skipped (non-finite / huge, alpha 0); items whose pivot code is not a centred one (decoded: shifted, no longer approximate)
  uint64_t items_quad = 0, tex_named = 0, tex_unresolved = 0, blend_read = 0, blend_other = 0, sampler_read = 0, zoffset_read = 0;  // F3
  uint64_t blend_unproven = 0;  // S103 F4: items whose blend code is not BlendCodeProven
  // S103 PRT fix 5 (sub_82254E30 +2A4 / +2B0, [C]): the game draws a state-2 entry only when its address is in the list [[0x83929A84] + 24] (count [... + 36]). INPUT: a SORTED list of entry addresses (the source reads it once per frame); list_n == 0 = no filter (unreadable / empty list: fail open).
  const uint32_t* list = nullptr;
  uint32_t list_n = 0;
  uint64_t entry_not_listed = 0, soft_flat_hard = 0;   // entries skipped for not being in the list; soft particles drawn hard because the sprite lies flat (the game's flat rule)
  uint8_t soft = 0;             // S103 PRT: INPUT, set by the source before each ReadNode: the soft-switch code of the node's call (SoftCodeOf, particle_source.h); every batch key of that node carries it
};

// Reads one entry (guest address of a 432-byte particle entry) and appends its sprites / quads. `namer` may be null (textures stay unresolved, counted).
void ReadEntry(const GuestMem& mem, uint32_t entry, const CameraBasis& cam, ParticleSnapshot& out, ReaderStats& st, const TextureNamer* namer = nullptr);

// Reads the particle node: entries at *(node + 16) + 432 * i for the i < *(node + 3088) whose state byte (node + 20 + i) is 2.
// `snapshot.guest_frame` must be set by the caller (the hook clock).
void ReadNode(const GuestMem& mem, uint32_t node, const CameraBasis& cam, ParticleSnapshot& out, ReaderStats& st, const TextureNamer* namer = nullptr);

}  // namespace rdr_scene::live::particles
