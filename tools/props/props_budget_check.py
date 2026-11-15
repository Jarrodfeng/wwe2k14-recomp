#!/usr/bin/env python3
"""S102 lane T17 offline proof (0 game runs): the props hook's per-frame budget (--wwe2k14_rdr_props_budget; src\\renderer_scene\\props_budget.h, props_ours.cpp).

  py -I tools\\props\\props_budget_check.py [--root DIR] [--no-build] [--runs 3]

Input: Check F's capture (the Brock run's case zip, 337 frames, scene records; lane 84's props check uses the same), the game files, fview.exe built from this tree.
Every fview arm replays the whole capture with the props hook on (--props-ours --replay --hash-frames --frame-times). Real arms (the judge below must find nothing):
  on        --props-budget on (defaults: 2 MB, 1000 us)
  tight     --props-budget-bytes 262144: the creation is spread over several frames
  us1       --props-budget-us 1 with 1 GB of bytes: the hook's MILLISECONDS are spread (Decide counts: a cold object is deferred before Decide)
  noimage   on + --props-budget-noimage-slot 4: the binder's slot 4 is empty, the role is never made, need must not count it
  defer_all --props-budget-bytes 1 --props-budget-noprogress: nothing is ever made, every draw keeps the stream's: pictures equal on all frames (coverage is not asked)
  thin      the default bundle (--arena-ours --wrestlers-ours --ring-ours --props-ours) with the thin emulation (--thin arena,ring,wrestlers --thin-run), tight budget vs budget off
Twins (each must FAIL the judge with the named key, or the check is blind):
  plant 1 no_budget (overrun) | 2 hole, a deferred draw answered with an empty draw (picture) | 3 image_twice (image_twice) | 4 ignore_us (spread) | 5 no_reset (fallback)
  | 6 need_ignores_slot (made_bytes) | 7 need_zero_texture (made_bytes) | 8 stale_pre: the worker's tables of another texture order (content, pictures stay equal)
  | 9 deferred_as_not_ready (deferred_binder) | noprogress on the tight arm (coverage) | a source without the DropAll free of the test-only resources (lint)
The judge: exit 0 and 'validation errors 0'; every frame's picture hash equals the off arm's; overrun 0; fallback == first_over (frames whose FIRST single need passes the bytes budget);
frames over the budget <= fallback; images made == kept; bytes made == bytes needed in every frame; the hook's deferred count == the binder's 'budget deferred' count (the stream draw
reached the thin learner as BudgetDeferred, not NotReady); coverage: no deferral in the last 50 frames, replaced within [off's replaced - deferred, off's replaced], content checks failing equal to the off arm's."""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TIGHT = 262144
BIG = 1 << 30
DEFER = ['--props-budget', 'on', '--props-budget-bytes', '1', '--props-budget-noprogress']
TIGHTARGS = ['--props-budget', 'on', '--props-budget-bytes', str(TIGHT)]
US1 = ['--props-budget', 'on', '--props-budget-bytes', str(BIG), '--props-budget-us', '1']


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    for attempt in range(6):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        text = (p.stdout or '') + (p.stderr or '')
        if p.returncode and 'Not enough graphics memory for the frame ring' in text and attempt < 5:
            print('INFO  GPU busy, attempt %d of 6, retrying in 20 s' % (attempt + 1))
            time.sleep(20)
            continue
        return p.returncode, text


def read_hashes(path):
    out = {}
    for line in open(path, encoding='utf-8'):
        a, _, b = line.strip().partition(',')
        if a.isdigit():
            out[int(a)] = b
    return out


def read_times(path):
    rows = []   # (frame, bind, render, need_bytes, replace_ms, made_bytes, deferred)
    for line in [l for l in open(path, encoding='utf-8').read().splitlines()[1:] if l.strip()]:
        f, bind, render, need, rep, made, dfr = line.split(',')
        rows.append((int(f), float(bind), float(render), int(need), float(rep), int(made), int(dfr)))
    return rows


CENSUS = re.compile(r'\[rdr\]\[props_budget\] frames (\d+), slices (\d+), bytes (\d+), fallback (\d+), first_over (\d+), forced_us (\d+), overrun (\d+), deferred (\d+), pre_deferred (\d+), '
                    r'most bytes in one frame (\d+), images made (\d+) of (\d+) kept, made (\d+) bytes, budget (\d+) bytes / (\d+) us')
BINDER = re.compile(r'\[rdr\]\[props\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(.*?persist-only \d+(?:, budget deferred (\d+))?')
CONTENT = re.compile(r'content: checks (\d+) ok (\d+) differs (\d+) no feed (\d+)')


