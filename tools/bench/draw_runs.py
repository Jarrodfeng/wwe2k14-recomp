"""Count draws that differ from the previous draw only in shader constants (Session 19 item 5, "count first").

    py tools/bench/draw_runs.py logs/anatomy_NNN_K.csv [--top 10]

Needs an anatomy capture from SDK pin-787857a or later (columns state_hash, fetch_hash: SDK patch DRAWRUNS).
Per captured frame:
  draws          draw rows (resolves are copy rows; a resolve breaks a run)
  const_only     draws whose state_hash AND fetch_hash equal the previous draw's (same shaders, render targets,
                 render state, index buffer / draw initiator, textures and vertex buffers): only float / bool / loop
                 constants can differ. These are what a constants-only fast path could take.
  same_state     state_hash equal but fetch constants differ (other textures or vertex buffers)
  runs           maximal runs of const_only draws (a run of k const_only draws = k+1 draws sharing one state)
  cp_us          CP time of the frame's draws, and of the const_only draws (an upper bound on what the fast path saves)
--top N lists the vertex shaders with the most const_only draws."""
import argparse
import csv
from collections import Counter, defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--top', type=int, default=8)
    a = ap.parse_args()
    frames = defaultdict(list)
    for r in csv.DictReader(open(a.csv, newline='')):
        frames[r['frame']].append(r)
    for fr, rows in frames.items():
        if 'state_hash' not in rows[0]:
            print('no state_hash column: capture with SDK pin-787857a or later'); return
        draws = const_only = same_state = 0
        cp_all = cp_const = 0.0
        runs = Counter()
        run = 0
        prev = None
        by_vs = Counter()
        for r in rows:
            if r['kind'] != 'draw':
                if run: runs[run] += 1
                run = 0
                prev = None
                continue
            draws += 1
            cp = float(r['cp_us'] or 0)
            cp_all += cp
            key = (r['state_hash'], r['fetch_hash'])
            if prev and key == prev:
                const_only += 1
                cp_const += cp
                run += 1
                by_vs[r['vs_hash']] += 1
            else:
                if prev and key[0] == prev[0]:
                    same_state += 1
                if run: runs[run] += 1
                run = 0
            prev = key
        if run: runs[run] += 1
        print(f'frame {fr}: draws {draws}, const_only {const_only} ({100 * const_only / max(1, draws):.1f}%), '
              f'same_state_other_fetch {same_state} ({100 * same_state / max(1, draws):.1f}%), '
              f'cp_us all draws {cp_all:.0f}, const_only draws {cp_const:.0f} ({100 * cp_const / max(1, cp_all):.1f}%)')
        lens = sorted(runs.items())
        print('  runs (const_only draws in a run: count): ' + ', '.join(f'{k}:{v}' for k, v in lens[:12]) +
              (f' ... longest {lens[-1][0]}' if len(lens) > 12 else ''))
        for vs, n in by_vs.most_common(a.top):
            print(f'  vs {vs}: {n} const_only draws')


if __name__ == '__main__':
    main()
