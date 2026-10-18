"""Input-to-present latency from a timeline (Session 19 item 9).

    py tools/perf/latency.py MAPRUN                 the mapping run (short/latency_map.txt: captures every frame)
    py tools/perf/latency.py RUN --map MAPRUN       a clean run (short/latency.txt), using the mapping run's offset
    py tools/perf/latency.py RUN --offset D         ... or a known offset

Timeline events used (run with --wwe2k14_timeline=true, SDK with the LATENCY patch):
  inread     a = buttons, b = guest frame: the game's first controller read returning new buttons (the press)
  gorefresh  a = refresh sequence number: a guest frame's image became ready on the host (one per CP swap)
  hpresent   a = refresh sequence number of the image presented, b = VkResult: right after vkQueuePresentKHR
  gocapture  a = refresh sequence number of the image a screenshot captured
  vdswap     a = guest frame (the game's present call)

Mapping run: for each press (an inread with the START bit set), the screenshots taken around it (captures/NNN_l_pK_*
in order = the run's gocapture events in order) are compared with the one before the press; the first that differs
(mean absolute difference > --mad, default 4) shows the response. Offset D = its refresh number minus the last refresh
before the read. Clean run: latency = time of the first hpresent of refresh (last refresh before the read + D) minus
the read. Also printed: the guest frames between the read and the response's VdSwap, and the host present gap.
Latency here = from the game seeing the press to the host present call; it leaves out the controller poll / USB
before it and the display's scanout after it."""
import argparse
import csv
import glob
import os
import re
from bisect import bisect_left, bisect_right

L = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs'
CAP = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/captures'
START = 0x0010


def load(run):
    ev = {k: [] for k in ('inread', 'gorefresh', 'hpresent', 'gocapture', 'vdswap')}
    for r in csv.DictReader(open(f'{L}/timeline_{run}.csv', newline='')):
        try:
            e = r['event']
            if e in ev:
                ev[e].append((int(r['t_ns']), int(r['a'], 16), int(r['b'], 16)))
        except (ValueError, KeyError):
            break
    for k in ev:
        ev[k].sort()
    return ev


def presses(ev):
    out = []
    prev = 0
    for t, buttons, frame in ev['inread']:
        if (buttons & START) and not (prev & START):
            out.append((t, frame))
        prev = buttons
    return out


def last_refresh_before(ev, t):
    times = [x[0] for x in ev['gorefresh']]
    i = bisect_right(times, t) - 1
    return ev['gorefresh'][i][1] if i >= 0 else 0


def mad(a, b):
    from PIL import Image, ImageChops, ImageStat
    ia = Image.open(a).convert('RGB').resize((640, 360))
    ib = Image.open(b).convert('RGB').resize((640, 360))
    return sum(ImageStat.Stat(ImageChops.difference(ia, ib)).mean) / 3


def mapping(run, threshold):
    ev = load(run)
    ps = presses(ev)
    caps = ev['gocapture']
    # Every screenshot of the run, in order (burst K, then index), is one gocapture event, in the same order.
    key = lambda p: (int(re.search(r'_l_p(\d+)_', p).group(1)), int(re.search(r'_(\d+)\.png$', p).group(1)))
    files = sorted(glob.glob(f'{CAP}/{run}_l_p*_*.png'), key=key)
    if len(files) != len(caps):
        print(f'warning: {len(files)} capture files but {len(caps)} gocapture events; matching in order anyway')
    offsets = []
    for k, (t, frame) in enumerate(ps, 1):
        idx = [i for i, c in enumerate(caps[:len(files)]) if t - 200_000_000 <= c[0] <= t + 600_000_000]
        before = [i for i in idx if caps[i][0] < t]
        if not idx or not before:
            print(f'press {k} (frame {frame}): no capture burst around it'); continue
        base_seq = last_refresh_before(ev, t)
        ref = files[before[-1]]
        hit = None
        for i in idx:
            if i <= before[-1]:
                continue
            d = mad(ref, files[i])
            if d > threshold:
                hit = (i, d)
                break
        if not hit:
            print(f'press {k} (frame {frame}): no change found in {len(idx)} captures'); continue
        i, d = hit
        seq = caps[i][1]
        offsets.append(seq - base_seq)
        print(f'press {k} (frame {frame}): first changed capture {os.path.basename(files[i])} (MAD {d:.1f}), '
              f'refresh {seq}, last refresh before the read {base_seq}: offset {seq - base_seq}')
    if offsets:
        print(f'offsets: {offsets}; using {max(set(offsets), key=offsets.count)}')
    return offsets

def clean(run, offset):
    ev = load(run)
    ps = presses(ev)
    pres = ev['hpresent']
    ptimes = [p[0] for p in pres]
    vd = ev['vdswap']
    lat = []
    breakdown = []
    for k, (t, frame) in enumerate(ps, 1):
        target = last_refresh_before(ev, t) + offset
        j = next((i for i in range(bisect_left(ptimes, t), len(pres)) if pres[i][1] >= target), None)
        if j is None:
            print(f'press {k}: no present of refresh {target}'); continue
        ms = (pres[j][0] - t) / 1e6
        lat.append(ms)
        vds = [v for v in vd if v[0] > t and v[0] <= pres[j][0]]
        # Breakdown: the refresh event of the response image (CP side), and the VdSwap the game made for it: the
        # refresh numbers trail the guest frame numbers by a constant (delta, at the read).
        ref = next((r for r in ev['gorefresh'] if r[1] == pres[j][1]), None)
        delta = frame - last_refresh_before(ev, t)
        vsw = next((v for v in vd if v[1] == pres[j][1] + delta), None)
        parts = ''
        if ref and vsw:
            parts = (f' = read -> VdSwap {(vsw[0] - t) / 1e6:.1f} + VdSwap -> CP refresh {(ref[0] - vsw[0]) / 1e6:.1f} + '
                     f'refresh -> present {(pres[j][0] - ref[0]) / 1e6:.1f} ms')
            breakdown.append(((vsw[0] - t) / 1e6, (ref[0] - vsw[0]) / 1e6, (pres[j][0] - ref[0]) / 1e6))
        print(f'press {k} (frame {frame}): present of refresh {pres[j][1]} after {ms:.1f} ms{parts}')
    if lat:
        lat.sort()
        print(f'latency read -> host present: mean {sum(lat) / len(lat):.1f} ms, min {lat[0]:.1f}, '
              f'median {lat[len(lat) // 2]:.1f}, max {lat[-1]:.1f} (n={len(lat)})')
    if breakdown:
        m = [sum(b[i] for b in breakdown) / len(breakdown) for i in range(3)]
        print(f'mean breakdown: read -> VdSwap {m[0]:.1f} ms (the game), VdSwap -> CP refresh {m[1]:.1f} ms '
              f'(command processor + host GPU), refresh -> present {m[2]:.1f} ms')
    # frames in flight seen from the present side: refreshes between a guest VdSwap and its present
    return lat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--map', default='')
    ap.add_argument('--offset', type=int, default=None)
    ap.add_argument('--mad', type=float, default=4.0)
    a = ap.parse_args()
    run = a.run.zfill(3)
    if a.offset is None and not a.map:
        offs = mapping(run, a.mad)
        if offs:
            print('\n-- the mapping run itself (captures add work, so only indicative):')
            clean(run, max(set(offs), key=offs.count))
        return
    offset = a.offset
    if offset is None:
        offs = mapping(a.map.zfill(3), a.mad)
        offset = max(set(offs), key=offs.count)
    print(f'\n-- run {run}, offset {offset}:')
    clean(run, offset)


if __name__ == '__main__':
    main()