def facts(text):
    """The numbers a judge compares across arms: binder replaced, content differs."""
    b = None
    for b in BINDER.finditer(text):
        pass
    c = None
    for c in CONTENT.finditer(text):
        pass
    return dict(replaced=int(b.group(3)) if b else -1, binder_deferred=int(b.group(5) or 0) if b else -1, differs=int(c.group(3)) if c else -1)


def judge(arm, ref, budget_bytes, coverage=True, spread=False):
    """-> list of failure keys (empty = every bar met). `ref` = the off arm."""
    bad = []
    if arm['rc'] != 0:
        bad.append('exit')
    if 'validation errors 0' not in arm['text']:
        bad.append('validation')
    h, rh = arm['hashes'], ref['hashes']
    if len(h) != len(rh) or any(h.get(k) != v for k, v in rh.items()):
        bad.append('picture')
    m = None
    for m in CENSUS.finditer(arm['text']):
        pass
    if not m:
        bad.append('census')
        return bad
    keys = ('frames', 'slices', 'bytes', 'fallback', 'first_over', 'forced_us', 'overrun', 'deferred', 'pre_deferred', 'most', 'made', 'kept', 'made_bytes', 'cap_b', 'cap_us')
    c = dict(zip(keys, (int(x) for x in m.groups())))
    arm['census'] = c
    f = facts(arm['text'])
    arm['facts'] = f
    if c['overrun']:
        bad.append('overrun')
    if c['fallback'] != c['first_over']:
        bad.append('fallback')
    if len([r for r in arm['times'] if r[3] > budget_bytes]) > c['fallback']:
        bad.append('frames_over_budget')
    if c['made'] != c['kept']:
        bad.append('image_twice')
    if any(r[5] != r[3] for r in arm['times']):
        bad.append('made_bytes')
    if f['binder_deferred'] != c['deferred']:
        bad.append('deferred_binder')
    if spread and not (c['slices'] >= 2 and c['deferred'] > 0):
        bad.append('spread')
    if coverage:
        lo = ref['facts']['replaced'] - f['binder_deferred']   # a deferred draw may be one the off arm would have kept anyway (an object that turns out foreign), so replaced may exceed lo, never the off arm's
        if any(r[6] for r in arm['times'][-50:]) or not (lo <= f['replaced'] <= ref['facts']['replaced']):   # nothing deferred late, and no replaced draw is lost beyond the deferred ones
            bad.append('coverage')
    if f['differs'] != ref['facts']['differs']:
        bad.append('content')
    return bad


