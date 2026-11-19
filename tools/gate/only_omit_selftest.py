# Selftest for the --only omission warning (S103 OMIT). py -I tools\gate\only_omit_selftest.py [--root DIR]
# Why: MIP's gate (S103, 15:02, an --only list of 10 lanes) left out s103_F5 although daf91b9fe edited src\renderer_scene\char_effects.cpp, a file in
# s103_F5's affects; F5 then failed unseen for 3 hours (docs\research\s103\F5S.md).
#   1 REAL lanes, a synthetic change to src/renderer_scene/char_effects.cpp: --only s103_C3 names s103_F5 as omitted; --only s103_C3,s103_F5 does not;
#     a change to a file no lane names lists none.
#   2 END TO END (scratch git repo, the real gate.py): the warning line and `omitted-affected: N` in the gate line appear; --strict-only exits 1 before running;
#     without --strict-only the exit code is unchanged. Against the gate before this change (no warning line) check 2 fails: that is the plant, see PLANT below.
#   3 RE-FIND of the real bug: commit daf91b9fe with MIP's 10-lane list (out\gate\times.csv, 15:02, agent-a83b3febfeab8e5f3) names s103_F5.
import json, os, subprocess, sys, tempfile, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fastgate as fg

PY = sys.executable
FAILS = []
GATE = Path(os.environ.get('ONLY_OMIT_GATE') or HERE / 'gate.py')   # PLANT: ONLY_OMIT_GATE=<the gate before this change> must fail part 2
MIP_LANES = 's103_B3F,s103_L6,s103_L7,s103_SLV,s103_W2,h2_lints,s102_c4,s102_l1,s102_r18_b2,s102_v1'


def ok(cond, msg):
    print(('ok   ' if cond else 'FAIL ') + msg)
    if not cond:
        FAILS.append(msg)


def load_checks(root):
    checks = []
    for f in sorted((Path(root) / 'tools' / 'gate' / 'lanes').glob('*.json')):
        data = json.loads(f.read_text(encoding='utf-8'))
        for c in data.get('checks', []):
            c['lane'] = f.stem
            c['_lane_rel'] = f'tools/gate/lanes/{f.name}'
            if 'affects' not in c and 'affects' in data:
                c['affects'] = data['affects']
            checks.append(c)
    return checks


def git(cwd, *a):
    return subprocess.run(['git', '-C', str(cwd)] + list(a), capture_output=True, text=True)


def part1(root):
    checks = load_checks(root)
    ch = ['src/renderer_scene/char_effects.cpp']
    om = fg.omitted_affected(checks, {'s103_C3'}, ch)
    ok('s103_F5' in om, f'real lanes: change to char_effects.cpp, --only s103_C3 omits s103_F5 (omitted: {sorted(om)[:6]}...)')
    om = fg.omitted_affected(checks, {'s103_C3', 's103_F5'}, ch)
    ok('s103_F5' not in om, 'real lanes: --only s103_C3,s103_F5 does not list s103_F5')
    om = fg.omitted_affected(checks, {'s103_C3'}, ['zz_nothing/x.bin'])
    ok(om == {}, f'real lanes: a file no lane names lists none (got {om})')
    om = fg.omitted_affected(checks, {'s103_F5.f_test'}, ch)
    ok('s103_F5' in om, 'a check name in --only keeps only that check: the lane stays listed through its other checks')


def part3(root):
    r = git(root, 'cat-file', '-t', 'daf91b9fe')
    if r.stdout.strip() != 'commit':
        print('SKIP daf91b9fe not in this repo')
        return
    names = [n for n in git(root, 'diff', '--name-only', '--no-renames', 'daf91b9fe~1', 'daf91b9fe').stdout.split('\n') if n.strip()]
    om = fg.omitted_affected(load_checks(root), set(MIP_LANES.split(',')), names)
    ok('s103_F5' in om, f're-find: daf91b9fe + MIP lane list names s103_F5 ({om.get("s103_F5")})')


