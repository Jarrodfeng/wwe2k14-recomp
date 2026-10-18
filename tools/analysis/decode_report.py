"""Report of the data-block decoder probe (Session 8 task 3; src/decode_probe.h, --wwe2k14_decode_probe[_shadow]).

Usage: py tools/analysis/decode_report.py RUN [--top 25] [--min-ms 2] [--drop-ms 20] [--logs DIR]

Reads logs/wwe2k14_RUN.log ([wwe2k14][decode] lines) and logs/frames_RUN.csv (our own run output only) and prints:
  - the last running totals line (calls, expanded / stream bytes, guest vs native ms, mismatches);
  - decode time per scene (scene anchors = first open of 343.bik2 / 103.bik2 / m3.pac, as tools/bench/fps60.py),
    from the per-thread per-frame lines (frames with >= --wwe2k14_decode_probe_frame_ms of decoding);
  - the frames with the most decode time, with frames_RUN.csv frame_ms of frames F, F+1, F+2 (a call that ran
    while g_guest_frame was F lands in the present of F+1 or later; see barrier_probe.h for the update thread);
  - BPE calls grouped by guest caller (the lr in the per-call lines, calls >= --wwe2k14_decode_probe_call_ms);
  - every frame >= --drop-ms with decoding in the frame before it (on any thread): the UPDATE THREAD's own decode
    (only its rows are used for the estimates; the update thread = the host tid of the CGameMainThread barrier slot
    82260EB0 in logs/barrier_RUN.csv, from --wwe2k14_barrier_probe, or --update-tid), the other threads' decode
    (shown only; it does not delay the frame directly), and the frame time and the update thread's barrier work
    measured, without the shadow (minus the whole shadow cost of the update thread's calls: shadow_ms in logs from
    the fixed probe, the native decode alone in older logs, which understates it by the allocation / compare / free)
    and with a native decoder (minus the recompiled BPE time, plus the native time; zlib is left in).
    Without _shadow there is no native time: those columns print '-' unless --native-ratio R is given (then native
    = recompiled / R, marked '~'; runs 383 / 391 measured R = 3.3).
"""
import argparse
import collections
import csv
import os
import re

