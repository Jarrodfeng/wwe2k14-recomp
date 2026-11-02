"""Offline pass tests (Session 82, harness builder): run a compute shader on the GPU with recorded inputs and compare its output with
the recorded real output, without booting the game. A builder writing a new compute pass (a blur, a reduction, a merged pass)
iterates the shader here in seconds and launches the game only for the finished piece's owned_check.

    py -I tools\\native\\compute_test\\compute_test.py SPEC.json [--keep]
    py -I tools\\native\\compute_test\\compute_test.py --self-test          (proves the runner end to end: a box blur vs numpy)

SPEC (JSON; relative paths are relative to the spec's folder):
  {"shader": "blur.comp" | "x.spv",        GLSL compute (compiled with glslc) or SPIR-V
   "groups": [gx, gy, gz],                 dispatch size
   "push": "push.bin",                     optional push constant bytes (<= 128)
   "bindings": [
     {"binding": 0, "kind": "tex", "format": "rgba8", "w": 1280, "h": 720, "in": "input.bin"},
     {"binding": 1, "kind": "img", "format": "rgba8", "w": 1280, "h": 720, "in": null,
      "expect": "real_output.bin", "tol": 1}]}
  kind: tex (sampled, nearest / clamp), img (storage image), ssbo / ubo (w = bytes); formats: see run_compute.cpp.
  A binding with "expect" is read back and compared: max / mean absolute difference (8-bit counts for unorm formats, absolute for
  floats), the share of values over "tol" (default 1 for 8-bit, 0.001 for floats, 0 for integers), and the worst position. PASS when no
  value is over tol. The inputs come from tools\\native\\compute_test\\rdc_dump_pass.py (a RenderDoc capture of the real pass).
Output (run_compute.exe built by build.ps1 on first use): the GPU, the dispatch time, one line per compared binding, PASS / FAIL.
"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
EXE = ROOT / 'out' / 'native' / 'compute_test' / 'run_compute.exe'
WORK = ROOT / 'out' / 'native' / 'compute_test' / 'work'
BPP = {'rgba8': 4, 'bgra8': 4, 'r8': 1, 'rg8': 2, 'r16f': 2, 'rg16f': 4, 'rgba16f': 8, 'r32f': 4, 'rg32f': 8, 'rgba32f': 16,
       'r32u': 4, 'rgba16s': 8}
DTYPE = {'rgba8': np.uint8, 'bgra8': np.uint8, 'r8': np.uint8, 'rg8': np.uint8, 'r16f': np.float16, 'rg16f': np.float16,
         'rgba16f': np.float16, 'r32f': np.float32, 'rg32f': np.float32, 'rgba32f': np.float32, 'r32u': np.uint32,
         'rgba16s': np.int16}
CHANNELS = {'rgba8': 4, 'bgra8': 4, 'r8': 1, 'rg8': 2, 'r16f': 1, 'rg16f': 2, 'rgba16f': 4, 'r32f': 1, 'rg32f': 2, 'rgba32f': 4,
            'r32u': 1, 'rgba16s': 4}


def ensure_exe():
    if not EXE.exists() or EXE.stat().st_mtime < (ROOT / 'tools' / 'native' / 'compute_test' / 'run_compute.cpp').stat().st_mtime:
        r = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'native' / 'compute_test' / 'build.ps1')],
                           capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode or not EXE.exists():
            sys.exit('compute_test: building run_compute.exe failed\n' + r.stderr)


def compile_shader(path):
    if path.suffix.lower() == '.spv':
        return path
    spv = WORK / (path.stem + '.spv')
    r = subprocess.run(['glslc', '-fshader-stage=compute', '--target-env=vulkan1.1', '-O', str(path), '-o', str(spv)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('compute_test: glslc failed:\n' + r.stdout + r.stderr)
    return spv


def run(spec_path, keep=False):
    ensure_exe()
    spec_path = Path(spec_path).resolve()
    base = spec_path.parent
    spec = json.loads(spec_path.read_text())
    WORK.mkdir(parents=True, exist_ok=True)
    spv = compile_shader((base / spec['shader']).resolve())
    cmd = [str(EXE), '--spv', str(spv), '--groups'] + [str(g) for g in spec['groups']]
    if spec.get('push'):
        cmd += ['--push', str((base / spec['push']).resolve())]
    outs = {}
    for b in spec['bindings']:
        fmt = b.get('format', 'rgba8')
        infile = '-' if not b.get('in') else str((base / b['in']).resolve())
        cmd += ['--bind', f"{b['binding']}:{b['kind']}:{fmt}:{b['w']}:{b.get('h', 1)}:{infile}"]
        if b.get('expect') or b.get('out'):
            outs[b['binding']] = WORK / f"{spec_path.stem}_out{b['binding']}.bin"
            cmd += ['--out', f"{b['binding']}:{outs[b['binding']]}"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print(r.stderr.strip())
        print('FAIL (the runner failed)')
        return 1
    bad = 0
    for b in spec['bindings']:
        if not b.get('expect'):
            continue
        fmt = b.get('format', 'rgba8')
        w, h = b['w'], b.get('h', 1)
        got = np.fromfile(outs[b['binding']], DTYPE.get(fmt, np.uint8))
        exp = np.fromfile((base / b['expect']).resolve(), DTYPE.get(fmt, np.uint8))
        if got.size != exp.size:
            print(f"  binding {b['binding']}: size differs ({got.size} vs expected {exp.size} values) FAIL")
            bad += 1
            continue
        tol = b.get('tol', 1 if DTYPE.get(fmt) == np.uint8 else (0 if DTYPE.get(fmt) in (np.uint32, np.int16) else 0.001))
        d = np.abs(got.astype(np.float64) - exp.astype(np.float64))
        ch = CHANNELS.get(fmt, 1)
        over = d > tol
        worst = int(np.argmax(d)) if d.size else 0
        px = worst // ch
        print(f"  binding {b['binding']} ({fmt} {w}x{h}): max diff {d.max():.4g}, mean {d.mean():.4g}, "
              f"{100.0 * over.mean():.3f}% of values over tol {tol}; worst at x {px % w} y {px // w} channel {worst % ch}: "
              f"got {got[worst]} expected {exp[worst]} -> {'FAIL' if over.any() else 'ok'}")
        bad += int(over.any())
    print('FAIL' if bad else 'PASS')
    return 1 if bad else 0


SELF_SHADER = """#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D src;
layout(binding = 1, rgba8) uniform writeonly image2D dst;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  ivec2 sz = textureSize(src, 0);
  if (p.x >= sz.x || p.y >= sz.y) return;
  vec4 s = vec4(0.0);
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) s += texelFetch(src, clamp(p + ivec2(x, y), ivec2(0), sz - 1), 0);
  imageStore(dst, p, s / 9.0);
}
"""


def self_test():
    W, H = 100, 70   # not a multiple of the group size: the bounds check matters
    WORK.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(7)
    img = rng.integers(0, 256, (H, W, 4), dtype=np.uint8)
    (WORK / 'self_in.bin').write_bytes(img.tobytes())
    f = img.astype(np.float64) / 255.0
    pad = np.pad(f, ((1, 1), (1, 1), (0, 0)), mode='edge')
    acc = sum(pad[1 + dy:1 + dy + H, 1 + dx:1 + dx + W] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9.0
    (WORK / 'self_expect.bin').write_bytes(np.round(acc * 255.0).astype(np.uint8).tobytes())
    (WORK / 'self.comp').write_text(SELF_SHADER)
    (WORK / 'self.json').write_text(json.dumps({
        'shader': 'self.comp', 'groups': [(W + 7) // 8, (H + 7) // 8, 1],
        'bindings': [{'binding': 0, 'kind': 'tex', 'format': 'rgba8', 'w': W, 'h': H, 'in': 'self_in.bin'},
                     {'binding': 1, 'kind': 'img', 'format': 'rgba8', 'w': W, 'h': H, 'in': None, 'expect': 'self_expect.bin', 'tol': 1}]}))
    return run(WORK / 'self.json')


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    sys.exit(self_test() if a[0] == '--self-test' else run(a[0], '--keep' in a))
