#!/usr/bin/env python3
"""Session 87 (stage 4): arithmetic harness for our own VERTEX shaders (src\\scene\\*.vert), the vertex twin of
mkharness.py. Our own tool; reads local dumps at run time, nothing from the game is in the repo.

It turns a vertex shader of ours (GLSL, with its includes) and the SDK translation of the guest shader it replaces (the
SPIR-V module of a --dump_shaders run, through spirv-cross) into compute shaders, runs both on the GPU over many random
vertex records and constant sets, and compares the clip position and the interpolators. Textures are smooth fake
functions (the same for both). It proves the vertex fetch decoding, the arithmetic and the channel mapping; the vertex
texture's sampler state is not exercised.

  py vs_harness.py --mine src\\scene\\arena_bg_lm_a.vert --ref out\\s70\\shader_dump\\shader_<hash>_<mod>.vk.bin.vert
                   [--rounds 64] [--verts 4096] [--seed 1] [--runner path\\to\\vkrun_vs.exe] [--work dir]

The runner (tools\\shaders\\harness\\vkrun_vs.cpp) is built by hand: clang++ -O1 vkrun_vs.cpp -I<VulkanSDK>\\Include
-L<VulkanSDK>\\Lib -lvulkan-1 -o vkrun_vs.exe (no binary is committed).
"""
import argparse, os, re, struct, subprocess, sys
import numpy as np

SDK = r'C:\VulkanSDK'
def sdk_bin(name):
    best = None
    for d in sorted(os.listdir(SDK), reverse=True):
        p = os.path.join(SDK, d, 'Bin', name)
        if os.path.exists(p):
            best = p
            break
    if not best:
        raise SystemExit('missing ' + name)
    return best

BLOCK_BINDING = {'XeSharedMemory': 0, 'XeSystemConstants': 1, 'XeFloatConstants': 2, 'XeBoolLoopConstants': 3,
                 'XeFetchConstants': 4}

PRELUDE = r'''
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 5) writeonly buffer OutBuf { vec4 v[]; } outb;
int g_vi;
vec4 g_pos;
// fake vertex textures: smooth, bounded, per texture id; lod-dependent so the fetch constant's LOD is exercised
vec4 fakeTex(int id, vec3 p, float lod) {
  float s = float(id) * 0.731;
  vec4 a = vec4(2.3, 3.1, 1.7, 2.9) + s * 0.05;
  vec4 b = vec4(1.9, 2.7, 3.3, 1.3) + s * 0.03;
  vec4 ph = vec4(0.3, 1.1, 2.0, 2.9) + s;
  return 0.5 + 0.5 * sin(a * p.x + b * p.y + ph + 0.4 * sin(p.x * 5.0 + s) + 0.1 * lod);
}
#define sampler2DArray(t, s) (t)
#define textureLod(t, c, l) fakeTex(t, c, l)
'''

def expand_includes(path, seen=None):
    base = os.path.dirname(path)
    out = []
    for line in open(path, encoding='utf-8').read().splitlines():
        m = re.match(r'\s*#include\s+"([^"]+)"', line)
        if m:
            out.append(expand_includes(os.path.join(base, m.group(1))))
        else:
            out.append(line)
    return '\n'.join(out)

