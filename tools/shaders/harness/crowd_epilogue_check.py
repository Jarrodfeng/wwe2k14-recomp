#!/usr/bin/env python3
"""Session 87 stage 4: arithmetic check of the colour epilogue our scene materials run (src/scene/xenos_ps.glsl XeOutputColor:
alpha test, exponent bias, the 360's piecewise-linear gamma) against the SDK translator's own epilogue in its NATIVE module of
the match crowd's pixel shader (ED94EC2F6D52E14F), the shader src/scene/crowd_match.frag now replaces with kOwnEpilogue.

In the style of the other harness scripts here (and the SDK's tools/restore_check/check.py): both pieces of code become compute
shaders that read the colour the shader computed (the epilogue's input) and the three system constants the epilogue uses (the
flags dword, the alpha-test reference, the exponent bias) from a buffer, run over random records on the GPU, and the output colour
(or "discarded") is compared BIT FOR BIT. The translation's colour/texture part (the coordinate nudge, LOD bias, exponent adjust)
is cut out: crowd_match.frag keeps its own texture read as before; only the epilogue is new. Alpha to coverage (alpha_to_mask) is
not covered here and is not ours: shader_swap.cpp declines those draws even for kFlagOwnEpilogue. kXeColorQuantize is 0 here (the
translation's module has no final quantize; the SDK's per-translation spec constant covers it for every material).

Needs: the Vulkan SDK (glslangValidator, spirv-cross), LLVM's clang++ (to build the small runner once), numpy, and the translator's
module as a SPIR-V file: --ref <module.spv> (a dump_shaders 'vk_native' module of the shader, the one with the full epilogue, e.g.
shader_ED94EC2F6D52E14F_C220000000000003_e00000002.vk_native.bin.frag; local, never committed).
Usage: py crowd_epilogue_check.py --ref <module.spv> [--count 1048576] [--work <dir>]
"""
import argparse, glob, os, re, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
XENOS_PS = os.path.join(ROOT, 'src', 'scene', 'xenos_ps.glsl')
VKRUN = os.path.join(HERE, 'vkrun.cpp')
MARK = 0x7FC0DEAD  # the output of a discarded record (a quiet NaN with a payload)

def sdk_bin(name):
    c = sorted(glob.glob(r'C:\VulkanSDK\*\Bin\%s' % name))
    if not c: sys.exit('Vulkan SDK tool not found: ' + name)
    return c[-1]

PRELUDE = r'''#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_spirv_intrinsics : require
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer InBuf { float data[]; } inb;
layout(std430, binding = 1) writeonly buffer OutBuf { vec4 col[]; } outb;
bool g_discard = false;
vec4 g_fragcoord = vec4(0.0);
'''

def strip_common(text):
    text = re.sub(r'#version 450\n', '', text)
    text = re.sub(r'#extension [^\n]*\n', '', text)
    text = re.sub(r'#if defined\(GL_EXT_control_flow_attributes\).*?#endif\n',
                  '#define SPIRV_CROSS_FLATTEN\n#define SPIRV_CROSS_BRANCH\n#define SPIRV_CROSS_UNROLL\n#define SPIRV_CROSS_LOOP\n', text, flags=re.S)
    return text

def mine_text():
    text = strip_common(open(XENOS_PS, encoding='utf-8').read())
    text = re.sub(r'layout\(set = 1, binding = 0, std140\) uniform XeSystemConstants \{(.*?)\} xe_system;',
                  lambda m: 'struct XeSys_ {' + re.sub(r'layout\(offset = \d+\) ', '', m.group(1)) + '}; XeSys_ xe_system;', text, flags=re.S)
    text = re.sub(r'layout\(location = 0\) out vec4 out_color;', 'vec4 out_color;', text)
    text = re.sub(r'layout\(constant_id = 0\) const uint kXeColorQuantize = 0u;', 'const uint kXeColorQuantize = 0u;', text)
    text = text.replace('discard;', '{ g_discard = true; return; }')
    return text