LOGS = os.path.join(os.path.dirname(__file__), '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
KEYS = ('343.bik2', '103.bik2', 'm3.pac')

RE_FRAME = re.compile(r'\[wwe2k14\]\[decode\] frame (\d+) tid (\d+): bpe calls=(\d+) out=(\d+) in=(\d+) '
                      r'ms=([\d.]+) native_ms=([\d.]+) max_call_ms=([\d.]+); zlib calls=(\d+) ms=([\d.]+)'
                      r'(?:; shadow_ms=([\d.]+))?')
RE_CALL = re.compile(r'\[wwe2k14\]\[decode\] call bpe frame=(\d+) tid=(\d+) lr=0x([0-9A-F]+) src=0x([0-9A-F]+) '
                     r'dst=0x([0-9A-F]+) out=(\d+) in=(\d+) blocks=(\d+) guest_ms=([\d.]+) native_ms=([\d.]+) '
                     r'match=(-?\d+) overlap=(\d+)(?: lr2=0x([0-9A-F]+))?(?: produced=(\d+) shadow_ms=([\d.]+))?')
UPDATE_SLOT = '82260EB0'  # CGameMainThread's barrier slot (barrier_probe.h, entrance_drops_4k.md)
RE_TOTAL = re.compile(r'\[wwe2k14\]\[decode\] totals at frame .*')
RE_MISMATCH = re.compile(r'\[wwe2k14\]\[decode\] MISMATCH .*')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--min-ms', type=float, default=2.0)
    ap.add_argument('--drop-ms', type=float, default=20.0)
    ap.add_argument('--logs', default=LOGS)
    ap.add_argument('--update-tid', type=int, default=None,
                    help='host tid of the update thread (default: from the 82260EB0 slot of logs/barrier_RUN.csv)')
    ap.add_argument('--native-ratio', type=float, default=None,
                    help='without _shadow: estimate native = recompiled / this ratio (e.g. 3.3)')
    a = ap.parse_args()
    n = a.run
    frames = {}
    fpath = os.path.join(a.logs, f'frames_{n}.csv')
    if os.path.exists(fpath):
        with open(fpath, newline='') as f:
            for row in csv.DictReader(f):
                frames[int(row['frame'])] = float(row['frame_ms'])
    anch = {}
    per_frame = []
    calls = []
    totals = None
    mismatches = []
    with open(os.path.join(a.logs, f'wwe2k14_{n}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            if '[decode]' not in line:
                m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
                if m:
                    p = m.group(1).lower()
                    for k in KEYS:
                        if k in p and k not in anch:
                            anch[k] = int(m.group(2))
                continue
            m = RE_FRAME.search(line)
            if m:
                per_frame.append(dict(frame=int(m.group(1)), tid=int(m.group(2)), calls=int(m.group(3)),
                                      out=int(m.group(4)), inb=int(m.group(5)), ms=float(m.group(6)),
                                      native=float(m.group(7)), maxcall=float(m.group(8)),
                                      zcalls=int(m.group(9)), zms=float(m.group(10)),
                                      shadow=float(m.group(11)) if m.group(11) is not None else None))
                continue
            m = RE_CALL.search(line)
            if m:
                calls.append(dict(frame=int(m.group(1)), tid=int(m.group(2)), lr=m.group(3), out=int(m.group(6)),
                                  inb=int(m.group(7)), blocks=int(m.group(8)), ms=float(m.group(9)),
                                  native=float(m.group(10)), match=int(m.group(11)), overlap=int(m.group(12)),
                                  lr2=m.group(13) or '-',
                                  produced=int(m.group(14)) if m.group(14) is not None else None))
                continue
            m = RE_TOTAL.search(line)
            if m:
                totals = m.group(0)
                continue
            m = RE_MISMATCH.search(line)
            if m:
                mismatches.append(m.group(0))
    print(f'run {n}: {len(per_frame)} per-frame lines, {len(calls)} per-call lines, anchors {anch}')
    print('last totals:', totals or '(none)')
    for mm in mismatches:
        print(mm)

    def scene(fr):
        s = 'before first entrance'
        if '343.bik2' in anch and fr >= anch['343.bik2']:
            s = 'first entrance (343.bik2..)'
        if '103.bik2' in anch and fr >= anch['103.bik2']:
            s = 'second entrance (103.bik2..)'
        if 'm3.pac' in anch and fr >= anch['m3.pac']:
            s = 'match (m3.pac..)'
        return s

    agg = collections.OrderedDict()
    for r in per_frame:
        s = scene(r['frame'])
        d = agg.setdefault(s, dict(frames=0, calls=0, out=0, inb=0, ms=0.0, native=0.0, zms=0.0))
        d['frames'] += 1
        d['calls'] += r['calls']
        d['out'] += r['out']
        d['inb'] += r['inb']
        d['ms'] += r['ms']
        d['native'] += r['native']
        d['zms'] += r['zms']
    print('\nper scene (frames with >= frame_ms of decoding only):')
    print(f"{'scene':32} {'frames':>6} {'calls':>6} {'out MB':>8} {'in MB':>8} {'bpe ms':>9} {'native ms':>9} {'zlib ms':>8}")
    for s, d in agg.items():
        print(f"{s:32} {d['frames']:6} {d['calls']:6} {d['out']/1e6:8.2f} {d['inb']/1e6:8.2f} {d['ms']:9.1f} "
              f"{d['native']:9.2f} {d['zms']:8.1f}")

    print(f'\ntop frames by decode time (>= {a.min_ms} ms):')
    print(f"{'frame':>6} {'tid':>6} {'calls':>5} {'out KB':>8} {'in KB':>8} {'bpe ms':>7} {'native':>7} {'max call':>8} "
          f"{'zlib ms':>7}  frame_ms F / F+1 / F+2   scene")
    for r in sorted(per_frame, key=lambda r: -(r['ms'] + r['zms']))[:a.top]:
        if r['ms'] + r['zms'] < a.min_ms:
            break
        fm = ' / '.join(f"{frames.get(r['frame'] + k, float('nan')):.1f}" for k in range(3))
        print(f"{r['frame']:6} {r['tid']:6} {r['calls']:5} {r['out']/1024:8.0f} {r['inb']/1024:8.0f} {r['ms']:7.2f} "
              f"{r['native']:7.2f} {r['maxcall']:8.2f} {r['zms']:7.2f}  {fm:24} {scene(r['frame'])}")

    by_lr = collections.defaultdict(lambda: [0, 0, 0.0, 0.0])
    for c in calls:
        v = by_lr[c['lr'] + ' <- ' + c['lr2']]
        v[0] += 1
        v[1] += c['out']
        v[2] += c['ms']
        v[3] += c['native']
    print('\nper-call lines by guest caller (return address):')
    for lr, v in sorted(by_lr.items(), key=lambda kv: -kv[1][2]):
        ratio = v[2] / v[3] if v[3] > 0 else float('nan')
        print(f'  0x{lr}: {v[0]} calls, out {v[1]/1e6:.2f} MB, guest {v[2]:.1f} ms, native {v[3]:.2f} ms, '
              f'guest/native {ratio:.1f}x')
    if calls:
        tot_g = sum(c['ms'] for c in calls)
        tot_n = sum(c['native'] for c in calls if c['native'] > 0)
        out = sum(c['out'] for c in calls)
        print(f'  all per-call lines: guest {tot_g:.1f} ms for {out/1e6:.2f} MB = {out/1e6/(tot_g/1000) if tot_g else 0:.0f} MB/s; '
              f'native {tot_n:.2f} ms = {out/1e6/(tot_n/1000) if tot_n else 0:.0f} MB/s; '
              f'mismatching calls {sum(1 for c in calls if c["match"] == 0)}, overlapping {sum(c["overlap"] for c in calls)}')
        withp = [c for c in calls if c['produced'] is not None]
        if withp:
            over = [c['produced'] - c['out'] for c in withp if c['produced'] != c['out']]
            print(f'  guest-written length (r5) on {len(withp)} lines: {len(over)} differ from the declared size'
                  f'{", largest difference " + str(max(over, key=abs)) + " bytes" if over else ""}')

    # Drop frames: decode time of the calls that ran while g_guest_frame was N-1 (they land in the present of
    # frame N, as the barrier rows do: barrier_probe.h) and, with logs/barrier_RUN.csv, the update thread's
    # (CGameMainThread, slot lr 82260EB0) work in that row = its arrival time after the release.
    work = {}
    slot_tids = collections.Counter()
    bpath = os.path.join(a.logs, f'barrier_{n}.csv')
    if os.path.exists(bpath):
        with open(bpath, newline='') as f:
            rd = csv.reader(f)
            next(rd, None)
            for row in rd:
                try:
                    fr = int(row[0])
                except (ValueError, IndexError):
                    continue
                for slot in row[5:]:
                    parts = slot.split(';')
                    if len(parts) == 4 and parts[0].upper() == UPDATE_SLOT:
                        try:
                            work[fr] = float(parts[3])
                            slot_tids[int(parts[1])] += 1
                        except ValueError:
                            pass
    upd = a.update_tid
    upd_src = '--update-tid'
    if upd is None and slot_tids:
        upd = slot_tids.most_common(1)[0][0]
        upd_src = f'barrier_{n}.csv slot {UPDATE_SLOT}'
        if len(slot_tids) > 1:
            upd_src += f' (several tids: {dict(slot_tids)}; most common used)'
    # Per frame: the update thread's own rows only, and the other threads' decode summed (shown, not subtracted).
    dec_upd = {}
    dec_other = collections.defaultdict(float)
    for r in per_frame:
        if upd is not None and r['tid'] == upd:
            dec_upd[r['frame']] = r
        else:
            dec_other[r['frame']] += r['ms'] + r['zms']

    def estimates(measured, r):
        """(without shadow, with a native decoder) for a measured frame time or barrier work, or '-' strings."""
        if measured is None:
            return '-', '-'
        if upd is None:
            return '-', '-'
        if r is None or r['ms'] <= 0:  # no BPE on the update thread in that frame: nothing to remove or replace
            return f'{measured:.1f}', f'{measured:.1f}'
        if r['native'] > 0:  # shadow on: the measured value contains the shadow cost on top of the recompiled decode
            shadow = r['shadow'] if r['shadow'] is not None else r['native']
            wo = measured - shadow
            return f'{wo:.1f}', f"{wo - r['ms'] + r['native']:.1f}"
        if a.native_ratio:  # no shadow: estimate native from the given ratio
            return f'{measured:.1f}', f"~{measured - r['ms'] + r['ms'] / a.native_ratio:.1f}"
        return f'{measured:.1f}', '-'

    print(f'\ndrop frames (frame_ms >= {a.drop_ms}) with decoding in the frame before (>= frame_ms setting, any '
          f'thread); update thread tid: {upd if upd is not None else "unknown (no barrier CSV; use --update-tid)"}'
          f'{" from " + upd_src if upd is not None else ""}. Estimates use the update thread\'s BPE only:')
    print(f"{'frame':>6} {'ms':>6} {'upd bpe':>7} {'native':>6} {'shadow':>6} {'upd zlib':>8} {'other thr':>9} "
          f"{'ms w/o shadow':>13} {'ms native':>9} {'work':>6} {'work w/o shadow':>15} {'work native':>11}  scene")
    for fr in sorted(frames):
        ms = frames[fr]
        if ms < a.drop_ms or ms > 400:
            continue
        r = dec_upd.get(fr - 1)
        oth = dec_other.get(fr - 1, 0.0)
        if (r is None or r['ms'] + r['zms'] <= 0) and oth <= 0:
            continue
        w = work.get(fr - 1)
        f_wo, f_nat = estimates(ms, r)
        w_wo, w_nat = estimates(w, r)
        if r is not None:
            sh = r['shadow'] if r['shadow'] is not None else r['native']
            ucols = f"{r['ms']:7.2f} {r['native']:6.2f} {sh:6.2f} {r['zms']:8.2f}"
        else:
            ucols = f"{'-' if upd is None else '0':>7} {'-':>6} {'-':>6} {'-':>8}"
        ws = f'{w:6.1f}' if w is not None else f"{'-':>6}"
        print(f'{fr:6} {ms:6.1f} {ucols} {oth:9.2f} {f_wo:>13} {f_nat:>9} {ws} {w_wo:>15} {w_nat:>11}  {scene(fr)}')


if __name__ == '__main__':
    main()
