"""S101 N1 (go-anywhere): one verb that takes the running game to an address.

    py -I tools/goto/goto.py ADDRESS [--dry-run] [--name main]
    py -I tools/goto/goto.py --help-forms

ADDRESS: menu:PATH | PLACE@WHEN (S101 lane 102: tools/goto/when.py; match:SPEC@pin-60f, moment:NAME@kickout:2~late) | match:SPEC[@entrance:N][@bell] | wm30:N[@moment:K] | caw:CAT/ITEM | universe:PAGE | cutscene:ID |
         scene:ID[:CHAR[/ATTIRE],...] (S101 lane 38: the stage push) | match:SPEC@scene:ID[,ID...] | match:SPEC@victory:ID[/L]  (S101 lane 32, the scene director)
(tools/goto/address.py; docs/systems/goto.md). --dry-run prints the plan and needs no game. Without it the plan runs against the live
game (--wwe2k14_live NAME; tools/live/live.py start). A goto that cannot arrive stops, says where the game is and takes the F11
bookmark bundle. Exit: 0 arrived / plan printed, 1 stopped or error, 2 not reachable yet.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import address  # noqa: E402
import resolve  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('address', nargs='?')
    ap.add_argument('--dry-run', action='store_true', help='print the plan, touch no game')
    ap.add_argument('--name', default='main', help='live channel name')
    ap.add_argument('--help-forms', action='store_true')
    ap.add_argument('--live', action='store_true', help='S101 lane 102: match:SPEC@WHEN builds the place and waits for the event even if a library recording has it')
    ap.add_argument('--recorded', action='store_true', help='S101 lane 102: match:SPEC@WHEN uses a library recording of the place or fails')
    ap.add_argument('--passive-cpu', action='store_true', help='lane 161: once the match starts, every CPU-driven wrestler is made passive (a check route with no CPU variance); per-slot read-back in the log')
    ap.add_argument('--director', action='append', default=[], metavar='TEXT', help='lane 161: after arriving, ask the situation director (passive 1 on | passive all off | momentum 2 100 | stock 0 3); repeatable')
    a = ap.parse_args(argv)
    if a.help_forms or not a.address:
        print(address.__doc__)
        return 0 if a.help_forms else 1
    if a.live and a.recorded:
        ap.error('--live and --recorded together')
    plan = resolve.resolve(a.address, resolve.Context(route='live' if a.live else 'recorded' if a.recorded else 'auto'))
    try:
        resolve.add_director(plan, a.director, a.passive_cpu)   # lane 161
    except address.AddressError as x:
        print('ERROR  %s' % x)
        return 1
    print('\n'.join(plan.lines()))
    if plan.status == 'error':
        return 1
    if plan.status == 'not_reachable':
        return 2
    if a.dry_run:
        return 0
    try:
        resolve.execute(plan, a.name)
    except resolve.Stopped as x:
        print(resolve.failure_report(a.name, str(x)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
