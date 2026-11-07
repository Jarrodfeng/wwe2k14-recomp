#pragma once
// Session 95 (gpu_native_skin_once): the skin-once shaders (src\scene\skin_once.comp and the _so variants, tools\shaders\build_skin_once.py). The SDK skins each wrestler vertex once a frame with the
// compute shader; the draws of the shaders listed here read the result as a real vertex buffer through their _so variant (vulkan/native/skin_once.h). Registered beside the plain shaders by
// scene_vs_swap::Install() when the SDK switch gpu_native_skin_once is on; the plain code stays for every draw the SDK does not prove safe.
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "scene/spirv/skin_once_comp.h"
#include "scene/spirv/vs_ch_anisotropic_so_vert.h"
#include "scene/spirv/vs_ch_gear_w12_so_vert.h"
#include "scene/spirv/vs_ch_gear_w9_so_vert.h"
#include "scene/spirv/vs_ch_reflect_w11_so_vert.h"
#include "scene/spirv/vs_ch_reflect_w12_so_vert.h"
#include "scene/spirv/vs_ch_reflect_w9_so_vert.h"
#include "scene/spirv/vs_ch_reflect_w9p_so_vert.h"
#include "scene/spirv/vs_ch_skin_a_so_vert.h"
#include "scene/spirv/vs_ch_skin_fx_so_vert.h"
#include "scene/spirv/vs_ch_skin_lean_so_vert.h"
#include "scene/spirv/vs_skin_rim_so_vert.h"
#include "scene/spirv/vs_skin_shadow_so_vert.h"

namespace wwe2k14::scene_vs_skin_once {

// The SDK's SkinOnce::Role, and bit 4: the shader's blend is CH_SKIN_STYLE 1 (src\scene\vs_ch.glsl).
inline constexpr uint32_t kShadow = 0, kRim = 1, kMain = 2, kStyle1 = 16;

struct Row {
  uint64_t hash;  // the guest vertex shader (the rows of scene_vs_swap.h)
  const uint32_t* so;
  size_t words;
  uint32_t role;
};
inline const Row kRows[] = {
    {0x0BBBF514D70CF0F4ull, vs_skin_shadow_so_vert_spv, std::size(vs_skin_shadow_so_vert_spv), kShadow},
    {0x0D7075D53FCA3F02ull, vs_skin_shadow_so_vert_spv, std::size(vs_skin_shadow_so_vert_spv), kShadow},
    {0xBDAF8155EBF110C2ull, vs_skin_shadow_so_vert_spv, std::size(vs_skin_shadow_so_vert_spv), kShadow},
    {0x222898168D2A226Aull, vs_skin_shadow_so_vert_spv, std::size(vs_skin_shadow_so_vert_spv), kShadow},
    {0xE8AC5C3074483B96ull, vs_skin_rim_so_vert_spv, std::size(vs_skin_rim_so_vert_spv), kRim},
    {0x3F63A55561994118ull, vs_skin_rim_so_vert_spv, std::size(vs_skin_rim_so_vert_spv), kRim},
    {0xA441E84E647C3D9Full, vs_skin_rim_so_vert_spv, std::size(vs_skin_rim_so_vert_spv), kRim},
    {0xF709D14F30FCC95Cull, vs_ch_gear_w12_so_vert_spv, std::size(vs_ch_gear_w12_so_vert_spv), kMain | kStyle1},
    {0x3F91EC05EDABDF7Bull, vs_ch_skin_a_so_vert_spv, std::size(vs_ch_skin_a_so_vert_spv), kMain | kStyle1},
    {0x0E92FB108A675C39ull, vs_ch_skin_fx_so_vert_spv, std::size(vs_ch_skin_fx_so_vert_spv), kMain | kStyle1},
    {0xA6767C4D4522DFAAull, vs_ch_gear_w9_so_vert_spv, std::size(vs_ch_gear_w9_so_vert_spv), kMain | kStyle1},
    {0xA8E30EED87562597ull, vs_ch_reflect_w9_so_vert_spv, std::size(vs_ch_reflect_w9_so_vert_spv), kMain},
    {0x56DD2E94E20F2826ull, vs_ch_anisotropic_so_vert_spv, std::size(vs_ch_anisotropic_so_vert_spv), kMain | kStyle1},
    {0xD345ACFD67B8D83Full, vs_ch_reflect_w12_so_vert_spv, std::size(vs_ch_reflect_w12_so_vert_spv), kMain},
    {0x9CB11A97F230260Cull, vs_ch_reflect_w11_so_vert_spv, std::size(vs_ch_reflect_w11_so_vert_spv), kMain},
    {0xB15688766DA713DCull, vs_ch_reflect_w9p_so_vert_spv, std::size(vs_ch_reflect_w9p_so_vert_spv), kMain},
    {0xF006439D879C5FDBull, vs_ch_skin_lean_so_vert_spv, std::size(vs_ch_skin_lean_so_vert_spv), kMain | kStyle1},
};

// After the plain rows are registered. SDK flag bits of RegisterNativeVertexSwap: 27 = the _so variant of the hash (role in the stride argument), 26 = the compute shader.
template <class Cp>
inline int RegisterAll(Cp* cp) {
  if (rex::cvar::GetFlagByName("gpu_native_skin_once") != "true") return 0;
  if (rex::cvar::GetFlagByName("gpu_native_vertex_input") != "0" && rex::cvar::GetFlagByName("gpu_native_skin_with_input") != "true") return 0;  // (Session 95, r3gpu: the _so module has its own slot now; without the switch the SDK leaves skin once off while vertex input is on)
  int ok = 0;
  for (const Row& r : kRows) ok += cp->RegisterNativeVertexSwap(r.hash, r.so, r.words, nullptr, 0, nullptr, 0x08000000u, r.role, 0) ? 1 : 0;
  const bool compute = cp->RegisterNativeVertexSwap(0, skin_once_comp_spv, std::size(skin_once_comp_spv), nullptr, 0, nullptr, 0x04000000u, 0, 0);
  REXLOG_INFO("[skin1] skin-once shaders registered: {} of {} rows, compute {}", ok, std::size(kRows), compute);
  return ok;
}

}  // namespace wwe2k14::scene_vs_skin_once
