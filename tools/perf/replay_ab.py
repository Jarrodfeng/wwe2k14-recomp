"""Session 21: whole-run A/B of replay runs on matching guest frames (tools/perf/s21_replay_ab.ps1).

    py tools/perf/replay_ab.py --off RUN,RUN,RUN --on RUN,RUN,RUN [--metric gpu_ms] [--pan 380-500]

Reads bench_report's per-CP-frame CSVs (out\\bench\\reports\\cpframes_RUN.csv). Replay runs show the same guest
frames, so each guest frame's metric is averaged over the off runs and over the on runs, and the per-frame
difference (on - off) is averaged per scene (brock = tron_343, bryan = tron_324, pan = tron_324 start + --pan,
match). Also each run's own scene means (the run-to-run spread). Numbers only; our own run output.
"""
import argparse
import collections
import csv
import os
import statistics

REPORTS = r'C:\Users\Jarrod\wwe2k14-recomp\out\bench\reports'
SCENES = ('brock', 'bryan', 'pan', 'match')


def load(run, metric, pan):
    rows = list(csv.DictReader(open(os.path.join(REPORTS, 'cpframes_%s.csv' % run), newline='')))
    start = min((int(r['guest_frame']) for r in rows if r['scene'] == 'tron_324'), default=None)
    out = collections.defaultdict(dict)  # scene -> guest frame -> value
    for r in rows:
        try:
            v = float(r[metric])
            g = int(r['guest_frame'])
        except (ValueError, KeyError):
            continue
        s = r['scene']
        if s == 'tron_343':
            out['brock'][g] = v
        elif s == 'tron_324':
            out['bryan'][g] = v
            if start is not None and pan[0] <= g - start < pan[1]:
                out['pan'][g] = v
        elif s == 'match':
            out['match'][g] = v
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--off', required=True)
    ap.add_argument('--on', required=True)
    ap.add_argument('--metric', default='gpu_ms')
    ap.add_argument('--pan', default='380-500')
    a = ap.parse_args()
    pan = tuple(map(int, a.pan.split('-')))
    offs = {r: load(r, a.metric, pan) for r in a.off.split(',')}
    ons = {r: load(r, a.metric, pan) for r in a.on.split(',')}
    print('%s per frame; off runs %s, on runs %s' % (a.metric, a.off, a.on))
    print('  %-6s %-28s %-28s %8s %8s  %s' % ('scene', 'off runs', 'on runs', 'on-off', 'frames', 'frames lower'))
    for s in SCENES:
        per_off = [statistics.mean(d[s].values()) for d in offs.values() if d[s]]
        per_on = [statistics.mean(d[s].values()) for d in ons.values() if d[s]]
        frames = set.intersection(*[set(d[s]) for d in list(offs.values()) + list(ons.values())])
        if not frames:
            print('  %-6s (no common frames)' % s)
            continue
        diffs = [statistics.mean(d[s][g] for d in ons.values()) - statistics.mean(d[s][g] for d in offs.values())
                 for g in sorted(frames)]
        print('  %-6s %-28s %-28s %+8.3f %8d  %d%%' % (
            s, ' '.join('%.2f' % x for x in per_off), ' '.join('%.2f' % x for x in per_on), statistics.mean(diffs),
            len(frames), round(100 * sum(d < 0 for d in diffs) / len(diffs))))


if __name__ == '__main__':
    main()
