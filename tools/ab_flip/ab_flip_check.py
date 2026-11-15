#!/usr/bin/env python3
"""S102 lane M7 gate check (offline, 0 game runs, 0 Surface runs): the in-run A/B (--wwe2k14_ab_flip, src\\ab_flip_math.h) and its reader (tools\\surface\\flip_line.py).

  py -I tools\\ab_flip\\ab_flip_check.py [--root DIR]

1. Builds tools\\ab_flip\\ab_flip_test.cpp against the shipped src\\ab_flip_math.h and runs it (parser, PerFrame / bool / armed refusals, the anchor schedule). Planted copies of the header
   (out\\ab_flip\\plant_N): start-only check removed, table check removed, phase change not remembered, bool check removed, armed check removed. Each must make the test exit 1; a
   plant that no longer applies is its own failure (DID NOT APPLY).
2. Synthetic single runs (known effect -1.0 ms, drift +3 ms, settle transient, noise; the log in the SHIPPED format: true / false text in [switch] lines, [switch] seen lines, an nf census line,
   the anchor line, rel in the phase lines): recovered -1.0 +- 0.2, the naive difference is not. Twins: phase lines one phase off the switch lines, a missing switch line, a missing seen line, a
   seen line with the wrong value, labels swapped by a whole phase (now caught by the nf census), a half-phase shift (mixed: not -1.0), a null run.
3. Synthetic PAIRS (complementary runs) with strongly frame-varying content: recovered -1.0 +- 0.1 (gpu -0.6), CLEAR on shown fps; a single run alone is biased. Plants (review F1, F3, F4, F7):
   run 2's whole route (marks, content) shifted 30 frames AND 7 rows missing -> still -1.0 +- 0.15, and a copy of the reader that pairs by the ABSOLUTE frame must fail it; a run-level offset
   of 2.46 ms in run 2 with no effect and a clipped window (--rel 230:700) -> ~0 with a narrow interval (the old one-direction estimate read -0.65); an A/A pair -> NOT SEPARATED; 1000 null
   seeds of the block statistics -> false-CLEAR rate <= 7 % at 6 / 8 / 10 blocks; an asymmetric lag (the old phase's effect persists 15 rows) with settle 10 -> SETTLE SHORT; a slow-state lever
   -> SLOW STATE; the same schedule twice, a run with no anchor and a log shifted one phase are refused.
4. The past bug: flip_line.py --null-blocks on the old two-run job out\\s102\\ab8\\g4w3\\x (R19 round 3: B's whole run 2.46 ms slower in gpu_ms at EQUAL package power): read in place.
5. The job line: s0_5_jobs.ps1 -CheckOnly accepts a good AbFlip job and refuses the bad ones; bench.ps1's helper passes the non-reference values on the command line; every ab arm gets SceneFlags
   before its arm flags (review F8); the PerFrame audit (tools\\ab_flip\\audit_perframe.py) finds no armed switch outside kArmed.
A check that cannot read its input fails loudly."""
import argparse
import csv
import math
import os
import random
import re
import shutil
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
HERE = os.path.dirname(os.path.abspath(__file__))
SURF = os.path.join(HERE, '..', 'surface')
COLS = ('frame,t_seconds,frame_ms,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,draw_rows,gpu_mhz,textures,texture_mb,warm_mb,gpu_wait_ms,gpu_wait_main,scene,presented,'
        'present_gap_ms,rdr_submit_ms,gpu_work_ms,render_thread_ms,acquire_ms,present_call_ms,'
        'fs_reader_draw_ms,fs_reader_present_ms,fs_scene_end_ms,fs_replay_ms,fs_fence_wait_ms,fs_barrier_wait_ms,fs_rest_ms').split(',')
SW0, SW1 = 'wwe2k14_nf_replay_loop', 'wwe2k14_nf_render_dispatch'
PERIOD, NPH = 150, 8


class Fail(Exception):
    pass


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip()))) if out.returncode == 0 else None


# ---------------------------------------------------------------- part 1: the C++ test and its plants
def run_test(root, tag, header_text=None):
    d = os.path.join(root, 'out', 'ab_flip', tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, 'renderer_adapter'), exist_ok=True)
    shutil.copy(os.path.join(root, 'src', 'renderer_adapter', 'rdr_switches.h'), os.path.join(d, 'renderer_adapter', 'rdr_switches.h'))
    shutil.copy(os.path.join(root, 'src', 'ab_flip_math.h'), os.path.join(d, 'ab_flip_math.h'))
    if header_text is not None:
        with open(os.path.join(d, 'ab_flip_math.h'), 'w', encoding='utf-8', newline='') as f:
            f.write(header_text)
    exe = os.path.join(d, 'ab_flip_test.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-I', d, '-o', exe, os.path.join(root, 'tools', 'ab_flip', 'ab_flip_test.cpp')],
                       capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        raise Fail('ab_flip_test (%s) did not compile\n%s%s' % (tag, b.stdout, b.stderr))
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout


def part1(root):
    code, text = run_test(root, 'shipped')
    passes = len(re.findall(r'^PASS ', text, re.M))
    if code != 0 or 'all checks passed' not in text or re.search(r'^FAIL ', text, re.M) or passes < 30:
        raise Fail('the shipped parser test: exit %d, %d PASS lines (need >= 30)\n%s' % (code, passes, text))
    print('part 1: the parser / schedule test passed (%d checks)' % passes)
    src = open(os.path.join(root, 'src', 'ab_flip_math.h'), encoding='utf-8').read()
    plants = [
        ('start-only check removed', 'if (rdr_sw::Infos()[id].kind == rdr_sw::Kind::kStartOnly) return Fail(s, "\'" + name + "\' is start-only: a mid-run change would be refused");', ''),
        ('table check removed', 'if (id < 0) return Fail(s, "\'" + name + "\' is not in the switch table (rdr_switches.h): only table switches have a per-frame refresh");', ''),
        ('phase change not remembered', 'last_phase_no = no;', ''),
        ('bool check removed', 'value != "0" && value != "1" && value != "true" && value != "false"', 'false'),
        ('armed check removed', 'if (!IsArmed(kv.first)) continue;', 'continue;'),
        ('anchor ignored (absolute frames)', 'if (spec.anchor) return anchor_frame ? anchor_frame + s.start : 0;', ''),
    ]
    plants[5] = ('anchor ignored (absolute frames)', 'if (s.anchor) return anchor_frame ? anchor_frame + s.start : 0;', 'if (s.anchor) return anchor_frame ? 2 : 0;')
    for i, (what, old, new) in enumerate(plants, 1):
        if old not in src:
            raise Fail('plant %d (%s) DID NOT APPLY: the text it replaces is gone from src/ab_flip_math.h' % (i, what))
        code, text = run_test(root, 'plant_%d' % i, src.replace(old, new))
        if code == 0 or not re.search(r'^FAIL ', text, re.M):
            raise Fail('plant %d (%s) was NOT caught: the parser test passed on the broken header' % (i, what))
        print('part 1: planted twin %d (%s) caught: %s' % (i, what, re.search(r'^FAIL .*', text, re.M).group(0)))


