#!/usr/bin/env python3
"""Scratch: turn (a) our wrestler_body.frag and (b) the spirv-cross text of the SDK translation into compute shaders
that read their interpolators + float constants from a buffer, with textures replaced by smooth fake functions.
Usage: mkharness.py mine|ref <src> <out.comp>"""
import re, sys

which, src, out = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src, encoding='utf-8').read()

PER_THREAD = 192  # floats: 10 interpolators * 4 + 38 constants * 4

prelude = r'''#version 450
#extension GL_EXT_spirv_intrinsics : require
#extension GL_EXT_scalar_block_layout : require
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer InBuf { float data[]; } inb;
layout(std430, binding = 1) writeonly buffer OutBuf { vec4 col[]; } outb;

vec4 g_out;
vec4 loadVec(uint base, uint k) { return vec4(inb.data[base + k*4u], inb.data[base + k*4u + 1u], inb.data[base + k*4u + 2u], inb.data[base + k*4u + 3u]); }

// fake textures: smooth, bounded, channel- and id-dependent
vec4 fake2D(int id, vec2 p) {
  float s = float(id) * 0.731;
  vec4 a = vec4(2.3, 3.1, 1.7, 2.9) + s * 0.05;
  vec4 b = vec4(1.9, 2.7, 3.3, 1.3) + s * 0.03;
  vec4 ph = vec4(0.3, 1.1, 2.0, 2.9) + s;
  return 0.5 + 0.5 * sin(a * p.x + b * p.y + ph + 0.4 * sin(p.x * 5.0 + s));
}
vec4 fakeCube(int id, vec3 dir) {
  vec3 d = normalize(dir);
  float s = float(id) * 0.531;
  return 0.5 + 0.5 * sin(vec4(2.1 * d.x + 1.3 * d.y, 1.7 * d.y - 2.2 * d.z, 2.9 * d.z + 0.8 * d.x, d.x - d.y + d.z) * 1.7 + vec4(0.2, 0.9, 1.6, 2.4) + s);
}
vec4 fakeTexture(int id, vec3 p) {
  if (id == 11 || id == 13) return fakeCube(id, p);
  return fake2D(id, p.xy);
}
#define texture(t, c) fakeTexture(t, c)
#define textureGrad(t, c, dx, dy) fakeTexture(t, c)
#define textureLod(t, c, l) fakeTexture(t, c)
#define sampler2DArray(t, s) (t)
#define samplerCube(t, s) (t)
#define dFdxCoarse(x) ((x) * 0.0)
#define dFdyCoarse(x) ((x) * 0.0)
'''

# ---- collect texture / sampler ids from the set 3 declarations and drop them
consts = []
def tex_decl(m):
    binding = int(m.group(1)); name = m.group(2)
    mm = re.match(r'xe_(?:texture|sampler)(\d+)_', name)
    ident = int(mm.group(1)) if mm else binding % 100
    consts.append(f'const int {name} = {ident};')
    return ''
text = re.sub(r'layout\(set = 3, binding = (\d+)\) uniform (?:texture2DArray|textureCube|sampler) (\w+);', tex_decl, text)

# ---- inputs
locs = []
def in_decl(m):
    loc = int(m.group(1)); name = m.group(2)
    locs.append((loc, name))
    return f'vec4 {name};'
text = re.sub(r'layout\(location = (\d+)\) in vec4 (\w+);', in_decl, text)
text = re.sub(r'layout\(location = 0\) invariant out vec4 (\w+);', r'vec4 \1;', text)
text = re.sub(r'layout\(location = 0\) out vec4 out_color;', '', text)

# ---- remove directives
text = re.sub(r'#version 450\n', '', text)
text = re.sub(r'#extension [^\n]*\n', '', text)
text = re.sub(r'#include "xenos_ps.glsl"\n', '', text)
text = re.sub(r'#if defined\(GL_EXT_control_flow_attributes\).*?#endif\n', '#define SPIRV_CROSS_FLATTEN\n#define SPIRV_CROSS_BRANCH\n#define SPIRV_CROSS_UNROLL\n#define SPIRV_CROSS_LOOP\n', text, flags=re.S)

