#!/usr/bin/env python3
"""DEV TOOL ONLY (player's or developer's machine; never used to produce a committed shader). Session 87 (stage 4): regenerates, from a
local game shader dump, vertex shaders as ucode_gen.py transcriptions, for a quick local look or as a reference to compare a hand-written
shader against. Such files come from the player's own game files, so, like the generated C++, they are never committed (CLAUDE.md rule 1;
the GENERATED set of vs_table_extra.py lists any that src/scene_vs_swap.h would register behind __has_include). It is empty now: all 66
shaders of the table are hand-written and tracked, and ROWS below is empty.

What it does, from a --wwe2k14_dump_shaders run's output (default out/s87/shader_dump: shader_<hash>.ucode.vert = the microcode listing, and
shader_<hash>_*_e00000002.vk.bin.vert = the SDK's native-mode translation, the reference):
  1. ucode_gen.py  ->  src/scene/<name>.vert (with a header comment)
  2. tools/post/build_shaders.ps1 -Name <name>  ->  src/scene/spirv/<name>_vert.h   (glslang)
  3. gen_header.py  ->  src/scene_vs_swap.h
  4. vs_check_all.py <names>  ->  every shader bit for bit against the translation (use --no-check to skip)
  py tools/shaders/harness/gen_local_vs.py [--dump DIR] [--only name ...] [--no-check]"""
import subprocess, sys, os, re, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
GEN = os.path.join(HERE, 'ucode_gen.py')
ap = argparse.ArgumentParser()
ap.add_argument('--dump', default=os.path.join(R, 'out', 's87', 'shader_dump'))
ap.add_argument('--only', nargs='*', default=[])
ap.add_argument('--no-check', action='store_true')
args = ap.parse_args()

ROWS = []   # (hash, file, mode, packed, effect, draws, pixel shaders, description, stride)

done = []
for h, f, mode, packed, eff, draws, ps, text, *rest in ROWS:
    stride = rest[0] if rest else 8
    if args.only and f not in args.only:
        continue
    src = os.path.join(args.dump, 'shader_%s.ucode.vert' % h)
    if not os.path.exists(src):
        print('MISSING', f, src); continue
    out = os.path.join(R, 'src', 'scene', f + '.vert')
    cmd = [sys.executable, GEN, src, out, '--stride', str(stride), '--fc', '95', '--mode', mode, '--finite', '1']
    if packed: cmd.append('--packed')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: print(f, r.stderr); continue
    s = open(out, encoding='utf-8').read().split(chr(10))
    nl = chr(10)
    hdr = ["// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's %s (guest vertex shader %s; %s draws a frame in" % (eff, h, draws),
           "// Brock's entrance / Bryan's entrance / the match). Pixel side: %s. Generated on this machine from the game's own shader dump by" % ps,
           "// tools/shaders/harness/gen_local_vs.py (ucode_gen.py): never committed. Every instruction in program order with the translation's multiply rule",
           "// (AvMulX where the translation keeps 0 x infinity = 0, the dot products fused or not as it does), checked bit for bit against the translation",
           "// by tools/shaders/harness/vs_harness.py (all outputs, every record, bounded and wild data).",
           "//", "// " + text, "// Registers r0.. are the guest's; o0.. are its exports (the interpolators, location = index); the vertex index is r0.x."]
    s = s[:3] + hdr + s[3:]
    open(out, 'w', encoding='utf-8').write(nl.join(s))
    print('wrote', f)
    r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(R, 'tools', 'post', 'build_shaders.ps1'), '-Name', f], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-300:])
    if r.returncode: continue
    done.append(f)
subprocess.run([sys.executable, os.path.join(HERE, 'gen_header.py')], check=True)
if done and not args.no_check:
    r = subprocess.run([sys.executable, os.path.join(HERE, 'vs_check_all.py')] + done)
    sys.exit(r.returncode)
