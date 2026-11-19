#!/usr/bin/env python3
"""Selftest for NEEDS-MAIN / gate_main / needs_main_owed (S103 WTG + the review's two HIGH holes). py -I tools\\gate\\wtg_selftest.py
Temp trees only (no game, no GPU, no junction or link of any kind). Plants:
  1 a root_needs row in a tree that lacks it -> NEEDS-MAIN at once, exit 4 (never 0), a needs_main_<sha>.json record
  2 the same row under --assume-main (gate-main) -> NEEDS-MAIN is impossible; a row dying on FileNotFoundError is a plain FAIL, exit 1; gate_main's final_exit is never 0 for it
  3 an UNDECLARED reader that dies on FileNotFoundError with nothing else printed -> NEEDS-MAIN, the original FAIL tail rides on the row
  4 PLANT (review HIGH 2): an assertion failure after a fallback "No such file" line stays FAIL
  5 PLANT: a half-present folder (generated/default exists, the file does not) stays FAIL
  6 gate_main.final_exit: rows that did not run / MAIN-ONLY / skipped -> 3 "row still needs main", never 0; the MAIN-ONLY filter matches names exactly (a.b is not a.bc)
  7 needs_main_owed: owed = record - passed - acked; MAIN-ONLY rows stay owed until acked"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gate_main as gm   # noqa: E402
import needs_main_owed as owedmod   # noqa: E402

PY = sys.executable
FAILS = []


def check(ok, text):
    print(('ok    ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def tree(base, name, checks, files=None):
    root = Path(base) / name
    (root / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
    (root / 'tools' / 'gate' / 'lanes' / 'w.json').write_text(json.dumps({'checks': checks}), encoding='utf-8')
    for fn, body in (files or {}).items():
        (root / fn).parent.mkdir(parents=True, exist_ok=True)
        (root / fn).write_text(body, encoding='utf-8')
    return root


def gate(root, *extra):
    p = subprocess.run([PY, '-I', str(HERE / 'gate.py'), '--root', str(root), '--out', str(root / 'gout'), '--no-cache', '--no-toolcache', '--no-exes', '--jobs', '1'] + list(extra),
                       capture_output=True, text=True, timeout=300, env=dict(__import__('os').environ, GATE_NO_RESTORE='1', GATE_NO_SHARE='1'))
    return p.returncode, p.stdout + p.stderr


READER = "open('generated/default/codegen.partition.json')\n"
ASSERTER = ("print(\"FileNotFoundError: [Errno 2] No such file or directory: 'generated/default/codegen.partition.json' (fallback, ignored)\")\n"
            "print('FAIL: the real verdict: 3 != 4')\nraise SystemExit(1)\n")


def main():
    with tempfile.TemporaryDirectory() as td:
        # 1
        r = tree(td, 't1', [{'name': 'w.declared', 'cmd': [PY, '-I', 'r.py'], 'timeout_s': 60, 'root_needs': ['generated/default']}], {'r.py': READER})
        rc, out = gate(r)
        check(rc == 4 and 'NEEDS-MAIN' in out and 'needs-main: w.declared' in out, '1 root_needs row -> NEEDS-MAIN, exit 4 (green with needs-main, not 0)')
        check(any((r / 'gout').glob('needs_main_*.json')), '1 the owed record needs_main_<sha>.json is written')
        # 2
        rc, out = gate(r, '--assume-main')
        check(rc == 1 and 'NEEDS-MAIN' not in out and 'FileNotFoundError' in out, '2 --assume-main: the row runs and a missing file is a plain FAIL (exit 1)')
        check(gm.final_exit(rc, [], ['w.declared'], {}, {})[0] != 0, '2 gate_main never exits 0 for that failure')
        # 3
        r = tree(td, 't3', [{'name': 'w.undeclared', 'cmd': [PY, '-I', 'r.py'], 'timeout_s': 60}], {'r.py': READER})
        rc, out = gate(r)
        check(rc == 4 and 'NEEDS-MAIN' in out and 'original FAIL' in out, '3 undeclared reader dying on FileNotFoundError -> NEEDS-MAIN with the original FAIL tail beside it')
        # 4
        r = tree(td, 't4', [{'name': 'w.assert', 'cmd': [PY, '-I', 'r.py'], 'timeout_s': 60}], {'r.py': ASSERTER})
        rc, out = gate(r)
        check(rc == 1 and 'NEEDS-MAIN' not in out, '4 PLANT: an assertion failure after a fallback "No such file" line stays FAIL')
        # 5
        r = tree(td, 't5', [{'name': 'w.half', 'cmd': [PY, '-I', 'r.py'], 'timeout_s': 60}], {'r.py': READER})
        (r / 'generated' / 'default').mkdir(parents=True)
        rc, out = gate(r)
        check(rc == 1 and 'NEEDS-MAIN' not in out, '5 PLANT: a half-present folder stays FAIL')
    # 6
    code, lines = gm.final_exit(0, ['a'], ['a', 'b'], {}, {})
    check(code == 3 and any('row still needs main: b' in l for l in lines), '6 a requested row with no result is "row still needs main", exit 3')
    code, lines = gm.final_exit(0, ['a'], ['a'], {'c': 'SKIP: missing x'}, {})
    check(code == 3 and any('row still needs main: c' in l for l in lines), '6 a SKIPped row is owed, exit 3')
    code, lines = gm.final_exit(0, ['a'], ['a'], {}, {'m': 'needs out/build'})
    check(code == 3 and any('MAIN-ONLY' in l and 'AFTER the merge' in l for l in lines), '6 a MAIN-ONLY row is owed, said plainly, exit 3')
    check(gm.final_exit(0, ['a'], ['a'], {}, {})[0] == 0, '6 every row ran and passed -> 0')
    check(gm.final_exit(1, ['a'], ['a'], {}, {})[0] == 1 and gm.final_exit(2, [], ['a'], {}, {})[0] == 2, '6 failed / timed out keep their codes')
    checks = [{'name': 'a.b', 'lane': 'a', 'cmd': ['x']}, {'name': 'a.bc', 'lane': 'a2', 'cmd': ['pwsh', '-File', 'tools/build.ps1']}]
    sel, unk, mo = gm.split_rows(checks, ['a.b', 'a.bc'])
    check(set(mo) == {'a.bc'} and not unk, '6 the MAIN-ONLY filter is an exact-name set (a.b is not a.bc)')
    # 7
    with tempfile.TemporaryDirectory() as od:
        sha = 'a' * 40
        Path(od, 'needs_main_%s.json' % sha[:12]).write_text(json.dumps({'sha': sha, 'rows': ['r1', 'r2', 'm1'], 'when': 'x'}))
        rec, owe, _p, _a = owedmod.owed(od, [sha])
        check(owe == ['m1', 'r1', 'r2'], '7 all rows are owed with no pass')
        Path(od, 'needs_main_pass_%s.json' % sha[:12]).write_text(json.dumps({'rows': ['r1', 'r2'], 'main_only': ['m1']}))
        rec, owe, _p, _a = owedmod.owed(od, [sha])
        check(owe == ['m1'], '7 the passed rows are cleared, the MAIN-ONLY row stays owed')
        Path(od, 'needs_main_ack_%s.json' % sha[:12]).write_text(json.dumps({'rows': ['m1']}))
        rec, owe, _p, _a = owedmod.owed(od, [sha])
        check(owe == [], '7 acked after the merge -> nothing owed')
    print('wtg_selftest: %s' % ('PASS' if not FAILS else '%d FAILED' % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
