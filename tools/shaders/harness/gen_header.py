#!/usr/bin/env python3
"""Session 87 (stage 4): regenerates src\\scene_vs_swap.h from the table below and the shaders' own sources. Our own tool.
For each row the float constant registers are read from the shader's `#define C<n> fc.c[i]` lines (the packing its SPIR-V
assumes), the SPIR-V header is src\\scene\\spirv\\<file>_vert.h (build it with tools\\post\\build_shaders.ps1 -Name <file>).
The header compiles against an SDK with or without the stream/flags registration arguments (a row that needs them is then
left unregistered).  Run:  py tools\\shaders\\harness\\gen_header.py
Row: (guest vertex shader hash, shader file, effect, draws a frame Brock / Bryan / match, pixel shaders it feeds,
      stream stride in dwords, stream fetch constant, flags: 0 or 'dynamic' (kFlagDynamicConstants))."""
import os, re

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
ARENA = 'arena background'
T = [
 ('4F1118D4B3F6D388', 'arena_vs_bg_lm', 'yBG_DefaultLM', '143 / 101 / 0', 'arena_bg_lm (2BC8853B)', 8, 95, 0),
 ('0093A3F475E9BA3F', 'arena_vs_bg', 'yBG_Default', '100 / 49 / 9', 'arena_bg (F335375C)', 8, 95, 0),
 ('EFCB021A03BA3E6A', 'arena_vs_bg_lm_tf', 'yBG_DefaultLM, lighting map per vertex', '97 / 51 / 0', 'arena_bg_lm (2BC8853B)', 8, 95, 0),
 ('AEE317D87B2A9CB8', 'arena_vs_bg_scroll2', 'yBG_UVScroll', '22 / 9 / 0', 'arena_bg_scroll (DB603A91)', 8, 95, 0),
 ('CAA5A55D7869AA08', 'arena_vs_bg_tf', 'yBG_Default, lighting map per vertex', '0 / 0 / 22', 'arena_bg (F335375C)', 8, 95, 0),
 ('3E4670E18A977431', 'arena_vs_bg_rim', 'yBG_Default, edge fade', '6 / 3 / 0', 'arena_bg (F335375C)', 8, 95, 0),
 ('5C0694B2FE1B593A', 'arena_vs_bg_shadow_spec_lm', 'yBG_DefaultLM, shadow + highlight', '12 / 0 / 0', 'arena_bg_shadow_spec_mask_lm (9B46FDF9)', 8, 95, 0),
 ('2F1B8D1655D529F5', 'arena_vs_bg_spec_fade_lm', 'yBG_DefaultLM, highlight faded by depth', '8 / 0 / 0', 'arena_bg_spec_mask_lm (AA33C0D6)', 8, 95, 0),
 ('2A3E3B9E6EDE114E', 'arena_vs_bg_spec', 'yBG_Default, highlight', '4 / 2 / 2', 'arena_bg_spec_plain (748BF8B0), arena_bg_spec (775AF960)', 8, 95, 0),
 ('C18C56D2C1C2D589', 'arena_vs_bg_shadow', 'yBG_Default, shadow + highlight', '0 / 0 / 8', 'arena_bg_shadow (E98D0C36)', 8, 95, 0),
 ('0D34F45940731536', 'arena_vs_bg_shadow_lm_tf', 'yBG_DefaultLM, shadow, lighting map per vertex', '5 / 2 / 0', 'arena_bg_shadow_lm (ED379393)', 8, 95, 0),
 ('24807879C84EB46B', 'arena_vs_bg_shadow_lm', 'yBG_DefaultLM, shadow', '4 / 3 / 0', 'arena_bg_shadow_lm (ED379393)', 8, 95, 0),
 ('94B69B274F560003', 'arena_vs_bg_spec_lm_tf', 'yBG_DefaultLM, highlight, lighting map per vertex', '5 / 2 / 0', 'arena_bg_spec_mask_lm (AA33C0D6)', 8, 95, 0),
 ('0EB3593F948BBFF0', 'arena_vs_bg_scroll2_lm', 'yBG_UVScrollLM, two layers', '3 / 2 / 0', 'arena_bg_scroll_blend_lm (DDBEB05B)', 8, 95, 0),
 ('E50620DCD7526CF0', 'arena_vs_bg_shadow_plain', 'yBG_Default, shadow', '2 / 0 / 2', 'arena_bg_shadow_plain (B63CD5FC)', 8, 95, 0),
 ('8FCFA0833E3DB382', 'arena_vs_bg_spec_fade', 'yBG_Default, highlight faded by depth', '0 / 0 / 4', 'arena_bg_spec (775AF960)', 8, 95, 0),
 ('9B582C106A74AE63', 'arena_vs_bg_scroll_lm', 'yBG_UVScrollLM', '1 / 1 / 0', 'arena_bg_scroll_lm (12280371)', 8, 95, 0),
 ('4B04CB1F86EEE25B', 'arena_vs_bg_scroll', 'yBG_UVScroll, one layer', '0 / 0 / 2', 'whatever the draw has (the bench shows the crowd atlas copy 7ABDFEFB)', 8, 95, 0),
 ('89D348AA9B28527C', 'arena_vs_bg_spec_lm', 'yBG_DefaultLM, highlight', '1 / 0 / 0', 'arena_bg_spec_mask_lm (AA33C0D6)', 8, 95, 0),
 ('94FE8F169F50CF85', 'arena_vs_bg_spec_tf', 'yBG_Default, highlight, lighting map per vertex', '0 / 0 / 1', 'arena_bg_spec (775AF960)', 8, 95, 0),
 # ---- batch 2 (props, particles, library and 2D quads)
 ('1E6883FCCDE1F688', 'vs_lib_flat', 'library flat-colour primitive', '40 / 41 / 37', 'debug_primitive (A4A965C1)', 7, 0, 0),
 ('1456901233B6564C', 'vs_lib_quad', 'library textured quad', '20 / 22 / 13', 'the post chain and copies', 6, 95, 0),
 ('2C132E0B6039C3F8', 'vs_lib_quad3', 'library textured quad, 3-float position', '1 / 3 / 0', 'restore copy (9A836B68)', 6, 95, 0),
 ('D6B8DEB0775CC47F', 'vs_glare_quad', 'yGlareEtc quad', '1 / 1 / 1', 'glare combines (4E2CD803, 1233F68C)', 6, 95, 0),
 ('13B1BADF537A2BFF', 'arena_vs_prop_bump', 'yProp_BumpMap', '35 / 35 / 35', 'arena_prop_bump (2B8A869C)', 8, 95, 0),
 ('C3570DF53E3AC981', 'arena_vs_particle', 'yParticle', '84 / 56 / 0', 'arena_particle (34B29856)', 10, 95, 0),
 ('2D1389E128EA124E', 'vs_rect', 'yRect', '12 / 15 / 2', 'movie_yuv / crowd atlas copy (B57EADC3, 7ABDFEFB)', 5, 95, 0),
 ('D261C3CC487FA436', 'vs_light_cube', 'yPreLight', '0 / 5 / 0', 'side_prelight (8479D8DA)', 12, 95, 0),
 ('6DC7C61EB5985AE8', 'arena_vs_prop', 'yProp_Default', '1 / 1 / 1', 'arena_prop (468C06E8)', 8, 95, 0),
 ('5D2AE398B797CF87', 'vs_liquid_copy', 'yLiquidMap copy', '0 / 6 / 6', 'liquid_map_copy (F235532B)', 6, 95, 0),
 ('8F60149686CB59D1', 'vs_liquid_layer', 'yLiquidMap layer', '0 / 2 / 2', 'side_liquid_layer (022F374B)', 6, 95, 0),
 ('3FCE6872E9C04D68', 'vs_sprite', 'ySprite', '11 / 0 / 0', 'sprite_modes (7754C618)', 5, 95, 0),
]
EXTRA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vs_table_extra.py')
if os.path.exists(EXTRA):
    exec(open(EXTRA, encoding='utf-8').read())  # (later batches append rows to T)

