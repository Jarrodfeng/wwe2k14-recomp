#!/usr/bin/env python3
"""Session 87 (stage 4): writes the boilerplate of one of our arena vertex shaders (src\\scene\\arena_vs_*.vert): the
includes, the float constants block with a macro per register in packed (ascending register) order, and the interpolator
declarations. The body (main) and the header comment are written by hand and passed in as files. Our own tool.
  mk_arena_vs.py <out.vert> <comment.txt> <body.txt> <regs: comma list> <outs: comma list of names> [--tex]
"""
import sys

out, comment, body, regs, outs = sys.argv[1:6]
tex = '--tex' in sys.argv
shadow = '--shadow' in sys.argv
bare = '--bare' in sys.argv
math = '--math' in sys.argv
skin = '--skin' in sys.argv
regs = [int(r) for r in regs.split(',') if r]
assert regs == sorted(regs), 'registers must be ascending (the packing order)'
lines = ['#version 450', '#extension GL_GOOGLE_include_directive : require', '#include "xenos_vs.glsl"']
if tex:
    lines.append('#include "xenos_vs_tex.glsl"')
lines.append(open(comment, encoding='utf-8').read().rstrip('\n'))
lines.append('')
if regs:
    lines.append('layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[%d]; } fc;' % len(regs))
for i, r in enumerate(regs):
    lines.append('#define C%-4d fc.c[%d]' % (r, i))
if math:
    lines.append('#include "xenos_vs_math.glsl"')
if skin:
    lines.append('#include "xenos_vs_skin.glsl"')
if not bare:
    lines.append('#include "arena_vs.glsl"')
if shadow:
    lines.append('#include "arena_vs_shadow.glsl"')
lines.append('')
for i, name in enumerate(outs.split(',')):
    lines.append('layout(location = %d) invariant out vec4 %s;' % (i, name))
lines.append('')
lines.append(open(body, encoding='utf-8').read().rstrip('\n'))
lines.append('')
open(out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
print(out)
