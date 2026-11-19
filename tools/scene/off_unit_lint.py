#!/usr/bin/env python3
"""S103 CW1 review fix 1: a unit that claims OFF-build (stream-free) behaviour must be built with -DWWE2K14_KEEP_STREAM=0.
Rule: every tools\\**\\*.cpp that carries the marker `OFF-BUILD UNIT` (first 5 lines) must
  (a) contain `static_assert(!WWE2K14_STREAM()` (so a build without the define cannot compile it), and
  (b) be named, with -DWWE2K14_KEEP_STREAM=0 on the same compile line, in a build script / check under tools\\ (a .py or .ps1 that mentions the file and the define).
  py -I tools\\scene\\off_unit_lint.py [--root DIR] [--selftest]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import sys


def units(root):
    for d, _, fs in os.walk(os.path.join(root, 'tools')):
        for f in fs:
            if f.endswith('.cpp'):
                p = os.path.join(d, f)
                try:
                    head = ''.join(open(p, encoding='utf-8', errors='replace').readlines()[:5])
                except OSError:
                    continue
                if 'OFF-BUILD UNIT' in head:
                    yield p


def check(root, files=None):
    bad = []
    scripts = []
    for d, _, fs in os.walk(os.path.join(root, 'tools')):
        for f in fs:
            if f.endswith(('.py', '.ps1')):
                p = os.path.join(d, f)
                try:
                    scripts.append(open(p, encoding='utf-8', errors='replace').read())
                except OSError:
                    pass
    for p in (files if files is not None else list(units(root))):
        src = open(p, encoding='utf-8', errors='replace').read()
        name = os.path.basename(p)
        if 'static_assert(!WWE2K14_STREAM()' not in src:
            bad.append('%s: an OFF-BUILD UNIT must static_assert(!WWE2K14_STREAM())' % name)
        if not any(name in s and '-DWWE2K14_KEEP_STREAM=0' in s for s in scripts):
            bad.append('%s: no build script under tools\\ compiles it with -DWWE2K14_KEEP_STREAM=0' % name)
    return bad


def selftest(root):
    import tempfile
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, 'tools'))
        u = os.path.join(t, 'tools', 'x_unit.cpp')
        open(u, 'w').write('// OFF-BUILD UNIT\nint main(){}\n')
        ok = bool(check(t))   # no static_assert, no builder: must fail
        open(u, 'w').write('// OFF-BUILD UNIT\nstatic_assert(!WWE2K14_STREAM(), "x");\n')
        open(os.path.join(t, 'tools', 'b.py'), 'w').write("x_unit.cpp built with '-DWWE2K14_KEEP_STREAM=0'")
        ok = ok and not check(t)
        open(os.path.join(t, 'tools', 'b.py'), 'w').write("x_unit.cpp built with nothing")
        ok = ok and bool(check(t))   # the builder lacks the define: must fail
    print('selftest: %s' % ('PASS' if ok else 'FAIL'))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.selftest and not selftest(root):
        return 1
    bad = check(root)
    for b in bad:
        print('FAIL: ' + b)
    print('off_unit_lint: %s (%d OFF-build unit(s))' % ('PASS' if not bad else 'FAIL', len(list(units(root)))))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
