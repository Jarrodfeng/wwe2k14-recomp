"""S102 S0-4: proof of tools\\bench\\ab_verdict.py, make_ab_route.py and switch_table.py, offline, no game.
  py -I tools\\bench\\ab_verdict_selftest.py [--old-log PATH]
Every group plants a fault and the tool must find it, or the input must be refused loudly:
  1  GPU +2 ms in the ON windows -> GPU side finds it, the others read "no difference"; same for a render-thread +2 ms; a switch that does nothing
  2  an ABBA route (no [switch] line for the 3rd, 5th and 7th window) + 2 ms ON arm: 4 + 4 windows, the 2 ms found (review B)
  3  no [switch] lines (an old real log) / tabled switch with no reader acknowledgement / only untabled lines (UNVERIFIED with --untabled-ok) (review C)
  4  fewer than 4 pairs / a window with no flip line / a window after step 15 + the route's bound (review D, G)
  5  200 seeded runs with per-window noise 0.5 ms: no effect -> "no difference" >= 95 %; +2 ms -> found >= 80 % (review D)
  6  frames file: a different run's frames_041.csv beside log 042 -> exit 2; a file whose frame times disagree with the log's summary -> exit 2 (review E)
  7  an int switch 2/4/2/4 gives a verdict (review H)
  8  the route generator: flag rewritten, lead-in, log_state, auto --first, 8 windows default, start-only switch refused (review F, G, H)
  9  the C++ log line shapes still match the verdict's regexes
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import ab_verdict
import switch_table

VERDICT = os.path.join(HERE, 'ab_verdict.py')
MAKE = os.path.join(HERE, 'make_ab_route.py')
fails = 0
GPU = 'GPU work (ms)'
RENDER = 'render thread (ms)'


def check(ok, what):
    global fails
    print('%s %s' % ('PASS' if ok else 'FAIL', what))
    if not ok:
        fails += 1


def is_plus2(text):
    m = re.match(r'ON higher by ([0-9.]+) ms', text)
    return bool(m) and abs(float(m.group(1)) - 2.0) < 0.15


def run(args):
    p = subprocess.run([sys.executable, '-I'] + args, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def synth(folder, arms, switch='wwe2k14_rdr_skip_prune', on_text='true', off_text='false', gpu_on=0.0, render_on=0.0, offset_sd=0.0, seed=7, frames_each=180,
          ack=True, with_csv=True, number='042', csv_number=None, steps=None, bound=None, untabled=False, frame_ms=16.667, drop_flip_for=()):
    """arms = ['ON','OFF',...]; one window per arm; returns the log path."""
    rng = random.Random(seed)
    log = os.path.join(folder, 'wwe2k14_%s.log' % number)
    csvp = os.path.join(folder, 'frames_%s.csv' % (csv_number or number))
    lines = ['[wwe2k14][frames] first guest frame (per-frame CSV: %s)' % csvp]
    if bound is not None:
        lines.append('[wwe2k14][input] line 8: ab: bound step15+%d (frame 100)' % bound)
    rows = ['frame,t_seconds,frame_ms,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,draw_rows,gpu_mhz,textures,texture_mb,warm_mb,gpu_wait_ms,gpu_wait_main,scene,presented,present_gap_ms,render_ms']
    frame, gen, prev = 200, 0, None
    n = len(arms)
    for w, arm in enumerate(arms):
        on = arm == 'ON'
        lines.append('[wwe2k14][input] line 20: ab: window %d/%d %s (frame %d)' % (w + 1, n, arm, frame))
        if steps:
            lines.append('[wwe2k14][input] line 21: state entrance_step_0 = %d (frame %d)' % (steps[w], frame))
        if prev != arm and (w + 1) not in drop_flip_for:
            gen += 1
            value = on_text if on else off_text
            suffix = ' (not in the table)' if untabled else ''
            lines.append('[wwe2k14][input] line 22: cvar %s = %s (frame %d) -> ok' % (switch, value, frame))
            lines.append('[switch] frame %d %s=%s%s' % (frame + 1, switch, value, suffix))
            if ack and not untabled:
                lines.append('[switch] seen frame %d gen %d %s=%s' % (frame + 3, gen, switch, switch_table.norm(value)))
        prev = arm
        offset = rng.gauss(0, offset_sd) if offset_sd else 0.0
        for _ in range(frames_each):
            gpu = 10.0 + rng.uniform(-0.2, 0.2) + offset + (gpu_on if on else 0.0)
            rend = 3.0 + rng.uniform(-0.1, 0.1) + (render_on if on else 0.0)
            swap = 5.0 + rng.uniform(-0.1, 0.1)
            gcell = '%.3f' % gpu if frame % 3 else ''   # gpu_ms is filled on some rows only
            rows.append('%d,%.3f,%.3f,%.3f,,%s,0,,0,0,0,0,,,1,16.6,%.3f' % (frame, frame / 60.0, frame_ms, swap, gcell, rend))
            frame += 1
    lines.append('[wwe2k14][input] line 90: scene=ab_end (frame %d)' % frame)
    lines.append('[wwe2k14][frames] t=10 s, frames 202-400: 60.0 fps, avg 16.7 ms, p95 17.0 ms, worst 20 ms, hitches 0')
    with open(log, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    if with_csv:
        with open(csvp, 'w') as f:
            f.write('\n'.join(rows) + '\n')
    return log


def vargs(log, **kw):
    d = dict(runlog=log, frames=None, switch=None, on='true', settle=30, tail=60, ci=98, untabled_ok=False)
    d.update(kw)
    return types.SimpleNamespace(**d)


def sides(out):
    got = {}
    for line in out.split('\n'):
        m = re.match(r'^(shown fps|GPU work \(ms\)|swap thread \(ms\)|render thread \(ms\))\s+(?:\S+\s+\S+\s+\S+\s+\S+|-\s+-\s+-\s+-)\s+(.*)$', line)
        if m:
            got[m.group(1)] = m.group(2).strip()
    return got


ALT = ['ON', 'OFF'] * 4
ABBA = ['ON', 'OFF', 'OFF', 'ON', 'ON', 'OFF', 'OFF', 'ON']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old-log', default=os.path.join(os.environ.get('WWE_MAIN', r'C:\Users\Jarrod\wwe2k14-recomp'), 'out', 's101', 'runs', '20261113_220106_s100_owner_match', 'run.log'))
    args = ap.parse_args()
    n_dirs = [0]

    def folder_in(tmp):
        n_dirs[0] += 1
        p = os.path.join(tmp, 'f%d' % n_dirs[0])
        os.makedirs(p)
        return p

    with tempfile.TemporaryDirectory(prefix='abv_') as tmp:
        # 1 + 2: the planted effects on the right side, the control reads nothing
        for name, arms, kw, want in (
            ('1a GPU +2 ms in ON', ALT, dict(gpu_on=2.0), {GPU: 'ON higher by 2.0'}),
            ('1b render thread +2 ms in ON', ALT, dict(render_on=2.0), {RENDER: 'ON higher by 2.0'}),
            ('1c a switch that does nothing', ALT, dict(), {}),
            ('2 ABBA route (no line for windows 3, 5, 7) + GPU +2 ms', ABBA, dict(gpu_on=2.0), {GPU: 'ON higher by 2.0'}),
        ):
            code, out, err = run([VERDICT, synth(folder_in(tmp), arms, **kw)])
            got = sides(out)
            check(code == 0 and len(got) == 4, '%s: the verdict ran and printed 4 sides (%s)' % (name, err.strip()[:120]))
            for side, text in got.items():
                if side in want:
                    check(is_plus2(text), '%s: %s reads "ON higher by ~2" (got "%s")' % (name, side, text))
                else:
                    check(text.startswith('no difference'), '%s: %s reads "no difference" (got "%s")' % (name, side, text))
        check('8 windows' in run([VERDICT, synth(folder_in(tmp), ABBA, gpu_on=2.0)])[1], '2 the ABBA run is read as 8 windows (4 ON + 4 OFF), cut at the route lines')
        # 3 refusals
        if os.path.isfile(args.old_log):
            code, out, err = run([VERDICT, args.old_log])
            check(code == 2 and 'no "[switch] frame N NAME=VALUE" line' in err and 'difference' not in out, '3 a real old run log with no [switch] lines fails loudly (exit %d)' % code)
        else:
            check(False, '3 the old run log %s is missing: the check cannot read its input' % args.old_log)
        code, out, err = run([VERDICT, synth(folder_in(tmp), ALT, gpu_on=2.0, ack=False)])
        check(code == 2 and 'never acknowledged by a reader' in err and 'higher' not in out, '3 a tabled switch no reader acknowledged fails loudly (exit %d)' % code)
        code, out, err = run([VERDICT, synth(folder_in(tmp), ALT, switch='wwe2k14_something_else', untabled=True, gpu_on=2.0)])
        check(code == 2 and 'not in the table' in err and 'higher' not in out, '3 an untabled-only log fails loudly (exit %d)' % code)
        code, out, err = run([VERDICT, '--untabled-ok', synth(folder_in(tmp), ALT, switch='wwe2k14_something_else', untabled=True, gpu_on=2.0)])
        check(code == 0 and 'UNVERIFIED' in out, '3 --untabled-ok prints the numbers as UNVERIFIED')
        # 4 other refusals
        code, out, err = run([VERDICT, synth(folder_in(tmp), ['ON', 'OFF', 'ON', 'OFF'], gpu_on=2.0)])
        check(code == 2 and 'pairs' in err and 'need >= 4' in err, '4 four windows (2 pairs) fail loudly (exit %d)' % code)
        code, out, err = run([VERDICT, synth(folder_in(tmp), ALT, gpu_on=2.0, drop_flip_for=(3,))])
        check(code == 2 and 'the flip did not happen' in err, '4 a window whose flip never logged fails loudly (exit %d)' % code)
        code, out, err = run([VERDICT, synth(folder_in(tmp), ALT, gpu_on=2.0, bound=300, steps=[5, 8, 11, 14, 20, 30, 40, 50])])
        check(code == 2 and 'step 15' in err and 'entrance is over' in err, '4 windows starting past step 15 + the route\'s bound fail loudly (exit %d)' % code)
        code, out, err = run([VERDICT, synth(folder_in(tmp), ALT, gpu_on=2.0, bound=300, steps=[5, 8, 11, 12, 13, 14, 15, 16])])
        check(code == 0, '4 the same bound with the entrance inside it passes (exit %d)' % code)
        # 6 frames-file rules
        f = folder_in(tmp)
        synth(f, ALT, number='041')            # a DIFFERENT run's frames_041.csv sits in the folder
        code, out, err = run([VERDICT, synth(f, ALT, number='042', with_csv=False)])
        check(code == 2 and 'no frames file belonging' in err, '6 log 042 with only frames_041.csv beside it fails loudly (exit %d)' % code)
        code, out, err = run([VERDICT, synth(folder_in(tmp), ALT, gpu_on=2.0, frame_ms=33.3)])
        check(code == 2 and 'not this run' in err, '6 a frames file whose frame times disagree with the log\'s summary fails loudly (exit %d)' % code)
        # 7 an int switch
        code, out, err = run([VERDICT, '--on', '4', synth(folder_in(tmp), ALT, switch='wwe2k14_rdr_max_queued', on_text='4', off_text='2', gpu_on=2.0)])
        check(code == 0 and is_plus2(sides(out).get(GPU, '')), '7 an int switch 4/2/4/2 gives a verdict (exit %d)' % code)
        # 5 the simulated error rates, in-process over 200 + 200 seeded runs
        null_ok = found = 0
        for seed in range(200):
            r = ab_verdict.verdict(vargs(synth(folder_in(tmp), ALT, offset_sd=0.5, seed=seed, frames_each=150)))
            null_ok += r['sides'][GPU]['verdict'].startswith('no difference')
            r = ab_verdict.verdict(vargs(synth(folder_in(tmp), ALT, offset_sd=0.5, seed=1000 + seed, frames_each=150, gpu_on=2.0)))
            found += r['sides'][GPU]['verdict'].startswith('ON higher')
        check(null_ok >= 190, '5 no effect, per-window noise 0.5 ms: "no difference" in %d of 200 runs (>= 190)' % null_ok)
        check(found >= 160, '5 +2 ms: found in %d of 200 runs (>= 160)' % found)
        # 8 the generator
        dest = os.path.join(tmp, 'bryan_ab.txt')
        code, out, err = run([MAKE, '--scene', 'bryan', '--switch', 'wwe2k14_rdr_skip_keeps_state', '--out', dest])
        text = open(dest).read() if code == 0 else ''
        cv = re.findall(r'^cvar (\S+) (\S+)$', text, re.M)
        check(code == 0 and [v for _, v in cv] == ['true', 'false'] * 4 and text.count('wait_frames 180') == 8 and 'mark 1 tron_324' in text and text.rstrip().endswith('quit'),
              '8 the Bryan route: 8 windows of 180 frames, ON first (the table default is off), starts at the tron, ends in quit')
        check('--vulkan_stored_pipelines_at_boot=true' in text and '--vulkan_stored_pipelines_at_boot=false' not in text.split('mark 1 ')[0], '8 the harness flag is rewritten to =true')
        first_cvar = text.index('cvar ')
        check('wait_frames 120' in text[:first_cvar] and text.count('log_state entrance_step_0') == 8 and 'log ab: bound step15+300' in text, '8 lead-in wait before the first cvar, log_state at each window, the bound logged')
        code, out, err = run([MAKE, '--scene', 'bryan', '--switch', 'wwe2k14_rdr_skip_prune', '--seconds', '15', '--out', os.path.join(tmp, 'x.txt')])
        check(code != 0 and 'shorten' in err, '8 8 windows of 15 s are refused for Bryan')
        code, out, err = run([MAKE, '--scene', 'bryan', '--switch', 'wwe2k14_rdr_skip_prune', '--windows', '4', '--out', os.path.join(tmp, 'x.txt')])
        check(code != 0 and 'fewer than 8' in err, '8 4 windows are refused (under 4 pairs)')
        code, out, err = run([MAKE, '--scene', 'match', '--switch', 'wwe2k14_rdr_skip_keeps_state', '--pattern', 'abba', '--out', os.path.join(tmp, 'm.txt')])
        cv = re.findall(r'^cvar (\S+) (\S+)$', open(os.path.join(tmp, 'm.txt')).read(), re.M) if code == 0 else []
        check(code == 0 and [v for _, v in cv] == ['true', 'false', 'false', 'true'] * 2 and 'log_state' not in open(os.path.join(tmp, 'm.txt')).read(), '8 the match route in ABBA order, no entrance state')
        code, out, err = run([MAKE, '--scene', 'bryan', '--switch', 'wwe2k14_rdr_max_queued', '--on', '4', '--off', '2', '--out', os.path.join(tmp, 'i.txt')])
        cv = re.findall(r'^cvar (\S+) (\S+)$', open(os.path.join(tmp, 'i.txt')).read(), re.M) if code == 0 else []
        check(code == 0 and [v for _, v in cv] == ['4', '2'] * 4, '8 an int switch (default 2): --on 4 --off 2 starts with 4, the value that CHANGES it')
        code, out, err = run([MAKE, '--scene', 'bryan', '--switch', 'wwe2k14_rdr_max_queued', '--out', os.path.join(tmp, 'j.txt')])
        check(code != 0 and '--on VALUE' in err, '8 an int switch without --on / --off is refused')
        code, out, err = run([MAKE, '--scene', 'bryan', '--switch', 'wwe2k14_rdr_crowd_ours', '--out', os.path.join(tmp, 'k.txt')])
        check(code != 0 and 'start-only' in err, '8 a start-only switch is refused (a mid-run flip would be refused by the game)')
        # the verdict reads the generator's own route shape: lines the route logs
        sample = '[2026-11-14 01:00:00] [info] [switch] frame 1234 wwe2k14_rdr_skip_prune=false'
        m = ab_verdict.SWITCH_RE.search(sample)
        check(bool(m) and m.group(1) == '1234' and m.group(3) == 'false', '9 the regex reads a logged line with the logger prefix')
        header = open(os.path.join(ROOT, 'src', 'renderer_adapter', 'rdr_switches.h'), encoding='utf-8').read()
        check('"[switch] frame %llu %s=%s%s"' in header and '"[switch] refused frame %llu %s=%s start-only' in header and '[switch] seen frame %llu gen %u %s=%lld' in header,
              '9 rdr_switches.h still writes the line shapes the verdict reads (switch, refused, seen)')
        check(ab_verdict.SEEN_RE.search('[switch] seen frame 105 gen 1 wwe2k14_rdr_skip_prune=0') is not None, '9 the seen regex reads the C++ shape')
    print('all passed' if not fails else 'FAILED (%d)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
