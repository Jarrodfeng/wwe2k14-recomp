r"""S102 lane L1 (review F7): per-pixel ALU cost of the cube function, counted from the optimised SPIR-V (0 game runs, 0 Surface runs).
  py -I tools\l1\l1_opcount.py
Compiles (1) the game's bake program as the per-pixel cost it would have if a reader evaluated it as written (yPreLight.frag, FX_SSS, with its normalise and the 12 raw dots) and (2) `fx_prelight_eval`
(the precomputed form) with glslangValidator, runs spirv-opt -O, disassembles, and counts per instruction the lanes of its result type: arithmetic lanes, of which transcendental (sqrt / rsqrt / divide)
lanes. Texture reads, loads, constructs and extracts are free. Prints both and the ratio. The count is a lane count (a vec4 op is 4), the unit the UHD 620 executes in (SIMD lanes), not a time."""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SDK = sorted(os.listdir('C:/VulkanSDK'))[-1]
BIN = os.path.join('C:/VulkanSDK', SDK, 'Bin')
FX = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
SOURCE = os.path.join(ROOT, 'src', 'renderer', 'shaders')
NEW = '''#version 450
#extension GL_GOOGLE_include_directive : require
#define FX_PRELIGHT_ANALYTIC
#include "fx_common.glsl"
layout(set = 0, binding = 1, std140) uniform FragmentConstants { vec4 PreLightK[18]; } fc;
#include "fx_prelight.glsl"
layout(location = 0) in vec3 inN;
layout(location = 0) out vec4 outColor;
void main() { outColor = fx_prelight_eval(inN); }
'''
FREE = {'OpLoad', 'OpStore', 'OpAccessChain', 'OpCompositeConstruct', 'OpCompositeExtract', 'OpVectorShuffle', 'OpLabel', 'OpBranch', 'OpBranchConditional', 'OpSelectionMerge', 'OpReturn', 'OpFunction',
        'OpFunctionEnd', 'OpVariable', 'OpPhi', 'OpImageSampleImplicitLod', 'OpConvertSToF', 'OpCopyObject'}
TRANS = {'OpFDiv', 'Sqrt', 'InverseSqrt', 'Pow', 'Exp', 'Log', 'OpExtInst:Sqrt', 'OpExtInst:InverseSqrt'}


def lanes_of(type_name):
    m = re.match(r'%v(\d)', type_name)
    return int(m.group(1)) if m else 1


def count(spv_dis):
    arith = trans = 0
    kinds = {}
    types = {}
    for ln in spv_dis.splitlines():
        m = re.match(r'\s*(%\S+) = Op\w+ (%\S+)', ln)
        if m:
            types[m.group(1)] = m.group(2)
    for ln in spv_dis.splitlines():
        m = re.match(r'\s*%\S+ = (Op\w+) (%\S+)(.*)', ln)
        if not m:
            continue
        op, ty, rest = m.groups()
        if op in FREE or op.startswith('OpType') or op.startswith('OpConstant') or op in ('OpUndef', 'OpDecorate'):
            continue
        lanes = lanes_of(ty)
        if op == 'OpDot':
            m2 = re.search(r'(%\S+) (%\S+)', rest)
            lanes = lanes_of(types.get(m2.group(1), '%float')) if m2 else 1   # a dot of n lanes costs n lane ops (a mul and n - 1 fma)
        name = op
        if op == 'OpExtInst':
            ext = re.search(r'\d+\s+(\w+)', rest)
            name = 'ext:' + (ext.group(1) if ext else '?')
        kinds[name] = kinds.get(name, 0) + lanes
        arith += lanes
        if op == 'OpFDiv' or name in ('ext:Sqrt', 'ext:InverseSqrt', 'ext:Pow'):
            trans += lanes
    return arith, trans, kinds


def compile_one(src_text, defines, work, name):
    path = os.path.join(work, name + '.frag')
    open(path, 'w').write(src_text)
    spv = os.path.join(work, name + '.spv')
    r = subprocess.run([os.path.join(BIN, 'glslangValidator.exe'), '-V', '--target-env', 'vulkan1.1', '-I' + SOURCE, '-I' + FX] + ['-D' + d for d in defines] + ['-o', spv, path], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('glslang %s: %s' % (name, (r.stdout + r.stderr)[-500:]))
    opt = os.path.join(work, name + '.opt.spv')
    subprocess.run([os.path.join(BIN, 'spirv-opt.exe'), '-O', spv, '-o', opt], check=True, capture_output=True)
    return subprocess.run([os.path.join(BIN, 'spirv-dis.exe'), opt], capture_output=True, text=True, check=True).stdout


def main():
    work = tempfile.mkdtemp(prefix='l1_opcount_')
    old_src = open(os.path.join(FX, 'yPreLight.frag'), encoding='utf-8').read()
    old = compile_one(old_src, ['FX_SSS'], work, 'old')
    new = compile_one(NEW, [], work, 'new')
    res = {}
    for label, dis in (('the bake program as written (normalise + 12 raw dots + the curve): yPreLight.frag FX_SSS', old), ('fx_prelight_eval, constants precomputed on the CPU, unit n given', new)):
        arith, trans, kinds = count(dis)
        res[label] = (arith, trans)
        top = sorted(kinds.items(), key=lambda kv: -kv[1])[:8]
        print('%s\n    lane ops %d (transcendental: sqrt / rsqrt / divide lanes %d); biggest: %s' % (label, arith, trans, ', '.join('%s %d' % kv for kv in top)))
    (a0, t0), (a1, t1) = res.values()
    print('ratio: %d -> %d lane ops (%.0f%%), transcendental %d -> %d' % (a0, a1, 100.0 * a1 / max(1, a0), t0, t1))
    return 0 if a1 < a0 else 1


if __name__ == '__main__':
    sys.exit(main())
