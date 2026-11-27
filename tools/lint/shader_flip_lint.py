#!/usr/bin/env python3
r"""S111 SHADERFLIP: every own vertex shader that writes gl_Position applies the renderer's clip-to-screen step (the y flip), or says in a named comment why not.

  py -I tools\lint\shader_flip_lint.py [--root DIR] [--selftest]

Why: tools\renderer\compile_shaders.py adds the step to the game's translated effect shaders (wrapped_source: gl_Position.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz *
gl_Position.w, ndc_scale.y = -1) but compile_plain, which compiles our own shaders (src\renderer\shaders\*.vert, not fx\), adds nothing: each own shader must apply it itself.
rdr_flash.vert forgot, and every camera flash was drawn mirrored for four releases (docs\research\s111\FLASHCHAIN.md).
The rule, per own .vert (src\renderer\shaders\*.vert, fx\ is the wrapped set and is not read): every `gl_Position = ...;` statement holds `ndc_scale` (the push-constant block
member the renderer fills with the flip; the expression the others use is `clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w`). A shader whose positions are already in
the target's own screen space (a pass that must not be flipped) carries a line `// FLIP-EXEMPT: <reason>` (a reason of 12+ characters). An exempt shader that DOES apply the flip is
a finding too (a stale exemption). src\scene\*.vert are the viewer's set (tools\shaders\build_viewer_spv.py, xenos_vs.glsl applies ndc_scale_*), not compile_plain's.
Every run also feeds planted twins (synthetic, and the real tree with the flip cut out of rdr_uiquad.vert / the reason cut out of an exemption) and requires each to fire.
Exit 0 = pass, 1 = a finding (or a twin that did not fire), 2 = shader folder missing. 0 game runs."""
import argparse
import glob
import os
import re
import sys

SHADERS = 'src/renderer/shaders'
POS = re.compile(r'\bgl_Position\s*=[^;]*;')
EXEMPT = re.compile(r'//\s*FLIP-EXEMPT:[ \t]*(?P<why>[^\n]*)')


def blank_comments(t):
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/', blank, t, flags=re.S)


def judge(files):
    """(findings, [(path, state)]) for {path: text}; state is 'flips', 'exempt' or 'no gl_Position'."""
    bad, table = [], []
    for path in sorted(files):
        text = files[path]
        stmts = POS.findall(blank_comments(text))
        if not stmts:
            table.append((path, 'no gl_Position'))
            continue
        flips = all('ndc_scale' in s for s in stmts)
        ex = EXEMPT.search(text)
        if ex:
            why = ex.group('why').strip()
            if len(why) < 12:
                bad.append('%s: the FLIP-EXEMPT comment gives no reason ("%s"): say why the flip must not touch this shader' % (path, why))
            elif flips:
                bad.append('%s: marked FLIP-EXEMPT but it applies ndc_scale: drop the stale exemption' % path)
            table.append((path, 'exempt'))
        elif not flips:
            bad.append('%s: writes gl_Position without the renderer flip (no ndc_scale in the statement): apply `clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w` '
                       '(see rdr_uiquad.vert), or add `// FLIP-EXEMPT: <why>` (compile_plain adds no flip to our own shaders; rdr_flash.vert was drawn mirrored for 4 releases)' % path)
            table.append((path, 'MISSING'))
        else:
            table.append((path, 'flips'))
    return bad, table


FLIPS = ('layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; } fx_sys;\n'
         'void main() {\n  vec4 clip = vec4(1.0);\n  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);\n}\n')
NOFLIP = 'void main() {\n  vec4 clip = vec4(1.0);\n  gl_Position = vec4(dot(vc.m[0], clip), dot(vc.m[1], clip), dot(vc.m[2], clip), dot(vc.m[3], clip));\n}\n'
GOOD = {'a_flips.vert': FLIPS,
        'b_comment_only.vert': '// ndc_scale is mentioned here only\n' + FLIPS,
        'c_exempt.vert': '// FLIP-EXEMPT: positions are already in the swapchain\'s own space\n' + NOFLIP,
        'd_none.vert': 'void main() {}\n'}