def ref_text(spv):
    out = subprocess.run([sdk_bin('spirv-cross.exe'), '--vulkan-semantics', spv], capture_output=True, text=True, check=True).stdout
    text = strip_common(re.sub(r'^//[^\n]*\n', '', out, flags=re.M))
    # the colour / texture part (the translation's main loop) is replaced by the epilogue's input
    text, n = re.subn(r'xe_out_fragment_data_0 = vec4\(0\.0\);\s*SPIRV_CROSS_LOOP\s*for \(;;\)\s*\{.*?\n        break;\n    \}\n',
                      'xe_out_fragment_data_0 = g_in;\n', text, flags=re.S)
    if n != 1: sys.exit('the translation\'s main loop was not found in the module text (%d)' % n)
    m = re.search(r'layout\(set = 1, binding = 0, scalar\) uniform XeSystemConstants\s*\{(.*?)\} xe_uniform_system_constants;', text, re.S)
    text = text.replace(m.group(0), 'struct XeSys_ {' + re.sub(r'layout\(offset = \d+\) ', '', m.group(1)) + '}; XeSys_ xe_uniform_system_constants;')
    text = re.sub(r'layout\(set = 1, binding = 3, std140\) uniform XeBoolLoopConstants\s*\{.*?\} xe_uniform_bool_loop_constants;', '', text, flags=re.S)
    text = re.sub(r'layout\(set = 1, binding = 4, std140\) uniform XeFetchConstants\s*\{.*?\} xe_uniform_fetch_constants;', '', text, flags=re.S)
    text = re.sub(r'layout\(set = 0, binding = 0, std430\) restrict readonly buffer XeSharedMemory\s*\{.*?\} xe_shared_memory;', '', text, flags=re.S)
    text = re.sub(r'layout\(set = 3, binding = \d+\) uniform (?:texture2DArray|sampler) \w+;', '', text)
    text = re.sub(r'layout\(location = \d+\) in vec4 (xe_in_interpolator_\d);', r'vec4 \1;', text)
    text = re.sub(r'layout\(location = 0\) invariant out vec4 xe_out_fragment_data_0;', 'vec4 xe_out_fragment_data_0;', text)
    text = re.sub(r'gl_SampleMask\[0\] = [^;]*;', '', text)
    text = text.replace('gl_FragCoord', 'g_fragcoord').replace('discard;', '{ g_discard = true; return; }')
    text = text.replace('void main()', 'void shaderMain()')
    return 'vec4 g_in;\n' + text

def glue(which):
    sysv = 'xe_system' if which == 'mine' else 'xe_uniform_system_constants'
    outv = 'out_color' if which == 'mine' else 'xe_out_fragment_data_0'
    lines = ['void main() {', '  uint i = gl_GlobalInvocationID.x;', '  uint base = i * 8u;',
             '  vec4 c = vec4(inb.data[base], inb.data[base + 1u], inb.data[base + 2u], inb.data[base + 3u]);',
             '  %s.flags = floatBitsToUint(inb.data[base + 4u]);' % sysv,
             '  %s.alpha_test_reference = inb.data[base + 5u];' % sysv,
             '  %s.color_exp_bias = vec4(inb.data[base + 6u]);' % sysv]
    if which == 'ref':
        lines += ['  %s.alpha_to_mask = 0u;' % sysv, '  g_in = c;', '  %s = vec4(0.0);' % outv, '  shaderMain();']
    else:
        lines += ['  XeOutputColor(c);']
    lines.append('  outb.col[i] = g_discard ? vec4(uintBitsToFloat(0x%Xu)) : %s;' % (MARK, outv))
    lines.append('}')
    return '\n'.join(lines) + '\n'

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout

