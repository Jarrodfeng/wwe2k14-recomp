#!/usr/bin/env python3
r"""images_rebind_lint (S103 UPL review item 5): a front-end model's texture-slot image changes (`m.images[i] = ...`) only with the bound materials rebuilt, because a bound material holds the image id it was bound with.

  py -I tools\scene\images_rebind_lint.py [--root .]      scan src\renderer_scene\front_end.cpp (exit 1 on an error)
  py -I tools\scene\images_rebind_lint.py --selftest      the planted twins: each bad snippet MUST be flagged, each good one must pass

Rule: every assignment `<expr>.images[<idx>] = <value>` (not `==`) must have, on the same line or within the next 6 lines, a call to `ResetModelBindings(` (the one helper: bound reset, rule_bound / rule_pick clear,
kept draw lists released), or carry a `// no-rebind: why` marker (a dead model that nothing draws again) or a `// rebind-after: where` marker (a batch that rebuilds once at its end). A call to the helper in a LATER branch counts only inside that window.
"""
import argparse, os, re, sys

ASSIGN = re.compile(r'\.images\[[^\]]*\]\s*=(?!=)')
WINDOW = 6


def scan(text):
    lines = text.splitlines()
    errs = []
    for i, ln in enumerate(lines):
        code = ln.split('//')[0]
        if not ASSIGN.search(code):
            continue
        if 'no-rebind:' in ln or 'rebind-after:' in ln:
            continue
        if any('ResetModelBindings(' in l.split('//')[0] for l in lines[i:i + WINDOW + 1]):
            continue
        errs.append('line %d: `%s` changes a texture-slot image with no ResetModelBindings() within %d lines (or a `// no-rebind: why` marker)' % (i + 1, ln.strip()[:110], WINDOW))
    return errs


def selftest():
    bad = ['  m.images[t] = img;\n  foo();\n', '  m.images[a] = b;\n' + '  x;\n' * 9 + '  ResetModelBindings(m, 0);\n']
    good = ['  m.images[t] = img;\n  ResetModelBindings(m, model);\n', '  m.images[t] = kNoImage;   // no-rebind: the model is dead\n', '  if (m.images[t] == img) return;\n']
    ok = True
    for b in bad:
        flagged = bool(scan(b))
        print('%s  planted twin flagged: %r' % ('ok  ' if flagged else 'FAIL', b.splitlines()[0].strip()))
        ok = ok and flagged
    for g in good:
        clean = not scan(g)
        print('%s  good snippet passes: %r' % ('ok  ' if clean else 'FAIL', g.splitlines()[0].strip()))
        ok = ok and clean
    print('PASS: every planted twin behaved' if ok else 'FAIL: selftest')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    path = os.path.join(a.root, 'src', 'renderer_scene', 'front_end.cpp')
    errs = scan(open(path, encoding='utf-8', errors='replace').read())
    for e in errs:
        print('front_end.cpp:' + e)
    print(('PASS: every texture-slot image change rebuilds the bound materials' if not errs else 'FAIL: %d error(s)' % len(errs)))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
