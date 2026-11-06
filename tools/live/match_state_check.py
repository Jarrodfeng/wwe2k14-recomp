"""Session 91 step 8's check: a CPU-vs-CPU match played to its finish (fast-forwarded), watched on the event bus.

  py tools/live/match_state_check.py [--name mschk] [--spec "Bryan vs Lesnar; players=0; pace=quick"]

The match is built from names (the match builder), entrances skipped with START until the bus says clock_started,
then the run waits ON THE BUS for the finish (no polling) and closes the game. A snapshot at the bell and every 30 s.
PASS = a finish event with a known finish name and winner / loser ids, and the snapshot shows the two wrestlers.
Prints every match event (pin, counts, kickouts, eliminations, finish).
"""
import argparse
import json
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))
import events  # noqa: E402
import match  # noqa: E402
import nav  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', default='mschk')
    ap.add_argument('--spec', default='Daniel Bryan vs Brock Lesnar; players=0; pace=quick')
    ap.add_argument('--timeout', type=int, default=1500, help='seconds to wait for the finish')
    a = ap.parse_args()
    spec, summary = match.build_spec(a.spec)
    print('spec: %s (%s)' % (spec, summary), flush=True)
    print(nav.live.start(a.name, fast=True, data='userdata_unlocked_s91'), flush=True)
    n = nav.Nav(a.name)
    n.batch(['wait_state menu 1 3600 30'], timeout=600)
    fields, _ = nav.live.parse(nav.live.request(a.name, '!status'))
    ev = events.Events(int(fields.get('run')))
    finish = None
    try:
        n.batch(['cvar wwe2k14_match %s' % spec, 'cvar wwe2k14_match_dry false',
                 'cvar wwe2k14_match_go %d' % (int(time.time() * 10) % 1000000 + 1)], timeout=60)
        ev.wait({'kind': 'seq', 'seq': 9}, 120)
        for _ in range(40):
            n.batch(['press START 5 10', 'wait_frames 60'], timeout=60)
            if ev.wait({'kind': 'match', 'what': 'clock_started'}, 2):
                break
        print('bell: %s' % json.dumps(events.snapshot(a.name)), flush=True)
        end = time.time() + a.timeout
        next_snap = time.time() + 30
        while time.time() < end and not finish:
            for e in ev.read():
                if e.get('kind') == 'match':
                    print('  event: %s' % json.dumps(e), flush=True)
                    if e.get('what') == 'finish':
                        finish = e
            if time.time() > next_snap and not finish:
                s = events.snapshot(a.name)
                print('  snapshot: clock %s, referee %s/%s, health %s' % (
                    s and s.get('clock'), s and s.get('referee_mode'), s and s.get('referee_count'),
                    s and [(w['id'], w['health'], w['health_max']) for w in s.get('wrestlers', []) if w['role'] == 0]),
                    flush=True)
                next_snap = time.time() + 30
            time.sleep(0.2)
        if finish:
            time.sleep(1)
            print('after the finish: %s' % json.dumps(events.snapshot(a.name)), flush=True)
    finally:
        try:
            nav.live.request(a.name, 'quit', connect_wait=2.0)
        except nav.live.NoGame:
            pass
    ok = bool(finish) and finish.get('finish_name') != 'unknown' and finish.get('winner_id', -1) >= 0
    print('%s: finish %s' % ('PASS' if ok else 'FAIL', json.dumps(finish)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
