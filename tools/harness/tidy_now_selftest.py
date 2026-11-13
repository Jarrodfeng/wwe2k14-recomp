"""Selftest for tidy.py --now / --auto watch / the 30-minute low pass (lane 187, Session 101). 0 game runs; fake trees only.

  py tools\\harness\\tidy_now_selftest.py            every check below
  py tools\\harness\\tidy_now_selftest.py --refind   tonight's case (2026-11-12): a finished, merged lane worktree last touched 40 min
                                                   ago, AGENT_LIVE_H = 2 h: the OLD low pass (git rev OLD_REV) keeps it, the new one
                                                   removes it

The fake trees come from tidy_selftest.py (World, age_tree, junction ...). free_gb() is replaced in a small runner: it starts at 10
and rises with the pad files removed, in 'GB' of 1 MB (T.GB = 1e6), so no real disk number matters and nothing real is touched.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tidy_selftest as S      # noqa: E402
from tidy_selftest import World, age_tree, write, junction, rm_world, set_times, git, Results, H, NOWIN      # noqa: E402

NEW = os.path.join(HERE, 'tidy.py')
OLD_REV = 'dd771d67'      # tidy.py before lane 187 (s101-renderer-arc)
MIN = 60.0
MB = 1_000_000

RUNNER = '''
import importlib.util, json, os, sys, time
spec = importlib.util.spec_from_file_location('tidy_under_test', sys.argv[1])
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
cfg = json.loads(sys.argv[2])
T.GB = 1e6                          # "GB" = 1 MB here
tracked = [(os.path.join(T.REPO, *p.split('/')), s) for p, s in cfg['tracked']]
base = cfg.get('base', 10.0)
def fg():
    return base + sum(s for p, s in tracked if not os.path.exists(p)) / 1e6
T.free_gb = fg
if 'target' in cfg:
    T.NOW_TARGET_GB = cfg['target']
t0 = time.time()
mode = cfg['mode']
if mode == 'now':
    rc = T.cmd_now(cfg.get('dry', False))
elif mode == 'low':
    print('free after', T.low_pass([], set()))
    rc = 0
elif mode == 'main':
    sys.argv = [sys.argv[0]] + cfg['argv']
    rc = T.main()
    print('RC', rc)
elif mode == 'lock':
    T.lock_busy = lambda: 'slot1: busy'
    T.LOCKED = cfg.get('locked', False)
    rc = T.cmd_now(False)
elif mode == 'wants':
    import argparse
    ns = argparse.Namespace(apply=None, auto=cfg.get('auto'), now=cfg.get('now', False), dry=cfg.get('dry', False))
    print('WANTS', T.wants_lock(ns))
    rc = 0
elif mode == 'watchbusy':
    marker = os.path.join(os.path.dirname(T.REPO), 'alert_marker.txt')
    T.TEST = False
    T.lock_busy = lambda: 'slot1: busy'
    T.alert = lambda st, txt, t: open(marker, 'w').write(txt)
    sys.argv = [sys.argv[0], '--auto', 'watch']
    rc = T.main()
    print('ALERT' if os.path.exists(marker) else 'NOALERT')
elif mode == 'watch':
    sys.argv = [sys.argv[0], '--auto', 'watch']
    rc = T.main()
print('RUNTIME %.3f' % (time.time() - t0))
sys.exit(rc or 0)
'''


def run(w, script, **cfg):
    runner = os.path.join(os.path.dirname(w.root), 'now_runner.py')
    write(runner, RUNNER)
    r = subprocess.run([sys.executable, runner, script, json.dumps(cfg)], env=w.env(), capture_output=True, text=True,
                       errors='replace', creationflags=NOWIN, timeout=300)
    return r.stdout + r.stderr


def lane(w, name, kind='merged', size=3 * MB, idle_min=40.0, finished=True, transcript_min=None, meta=True, fresh_file_min=None, parent='sess1'):
    """An agent worktree idle idle_min minutes with its agent's meta/transcript (transcript_min defaults to idle_min)."""
    path = w.wt(name, kind, size=size)
    age_tree(path, time.time() - idle_min * MIN)
    if fresh_file_min is not None:
        set_times(os.path.join(path, 'pad.bin'), time.time() - fresh_file_min * MIN)
    tm = idle_min if transcript_min is None else transcript_min
    w.transcript(name, tm / 60.0, parent=parent)
    if meta:
        w.meta(name, finished, hours=tm / 60.0, parent=parent)
    return path


def tracked(*names, size=3 * MB):
    return [['.claude/worktrees/%s/pad.bin' % n, size] for n in names]


