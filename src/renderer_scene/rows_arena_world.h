// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {

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
inline constexpr uint8_t kAwLast = kAwClock;   // S111 FXSKIP: the last known kind; record_io.cpp keeps kAwObject..kAwLast and skips (and counts) a newer kind's rows. A new kind: move it.
enum ArenaWorldObjFlags : uint32_t { kAwForeignPalette = 1,  // r5 was not [model+200] (another caller's array, e.g. sub_82C13EC8): still what the packer read
                                     kAwNodeAssumed = 2 };   // the part descriptor was not readable: node 1 assumed (counted node_assumed)
static_assert(sizeof(ArenaWorld) == 68, "ArenaWorld is a flat POD table");

}  // namespace rdr_scene
