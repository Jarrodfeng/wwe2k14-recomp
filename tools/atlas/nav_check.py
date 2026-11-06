"""Session 91 step 3's check: drive the navigator through a fixed tour of targets on a live game and report each leg.

  py tools/atlas/nav_check.py [--name navchk] [--start] [--quit] [--data userdata_unlocked_s91] [--targets "a;b;c"]

Each leg: game_nav TARGET from wherever the previous leg ended; PASS when where() then reports the target node.
The default tour crosses every family: PLAY setup screens (deep), MATCH RULES, option pages, lists, Universe, the
creation modes (out and back), 30 Years. Fast-forwarded (every press is state-confirmed). Output: the pass list.
"""
import argparse
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import nav  # noqa: E402

TOUR = ['one on one > ladder', 'royal rumble > 30', 'options', 'visual presentation', 'tag team > tornado tag',
        'create a superstar', 'universe', 'match rules > fatal 4-way > ladder', 'special referee', 'list:1',
        '30 years', 'entrance', 'elimination chamber > normal', 'unlockables', 'main menu']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', default='navchk')
    ap.add_argument('--start', action='store_true')
    ap.add_argument('--quit', action='store_true')
    ap.add_argument('--data', default='userdata_unlocked_s91')
    ap.add_argument('--targets', default='')
    a = ap.parse_args()
    if a.start:
        print(nav.live.start(a.name, fast=True, data=a.data), flush=True)
        n0 = nav.Nav(a.name)
        n0.batch(['wait_state menu 1 3600 30'], timeout=600)
    n = nav.Nav(a.name)
    results = []
    for t in ([x.strip() for x in a.targets.split(';') if x.strip()] or TOUR):
        t0 = time.time()
        try:
            log = n.go(t)
            dst = n.atlas.resolve(t)[0]
            w = n.where()
            ok = w['node'] == dst
            results.append((t, 'PASS' if ok else 'FAIL', time.time() - t0, log[-1] if ok else nav.Nav.describe(w)))
        except nav.Lost as x:
            results.append((t, 'LOST', time.time() - t0, str(x)[:300]))
        r = results[-1]
        print('%-4s %-40s %5.1f s  %s' % (r[1], r[0], r[2], r[3]), flush=True)
    print('%d / %d PASS' % (sum(r[1] == 'PASS' for r in results), len(results)))
    if a.quit:
        try:
            nav.live.request(a.name, 'quit', connect_wait=2.0)
        except nav.live.NoGame:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
