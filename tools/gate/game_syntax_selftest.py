# Selftest for game_syntax --changed's pure parts (S103 GSB review fixes). py -I tools\gate\game_syntax_selftest.py   (GS_DIR=<folder holding game_syntax.py + proc.py> tests another copy)
# Scratch git repos in a temp folder, no compile, no game:
#   1 default_base in a builder worktree after `git merge <session branch>` = the merge-base (the builder's own edits are in the changed set), never HEAD^1 (what the session brought in)
#   2 default_base on the main checkout: last-pass sha (an ancestor) wins; a non-ancestor / missing one falls back to HEAD^1 (a non-merge HEAD is still diffed against its parent)
#   3 a failed git diff gives None (the caller FAILs), not an empty changed set
#   4 a renamed header is split into delete + add (--no-renames); a deleted header still maps to its includers; src/pch_main.h reaches src/main.cpp
import os, subprocess, sys, tempfile, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, os.environ.get('GS_DIR', str(HERE)))
import game_syntax as gs

FAILS = []
ENV = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t')


def ok(cond, text):
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


def git(root, *a):
    r = subprocess.run(['git', '-C', str(root)] + list(a), env=ENV, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()


def put(root, rel, text):
    p = Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def main():
    tmp = Path(tempfile.mkdtemp(prefix='gs_selftest_'))
    try:
        M = tmp / 'main'
        M.mkdir()
        git(M, 'init', '-q', '-b', 'main')
        put(M, 'src/a.h', '// a\n')
        put(M, 'src/a.cpp', '#include "a.h"\n#include "gone.h"\n')
        put(M, 'src/gone.h', '// gone\n')
        put(M, 'src/main.cpp', 'int main(){}\n')
        put(M, 'src/pch_main.h', '#pragma once\n')
        git(M, 'add', '-A'); git(M, 'commit', '-qm', 'c1')
        c1 = git(M, 'rev-parse', 'HEAD')
        W = tmp / 'wt'
        git(M, 'worktree', 'add', '-q', '-b', 'b', str(W))
        put(W, 'src/a.h', '// builder edit\n')
        git(W, 'commit', '-qam', 'builder')
        put(M, 'src/main.cpp', 'int main(){return 1;}\n')
        git(M, 'commit', '-qam', 'c3 on main')
        git(W, 'merge', '-q', '--no-edit', 'main')
        # 1
        base = gs.default_base(str(W), str(M))
        c3 = git(M, 'rev-parse', 'HEAD')
        ok(base == c3 and base != git(W, 'rev-parse', 'HEAD^1'), '1 builder worktree after the session merge: base is the merge-base (the session tip), not HEAD^1 (got %s)' % base)
        ch = gs.changed_files(str(W), base)
        ok(ch == ['src/a.h'], '1 the changed set is exactly the builder\'s own edit, not what the session brought in (%s)' % ch)
        # 2
        ok(gs.default_base(str(M), str(M)) == 'HEAD^1', '2 main checkout, no last-pass file: HEAD^1')
        lp = Path(gs.last_pass_file(str(M), str(M)))
        lp.parent.mkdir(parents=True, exist_ok=True)
        lp.write_text(c1 + '\n')
        ok(gs.default_base(str(M), str(M)) == c1, '2 main checkout, last-pass sha is an ancestor of HEAD: it is the base')
        lp.write_text(git(W, 'rev-parse', 'HEAD') + '\n')
        ok(gs.default_base(str(M), str(M)) == 'HEAD^1', '2 main checkout, last-pass sha not an ancestor: HEAD^1')
        lp.write_text('deadbeef\n')
        ok(gs.default_base(str(M), str(M)) == 'HEAD^1', '2 main checkout, junk last-pass sha: HEAD^1')
        # 3
        ok(gs.changed_files(str(M), 'no_such_ref_xyz') is None, '3 a failed git diff returns None, not an empty list')
        # 4
        git(M, 'mv', 'src/a.h', 'src/b.h')
        git(M, 'rm', '-q', 'src/gone.h')
        ch = gs.changed_files(str(M), 'HEAD')
        ok(ch is not None and 'src/a.h' in ch and 'src/b.h' in ch, '4 a rename is a delete plus an add (%s)' % ch)
        ok('src/a.cpp' in gs.units_for_changed(str(M), ['src/gone.h']), '4 a deleted header still reaches the unit that includes it')
        ok('src/main.cpp' in gs.units_for_changed(str(M), ['src/pch_main.h']), '4 src/pch_main.h reaches src/main.cpp')
        ok('src/a.cpp' in gs.units_for_changed(str(M), ['src/a.h']), '4 a changed (renamed-away) header reaches its includer')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('FAIL: %d' % len(FAILS) if FAILS else 'PASS: game_syntax selftest')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