def suite_now(script, base, R):
    print('--now: finished idle >= 30 min removed; running / active / KEEP / held / junction / real edit kept')
    w = World(base, 'now')
    now = time.time()
    w.live = [['sess1', now - 5 * H, now - 60]]
    ext = os.path.join(base, 'now_ext')
    write(os.path.join(ext, 'precious.txt'), 'keep me')
    age_tree(ext, now - 50 * H)
    lane(w, 'agent-done')                                              # finished, merged, 40 min idle
    lane(w, 'agent-unm', 'unmerged', idle_min=45)                      # B1: finished, NOT merged, 45 min: kept (under review / resumed)
    lane(w, 'agent-unm3h', 'unmerged', idle_min=180)                   # unmerged, 3 h idle (past AGENT_LIVE_H): goes, branch stays
    lane(w, 'agent-det', 'detached', idle_min=200)                     # commit on no branch, 3 h: saved under refs/tidy first
    resumed = w.wt('agent-resumed'); age_tree(resumed, now - 45 * MIN)      # S2: meta says finished 100 min ago, transcript 45 min ago
    w.transcript('agent-resumed', 45 / 60.0)
    w.meta('agent-resumed', True, hours=100 / 60.0)
    oldr = w.wt('agent-oldresumed'); age_tree(oldr, now - 45 * MIN)      # S2 refinement: same, but the parent session is not live
    w.transcript('agent-oldresumed', 45 / 60.0, parent='deadsess')
    w.meta('agent-oldresumed', True, hours=100 / 60.0, parent='deadsess')
    lane(w, 'agent-orph', finished=False, idle_min=180, parent='deadsess')   # S3: no finished flag, parent session not live
    lane(w, 'agent-run5h', finished=False, idle_min=300)               # meta without finished, parent session live: running
    lane(w, 'agent-run5m', finished=False, idle_min=5)                 # running, fresh
    lane(w, 'agent-tr10', idle_min=40, transcript_min=10)              # finished but its transcript is 10 min old
    lane(w, 'agent-file10', idle_min=40, fresh_file_min=10)            # a file 10 min old
    keep = lane(w, 'agent-keep')
    write(os.path.join(keep, 'KEEP'), 'x')
    age_tree(keep, now - 40 * MIN)
    lane(w, 'agent-held')
    with open(w.p('out', 'tidy', 'carry.json'), 'w') as f:
        json.dump([{'path': '.claude/worktrees/agent-held', 'why': 'x', 'added': now - 5 * H, 'expires': now + 5 * 86400}], f)
    junc = lane(w, 'agent-junc')
    junction(os.path.join(junc, 'link'), ext)
    edit = lane(w, 'agent-edit')
    write(os.path.join(edit, 'README.md'), 'changed by a builder')       # tracked file modified: a real edit
    age_tree(edit, now - 40 * MIN)
    names = ['agent-done', 'agent-unm', 'agent-unm3h', 'agent-det', 'agent-resumed', 'agent-oldresumed', 'agent-orph', 'agent-run5h', 'agent-run5m', 'agent-tr10', 'agent-file10', 'agent-keep',
             'agent-held', 'agent-junc', 'agent-edit']
    out = run(w, script, mode='now', tracked=tracked(*names), target=1000, dry=True, base=40.0)
    R.check('--now --dry removes nothing', all(w.exists('.claude', 'worktrees', n) for n in names), out)
    R.check('--now --dry names the agent worktrees it would free', 'would free' in out and 'agent worktrees (4)' in out, out)
    mgd = w.wt('mg', 'merged', under=('out', 'wt')); age_tree(mgd, now - 40 * MIN)      # S1: plain worktrees stay on the low-pass age
    mg3 = w.wt('mg3', 'merged', under=('out', 'wt')); age_tree(mg3, now - 3 * H)
    w.temp('cap', 1.5); write(w.p('out', 'cap', 'images.bin'), b'x'); age_tree(w.p('out', 'cap'), now - 1.5 * H)      # B2: capture folder 1.5 h old
    w.temp('plain3h', 3)
    out = run(w, script, mode='now', tracked=tracked(*names), target=1000, base=40.0)
    ex = lambda n: w.exists('.claude', 'worktrees', n)
    R.check('finished merged worktree idle 40 min removed', not ex('agent-done'), out)
    R.check('B1: finished UNMERGED worktree idle 45 min kept (above the hard floor)', ex('agent-unm'), out)
    R.check('unmerged worktree idle 3 h removed, its branch stays',
            not ex('agent-unm3h') and 'br_unm3h' in git(w.root, 'branch', '--list'))
    R.check('commit on no branch (3 h idle) saved under refs/tidy, then removed',
            not ex('agent-det') and 'refs/tidy/agent-det' in git(w.root, 'for-each-ref', 'refs/tidy'))
    R.check('S2: meta finished but transcript written later (resumed agent) kept', ex('agent-resumed'))
    R.check('S2: the same with a parent session that is not live (bulk-touched old transcript) is removed', not ex('agent-oldresumed'))
    R.check('S3: meta without finished, parent session not live, kept', ex('agent-orph'))
    R.check('S1: plain worktree (out\\wt) idle 40 min kept, 3 h removed', w.exists('out', 'wt', 'mg', 'pad.bin') and not w.exists('out', 'wt', 'mg3'))
    R.check('B2: capture folder 1.5 h old kept, plain 3 h folder removed', w.exists('out', 'cap', 'images.bin') and not w.exists('out', 'plain3h'), out)
    R.check('running agent (no finished flag), idle 5 h, kept', ex('agent-run5h'))
    R.check('running agent, idle 5 min, kept', ex('agent-run5m'))
    R.check('finished but transcript 10 min old: kept', ex('agent-tr10'))
    R.check('finished but a file 10 min old: kept', ex('agent-file10'))
    R.check('KEEP file kept', ex('agent-keep'))
    R.check('held worktree kept', ex('agent-held'))
    R.check('worktree with a junction kept, junction target untouched',
            ex('agent-junc') and os.path.exists(os.path.join(ext, 'precious.txt')))
    R.check('real edit kept and reported', ex('agent-edit') and 'real edits (reported)' in out, out)
    R.check('summary is short (at most 15 lines) and names free before / after', len(out.strip().splitlines()) <= 16 and 'C: free' in out, out)
    R.check('history line written', 'now:' in open(w.p('out', 'tidy', 'history.txt'), encoding='utf-8').read())

    # stops at the target: biggest first, the smaller one stays
    big = lane(w, 'agent-big', size=5 * MB)
    small = lane(w, 'agent-small', size=1 * MB)
    out = run(w, script, mode='now', tracked=tracked('agent-big', size=5 * MB) + tracked('agent-small', size=1 * MB), target=14, base=10.0)
    R.check('stops at the target, biggest first (small one stays)', not ex('agent-big') and ex('agent-small'), out)

    # B1 hard floor: under 15 GB free the unmerged one goes at 30 min too, and the summary says so
    lane(w, 'agent-unmfloor', 'unmerged', idle_min=45)
    out = run(w, script, mode='now', tracked=tracked('agent-unmfloor') + tracked('agent-unm'), target=1000, base=5.0)
    R.check('B1: below the hard floor an unmerged 45 min worktree goes, and the summary says HARD FLOOR',
            not ex('agent-unmfloor') and not ex('agent-unm') and 'HARD FLOOR' in out, out)
    # B3: the out pass is skipped while the run lock is busy, but not inside our own hold
    w.temp('lockcase', 3)
    out = run(w, script, mode='lock', tracked=[], base=5.0, target=1000)
    R.check('B3: run lock busy -> out pass skipped', w.exists('out', 'lockcase', 'data.bin') and 'game or build is running' in out, out)
    out = run(w, script, mode='lock', tracked=[], base=5.0, target=1000, locked=True)
    R.check('B3: inside our own run-lock hold the out pass runs', not w.exists('out', 'lockcase'), out)
    for kw, want in (({'now': True}, True), ({'auto': 'watch'}, True), ({'now': True, 'dry': True}, False), ({'auto': 'hourly'}, True)):
        o = run(w, script, mode='wants', tracked=[], **kw)
        R.check('B3: wants_lock(%s) is %s' % (kw, want), 'WANTS %s' % want in o, o)
    # watch blocked by the run lock: alert only below the hard floor
    try:
        os.unlink(w.p('out', 'tidy', 'state.json'))      # earlier runs set the once-a-day alert throttle
    except OSError:
        pass
    o = run(w, script, mode='watchbusy', tracked=[], base=10.0)
    R.check('watch: lock busy and under 15 GB free -> alert', 'ALERT' in o and 'NOALERT' not in o and 'skipped' in o, o)
    try:
        os.unlink(os.path.join(os.path.dirname(w.root), 'alert_marker.txt'))
    except OSError:
        pass
    o = run(w, script, mode='watchbusy', tracked=[], base=30.0)
    R.check('watch: lock busy at 30 GB free -> no alert', 'NOALERT' in o and 'skipped' in o, o)
    o = run(w, script, mode='main', tracked=[], argv=['--now', '--force'])
    R.check('--force is gone: argparse refuses it', 'unrecognized arguments' in o, o)
    # N5: --target tidy-up
    o = run(w, script, mode='main', tracked=[], argv=['--target', '5'])
    R.check('N5: --target without --now refused', 'RC 2' in o and 'only goes with --now' in o, o)
    o = run(w, script, mode='main', tracked=[], argv=['--now', '--target', '0'])
    R.check('N5: --now --target 0 refused', 'RC 2' in o, o)

    # nothing to do above the target
    lane(w, 'agent-last')
    out = run(w, script, mode='now', tracked=[], base=500.0, target=100)
    R.check('already above the target: says so, deletes nothing', 'nothing to do' in out and ex('agent-last'), out)
    rm_world(w)
    shutil.rmtree(ext, ignore_errors=True)


