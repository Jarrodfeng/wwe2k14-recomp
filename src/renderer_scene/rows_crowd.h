// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {


// S103 lane D4 (46 / 47; lane D's range 46-48, docs\renderer\CROWD_PROPS_LATCH.md "Lane D4"): what no other table carries of the crowd. 46 kScCrowdProps = the signs and foam hands (the two per-member
// loops sub_82518950 / sub_82518EB8: pass 1 the 32 bone-held props, pass 2 the 96 free ones), one row per prop the game would draw; 47 kScCrowdSingles = the single standing people
// (sub_8276E7B0: skinned bodies placed one at a time). Both optional (a record without them is byte-identical to today's); an unknown layout drops THAT section only, an unknown kind that row only (c_errors; S111 FXSKIP). Written by a
// latch reader through src\renderer_scene\crowd_prop_rows.h (the append API checks every row), drawn by LiveCrowd (live_crowd.cpp) from the record alone.
inline constexpr uint32_t kScCrowdProps = 46;
inline constexpr uint32_t kScCrowdSingles = 47;
inline constexpr uint8_t kCrowdPropLayout = 2, kCrowdSingleLayout = 1;   // S103 CSF: CrowdPropRow layout 2 = 68 bytes (file_key); a layout-1 record's section fails closed (size and layout checks drop it, counted)
// S103 SG (Q2 / V2): kind = WHICH LOOP, by act code: 1 = a sign (sub_82518950, act 14006 / 14007, draw at 0x82518D60), 2 = a foam hand (sub_82518EB8, act 14127 / 14128, draw at 0x825192D0).
// Not a technique: BOTH loops pick T_Audience_LightMap iff the phase word [0x836F3940] == 4, else T_Audience (0x82518A20 / 0x82518EF0), the same choice as the stands in the same frame.
enum CrowdPropKind : uint8_t { kCpSign = 1,   // a sign (one mesh, key 40000)
                               kCpHand = 2 };  // a foam hand (mesh 40999 + h per hand type b: model_key)
inline constexpr uint8_t kCpLast = kCpHand;   // S111 FXSKIP: the last known kind; record_io.cpp keeps kCpSign..kCpLast and skips (and counts) a newer kind's rows. A new kind: move it.
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
enum CrowdSingleKind : uint8_t { kCsBody = 1 };   // a body (the only kind so far)
inline constexpr uint8_t kCsLast = kCsBody;   // S111 FXSKIP review: the last known kind; record_io.cpp keeps kCsBody..kCsLast and skips (and counts) a newer kind's rows. A new kind: move it (tools\lint\kind_last_lint.py fails if not).
struct CrowdSingleRow {
  uint8_t kind = kCsBody;  // CrowdSingleKind
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

}  // namespace rdr_scene
