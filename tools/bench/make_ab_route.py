"""S102 S0-4: make an in-run on / off route for ONE scene and ONE switch, so a single Surface run measures the switch with the same
heat, scene and build in both arms (docs\\agents\\s102\\briefs\\S0-4_switches_per_frame.md; review D/F/G/H in docs\\renderer\\SWITCHES.md).

  py -I tools\\bench\\make_ab_route.py --scene bryan --switch wwe2k14_rdr_skip_prune [--windows 8] [--seconds 3]
                                      [--pattern alt|abba] [--first on|off] [--on true] [--off false] [--fps 60] [--out FILE] [--force]

The scene route (tools\\input_scripts\\scenes\\<scene>_entrance.txt / match.txt) is kept up to its first `mark` line (the scene's start:
Bryan / Brock `mark 1 tron_NNN`, the match `mark 1 match_clock`). Then: a lead-in `wait_frames 120` (the scene settles, nothing flips in the
first 2 s), and per window `log ab: window k/N ON|OFF`, `log_state entrance_step_0` (entrance scenes), `cvar SWITCH VALUE`, `wait_frames W`;
at the end `log scene=ab_end`, `mark 99 ab_end`, `quit`. Output: tools\\input_scripts\\ab\\<scene>_ab.txt.

Why these numbers: 8 windows of 3 s (180 frames; the verdict drops the first 30 frames of each) give 4 ON / OFF pairs, the least that tells a real
difference from window-to-window noise (a 2 + 2 design called "no difference" wrongly 22 % of the time in a simulation, review D). Windows are
counted in GUEST frames. Bryan has ~30 s of entrance after his tron (~25 s to step 15 + 5 s; R10), so 8 x 3 s = 24 s fits; Brock's bound is the
route's own: 600 frames after step 15. The verdict refuses windows that start later than that (the route logs `ab: bound step15+N`).
`--pattern alt` = ON OFF ON OFF ...; `--pattern abba` = ON OFF OFF ON ... (cancels a straight heat drift; the second OFF is NOT a flip, so no
`[switch]` line: the verdict cuts windows at the route's `log ab:` lines, not at switch lines).
The first window must CHANGE the switch from its default or no `[switch]` line proves it: `--first` is chosen from the table default in
rdr_switches.h (a switch whose default equals --on starts OFF). A non-boolean switch needs --on and --off (e.g. --on 4 --off 2).

The harness flags line is rewritten to --vulkan_stored_pipelines_at_boot=true (the scene routes skip the boot compiles for speed; a timed A/B
must have them done before the windows, review F).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switch_table

SCENES = {
    # scene: (route file, frames of scene content after the anchor or None = unlimited, frames after step 15 the route itself allows or None, entrance?, source)
    'bryan': ('bryan_entrance.txt', 1800, 300, True, 'R10 10(b): tron -> step 15 ~25 s + 5 s; the route ends 300 frames after step 15'),
    'brock': ('brock_entrance.txt', None, 600, True, 'the route ends 600 frames after step 15; the length before it is unconfirmed'),
    'match': ('match.txt', None, None, False, 'the match runs as long as asked'),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', required=True, choices=sorted(SCENES))
    ap.add_argument('--switch', required=True, help='the setting to flip, e.g. wwe2k14_rdr_skip_prune')
    ap.add_argument('--windows', type=int, default=8)
    ap.add_argument('--seconds', type=float, default=3.0)
    ap.add_argument('--fps', type=int, default=60, help='guest frames a second (the game steps 60)')
    ap.add_argument('--pattern', choices=('alt', 'abba'), default='alt')
    ap.add_argument('--first', choices=('on', 'off'))
    ap.add_argument('--on')
    ap.add_argument('--off')
    ap.add_argument('--lead-in', type=int, default=120)
    ap.add_argument('--out')
    ap.add_argument('--force', action='store_true', help='allow windows longer than the scene content / fewer than 8')
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    route_name, content_frames, after15, entrance, why = SCENES[args.scene]
    table = switch_table.load()
    info = table.get(args.switch)
    default = info['default'] if info else None
    boolean = info is None or info['default'] in (0, 1)
    if not boolean and (args.on is None or args.off is None):
        sys.exit('make_ab_route: %s is not a boolean switch (default %s): pass --on VALUE and --off VALUE' % (args.switch, default))
    on_value, off_value = args.on or 'true', args.off or 'false'
    if args.on and args.on == args.off:
        sys.exit('make_ab_route: --on and --off are the same value')
    if info and info['kind'] == 'StartOnly':
        sys.exit('make_ab_route: %s is a start-only switch (the table in rdr_switches.h): a mid-run flip is refused, use the between-run A/B' % args.switch)
    if default is None:
        print('make_ab_route: WARNING %s is not in the switch table: nothing re-reads it per frame, the verdict will call it UNVERIFIED' % args.switch)
    if args.first:
        first_on = args.first == 'on'
    else:
        first_on = default is None or switch_table.norm(on_value) != str(default)   # the first window must CHANGE the switch from its default
    if default is not None and (switch_table.norm(on_value) == str(default)) == first_on:
        sys.exit('make_ab_route: the first window would not change %s from its default (%s): no [switch] line would prove it' % (args.switch, default))
    route = os.path.join(root, 'tools', 'input_scripts', 'scenes', route_name)
    lines = open(route, encoding='utf-8').read().split('\n')
    try:
        cut = next(i for i, l in enumerate(lines) if l.startswith('mark 1 '))
    except StopIteration:
        sys.exit('make_ab_route: %s has no "mark 1" line' % route)
    if args.windows < 8 and not args.force:
        sys.exit('make_ab_route: fewer than 8 windows gives under 4 ON / OFF pairs, which the verdict refuses (--force to make it anyway)')
    if args.windows % 2:
        sys.exit('make_ab_route: --windows must be even')
    per = int(round(args.seconds * args.fps))
    if per < 90:
        sys.exit('make_ab_route: %d frames a window is too short to settle and measure (>= 90)' % per)
    total = per * args.windows
    if content_frames is not None and total > content_frames and not args.force:
        sys.exit('make_ab_route: %d windows x %d frames = %d frames, the %s scene has ~%d (%s); shorten --seconds or pass --force'
                 % (args.windows, per, total, args.scene, content_frames, why))
    if args.pattern == 'abba' and args.windows % 4:
        sys.exit('make_ab_route: --pattern abba needs a multiple of 4 windows')
    arms = ['A', 'B'] * (args.windows // 2) if args.pattern == 'alt' else ['A', 'B', 'B', 'A'] * (args.windows // 4)
    head = lines[:cut + 1]
    flags_fixed = 0
    for i, l in enumerate(head):
        if l.startswith('# harness:') and '--vulkan_stored_pipelines_at_boot=false' in l:
            head[i] = l.replace('--vulkan_stored_pipelines_at_boot=false', '--vulkan_stored_pipelines_at_boot=true')
            flags_fixed += 1
    out = list(head)
    out += ['# S102 S0-4: in-run A/B of %s, %d windows x %d frames (%.1f s at %d fps), pattern %s, made by tools\\bench\\make_ab_route.py'
            % (args.switch, args.windows, per, args.seconds, args.fps, args.pattern),
            '# ON = %s, OFF = %s. The route logs `ab: window k/N ON|OFF` at each window start; `[switch] frame N %s=VALUE` proves each flip.'
            % (on_value, off_value, args.switch)]
    if after15 is not None:
        out += ['log ab: bound step15+%d' % after15]
    out += ['wait_frames %d' % args.lead_in]
    for i, arm in enumerate(arms):
        on = (arm == 'A') == first_on
        out += ['log ab: window %d/%d %s' % (i + 1, args.windows, 'ON' if on else 'OFF')]
        if entrance:
            out += ['log_state entrance_step_0']
        out += ['cvar %s %s' % (args.switch, on_value if on else off_value), 'wait_frames %d' % per]
    out += ['log scene=ab_end', 'mark 99 ab_end', 'log bench: done', 'quit', '']
    dest = args.out or os.path.join(root, 'tools', 'input_scripts', 'ab', '%s_ab.txt' % args.scene)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    print('wrote %s: %d windows x %d frames, pattern %s, first window %s, pipelines-at-boot flag rewritten on %d line(s)'
          % (dest, args.windows, per, args.pattern, 'ON' if first_on else 'OFF', flags_fixed))


if __name__ == '__main__':
    main()
