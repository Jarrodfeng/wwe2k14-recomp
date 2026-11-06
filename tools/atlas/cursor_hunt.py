"""Session 91 (the game atlas): find a screen's cursor words by memory snapshots around known presses.

  py tools/atlas/cursor_hunt.py [--screens wm30,universe,caw,...] [--start] [--quit] [--data userdata_unlocked_s91]
                                [--keep-snaps]

For each screen: open it from the main menu (the list part by crawler v2's state-confirmed navigation, then A and a
wait for the screen's sequence / menu id), let it settle, then take memory snapshots around a fixed press pattern:
  v0, DOWN, v1, DOWN, v2, UP, v3, RIGHT, h1, RIGHT, h2, LEFT, h3
(each press: hold 3, release 3, then 40 frames). A vertical cursor reads (x, x+1, x+2, x+1) over v0..v3, a horizontal
one (y, y+1, y+2, y+1) over v3, h1, h2, h3; `tools/menus/snap_find.py --offset-ok` finds the words that follow it
(the press may have been refused by a screen with no list in that direction: then nothing is found, which is an answer
too). The screen is left with B until the main menu comes back (or the game is restarted, crawl2's cap).
Runs at normal speed (fast-forwarded menus drop presses, LESSONS). Snapshots are ~650 MB each, local only (game
memory: never committed); deleted after each screen unless --keep-snaps.
Output: out/harness/atlas/hunt/<date_time>/hunt.json (per screen: entry check, candidate addresses with their values).
"""
import argparse
import datetime
import glob
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import crawl2  # noqa: E402

LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')
# name: (list path to the item as [(list, item)], the item's A lands on: ('seq', id) or ('menu', id), settle frames)
SCREENS = {
    'wm30': ([(1, 1)], ('seq', 31), 600),  # sequence 31 = the era cards (Jarrod's recording, run 5895)
    'universe': ([(1, 2)], ('menu', 1497), 600),
    'shop': ([(1, 7)], ('seq', 5), 900),
    'caw': ([(1, 3), (3, 0), (14, 0)], ('seq', 76), 900),
    'threads': ([(1, 3), (3, 1)], ('seq', 77), 600),
    'entrance': ([(1, 3), (3, 2)], ('seq', 25), 900),
    'moves': ([(1, 3), (3, 3)], ('seq', 20), 600),
    'finisher': ([(1, 3), (3, 4), (15, 0), (28, 0)], ('seq', 26), 600),
    'story': ([(1, 3), (3, 5), (16, 0)], ('seq', 9), 1200),
    'arena': ([(1, 3), (3, 6), (17, 0)], ('seq', 28), 900),
}
PATTERN = [('v0', None), ('v1', 'DOWN'), ('v2', 'DOWN'), ('v3', 'UP'), ('h1', 'RIGHT'), ('h2', 'RIGHT'), ('h3', 'LEFT')]


def snap_find(run, pairs, limit=40):
    cmd = [sys.executable, os.path.join(REPO, 'tools', 'menus', 'snap_find.py'), str(run)] + \
          ['%s=%d' % p for p in pairs] + ['--offset-ok', '--limit', str(limit)]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    return [ln for ln in (out.stdout + out.stderr).splitlines() if ln.strip()]


