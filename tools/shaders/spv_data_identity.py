"""S110 SPIRVDATA: the SPIR-V words of our own shaders did not change when the arrays moved out of the headers, and no header holds words again.

    py -I tools\\shaders\\spv_data_identity.py [--base REF]    compare every array of the headers at REF (default: main; the old `inline constexpr uint32_t NAME[] = {...}` form)
                                                            with the arrays of the <stem>_data.inc files on disk: names, counts and every word. Exit 1 on any difference.
                                                            Also the structure rules (a lint of the generated headers):
                                                              * a header only DECLARES:  extern const uint32_t NAME[COUNT];  with COUNT == the words of its _data.inc, no `inline constexpr`, no word;
                                                              * every _data.inc is included by exactly one data unit (scene_spirv_data_<k>.cpp / post_spirv_data.cpp), after its header.
    py -I tools\\shaders\\spv_data_identity.py --selftest      the planted cases: a one-word change in a data unit, a header that holds words again, an inc no unit includes (each MUST be caught); then the real tree.
"""
import os, re, subprocess, sys

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIRS = ('src/scene/spirv', 'src/post/spirv')
HEX = re.compile(r'0x[0-9A-Fa-f]{8}')
OLD_ARR = re.compile(r'inline constexpr uint32_t (\w+)\[\] = \{')
NEW_DECL = re.compile(r'extern const uint32_t (\w+)\[(\d+)\];')
INC_ARR = re.compile(r'const uint32_t (\w+)\[(\d+)\] = \{')


def git(*a):
    return subprocess.run(['git', '-C', R] + list(a), capture_output=True, check=True).stdout


def old_arrays(base):
    """{stem: (name, words)} of the headers at base."""
    out = {}
    for d in DIRS:
        for p in git('ls-tree', '-r', '--name-only', base, '--', d).decode().split():
            if not p.endswith('.h'):
                continue
            t = git('show', base + ':' + p).decode('utf-8')
            m = OLD_ARR.search(t)
            if not m:
                continue  # (not an array header)
            out[p] = (m.group(1), [int(x, 16) for x in HEX.findall(t[m.end():])])
    return out


def new_arrays(root, planted=None):
    """{header path: (name, count in the header, words of the inc, problems)} of the tree on disk."""
    out = {}
    for d in DIRS:
        full = os.path.join(root, d)
        for f in sorted(os.listdir(full)):
            if not f.endswith('.h'):
                continue
            p = d + '/' + f
            h = open(os.path.join(full, f), encoding='utf-8').read()
            probs = []
            if 'inline constexpr' in h or HEX.search(h) or '= {' in h:
                probs.append('the header holds a definition / words')
            m = NEW_DECL.search(h)
            if not m:
                probs.append('no extern declaration')
            incp = os.path.join(full, f[:-2] + '_data.inc')
            if not os.path.exists(incp):
                out[p] = (m.group(1) if m else '?', int(m.group(2)) if m else -1, None, probs + ['no _data.inc'])
                continue
            t = open(incp, encoding='utf-8').read()
            if planted and p in planted:
                t = planted[p](t)
            im = INC_ARR.search(t)
            words = [int(x, 16) for x in HEX.findall(t[im.end():])] if im else []
            if not im or (m and (im.group(1) != m.group(1) or int(im.group(2)) != int(m.group(2)))):
                probs.append('the inc defines %s[%s], the header declares %s[%s]' % (im.group(1) if im else '?', im.group(2) if im else '?', m.group(1) if m else '?', m.group(2) if m else '?'))
            out[p] = (m.group(1) if m else '?', int(m.group(2)) if m else -1, words, probs)
    return out


def unit_problems(root):
    probs = []
    for d in DIRS:
        full = os.path.join(root, d)
        units = [f for f in os.listdir(full) if re.match(r'(scene|post)_spirv_data(_\d+)?\.cpp$', f)]
        text = {u: open(os.path.join(full, u), encoding='utf-8').read() for u in units}
        for f in sorted(os.listdir(full)):
            if not f.endswith('_data.inc'):
                continue
            stem = f[:-len('_data.inc')]
            hits = [u for u in units if '#include "%s_data.inc"' % f[:-len('_data.inc')] in text[u]]
            if len(hits) != 1:
                probs.append('%s/%s is included by %d data units (want 1)' % (d, f, len(hits)))
            elif text[hits[0]].find('#include "%s.h"' % stem) < 0 or text[hits[0]].find('#include "%s.h"' % stem) > text[hits[0]].find('#include "%s"' % f):
                probs.append('%s/%s: the unit does not include the header before the inc' % (d, f))
    return probs


