#!/usr/bin/env python3
"""twin_lint (S103 SLV2): every gate check that claims a picture `max diff 0` carries a planted twin that shows diff > 0.

A bar of 0 that cannot fail proves nothing (the SLV review: its post part compared two pictures that were always equal). Convention in a lane json (tools\\gate\\lanes\\*.json):
  the claiming check:  "picture_diff0": true          and the script text it runs carries `(bar 0)` or `max diff 0` in a message
  its twin:            "twin_of": "<claiming check name, same lane>"   (a check that PASSES only when a planted fault gives diff above 0)
Rules (each error is `lane: text`):
  T1 a check with picture_diff0 has at least one check in the same lane whose twin_of names it.
  T2 a twin_of names a check of the same lane that has picture_diff0; a twin is never itself picture_diff0.
  T3 a py script run by a lane check whose text holds `(bar 0)` / `max diff 0` in a `say(` message is run by some check that declares picture_diff0 (an undeclared claim is the hole).
  py -I tools\\gate\\twin_lint.py [--root DIR] | --selftest
"""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
CLAIM = re.compile(r'say\([^\n]*(\(bar 0\)|max diff 0)')


def script_of(cmd):
    for a in cmd:
        if a.endswith('.py'):
            return a
    return None


def check_lanes(lanes, read_script):
    """lanes: {lane name: json}. read_script(path) -> text or None."""
    problems, declared_scripts, claim_scripts = [], set(), {}
    for lane, spec in lanes.items():
        checks = spec.get('checks', [])
        names = {c['name']: c for c in checks}
        for c in checks:
            if c.get('picture_diff0'):
                if c.get('twin_of'):
                    problems.append('%s: %s is both picture_diff0 and a twin' % (lane, c['name']))
                if not any(t.get('twin_of') == c['name'] for t in checks):
                    problems.append('%s: %s claims picture max diff 0 but no check of the lane has twin_of=%s (a planted twin that shows diff > 0)' % (lane, c['name'], c['name']))
                s = script_of(c.get('cmd', []))
                if s:
                    declared_scripts.add(s)
            if c.get('twin_of'):
                t = names.get(c['twin_of'])
                if t is None or not t.get('picture_diff0'):
                    problems.append('%s: %s is twin_of %s, which is not a picture_diff0 check of this lane' % (lane, c['name'], c['twin_of']))
            s = script_of(c.get('cmd', []))
            if s and s not in claim_scripts:
                text = read_script(s)
                claim_scripts[s] = bool(text and CLAIM.search(text))
    for s, claims in claim_scripts.items():
        if claims and s not in declared_scripts:
            problems.append('%s: the script has a `picture max diff 0` bar in a message but no lane check running it declares picture_diff0 (declare it and add its twin)' % s)
    return problems


def load(root):
    lanes = {}
    for f in sorted(glob.glob(os.path.join(root, 'tools', 'gate', 'lanes', '*.json'))):
        with open(f, encoding='utf-8') as fh:
            lanes[os.path.basename(f)[:-5]] = json.load(fh)

    def read(path):
        try:
            with open(os.path.join(root, path), encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return None
    return lanes, read


def selftest():
    claim_script = 'sa' + 'y(d == 0, "x picture max diff %d (bar 0)")'
    read = lambda p: claim_script if p == 'a.py' else None
    good = {'L': {'checks': [{'name': 'L.a', 'cmd': ['py', 'a.py'], 'picture_diff0': True}, {'name': 'L.t', 'cmd': ['py', 'a.py', '--twin'], 'twin_of': 'L.a'}]}}
    plants = {
        'claim with no twin': {'L': {'checks': [{'name': 'L.a', 'cmd': ['py', 'a.py'], 'picture_diff0': True}]}},
        'twin of a check that is not a claim': {'L': {'checks': [{'name': 'L.a', 'cmd': ['py', 'a.py']}, {'name': 'L.t', 'cmd': ['py', 'a.py'], 'twin_of': 'L.a'}]}},
        'undeclared claim in a script': {'L': {'checks': [{'name': 'L.a', 'cmd': ['py', 'a.py']}]}},
    }
    bad = 0
    p = check_lanes(good, read)
    print('good lane -> %s' % ('passes' if not p else 'FLAGGED %s' % p))
    bad += bool(p)
    for n, lanes in plants.items():
        p = check_lanes(lanes, read)
        print('plant %-40s -> %s' % (n, 'caught' if p else 'NOT CAUGHT'))
        bad += not p
    print('SELFTEST %s' % ('FAIL' if bad else 'ok'))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    lanes, read = load(a.root)
    problems = check_lanes(lanes, read)
    for p in problems:
        print('PROBLEM: ' + p)
    print('twin_lint: %d lane(s), %d problem(s)' % (len(lanes), len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
