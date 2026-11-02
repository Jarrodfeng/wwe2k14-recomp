r"""The owned frame's GPU pieces (Session 82): the [native][pass] lines of logged frames, truth passes and owned work
(kind 16, "owned LABEL": pass, box_copy, copy_in_col, copy_in_ds, snapshot, depth_fill, page_fill, rect_copy) together,
per piece GPU ms per frame; owned frame on / off side by side (each side one run or several, "003,005").

    py -I tools\native\owned_pieces.py ON_RUNS OFF_RUNS [--dir LOGDIR] [--top N]
    py -I tools\native\owned_pieces.py --frames RUN [--dir LOGDIR]       one run's pieces, in recording order

A truth pass is keyed by its size and opening pixel shader; an owned piece by its label and size. "gap" = the GPU time
between timed segments (barriers, copies nothing times) without the time the GPU sat idle waiting for a submission
(the log's "submission started ... into the gap" lines): busy total = segments + busy gaps.
"""
import argparse
import os
import re
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PASS = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+)x(\d+) pitch (\d+) colour kind (\d+) depth (\d) '
                  r'draws (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+): ([0-9.]+) ms \(before it ([0-9.]+) ms\)(?: owned (\S+))?')
TOTAL = re.compile(r'\[native\]\[pass\] frame (\d+): (\d+) timed segments, ([0-9.]+) ms in them, ([0-9.]+) ms between '
                   r'them, ([0-9.]+) ms first to last')
IDLE = re.compile(r'\[native\]\[pass\] frame (\d+): (\d+) submissions; the GPU waited for one in (\d+) gaps, ([0-9.]+) ms of the')
MARK = re.compile(r'passlog: (\S+)')
KINDS = {6: 'truth resolve rec', 7: 'truth writeback rec', 8: 'truth flush: renames', 9: 'truth flush: barriers in',
         10: 'truth flush: barriers back', 11: 'truth flush: dispatch'}


ARM = {}  # (run, frame) -> the route's passlog label at that frame