def judge(base, root, planted=None, quiet=False):
    old = old_arrays(base)
    new = new_arrays(root, planted)
    diffs, compared = [], 0
    for p, (name, words) in sorted(old.items()):
        if p not in new:
            diffs.append('%s: in %s, not in the tree' % (p, base))
            continue
        nname, cnt, nwords, probs = new[p]
        compared += 1
        if nname != name:
            diffs.append('%s: array name %s -> %s' % (p, name, nname))
        if nwords is None or nwords != words:
            where = next((i for i in range(min(len(words), len(nwords or []))) if words[i] != nwords[i]), min(len(words), len(nwords or [])))
            diffs.append('%s: words differ (old %d, new %d, first difference at word %d)' % (p, len(words), len(nwords or []), where))
        if cnt != len(words):
            diffs.append('%s: declared count %d, old words %d' % (p, cnt, len(words)))
    for p, (name, cnt, nwords, probs) in sorted(new.items()):
        for q in probs:
            diffs.append('%s: %s' % (p, q))
    diffs += unit_problems(root)
    if not quiet:
        for x in diffs[:40]:
            print('DIFF', x)
        print('spv_data_identity: %d arrays compared against %s (%d headers on disk), %d difference(s)' % (compared, base, len(new), len(diffs)))
    return compared, diffs


def selftest():
    base = 'main'
    bad = 0
    n, d = judge(base, R, quiet=True)
    one = sorted(new_arrays(R))[3]
    n2, d2 = judge(base, R, planted={one: lambda t: re.sub(r'(= \{\s*0x)([0-9A-F])', lambda m: m.group(1) + ('0' if m.group(2) != '0' else '1'), t, 1)}, quiet=True)
    # (the planted change turns the first word's first hex digit into another value: the same header, the same count, ONE word different)
    ok = any('words differ' in x and one in x for x in d2)
    print('selftest planted one-word change in a data unit is caught:', 'PASS' if ok else 'FAIL'); bad += not ok
    ok = not d
    print('selftest the real tree: %d arrays, %d difference(s):' % (n, len(d)), 'PASS' if ok else 'FAIL'); bad += not ok
    for x in d[:10]:
        print('   ', x)
    # a header that holds words again
    hp = os.path.join(R, one)
    saved = open(hp, encoding='utf-8', newline='').read()
    try:
        open(hp, 'w', encoding='utf-8', newline='').write(saved + 'inline constexpr uint32_t x_planted[] = {0x07230203};\r\n')
        n3, d3 = judge(base, R, quiet=True)
        ok = any('holds a definition' in x for x in d3)
        print('selftest a header holding words again is caught:', 'PASS' if ok else 'FAIL'); bad += not ok
    finally:
        open(hp, 'w', encoding='utf-8', newline='').write(saved)
    # an inc no unit includes
    stray = os.path.join(R, os.path.dirname(one), 'zz_planted_data.inc')
    strayh = os.path.join(R, os.path.dirname(one), 'zz_planted.h')
    try:
        open(stray, 'w').write('const uint32_t zz_planted_spv[1] = {\r\n    0x07230203,\r\n};\r\n')
        open(strayh, 'w').write('#pragma once\r\nextern const uint32_t zz_planted_spv[1];\r\n')
        n4, d4 = judge(base, R, quiet=True)
        ok = any('zz_planted_data.inc is included by 0' in x for x in d4)
        print('selftest an inc that no data unit includes is caught:', 'PASS' if ok else 'FAIL'); bad += not ok
    finally:
        for p in (stray, strayh):
            if os.path.exists(p):
                os.remove(p)
    return bad


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(1 if selftest() else 0)
    base = sys.argv[sys.argv.index('--base') + 1] if '--base' in sys.argv else 'main'
    n, d = judge(base, R)
    sys.exit(1 if d else 0)