GENERATED = globals().get('GENERATED', set())      # (from vs_table_extra.py: built from a local dump, never committed)
GEN_FLOATS = globals().get('GEN_FLOATS', {})
lists = {}
for _, f, *_ in T:
    path = os.path.join(R, 'src', 'scene', f + '.vert')
    if f in GENERATED and not os.path.exists(path):
        regs = list(GEN_FLOATS.get(f, []))      # (a checkout without the generated files: the committed list)
    else:
        s = open(path, encoding='utf-8').read()
        regs = [int(m[0]) for m in re.findall(r'^#define C(\d+)\s+fc\.c\[(\d+)\]', s, re.M)]
        if f in GENERATED:
            assert regs == list(GEN_FLOATS.get(f, [])), f + ': GEN_FLOATS in vs_table_extra.py is out of date'
    assert regs == sorted(regs), f
    lists[f] = regs
names, order = {}, []
for f, regs in lists.items():
    key = tuple(regs)
    if key not in names:
        names[key] = 'kFloats%d_%d' % (len(regs), len(order)) if regs else 'kNoFloats'
        order.append(key)
decl = []
for key in order:
    users = [f for f, r in lists.items() if tuple(r) == key]
    decl.append('// ' + ', '.join(users))
    if key:
        decl.append('inline constexpr uint8_t %s[] = {%s};' % (names[key], ', '.join(str(r) for r in key)))
    else:
        decl.append('inline constexpr uint8_t kNoFloats[] = {0};  // (none: float_count 0 below)')