# ---------------------------------------------------------------- synthetic runs
def clock(s):
    s = s % 86400
    return '%02d:%02d:%06.3f' % (s // 3600, s % 3600 // 60, s % 60)


def make_run(folder, effect=-1.0, gpu_effect=-0.6, replay_effect=-0.8, drift=3.0, flips=True, shift_log=0, shift_switch=0, drop_switch=None, drop_seen=None, wrong_seen=None,
             seed=7, first_on=0, content=None, slow=0.0, run_offset=0.0, off=0, lag=0, settle=20, anchor=True, drop_rel=(), census_wrong=False, bool_text=True):
    """A synthetic step folder in the SHIPPED log format. Row F belongs to the phase whose flip frame f0 < F (the flip at swap f0 applies to the NEXT frame). `off` moves the whole route
    (the anchor mark, every flip, the end, and the content) by that many guest frames (a different tron frame). run_offset = a whole-run slowdown (R19). lag = rows the old phase persists."""
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    rng = random.Random(seed)
    A = 101 + off
    total = A + NPH * PERIOD
    flip_frames = [A + k * PERIOD for k in range(NPH)]

    def kof(f):
        return (f - 1 - A) // PERIOD if f - 1 >= A else -1

    def ison(k):
        return k >= 0 and (k + first_on) % 2 == 1

    t, base_clock = 0.0, 22 * 3600 + 1 * 60 + 30.0
    rows, tt = [], {}
    for f in range(1, total + 1):
        k = kof(f)
        since = (f - 1 - flip_frames[k]) if k >= 0 else 999
        on = ison(k)
        if lag and k >= 0 and since < lag:
            on = ison(k - 1) if k > 0 else False
        cont = content(f - off) if content else 0.0
        ramp = min(1.0, since / (slow * PERIOD)) if (slow and 0 <= since < 999) else 1.0
        trans = max(0.0, 1.0 - since / 20.0) if 0 <= since < 20 else 0.0
        fm = 20.0 + cont + run_offset + drift * f / total + (effect * ramp if on else 0.0) + 4.0 * trans + rng.gauss(0, 1.5)
        gm = 15.0 + 0.6 * cont + run_offset + drift * 0.5 * f / total + (gpu_effect * ramp if on else 0.0) + 3.0 * trans + rng.gauss(0, 1.0)
        fr = 2.0 + (replay_effect if on else 0.0) + rng.gauss(0, 0.3)
        t += fm / 1000.0
        tt[f] = t
        if (f - A) in drop_rel:
            continue
        rows.append([f, '%.3f' % t, '%.3f' % fm, '%.3f' % (10 + rng.gauss(0, 0.5)), '', '%.3f' % gm, 0, '', 0, '0.0', 0, '0.00', '', '', 1, '%.3f' % fm, '', '', '%.3f' % (8 + rng.gauss(0, .4)), '', '',
                     '%.3f' % (3 + rng.gauss(0, .3)), '0.100', '0.100', '%.3f' % fr, '0.000', '%.3f' % (1 + rng.gauss(0, .2)), '%.3f' % (2 + rng.gauss(0, .3))])
    with open(os.path.join(folder, 'frames_1.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(COLS)
        w.writerows(rows)
    stamp = lambda fr: '[2026-11-14 %s] [info] [core] [t1] ' % clock(base_clock + tt[min(max(fr, 1), total)])
    spec = 'period=%d/settle=%d/%soff=%s=0+%s=0/on=%s=1+%s=1' % (PERIOD, settle, 'anchor=2/' if anchor else '', SW0, SW1, SW0, SW1) + ('/first=on' if first_on else '')
    lines = ['[2026-11-14 %s] [info] [core] [t1] [wwe2k14] settings file x.json: upscale 0, resolution 0, frame rate 60, renderer 1' % clock(base_clock - 5),
             '[2026-11-14 %s] [info] [core] [t1] [wwe2k14] quality tier: STANDARD' % clock(base_clock - 5)]
    lines.append(stamp(A) + '[wwe2k14][input] line 5: scene=heavy (frame %d)' % A)
    lines.append(stamp(A) + '[wwe2k14][input] mark 2 heavy (frame %d)' % A)
    val = (lambda v: 'true' if v else 'false') if bool_text else (lambda v: '1' if v else '0')
    if flips:
        lines.append(stamp(1) + '[ab_flip] on at frame 1: period %d settle %d anchor %d start 0 cycle %s (reference = off)' % (PERIOD, settle, 2 if anchor else 0, 'on,off' if first_on else 'off,on'))
        lines.append(stamp(1) + '[ab_flip] spec ' + spec)
        lines.append(stamp(2) + '[ab_flip] frame 2 phase off no -1 rel %d' % (2 - (A if anchor else 0)))
        if anchor:
            lines.append(stamp(A) + '[ab_flip] anchor mark 2 at frame %d' % A)
        for k, ff in enumerate(flip_frames):
            lf, sf = ff + shift_log, ff + shift_switch
            lines.append(stamp(lf) + '[ab_flip] frame %d phase %s no %d rel %d' % (lf, 'on' if ison(k) else 'off', k, lf - (A if anchor else 0)))
            changed = ison(k) != (ison(k - 1) if k > 0 else False)
            if changed and (drop_switch is None or k != drop_switch):
                for sw in (SW0, SW1):
                    lines.append(stamp(sf) + '[switch] frame %d %s=%s' % (sf, sw, val(ison(k))))
            if changed and (drop_seen is None or k != drop_seen):
                for sw in (SW0, SW1):
                    v = ison(k) if wrong_seen is None or k != wrong_seen else not ison(k)
                    lines.append(stamp(sf + 3) + '[switch] seen frame %d gen %d %s=%d' % (sf + 3, k + 1, sw, 1 if v else 0))
        G = A + 3 * PERIOD + 80
        lv = 1 if ison(3) else 0
        if census_wrong:
            lv = 1 - lv
        lines.append(stamp(G) + '[rdr][nf_replay] frames 600, taken 1, declined 0 (by reason: x); guest frame %d, loop %d, dispatcher %d' % (G, lv, lv))
    lines.append(stamp(total) + '[wwe2k14][input] line 9: scene=end (frame %d)' % total)
    lines.sort(key=lambda s: s[:30])
    with open(os.path.join(folder, 'wwe2k14_1.log'), 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    with open(os.path.join(folder, 'power.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['time', 't_s', 'cpu_nominal_mhz', 'cpu_perf', 'cpu_utility', 'gpu3d_pct', 'perf_limit', 'perf_limit_flags', 'rapl_dram', 'rapl_dram_pwh', 'rapl_pkg'])
        for i in range(int(tt[total]) + 12):
            w.writerow([clock(base_clock - 5 + i), i, 1600, 100, 50, 50, 88, 2, 0, 0, '%.2f' % (16500 - 1200 * i / (tt[total] + 12) + rng.gauss(0, 80))])


def run_reader(folder, extra=(), reader=None):
    p = subprocess.run([sys.executable, '-I', reader or os.path.join(SURF, 'flip_line.py'), folder] + list(extra), capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout + p.stderr


def run_pair(a, b, extra=(), reader=None):
    return run_reader(a, ['--pair', b] + list(extra), reader)


def pick(out, metric):
    m = re.search(r'^%s\s+on - off: ([+-][\d.]+)\s+t95 \[([+-][\d.]+), ([+-][\d.]+)\].*?naive whole-run ([+-][\d.]+)' % metric, out, re.M)
    if not m:
        raise Fail('flip_line.py printed no %s line:\n%s' % (metric, out))
    return float(m.group(1)), float(m.group(4)), (float(m.group(2)), float(m.group(3)))


def pickp(out, metric):
    m = re.search(r'^%s\s+on - off: ([+-][\d.]+)(?:\s+t95 \[([+-][\d.]+), ([+-][\d.]+)\])?' % metric, out, re.M)
    if not m:
        raise Fail('flip_line.py --pair printed no %s line:\n%s' % (metric, out))
    return float(m.group(1)), (float(m.group(2)), float(m.group(3))) if m.group(2) else None


def content_fn(f):
    """Content that varies strongly by (route) frame, the same in both runs of a pair: a bump, a step and spikes."""
    return 6.0 * math.exp(-((f - 600) / 120.0) ** 2) + (4.0 if 800 <= f < 950 else 0.0) + (10.0 if f % 53 < 2 else 0.0)


def part2(root):
    base = os.path.join(root, 'out', 'ab_flip', 'synth')
    f = os.path.join(base, 'clean')
    make_run(f)
    code, out = run_reader(f)
    if code != 0:
        raise Fail('clean synthetic run: exit %d\n%s' % (code, out))
    d, naive, ci = pick(out, 'frame_ms')
    g, _, _ = pick(out, 'gpu_ms')
    r, _, _ = pick(out, 'fs_replay_ms')
    print('part 2: single run, known effect on = off - 1.0 ms (drift +3 ms, settle transient, noise; log with true / false, seen lines, census): recovered %+.2f (t95 %s), naive whole-run %+.2f; gpu_ms %+.2f (known -0.6); fs_replay_ms %+.2f (known -0.8)' % (d, ci, naive, g, r))
    if abs(d + 1.0) > 0.2 or abs(g + 0.6) > 0.2 or abs(r + 0.8) > 0.2:
        raise Fail('the reader did not recover the planted effect (frame_ms %+.2f, gpu_ms %+.2f, fs_replay_ms %+.2f)' % (d, g, r))
    if abs(naive + 1.0) <= 0.2:
        raise Fail('the naive whole-run difference (%+.2f) is within 0.2 of the truth: the planted drift does not separate the two' % naive)
    if 'MISMATCH' in out:
        raise Fail('the clean run (true / false text in the [switch] lines) must not MISMATCH (review F2):\n' + out)
    f = os.path.join(base, 'clean_int_text')
    make_run(f, bool_text=False)
    code, out = run_reader(f)
    if code != 0 or 'MISMATCH' in out:
        raise Fail('the same run with 1 / 0 in the [switch] lines must not MISMATCH either:\n' + out)
    print('part 2: [switch] text true / false AND 1 / 0 both read clean (F2)')
    twins = [
        ('twin (a) phase lines one phase off the switch lines', dict(shift_log=150), 'became'),
        ('twin (b) a flip with no [switch] line', dict(drop_switch=3), 'no "[switch] frame'),
        ('twin (c) a flip no reader acknowledged (no seen line)', dict(drop_seen=3), 'seen'),
        ('twin (d) a seen line with the wrong value', dict(wrong_seen=3), 'seen'),
        ('twin (e) labels swapped by a whole phase in both logs (the nf census at guest frame G still shows the truth)', dict(shift_log=150, shift_switch=150), 'nf census'),
        ('twin (f) the census says the other value', dict(census_wrong=True), 'nf census'),
    ]
    for what, kw, needle in twins:
        f = os.path.join(base, 'twin_' + what.split(')')[0].split('(')[1])
        make_run(f, **kw)
        code, out = run_reader(f)
        if code != 3 or 'MISMATCH' not in out or needle not in out or re.search(r'^frame_ms', out, re.M):
            raise Fail('%s was not refused as MISMATCH with %r (exit %d):\n%s' % (what, needle, code, out))
        print('part 2: %s: exit 3, %s' % (what, re.search(r'^MISMATCH: .*', out, re.M).group(0)[:120]))
    f = os.path.join(base, 'twin_half')
    make_run(f, shift_log=75, shift_switch=75)
    code, out = run_reader(f)
    if code == 0:
        d2, _, _ = pick(out, 'frame_ms')
        if abs(d2 + 1.0) <= 0.3:
            raise Fail('twin: both logs half a phase off still read %+.2f (the check is blind)' % d2)
        print('part 2: twin both logs half a phase off: reads %+.2f, not the true -1.0' % d2)
    else:
        print('part 2: twin both logs half a phase off: refused (exit %d)' % code)
    f = os.path.join(base, 'null')
    make_run(f, effect=0.0, gpu_effect=0.0, replay_effect=0.0, flips=False)
    code, out = run_reader(f, ['--null-blocks', '30'])
    d4, _, _ = pick(out, 'frame_ms')
    if code != 0 or abs(d4) >= 0.3:
        raise Fail('the null run (drift only, no effect) read %+.2f with --null-blocks 30 (exit %d)' % (d4, code))
    print('part 2: null run (drift +3 ms, no effect, alternating 30-frame blocks): delta %+.2f (< 0.3)' % d4)


def reader_plant(root, tag, old, new):
    """A copy of flip_line.py (and bench_line.py beside it) with one text replaced: the reader as it would be with that bug."""
    d = os.path.join(root, 'out', 'ab_flip', 'plant_reader_' + tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for n in ('flip_line.py', 'bench_line.py', 'heat_rules.py'):
        shutil.copy(os.path.join(SURF, n), os.path.join(d, n))
    p = os.path.join(d, 'flip_line.py')
    t = open(p, encoding='utf-8').read()
    if old not in t:
        raise Fail('reader plant %s DID NOT APPLY: %r is gone from flip_line.py' % (tag, old))
    open(p, 'w', encoding='utf-8').write(t.replace(old, new))
    return p


def part3(root):
    base = os.path.join(root, 'out', 'ab_flip', 'synth')
    a, b = os.path.join(base, 'pair_1'), os.path.join(base, 'pair_2')
    make_run(a, content=content_fn, seed=7, first_on=0)
    make_run(b, content=content_fn, seed=11, first_on=1)
    code, out = run_pair(a, b)
    if code != 0:
        raise Fail('paired synthetic job: exit %d\n%s' % (code, out))
    d, _ = pickp(out, 'frame_ms')
    gd, _ = pickp(out, 'gpu_ms')
    sf, sci = pickp(out, 'shown_fps')
    singles = [float(x) for x in re.findall(r'^single run run \d alone, frame_ms: within-run adjacent-phase delta ([+-][\d.]+)', out, re.M)]
    print('part 3: complementary pair, content varying by frame, known effect -1.0 / -0.6: frame_ms %+.2f, gpu %+.2f, shown fps %+.2f (%s); each run alone: %s' % (d, gd, sf, sci, singles))
    if abs(d + 1.0) > 0.1 or abs(gd + 0.6) > 0.15:
        raise Fail('the paired reader did not recover the planted effect (frame_ms %+.2f, gpu_ms %+.2f)' % (d, gd))
    if len(singles) != 2 or max(abs(x + 1.0) for x in singles) <= 0.3:
        raise Fail('no single run is biased by the content (%s): the test proves nothing' % singles)
    if sf <= 0 or 'SLOW STATE' in out or 'SETTLE SHORT' in out or not re.search(r'^VERDICT shown_fps on-off: CLEAR', out, re.M):
        raise Fail('the clean pair should be CLEAR on shown fps (fps goes up when frame_ms goes down) without warnings:\n' + out)
    # F1: run 2's whole route shifted 30 frames + 7 rows missing
    c2 = os.path.join(base, 'pair_shift')
    make_run(c2, content=content_fn, seed=11, first_on=1, off=30, drop_rel=range(300, 307))
    code, out = run_pair(a, c2)
    d1, _ = pickp(out, 'frame_ms')
    print('part 3: F1 run 2 route shifted 30 frames (marks + content) and 7 rows missing: paired by the anchor reads %+.2f' % d1)
    if code != 0 or abs(d1 + 1.0) > 0.15:
        raise Fail('with the route shifted 30 frames the anchored pair must still recover -1.0 +- 0.15 (read %+.2f, exit %d)' % (d1, code))
    pr = reader_plant(root, 'absolute', "sel = [dict(r, rel=r['frame'] - A) for r in rows", "sel = [dict(r, rel=r['frame']) for r in rows")
    code, out = run_pair(a, c2, reader=pr)
    if code == 0:
        d2, _ = pickp(out, 'frame_ms')
        if abs(d2 + 1.0) <= 0.15:
            raise Fail('a reader pairing by the ABSOLUTE frame read %+.2f on the shifted route: the plant does not separate the two' % d2)
        print('part 3: F1 plant: the same job read by absolute frame reads %+.2f (wrong): the anchor is what saves it' % d2)
    else:
        print('part 3: F1 plant: a reader pairing by the absolute frame refuses or fails the shifted job (exit %d)' % code)
    # F3: a run-level offset of 2.46 ms in run 2, no effect, window clipped to rel 230..700
    o1, o2 = os.path.join(base, 'off_1'), os.path.join(base, 'off_2')
    make_run(o1, effect=0.0, gpu_effect=0.0, replay_effect=0.0, seed=7, first_on=0)
    make_run(o2, effect=0.0, gpu_effect=0.0, replay_effect=0.0, seed=11, first_on=1, run_offset=2.46)
    for rel in (None, '30:1100', '230:700', '230:560'):
        code, out = run_pair(o1, o2, ['--rel', rel] if rel else [])
        d3, ci3 = pickp(out, 'frame_ms')
        print('part 3: F3 run-level offset 2.46 ms in run 2, true effect 0, rel window %s: crossover %+.2f %s' % (rel or 'all', d3, ci3))
        if code != 0 or abs(d3) > 0.3:
            raise Fail('the run offset must cancel (read %+.2f with window %s)' % (d3, rel))
        if rel in (None, '30:1100') and ci3 and ci3[1] - ci3[0] > 1.5:
            raise Fail('the interval with window %s is far too wide (%s)' % (rel, ci3))
        if rel in ('230:700', '230:560') and 'TOO FEW BLOCKS' not in out:
            raise Fail('a window with fewer than 6 blocks must say TOO FEW BLOCKS (window %s):\n%s' % (rel, out))
    # A/A
    aa1, aa2 = os.path.join(base, 'aa_1'), os.path.join(base, 'aa_2')
    make_run(aa1, effect=0.0, gpu_effect=0.0, replay_effect=0.0, content=content_fn, seed=21, first_on=0)
    make_run(aa2, effect=0.0, gpu_effect=0.0, replay_effect=0.0, content=content_fn, seed=22, first_on=1)
    code, out = run_pair(aa1, aa2)
    if code != 0 or not re.search(r'^VERDICT shown_fps on-off: NOT SEPARATED', out, re.M) or not re.search(r'^VERDICT frame_ms on-off: NOT SEPARATED', out, re.M):
        raise Fail('an A/A pair must read NOT SEPARATED:\n' + out)
    print('part 3: A/A pair (no effect, content, drift): NOT SEPARATED on shown fps and frame_ms')
    # F4: 1000 null seeds of the block statistics
    sys.path.insert(0, SURF)
    import flip_line as fl
    for nb in (6, 8, 10):
        clear = 0
        rng = random.Random(100 + nb)
        for _ in range(1000):
            blocks = []
            for k in range(nb):
                dr = 1 if k % 2 == 0 else 2
                c = 2.46 * (1 if dr == 1 else -1)                      # the run-level offset enters the two directions with opposite signs
                blocks.append((k, dr, c + sum(rng.gauss(0, 2.83) for _ in range(60)) / 60 + rng.gauss(0, 0.25)))
            if fl.verdict_of(fl.crossover(blocks), 'on', 'off', 'ms').startswith('CLEAR'):
                clear += 1
        print('part 3: F4 1000 null seeds, %d blocks (with a 2.46 ms run offset): false CLEAR %.1f %%' % (nb, clear / 10.0))
        if clear > 70:
            raise Fail('false-CLEAR rate %.1f %% at %d blocks (limit 7 %%)' % (clear / 10.0, nb))
    few = fl.verdict_of(fl.crossover([(k, 1 + k % 2, 0.5) for k in range(4)]), 'on', 'off', 'ms')
    if not few.startswith('TOO FEW BLOCKS'):
        raise Fail('4 blocks must read TOO FEW BLOCKS, got: ' + few)
    # F6 / settle profile: the old phase's effect persists 15 rows, settle 10
    l1, l2 = os.path.join(base, 'lag_1'), os.path.join(base, 'lag_2')
    make_run(l1, content=content_fn, seed=7, first_on=0, lag=15, settle=10, effect=-2.5, gpu_effect=-1.5)
    make_run(l2, content=content_fn, seed=11, first_on=1, lag=15, settle=10, effect=-2.5, gpu_effect=-1.5)
    code, out = run_pair(l1, l2)
    if code != 0 or 'SETTLE SHORT' not in out:
        raise Fail('an asymmetric lag of 15 rows with settle 10 must raise SETTLE SHORT (exit %d):\n%s' % (code, out))
    print('part 3: asymmetric lag (the old phase persists 15 rows), settle 10: ' + re.search(r'settle lag: [^\n]*', out).group(0) + ' -> SETTLE SHORT raised')
    code, out = run_pair(a, b)
    lg = re.search(r'settle lag: the delta crosses zero at row (\d+)', out)
    print('part 3: clean pair, settle 20: ' + (lg.group(0) if lg else 'no lag line'))
    if not lg or int(lg.group(1)) > 4:
        raise Fail('the clean pair should show a lag of 4 rows or less:\n' + out)
    # slow state
    s1, s2 = os.path.join(base, 'slow_1'), os.path.join(base, 'slow_2')
    make_run(s1, content=content_fn, seed=7, first_on=0, slow=0.8)
    make_run(s2, content=content_fn, seed=11, first_on=1, slow=0.8)
    code, out = run_pair(s1, s2)
    if code != 0 or 'SLOW STATE' not in out:
        raise Fail('a slow-state lever did not raise SLOW STATE (exit %d):\n%s' % (code, out))
    print('part 3: slow-state twin: ' + re.search(r'SLOW STATE: [^|]*', out).group(0)[:120])
    # refusals
    code, out = run_pair(a, a)
    if code != 2 or 'not complementary' not in out:
        raise Fail('two runs with the SAME schedule were not refused (exit %d):\n%s' % (code, out))
    na = os.path.join(base, 'noanchor_1')
    make_run(na, anchor=False)
    code, out = run_pair(na, b)
    if code != 2 or 'anchor' not in out:
        raise Fail('a run with no anchor was not refused (exit %d):\n%s' % (code, out))
    h = os.path.join(base, 'pair_badlog')
    make_run(h, content=content_fn, seed=11, first_on=1, shift_log=150)
    code, out = run_pair(a, h)
    if code != 3 or 'MISMATCH' not in out:
        raise Fail('run 2 logged one phase off its switch lines was not refused as MISMATCH (exit %d):\n%s' % (code, out))
    print('part 3: refused: same schedule twice (exit 2), no anchor (exit 2), a log shifted one phase (exit 3)')


def part4(root):
    main = main_checkout(root) or root
    old = os.path.join(main, 'out', 's102', 'ab8', 'g4w3', 'x')
    if not os.path.isdir(old):
        raise Fail('the old job %s is missing (needs out/s102/ab8/g4w3/x): the past-bug re-find cannot be read' % old)
    lvl, nulls = {}, {}
    for rd in (1, 2, 3):
        for arm in 'AB':
            folder = os.path.join(old, 'ab_%d_%s' % (rd, arm))
            code, out = run_reader(folder, ['--null-blocks', '30', '--quiet'])
            if code != 0:
                raise Fail('flip_line --null-blocks on %s: exit %d\n%s' % (folder, code, out))
            m = re.search(r'^gpu_ms\s+on - off: ([+-][\d.]+).*?off level ([\d.]+)', out, re.M)
            p = re.search(r'^pkg_w\s+.*?off level ([\d.]+)', out, re.M)
            if not m or not p:
                raise Fail('no gpu_ms / pkg_w line for %s:\n%s' % (folder, out))
            nulls[(rd, arm)] = float(m.group(1))
            lvl[(rd, arm)] = (float(m.group(2)), float(p.group(1)))
    print('part 4: old two-run job g4w3 (R19), each run read in place, alternating 30-frame null blocks:')
    for rd in (1, 2, 3):
        da = lvl[(rd, 'B')][0] - lvl[(rd, 'A')][0]
        dp = lvl[(rd, 'B')][1] - lvl[(rd, 'A')][1]
        print('  round %d: whole-run gpu_ms level A %.2f B %.2f (B - A %+.2f ms) at package power A %.2f B %.2f W (%+.2f W) | in-run null delta A %+.2f B %+.2f ms'
              % (rd, lvl[(rd, 'A')][0], lvl[(rd, 'B')][0], da, lvl[(rd, 'A')][1], lvl[(rd, 'B')][1], dp, nulls[(rd, 'A')], nulls[(rd, 'B')]))
    da3 = lvl[(3, 'B')][0] - lvl[(3, 'A')][0]
    dp3 = lvl[(3, 'B')][1] - lvl[(3, 'A')][1]
    if da3 < 1.5 or abs(dp3) > 0.5:
        raise Fail('round 3 no longer reads as a whole-run step at equal power (gpu step %+.2f ms, power step %+.2f W)' % (da3, dp3))
    if any(abs(v) > 1.0 for v in nulls.values()):
        raise Fail('an old run has an in-run null gpu_ms delta above 1.0 ms: %s' % nulls)
    print('part 4: round 3 B is %.2f ms slower as a whole at %.2f W apart (the old power gate calls that pair matched); every run\'s in-run delta is within +-1 ms, so an in-run flip inside it '
          'would have read ~0 where the two-run job read the step. The old job has no flips: what is shown is that the offset is run-level, and an interleaved design cancels it.' % (da3, abs(dp3)))


def part5(root):
    """The job line, the bench helper, F8's SceneFlags, and the PerFrame audit."""
    ok_spec = 'period=100/settle=15/anchor=2/off=%s=0/on=%s=1' % (SW0, SW0)

    def job(spec_tail, extra=''):
        return 'flip1|Only=ab;AbScene=frames_bryan_heavy;AbRounds=1;FrameSplit=true;SceneFlags=@BASE%s;AbFlip=%s' % (extra, spec_tail)
    cases = [
        (True, job(ok_spec), 'a good AbFlip job'),
        (True, job('period=100/settle=15/anchor=2/off=%s=0/on=%s=1/order=off:on' % (SW0, SW0)), 'order with a colon (a comma cannot pass the bench)'),
        (False, job('period=150/anchor=2/off=wwe2k14_rdr_copy_alias=0/on=wwe2k14_rdr_copy_alias=1'), 'a start-only switch'),
        (False, job('period=150/anchor=2/off=wwe2k14_no_such=0/on=wwe2k14_no_such=1'), 'a name outside the table'),
        (False, job('period=150/anchor=2/off=wwe2k14_rdr_lazy_banks=false/on=wwe2k14_rdr_lazy_banks=true'), 'a phase that turns a kept (@BASE) lever off'),
        (False, job(ok_spec + '/first=on'), 'first= written by hand'),
        (False, job('period=100/off=%s=0/on=%s=1' % (SW0, SW0)), 'no anchor'),
        (False, job(ok_spec, ';ArmAFlags=wwe2k14_rdr_lazy_banks=true'), 'ArmAFlags beside AbFlip'),
        (False, job('period=150/anchor=2/off=%s=0' % SW0), 'one phase only'),
    ]
    for want_ok, line, what in cases:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'surface', 's0_5_jobs.ps1'), '-Out', os.path.join(root, 'out', 'ab_flip', 'jobs_out'), '-CheckOnly', '-Jobs', line],
                           capture_output=True, text=True, cwd=root)
        text = p.stdout + p.stderr
        good = (p.returncode == 0 and 'job lines ok' in text) if want_ok else (p.returncode != 0 and ('AbFlip' in text or 'bad job line' in text))
        if not good:
            raise Fail('job line "%s": expected %s, got exit %d:\n%s' % (what, 'accepted' if want_ok else 'refused', p.returncode, text[-600:]))
        print('part 5: job line, %s: %s' % (what, 'accepted' if want_ok else 'refused'))
    b = open(os.path.join(root, 'tools', 'surface', 'bench.ps1'), encoding='utf-8', errors='replace').read()
    if "$(if ($e[1] -eq 'B') { '/first=on' } else { '' })" not in b:
        raise Fail("bench.ps1 no longer gives arm B '/first=on'")
    # F5: the helper passes the non-reference values (run the function text in pwsh)
    m = re.search(r'function Get-AbFlipCli.*?\n}\n', b, re.S)
    if not m:
        raise Fail('bench.ps1 has no Get-AbFlipCli')
    script = m.group(0) + "\n(Get-AbFlipCli 'period=100/anchor=2/off=a_x=0+b_y=0/on=a_x=1+b_y=true') -join ' '\n(Get-AbFlipCli 'period=100/anchor=2/off=a_x=1/on=a_x=0/order=on:off') -join ' '"
    p = subprocess.run(['pwsh', '-NoProfile', '-Command', script], capture_output=True, text=True)
    lines = p.stdout.strip().splitlines()
    if p.returncode or lines[:1] != ['--a_x=1 --b_y=true'] or lines[1:2] != ['--a_x=0']:
        raise Fail('Get-AbFlipCli did not return the non-reference values (--a_x=1 --b_y=true / --a_x=0): %r %s' % (p.stdout, p.stderr))
    print('part 5: bench.ps1 passes the non-reference values on the command line (F5): %s' % lines[0])
    # F8: every ab arm gets the SceneFlags before its arm flags
    fl = re.search(r'\n\s*\$fl = [^\n]*', b).group(0)
    if '$abSceneFlags' not in fl or fl.index('$abSceneFlags') > fl.index('$armBFlagList') or fl.index('$abSceneFlags') > fl.index('$armSpec'):
        raise Fail('bench.ps1: the ab arms do not get SceneFlags before their arm flags:\n' + fl)
    if not re.search(r'\$abSceneFlags = @\(\$SceneFlags', b):
        raise Fail('bench.ps1: $abSceneFlags is not built from SceneFlags')
    print('part 5: every ab arm gets SceneFlags before its arm flags (F8)')
    # audit: no armed switch outside kArmed
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'audit_perframe.py'), '--root', root], capture_output=True, text=True, timeout=600)
    if p.returncode or 'PerFrame' not in p.stdout:
        raise Fail('audit_perframe.py failed: ' + p.stdout[-300:] + p.stderr[-300:])
    hdr = open(os.path.join(root, 'src', 'ab_flip_math.h'), encoding='utf-8').read()
    for line in p.stdout.splitlines():
        m = re.match(r'(wwe2k14_\w+) \| outside the table: ([^|]*)\|', line)
        if m and re.search(r'(reader|device)\.(h|cpp):\d+', m.group(2)) and m.group(1) not in hdr:
            raise Fail('the audit found %s read at start in reader.h / device.cpp (%s) but it is not in kArmed' % (m.group(1), m.group(2).strip()))
    print('part 5: the PerFrame audit finds no reader.h / device.cpp start-time read outside kArmed')


def part6(root):
    """Real Surface data (ab14, 2026-11-15 02:0x; logs and csvs of our own counters, scene column blanked): the knock-out pair must read CLEAR, large, right sign (the reader once REFUSED it: the
    shorter run's last phase was cut by the quit); the A/A pair NOT SEPARATED; a flip frame moved in one log is refused with the first differing entry. And bench.ps1's real `$fl` line is built
    under Windows PowerShell 5.1 (powershell.exe), where a one-item array unrolls to a string (the first real job passed `--x=1--wwe2k14_ab_flip=...`)."""
    fx = os.path.join(root, 'tools', 'ab_flip', 'fixtures')
    a, b = os.path.join(fx, 'flipko', 'ab_1_A'), os.path.join(fx, 'flipko', 'ab_1_B')
    for p in (a, b):
        if not os.path.isdir(p):
            raise Fail('fixture %s is missing' % p)
    code, out = run_pair(a, b)
    d, ci = pickp(out, 'frame_ms')
    sf, _ = pickp(out, 'shown_fps')
    print('part 6: real knock-out pair (ko_draw_none, period 60, settle 15, run 2 one phase shorter): frame_ms %+.2f %s, shown fps %+.2f' % (d, ci, sf))
    if code != 0 or not re.search(r'^VERDICT shown_fps on-off: CLEAR', out, re.M) or not (-9.0 < d < -4.0) or sf < 8:
        raise Fail('the knock-out pair must read CLEAR, large (-9..-4 ms, > +8 fps), right sign (exit %d):\n%s' % (code, out))
    code, out = run_pair(os.path.join(fx, 'flipaa', 'ab_1_A'), os.path.join(fx, 'flipaa', 'ab_1_B'))
    if code != 0 or len(re.findall(r'^VERDICT \w+ on-off: NOT SEPARATED', out, re.M)) < 3 or 'CLEAR' in out:
        raise Fail('the A/A pair must read NOT SEPARATED on every verdict (exit %d):\n%s' % (code, out))
    print('part 6: real A/A pair: NOT SEPARATED on every verdict')
    t = os.path.join(root, 'out', 'ab_flip', 'fx_twin')
    shutil.rmtree(t, ignore_errors=True)
    shutil.copytree(os.path.join(fx, 'flipko'), t)
    lg = [x for x in os.listdir(os.path.join(t, 'ab_1_B')) if x.endswith('.log')][0]
    p = os.path.join(t, 'ab_1_B', lg)
    s = open(p, encoding='utf-8').read()
    if 'phase on no 4 rel 240' not in s and 'phase off no 4 rel 240' not in s:
        raise Fail('the fixture twin DID NOT APPLY: no phase 4 at rel 240 in the log')
    open(p, 'w', encoding='utf-8').write(re.sub(r'(phase \w+ no 4 rel )240', r'\g<1>241', s))
    code, out = run_pair(os.path.join(t, 'ab_1_A'), os.path.join(t, 'ab_1_B'))
    if code != 2 or 'first differing entry: phase 4 flips at rel 240 in run 1 and rel 241 in run 2' not in out:
        raise Fail('a flip frame moved in one log must be refused naming the first differing entry (exit %d):\n%s' % (code, out))
    print('part 6: a flip moved one frame in one log: REFUSED, first differing entry named')
    # bench.ps1's real argument line under Windows PowerShell 5.1
    b_txt = open(os.path.join(root, 'tools', 'surface', 'bench.ps1'), encoding='utf-8', errors='replace').read()
    fn = re.search(r'function Get-AbFlipCli.*?\n}\n', b_txt, re.S)
    line = re.search(r'\n\s*(\$fl = [^\n]*)', b_txt)
    if not fn or not line:
        raise Fail('bench.ps1: Get-AbFlipCli or the $fl line not found')
    stub = ("$sceneSteps = @{ 'frames_bryan_heavy' = @('r.txt', @('--wwe2k14_launch=x')) }; $AbScene = 'frames_bryan_heavy'; $abSceneFlags = @('--wwe2k14_rdr_lazy_banks=true'); $armSpec = $null; "
            "$armAFlagList = @(); $armBFlagList = @(); $FrameSplit = $true; $AbFlip = 'period=60/settle=15/anchor=2/off=wwe2k14_rdr_ko_draw_none=0/on=wwe2k14_rdr_ko_draw_none=1'\n")
    ps = shutil.which('powershell.exe') or shutil.which('powershell')
    if not ps:
        raise Fail('powershell.exe (Windows PowerShell 5.1) not found: the 5.1 argument test cannot run')
    for arm in ('A', 'B'):
        script = stub + fn.group(0) + "\n$e = @('x', '%s')\n" % arm + line.group(1) + "\n$fl.Count\n$fl -join ' | '\n"
        sp = os.path.join(root, 'out', 'ab_flip', 'ps51_%s.ps1' % arm)
        open(sp, 'w', encoding='utf-8').write(script)
        r = subprocess.run([ps, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', sp], capture_output=True, text=True)
        outl = r.stdout.strip().splitlines()
        if r.returncode or not outl or outl[0] != '5' or not any('--wwe2k14_rdr_ko_draw_none=1 | --wwe2k14_ab_flip=' in x for x in outl):
            raise Fail('under powershell.exe 5.1 the ab arm %s argument list is wrong (want 5 items, the value and the spec separate): %s %s' % (arm, r.stdout, r.stderr))
    print('part 6: bench.ps1 arm arguments under Windows PowerShell 5.1: 5 separate items for both arms (the bug joined two of them)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    root = os.path.abspath(ap.parse_args().root)
    if not os.path.isfile(CLANG):
        print('FAIL: %s not found' % CLANG)
        return 1
    try:
        part1(root)
        part2(root)
        part3(root)
        part4(root)
        part5(root)
        part6(root)
    except Fail as e:
        print('FAIL: %s' % e)
        return 1
    print('ab_flip check: all parts passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
