#!/usr/bin/env python3
r"""S111 CDLINTS (WHOLEDIFF_PLAN A.6.2): a draw family's alpha test never comes from `Material::alpha_test` alone.

  py -I tools\lint\alpha_source_lint.py [--root DIR] [--selftest] [--table]

Why: Material::alpha_test is the jboy loader's texel rule (AlphaCutsOut: >= 8 % of the texels <= 32), a property of the TEXTURE. The game's alpha test is set per DRAW FAMILY by its pass / node
(pass_state.h rows with their game addresses; the crowd node sets it per family, sub_82519508). The crowd family drew its stands members tested because DrawInstanced read the material flag
(`ProgramFor(vs_twin, ps, mat.alpha_test)`); the fix (CROWDATEST, docs\research\s111\CROWDATEST.md) gave the caller a say (InstancedDraw::alpha_test) and moved the crowd off the flag.
The rule, over src\renderer_scene\*.cpp / *.h (comments masked): a SINK statement, i.e. a `ProgramFor(...)` call (the program, hence the variant's alpha twin, a draw runs) or an assignment
`x->alpha_test = ...` (a bind's test), that READS a material's flag (`mat.alpha_test`, `mat->alpha_test`, `materials[..].alpha_test`) must also show one of
  * a pass_state row (`pass_state::`),
  * a caller-stated test (`d.alpha_test` / `InstancedDraw`),
  * the game's rule function (`GameTestsAlpha(` / `YbgTestsAlpha(`: the game-derived decision that the flag is OR-ed with),
  * a cited game constant (`sub_82xxxxxx` or `0x8xxxxxxx`) on the statement's lines or the line above,
  * a `// ALPHA-SOURCE: <why>` comment (12+ characters) on a statement line or the line above (the loader's own rule used on purpose).
A statement that only READS the flag for something else (a log line, a effect-pick want, a table pick's argument) is not a sink and is not looked at. A flag that reaches a sink through a local
variable is not followed (the sink statement itself must be clean): the tip's `prog_tested` is built from `d.alpha_test` on its own line.
Every run also feeds planted twins (synthetic, the crowd block as it stood at the parent of CROWDATEST's merge, and the real tree with the crowd's caller rule cut out) and requires each to fire.
Known rows (tools\lint\alpha_source_known.txt: file|statement-text|note) are the sinks on the tree that read the flag alone and have no marker yet; a row with no matching sink is a finding.
Exit 0 = pass, 1 = a finding (or a twin that did not fire), 2 = src\renderer_scene missing. 0 game runs."""
import argparse
import os
import re
import sys

SCOPE = 'src/renderer_scene'
KNOWN = 'tools/lint/alpha_source_known.txt'
READ = re.compile(r'(?:\bmat\w*|\bmaterials\s*\[[^\]]*\]|\bmaterial\w*)\s*(?:\.|->)\s*alpha_test\b(?!\s*=[^=])')
SINK = re.compile(r'\bProgramFor\s*\(|\b\w+\s*->\s*alpha_test\s*=(?!=)')
OK_TEXT = re.compile(r'\bpass_state::|\bd\.alpha_test\b|\bInstancedDraw\b|\bGameTestsAlpha\s*\(|\bYbgTestsAlpha\s*\(')
GAME = re.compile(r'\bsub_82[0-9A-Fa-f]{6}\b|\b0x8[0-9A-Fa-f]{7}\b')
MARK = re.compile(r'ALPHA-SOURCE:[ \t]*(?P<why>[^\n]*)')


def mask(t):
    """comments and string literals blanked (newlines kept)."""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"', blank, t, flags=re.S)


def statements(code):
    """[(start_offset, end_offset)] of ';'-terminated statements at paren depth 0 (braces split too, so an `if (..) { stmt; }` yields its inner statement)."""
    out, start, d = [], 0, 0
    for i, ch in enumerate(code):
        if ch == '(':
            d += 1
        elif ch == ')':
            d = max(0, d - 1)
        elif d == 0 and ch in ';{}':
            if code[start:i].strip():
                out.append((start, i + 1))
            start = i + 1
    return out


