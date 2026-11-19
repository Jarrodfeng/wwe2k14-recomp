// S103 RLB2 (review item 7): the game's low-detail ring pieces and the high twin each stands in for (docs\systems\ring.md B2.2). One list for the loader (asset_load.h), the posed ring (rf_ring.cpp,
// rf_c_core.cpp) and the rig. Header-only, no dependencies.
#pragma once

namespace assets {

struct RingLowKey {
  int low;   // the low-detail piece's pac entry
  int high;  // the high-detail model the same call draws when the LOD test says near
};
// S103 CRR [C, docs\research\s103\CRK.md section 1]: a created arena (ids 200-249) draws its ring from the ALT keys sub_824EB648 loads for it: canvas 1017, ropes 1021 / 1041, covers 1070-1077 (one model per corner, near / far),
// posts 1050-1053 and the post logos 1080-1083, skirts 1064 / 1065. 952 (turnbuckle), 956 and 980 are shared with the normal ring.
inline constexpr RingLowKey kRingLowKeys[] = {{980, 960}, {981, 951}, {982, 964},   // rope, post, cover (the first three: rf_ring.cpp indexes them)
                                              {1041, 1021}, {1052, 1050}, {1074, 1070}, {1075, 1071}, {1076, 1072}, {1077, 1073}, {1053, 1051}, {1082, 1080}, {1083, 1081}};   // the alt twins (CRK section 3 row 6)
// The alt ring's HIGH keys (loaded with the normal ones for a created arena) and the test for any alt key.
inline constexpr int kAltRingHighKeys[] = {1017, 1021, 1050, 1051, 1064, 1065, 1070, 1071, 1072, 1073, 1080, 1081};
inline constexpr bool IsAltRingKey(int key) { return key >= 1017 && key <= 1083; }

inline constexpr bool IsRingLowKey(int key) {
  for (const RingLowKey& k : kRingLowKeys)
    if (k.low == key) return true;
  return false;
}
// The high twin of a low key; 0 for any other key.
inline constexpr int RingHighTwin(int low) {
  for (const RingLowKey& k : kRingLowKeys)
    if (k.low == low) return k.high;
  return 0;
}

}  // namespace assets
