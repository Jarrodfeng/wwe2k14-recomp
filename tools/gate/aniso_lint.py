r"""S103 B1 lint: the hard-coded 8x anisotropy can't come back (review item 7).

  py -I tools\gate\aniso_lint.py [--root .] [--selftest]

No SamplerDesc in src\renderer_scene sets max_anisotropy to a constant above 1 outside sampler_table.h (the player's TEXTURE FILTERING reaches a sampler only through sampler_table::AnisoWanted()).
Also: front_end.cpp keeps no per-draw counter on the sampler table (the removed sampler_table::Count()). --selftest plants both faults in a copy of the text; each must be caught."""
import argparse
import os
import re
import sys

CONST = re.compile(r'max_anisotropy\s*=\s*(?:uint8_t\()?\s*(\d+)')


def scan(rel, text):
    bad = []
    for n, ln in enumerate(text.splitlines(), 1):
        code = ln.split('//')[0]
        m = CONST.search(code)
        if m and int(m.group(1)) > 1 and not rel.endswith('sampler_table.h'):
            bad.append('%s:%d: max_anisotropy set to the constant %s (use sampler_table::AnisoWanted())' % (rel, n, m.group(1)))
        if 'sampler_table::Count' in code or 'st::Count()' in code:
            bad.append('%s:%d: a per-draw counter on the sampler table (nothing reads it)' % (rel, n))
    return bad


def tree(root):
    out, n = [], 0
    d = os.path.join(root, 'src', 'renderer_scene')
    for fn in sorted(os.listdir(d)):
        if fn.endswith(('.cpp', '.h')):
            n += 1
            out += scan('src/renderer_scene/' + fn, open(os.path.join(d, fn), encoding='utf-8', errors='replace').read())
    return out, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    bad, n = tree(a.root)
    if not n:
        print('FAIL: no source files read')
        return 1
    for b in bad:
        print('  FAIL', b)
    rc = 1 if bad else 0
    if a.selftest:
        for name, text in (('const8', 'sd.max_anisotropy = 8;\n'), ('const16', 'x.max_anisotropy = uint8_t(16);\n'), ('counter', '++st::Count().table_hits;\n')):
            caught = bool(scan('src/renderer_scene/front_end.cpp', text))
            print('twin %-8s %s' % (name, 'CAUGHT' if caught else 'NOT CAUGHT'))
            rc |= 0 if caught else 1
        if scan('src/renderer_scene/sampler_table.h', 'sd.max_anisotropy = 8;\n'):
            print('FAIL: sampler_table.h is exempt')
            rc = 1
        if scan('x.cpp', 'sd.max_anisotropy = 1;\nsd.max_anisotropy = sampler_table::AnisoWanted();\n'):
            print('FAIL: a 1x / AnisoWanted line was flagged')
            rc = 1
    print('%s  aniso_lint: %d files in src\\renderer_scene' % ('PASS' if rc == 0 else 'FAIL', n))
    return rc


if __name__ == '__main__':
    sys.exit(main())
