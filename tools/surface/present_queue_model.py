#!/usr/bin/env python3
"""S102 R25-C2 piece B: the present-queue model (offline, no game).

Replays a recorded match window (rows of a frames csv: t_seconds, presented, present_gap_ms, acquire_ms) through a small queue model of OUR present path:

  sink thread (the game's swap, 60 Hz clock)  -> mail (frames arrived, not yet taken)  -> render thread (acquire an image, work, present)  -> display (FIFO, one image
  is shown per vblank, an image is free again once the next one is shown)

Parameters of a run: max_queued D (the sink skips a frame when D or more frames wait behind it: the newest frames win, the oldest is dropped), swapchain images N
(the render thread's acquire waits until fewer than N-1 images are queued: one shown + queued + the one being drawn). Today's default is D = 2, N = 3.

HOW THE RECORD BECOMES A MODEL (every step is an assumption, written down; the model is [E], not a measurement):
  1. Present k is the k-th present of the window; it is frame k (rows are guest frames; presents / frames = 596 / 601 in bar4: the missing 5 are the frames still
     queued at the end; no skips in between). Frame k arrives at the end of its own row, a_k = t_seconds of row k.
  2. Present times P_k are rebuilt from the csv's gaps: P_k = P_(k-1) + gap. A row with 2 presents shows only its LAST gap; the hidden first gap is set to
     (wall time - sum of the reported gaps) / (number of 2-present rows), the same rule R25-C used (its text: the hidden ones average 16.7 ms).
  3. The render thread's WORK for frame k is what is left of its present interval after the part the record explains: work_k = max(0, P_k - max(P_(k-1), a_k) -
     acquire_k) with acquire_k = the row's acquire_ms (on the row's last present). It holds the render CPU, the GPU slot fence and every other wait that does not
     depend on the swapchain. It does not change with N or D: that is the model's central assumption (a deeper queue does not make the render thread faster).
  4. The display shows one queued image per vblank (period 1000/refresh ms, a phase found by the calibration below). A present enters the display queue at its
     present time; the render thread's acquire for the next frame waits until the display queue holds at most N-2 images.
  5. CALIBRATION: with D = 2, N = 3 the replay must give the recorded p99 of the bench rule (the last gap of each row with a present) within 1.0 ms. The only
     free parameter is the vblank phase; the best phase over a 0.25 ms grid is taken (the smallest error against the recorded P_k). A model that cannot reproduce
     today's number FAILS (the past-bug check).

Output per (D, N): gap p99 by the bench rule (last gap per row) and over all gaps, mean frame latency (arrival -> shown), p99 latency, skips, out-of-order frames.
"""
import bisect
import csv
import math
import sys

VBLANK_MS = 1000.0 / 60.0


def load_rows(path, f0, f1):
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                fr = int(r['frame'])
            except (KeyError, ValueError):
                continue
            if f0 <= fr <= f1:
                rows.append(r)
    if not rows:
        raise SystemExit('FAIL: no rows %d..%d in %s' % (f0, f1, path))
    return rows


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


class Trace:
    """What the recorded window gives the model: arrivals, present times, work, and the row of every present."""

    def __init__(self, rows):
        self.t = [1000.0 * float(r['t_seconds']) for r in rows]          # row ends (ms)
        pres = [int(r['presented'] or 0) for r in rows]
        gaps = [num(r.get('present_gap_ms')) for r in rows]
        acq = [num(r.get('acquire_ms')) or 0.0 for r in rows]
        n2 = sum(1 for c in pres if c >= 2)
        reported = sum(g for c, g in zip(pres, gaps) if c and g)
        first_row = next(i for i, c in enumerate(pres) if c)
        wall = self.t[-1] - self.t[first_row]
        hidden = max(0.5, (wall - reported) / n2) if n2 else VBLANK_MS
        self.hidden = hidden
        self.P, self.row_of, self.acq = [], [], []
        last = None
        for i, (c, g) in enumerate(zip(pres, gaps)):
            for j in range(c):
                if last is None:
                    last = self.t[i]                 # the window's first present: pinned to its row end (no gap before it)
                elif j == c - 1 and g is not None:
                    last = last + g                  # the csv's gap is the LAST present's gap to the one before it
                else:
                    last = last + hidden             # an earlier present of a 2-present row: its gap is not in the csv
                self.P.append(last)
                self.row_of.append(i)
                self.acq.append(acq[i] if j == c - 1 else 0.0)
        # rows that hid a first gap: the last present's time = previous present + hidden + gap (set above for j == c-1 only after the first present was placed with `hidden`)
        self.n = len(self.P)
        self.A = self.t[:self.n]                      # frame k arrives at the end of row k
        self.work = []
        prev = None
        for k in range(self.n):
            start = self.A[k] if prev is None else max(prev, self.A[k])
            self.work.append(max(0.0, self.P[k] - start - self.acq[k]) if prev is not None else 0.0)
            prev = self.P[k]
        self.rows = len(rows)
        self.pres_rows = pres


def p99(v):
    s = sorted(v)
    return s[min(len(s) - 1, int(0.99 * len(s)))] if s else float('nan')


