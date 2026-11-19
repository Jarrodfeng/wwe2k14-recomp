#!/usr/bin/env python3
"""S103 I2 review F1: with the row-rig switch off, UiQuads must add exactly the programs it always did (rdr_uiquad, rdr_uiquad_flat), in that order, so no player builds an extra shader module at boot and no
later program id shifts. Every AddProgram of src/renderer_scene/ui_quads.cpp that names another program must sit inside `if (rdr_scene::q2d::rig::g_on)`.  py -I tools/scene/uiquads_programs_lint.py [--root DIR]"""
import os, re, sys
root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
src = open(os.path.join(root, 'src', 'renderer_scene', 'ui_quads.cpp'), encoding='utf-8').read()
ctor = src[src.index('UiQuads::UiQuads('):src.index('bool UiQuads::Build')]
# strip the guarded block(s)
unguarded = re.sub(r'if \(rdr_scene::q2d::rig::g_on\) \{.*?\n  \}\n', '', ctor, flags=re.S)
names = re.findall(r'program\.name = "(\w+)";', unguarded)
adds = len(re.findall(r'AddProgram\(', unguarded))
# S103 P3c review: rdr_uiquad_flat is built with the PLAIN vertex program (kSpv_rdr_uiquad_vert), whatever the matrix block above it left in the shared program struct
before_flat = ctor[:ctor.index('program.name = "rdr_uiquad_flat";')]
last_vs = re.findall(r'program\.vertex_spirv = (\w+);', before_flat)
flat_vs_ok = bool(last_vs) and last_vs[-1] == 'kSpv_rdr_uiquad_vert'
ok = names == ['rdr_uiquad', 'rdr_uiquad_flat'] and adds == 2 and 'rdr_uiquad_hsl' in ctor and 'rdr_uiglyph' in ctor and flat_vs_ok
print(('PASS' if ok else 'FAIL') + '  uiquads_programs: unguarded programs %s, AddProgram calls %d (want rdr_uiquad, rdr_uiquad_flat, 2; the hue and glyph programs only under the rig switch); rdr_uiquad_flat vertex program %s (want kSpv_rdr_uiquad_vert)' % (names, adds, last_vs[-1] if last_vs else None))
sys.exit(0 if ok else 1)
