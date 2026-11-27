r"""S102 lane P3 (review F5, F9): the lint for the owned-pass work. Offline, no game.

  py -I tools\shadow\own_pass_helpers_lint.py [--root DIR]

src\renderer_adapter\binder.h reaches the builder with `builder.SetTargets(` / `builder.Copy(` only through IssueTargets / IssueCopy (the walk and every owned pass share them: the alias
   elide, EndAliasOf, post_last_targets_, bind_last_targets_, the gallery, SH2's rule are in ONE place) and a short allowlist of other planners' restores. Every such call line must carry a marker
   `helper:IssueTargets` / `helper:IssueCopy` (the helpers) or `allow:post` / `allow:bake` / `allow:d3` or `allow:last_picture` (S102 C3: the picture saved aside between two renderer images; the restores and the mini frames); an unmarked call fails. The IssueCopy helper must call
   EndAliasOf( right before its builder.Copy. Planted twins (each must FAIL): a bare builder.Copy( added to PlaneRun; the EndAliasOf call removed from the helper.
Exit 1 on any FAIL."""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CALL = re.compile(r'builder\.(SetTargets|Copy)\(')
MARK = re.compile(r'(helper:(IssueTargets|IssueCopy)|allow:(post|bake|d3|last_picture|row2d))')
FAILS = 0


def line(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def helper_problems(text):
    """-> list of problems in binder.h's text."""
    out = []
    lines = text.splitlines()
    for i, l in enumerate(lines):
        if CALL.search(l) and not MARK.search(l):
            out.append('line %d: unmarked %s' % (i + 1, l.strip()[:80]))
        if 'helper:IssueCopy' in l:
            window = '\n'.join(lines[max(0, i - 4):i])
            if 'EndAliasOf(' not in window:
                out.append('line %d: IssueCopy does not call EndAliasOf( before its builder.Copy' % (i + 1))
    if not any('helper:IssueTargets' in l for l in lines) or not any('helper:IssueCopy' in l for l in lines):
        out.append('the helpers are missing')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    a = ap.parse_args()
    binder = os.path.join(a.root, 'src', 'renderer_adapter', 'binder.h')
    if not os.path.isfile(binder):
        line(False, 'cannot read ' + binder + ' (a lint that reads nothing must fail)')
        return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # binder.h is split: read it with its parts expanded (tools/split/read_unit.py)
    text = read_path(binder)
    n_calls = sum(1 for l in text.splitlines() if CALL.search(l))
    probs = helper_problems(text)
    line(not probs and n_calls >= 10, 'helpers: %d builder.SetTargets / builder.Copy call lines in binder.h, every one marked helper: / allow:%s' % (n_calls, ('; ' + '; '.join(probs[:3])) if probs else ''))
    plant1 = text.replace('        IssueCopy(frame, p.copy_op, capture, builder, stats);\n      }', '        IssueCopy(frame, p.copy_op, capture, builder, stats);\n        builder.Copy(rdr::CopyOp{});\n      }', 1)
    if plant1 == text:
        plant1 = text.replace('\r\n      }\r\n', '\r\n        builder.Copy(rdr::CopyOp{});\r\n      }\r\n', 1)
    line(plant1 != text and bool(helper_problems(plant1)), 'planted twin (a bare builder.Copy( in PlaneRun): the lint FAILS it')
    plant2 = re.sub(r'\n[^\n]*EndAliasOf\(copy\.dest\.image\)[^\n]*', '', text, count=1)
    line(plant2 != text and bool(helper_problems(plant2)), 'planted twin (EndAliasOf removed from IssueCopy): the lint FAILS it')
    print('own_pass_helpers_lint: %s' % ('PASS' if not FAILS else '%d FAIL' % FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