def judge_text(path, text):
    """([(path, norm statement, message)], n_sinks) for one file."""
    code = mask(text)
    raw = text.split('\n')
    starts = [0]
    for ln in raw:
        starts.append(starts[-1] + len(ln) + 1)

    def line_of(off):
        lo, hi = 0, len(starts) - 1
        while lo < hi - 1:
            mid = (lo + hi) // 2
            if starts[mid] <= off:
                lo = mid
            else:
                hi = mid
        return lo + 1
    bad, sinks = [], 0
    for a, b in statements(code):
        st = code[a:b]
        if not SINK.search(st):
            continue
        sinks += 1
        if not READ.search(st):
            continue
        if OK_TEXT.search(st):
            continue
        l0, l1 = line_of(a + (len(st) - len(st.lstrip()))), line_of(b - 1)
        lines = raw[max(0, l0 - 2):l1]   # the statement's lines and the one above
        blob = '\n'.join(lines)
        if GAME.search(blob):
            continue
        mk = MARK.search(blob)
        if mk and len(mk.group('why').strip()) >= 12:
            continue
        norm = re.sub(r'\s+', ' ', st).strip()
        bad.append((path, norm, '%s (line %d): the draw\'s alpha test is read off the material flag alone: `%s`: take it from a pass_state row, the caller\'s InstancedDraw::alpha_test, or a game constant '
                                '(a sub_82 / 0x8 address on the line or above), or mark the loader rule on purpose with `// ALPHA-SOURCE: <why>` (CROWDATEST: the crowd drew tested by the material\'s flag)' % (path, l0, norm[:140])))
    return bad, sinks


def judge(files):
    bad, n = [], 0
    for p in sorted(files):
        b, s = judge_text(p, files[p])
        bad += b
        n += s
    return bad, n


def read_known(root):
    out = set()
    p = os.path.join(root, KNOWN)
    if os.path.isfile(p):
        with open(p, encoding='utf-8') as f:
            for ln in f:
                if ln.strip() and not ln.lstrip().startswith('#'):
                    c = ln.rstrip('\n').split('|')
                    if len(c) >= 2:
                        out.add((c[0], c[1]))
    return out


# ---- planted twins -----------------------------------------------------------------------------------------------------------------------------------------------------------
# the crowd block of DrawInstanced as it stood at 13ded28c9^1 (the parent of CROWDATEST's merge): the program and the bind's test from the material / bind flag alone
PARENT_CROWD = ('    rdr::DrawState state{};\n    state.program = ProgramFor(vs_twin, ps, mat.alpha_test);\n    state.vertex_layout = layout;\n')
TIP_CROWD = ('    const bool prog_tested = d.alpha_test < 0 ? mat.alpha_test : d.alpha_test != 0;\n    rdr::DrawState state{};\n    state.program = ProgramFor(vs_twin, ps, prog_tested);\n')
GOOD = {
    'a_tip_crowd.cpp': TIP_CROWD,
    'b_pass_state.cpp': '  b->alpha_test = mat.alpha_test && pass_state::Of(pcls).tested_blends;\n',
    'c_game_rule.cpp': '  b->alpha_test = (mat.alpha_test || GameTestsAlpha(mat, ps, ps, plant)) && fv.bytes_alpha != 0;\n',
    'd_game_address.cpp': '  // the game tests this family (sub_825185B0 card loop)\n  b->program = ProgramFor(vs, ps, mat.alpha_test);\n',
    'e_marker.cpp': '  // ALPHA-SOURCE: the wrestler effects pick their variant by the loader flag\n  b->alpha_test = mat.alpha_test;\n',
    'f_not_a_sink.cpp': '  cc = ChooseCharEffectTable(mat.shader, skinned, mat.alpha_test, mat.technique);\n  w.alpha_test = mat.alpha_test;\n  q.materials[i].alpha_test = true;\n',
    'g_other_source.cpp': '  state.program = ProgramFor(vs, ps, b->alpha_test);\n  b->program = ProgramFor(debug_vs_, debug_ps_, false);\n',
    'h_comment_only.cpp': '  // state.program = ProgramFor(vs_twin, ps, mat.alpha_test);\n  Other();\n',
}
TWINS = [
    ('the crowd block as it stood at the parent of CROWDATEST (ProgramFor reads mat.alpha_test)', {'a.cpp': PARENT_CROWD}),
    ('a bind\'s test assigned from the material flag alone', {'a.cpp': '  b->alpha_test = mat.alpha_test;\n'}),
    ('a program picked by the flag through a materials[] index', {'a.cpp': '  b->program = ProgramFor(vs, ps, q.materials[i].alpha_test);\n'}),
    ('a marker with a thin reason', {'a.cpp': '  // ALPHA-SOURCE: ok\n  b->alpha_test = mat.alpha_test;\n'}),
    ('a game address two lines above (not adjacent)', {'a.cpp': '  // sub_825185B0 card loop\n  Other();\n  b->alpha_test = mat.alpha_test;\n'}),
    ('a flag read in a multi-line ProgramFor call', {'a.cpp': '  state.program = ProgramFor(vs_twin,\n                             ps,\n                             mat.alpha_test);\n'}),
]


