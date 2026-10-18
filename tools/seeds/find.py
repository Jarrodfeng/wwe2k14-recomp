"""Session 16 item 4: list seeds from a sweep (tools/seeds/sweep.py) that give an outcome.

    py tools/seeds/find.py SCENARIO [winner=SLOT] [loser=SLOT] [finish=TYPE] [max_clock=SECONDS] [min_clock=SECONDS]
                                    [min_near_falls=N]

finish: a number (1 pinfall, 6 tag finish, 7 Rumble) or 'pinfall'. Clock = match time at the finish in seconds.
Session 17: clock=MIN..MAX (a match-length range in seconds, either end may be empty), min_pins=N; SCENARIO+DIRECTOR
reads a sweep made with --director (tools/seeds/sweep.py) and the replay hint includes the director script.
Prints the matching rows and the command that replays one of them (replay mode with that seed).
"""
import csv
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    scenario = sys.argv[1]
    cond = dict(kv.split('=', 1) for kv in sys.argv[2:])
    if 'clock' in cond:  # clock=MIN..MAX
        lo, _, hi = cond.pop('clock').partition('..')
        if lo:
            cond['min_clock'] = lo
        if hi:
            cond['max_clock'] = hi
    if cond.get('finish') == 'pinfall':
        cond['finish'] = '1'
    path = os.path.join(REPO, 'out', 'seeds', scenario + '.csv')
    with open(path, newline='') as f:
        rows = [r for r in csv.DictReader(f) if r['status'].startswith('ok')]
    hits = []
    for r in rows:
        ok = True
        for k, v in cond.items():
            if k in ('winner', 'loser', 'finish'):
                ok &= r[k] == v
            elif k == 'max_clock':
                ok &= r['clock_s'] != '' and float(r['clock_s']) <= float(v)
            elif k == 'min_clock':
                ok &= r['clock_s'] != '' and float(r['clock_s']) >= float(v)
            elif k == 'min_near_falls':
                ok &= int(r['near_falls'] or 0) >= int(v)
            elif k == 'min_pins':
                ok &= int(r['pins'] or 0) >= int(v)
            else:
                print('unknown condition %s' % k)
                return 2
        if ok:
            hits.append(r)
    print('%d of %d decided seeds match %s' % (len(hits), len(rows), ' '.join(sys.argv[2:]) or '(no conditions)'))
    for r in hits:
        print('  seed %-10s run %-6s finish %s winner %s loser %s clock %s s near-falls %s' % (
            r['seed'], r['run'], r['finish'], r['winner'], r['loser'], r['clock_s'], r['near_falls']))
    if hits:
        print('replay one (any input script; add --wwe2k14_cinecam=true for the free camera):')
        scen, _, director = scenario.partition('+')
        print("  -Extra '--wwe2k14_launch=%s --wwe2k14_launch_skip_entrances=true --wwe2k14_replay_mode=true "
              "--wwe2k14_fixed_seed=%s%s'" % (scen, hits[0]['seed'],
                                             ' --wwe2k14_director=' + director if director else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
