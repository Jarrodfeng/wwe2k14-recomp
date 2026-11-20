#!/usr/bin/env python3
r"""WGS review item 11: a planted-fault switch is set by the offline tools only. `Plants` structs (rf::c::Plants, stage_look::Plants) are compiled into the game, so a WRITE to a
member outside tools\ (`PlantsRef().x = `, `g_plants.x = `) could switch a fault on for players. This lint fails on any such write in our code
outside tools\ (src\, probes\, android\ ...). Reads and comparisons (`==`) are fine.
  py -I tools\scene\plants_write_lint.py [--root DIR]
Exit 0 = none; 1 = a write outside tools\ (each printed with file:line).
"""
# entry: main
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
# PlantsRef() (rf::c, stage_look) and g_plants (rf_c_core.cpp's copy). rf::Plants() (WrestlerPlants) is left out on purpose: start_settings.cpp:209 sets its guest_flat from the
# +dev-only cvar wwe2k14_rdr_guest_flat (`dev && ...`), a dev switch, not a planted fault.
WRITE = re.compile(r'(?:PlantsRef\(\)|\bg_plants)\s*\.\s*\w+\s*=(?!=)')
SKIP_DIRS = {'tools', 'thirdparty', 'generated', 'out', 'game_data', 'logs', '.git', '.claude', 'node_modules'}
EXTS = ('.cpp', '.h', '.hpp', '.cc', '.inl')


def scan(root):
    bad = []
    for dp, dns, fns in os.walk(root):
        rel = os.path.relpath(dp, root)
        top = rel.split(os.sep)[0]
        if top in SKIP_DIRS:
            dns[:] = []
            continue
        dns[:] = [d for d in dns if d not in SKIP_DIRS or rel != '.']
        for fn in fns:
            if not fn.endswith(EXTS):
                continue
            p = os.path.join(dp, fn)
            try:
                with open(p, encoding='utf-8', errors='replace') as f:
                    for n, ln in enumerate(f, 1):
                        code = ln.split('//')[0]
                        if WRITE.search(code):
                            bad.append('%s:%d: %s' % (os.path.relpath(p, root), n, ln.strip()[:160]))
            except OSError:
                pass
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=ROOT)
    a = ap.parse_args(argv)
    bad = scan(a.root)
    for b in bad:
        print('FAIL a Plants member is written outside tools\\ (the game compiles it): ' + b)
    print('plants_write_lint: %s (%d write(s) outside tools\\)' % ('PASS' if not bad else 'FAIL', len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