def inc(f):
    if f in GENERATED:
        return ('#if __has_include("scene/spirv/%s_vert.h")\n#include "scene/spirv/%s_vert.h"\n#define WWE_VS_HAVE_%s 1\n#endif'
                % (f, f, f.upper()))
    return '#include "scene/spirv/%s_vert.h"' % f
includes = '\n'.join(inc(f) for f in sorted(set(f for _, f, *_ in T)))
rows = []
for h, f, eff, draws, ps, stride, fc, flags in T:
    key = tuple(lists[f])
    nm = names[key]
    cnt = 'std::size(%s)' % nm if key else '0'
    fl = 'kDynamic' if flags == 'dynamic' else '0'
    row = ('    // %s, %s draws a frame (Brock / Bryan / match); feeds %s.\n'
           '    {0x%sull, %s_vert_spv, std::size(%s_vert_spv), %s, %s, "%s", %s, %d, %d},' %
           (eff, draws, ps, h, f, f, nm, cnt, eff, fl, stride, fc))
    if f in GENERATED:
        row = '#ifdef WWE_VS_HAVE_%s\n%s\n#endif' % (f.upper(), row)
    rows.append(row)
table = '\n'.join(rows)

OUT = r'''#pragma once
// Session 87 (stage 4 of D3D_LAYER_PLAN.md: "our own shaders for everything, nothing of the Xbox 360 GPU model left"):
// our own VERTEX shaders in place of the game's, the vertex-side twin of src\scene_swap.h. The SDK swaps a guest vertex
// shader's translation for ours by the guest shader's hash on NATIVE's plain vertex draws (vulkan/native/shader_swap.h
// RegisterVertex / BuildVertex; the SDK cvar gpu_native_vs_swap, default false, is the switch): our SPIR-V reads the
// vertex data, the system / float / fetch constants and the vertex textures itself and writes the interpolators our own
// pixel shaders (src\scene\arena_*.frag) read. The shaders: src\scene\arena_vs_*.vert and vs_*.vert (+ xenos_vs.glsl,
// xenos_vs_math.glsl, arena_vs.glsl), compiled by tools\post\build_shaders.ps1; each checked bit for bit against the SDK's
// translation of its guest shader on random vertices and constants by tools\shaders\harness\vs_harness.py.
//
// GENERATED by tools\shaders\harness\gen_header.py from its table: edit the table, not this file.
// Call wwe2k14::scene_vs_swap::Install() after scene_swap::Install(). wwe2k14_vs_swap (read at start, default false) turns
// gpu_native_vs_swap on when the SDK flag is still at its default; the shaders are registered either way (registration
// is cheap, and nothing happens with the SDK switch off). The rows of the shaders made from the game's own shader dump
// (GENERATED in vs_table_extra.py; tools/shaders/harness/gen_local_vs.py) exist only where those files do (__has_include).

#include <cstddef>
#include <cstdint>
#include <iterator>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "half2_hooks.h"  // wwe2k14::half2::Cp()
#include "scene_vs_instance.h"  // Session 94 (gpu_native_instance_world): the instancing variants
#include "scene_vs_multiview.h"  // Session 94 (gpu_d3d_decouple bit 3): the multiview variants
#include "scene_vs_mirror.h"  // Session 95 (item D, gpu_native_vertex_mirror): the vertex-input variants
#include "scene_vs_skin_once.h"  // Session 95 (gpu_native_skin_once): the skin-once variants
@@INCLUDES@@

REXCVAR_DEFINE_BOOL(wwe2k14_vs_swap, false, "wwe2k14",
                    "Session 87 (stage 4; read at start): our own vertex shaders for the arena background in place of "
                    "the game's translated ones (src\\scene_vs_swap.h; sets gpu_native_vs_swap when that is left at its "
                    "default). Off by default");

namespace wwe2k14::scene_vs_swap {

// Registration flag (SDK vulkan/native/shader_swap.h kFlagDynamicConstants): the shader indexes the whole 256-register float
// block by number (skinned shaders), and the guest shader must index its constants too.
inline constexpr uint32_t kDynamic = 4;

struct VertexMaterial {
  uint64_t hash;  // the guest vertex shader (a vertex-declaration copy of a base shader: one hash per copy)
  const uint32_t* spv;
  size_t words;
  const uint8_t* floats;  // the float constant registers the guest shader reads, ascending: the packing the SPIR-V assumes
  size_t float_count;
  const char* name;
  uint32_t flags;         // kDynamic or 0
  uint32_t stride_words;  // the one vertex stream the shader reads: dwords a vertex ...
  uint32_t fetch_constant;  // ... in this vertex fetch constant (95 = fetch constants dwords 190 / 191; the library's shaders: 0)
  // (Session 93: the Session 92 split variants spv_s2 / spv_s4 are gone: one compile reads the shared memory through the
  // per-draw window base on every GPU, see src\scene\xenos_vs.glsl.)
};

// The float constant registers each shader reads, ascending (the packing its SPIR-V assumes; BuildVertex refuses a draw
// whose guest shader reads others).
@@DECL@@

// Each entry: the guest vertex shader, the draws of the bench (Brock's entrance / Bryan's entrance / the match) it takes
// a frame, the pixel shader it feeds (src\scene\*.frag).
inline const VertexMaterial kArenaVertexShaders[] = {
@@TABLE@@
};

// One registration; against an SDK without the stream arguments only the arena-layout rows (stride 8, fetch constant 95,
// no flags) can be registered.
template <class Cp>
inline bool RegisterOne(Cp* cp, const VertexMaterial& m) {
  if constexpr (requires {
                  cp->RegisterNativeVertexSwap(m.hash, m.spv, m.words, m.floats, m.float_count, m.name, m.flags,
                                               m.stride_words, m.fetch_constant);
                }) {
    const bool ok = cp->RegisterNativeVertexSwap(m.hash, m.spv, m.words, m.floats, m.float_count, m.name, m.flags,
                                                 m.stride_words, m.fetch_constant);
    return ok;

  } else {
    if (m.flags || m.stride_words != 8 || m.fetch_constant != 95) return false;
    return cp->RegisterNativeVertexSwap(m.hash, m.spv, m.words, m.floats, m.float_count, m.name);
  }
}

// Once, after the graphics system exists (OnPostSetup, beside scene_swap::Install()).
inline void Install() {
  rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
  if (!cp) {
    REXLOG_WARN("[wwe2k14][scene_vs_swap] no command processor yet: no vertex shaders registered");
    return;
  }
  if (REXCVAR_GET(wwe2k14_vs_swap) && rex::cvar::GetFlagSource("gpu_native_vs_swap") == rex::cvar::Source::kDefault) {
    rex::cvar::SetFlagByName("gpu_native_vs_swap", "true");
  }
  int ok = 0;
  for (const VertexMaterial& m : kArenaVertexShaders) {
    ok += RegisterOne(cp, m) ? 1 : 0;
  }
  scene_vs_instance::RegisterAll(cp, kArenaVertexShaders, std::size(kArenaVertexShaders));  // Session 94
  scene_vs_multiview::RegisterAll(cp, kArenaVertexShaders, std::size(kArenaVertexShaders));  // Session 94 (gpu_d3d_decouple bit 3)
  scene_vs_mirror::RegisterAll(cp, kArenaVertexShaders, std::size(kArenaVertexShaders));  // Session 95 (item D, gpu_native_vertex_mirror)
  scene_vs_skin_once::RegisterAll(cp);  // Session 95 (gpu_native_skin_once)
  REXLOG_INFO("[wwe2k14][scene_vs_swap] vertex shaders registered: {} of {} (gpu_native_vs_swap = {})", ok,
              std::size(kArenaVertexShaders), rex::cvar::GetFlagByName("gpu_native_vs_swap"));
}

}  // namespace wwe2k14::scene_vs_swap
'''
out = OUT.replace('@@INCLUDES@@', includes).replace('@@DECL@@', '\n'.join(decl)).replace('@@TABLE@@', table)
open(os.path.join(R, 'src', 'scene_vs_swap.h'), 'w', encoding='utf-8', newline='\n').write(out)
print(len(T), 'shaders', len(order), 'float lists')
