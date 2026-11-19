// The ring's key rules as pure functions (S103 lane CRR; moved out of rf_ring.cpp so the offline unit tools\scene\crr_unit.cpp runs THESE, not a copy): which pac key's model a ring entry is an instance of, which group
// a class belongs to, how many calls make a group whole, the keys a group owns, and the game's draw order. Header-only; includes only the record layout and the ring key table.
//
// NORMAL ring (arena ids 1..199): keys 951-982 (rope 960 / 980, cover 964 / 982, turnbuckle 952, post 951 / 981, skirt 973 / 974, canvas 953).
// ALT ring (a created arena, ids 200..249; docs\research\s103\CRK.md sections 1-2, sub_824EB648 / sub_825CCFE0 / sub_825CE668 / sub_825CF2F8): the game loads a FIXED alt key set by the arena id alone [C]:
//   rope 1021 / 1041, cover 1070 / 1074 (corner 3), 1071 / 1075 (corner 0), 1072 / 1076 (corner 1), 1073 / 1077 (corner 2), turnbuckle 952 (shared), post 1050 / 1052, loop 2 posts 1051 / 1053 + logos 1080 / 1082 and 1081 / 1083,
//   skirts 1064 (973's) / 1065 (974's), canvas 1017.
//   ENTRIES. OURS (kRrRecomputed): covers keep the normal rule (call = 3 * corner + height, the corner picks the model: call / 3); the posts are 8 calls (CRK section 2): 0, 1 = loop 1 (1050, corners 1, 2), 2, 3 = loop 2 h 0 (1051,
//   corners 0, 3), 4, 5 = h 1 (1080, corners 1, 2), 6, 7 = h 2 (1081, corners 0, 3). THE GAME'S rows (ring_rows_core.h kClassTable): the base slots G+20.. take the class's alt key (cover 1070, post 1050, ...); the slots G+68..+112
//   carry their key in RingRows::pad2 (a cover / post class row of a 1017..1083 pad2 names its key outright).
#pragma once
#include <cstdint>
#include <vector>

#include "../assets/ring_low_keys.h"
#include "scene_record.h"