if which == 'mine':
    # float constants block
    text = text.replace('layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[38]; } fc;',
                        'struct XeFC_ { vec4 c[38]; }; XeFC_ fc;')
    text = text.replace('XeOutputColor(vec4(color, in_vertex.w));', 'g_out = vec4(color, in_vertex.w);')
    text = text.replace('void main() {', 'void shaderMain() {')
    fcvar = 'fc.c'
else:
    # reference: float constants, system constants, other blocks
    text = re.sub(r'layout\(set = 1, binding = 2, std140\) uniform XeFloatConstants\s*\{\s*vec4 float_constants\[38\];\s*\} xe_uniform_float_constants;',
                  'struct XeFC_ { vec4 float_constants[38]; }; XeFC_ xe_uniform_float_constants;', text)
    m = re.search(r'layout\(set = 1, binding = 0, scalar\) uniform XeSystemConstants\s*\{(.*?)\} xe_uniform_system_constants;', text, re.S)
    body = re.sub(r'layout\(offset = \d+\) ', '', m.group(1))
    text = text.replace(m.group(0), 'struct XeSys_ {' + body + '}; XeSys_ xe_uniform_system_constants;')
    text = re.sub(r'layout\(set = 1, binding = 3, std140\) uniform XeBoolLoopConstants\s*\{.*?\} xe_uniform_bool_loop_constants;', '', text, flags=re.S)
    text = re.sub(r'layout\(set = 1, binding = 4, std140\) uniform XeFetchConstants\s*\{\s*uvec4 fetch_constants\[48\];\s*\} xe_uniform_fetch_constants;',
                  'struct XeFetch_ { uvec4 fetch_constants[48]; }; XeFetch_ xe_uniform_fetch_constants;', text)
    text = re.sub(r'layout\(set = 0, binding = 0, std430\) restrict readonly buffer XeSharedMemory\s*\{.*?\} xe_shared_memory;', '', text, flags=re.S)
    # no half-texel nudge in this comparison (ours has none): drop the nudge term
    text = re.sub(r'0\.00146484375 / float\(bitfieldExtract\([^;]*?\) \+ 1u\)', '0.0', text)
    text = re.sub(r'gl_SampleMask\[0\] = [^;]*;', '', text)
    text = text.replace('gl_FragCoord', 'vec4(0.0)')
    text = text.replace('discard;', 'return;')
    text = text.replace('void main()', 'void shaderMain()')
    # epilogue result is xe_out_fragment_data_0
    fcvar = 'xe_uniform_float_constants.float_constants'

glue = '\n'.join(consts) + '\n'
setup = ['void main() {', '  uint i = gl_GlobalInvocationID.x;', f'  uint base = i * {PER_THREAD}u;']
for loc, name in locs:
    setup.append(f'  {name} = loadVec(base, {loc}u);')
setup.append(f'  for (uint k = 0u; k < 38u; ++k) {fcvar}[k] = loadVec(base, 10u + k);')
if which == 'ref':
    setup.append('  xe_uniform_system_constants.flags = 7u << 16;')
    setup.append('  xe_uniform_system_constants.alpha_to_mask = 0u;')
    setup.append('  xe_uniform_system_constants.alpha_test_reference = 0.0;')
    setup.append('  xe_uniform_system_constants.color_exp_bias = vec4(1.0);')
    setup.append('  for (int k = 0; k < 2; ++k) xe_uniform_system_constants.texture_swizzled_signs[k] = uvec4(0u);')
    setup.append('  for (int k = 0; k < 48; ++k) xe_uniform_fetch_constants.fetch_constants[k] = uvec4(0u);')
    setup.append('  g_out = vec4(0.0);')
    setup.append('  shaderMain();')
    setup.append('  outb.col[i] = xe_out_fragment_data_0;')
else:
    setup.append('  shaderMain();')
    setup.append('  outb.col[i] = g_out;')
setup.append('}')

open(out, 'w').write(prelude + glue + text + '\n' + '\n'.join(setup) + '\n')
print(which, 'locs', locs)
