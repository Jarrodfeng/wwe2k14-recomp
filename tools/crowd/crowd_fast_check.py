#!/usr/bin/env python3
"""S102 lane R21-C: the crowd's rank work done once and flat (docs/renderer/CROWD_RANK_FAST.md). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\crowd\\crowd_fast_check.py [--root DIR] [--only unit|wiring|bryan|brock|counts] [--smoke]

Switches: wwe2k14_rdr_crowd_fast (PerFrame, default off; fview / rplay --crowd-fast on|off [--crowd-fast-steps 1|2|3]) = step 1 (whether a program is a crowd one comes from a table made once, not two string compares
on each draw of the frame) + step 2 (the density rank's memory is an open-addressing table); wwe2k14_rdr_crowd_workers (0-2, default 0; --crowd-workers N) = step 3 (the pose of the crowd members' classes on a small pool).
Arms (S102 common.md "Speed of the lane itself": the player config on / off, the planted twins, ONE other capture):
  unit    the rank unit (tools\\crowd\\crowd_rank_test.cpp: flat == node for 3.4 million calls, the flipping run, the lane 234 burst plant re-found on the flat tracker, 2 planted twins) + the pool unit
          (crowd_workers_test.cpp, 3 planted twins).
  wiring  every link of both switches in the source (table PerFrame default 0, cvar default off / 0, live.cpp per-frame read, fview / rplay flags, binder SetFast + the crowd table, crowd_ours' per-frame read and join);
          each removed in a text copy must fail the lint (each text plant checks it applied).
  bryan   fview headless on the standard Bryan capture, the whole player bundle AND crowd density 70 (the AUTO LOW tier's value: at 100 the rank is never called): every frame's picture hash byte-equal off vs
          fast vs fast + 2 workers, validation 0 / exit 0, the crowd census lines (members drawn, draws dropped / kept, cards) and the rank counters (calls, followed / inherited / fresh) equal, how many frames took
          each step; the twins early_keep (a stream draw the hook keeps is dropped: pictures differ from the same arm without the plant), early_first (the draw that triggers the injection takes the early path:
          ours never draws), rank_radius_off (ranks differ: pictures differ), workers_unjoined (the pool is not joined: pictures differ or validation).
  brock   the same pair (off vs fast + 2 workers) on the Brock capture (the one other capture).
  counts  rplay --timing bind_us, off vs on, interleaved, 3 repeats, Bryan and Brock; per step (1, 2, 3) and workers 2. Reported; the lane fails only when on is more than 5 % slower than off."""
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'g12'))
import g12_check as g  # noqa: E402  (Arm, BUNDLE, main_checkout: the same harness)

fails = 0
BROCK_ZIP = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
BRYAN_ZIP = g.CAPTURES['bryan']
BROCK_COUNTS_DIR = 'out/lane86/brock/capture'
DENSITY = ['--crowd-density', '70']
FAST = re.compile(r'\[rdr\]\[crowd_fast\] flat (\d), stream crowd draws (\d+): early-outs \(dropped\) (\d+), kept (\d+); density tests (\d+) \(non-origin (\d+), before inject (\d+), kept (\d+), ours absent (\d+); culled (\d+)\); rank calls (\d+) \(followed (\d+), inherited (\d+), fresh (\d+)\)')
CROWD = re.compile(r'\[rdr\]\[crowd\] ours frames (\d+), fallback (\d+), stream draws dropped (\d+), members drawn (\d+), ms ours [\d.]+ \(stream draws kept for members ours could not draw: (\d+)\)')
CARDS = re.compile(r'\[rdr\]\[crowd\] cards: frames ours (\d+), fallback (\d+), card draws dropped (\d+), cards drawn (\d+), kept (\d+)')


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def census(arm):
    m = FAST.search(arm.text)
    c = CROWD.search(arm.text)
    k = CARDS.search(arm.text)
    d = dict(fast=None, crowd=None, cards=None)
    if m:
        v = [int(x) for x in m.groups()]
        d['fast'] = dict(flat=v[0], stream=v[1], dropped=v[2], kept=v[3], tests=v[4], nonorigin=v[5], before=v[6], keptt=v[7], absent=v[8], culled=v[9], calls=v[10], followed=v[11], inherited=v[12], fresh=v[13])
    if c:
        d['crowd'] = tuple(int(x) for x in c.groups())
    if k:
        d['cards'] = tuple(int(x) for x in k.groups())
    return d