namespace rdr_scene::rf::c::ringkeys {

inline bool IsAltArena(int arena) { return arena >= 200 && arena <= 249; }   // the same test as stage_slot_vis.h VisArena200to249 / sub_822A7240
inline bool IsExplicitKey(uint16_t v) { return assets::IsAltRingKey(int(v)); }

inline constexpr int kAltCoverHi[4] = {1071, 1072, 1073, 1070};   // by corner 0..3 (sub_82740550 for 0..2, sub_827403E8 for 3)
inline constexpr int kAltCoverLo[4] = {1075, 1076, 1077, 1074};
inline constexpr int kAltPostHi[8] = {1050, 1050, 1051, 1051, 1080, 1080, 1081, 1081};   // by OUR post call 0..7
inline constexpr int kAltPostLo[8] = {1052, 1052, 1053, 1053, 1082, 1082, 1083, 1083};
// The angle index of our alt post call (RotY(index * pi/2); sub_825CCFE0 loop 1: corners {1, 2}; loop 2 h 0 / 2: {0, 3} = {0, -1}, h 1: {1, 2}); the one rule shared with the recompute (ring_recompute_core.h).
inline constexpr int kAltPostAngle[8] = {1, 2, 0, -1, 1, 2, 0, -1};
inline constexpr uint32_t kAltPostCalls = 8;

// Which key's model a ring entry is an instance of (0 = none). `ours` = the record's entries are the recompute's (the rope LOD stand-in, the skirt index in pad2); `alt` = a created arena (ids 200..249).
inline int KeyOf(const RingRows& e, bool ours, bool alt) {
  if (!alt) {
    switch (e.klass) {
      case kRcRopeHi: return 960;   // S103 RLB2: 980 is loaded now (the live load), so a HIGH rope draws 960 for ours and the game's rows alike; the low class is kRcRopeLo (ours carries it since RLB)
      case kRcRopeLo: return 980;
      case kRcCoverHi: return 964;
      case kRcCoverLo: return 982;
      case kRcTurnbuckle: return 952;
      case kRcPostHi: return 951;
      case kRcPostLo: return 981;
      case kRcSkirt: return ours ? ((e.pad2 & 1u) == 0 ? 974 : 973) : 0;   // G[332 .. 344] = 974, 973, 974, 973 (ring.md section 1 table)
      default: return 0;
    }
  }
  switch (e.klass) {
    case kRcRopeHi: return 1021;
    case kRcRopeLo: return 1041;
    case kRcCoverHi: case kRcCoverLo: {
      const bool hi = e.klass == kRcCoverHi;
      if (!ours && IsExplicitKey(e.pad2)) return int(e.pad2);   // the game's G+68..+88 slots
      if (ours) return (hi ? kAltCoverHi : kAltCoverLo)[(e.call / 3) & 3u];   // ours: call = 3 * corner + height
      return hi ? 1070 : 1074;                                              // the game's base slots G+36 / G+40
    }
    case kRcTurnbuckle: return 952;
    case kRcPostHi: case kRcPostLo: {
      const bool hi = e.klass == kRcPostHi;
      if (!ours && IsExplicitKey(e.pad2)) return int(e.pad2);   // the game's G+92..+112 slots
      if (ours) return (hi ? kAltPostHi : kAltPostLo)[e.call & 7u];
      return hi ? 1050 : 1052;                                  // the game's base slots G+48 / G+52
    }
    case kRcSkirt: return ours ? ((e.pad2 & 1u) == 0 ? 1065 : 1064) : 0;   // G[332 .. 344] = 1065, 1064, 1065, 1064
    default: return 0;
  }
}

inline int GroupOf(const RingRows& e) {
  switch (e.klass) {
    case kRcRopeHi: case kRcRopeLo: return 0;
    case kRcCoverHi: case kRcCoverLo: return 1;
    case kRcTurnbuckle: return 2;
    case kRcPostHi: case kRcPostLo: return 3;
    case kRcSkirt: return 4;
    default: return -1;
  }
}
// The calls that make a group whole (rope, cover, turnbuckle, post, skirt): the alt ring draws its posts 2 + 6 times (CRK section 2), not 4.
inline uint32_t Need(int g, bool alt) {
  static constexpr uint32_t k[5] = {12, 12, 12, 4, 3};
  return g == 3 && alt ? kAltPostCalls : k[g];
}
// The keys a group owns (each high key and its low twin): the at-rest models of a complete group are not drawn.
inline std::vector<int> GroupKeys(int g, bool alt) {
  if (!alt) {
    switch (g) {
      case 0: return {960, 980};
      case 1: return {964, 982};
      case 2: return {952};
      case 3: return {951, 981};
      default: return {973, 974};
    }
  }
  switch (g) {
    case 0: return {1021, 1041};
    case 1: return {1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077};
    case 2: return {952};
    case 3: return {1050, 1051, 1052, 1053, 1080, 1081, 1082, 1083};
    default: return {1064, 1065};
  }
}

// S103 FOB: the game draws the ring in TWO runs (XFO.md G1 / G4): the canvas and the apron skirts BEFORE the arena's slots (run 0), then the posts, turnbuckles, covers and ropes (run 1).
struct RunRank {
  int run;           // 0 = early (before the arena), 1 = late (after the screens)
  int rank;          // inside the run: the game's order
  const char* tok;   // the order-log token (nullptr = not a family piece the game's list names: the rope shadow proxy 956, unknown keys)
};
inline RunRank RankOfKey(int key) {
  switch (key) {
    case 953: case 1017: return {0, 0, "canvas"};
    case 973: case 974: case 1064: case 1065: return {0, 1, "skirt"};
    case 951: case 981: case 1050: case 1051: case 1052: case 1053: case 1080: case 1081: case 1082: case 1083: return {1, 0, "post"};
    case 952: return {1, 1, "turnbuckle"};
    case 964: case 982: case 1070: case 1071: case 1072: case 1073: case 1074: case 1075: case 1076: case 1077: return {1, 2, "cover"};
    case 960: case 980: case 1021: case 1041: return {1, 3, "rope"};
    default: return {1, 4, nullptr};
  }
}

}  // namespace rdr_scene::rf::c::ringkeys