def part2():
    tmp = Path(tempfile.mkdtemp(prefix='omit_'))
    try:
        (tmp / 'src').mkdir()
        (tmp / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
        (tmp / 'src' / 'a.cpp').write_text('1\n')
        (tmp / 'src' / 'b.cpp').write_text('1\n')
        for n, f in (('la', 'src/a.cpp'), ('lb', 'src/b.cpp')):
            (tmp / 'tools' / 'gate' / 'lanes' / f'{n}.json').write_text(json.dumps(
                {'checks': [{'name': n + '.c', 'cmd': [PY, '-c', 'pass'], 'timeout_s': 30, 'affects': [f]}]}))
        git(tmp, 'init', '-q', '-b', 'main')
        git(tmp, '-c', 'user.name=t', '-c', 'user.email=t@t', 'add', '-A')
        git(tmp, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'base')
        (tmp / 'src' / 'a.cpp').write_text('2\n')   # the change touches la only
        env = dict(os.environ, GATE_NO_RESTORE='1', GATE_NO_SHARE='1')
        base = [PY, '-I', str(GATE), '--root', str(tmp), '--out', str(tmp / 'out'), '--no-exes', '--no-toolcache', '--jobs', '1']
        r = subprocess.run(base + ['--only', 'lb'], capture_output=True, text=True, env=env, timeout=120)
        ok('--only leaves out 1 lane(s) whose affects match the change: la (src/a.cpp)' in r.stdout, 'e2e: --only lb names la (src/a.cpp) as left out')
        ok('omitted-affected: 1' in r.stdout and r.returncode == 0, f'e2e: gate line carries omitted-affected: 1, exit code unchanged (rc {r.returncode})')
        r = subprocess.run(base + ['--only', 'la,lb'], capture_output=True, text=True, env=env, timeout=120)
        ok('leaves out' not in r.stdout and 'omitted-affected: 0' in r.stdout, 'e2e: --only la,lb prints no warning, omitted-affected: 0')
        r = subprocess.run(base + ['--only', 'lb', '--strict-only'], capture_output=True, text=True, env=env, timeout=120)
        ok(r.returncode == 1 and 'refused by --strict-only' in r.stdout, f'e2e: --strict-only exits 1 before running (rc {r.returncode})')
        r = subprocess.run(base + ['--only', 'lb', '--base', 'HEAD'], capture_output=True, text=True, env=env, timeout=120)
        ok('la (src/a.cpp)' in r.stdout, 'e2e: --base REV is honoured')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def commit(cwd, msg):
    git(cwd, '-c', 'user.name=t', '-c', 'user.email=t@t', 'add', '-A')
    return git(cwd, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', msg)


def part4():
    """default base: the session branch is the MAIN checkout's branch (any name); a worktree branch diffs from merge-base(HEAD, session); the main
    checkout on the session branch right after a merge diffs from HEAD^1; a renamed session branch must not matter."""
    tmp = Path(tempfile.mkdtemp(prefix='omitb_'))
    mainco, wt = tmp / 'main', tmp / 'wt'
    try:
        (mainco / 'src').mkdir(parents=True)
        (mainco / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
        for n in ('a', 'b', 'c'):
            (mainco / 'src' / (n + '.cpp')).write_text('1\n')
            (mainco / 'tools' / 'gate' / 'lanes' / ('l' + n + '.json')).write_text(json.dumps(
                {'checks': [{'name': 'l' + n + '.c', 'cmd': [PY, '-c', 'pass'], 'timeout_s': 30, 'affects': ['src/' + n + '.cpp']}]}))
        git(mainco, 'init', '-q', '-b', 'sess')
        commit(mainco, 'c0')
        c0 = git(mainco, 'rev-parse', 'HEAD').stdout.strip()
        git(mainco, 'worktree', 'add', '-q', '-b', 'worktree-x', str(wt), 'sess')
        (mainco / 'src' / 'a.cpp').write_text('2\n')
        commit(mainco, 'session changes a')
        (wt / 'src' / 'b.cpp').write_text('3\n')
        commit(wt, 'worktree changes b')
        git(wt, '-c', 'user.name=t', '-c', 'user.email=t@t', 'merge', '-q', '--no-edit', 'sess')   # the worktree merges the session: a.cpp comes in
        env = dict(os.environ, GATE_NO_RESTORE='1', GATE_NO_SHARE='1')

        def run(root, *extra):
            return subprocess.run([PY, '-I', str(GATE), '--root', str(root), '--out', str(tmp / 'out'), '--no-exes', '--no-toolcache', '--jobs', '1',
                                   '--only', 'lc'] + list(extra), capture_output=True, text=True, env=env, timeout=120).stdout

        o = run(wt)
        ok('lb (src/b.cpp)' in o and 'la (' not in o, 'base: worktree branch after merging the session lists lb only (a.cpp came from the session)')
        o = run(wt, '--base', c0)
        ok('la (src/a.cpp)' in o and 'lb (src/b.cpp)' in o, 'base: --base <root commit> also lists la')
        git(mainco, 'checkout', '-q', '-b', 'side', c0)
        (mainco / 'src' / 'b.cpp').write_text('side\n')
        commit(mainco, 'side changes b')
        git(mainco, 'checkout', '-q', 'sess')
        git(mainco, '-c', 'user.name=t', '-c', 'user.email=t@t', 'merge', '-q', '--no-ff', '-m', 'merge side', 'side')
        o = run(mainco)
        ok('lb (src/b.cpp)' in o and 'la (' not in o, 'base: main checkout on the session branch after a merge commit uses HEAD^1 (lb only)')
        git(mainco, 'branch', '-m', 'sess', 'rlb')
        o = run(wt)
        ok('lb (src/b.cpp)' in o and 'la (' not in o, 'base: session branch renamed to rlb: still the main checkout branch, lists lb only')
        o = run(wt, '--strict-only')
        ok('refused by --strict-only' in o and 'omitted-affected: 1' in o, 'strict: a refusal prints a gate: line (gate_wait / gate_main parse it)')
        o = run(mainco, '--base', 'no-such-rev-zz', '--strict-only')
        ok('refused by --strict-only' in o, 'strict: an error computing the list refuses too')
        o = run(mainco, '--base', 'no-such-rev-zz')
        ok('omitted-affected: ?' in o, 'non-strict: an error prints omitted-affected: ?')
    finally:
        git(mainco, 'worktree', 'remove', '--force', str(wt))
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    root = Path(sys.argv[sys.argv.index('--root') + 1]) if '--root' in sys.argv else HERE.parents[1]
    part1(root)
    part2()
    part4()
    part3(root)
    print('FAIL' if FAILS else 'PASS', f'only_omit_selftest: {len(FAILS)} failure(s)')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
