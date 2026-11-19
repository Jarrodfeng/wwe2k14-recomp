#!/usr/bin/env python3
"""posed_vb_lint (S103 PF2 review): a model's own vertex buffer `vb` is read ONLY through FrontEnd::PosedVertexBuffer.

  py -I tools\\scene\\posed_vb_lint.py [--root .]     scan src\\renderer_scene\\front_end.cpp (exit 1 on an error)
  py -I tools\\scene\\posed_vb_lint.py --selftest     the planted twin: a draw binding `m.vb` directly MUST be flagged

Why: with the direct path (docs\\research\\s103\\RCP.md F2) a re-posed model's latest pose lives in a host-visible copy and `vb` is STALE (FrontEnd::ModelData::pcopy_cur). A draw that binds `m.vb` straight draws an old
pose. Rule: in front_end.cpp a line that assigns `vertex[` may name `m.vb` / `->vb` only as the argument of PosedVertexBuffer(...), i.e. through it.
"""
import argparse, os, re, sys

BAD = re.compile(r'vertex\[\s*\d\s*\]\s*=\s*\{[^}]*\b(?:m|mod|model)(?:\.|->)vb\b')
OK_LINE = re.compile(r'PosedVertexBuffer\s*\(')


def scan(text, name='front_end.cpp'):
    errs = []
    for i, line in enumerate(text.splitlines(), 1):
        if BAD.search(line) and not OK_LINE.search(line):
            errs.append('%s:%d: a draw binds the model\'s `vb` directly; use PosedVertexBuffer(m) (vb is stale while pcopy_cur >= 0)' % (name, i))
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        bad = '    op.vertex[0] = {m.vb, me.vb_offset};\n'
        good = '    op.vertex[0] = {PosedVertexBuffer(m), me.vb_offset};\n'
        ok = len(scan(bad)) == 1 and not scan(good)
        print('selftest: %s' % ('PASS (the planted direct bind is flagged, the guarded one passes)' if ok else 'FAIL'))
        return 0 if ok else 1
    p = os.path.join(a.root, 'src', 'renderer_scene', 'front_end.cpp')
    errs = scan(open(p, encoding='utf8', errors='replace').read(), 'src/renderer_scene/front_end.cpp')
    for e in errs:
        print(e)
    print('%s: %d errors' % ('FAIL' if errs else 'PASS', len(errs)))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