TWINS = [
    ('the flip removed', {'a.vert': NOFLIP}),
    ('the flip only named in a comment', {'a.vert': '// uses ndc_scale\n' + NOFLIP}),
    ('one of two statements without the flip', {'a.vert': FLIPS.replace('}\n', '  gl_Position = vec4(0.0);\n}\n')}),
    ('an exemption with no reason', {'a.vert': '// FLIP-EXEMPT:\n' + NOFLIP}),
    ('an exemption with a thin reason', {'a.vert': '// FLIP-EXEMPT: ok\n' + NOFLIP}),
    ('a stale exemption on a shader that flips', {'a.vert': '// FLIP-EXEMPT: screen space already, no flip wanted\n' + FLIPS}),
]
REAL_PLANTS = [   # (file, text to cut, replacement): the real own shader changed so that it must fire
    ('rdr_uiquad.vert', 'clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w', 'clip.xyz'),
    ('rdr_plane_mask.vert', 'clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w', 'clip.xyz'),
]


def selftest(files):
    out = []
    bad, _t = judge(GOOD)
    if bad:
        out.append('selftest: the good synthetic set fails: %s' % bad)
    for name, fs in TWINS:
        if not judge(fs)[0]:
            out.append('selftest: the planted twin "%s" did not fire' % name)
    for nm, old, new in REAL_PLANTS:
        p = SHADERS + '/' + nm
        if p not in files or old not in files[p]:
            out.append('selftest: real-tree plant: %s does not hold the flip text (the shader moved: update REAL_PLANTS)' % p)
            continue
        planted = dict(files)
        planted[p] = files[p].replace(old, new, 1)
        if not judge(planted)[0]:
            out.append('selftest: real-tree plant (flip cut out of %s) did not fire' % nm)
    # an exemption whose reason is cut, on a real exempt shader
    for p in sorted(files):
        m = EXEMPT.search(files[p])
        if m:
            planted = dict(files)
            planted[p] = files[p].replace(m.group(0), '// FLIP-EXEMPT:', 1)
            if not judge(planted)[0]:
                out.append('selftest: real-tree plant (reason cut from the exemption of %s) did not fire' % p)
            break
    return out


def read_shaders(root):
    files = {}
    for p in sorted(glob.glob(os.path.join(root, SHADERS, '*.vert'))):
        with open(p, encoding='utf-8', errors='replace') as f:
            files[os.path.relpath(p, root).replace('\\', '/')] = f.read()
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true', help='only the planted twins')
    ap.add_argument('--table', action='store_true', help='print every shader and its state')
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(a.root, SHADERS)):
        print('shader_flip_lint: no %s under %s' % (SHADERS, a.root))
        return 2
    files = read_shaders(a.root)
    st = selftest(files)
    for s in st:
        print('FAIL ' + s)
    if a.selftest:
        print('shader_flip_lint selftest: %s' % ('FAIL' if st else 'every twin fires (%d synthetic, %d + 1 on the real tree)' % (len(TWINS), len(REAL_PLANTS))))
        return 1 if st else 0
    bad, table = judge(files)
    for b in bad:
        print('FAIL ' + b)
    if a.table:
        for p, s in table:
            print('%-8s %s' % (s, p))
    n_ex = sum(1 for _p, s in table if s == 'exempt')
    print('shader_flip_lint: %s (%d own .vert, %d exempt; %d synthetic + %d real-tree twins, %d did not fire)' % ('FAIL (%d)' % (len(bad) + len(st)) if bad or st else 'pass',
                                                                                                         len(files), n_ex, len(TWINS), len(REAL_PLANTS) + 1, len(st)))
    return 1 if bad or st else 0


if __name__ == '__main__':
    sys.exit(main())
