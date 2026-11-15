r"""S102 lane P3 (review F5, F9): two lints for the owned-pass work. Offline, no game.

  py -I tools\shadow\own_pass_helpers_lint.py [--root DIR]

1. HELPERS. src\renderer_adapter\binder.h reaches the builder with `builder.SetTargets(` / `builder.Copy(` only through IssueTargets / IssueCopy (the walk and every owned pass share them: the alias
   elide, EndAliasOf, post_last_targets_, bind_last_targets_, the gallery, SH2's rule are in ONE place) and a short allowlist of other planners' restores. Every such call line must carry a marker
   `helper:IssueTargets` / `helper:IssueCopy` (the helpers) or `allow:post` / `allow:bake` / `allow:d3` or `allow:last_picture` (S102 C3: the picture saved aside between two renderer images; the restores and the mini frames); an unmarked call fails. The IssueCopy helper must call
   EndAliasOf( right before its builder.Copy. Planted twins (each must FAIL): a bare builder.Copy( added to PlaneRun; the EndAliasOf call removed from the helper.
2. CLAIMS. A lane's gate json claim with side "gpu" on a switch named own_* must cite an `ours-vs-game` arm in its basis (owning a pass re-issues the same GPU work: the claim is about 0 unless an ours-vs-game
   measurement says otherwise; the swap-thread claim is a separate row). Planted twin: such a claim without the citation must FAIL.
Exit 1 on any FAIL."""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CALL = re.compile(r'builder\.(SetTargets|Copy)\(')
MARK = re.compile(r'(helper:(IssueTargets|IssueCopy)|allow:(post|bake|d3|last_picture))')
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


def claim_problems(lanes_dir):
    out = []
    for f in sorted(glob.glob(os.path.join(lanes_dir, '*.json'))):
        try:
            data = json.load(open(f, encoding='utf-8'))
        except Exception as e:
            out.append('%s: unreadable (%s)' % (os.path.basename(f), e))
            continue
        c = data.get('claim')
        for k in (c if isinstance(c, list) else [c] if c else []):
            if isinstance(k, dict) and 'own_' in str(k.get('switch', '')) and 'gpu' in str(k.get('side', '')).lower() and 'ours-vs-game' not in str(k.get('basis', '')):
                out.append('%s: a gpu claim on %s must cite an ours-vs-game arm in its basis' % (os.path.basename(f), k.get('switch')))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    a = ap.parse_args()
    binder = os.path.join(a.root, 'src', 'renderer_adapter', 'binder.h')
    if not os.path.isfile(binder):
        line(False, 'cannot read ' + binder + ' (a lint that reads nothing must fail)')
        return 1
    text = open(binder, encoding='utf-8', errors='replace').read()
    n_calls = sum(1 for l in text.splitlines() if CALL.search(l))
    probs = helper_problems(text)
    line(not probs and n_calls >= 10, 'helpers: %d builder.SetTargets / builder.Copy call lines in binder.h, every one marked helper: / allow:%s' % (n_calls, ('; ' + '; '.join(probs[:3])) if probs else ''))
    plant1 = text.replace('        IssueCopy(frame, p.copy_op, capture, builder, stats);\n      }', '        IssueCopy(frame, p.copy_op, capture, builder, stats);\n        builder.Copy(rdr::CopyOp{});\n      }', 1)
    if plant1 == text:
        plant1 = text.replace('\r\n      }\r\n', '\r\n        builder.Copy(rdr::CopyOp{});\r\n      }\r\n', 1)
    line(plant1 != text and bool(helper_problems(plant1)), 'planted twin (a bare builder.Copy( in PlaneRun): the lint FAILS it')
    plant2 = re.sub(r'\n[^\n]*EndAliasOf\(copy\.dest\.image\)[^\n]*', '', text, count=1)
    line(plant2 != text and bool(helper_problems(plant2)), 'planted twin (EndAliasOf removed from IssueCopy): the lint FAILS it')
    lanes = os.path.join(a.root, 'tools', 'gate', 'lanes')
    cp = claim_problems(lanes)
    line(not cp, 'claims: every gpu claim on an own_* switch cites an ours-vs-game arm%s' % (('; ' + '; '.join(cp[:3])) if cp else ''))
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        json.dump({'claim': {'scene': 'brock', 'side': 'gpu', 'ms': [0.2, 1.0], 'switch': 'wwe2k14_rdr_own_x', 'basis': 'an estimate'}, 'checks': []}, open(os.path.join(d, 'x.json'), 'w'))
        line(bool(claim_problems(d)), 'planted twin (a gpu claim on an own_ switch without an ours-vs-game citation): the lint FAILS it')
    print('own_pass_helpers_lint: %s' % ('PASS' if not FAILS else '%d FAIL' % FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