def suite_watch(script, base, R):
    print('--auto watch: free space only; above the threshold it does nothing and returns at once')
    w = World(base, 'watch')
    now = time.time()
    w.live = [['sess1', now - 5 * H, now - 60]]
    lane(w, 'agent-done')
    names = ['agent-done']
    out = run(w, script, mode='watch', tracked=tracked(*names), base=80.0, target=1000)
    rt = float(out.split('RUNTIME')[1].split()[0]) if 'RUNTIME' in out else 99
    R.check('watch above LOW_GB: nothing removed', w.exists('.claude', 'worktrees', 'agent-done'), out)
    R.check('watch above LOW_GB: returns in well under a second', rt < 0.5, out)
    R.check('watch above LOW_GB: prints nothing', [l for l in out.splitlines() if l.strip() and not l.startswith('RUNTIME')] == [], out)
    out = run(w, script, mode='watch', tracked=tracked(*names), base=10.0, target=1000)
    R.check('watch below LOW_GB: runs the --now logic and removes the finished idle worktree', not w.exists('.claude', 'worktrees', 'agent-done'), out)
    R.check('watch is logged in the history', 'auto watch:' in open(w.p('out', 'tidy', 'history.txt'), encoding='utf-8').read())
    rm_world(w)


def old_script(base):
    p = os.path.join(base, 'tidy_old.py')
    src = subprocess.run(['git', '-C', os.path.dirname(HERE), 'show', '%s:tools/harness/tidy.py' % OLD_REV], capture_output=True,
                         creationflags=NOWIN).stdout
    if not src:
        raise RuntimeError('cannot read tidy.py at ' + OLD_REV)
    open(p, 'wb').write(src)
    return p


