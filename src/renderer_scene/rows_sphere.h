// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {

// lane 242 (SKIP_GAME_WORK_MAP.md 18.8, H3): the wrestlers' sphere occluders. The formula is settled offline (tools\skip\sphere_check.py: 7,854 draws a capture, centre within 1.1e-4, axis 6e-7, strength 1.4e-6)
// but three of its inputs are in no record: the character's sphere TABLE (bone roles -> nodes, offsets, angles, radius, scale, blend: loaded from the character's data), the move's CONTACT record
// (sub_8265B470; it blends the centre toward a point of another bone) and the WORLD node matrices (the record holds the skinning matrices). So the recorder takes the RESULT: at the exit of
// sub_826DF8E0 ('CCSO' update, this = the component) it copies the component's own output arrays [this+48] (P, 16 bytes a sphere) and [this+52] (V) = exactly what sub_82BCC888 stores and
// sub_82BE7AC0 uploads to VS c168-c171 (P0 = c168, P1 = c169, V0 = c170, V1 = c171), the 52-byte table rows [this+44] for the census, and the skeleton [[this+32]+48] (the record's engine object).
inline constexpr uint32_t kSphereOccLayout = 1;
inline constexpr uint32_t kSphereMax = 2;           // the component's count is 2 (sub_826E0030); a larger count is refused
inline constexpr uint32_t kSphereRecWords = 13;     // 52 bytes a table row
enum SphereOccFlags : uint32_t {
  kSoSpheres = 1,    // P / V were read from the component's output arrays
  kSoTable = 2,      // table[] was read
  kSoSkeleton = 4,   // object = [[this+32]+48] was read (else 0)
  kSoSphere0Ran = 8, // the game computed sphere 0 in this call (its table row has both nodes >= 0); else p[0] / v[0] are the previous call's, not this frame's
  kSoSphere1Ran = 16,
};
struct SphereOccRow {
  uint32_t layout = kSphereOccLayout;
  uint32_t flags = 0;                          // SphereOccFlags
  uint32_t object = 0;                         // the skeleton's guest address = the record's engine object (0 = unreadable)
  uint32_t frame = 0;                          // the hook's guest frame (low 32 bits), never the record's
  uint32_t count = 0;                          // spheres read (<= kSphereMax)
  uint32_t component = 0;                      // `this` of sub_826DF8E0 (the 'CCSO' component), for the census
  uint32_t character = 0;                      // [this+32]
  uint32_t calls = 0;                          // sub_826DF8E0 calls of this skeleton in this hook frame (the row is the LAST one)
  float p[kSphereMax][4] = {};                 // sphere centre xyz + radius (VS c168, c169), host floats
  float v[kSphereMax][4] = {};                 // axis xyz + strength (VS c170, c171)
  uint32_t table[kSphereMax][kSphereRecWords] = {};   // the 52-byte rows as guest words (big-endian converted to host order; floats are bit patterns)
};
static_assert(sizeof(SphereOccRow) == 32 + 64 + 104, "SphereOccRow is a flat POD table");

}  // namespace rdr_scene