def selftest(files):
    out = []
    bad, _n = judge(GOOD)
    if bad:
        out.append('selftest: the good synthetic set fails: %s' % [b[2][:120] for b in bad])
    for name, fs in TWINS:
        if not judge(fs)[0]:
            out.append('selftest: the planted twin "%s" did not fire' % name)
    # real-tree plant: the crowd caller rule of DrawInstanced cut out, so the parent's shape is back
    p = SCOPE + '/front_end.cpp'
    old = 'const bool prog_tested = d.alpha_test < 0 ? mat.alpha_test : d.alpha_test != 0;'
    cut = 'state.program = ProgramFor(vs_twin, ps, prog_tested);'
    if p not in files or old not in files[p] or cut not in files[p]:
        out.append('selftest: real-tree plant: %s no longer holds the DrawInstanced caller rule (prog_tested): update the plant' % p)
    else:
        planted = files[p].replace(cut, 'state.program = ProgramFor(vs_twin, ps, mat.alpha_test);', 1)
        if not judge_text(p, planted)[0]:
            out.append('selftest: real-tree plant (DrawInstanced back to the material flag) did not fire')
    return out


def read_scope(root):
    files = {}
    d = os.path.join(root, SCOPE)
    for fn in sorted(os.listdir(d)):
        if fn.endswith(('.cpp', '.h', '.inl')):
            with open(os.path.join(d, fn), encoding='utf-8', errors='replace') as f:
                files[SCOPE + '/' + fn] = f.read()
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true', help='only the planted twins')
    ap.add_argument('--table', action='store_true', help='print every sink statement that reads the material flag alone')
    ap.add_argument('--list-known', action='store_true', help='print the current findings as baseline rows (file|statement|note)')
    ap.add_argument('--text', help='judge this one file (a path) as a src\\renderer_scene file: for the parent re-find')
    a = ap.parse_args()
    if a.text:
        with open(a.text, encoding='utf-8', errors='replace') as f:
            b, n = judge_text(os.path.basename(a.text), f.read())
        for x in b:
            print('FAIL ' + x[2])
        print('alpha_source_lint --text: %d sink statements, %d findings' % (n, len(b)))
        return 1 if b else 0
    if not os.path.isdir(os.path.join(a.root, SCOPE)):
        print('alpha_source_lint: no %s under %s' % (SCOPE, a.root))
        return 2
    files = read_scope(a.root)
    st = selftest(files)
    for s in st:
        print('FAIL ' + s)
    if a.selftest:
        print('alpha_source_lint selftest: %s' % ('FAIL' if st else 'every twin fires (%d synthetic, 1 on the real tree)' % len(TWINS)))
        return 1 if st else 0
    bad, n = judge(files)
    if a.list_known:
        for p, norm, _m in bad:
            print('%s|%s|unreviewed' % (p, norm))
        return 0
    known = read_known(a.root)
    new = [x for x in bad if (x[0], x[1]) not in known]
    stale = sorted(k for k in known if k not in {(x[0], x[1]) for x in bad})
    for x in new:
        print('FAIL ' + x[2])
    for k in stale:
        print('FAIL %s: baseline row `%s` no longer matches a finding: delete it from %s' % (k[0], k[1][:100], KNOWN))
    if a.table:
        for x in bad:
            print('flag-alone  %s: %s' % (x[0], x[1][:160]))
    nfail = len(new) + len(stale) + len(st)
    print('alpha_source_lint: %s (%d files, %d sink statements (ProgramFor / ->alpha_test =), %d read the material flag alone: %d in the baseline, %d new, %d stale; %d synthetic + 1 real-tree twins, %d did not fire)' % (
        'FAIL (%d)' % nfail if nfail else 'pass', len(files), n, len(bad), len(bad) - len(new), len(new), len(stale), len(TWINS), len(st)))
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
