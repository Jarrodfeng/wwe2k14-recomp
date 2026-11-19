#!/usr/bin/env python3
r"""Session 103 lane PS1 gate check: a job on a `frame_rig.exe --serve` process draws exactly the picture a one-shot run of the same arm draws (0 game runs: fview headless + frame_rig.exe).

  py -I tools\scene\serve_equal_check.py [--out DIR] [--plant lights_key|no_reset|no_keep_cpu] [--orders N] [--first-order K]

Six arms (a job = the one-shot command line of one frame):
  after / before / nohooks  brock_losswin_rt frame 100 with the stage look rows, with none, and with --no-hooks (the F2 case: a no-hooks job leaves wrestlers cached with no CPU copy)
  other_record              brock_losswin_rt frame 20: ANOTHER record of the same arena (F1: the arena's moving slots / screens / debounce of the job before must not reach it)
  entr / match              a04_entrance frame 60 then a04_match frame 60: an entrance record, a match record of one arena, after the other arena
Every arm is run as its own process (frame_rig.py one-shot, the reference) and as a job of ONE serve process, in as many orders as there are arms: order k is the arm list ROTATED by k, so every arm is
the FIRST job of one process and comes after every other arm in the others. Every PNG an arm wrote (the whole picture and the nine family panels) must be byte-equal to its reference. The crowd of a serve
process is KEPT between jobs (S103 PS1 Q5: LiveCrowd's per-frame light cache is keyed on the record's light + stage-look rows).

Planted twins (each exits 0 only when the check FAILS the fault; they run every order until the first inequality):
  lights_key    WWE_RDR_PLANT_LIGHTS_KEY_OLD=1: the crowd's light cache keyed on the record's address + guest frame alone (the Q5 cause: the old key; 38 081 pixels differ on `before` after `after`)
  no_reset      WWE_RIG_PLANT_NO_RESET=1: the serve path does not call rf::c::ResetForJob (F1)
  no_keep_cpu   WWE_RIG_PLANT_NO_KEEP_CPU=1: the serve cache does not keep the wrestlers' CPU copies (F2)
Writes under --out (default: GATE_CHECK_DIR\ps1[_plant], else a per-run <main checkout>\out\ps1[_plant]_<pid>, removed on PASS; run_dirs.py). A state a future change lets one job leave behind for the next shows up here as an unequal picture.
"""
import argparse, glob, hashlib, io, os, subprocess, sys, contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import frame_rig  # noqa: E402
import run_dirs  # noqa: E402
os.environ['WWE_FVIEW_ALLOW_STALE'] = '1'   # the game side (fview) is not under test (as l_check / lay_check)

# (tag, capture, frame, extra args)
ARMS = [
    ('after', 'brock_losswin_rt', 100, ['--look-from-stream']),
    ('before', 'brock_losswin_rt', 100, []),
    ('nohooks', 'brock_losswin_rt', 100, ['--no-hooks']),
    ('other_record', 'brock_losswin_rt', 20, []),
    ('entr', 'a04_entrance', 60, []),
    ('match', 'a04_match', 60, []),
]
PLANTS = {'lights_key': 'WWE_RDR_PLANT_LIGHTS_KEY_OLD', 'no_reset': 'WWE_RIG_PLANT_NO_RESET', 'no_keep_cpu': 'WWE_RIG_PLANT_NO_KEEP_CPU'}


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def pictures(d, cap, frame):
    w = os.path.join(d, 'work_' + cap)
    return {os.path.basename(f): sha(f) for f in glob.glob(os.path.join(w, 'ours_%d_*.png' % frame)) if '_fov_' not in f}


def main():
    return _main(sys.argv[1:])


def _main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    ap.add_argument('--plant', default='', choices=[''] + sorted(PLANTS))
    ap.add_argument('--orders', type=int, default=0, help='how many rotated orders to run (default: one per arm)')
    ap.add_argument('--first-order', type=int, default=0, help='start at this rotation (a planted twin starts at the order known to catch it: S103 PS1F found lights_key at 0, no_reset at 5, no_keep_cpu at 2)')
    a = ap.parse_args(argv)
    main_root = frame_rig.main_checkout()
    out = run_dirs.run_out('ps1' + ('_' + a.plant if a.plant else ''), a.out)   # S103 PSO: a private folder per run (never a fixed path shared with other runs / worktrees)
    return run_dirs.finish(out, _run(a, main_root, out))   # PASS removes it, FAIL keeps it and prints where


def _run(a, main_root, out):
    caps = {}
    for tag, cap, fr, extra in ARMS:
        caps[cap] = os.path.join(main_root, 'out', 'captures', cap)
        if not os.path.isdir(caps[cap]):
            print('FAIL  missing input: ' + caps[cap])
            return 1
    ref_root = os.path.join(out, 'ref')   # the one-shot reference pictures: inside this run's own folder
    ref = {}
    for i, (tag, cap, fr, extra) in enumerate(ARMS):   # the reference: every arm its own process
        d = os.path.join(ref_root, 'oneshot_%d_%s' % (i, tag))
        env = {k: v for k, v in os.environ.items() if k not in PLANTS.values()}
        p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'frame_rig.py'), '--capture', caps[cap], '--frames', str(fr), '--out', d] + extra, capture_output=True, text=True, errors='replace', timeout=900, env=env)
        ref[tag] = pictures(d, cap, fr)
        if not ref[tag]:
            print('FAIL  the one-shot arm %s wrote no pictures:\n%s' % (tag, (p.stdout or '')[-600:] + (p.stderr or '')[-300:]))
            return 1
    n_orders = a.orders or len(ARMS)
    bad, n, jobs = [], 0, 0
    for k in range(a.first_order, a.first_order + n_orders):
        order = ARMS[k % len(ARMS):] + ARMS[:k % len(ARMS)]
        if a.plant:
            os.environ[PLANTS[a.plant]] = '1'
        frame_rig.SESSION = frame_rig.RigSession(prep_root=os.path.join(out, 'prep%d' % k))
        try:
            for j, (tag, cap, fr, extra) in enumerate(order):
                d = os.path.join(out, 'o%d_%d_%s' % (k, j, tag))
                with contextlib.redirect_stdout(io.StringIO()):
                    frame_rig.main(['--capture', caps[cap], '--frames', str(fr), '--out', d] + extra)   # (a --no-hooks arm's own HOOKS MISSING verdict is not under test: its pictures are)
                got = pictures(d, cap, fr)
                for name, h in ref[tag].items():
                    n += 1
                    if got.get(name) != h:
                        bad.append('order %d job %d (%s) %s' % (k, j, tag, name))
                jobs += 1
        finally:
            st = frame_rig.SESSION.stats
            frame_rig.SESSION.close()
            os.environ.pop(PLANTS.get(a.plant, ''), None)
        if st['one_shot']:
            print('FAIL  a job fell back to a one-shot process: the serve path was not what was tested')
            return 1
        if a.plant and bad:
            break   # the twin is caught
    print('INFO  %d pictures compared over %d served jobs (%d orders of %d arms)' % (n, jobs, n_orders, len(ARMS)))
    if a.plant:
        print(('PASS  planted fault caught (%s): %d picture(s) changed: %s' % (a.plant, len(bad), ', '.join(bad[:4]))) if bad else 'FAIL  the planted %s twin drew every picture equal: the check cannot see that leak' % a.plant)
        return 0 if bad else 1
    for b in bad:
        print('FAIL  served job differs from the one-shot run: ' + b)
    print('%s  serve_equal: %d of %d pictures byte-equal' % ('PASS' if not bad else 'FAIL', n - len(bad), n))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