def suite_low(script, base, R):
    print('the under-50-GB pass: a finished MERGED lane idle 40 min goes, an unmerged one and a running one stay')
    w = World(base, 'lowpass')
    now = time.time()
    w.live = [['sess1', now - 5 * H, now - 60]]
    lane(w, 'agent-merged')
    lane(w, 'agent-unmerged', 'unmerged')
    lane(w, 'agent-running', finished=False, idle_min=300)
    names = ['agent-merged', 'agent-unmerged', 'agent-running']
    out = run(w, script, mode='low', tracked=tracked(*names))
    ex = lambda n: w.exists('.claude', 'worktrees', n)
    R.check('low pass: finished merged lane, 40 min idle, removed', not ex('agent-merged'), out)
    R.check('low pass: unmerged finished lane keeps its 24 h wait', ex('agent-unmerged'))
    R.check('low pass: running agent kept', ex('agent-running'))
    rm_world(w)


def refind(base):
    print("re-find of 2026-11-12 23:4x: a finished, merged lane worktree last touched 40 min ago, AGENT_LIVE_H = 2 h")
    rows = {}
    for label, script in (('OLD', old_script(base)), ('NEW', NEW)):
        w = World(base, 'refind_' + label.lower())
        now = time.time()
        w.live = [['sess1', now - 5 * H, now - 60]]
        lane(w, 'agent-lane')
        run(w, script, mode='low', tracked=tracked('agent-lane'))
        rows[label] = not w.exists('.claude', 'worktrees', 'agent-lane')
        rm_world(w)
    print('  old low pass removed it: %s   new low pass removed it: %s' % (rows['OLD'], rows['NEW']))
    ok = (not rows['OLD']) and rows['NEW']
    print('  re-find %s' % ('OK: the old pass keeps the 40 min lane (2 h rule), the new one removes it' if ok else 'NOT AS EXPECTED'))
    return 0 if ok else 1


def main():
    base = tempfile.mkdtemp(prefix='tidy_now_selftest_', dir=os.environ.get('TIDY_SELFTEST_DIR') or None)
    try:
        if '--refind' in sys.argv:
            return refind(base)
        R = Results()
        for suite in (suite_now, suite_watch, suite_low):
            suite(NEW, base, R)
        print('%d checks, %d failed' % (len(R.rows), len(R.failed())))
        return 1 if R.failed() else 0
    finally:
        shutil.rmtree(base, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
