"""Session 91 step 4's check: the match builder against menu captures (bytes) and in play (fast-forwarded).

  py tools/atlas/build_check.py compare [NAMES ...]   one game: each capture rebuilt dry (finalize, no launch) and
                                                      compared (M at apply entry, G / A at apply exit); then one
                                                      real build of the first capture played into the ring
  py tools/atlas/build_check.py play "SPEC WORDS"     one game: build from names (tools/atlas/match.py), play
Fast-forwarded (Jarrod, 2026-11-04: "ran sped up, so we don't spend ages waiting"): entrances skipped with START,
PASS = the match clock running + a screenshot in the ring, then the game is closed. Frozen save (the captures' save).
Reads the results from the run's log ([wwe2k14][build] lines).
"""
import argparse
import glob
import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import match  # noqa: E402
import nav  # noqa: E402
import spec_from_capture  # noqa: E402

LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')
CAPS = ['lesnar_bryan', 'sixpack', 'hiac_cena_brodus', 's30_cage', 'chamber_6man']


def start(name):
    print(nav.live.start(name, fast=True), flush=True)
    n = nav.Nav(name)
    n.batch(['wait_state menu 1 3600 30'], timeout=600)
    fields, _ = nav.live.parse(nav.live.request(name, '!status'))
    return n, fields.get('run')


def log_lines(run, pattern):
    path = sorted(glob.glob(os.path.join(LOGS, 'wwe2k14_%s*.log' % run)))[0]
    return [ln.rstrip() for ln in open(path, encoding='utf-8', errors='replace') if pattern in ln]


def build(n, spec, dry, compare=''):
    n.go('list:1')
    go = int(time.time() * 10) % 1000000 + 1
    n.batch(['cvar wwe2k14_match %s' % spec, 'cvar wwe2k14_match_dry %s' % ('true' if dry else 'false'),
             'cvar wwe2k14_match_compare %s' % (compare or '""'), 'cvar wwe2k14_match_go %d' % go,
             'wait_frames 150'], timeout=300)


def play(n, shot):
    st, _, failed = n.batch(['wait_state seq 9 3600 2', 'press_until_state START match_clock >0 40 240 5',
                             'wait_frames 120', 'log_state match_clock', 'screenshot %s' % shot], timeout=900)
    return failed, st.get('match_clock')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['compare', 'play'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--name', default='bchk')
    a = ap.parse_args()
    n, run = start(a.name)
    print('run %s' % run, flush=True)
    try:
        if a.mode == 'compare':
            caps = a.args or CAPS
            for cap in caps:
                spec = spec_from_capture.spec_for(cap, os.path.join(REPO, 'out', 'scenarios'))
                build(n, spec, True, cap)
                print('%s: %s' % (cap, spec), flush=True)
            for ln in log_lines(run, '[wwe2k14][build]'):
                print('  ' + ln.split('[wwe2k14][build] ', 1)[1][:400])
            spec = spec_from_capture.spec_for(caps[0], os.path.join(REPO, 'out', 'scenarios'))
            build(n, spec, False)
            failed, clock = play(n, 'build_%s' % caps[0])
            print('PLAY %s: %s (match clock %s)' % (caps[0], 'FAIL ' + failed if failed else 'PASS', clock))
        else:
            spec, summary = match.build_spec(' '.join(a.args))
            print('spec: %s (%s)' % (spec, summary), flush=True)
            build(n, spec, False)
            failed, clock = play(n, 'build_play')
            for ln in log_lines(run, '[wwe2k14][build]'):
                print('  ' + ln.split('[wwe2k14][build] ', 1)[1][:400])
            print('PLAY: %s (match clock %s)' % ('FAIL ' + failed if failed else 'PASS', clock))
    finally:
        try:
            nav.live.request(a.name, 'quit', connect_wait=2.0)
        except nav.live.NoGame:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
