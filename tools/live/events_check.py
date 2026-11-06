"""Session 91 step 7's check: the event bus sees every kind of transition (one game, fast-forwarded).

  py tools/live/events_check.py [--name evchk]

Tour: main menu -> VISUAL PRESENTATION (a value changed and left: msgbox events) -> Create a Superstar and back (seq)
-> a built 1v1 launched (exit, seq 9, loads, entrance steps); the match start is WAITED FOR ON THE BUS (no polling),
START skips entrances. PASS = each kind seen at least once: seq, screen, exit, msgbox, match, entrance, load.
"""
import argparse
import collections
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))
import events  # noqa: E402
import nav  # noqa: E402

KINDS = ['seq', 'screen', 'exit', 'msgbox', 'match', 'entrance', 'load']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', default='evchk')
    a = ap.parse_args()
    print(nav.live.start(a.name, fast=True, data='userdata_unlocked_s91'), flush=True)
    n = nav.Nav(a.name)
    n.batch(['wait_state menu 1 3600 30'], timeout=600)
    fields, _ = nav.live.parse(nav.live.request(a.name, '!status'))
    ev = events.Events(int(fields.get('run')))
    try:
        n.go('screen:1300')
        n.batch(['press RIGHT 4 8', 'wait_frames 30'])
        n.go('mode:caw')
        n.go('list:1')
        go = int(time.time() * 10) % 1000000 + 1
        n.batch(['cvar wwe2k14_match type=0;p=324:0,343:-1;arena=4', 'cvar wwe2k14_match_dry false',
                 'cvar wwe2k14_match_go %d' % go], timeout=60)
        # on the bus: the match sequence, then press START through the entrances until the clock starts
        e9 = ev.wait({'kind': 'seq', 'seq': 9}, 120)
        print('seq 9 event: %s' % e9, flush=True)
        clock = None
        for _ in range(40):
            n.batch(['press START 5 10', 'wait_frames 60'], timeout=60)
            clock = ev.wait({'kind': 'match', 'what': 'clock_started'}, 2)
            if clock:
                break
        print('clock_started event: %s' % clock, flush=True)
    finally:
        try:
            nav.live.request(a.name, 'quit', connect_wait=2.0)
        except nav.live.NoGame:
            pass
    allev = ev.read(since_start=True)
    count = collections.Counter(e['kind'] for e in allev)
    for k in KINDS:
        sample = next((e for e in allev if e['kind'] == k), None)
        print('%s %-9s %4d  e.g. %s' % ('PASS' if count[k] else 'FAIL', k, count[k], sample))
    print('%d / %d kinds seen; %d events in %s' % (sum(1 for k in KINDS if count[k]), len(KINDS), len(allev), ev.path))
    return 0


if __name__ == '__main__':
    sys.exit(main())
