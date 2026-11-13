// The plane shadow chain's upload sites: the committed table of facts the writer is built from (Session 101, lane 139; docs/renderer/SHADOW_PLANE.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no game or SDK includes. Every row is a call of the op-21 recorder sub_82BB27D8 read from a listing (py tools\analysis\list_function.py FUNCTION): the return address (the call's address + 4),
// r4 = the first vertex register, r6 = the row count, and where r5 points. Addresses and our annotation only. The tracker accepts a call only when its return address, register and count match a row; the
// oracle driver (plane_oracle.h) replays these very rows; the unit test asserts Tracker::IsOurs covers every row. A listing that disagrees with a row is a bug in the row, fixed here, once.
#pragma once
#include <cstdint>

namespace rdr_scene::live::plane {

enum class Chain : uint8_t { Mask, Blur };
// What the uploaded rows are. Fade = one row of the module's 10-float distance array (each row is 16 bytes read at 4-byte steps); the other rows are whole.
enum class What : uint8_t { Fade, PlaneNormal, FitMatrix, Camera, PlanePos, BlurScale, BlurWeight, BlurMatrix, BlurPoint };

struct Site {
  uint32_t lr;       // the recorder's return address
  uint16_t reg;      // r4: the first vertex-constant register
  uint16_t rows;     // r6
  Chain chain;
  What what;
  uint8_t index;     // Fade: which row (0..6); else 0
  const char* from;  // where r5 points (the listing)
};

inline constexpr Site kSites[] = {
    // sub_827EC600 (the plane module's constant upload, called by the plane module's draw sub_827EED58 at 0x827EEE00; listing 0x827EC600-0x827EC714): ten calls
    {0x827EC628, 0, 1, Chain::Mask, What::Fade, 0, "module+68"},
    {0x827EC63C, 1, 1, Chain::Mask, What::Fade, 1, "module+72"},
    {0x827EC650, 2, 1, Chain::Mask, What::Fade, 2, "module+76"},
    {0x827EC664, 3, 1, Chain::Mask, What::Fade, 3, "module+80"},
    {0x827EC678, 4, 1, Chain::Mask, What::Fade, 4, "module+84"},
    {0x827EC68C, 5, 1, Chain::Mask, What::Fade, 5, "module+88"},
    {0x827EC6A0, 6, 1, Chain::Mask, What::Fade, 6, "module+92 (row 6: w = module+104, the weight)"},
    {0x827EC6DC, 8, 1, Chain::Mask, What::PlaneNormal, 0, "r1+80: (0, -1, 0, 0) built in the frame (listing 0x827EC6A0-0x827EC6D4)"},
    {0x827EC6F0, 13, 4, Chain::Mask, What::Camera, 0, "module+1072"},
    {0x827EC704, 9, 4, Chain::Mask, What::FitMatrix, 0, "module+1312"},
    // sub_827EE190 (per proxy kind: c7, then the proxy model's draw through vtable +136; listing 0x827EE190-0x827EE2BC): value (f31, [module+1440], f31, 0)
    {0x827EE248, 7, 1, Chain::Mask, What::PlanePos, 0, "r1+80"},
    // sub_827ECD10 (the blur's constants, called by sub_827EEAE0 at 0x827EEC18; listing 0x827ECD10-0x827ECF7C)
    {0x827ECD90, 40, 1, Chain::Blur, What::BlurPoint, 0, "r1+96: (table[idx].x, 0, table[idx].y, 1), table 0x83625ADC"},
    {0x827ECDB4, 33, 1, Chain::Blur, What::BlurScale, 0, "r1+80: [module+96]"},
    {0x827ECDEC, 35, 1, Chain::Blur, What::BlurWeight, 0, "r1+80: [module+104], the weight"},
    {0x827ECF74, 36, 4, Chain::Blur, What::BlurMatrix, 0, "r1+112: camera block x fit block, transposed"},
};
inline constexpr uint32_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

// The draws the snapshots hang on: the existing hooks' return addresses.
inline constexpr uint32_t kMaskDrawReturn = 0x82BF4D3Cu;   // after the bl sub_82BDED40 at 0x82BF4D38 (the part draw of sub_82BF22C8; the pass site shadow_roles::kPlanePassSite)
inline constexpr uint32_t kBlurDrawReturn = 0x827EEC88u;   // after the bl sub_82BB10C0 at 0x827EEC84 (the blur's op-15 draw; call site 0x827EEC84)

inline const Site* FindSite(uint32_t lr) {
  for (const Site& s : kSites)
    if (s.lr == lr) return &s;
  return nullptr;
}

// Not owned, stated (SHADOW_PLANE.md): EyePos c72 and the palette c76.. are NOT uploaded by sub_82BF22C8's op-21 sites (those 18 calls are engine-parameter slots at [r26+676..692]); sub_827ED278
// writes the proxy models' palette into guest memory (model+192 / +200 arrays) and the part draw sub_82BDED40 flushes it; the uploader of c72 is [U]. The blur's c34 is a PIXEL constant (op 28,
// sub_82BB1CF8 at 0x827ECDCC): not tracked.

}  // namespace rdr_scene::live::plane
