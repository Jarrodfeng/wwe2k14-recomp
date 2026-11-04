// wwe2k14 - the game exe's own copy of the D3D layer F2 record layout (Session 87). The SDK's rex/graphics/d3d_copy.h is
// the other copy; the exe never includes it, so a changed layout never forces an SDK re-pin. The two are tied by one
// number: the SDK reports its layout version as the int capability cvar gpu_d3d_copy_abi, and the hooks take records
// only when it equals kAbi below. Change a field here -> bump kAbi here AND kD3dCopyAbi in the SDK header.
#pragma once

#include <cstdint>

namespace wwe2k14::d3d_copy_layout {

inline constexpr int32_t kAbi = 3;
inline constexpr uint32_t kOpcode = 0x7D;  // the private type-3 packet (F1's token is 0x7C)
inline constexpr uint32_t kMagic = 0xD3D20001u;
inline constexpr uint32_t kHeaderDwords = 4;  // magic, sequence, kind, semantic dword count
inline constexpr uint32_t kMaxSemDwords = 64;

inline constexpr uint32_t kKindClear = 1;
inline constexpr uint32_t kKindGprSplit = 2;  // unused by the exe (census is the exe's own)
inline constexpr uint32_t kKindResolve = 3;

namespace clear {
inline constexpr uint32_t kFlags = 0, kColor = 1 /* 4 floats */, kDepth = 5, kStencil = 6, kSurfaceInfo = 7,
                          kDepthInfo = 8, kColorInfo = 9, kRect = 10 /* x0 y0 x1 y1 */, kPassRt = 14, kVtxCntl = 15,
                          kColorMask = 16 /* appended, ABI unchanged: the colour state a depth-only clear inherits */,
                          kBlendControl = 17, kColorControl = 18, kDwords = 19;
}
namespace resolve {
inline constexpr uint32_t kFlags = 0, kCopyControl = 1, kDestBase = 2, kDestPitch = 3, kDestInfo = 4, kDepthClear = 5,
                          kColorClear = 6, kColorClearLo = 7, kSurfaceInfo = 8, kColorInfo = 9, kDepthInfo = 10,
                          kRect = 11 /* x0 y0 x1 y1 */, kVtxCntl = 15, kDwords = 16;
}

}  // namespace wwe2k14::d3d_copy_layout