def hunt(c, name, run):
    hops, (kind, want), settle = SCREENS[name]
    res = {'screen': name}
    try:
        c.to_main()
        for lst, item in hops[:-1]:
            if not c.cursor_to(item, lst):
                raise crawl2.Lost('cursor %d/%d' % (lst, item))
            sub = next(i for i in c.graph.menus[lst]['items'] if i['item'] == item)['submenu']
            c.g.batch(['press_until_state A menu %d 4 90 5' % sub, 'wait_frames 10'])
        lst, item = hops[-1]
        if not c.cursor_to(item, lst):
            raise crawl2.Lost('cursor %d/%d' % (lst, item))
        ok, st, _, failed, _ = c.g.batch(['press_until_state A %s %d 2 600 5' % (kind, want),
                                          'wait_frames %d' % settle, 'log_state seq', 'log_state menu', 'log_text'],
                                         timeout=600)
        res['entry'] = {'ok': ok and not failed, 'seq': st.get('seq'), 'menu': st.get('menu')}
        if failed:
            raise crawl2.Lost('the screen did not open (%s)' % failed)
        lines = []
        for snap, btn in PATTERN:
            if btn:
                lines += ['press %s 3 3' % btn, 'wait_frames 40']
            lines += ['cvar wwe2k14_snapshot_now hunt_%s_%s' % (name, snap), 'wait_frames 30']
        lines += ['log_state seq', 'log_text', 'screenshot hunt_%s' % name]
        ok, st, text, failed, _ = c.g.batch(lines, timeout=900)
        res['after'] = {'seq': st.get('seq'), 'text': text, 'ok': ok and not failed}
        v = [('hunt_%s_v%d' % (name, k), x) for k, x in enumerate((0, 1, 2, 1))]
        h = [('hunt_%s_%s' % (name, s), x) for s, x in (('v3', 0), ('h1', 1), ('h2', 2), ('h3', 1))]
        res['vertical'] = snap_find(run, v)
        res['horizontal'] = snap_find(run, h)
        # a static chain to each candidate (heap addresses move between runs): reverse pointer scan in v0, checked
        # against the other snapshots
        res['chains'] = {}
        for axis in ('vertical', 'horizontal'):
            for ln in res[axis][1:4]:
                addr = ln.split()[0]
                cmd = [sys.executable, os.path.join(REPO, 'tools', 'menus', 'pointer_scan.py'), str(run),
                       'hunt_%s_v0' % name, addr, '--check'] + \
                      [','.join('hunt_%s_%s' % (name, s) for s, _ in PATTERN[1:])]
                out = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, timeout=600)
                res['chains'][addr] = [x for x in (out.stdout + out.stderr).splitlines() if x.strip()][:12]
    except crawl2.Lost as x:
        res['lost'] = str(x)
    finally:
        try:
            c.come_back(1, {'item': -1}, res)
        except crawl2.Lost as x:
            res['back_lost'] = str(x)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--flow', default=os.path.join(REPO, 'out', 'harness', 'atlas', 'flow_5889.json'))
    ap.add_argument('--screens', default=','.join(SCREENS))
    ap.add_argument('--name', default='hunt')
    ap.add_argument('--start', action='store_true')
    ap.add_argument('--quit', action='store_true')
    ap.add_argument('--data', default='userdata_unlocked_s91')
    ap.add_argument('--extra', default=crawl2.STOCK)
    ap.add_argument('--keep-snaps', action='store_true')
    a = ap.parse_args()
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M')
    out_dir = os.path.join(REPO, 'out', 'harness', 'atlas', 'hunt', stamp)
    os.makedirs(out_dir, exist_ok=True)
    results = []
    with open(os.path.join(out_dir, 'hunt.log'), 'w', encoding='utf-8') as logf:
        game = crawl2.Game(a.name, logf, a.extra, a.data, False)
        game.fast = False
        if a.start:
            game.start()
        else:
            fields, _ = crawl2.live.parse(crawl2.live.request(a.name, '!status'))
            game.run = fields.get('run')
        c = crawl2.Crawler(game, crawl2.Graph(a.flow))
        try:
            for name in [s for s in a.screens.split(',') if s]:
                r = hunt(c, name, game.run)
                results.append(r)
                game.say('%s: entry %s, vertical %d lines, horizontal %d lines%s' % (
                    name, r.get('entry'), len(r.get('vertical', [])), len(r.get('horizontal', [])),
                    ' LOST %s' % r['lost'] if 'lost' in r else ''))
                if not a.keep_snaps:
                    for p in glob.glob(os.path.join(LOGS, 'snap_%s_hunt_%s_*' % (game.run, name))):
                        os.remove(p)
        finally:
            with open(os.path.join(out_dir, 'hunt.json'), 'w', encoding='utf-8') as f:
                json.dump({'run': game.run, 'results': results}, f, indent=1)
            game.say('done -> %s' % out_dir)
            if a.quit:
                try:
                    crawl2.live.request(a.name, 'quit', connect_wait=2.0)
                except crawl2.live.NoGame:
                    pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
