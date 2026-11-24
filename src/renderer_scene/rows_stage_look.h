// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {


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

}  // namespace rdr_scene