def preprocessed(path):
    """glslang -E: the includes and the #if branches resolved (the _vi shader's attribute declarations depend on XE_VI_DWORDS)."""
    r = subprocess.run([sdk_bin('glslangValidator.exe'), '-E', '-S', 'vert', path], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('glslang -E failed: ' + r.stdout + r.stderr)
    return ''.join(l for l in r.stdout.splitlines(True) if not l.startswith('#line'))

def optimized_cross(path, defs, opt=True):
    """Session 95: the shader as shipped: glslang, spirv-opt -O --target-env=vulkan1.0, spirv-val, then spirv-cross text (the harness's own input form)."""
    import tempfile
    d = tempfile.mkdtemp(prefix='vsopt_')
    s1, s2 = os.path.join(d, 'a.spv'), os.path.join(d, 'b.spv')
    for cmd in ([sdk_bin('glslangValidator.exe'), '-V', '--target-env', 'vulkan1.0'] + defs + ['-S', 'vert', '-o', s1, path],
                ([sdk_bin('spirv-opt.exe'), '-O', '--target-env=vulkan1.0', s1, '-o', s2] if opt else [sys.executable, '-c', 'import shutil,sys; shutil.copyfile(sys.argv[1], sys.argv[2])', s1, s2]),
                [sdk_bin('spirv-val.exe'), '--target-env', 'vulkan1.0', s2]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('%s failed: %s' % (os.path.basename(cmd[0]), r.stdout + r.stderr))
    r = subprocess.run([sdk_bin('spirv-cross.exe'), '--version', '450', '--vulkan-semantics', s2], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('spirv-cross failed: ' + r.stderr)
    return r.stdout

def to_compute(text, verts, vi_stride=0):
    # directives
    text = re.sub(r'#version [^\n]*\n', '', text)
    text = re.sub(r'#extension GL_GOOGLE_include_directive[^\n]*\n', '', text)
    exts = set(re.findall(r'#extension [^\n]*', text))
    text = re.sub(r'#extension [^\n]*\n', '', text)
    # textures: strip, name -> id (a fetch constant number, 100 + for the signed image)
    consts = []
    def tex(m):
        binding = int(m.group(1)); name = m.group(3)
        if m.group(2) == 'sampler':
            consts.append('const int %s = -1;' % name)
            return ''
        mm = re.match(r'xe_texture(\d+)_2d_([us])', name)
        if mm:
            ident = int(mm.group(1)) + (100 if mm.group(2) == 's' else 0)
        else:
            ident = binding % 1000 if binding < 1100 else 100 + binding % 100
        consts.append('const int %s = %d;' % (name, ident))
        return ''
    text = re.sub(r'layout\(set = 2, binding = (\d+)\) uniform (texture2DArray|sampler) (\w+);', tex, text)
    # blocks: set 0 for all, a binding of the harness's own per block, buffers instead of uniforms
    def block(m):
        name = m.group(4)
        if name not in BLOCK_BINDING:
            raise SystemExit('unknown block ' + name)
        return 'layout(set = 0, binding = %d%s) readonly buffer %s' % (BLOCK_BINDING[name], m.group(3) or '', name)
    text = re.sub(r'layout\(set = (\d+), binding = (\d+)(, \w+)?\)\s*(?:restrict\s+)?(?:readonly\s+)?(?:uniform|buffer)\s+(\w+)', block, text)
    # outputs -> globals
    outs = {}
    def out(m):
        outs[int(m.group(1))] = m.group(2)
        return 'vec4 %s;' % m.group(2)
    text = re.sub(r'layout\(location = (\d+)\)\s*(?:invariant\s+)?out\s+vec4\s+(\w+);', out, text)
    text = re.sub(r'\bgl_VertexIndex\b', 'g_vi', text)
    text = re.sub(r'out\s+gl_PerVertex\s*\{[^}]*\}\s*;', '', text)
    text = re.sub(r'invariant\s+gl_Position\s*;', '', text)
    text = re.sub(r'\bgl_Position\b', 'g_pos', text)
    text = re.sub(r'\bvoid main\s*\(\s*\)', 'void vsMain()', text)
    text = re.sub(r'#if defined\(GL_EXT_control_flow_attributes\).*?#endif\n',
                  '#define SPIRV_CROSS_FLATTEN\n#define SPIRV_CROSS_BRANCH\n#define SPIRV_CROSS_UNROLL\n#define SPIRV_CROSS_LOOP\n',
                  text, flags=re.S)
    # (a stray layout(location) block for gl_PerVertex does not exist in spirv-cross output)
    vi_fill = []
    if vi_stride:
        # Session 95 (the _vi shaders, vertex_input_layout.h's ViAttributes): the vertex inputs are the console's raw dwords, filled from the same shared memory the plain
        # shader reads (vertex i = the words at i * stride, NOT swapped: the _vi shader swaps them itself), so ours-vi and ours-plain see identical vertices
        attrs, word = {}, 0
        while word < vi_stride:
            left = vi_stride - word
            n = 4 if left >= 4 else 2 if left >= 2 else 1
            attrs[len(attrs)] = (word, n)
            word += n
        decl = re.findall(r'layout\(location = (\d+)\)\s*in\s+(uvec[24]|uint)\s+(\w+)\s*;', text)
        text = re.sub(r'layout\(location = (\d+)\)\s*in\s+(uvec[24]|uint)\s+(\w+)\s*;', r'\2 \3;', text)
        for loc, typ, name in decl:
            first, n = attrs[int(loc)]
            comps = ['shared_memory[i * %du + %du]' % (vi_stride, first + c) for c in range(n)]
            vi_fill.append('  %s = %s;' % (name, comps[0] if typ == 'uint' else '%s(%s)' % (typ, ', '.join(comps))))
    main = ['void main() {', '  uint i = gl_GlobalInvocationID.x;', '  g_vi = int(i);'] + vi_fill + ['  vsMain();',
            '  outb.v[i * 13u] = g_pos;']
    for loc in range(12):
        main.append('  outb.v[i * 13u + %du] = %s;' % (loc + 1, outs[loc] if loc in outs else 'vec4(0.0)'))
    main.append('}')
    head = '#version 450\n' + '\n'.join(sorted(exts | {'#extension GL_EXT_scalar_block_layout : require'})) + '\n'
    # (the optimised _vi module holds no storage block: declare the memory the inputs are filled from)
    extra = '' if (not vi_stride or 'XeSharedMemory' in text) else 'layout(set = 0, binding = 0, std430) readonly buffer XeSharedMemory { uint shared_memory[]; };' + chr(10)
    return head + PRELUDE + extra + chr(10).join(consts) + chr(10) + text + chr(10) + chr(10).join(main) + chr(10), sorted(outs)

def float_const_count(text):
    m = re.search(r'float_constants\[(\d+)\]', text) or re.search(r'vec4 c\[(\d+)\]', text)
    return int(m.group(1)) if m else 0

def compile_comp(src_path, spv_path):
    r = subprocess.run([sdk_bin('glslangValidator.exe'), '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-o', spv_path, src_path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:], r.stderr[-2000:])
        raise SystemExit('glslang failed on ' + src_path)

def bswap(a):
    a = a.astype(np.uint32)
    return ((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24)

WILD = False
BOOL0 = None
DYNAMIC = False
VI = False
BONE_SAFE = False  # Session 95 (--bone-safe): in --dynamic mode EVERY dword holds bytes below 40, so no bone index leaves the register file (XeBone clamps it; the translation reads past it)


def make_inputs(rounds, verts, seed, nfloat):
    rng = np.random.default_rng(seed)
    recs = []
    for r in range(rounds):
        sys_ = np.zeros(128, np.uint32)  # 512 bytes
        flags = 0
        for bit in (8, 9, 10):
            if rng.random() < 0.5:
                flags |= 1 << bit
        # (flag bit 10 = the guest w is not reciprocal)
        sys_[0] = flags
        sys_[2] = 0  # vertex index endian: none
        sys_[3] = 0xFFFFFFFF
        sys_[4] = np.uint32(rng.integers(0, 3) if (r % 4 == 3 and not VI) else 0)  # a vertex base index sometimes (never for --vi: the variant needs identity indices)
        scale = rng.uniform(0.3, 2.0, 3).astype(np.float32); offs = rng.uniform(-1, 1, 3).astype(np.float32)
        sys_[32:35] = scale.view(np.uint32)   # offset 128
        sys_[36:39] = offs.view(np.uint32)    # offset 144
        sys_[124] = 0                         # vertex index min (offset 496)
        sys_[125] = 0xFFFFFF                  # vertex index max (offset 500)
        # the swizzled signs of the vertex texture (system constants offset 176 + 16: array element 1, x): mostly unsigned
        signs = int(rng.choice([0, 0, 0, 0x15, 0x2A, 0x3F, 0x01, 0x04, 0x10, 0x40, 0x80, 0xC0, 0x55, 0xAA, 0xFF])) | int(rng.choice([0, 0, 0, 0x1500, 0x2A00, 0x3F00, 0x0100, 0x0400, 0x1000]))  # (fetch constant 16, 17)
        sys_[48] = np.uint32(signs)  # offset 192 = texture_swizzled_signs[1].x
        flt = rng.uniform(-2, 2, (1024, 4)).astype(np.float32)
        nice = rng.choice([0.0, 0.5, 1.0, -1.0, 0.25], size=(1024, 4))
        pick = rng.random((1024, 4)) < 0.3
        flt = np.where(pick, nice, flt).astype(np.float32)
        if DYNAMIC:
            # (a skinned shader indexes the float block by bone byte x c255.z: non-negative constants keep the index in range)
            flt = np.abs(flt)
            # (Session 95: the bone index is kept inside the register file in the shaders (XeBone, -76..177); a byte x c255.z beyond that read past the constants in the translation, so the harness keeps it inside: <= 255 x 0.6)
            flt[255, 2] = rng.choice([0.0, 0.25, 0.5, 0.6])
        fetch = rng.integers(0, 2**32, (48, 4), dtype=np.uint64).astype(np.uint32)
        fetch[0, 0] = 3                                            # (the library shaders' stream: fetch constant 0)
        fetch[47, 2] = 3                                           # the vertex fetch constant: base 0, type 3
        fetch[47, 3] = 0x80000000                                  # (the endian word is baked)
        fetch[24, 2] = np.uint32(rng.integers(0, 256) | (rng.integers(0, 256) << 13))   # the vertex texture's size
        fetch[25, 0] = np.uint32(rng.integers(0, 1 << 12) << 12)   # (the LOD bias bits)
        bl = rng.integers(0, 2**32, 64, dtype=np.uint64).astype(np.uint32)  # (bool constants: every branch of a skinned shader)
        if BOOL0 is not None:
            bl[0] = np.uint32(BOOL0)
        if DYNAMIC:
            v_small = True
        # 12 dwords a vertex (the strides of the game's meshes are 3-12): the position first, then each dword either a
        # plausible float or random bits, so packed fields (normals, colours, halves) see every bit pattern
        fl = rng.uniform(-10, 10, (verts, 12)).astype(np.float32).view(np.uint32)
        bits = rng.integers(0, 2**32, (verts, 12), dtype=np.uint64).astype(np.uint32)
        if not WILD:
            # (bounded: the exponent of the raw bits stays in 2^-15 .. 2^15, so no product overflows to an infinity and the
            # guest's 0 x infinity rule is only met where a reciprocal root or reciprocal makes one; the packed fields
            # still see every sign / mantissa pattern)
            exp = rng.integers(112, 143, (verts, 12), dtype=np.uint64).astype(np.uint32)
            bits = (bits & np.uint32(0x807FFFFF)) | (exp << np.uint32(23))
        v = np.where(rng.random((verts, 12)) < 0.5, fl, bits).astype(np.uint32)
        pos = rng.uniform(-50, 50, (verts, 4)).astype(np.float32)
        pos[:, 3] = np.where(rng.random(verts) < 0.5, 1.0, rng.uniform(0.5, 2.0, verts)).astype(np.float32)
        v[:, 0:4] = pos.view(np.uint32)
        if DYNAMIC:
            # (any dword may hold bone bytes: keep the four bytes of a share of the dwords below 40)
            small = rng.integers(0, 40, (verts, 12, 4), dtype=np.uint64).astype(np.uint32)
            packed = small[..., 0] | (small[..., 1] << np.uint32(8)) | (small[..., 2] << np.uint32(16)) | (small[..., 3] << np.uint32(24))
            v = np.where(rng.random((verts, 12)) < (1.0 if BONE_SAFE else 0.25), packed, v).astype(np.uint32)
            v[:, 0:4] = pos.view(np.uint32)
        v = bswap(v)  # the console's big-endian dwords as the host reads them from memory
        recs.append(sys_.tobytes() + flt.tobytes() + fetch.tobytes() + bl.tobytes() + b'\0' * (256 - 256) + v.tobytes())
    return b''.join(recs)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mine', required=True)
    ap.add_argument('--ref', default=None)
    ap.add_argument('--vi', action='store_true', help='Session 95: compare the plain arena shader (--mine) with its _vi variant (vertex inputs, no swap); no --ref')
    ap.add_argument('--rounds', type=int, default=64)
    ap.add_argument('--verts', type=int, default=4096)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--runner', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\harness_vs\vkrun_vs.exe')
    ap.add_argument('--work', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\harness_vs')
    ap.add_argument('--tol', type=float, default=2e-5)
    ap.add_argument('--wild', action='store_true', help='unbounded floats and raw bits in the vertex data (overflow to infinities)')
    ap.add_argument('--dynamic', action='store_true', help='skinned shader: non-negative constants, bone bytes below 40')
    ap.add_argument('--bool0', type=lambda x: int(x, 0), default=None, help='fix bool constants 0-31 (a word) to find which skinning branch differs')
    ap.add_argument('--bone-safe', action='store_true', help='--dynamic: all dwords hold bytes below 40 (no bone index past the register file, where the shaders clamp and the translation reads out of range)')
    ap.add_argument('--noclamp', action='store_true', help='Session 95: compile ours with XE_NO_BONE_CLAMP (the bone index is not clamped to the register file), to compare with the translation on out-of-range indices')
    ap.add_argument('--opt', action='store_true', help='Session 95: both sides are the glslang module after spirv-opt -O (spirv-cross text), i.e. what the shipped headers hold')
    ap.add_argument('--plain-noopt', action='store_true', help='Session 95 (with --vi --opt): the reference side is the plain shader as glslang makes it, NOT optimised, so the optimiser itself is under test (the shipped plain modules are unoptimised)')
    ap.add_argument('--plain', action='store_true', help='compile ours with AV_PLAIN (no fused adds), to compare with a translation without FMA')
    a = ap.parse_args()
    global WILD, DYNAMIC, BOOL0, VI, BONE_SAFE
    VI = a.vi
    BONE_SAFE = a.bone_safe
    WILD = a.wild
    BOOL0 = a.bool0
    DYNAMIC = a.dynamic
    os.makedirs(a.work, exist_ok=True)
    stride = 0
    if a.vi or a.opt:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        import build_vertex_input_vs as bv
    if a.vi:
        stride = bv.table_rows()[os.path.splitext(os.path.basename(a.mine))[0]][0]
        # ref = the plain shader (shared-memory fetch), mine = the same source compiled with the _vi defines
        raw = open(a.mine, encoding='utf-8').read()
        plain_src = expand_includes(a.mine)
        tmp_vi = os.path.join(os.path.dirname(os.path.abspath(a.mine)), '_vi_tmp_%d.vert' % os.getpid())
        vi_raw = raw.replace('#include "xenos_vs.glsl"', '#define XE_VERTEX_INPUT\n#define XE_VI_DWORDS %d\n#include "xenos_vs.glsl"' % stride, 1)
        open(tmp_vi, 'w', encoding='utf-8', newline='\n').write(vi_raw)
        try:
            mine_src = preprocessed(tmp_vi)
            if a.opt:
                mine_src = optimized_cross(tmp_vi, [])
        finally:
            os.remove(tmp_vi)
        if a.opt:
            plain_src = optimized_cross(a.mine, [], opt=not a.plain_noopt)
        ref_text, ref_outs = to_compute(plain_src, a.verts)
        cross_stdout = plain_src
    else:
        cross = subprocess.run([sdk_bin('spirv-cross.exe'), '--version', '450', '--vulkan-semantics', a.ref], capture_output=True, text=True)
        if cross.returncode != 0:
            raise SystemExit('spirv-cross failed: ' + cross.stderr)
        ref_text, ref_outs = to_compute(cross.stdout, a.verts)
        cross_stdout = cross.stdout
        mine_src = optimized_cross(a.mine, ['-DXE_NO_BONE_CLAMP'] if a.noclamp else []) if a.opt else expand_includes(a.mine)
    if a.noclamp and not a.opt:
        mine_src = mine_src.replace('#version 450', '#version 450' + chr(10) + '#define XE_NO_BONE_CLAMP', 1)
    if a.plain and a.opt:
        raise SystemExit('--plain with --opt does nothing (the define would land in already compiled text); use one of them')
    if a.plain:
        mine_src = mine_src.replace('#version 450', '#version 450\n#define AV_PLAIN', 1)
    mine_text, mine_outs = to_compute(mine_src, a.verts, stride)
    nfloat = max(float_const_count(cross_stdout), float_const_count(mine_src))
    res = {}
    inp = os.path.join(a.work, 'in.bin')
    open(inp, 'wb').write(make_inputs(a.rounds, a.verts, a.seed, nfloat))
    for tag, text in (('ref', ref_text), ('mine', mine_text)):
        comp = os.path.join(a.work, tag + '.comp'); spv = os.path.join(a.work, tag + '.spv'); outp = os.path.join(a.work, tag + '.out')
        open(comp, 'w').write(text)
        compile_comp(comp, spv)
        r = subprocess.run([a.runner, spv, inp, outp, str(a.rounds), str(a.verts)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout, r.stderr)
            raise SystemExit('runner failed for ' + tag)
        res[tag] = np.fromfile(outp, np.float32).reshape(a.rounds * a.verts, 13, 4)
    print('outputs: ref', ref_outs, 'mine', mine_outs)
    A, B = res['mine'], res['ref']
    names = ['position'] + ['interp%d' % i for i in range(12)]
    worst = 0.0
    for k in range(13):
        loc = k - 1
        if loc >= 0 and loc not in ref_outs:
            if loc in mine_outs:
                print('%-9s only in ours (ignored)' % names[k])
            continue
        x, y = A[:, k, :], B[:, k, :]
        finite_ref = np.ones(len(A), bool)  # (every record is compared; infinities and NaNs must agree)
        both_nan = np.isnan(x) & np.isnan(y)
        same_inf = np.isinf(x) & np.isinf(y) & (np.sign(x) == np.sign(y))
        d = np.abs(x - y)
        scale = np.maximum(1.0, np.nanmax(np.maximum(np.abs(x), np.abs(y)), axis=1, keepdims=True))  # (per record: the largest component)
        e = np.where(both_nan | same_inf, 0.0, d / scale)
        e = np.where(np.isnan(e), 1e9, e)
        bad = (e > a.tol).any(axis=1)
        w = float(e.max())
        worst = max(worst, w)
        exact = float((x == y).all(axis=1).mean() + (both_nan.all(axis=1)).mean())
        if k == 0:
            nonfinite = float((~np.isfinite(A).all(axis=(1, 2)) | ~np.isfinite(B).all(axis=(1, 2))).mean())
            print('records compared 100.0%% (%.2f%% hold an infinity or NaN in one output)' % (100.0 * nonfinite))
        print('%-9s worst %.3e  records over tol %d of %d  bit-identical %.1f%%' % (names[k], w, int(bad.sum()), len(bad), 100 * exact))
        if bad.any():
            i = int(np.argmax(e.max(axis=1)))
            print('   worst record', i, 'ours', x[i], 'ref', y[i])
    print('RESULT', 'PASS' if worst <= a.tol else 'FAIL', 'worst', '%.3e' % worst)

if __name__ == '__main__':
    main()