def records(n, seed):
    rng = np.random.default_rng(seed)
    rec = np.zeros((n, 8), np.float32)
    # colours: mostly in and around 0..1, plus the gamma pieces' boundaries (+-1 ulp), 0, 1, exact fractions, huge, tiny, inf, NaN
    c = rng.uniform(-1.0, 2.0, (n, 4)).astype(np.float32)
    edges = np.array([0.0, 1.0, 64 / 1023, 128 / 1023, 512 / 1023, 32 / 255, 0.5, 1e-30, -1e-30, 1e30, -1e30, np.inf, -np.inf, np.nan], np.float32)
    pick = rng.random((n, 4)) < 0.35
    e = edges[rng.integers(0, len(edges), (n, 4))]
    e = np.where(rng.random((n, 4)) < 0.4, np.nextafter(e, np.float32(2.0)), e)
    e = np.where(rng.random((n, 4)) < 0.4, np.nextafter(e, np.float32(-1.0)), e)
    c = np.where(pick, e, c).astype(np.float32)
    rec[:, 0:4] = c
    # flags: random bits everywhere, then the alpha-test function (bits 16-18, all 8 values) and the gamma bit (19) set explicitly
    flags = rng.integers(0, 2 ** 32, n, dtype=np.uint64).astype(np.uint32)
    flags &= ~np.uint32(0xF << 16)
    flags |= (rng.integers(0, 8, n).astype(np.uint32) << 16) | (rng.integers(0, 2, n).astype(np.uint32) << 19)
    rec[:, 4] = flags.view(np.float32)
    # alpha reference: the alpha itself (+-1 ulp) a third of the time (the equal / not equal cases), else random
    a = c[:, 3]
    ref = rng.uniform(-0.5, 1.5, n).astype(np.float32)
    sel = rng.random(n)
    ref = np.where(sel < 0.2, a, ref)
    ref = np.where((sel >= 0.2) & (sel < 0.3), np.nextafter(a, np.float32(10.0)), ref)
    ref = np.where((sel >= 0.3) & (sel < 0.4), np.nextafter(a, np.float32(-10.0)), ref)
    rec[:, 5] = ref.astype(np.float32)
    bias = np.array([1, 2, 4, 8, 0.5, 0.25, 0.125, 1, 1], np.float32)[rng.integers(0, 9, n)]
    rec[:, 6] = np.where(rng.random(n) < 0.3, rng.uniform(0.0, 4.0, n), bias).astype(np.float32)
    return rec

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True)
    ap.add_argument('--count', type=int, default=1048576)
    ap.add_argument('--work', default=os.path.join(ROOT, 'out', 'crowd_epilogue_check'))
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    runner = os.path.join(a.work, 'vkrun.exe')
    if not os.path.exists(runner):
        sdk = os.path.dirname(os.path.dirname(sdk_bin('glslangValidator.exe')))
        clang = r'C:\Program Files\LLVM\bin\clang++.exe'
        run([clang, '-O1', '-I', os.path.join(sdk, 'Include'), VKRUN, os.path.join(sdk, 'Lib', 'vulkan-1.lib'), '-o', runner])
    n = a.count
    rec = records(n, 87)
    inp = os.path.join(a.work, 'in.bin'); rec.tofile(inp)
    results = {}
    for which in ('mine', 'ref'):
        body = mine_text() if which == 'mine' else ref_text(a.ref)
        comp = os.path.join(a.work, '%s.comp' % which)
        open(comp, 'w').write(PRELUDE + body + '\n' + glue(which))
        spv = comp[:-5] + '.spv'
        run([sdk_bin('glslangValidator.exe'), '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-o', spv, comp])
        out = comp[:-5] + '.out'
        print(run([runner, spv, inp, out, str(n), '8']).strip().splitlines()[0])
        results[which] = np.fromfile(out, np.uint32).reshape(-1, 4)
    m, r = results['mine'], results['ref']
    bad = (m != r).any(1)
    disc = (r[:, 0] == MARK)
    flags = rec[:, 4].view(np.uint32)
    print('records %d: discarded %d (%.1f%%), gamma on %d, alpha test not "always" %d, exp bias != 1 %d' % (
        n, int(disc.sum()), 100.0 * disc.mean(), int(((flags >> 19) & 1).sum()), int((((flags >> 16) & 7) != 7).sum()), int((rec[:, 6] != 1.0).sum())))
    print('%d of %d records differ (bitwise, 4 components or the discard mark)' % (int(bad.sum()), n))
    if bad.any():
        nan_only = bad & np.isnan(m.view(np.float32)).any(1) & np.isnan(r.view(np.float32)).any(1)
        print('  of them with a NaN in both (payload / sign differences only): %d' % int(nan_only.sum()))
        for i in np.nonzero(bad & ~nan_only)[0][:8]:
            print('  record %d: c %s flags %08X ref %g bias %g mine %s ref %s' % (
                i, rec[i, :4], flags[i], rec[i, 5], rec[i, 6], m[i].view(np.float32), r[i].view(np.float32)))
    ok = not (bad & ~(np.isnan(m.view(np.float32)).any(1) & np.isnan(r.view(np.float32)).any(1))).any()
    print('RESULT', ('bit-identical on %d records%s' % (n, '' if not bad.any() else ' (NaN payloads aside)')) if ok else 'DIFFERS')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
