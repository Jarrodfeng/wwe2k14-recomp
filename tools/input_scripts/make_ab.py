"""Make an in-run A/B variant of a benchmark route: toggle settings on/off every PERIOD guest frames through the
entrances and the match, so both arms see the same content (no camera-variant or boot noise).

Usage:
  py tools/input_scripts/make_ab.py <route.txt> --cvar NAME [--cvar NAME2 ...] [--period 90] [--set NAME=VALUE ...]
                                    [--entrance-frames N] [--match-frames 3600] [--out file.txt]
  bench_report.py --ab NAME then splits frames by the 'cvar' timeline events (value 1 = B, 0 = A).

The route is kept up to its `log scene=tron_343` line (Brock's titantron opens). From there the script toggles
for --entrance-frames (default: long enough to cover both entrances of the route), waits for the match (M3.PAC),
toggles for --match-frames, and quits. --set applies fixed settings right after boot (e.g. a _verify flag, or a
second feature that both arms share). Several --cvar flip together (one combined arm).
"""
import argparse
import os
import sys

ENTRANCE_FRAMES = {'taker_wm': 5450, 'lesnar_undertaker': 5450, 'lesnar_bryan': 3900}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('route')
    ap.add_argument('--cvar', action='append', required=True)
    ap.add_argument('--period', type=int, default=90)
    ap.add_argument('--on', default='true', help='value for arm B (default true)')
    ap.add_argument('--off', default='false', help='value for arm A (default false)')
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('--entrance-frames', type=int)
    ap.add_argument('--match-frames', type=int, default=3600)
    ap.add_argument('--out')
    args = ap.parse_args()

    lines = open(args.route, encoding='utf-8').read().split('\n')
    try:
        cut = next(i for i, l in enumerate(lines) if l.strip() == 'log scene=tron_343')
    except StopIteration:
        sys.exit('route has no "log scene=tron_343" line')
    name = os.path.basename(args.route).removeprefix('bench_').removesuffix('.txt')
    entrance = args.entrance_frames or ENTRANCE_FRAMES.get(name, 5450)

    def toggles(total):
        n = max(1, total // (2 * args.period))
        block = [f'repeat {n}']
        block += [f'cvar {c} {args.on}' for c in args.cvar] + [f'wait_frames {args.period}']
        block += [f'cvar {c} {args.off}' for c in args.cvar] + [f'wait_frames {args.period}']
        return block + ['end_repeat']

    head = lines[:cut + 1]
    # Fixed settings right after on_fail (first non-comment command lines).
    insert_at = next(i for i, l in enumerate(head) if l.startswith('on_fail')) + 1
    fixed = [f'cvar {kv.split("=", 1)[0]} {kv.split("=", 1)[1]}' for kv in args.set]
    head = head[:insert_at] + fixed + head[insert_at:]
    label = '+'.join(args.cvar)
    out = [f'# In-run A/B of {label} (period {args.period} frames) on {os.path.basename(args.route)}; made by make_ab.py'] + head
    out += ['mark 1 ab_entrances'] + toggles(entrance)
    out += [f'cvar {c} {args.off}' for c in args.cvar]
    out += ['wait_file M3.PAC 6000', 'mark 2 ab_match', 'wait_frames 300'] + toggles(args.match_frames)
    out += [f'cvar {c} {args.off}' for c in args.cvar] + ['mark 3 ab_done', 'wait_frames 30', 'quit']
    text = '\n'.join(out) + '\n'
    if args.out:
        open(args.out, 'w', newline='\n').write(text)
        print(f'{args.out}: {len(out)} lines, {entrance} entrance frames, {args.match_frames} match frames')
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