def replay(tr, depth, images, phase, refresh_ms=VBLANK_MS, latest_wins=True):
    """-> dict. depth = max_queued D, images = N. Presents in frame order; a frame the sink drops is never presented."""
    n = tr.n
    shown_at = []          # display time of presented frames, in present order
    disp_q = []            # present times of the images queued for display (the shown one is not in it)
    next_vblank = phase    # the next vblank time (grid phase + m * refresh)
    P2, frames_out, skipped = [], [], 0
    t_free = tr.A[0]       # the render thread is free from here
    head = 0
    last_present = None

    held = [0]             # vblanks (after the first image was shown) with nothing queued: the previous image is shown again = a held frame the player sees

    def drain(until):
        nonlocal next_vblank
        while next_vblank <= until:
            if disp_q:
                disp_q.pop(0)
                shown_at.append(next_vblank)
            elif shown_at:
                held[0] += 1
            next_vblank += refresh_ms

    while head < n:
        now = max(t_free, tr.A[head])
        # frames that have arrived by `now` and are still in the mail behind the head
        arrived = bisect.bisect_right(tr.A, now)
        waiting = max(0, arrived - head - 1)
        if latest_wins and waiting >= depth and head + 1 < n:
            skipped += 1                            # the oldest frame is dropped; the newer ones win (live.cpp: skip_wanted)
            frames_out.append((head, None))
            head += 1
            continue
        drain(now)
        # acquire: wait until at most images - 2 images are queued behind the shown one
        t = now
        while len(disp_q) > images - 2:
            t = next_vblank
            drain(t)
        t += tr.work[head]
        drain(t)
        disp_q.append(t)
        P2.append(t)
        frames_out.append((head, t))
        last_present = t
        t_free = t
        head += 1
    held_run = held[0]     # taken before the tail drain: the end of the window is not a hitch
    drain(t_free + 10 * refresh_ms)
    return {'P': P2, 'shown': shown_at, 'skipped': skipped, 'frames': frames_out, 'held': held_run}


def bench_p99(tr, P):
    """The bench rule: the last gap of each row that has a present."""
    last_in_row = {}
    for k in range(1, len(P)):
        r = bisect.bisect_left(tr.t, P[k])
        last_in_row[r] = P[k] - P[k - 1]
    return p99(list(last_in_row.values()))


def all_gaps_p99(P):
    return p99([P[k] - P[k - 1] for k in range(1, len(P))])


def latency(tr, res):
    """mean / p99 of (shown - arrival) over the frames that were presented and shown, in order."""
    idx = [i for i, t in res['frames'] if t is not None]
    n = min(len(idx), len(res['shown']))
    lat = [res['shown'][j] - tr.A[idx[j]] for j in range(n)]
    return (sum(lat) / len(lat) if lat else float('nan')), p99(lat)


def out_of_order(res):
    """presented frame indices must rise, display times must rise."""
    idx = [i for i, t in res['frames'] if t is not None]
    bad = sum(1 for a, b in zip(idx, idx[1:]) if b <= a)
    bad += sum(1 for a, b in zip(res['shown'], res['shown'][1:]) if b < a)
    return bad


def calibrate(tr, target_p99):
    """The vblank phase (0.25 ms grid) whose D=2 N=3 replay follows the recorded present times best; returns (phase, replay, error)."""
    best = None
    ph = 0.0
    while ph < VBLANK_MS:
        res = replay(tr, 2, 3, tr.t[0] + ph)
        m = min(len(res['P']), tr.n)
        err = math.sqrt(sum((res['P'][k] - tr.P[k]) ** 2 for k in range(m)) / max(1, m))
        if best is None or err < best[2]:
            best = (ph, res, err)
        ph += 0.25
    return best


def run(path, f0, f1):
    rows = load_rows(path, f0, f1)
    tr = Trace(rows)
    recorded = bench_p99(tr, tr.P)
    ph, base, err = calibrate(tr, recorded)
    out = {'tr': tr, 'recorded_p99': recorded, 'phase': ph, 'rms_err': err, 'cells': {}}
    for d, n in ((2, 3), (3, 3), (3, 4), (4, 4), (2, 4)):
        res = replay(tr, d, n, tr.t[0] + ph)
        lm, l99 = latency(tr, res)
        out['cells'][(d, n)] = {'p99_bench': bench_p99(tr, res['P']), 'p99_all': all_gaps_p99(res['P']), 'lat_mean': lm, 'lat_p99': l99, 'skipped': res['skipped'], 'ooo': out_of_order(res), 'presents': len(res['P']), 'held': res['held']}
    return out


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'frames_484.csv'
    f0 = int(sys.argv[2]) if len(sys.argv) > 2 else 2987
    f1 = int(sys.argv[3]) if len(sys.argv) > 3 else 3587
    o = run(path, f0, f1)
    print('recorded p99 (bench rule, last gap per row): %.2f ms; hidden first gap %.2f ms; vblank phase %.2f ms; rms error of the D2/N3 replay %.2f ms' % (o['recorded_p99'], o['tr'].hidden, o['phase'], o['rms_err']))
    for (d, n), c in o['cells'].items():
        print('D%d N%d: p99 bench rule %.2f, all gaps %.2f | latency mean %.1f p99 %.1f | skipped %d, held vblanks %d, out of order %d, presents %d' % (d, n, c['p99_bench'], c['p99_all'], c['lat_mean'], c['lat_p99'], c['skipped'], c['held'], c['ooo'], c['presents']))