def load(run, d):
    frames = defaultdict(list)
    totals = {}
    idle = {}
    label = ''
    with open(os.path.join(d, f'wwe2k14_{run}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MARK.search(line)
            if m and '[input]' in line:
                label = m.group(1)
                continue
            m = PASS.search(line)
            if m:
                ARM[(run, int(m.group(1)))] = label
                frames[int(m.group(1))].append(dict(
                    i=int(m.group(2)), kind=int(m.group(3)), w=int(m.group(4)), h=int(m.group(5)), draws=int(m.group(9)),
                    ps=m.group(11), ms=float(m.group(12)), before=float(m.group(13)), label=m.group(14)))
                continue
            m = TOTAL.search(line)
            if m:
                totals[int(m.group(1))] = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
                continue
            m = IDLE.search(line)
            if m:
                idle[int(m.group(1))] = float(m.group(4))
    return frames, totals, idle


def key_of(p):
    if p['label']:
        return 'owned ' + p['label'] + (f" {p['w']}x{p['h']}" if p['w'] else '')
    if p['kind'] == 4:
        return f"truth {p['w']}x{p['h']} ps {p['ps'][:8]}"
    return KINDS.get(p['kind'], f"truth kind {p['kind']}")


def group_of(k):
    """Coarser classes for the summary."""
    if k.startswith('owned pass'):
        return 'owned passes'
    if k.startswith('owned copy_in'):
        return 'owned copy-ins (dirty tiles)'
    if k.startswith('owned box_copy'):
        return 'owned box copies (converted starts)'
    if k.startswith('owned page_fill') or k.startswith('owned rect_copy') or k.startswith('owned snapshot'):
        return 'owned page fills / rect copies / snapshots'
    if k.startswith('owned depth_fill'):
        return 'owned depth fill'
    if k.startswith('truth flush') or k.startswith('truth resolve') or k.startswith('truth writeback'):
        return 'truth flush / resolve timers'
    return 'truth passes'


def stats(runs, d, arm=None):
    """Averages over the runs: per piece (count, ms) per frame, gap busy, segments, idle."""
    cnt = defaultdict(float)
    ms = defaultdict(float)
    seg = gap = idle_total = span = 0.0
    nframes = 0
    for run in runs:
        frames, totals, idle = load(run, d)
        for fr, ps in frames.items():
            if arm is not None and ARM.get((run, fr)) != arm:
                continue
            nframes += 1
            for p in ps:
                k = key_of(p)
                cnt[k] += 1
                ms[k] += p['ms']
            if fr in totals:
                seg += totals[fr][0]
                gap += totals[fr][1]
                span += totals[fr][2]
            idle_total += idle.get(fr, 0.0)
    n = max(1, nframes)
    pieces = {k: (cnt[k] / n, ms[k] / n) for k in ms}
    return pieces, seg / n, (gap - idle_total) / n, idle_total / n, span / n, nframes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('on', nargs='?')
    ap.add_argument('off', nargs='?')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    ap.add_argument('--frames', help='print every piece of the logged frames of this run, in order')
    ap.add_argument('--top', type=int, default=5)
    ap.add_argument('--inrun', help='one run with the owned frame flipped inside it (marks passlog: on / off)')
    a = ap.parse_args()
    if a.frames:
        fr, tot, _ = load(a.frames, a.dir)
        for f, ps in sorted(fr.items()):
            print(f'frame {f}: {tot.get(f, ("?", "?", "?"))}')
            for p in ps:
                print(f"  #{p['i']:3d} {key_of(p):44s} draws {p['draws']:5d} {p['ms']:7.3f} ms (before {p['before']:.3f})")
        return
    if a.inrun:
        # (one run, the owned frame flipped inside it: frames marked "passlog: on" / "passlog: off")
        on_runs = off_runs = a.inrun.split(',')
        on, on_seg, on_gap, on_idle, on_span, on_n = stats(on_runs, a.dir, 'on')
        off, off_seg, off_gap, off_idle, off_span, off_n = stats(off_runs, a.dir, 'off')
    else:
        on_runs, off_runs = a.on.split(','), a.off.split(',')
        on, on_seg, on_gap, on_idle, on_span, on_n = stats(on_runs, a.dir)
        off, off_seg, off_gap, off_idle, off_span, off_n = stats(off_runs, a.dir)
    print(f'frames logged: on {on_n} ({len(on_runs)} runs), off {off_n} ({len(off_runs)} runs)')
    # by group
    groups = defaultdict(lambda: [0.0, 0.0])
    for k, (c, m) in on.items():
        groups[group_of(k)][0] += m
    for k, (c, m) in off.items():
        groups[group_of(k)][1] += m
    print(f'\n{"group":46s} {"on ms":>8s} {"off ms":>8s} {"delta":>8s}')
    for g, (mo, mf) in sorted(groups.items(), key=lambda kv: -(kv[1][0] - kv[1][1])):
        print(f'{g:46s} {mo:8.3f} {mf:8.3f} {mo - mf:+8.3f}')
    print(f'{"timed segments (sum of the above)":46s} {on_seg:8.3f} {off_seg:8.3f} {on_seg - off_seg:+8.3f}')
    print(f'{"busy gaps between segments":46s} {on_gap:8.3f} {off_gap:8.3f} {on_gap - off_gap:+8.3f}')
    print(f'{"BUSY TOTAL (segments + busy gaps)":46s} {on_seg + on_gap:8.3f} {off_seg + off_gap:8.3f} '
          f'{on_seg + on_gap - off_seg - off_gap:+8.3f}')
    print(f'{"(idle: GPU waiting for a submission)":46s} {on_idle:8.3f} {off_idle:8.3f} {on_idle - off_idle:+8.3f}')
    # by piece
    keys = sorted(set(on) | set(off), key=lambda k: -(on.get(k, (0, 0))[1] - off.get(k, (0, 0))[1]))
    print(f'\n{"piece":46s} {"on n":>6s} {"on ms":>8s} {"off n":>6s} {"off ms":>8s} {"delta":>8s}')
    for k in keys[:a.top * 3] + ['...'] + keys[-a.top:]:
        if k == '...':
            print('...')
            continue
        co, mo = on.get(k, (0, 0.0))
        cf, mf = off.get(k, (0, 0.0))
        print(f'{k:46s} {co:6.1f} {mo:8.3f} {cf:6.1f} {mf:8.3f} {mo - mf:+8.3f}')


if __name__ == '__main__':
    main()
