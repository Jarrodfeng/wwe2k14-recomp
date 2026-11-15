r"""S102 lane L1b: lane-op cost of the light-cube compute shaders (0 game runs, 0 Surface runs), counted from the optimised SPIR-V the way tools\l1\l1_opcount.py counts L1's per-pixel function.
  py -I tools\l1b\l1b_opcount.py
Compiles src\renderer\shaders\rdr_light_cubes.comp (the per-frame dispatch) and rdr_light_dirs.comp (the direction table, made once and again only when the mapping changes) with glslangValidator,
spirv-opt -O, disassembles, and counts per instruction the lanes of its result type (a vec4 op is 4 lanes; constructs, extracts and the imageStore are free). Review F6: MEMORY lanes are counted too
(loads of uniform / image data: an OpLoad of a vec4 is 4, an OpImageRead 4), and the optimised module is checked for arrays kept in Function / Private memory (a dynamically indexed private array is
scratch on a Gen9 compiler): the per-frame shader must have none. One invocation = one texel. Prints the lane ops a texel costs, the dispatch total (5 cubes x 6 faces x 64 x 64 = 122,880 texels) and
the Surface estimate against the work it removes (the raster pre-light pass's 267 lane ops per pixel, the bake's cost on the UHD 620). The UHD 620 does about 190 G lane-ops/s at full MAD rate
(PRELIGHT_DIRECT.md 8.6 F7); the estimate gives full rate and half rate. Exit 1 when a shader does not compile or the per-frame shader keeps a private array."""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'l1'))
import l1_opcount as oc  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SHADERS = os.path.join(ROOT, 'src', 'renderer', 'shaders')
TEXELS = 5 * 6 * 64 * 64
L1_OPS, L1_PIXELS = 237, 321664
RASTER_OPS = 267   # the bake program per pixel as the strip pass shades it (L1's F7 measurement: normalise, 12 raw dots, the curve)
RASTER_PIXELS = 5 * 6 * 64 * 64 * 0 + 5 * 384 * 64   # the five strips' pixels (the raster pass shades 384 x 64 each)
RATE = 190e9


def compile_opt(name, work, define=None):
    spv = os.path.join(work, name + '.spv')
    cmd = [os.path.join(oc.BIN, 'glslangValidator.exe'), '-V', '--target-env', 'vulkan1.1', '-I' + oc.SOURCE, '-I' + oc.FX] + (['-D' + define] if define else []) + ['-o', spv, os.path.join(SHADERS, name)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('glslang %s: %s' % (name, (r.stdout + r.stderr)[-600:]))
    opt = os.path.join(work, name + '.opt.spv')
    subprocess.run([os.path.join(oc.BIN, 'spirv-opt.exe'), '-O', spv, '-o', opt], check=True, capture_output=True)
    return subprocess.run([os.path.join(oc.BIN, 'spirv-dis.exe'), opt], capture_output=True, text=True, check=True).stdout


def memory_lanes(dis):
    """lanes of OpLoad results and OpImageRead results (the data the shader reads from memory), and the OpVariables of array type in Function / Private storage"""
    lanes = 0
    for ln in dis.splitlines():
        m = re.match(r'\s*%\S+ = (OpLoad|OpImageRead) (%\S+)', ln)
        if m:
            lanes += oc.lanes_of(m.group(2))
    types = {}
    for ln in dis.splitlines():
        m = re.match(r'\s*(%\S+) = OpTypeArray', ln)
        if m:
            types[m.group(1)] = True
    ptr_array = {}
    for ln in dis.splitlines():
        m = re.match(r'\s*(%\S+) = OpTypePointer (Function|Private) (%\S+)', ln)
        if m and m.group(3) in types:
            ptr_array[m.group(1)] = m.group(2)
    arrays = 0
    for ln in dis.splitlines():
        m = re.match(r'\s*%\S+ = OpVariable (%\S+) (Function|Private)', ln)
        if m and m.group(1) in ptr_array:
            arrays += 1
    return lanes, arrays


def main():
    work = tempfile.mkdtemp(prefix='l1b_opcount_')
    try:
        main_dis = compile_opt('rdr_light_cubes.comp', work)
        dirs_dis = compile_opt('rdr_light_dirs.comp', work)
    except RuntimeError as error:
        print('FAIL ' + str(error))
        return 1
    a_main, t_main, kinds = oc.count(main_dis)
    a_dirs, t_dirs, _ = oc.count(dirs_dis)
    mem_main, arrays_main = memory_lanes(main_dis)
    mem_dirs, arrays_dirs = memory_lanes(dirs_dis)
    top = sorted(kinds.items(), key=lambda kv: -kv[1])[:8]
    total = a_main * TEXELS
    raster = RASTER_OPS * RASTER_PIXELS
    print('rdr_light_cubes.comp (every dispatch): %d ALU lane ops a texel (transcendental %d), %d memory lanes (loads / image reads), %d arrays in Function / Private memory; biggest: %s' % (
        a_main, t_main, mem_main, arrays_main, ', '.join('%s %d' % kv for kv in top)))
    print('rdr_light_dirs.comp (the direction table: first frame and when the mapping changes only): %d ALU lane ops a texel, %d memory lanes, %d private arrays' % (a_dirs, mem_dirs, arrays_dirs))
    print('dispatch: %d texels x %d = %.1f M lane ops a frame (+ %d memory lanes a texel); L1 per-pixel path: %d x %d = %.1f M; the raster pre-light pass it replaces: %d ops x %d px = %.1f M' % (
        TEXELS, a_main, total / 1e6, mem_main, L1_OPS, L1_PIXELS, L1_OPS * L1_PIXELS / 1e6, RASTER_OPS, RASTER_PIXELS, raster / 1e6))
    print('UHD 620 estimate [E]: %.2f ms at full MAD rate, %.2f ms at half rate (190 G lane-ops/s) against the raster pass\'s %.2f / %.2f ms of ALU alone' % (total / RATE * 1e3, total / (RATE / 2) * 1e3, raster / RATE * 1e3, raster / (RATE / 2) * 1e3))
    print('RESULT lane_ops=%d dispatch_mops=%.2f est_ms_full=%.3f est_ms_half=%.3f mem_lanes=%d private_arrays=%d dirs_lane_ops=%d raster_mops=%.2f' % (a_main, total / 1e6, total / RATE * 1e3, total / (RATE / 2) * 1e3, mem_main, arrays_main, a_dirs, raster / 1e6))
    return 1 if arrays_main else 0


if __name__ == '__main__':
    sys.exit(main())
