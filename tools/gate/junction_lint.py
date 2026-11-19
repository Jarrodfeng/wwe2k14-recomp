#!/usr/bin/env python3
"""junction_lint (S103 WTG review): no code under tools\\ creates a junction or symlink, and none removes a worktree, except the allow-listed tools that already do.

  py -I tools\\gate\\junction_lint.py [--root DIR] [--selftest]

Why: `git worktree remove --force` FOLLOWS a junction into the real folder and deletes its contents (CLAUDE.md Safety). Anything that makes links inside a checkout, or removes
worktrees, is a hazard the gate tools must not add. The allow-lists below are the tools that did so before this lint (each is reviewed where it lives: wt_build.ps1 makes
its links only while a build runs and removes them in a finally; the tidy refuses to delete through a link). A NEW file on either list is an error: add it deliberately,
with a reason, or do not make the link. Exit 1 on any error."""
import argparse
import re
import sys
import tempfile
from pathlib import Path

LINK_RE = re.compile(r'mklink|os\.symlink|CreateJunction|symlink_to|ItemType\s+\(?[\'"]?(Junction|SymbolicLink)|New-Junction|\bln\s+-s\b')
WT_RE = re.compile(r'worktree[\'",\s]+remove|worktree\s+remove')

LINK_ALLOW = {
    'tools/bisect/wt_build.ps1': 'links only while a build runs, removed in a finally (its own header)',
    'tools/harness/tidy.py': 'the deleter: knows and refuses links',
    'tools/harness/tidy_k1_selftest.py': 'plants links to prove the tidy refuses them',
    'tools/harness/tidy_selftest.py': 'plants links to prove the tidy refuses them',
    'tools/owned/fast_build.ps1': 'SDK worktree builds (existing)',
    'tools/release/pgo_build.ps1': 'existing',
    'tools/release/prove_build_flags.ps1': 'existing',
    'tools/sdk_rebase/build_scratch_sdk.ps1': 'existing',
    'tools/surface/rplay_remote.ps1': 'existing (remote kit)',
}
WT_ALLOW = {
    'tools/assets/mcp_asset_test.py': 'existing', 'tools/bisect/autobisect.py': 'existing', 'tools/bisect/wt_build.ps1': 'the bisect clean-up (-Clean)',
    'tools/harness/tidy.py': 'the tidy (guards links first)', 'tools/harness/tidy_selftest.py': 'selftest', 'tools/harness/train.py': 'existing',
    'tools/live/mcp_server.py': 'existing', 'tools/owned/fast_build.ps1': 'existing', 'tools/sdk_rebase/build_scratch_sdk.ps1': 'existing', 'tools/sdk_rebase/sdk_rebase.py': 'existing',
}
SELF = 'tools/gate/junction_lint.py'


def scan(root):
    errs = []
    for ext in ('*.py', '*.ps1', '*.bat', '*.cmd', '*.sh'):
        for f in sorted((Path(root) / 'tools').rglob(ext)):
            rel = f.relative_to(root).as_posix()
            if rel == SELF or '__pycache__' in rel:
                continue
            try:
                text = f.read_text(encoding='utf-8', errors='replace')
            except OSError:
                continue
            if LINK_RE.search(text) and rel not in LINK_ALLOW:
                errs.append('%s: creates a junction / symlink (mklink /J, os.symlink, CreateJunction, New-Item Junction / SymbolicLink): not on the allow-list; use an argument or RDR_MAIN_CHECKOUT instead' % rel)
            if WT_RE.search(text) and rel not in WT_ALLOW:
                errs.append('%s: runs `git worktree remove`: not on the allow-list (it follows junctions: CLAUDE.md Safety; use tools\\bisect\\wt_build.ps1 -Clean or tools\\harness\\tidy.py)' % rel)
    return errs


def selftest():
    fails = []
    with tempfile.TemporaryDirectory() as td:
        t = Path(td) / 'tools' / 'x'
        t.mkdir(parents=True)
        cases = [('PLANT: mklink /J in a new tool fails', 'a.ps1', 'cmd /c mklink /J a b\n', True),
                 ('PLANT: os.symlink fails', 'b.py', 'import os\nos.symlink(a, b)\n', True),
                 ('PLANT: New-Item -ItemType Junction fails', 'c.ps1', "New-Item -ItemType Junction -Path a -Target b\n", True),
                 ('PLANT: git worktree remove fails', 'd.py', "subprocess.run(['git', 'worktree', 'remove', p])\n", True),
                 ('PLANT: a bare `git worktree remove --force` line fails', 'e.ps1', 'git worktree remove --force out\\wt\\x\n', True),
                 ('plain code passes', 'f.py', 'print(1)\n', False)]
        for name, fn, body, want in cases:
            (t / fn).write_text(body, encoding='utf-8')
            got = bool([e for e in scan(td) if ('x/' + fn) in e])
            print(('PASS  ' if got == want else 'FAIL  ') + name)
            if got != want:
                fails.append(name)
            (t / fn).unlink()
    print('%d failed' % len(fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    errs = scan(a.root)
    for e in errs:
        print('ERROR ' + e)
    print('junction_lint: %d error(s)' % len(errs))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