# ---- unit -----------------------------------------------------------------------------------------------------------------------------------------------------------
def unit(root):
    for ps1, exe_name, plants in (('build_crowd_rank_test.ps1', 'crowd_rank_test.exe', ('no_track', 'no_swap', 'no_guard', 'rank_radius_off', 'flat_no_rotate')),
                                  ('build_crowd_workers_test.ps1', 'crowd_workers_test.exe', ('unjoined', 'double_run', 'drop_job'))):
        r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'crowd', ps1)], capture_output=True, text=True, timeout=600)
        exe = os.path.join(root, 'out', 'tools', exe_name)
        line(r.returncode == 0 and os.path.isfile(exe), '%s builds' % exe_name)
        if not os.path.isfile(exe):
            continue
        r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
        text = r.stdout
        print('\n'.join('      ' + l for l in text.splitlines() if l.startswith('flat vs node') or l.startswith('burst plant')))
        line(r.returncode == 0 and 'all passed' in text, '%s: the shipped code passes (%d checks)' % (exe_name, sum(1 for l in text.splitlines() if l.startswith('PASS'))))
        for plant in plants:
            r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, timeout=300)
            line(r.returncode == 1, '%s twin %s is caught (exit %d)' % (exe_name, plant, r.returncode))


# ---- wiring ---------------------------------------------------------------------------------------------------------------------------------------------------------
LINKS = [
    ('table: crowd_fast PerFrame default 0', 'src/renderer_adapter/rdr_switches.h', r'X\(CrowdFast, "wwe2k14_rdr_crowd_fast", PerFrame, 0\)'),
    ('table: crowd_workers PerFrame default 0', 'src/renderer_adapter/rdr_switches.h', r'X\(CrowdWorkers, "wwe2k14_rdr_crowd_workers", PerFrame, 0\)'),
    ('cvar crowd_fast default false', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_crowd_fast, false'),
    ('cvar crowd_workers default 0', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_crowd_workers, 0'),
    ('live.cpp reads crowd_fast every frame', 'src/renderer_adapter/live.cpp', r'g_crowd_fast = rdr_sw::BoolSeen\(rdr_sw::kCrowdFast'),
    ('binder: the rank memory follows the switch each Bind', 'src/renderer_adapter/binder.h', r'g_crowd_rank\.SetFast\(g_crowd_fast && \(g_crowd_fast_steps & 2\) != 0\)'),
    ('binder: the crowd program table is used under step 1', 'src/renderer_adapter/binder.h', r'\(g_crowd_fast && \(g_crowd_fast_steps & 1\)\) \? CrowdVariantTable\(\)'),
    ('crowd_ours reads crowd_workers every frame', 'src/renderer_scene/crowd_ours.cpp', r'rdr_sw::IntSeen\(rdr_sw::kCrowdWorkers'),
    ('crowd_ours times the pool pose as kPose (review F2)', 'src/renderer_scene/crowd_ours.cpp', r'\}, unjoined\);\s*ph_\.Lap\(kPose\)'),
    ('crowd_ours report prints the pool census (review F3)', 'src/renderer_scene/crowd_ours.cpp', r'\[rdr\]\[crowd\] workers: pose on the pool %llu frames'),
    ('pool only with 8+ hardware threads, never on Android (review F1)', 'src/renderer_scene/crowd_ours.cpp', r'hardware_concurrency\(\) >= 8'),
    ('pool threads are named (review F1)', 'src/renderer_scene/crowd_workers.h', r'wwe2k14::NameThread\(name\)'),
    ('pool threads run below normal priority (review F1)', 'src/renderer_scene/crowd_workers.h', r'SetThreadPriority\(reinterpret_cast<void\*>\(static_cast<intptr_t>\(-2\)\), -1\)'),
    ('the unjoined twin joins before the locals go (review F4)', 'src/renderer_scene/crowd_ours.cpp', r'~JoinGuard\(\) \{\s*if \(w\) w->WaitIdle\(\)'),
    ('crowd_ours poses on the pool and skips the loop pose after it', 'src/renderer_scene/crowd_ours.cpp', r'if \(!pre_posed\)'),
    ('fview flags', 'src/fview/main.cpp', r'"--crowd-fast"[\s\S]*"--crowd-workers"'),
    ('rplay flags', 'src/renderer/player/rplay.cpp', r'"--crowd-fast"[\s\S]*"--crowd-workers"'),
]


def wiring(root):
    for name, path, rx in LINKS:
        text = open(os.path.join(root, path), encoding='utf-8', errors='replace').read()
        m = re.search(rx, text)
        line(m is not None, 'wiring: ' + name)
        if not m:
            continue
        planted = text[:m.start()] + text[m.end():]
        line(planted != text and re.search(rx, planted) is None, 'wiring twin (link removed in a text copy): %s is caught' % name)


# ---- replays --------------------------------------------------------------------------------------------------------------------------------------------------------
def replay(root, which, smoke=False):
    main_co = g.main_checkout(root)
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(root, 'out', 'r21c_check')
    os.makedirs(work, exist_ok=True)
    z = os.path.join(main_co, BRYAN_ZIP if which == 'bryan' else BROCK_ZIP)
    if not (os.path.isfile(exe) and os.path.isfile(z)):
        line(False, 'input missing: %s %s' % (exe, z))
        return
    B = g.BUNDLE + DENSITY
    A = g.Arm(exe, z, game, work, which + '_A', B)
    line(A.clean and len(A.hashes) > 1, 'baseline %s (the player bundle, density 70, crowd_fast off): %d frames hashed, validation %d, exit %d' % (which, A.frames_hashed, A.valid, A.rc))
    ca = census(A)
    line(ca['fast'] is not None and ca['fast']['tests'] > 0 and ca['fast']['calls'] > 1000, 'baseline census: the density rank runs at 70 (%s)' % (ca['fast'],))
    line('rank thread breaks 0' in A.text, 'the rank tracker was called from one thread only (review F6: "rank thread breaks 0" in the census)')
    if which == 'bryan':
        arms = [('fast (steps 1+2)', ['--crowd-fast', 'on'], 'F'), ('step 1 alone', ['--crowd-fast', 'on', '--crowd-fast-steps', '1'], 'S1'), ('step 2 alone', ['--crowd-fast', 'on', '--crowd-fast-steps', '2'], 'S2'),
                ('fast + 2 workers', ['--crowd-fast', 'on', '--crowd-workers', '2'], 'W')]
        if smoke:
            arms = [arms[0], arms[3]]
    else:
        arms = [('fast + 2 workers', ['--crowd-fast', 'on', '--crowd-workers', '2'], 'W')]
    for name, extra, tag in arms:
        X = g.Arm(exe, z, game, work, '%s_%s' % (which, tag), B + extra)
        cx = census(X)
        d = X.differing(A)
        equal = ca['crowd'] == cx['crowd'] and ca['cards'] == cx['cards'] and ca['fast'] and cx['fast'] and all(ca['fast'][k] == cx['fast'][k] for k in ('stream', 'dropped', 'kept', 'tests', 'nonorigin', 'before', 'keptt', 'absent', 'culled', 'calls', 'followed', 'inherited', 'fresh'))
        flat_ok = (cx['fast'] or {}).get('flat') == (1 if '--crowd-fast-steps' not in extra or extra[-1] in ('2', '3') else 0)
        line(d == 0 and len(X.hashes) == len(A.hashes) and X.clean and equal and flat_ok,
             '%s %s: %d frames, %d pictures differ, validation %d, exit %d, crowd + cards census equal %s, rank counters equal %s (calls %d, followed %d, inherited %d, fresh %d), flat table %s' %
             (name, which, X.frames_hashed, d, X.valid, X.rc, ca['crowd'] == cx['crowd'] and ca['cards'] == cx['cards'], bool(equal), (cx['fast'] or {}).get('calls', -1), (cx['fast'] or {}).get('followed', -1),
              (cx['fast'] or {}).get('inherited', -1), (cx['fast'] or {}).get('fresh', -1), 'on' if (cx['fast'] or {}).get('flat') else 'off'))
        f = cx['fast'] or {}
        wl = re.search(r'\[rdr\]\[crowd\] workers: pose on the pool (\d+) frames, (\d+) pool runs, (\d+) class jobs, (\d+) threads', X.text)
        want_workers = '--crowd-workers' in extra
        line(wl is not None and ((int(wl.group(1)) > 0) == want_workers), 'workers census (review F3): %s' % (wl.group(0) if wl else 'line missing'))
        step1 = '--crowd-fast-steps' not in extra or extra[-1] in ('1', '3')
        step2 = '--crowd-fast-steps' not in extra or extra[-1] in ('2', '3')
        print('      took: step 1 (the crowd-program table: every draw of a frame asks it while the density is under 100) %s; step 2 (the flat rank table) %d rank calls, %d density tests; %d early-outs of %d stream crowd draws '
              '(the base\'s own early-out: it leaves before the density test)' % ('on' if step1 else 'off', f.get('calls', 0) if step2 else 0, f.get('tests', 0), f.get('dropped', 0), f.get('stream', 0)), flush=True)
    if which != 'bryan' or smoke:
        return
    # twins (Bryan only)
    Bu = B + ['--crowd-fault', 'undrawable_type']
    U = g.Arm(exe, z, game, work, 'bryan_U', Bu + ['--crowd-fast', 'on'])
    cu = census(U)
    line(U.clean and cu['fast'] and cu['fast']['kept'] > 0, 'control for the keep twin: with the planted undrawable type some stream draws stay the game\'s (kept %s) and the arm is clean' % ((cu['fast'] or {}).get('kept'),))
    EK = g.Arm(exe, z, game, work, 'bryan_EK', Bu + ['--crowd-fast', 'on', '--crowd-fast-fault', 'early_keep'], allow_stop=True)
    line(EK.differing(U) > 0, 'twin crowd_early_keep (the early-out also drops a draw the hook keeps: a member vanishes) is caught: %d frames differ' % EK.differing(U))
    EF = g.Arm(exe, z, game, work, 'bryan_EF', B + ['--crowd-fast', 'on', '--crowd-fast-fault', 'early_first'], allow_stop=True)
    cf = census(EF)
    line(cf['crowd'] is not None and cf['crowd'][0] == 0 and EF.differing(A) > 0, 'twin crowd_early_first (the injecting draw takes the early path: ours never injects) is caught: ours frames %s, %d frames differ' % (cf['crowd'][0] if cf['crowd'] else '?', EF.differing(A)))
    RR = g.Arm(exe, z, game, work, 'bryan_RR', B + ['--crowd-fast', 'on', '--crowd-fast-fault', 'rank_radius_off'], allow_stop=True)
    line(RR.differing(A) > 0, 'twin rank_radius_off (the inheritance radius 24 -> 12) is caught: %d frames differ' % RR.differing(A))
    WU = g.Arm(exe, z, game, work, 'bryan_WU', B + ['--crowd-fast', 'on', '--crowd-workers', '2', '--crowd-workers-fault', 'unjoined'], allow_stop=True)
    line(WU.differing(A) > 0 and WU.rc == 0 and WU.valid == 0, 'twin workers_unjoined (the pool is joined only after the geometry is pushed: no undefined behaviour, a clean exit) is caught by the pictures: %d frames differ, validation %d, exit %d' % (WU.differing(A), WU.valid, WU.rc))


# ---- counts ---------------------------------------------------------------------------------------------------------------------------------------------------------
def bind_us(rplay, capdir, game, work, name, extra):
    path = os.path.join(work, name + '.csv')
    if os.path.exists(path):
        os.remove(path)
    p = subprocess.run([rplay, '--capture', capdir, '--frames', '120', '--game', game, '--timing', path] + g.BUNDLE + DENSITY + extra, capture_output=True, text=True, timeout=1700, errors='replace')
    if p.returncode != 0 or not os.path.isfile(path):
        raise RuntimeError('rplay %s exit %d: %s' % (name, p.returncode, (p.stdout + p.stderr)[-600:]))
    vals = []
    with open(path) as f:
        next(f)
        for ln in f:
            c = ln.strip().split(',')
            if int(c[0]) >= 20:
                vals.append(float(c[3]))
    return statistics.mean(vals), len(vals)


def counts(root):
    main_co = g.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    work = os.path.join(root, 'out', 'r21c_check')
    os.makedirs(work, exist_ok=True)
    for which, capdir in (('bryan', os.path.join(main_co, g.BRYAN_COUNTS_DIR)), ('brock', os.path.join(main_co, BROCK_COUNTS_DIR))):
        if not (os.path.isfile(rplay) and os.path.isdir(capdir)):
            line(False, 'rplay or its capture is missing: %s %s' % (rplay, capdir))
            continue
        variants = [('off', []), ('step1', ['--crowd-fast', 'on', '--crowd-fast-steps', '1']), ('step2', ['--crowd-fast', 'on', '--crowd-fast-steps', '2']), ('steps1+2', ['--crowd-fast', 'on']),
                    ('workers2', ['--crowd-fast', 'on', '--crowd-workers', '2'])]
        if which == 'brock':   # (the one other capture: off, both steps, workers; two repeats: the check's time)
            variants = [variants[0], variants[3], variants[4]]
        res = {n: [] for n, _ in variants}
        for rep in range(3 if which == 'bryan' else 2):  # interleaved
            for n, extra in variants:
                v, nf = bind_us(rplay, capdir, game, work, '%s_%s_%d' % (which, n, rep), extra)
                res[n].append(v)
        base = statistics.mean(res['off'])
        row = ', '.join('%s %.0f us (%+.1f%%)' % (n, statistics.mean(res[n]), 100 * (statistics.mean(res[n]) - base) / base) for n, _ in variants)
        line(statistics.mean(res['steps1+2']) <= base * 1.05, 'counts %s (rplay --timing, bind_us mean of frames 20-119, interleaved repeats (Bryan 3, Brock 2), density 70, the bundle; the PC is a 4090 desktop): %s' % (which, row))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    ap.add_argument('--smoke', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if args.only in ('', 'unit'):
        unit(root)
    if args.only in ('', 'wiring'):
        wiring(root)
    if args.only in ('', 'bryan'):
        replay(root, 'bryan', args.smoke)
    if args.only in ('', 'brock'):
        replay(root, 'brock', args.smoke)
    if args.only in ('', 'counts'):
        counts(root)
    print('R21-C: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