def lint_test_only(root, mutate=False):
    """Every member the header marks TESTONLY_RES is freed by FreeTestOnly, and DropAll calls it (so the destructor does)."""
    h = open(os.path.join(root, 'src', 'renderer_scene', 'props_ours.h'), encoding='utf-8').read()
    cpp = open(os.path.join(root, 'src', 'renderer_scene', 'props_ours.cpp'), encoding='utf-8').read()
    names = []
    for line in h.splitlines():
        if 'TESTONLY_RES' in line:
            m = re.search(r'(\w+_)\s*(=[^;]*)?;', line)
            names.append(m.group(1) if m else '?')
    if mutate:
        mut = cpp.replace('  FreeTestOnly();   // S102 T17: every TEST ONLY resource', '  // (planted: the free is gone) every TEST ONLY resource')
        if mut == cpp:
            return ['DID NOT APPLY: the planted lint source equals the original']
        cpp = mut

    def body(fn):
        i = cpp.find(fn)
        if i < 0:
            return None
        j = cpp.find('\n}\n', i)
        j = cpp.find('\r\n}\r\n', i) if j < 0 else j
        return cpp[i:j]
    bad = []
    if not names:
        bad.append('no TESTONLY_RES member found')
    drop, free = body('void PropsOurs::DropAll()'), body('void PropsOurs::FreeTestOnly()')
    if drop is None or free is None:
        return bad + ['DropAll / FreeTestOnly not found']
    if 'FreeTestOnly()' not in drop:
        bad.append('DropAll does not call FreeTestOnly')
    for n in names:
        if n not in free:
            bad.append('%s is not freed in FreeTestOnly' % n)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--runs', type=int, default=3)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261112_101633_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        print("FAIL  input missing: Check F's case zip under " + runs)
        return 1
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    out = os.path.join(main_co, 'out', 'lane84')
    cap = os.path.join(out, 'checkF', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'checkF'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'checkF'))
    if not os.path.isdir(os.path.join(cap, 'scene')):
        print('FAIL  the capture has no scene records: ' + cap)
        return 1
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe
    exe = _fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 't17check')
    os.makedirs(work, exist_ok=True)

    def arm(tag, extra, validation=False):
        d = os.path.join(work, tag)
        os.makedirs(d, exist_ok=True)
        hp, tp = os.path.join(d, 'hash.csv'), os.path.join(d, 'times.csv')
        for p in (hp, tp):
            if os.path.isfile(p):
                os.remove(p)
        cmd = [exe, '--capture', cap, '--props-ours', '--game', game, '--replay', '--frames', '1000', '--hash-frames', hp, '--frame-times', tp] + list(extra)
        if validation:
            cmd += ['--validation', '1']
        rc, text = run(cmd)
        if not (os.path.isfile(hp) and os.path.isfile(tp)):
            print(text[-1500:])
            print('FAIL  arm %s wrote no hash / time file (exit %d)' % (tag, rc))
            sys.exit(1)
        return dict(rc=rc, text=text, hashes=read_hashes(hp), times=read_times(tp), facts=facts(text))

    def thin_arm(tag, extra):
        d = os.path.join(work, tag)
        os.makedirs(d, exist_ok=True)
        cmd = [exe, '--capture', cap, '--props-ours', '--game', game, '--arena-ours', '--wrestlers-ours', '--ring-ours', '--thin', 'arena,ring,wrestlers', '--thin-run', d, '--frames', '1000'] + list(extra)
        rc, text = run(cmd)
        hs = {}
        fp = os.path.join(d, 'frames.csv')
        if os.path.isfile(fp):
            for line in open(fp, encoding='utf-8').read().splitlines()[1:]:
                p = line.split(',')
                if len(p) > 1 and p[0].isdigit():
                    hs[int(p[0])] = p[1]
        return dict(rc=rc, text=text, hashes=hs)

    fails = 0

    def verdict(ok, msg):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + msg)
        fails += not ok

    ref = arm('off', [], validation=True)
    if ref['rc'] != 0 or 'validation errors 0' not in ref['text'] or len(ref['hashes']) < 100 or ref['facts']['replaced'] <= 0:
        print(ref['text'][-1500:])
        print('FAIL  the off arm itself: exit %d, %d frame hashes, facts %s' % (ref['rc'], len(ref['hashes']), ref['facts']))
        return 1
    print('INFO  off arm: %d frames hashed, validation errors 0, replaced %d, content checks failing %d' % (len(ref['hashes']), ref['facts']['replaced'], ref['facts']['differs']))

    real = {}
    for tag, extra, cap_b, kw in (('on', ['--props-budget', 'on'], 2097152, {}), ('tight', TIGHTARGS, TIGHT, {}), ('us1', US1, BIG, {'spread': True}),
                                  ('noimage', ['--props-budget', 'on', '--props-budget-noimage-slot', '4'], 2097152, {}), ('defer_all', DEFER, 1, {'coverage': False})):
        a = arm(tag, extra, validation=True)
        real[tag] = a
        bad = judge(a, ref, cap_b, **kw)
        verdict(not bad, '%s arm: judge %s; census %s' % (tag, bad or 'clean', a.get('census')))
    c = real['tight']['census']
    verdict(c['slices'] >= 2 and c['deferred'] > 0 and c['most'] <= TIGHT, 'tight arm: the creation is spread (slices %d >= 2, deferred %d > 0, most bytes in one frame %d <= %d)' % (c['slices'], c['deferred'], c['most'], TIGHT))
    c = real['us1']['census']
    verdict(c['pre_deferred'] > 0 or c['deferred'] > 0, 'us1 arm: the hook\'s time is spread by --props-budget-us 1 (slices %d, deferred %d, of them before Decide %d)' % (c['slices'], c['deferred'], c['pre_deferred']))
    c = real['noimage']['census']
    verdict(c['made_bytes'] == c['bytes'], 'noimage arm: bytes made %d == bytes needed %d (the empty role is not counted)' % (c['made_bytes'], c['bytes']))
    c = real['on']['census']
    verdict(c['slices'] >= 1 and c['bytes'] > 0, 'on arm (the lever under the shipped defaults): the hook made %d bytes in %d frame(s), deferred %d, before Decide %d' % (c['bytes'], c['slices'], c['deferred'], c['pre_deferred']))
    for tag in ('on', 'tight', 'us1'):
        print('INFO  %s arm: need / made / deferred per frame (frame, need, made, deferred): %s' % (tag, [(r[0], r[3], r[5], r[6]) for r in real[tag]['times'] if r[3] or r[5] or r[6]][:12]))

    # the default bundle with the thin emulation (F6): pictures equal, frames the lever took
    tb = thin_arm('thin_base', [])
    tt = thin_arm('thin_tight', TIGHTARGS)
    m = None
    for m in CENSUS.finditer(tt['text']):
        pass
    ok = tb['rc'] == 0 and tt['rc'] == 0 and len(tb['hashes']) > 100 and tb['hashes'] == tt['hashes'] and m is not None and int(m.group(8)) > 0 and 'validation errors 0' in tt['text']
    verdict(ok, 'default bundle (arena + wrestlers + ring + props, thin on): pictures equal on all %d frames with the budget on; the lever took %s slices, deferred %s draws' % (len(tt['hashes']), m.group(2) if m else '?', m.group(8) if m else '?'))
    thin_lines = [l for l in tt['text'].splitlines() if l.startswith('[rdr][thin]')]
    print('INFO  thin counters with the budget on: %s' % [l[:110] for l in thin_lines])

    # twins
    twins = (('plant1_no_budget', TIGHTARGS + ['--props-budget-plant', '1'], TIGHT, 'overrun', {}),
             ('plant2_hole', DEFER + ['--props-budget-plant', '2'], 1, 'picture', {'coverage': False}),
             ('plant3_image_twice', TIGHTARGS + ['--props-budget-plant', '3'], TIGHT, 'image_twice', {}),
             ('plant4_ignore_us', US1 + ['--props-budget-plant', '4'], BIG, 'spread', {'spread': True}),
             ('plant5_no_reset', TIGHTARGS + ['--props-budget-plant', '5'], TIGHT, 'fallback', {}),
             ('plant6_need_ignores_slot', ['--props-budget', 'on', '--props-budget-noimage-slot', '4', '--props-budget-plant', '6'], 2097152, 'made_bytes', {}),
             ('plant7_need_zero_texture', TIGHTARGS + ['--props-budget-plant', '7'], TIGHT, 'made_bytes', {}),
             ('plant8_stale_pre', ['--props-budget', 'on', '--props-budget-plant', '8'], 2097152, 'content', {}),
             ('plant9_deferred_as_not_ready', TIGHTARGS + ['--props-budget-plant', '9'], TIGHT, 'deferred_binder', {}),
             ('noprogress_on_tight', ['--props-budget', 'on', '--props-budget-bytes', str(TIGHT // 4), '--props-budget-noprogress'], TIGHT // 4, 'coverage', {}))
    for tag, extra, cap_b, want, kw in twins:
        a = arm(tag, extra, validation=True)
        bad = judge(a, ref, cap_b, **kw)
        caught = want in bad
        extra_ok = True
        if tag == 'plant8_stale_pre':
            extra_ok = 'picture' not in bad   # the stale table only fails the content checks: the geometry is still ours and equal
        verdict(caught and extra_ok, 'twin %s: the judge reports %s (wanted %s%s)%s' % (tag, bad, want, ', pictures equal' if tag == 'plant8_stale_pre' else '', '' if caught and extra_ok else '  <-- NOT caught, the check is blind to this fault'))
    lint_ok = lint_test_only(root)
    verdict(not lint_ok, 'lint: every TESTONLY_RES resource is freed in FreeTestOnly and DropAll calls it %s' % (lint_ok or ''))
    lint_bad = lint_test_only(root, mutate=True)
    verdict(bool(lint_bad) and not any(b.startswith('DID NOT APPLY') for b in lint_bad), 'twin lint: DropAll without the free is reported %s' % lint_bad)

    # timing: INFO only (moves with the PC's load); the credit split of the claim
    summary = {}
    for tag, extra in (('off', []), ('pre_only', ['--props-budget', 'on', '--props-budget-bytes', str(BIG), '--props-budget-us', str(BIG)]), ('us1', US1), ('tight', TIGHTARGS)):
        reps = []
        for i in range(args.runs):
            a = arm('time_%s_%d' % (tag, i), extra)
            if a['rc'] != 0 or not a['times']:
                print('FAIL  timing arm %s run %d: exit %d' % (tag, i, a['rc']))
                return 1
            reps.append(max(r[4] for r in a['times']))
        summary[tag] = statistics.median(reps)
        print('INFO  %s: biggest ms inside the hook in one frame, median of %d runs = %.2f (runs %s)' % (tag, args.runs, summary[tag], ['%.2f' % x for x in reps]))
    print('INFO  credit: the worker tables (off -> pre_only) %.2f ms; the budgeted Decide + making (pre_only -> us1) %.2f ms; the bytes budget (pre_only -> tight) %.2f ms' % (summary['off'] - summary['pre_only'], summary['pre_only'] - summary['us1'], summary['pre_only'] - summary['tight']))
    print('props_budget_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
