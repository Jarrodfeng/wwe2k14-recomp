"""Session 91 step 5: unattended acceptance of the atlas, the navigator and the match builder (one game).

  py tools/atlas/accept.py [--name accept] [--pairs 12] [--seed N] [--data userdata_unlocked_s91]

Fast-forwarded, on the unlocked test save:
  1. random navigation: N targets drawn from the atlas's verified, non-dead-end nodes; each leg goes from wherever the
     last one ended; PASS = where() reports the target node; a screenshot per leg (the atlas contact sheet);
  2. a dropped press during a load: A pressed on CREATIONS > LOGO and B released into the load at once, then
     game_nav back to the main menu (recovery);
  3. a changed option page left: VISUAL PRESENTATION, RIGHT (a value change), then game_nav away (the keep-settings
     box, answered YES - run 5906);
  4. one built match (dry, finalize only) per family from names: tag, Hell in a Cell, ladder, steel cage, handicap,
     triple threat, fatal 4-way, Elimination Chamber, a 30-man Rumble: PASS = the build log line, no failure.
Writes out/harness/atlas/accept/<date_time>/accept.json and prints the pass list.
"""
import argparse
import datetime
import glob
import json
import os
import random
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import match  # noqa: E402
import nav  # noqa: E402

LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')
BUILDS = ['The Shield (#367), Rollins, Reigns, Cena; type=two on two > tag team',
          'Cena vs Brodus; type=one on one > hell in a cell', 'Bryan vs Lesnar; type=one on one > ladder',
          'Cena vs Mysterio; type=one on one > steel cage', 'Big Show vs Bryan, Punk; type=handicap > one on two > tag team',
          'Cena, Orton, Bryan; type=triple threat > normal', 'Cena, Orton, Bryan, Punk; type=fatal 4-way > normal',
          'Cena, Orton, Bryan, Punk, Sheamus, Del Rio; type=elimination chamber > normal',
          'royal rumble; type=royal rumble > 30; rumble=30 random']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', default='accept')
    ap.add_argument('--pairs', type=int, default=12)
    ap.add_argument('--seed', type=int, default=91)
    ap.add_argument('--data', default='userdata_unlocked_s91')
    ap.add_argument('--only', default='', help='box,build: run only these parts (a re-check after a fix)')
    a = ap.parse_args()
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M')
    out = os.path.join(REPO, 'out', 'harness', 'atlas', 'accept', stamp)
    os.makedirs(out, exist_ok=True)
    print(nav.live.start(a.name, fast=True, data=a.data), flush=True)
    n = nav.Nav(a.name)
    n.batch(['wait_state menu 1 3600 30'], timeout=600)
    fields, _ = nav.live.parse(nav.live.request(a.name, '!status'))
    run = fields.get('run')
    res = []

    def rec(kind, what, ok, note):
        res.append({'kind': kind, 'what': what, 'ok': bool(ok), 'note': note})
        print('%-4s %-9s %-55s %s' % ('PASS' if ok else 'FAIL', kind, what[:55], note[:160]), flush=True)

    try:
        parts = set(a.only.split(',')) if a.only else {'nav', 'recover', 'box', 'build'}
        # 1. random legs
        rnd = random.Random(a.seed)
        pool = [k for k, v in n.atlas.nodes.items() if v.get('recognise') and not v.get('dead_end')
                and n.atlas.plan(k, 'list:1') is not None and n.atlas.plan('list:1', k) is not None]
        for i, t in enumerate(rnd.sample(pool, min(a.pairs, len(pool))) if 'nav' in parts else []):
            t0 = time.time()
            try:
                n.go(t)
                w = n.where()
                n.batch(['screenshot atlas_%02d_%s' % (i, t.replace(':', '_'))])
                rec('nav', '%s (%s)' % (t, n.atlas.names.get(t)), w['node'] == t,
                    '%.1f s; %s' % (time.time() - t0, nav.Nav.describe(w)))
            except nav.Lost as x:
                rec('nav', t, False, str(x))
        # 2. a dropped press during a load, then recovery
        try:
            if 'recover' not in parts:
                raise StopIteration
            n.go('list:3')
            n.row_to({'from': 'list:3', 'how': {'item': 8}, 'target': 1522})
            n.batch(['press A 4 0', 'press B 2 2', 'wait_frames 200'])  # B into the logo tool's load
            n.go('list:1')
            w = n.where()
            rec('recover', 'press dropped during the logo load', w['node'] == 'list:1', nav.Nav.describe(w))
        except StopIteration:
            pass
        except nav.Lost as x:
            rec('recover', 'press dropped during the logo load', False, str(x))
        # 3. a changed option page left (keep-settings box)
        try:
            if 'box' not in parts:
                raise StopIteration
            n.go('screen:1300')
            n.batch(['press RIGHT 4 8', 'wait_frames 30'])
            n.go('list:1')
            rec('box', 'VISUAL PRESENTATION changed, then away', n.where()['node'] == 'list:1', 'keep-settings box')
        except StopIteration:
            pass
        except nav.Lost as x:
            rec('box', 'VISUAL PRESENTATION changed, then away', False, str(x))
        # 4. built matches, dry
        for text in (BUILDS if 'build' in parts else []):
            try:
                spec, summary = match.build_spec(text)
            except match.SpecError as x:
                rec('build', text, False, 'SPEC: %s' % x)
                continue
            n.go('list:1')
            go = int(time.time() * 10) % 1000000 + 1
            n.batch(['cvar wwe2k14_match %s' % spec, 'cvar wwe2k14_match_dry true', 'cvar wwe2k14_match_compare ""',
                     'cvar wwe2k14_match_go %d' % go, 'wait_frames 150'], timeout=300)
            path = sorted(glob.glob(os.path.join(LOGS, 'wwe2k14_%s*.log' % run)))[0]
            lines = [ln for ln in open(path, encoding='utf-8', errors='replace') if '[wwe2k14][build]' in ln]
            last = lines[-1].split('[wwe2k14][build] ', 1)[1].strip() if lines else 'no build line'
            rec('build', summary, last.startswith('built (dry'), last)
    finally:
        with open(os.path.join(out, 'accept.json'), 'w', encoding='utf-8') as f:
            json.dump({'run': run, 'results': res}, f, indent=1)
        print('%d / %d PASS (run %s) -> %s' % (sum(r['ok'] for r in res), len(res), run, out))
        try:
            nav.live.request(a.name, 'quit', connect_wait=2.0)
        except nav.live.NoGame:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
