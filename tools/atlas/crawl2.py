"""Session 91 (the game atlas, step 2): crawler v2 - walk every SmartMenu edge of the read list graph, state-confirmed.

  py tools/atlas/crawl2.py [--flow out/harness/atlas/flow_5889.json] [--name crawl] [--start] [--quit]
                           [--menus 2,3,4] [--exits] [--shots] [--data userdata_unlocked]

Drives a live game (live channel, fast-forwarded) through the list graph that `tools/menus/flow_graph.py` read from the
menu flow table. For every item of every list menu it: opens the list by the shortest path from the main menu, moves
the cursor to the item (each move confirmed by the cursor word), records the highlighted target (SM+368), presses A
(confirmed by the state change), records where it landed (menu id, screen controller mode, sequence id, screen text,
optional screenshot), and comes back with B (confirmed: the list and its cursor again). Items that leave the main menu
sequence (exit actions: Creation Suite modes, Shop, 30 Years, ...) are walked only with --exits, after the rest; each
is recovered by B presses (logged) or, when that fails, by closing the game and starting a new one.

Every edge is recorded with its verdict against the read graph (expected target vs reached): `ok`, `mismatch`,
`refused` (A did nothing: a locked / gated item), `unreachable` (the cursor never reached the item: hidden), `lost`.
Output: out/harness/atlas/crawl/<date_time>/edges.json + crawl.log (+ screenshots in the run's captures).
States used (tools/input_scripts/states.toml): seq, smart_menu, menu, cursor, target, screen_mode, exit_action.
Take the crawl with the stock menus (default --extra): the flow table read is the stock one (our GRAPHICS / QUIT items
shift OPTIONS by one).
"""
import argparse
import collections
import datetime
import json
import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))
import live  # noqa: E402

STATE = re.compile(r'line \d+(?: \([^)]*\))?: state (\w+) = (?:0x[0-9A-F]+ \((-?\d+)\)|(unreadable))')
TEXT = re.compile(r'line \d+(?: \([^)]*\))?: screen text: (.*) \(frame \d+\)$')
FAILED = re.compile(r'script FAILED at line (\d+)')
STATES = ('seq', 'smart_menu', 'menu', 'cursor', 'target', 'screen_mode', 'exit_action')
STOCK = '--wwe2k14_gfx_menu=false --wwe2k14_quit_item=false'


class Lost(Exception):
    pass


class Graph:
    def __init__(self, path):
        with open(path, encoding='utf-8') as f:
            g = json.load(f)
        self.menus = {m['menu']: m for m in g['menus']}
        self.parent = {}  # list menu -> (parent menu, item)
        for m in g['menus']:
            for it in m['items']:
                sub = it.get('submenu')
                if isinstance(sub, int):
                    self.parent.setdefault(sub, (m['menu'], it['item']))

    def path(self, menu):
        """[(list, item), ...] from the main menu (1) to `menu`; None when not reachable from 1."""
        hops = []
        while menu != 1:
            if menu not in self.parent:
                return None
            p, i = self.parent[menu]
            hops.append((p, i))
            menu = p
        return list(reversed(hops))


class Game:
    def __init__(self, name, log, extra, data, shots):
        self.name, self.logf, self.extra, self.data, self.shots = name, log, extra, data, shots
        self.run = None

    def say(self, text):
        print(text, flush=True)
        self.logf.write(time.strftime('%H:%M:%S ') + text + '\n')
        self.logf.flush()

    def batch(self, lines, timeout=180):
        fields, log, pairs = live.run_batch(self.name, lines, timeout)
        st, text, failed = {}, None, None
        for ln in log:
            m = STATE.search(ln)
            if m:
                st[m.group(1)] = None if m.group(3) else int(m.group(2))
            m = TEXT.search(ln.rstrip())
            if m:
                text = m.group(1)
            m = FAILED.search(ln)
            if m:
                failed = ln.strip()
        return fields.get('status') == 'ok', st, text, failed, pairs

    def state(self, extra_lines=()):
        ok, st, text, _, pairs = self.batch(list(extra_lines) + ['log_state %s' % s for s in STATES] + ['log_text'])
        return st, text, pairs

    def start(self):
        self.say(live.start(self.name, fast=getattr(self, 'fast', True), extra=self.extra, data=self.data))
        ok, st, _, _, _ = self.batch(['wait_state menu 1 3600 30', 'log_state menu'], timeout=600)
        fields, _ = live.parse(live.request(self.name, '!status'))
        self.run = fields.get('run')
        self.say('live run %s, menu %s' % (self.run, st.get('menu')))
        if st.get('menu') != 1:
            raise Lost('the main menu did not come up')

    restarts, max_restarts = 0, 4

    def restart(self):
        self.restarts += 1
        if self.restarts > self.max_restarts:
            raise Lost('restart cap (%d) reached: stopping (rule 7 run budget)' % self.max_restarts)
        self.say('restart %d: closing the game' % self.restarts)
        try:
            live.request(self.name, 'quit', connect_wait=2.0)
        except live.NoGame:
            pass
        for _ in range(60):
            time.sleep(2)
            try:
                live.request(self.name, '!status', connect_wait=0.3)
            except live.NoGame:
                break
        time.sleep(5)
        self.start()


class Crawler:
    def __init__(self, game, graph):
        self.g, self.graph = game, graph
        self.edges = []
        self.labels = {}

    # ---- navigation (the core the navigator reuses) ----
    def to_main(self):
        """B until the main menu list is up (menu 1). Raises Lost."""
        for _ in range(8):
            st, _, _ = self.g.state()
            if st.get('seq') == 75 and st.get('menu') == 1:
                return
            self.g.batch(['press B 5 12', 'wait_frames 40'])
        raise Lost('B did not reach the main menu')

    def cursor_to(self, item, menu):
        st, _, _ = self.g.state()
        cur = st.get('cursor')
        if cur is None or st.get('menu') != menu:
            raise Lost('not on list %d (menu %s)' % (menu, st.get('menu')))
        if cur == item:
            return True
        btn = 'DOWN' if item > cur else 'UP'
        ok, st, _, _, _ = self.g.batch(['press_until_state %s cursor %d %d 30 5' % (btn, item, abs(item - cur) + 4),
                                        'log_state cursor'])
        return st.get('cursor') == item

    def open_list(self, menu):
        hops = self.graph.path(menu)
        if hops is None:
            raise Lost('list %d has no path from the main menu' % menu)
        st, _, _ = self.g.state()
        if st.get('seq') == 75 and st.get('menu') == menu:
            return
        self.to_main()
        for lst, item in hops:
            sub = next(i for i in self.graph.menus[lst]['items'] if i['item'] == item)['submenu']
            if not self.cursor_to(item, lst):
                raise Lost('cursor could not reach item %d of list %d' % (item, lst))
            ok, st, _, _, _ = self.g.batch(['press_until_state A menu %d 4 60 5' % sub, 'wait_frames 10',
                                            'log_state menu'])
            if st.get('menu') != sub:
                raise Lost('A on %d/%d did not open list %d (menu %s)' % (lst, item, sub, st.get('menu')))

    # ---- one edge ----
    def walk_item(self, menu, it):
        e = {'list': menu, 'item': it['item'], 'expected': it['leads_to'], 'flags': it['flags']}
        try:
            self.open_list(menu)
        except Lost as x:
            e.update(verdict='lost', why=str(x))
            return e
        if menu not in self.labels:
            _, text, _ = self.g.state()
            self.labels[menu] = text
        if not self.cursor_to(it['item'], menu):
            e['verdict'] = 'unreachable'
            return e
        st, _, _ = self.g.state()
        e['target'] = st.get('target')
        shot = ['screenshot atlas_m%d_i%d' % (menu, it['item'])] if self.g.shots else []
        # A once (re-pressed only while nothing changed); 'unreadable' = the SmartMenu went away (a mode switch).
        ok, st, text, failed, _ = self.g.batch(['press_until_state A menu !=%d 2 90 5 unreadable' % menu,
                                                'wait_frames 45'] + ['log_state %s' % s for s in STATES] +
                                               ['log_text'] + shot)
        e.update(after={k: st.get(k) for k in STATES}, text=text)
        if failed:
            e.update(verdict='refused', why='A did not change the menu (2 presses)')
            return e
        reached_menu, seq = st.get('menu'), st.get('seq')
        if 'submenu' in it:
            e['verdict'] = 'ok' if reached_menu == it['submenu'] else 'mismatch'
        elif 'code' in it:
            want = it['code'] + it.get('match_type', 0) if it['code'] in (2000, 1001) else it['code']
            if seq == 75 and reached_menu == want:
                e['verdict'] = 'ok'
            elif seq != 75 or st.get('smart_menu') in (0, None):
                e['verdict'] = 'exit'  # left the main menu sequence (an exit action): recovered below
            else:
                e['verdict'] = 'mismatch'
        self.come_back(menu, it, e)
        return e

    def come_back(self, menu, it, e):
        for k in range(10):
            ok, st, _, _, _ = self.g.batch(['log_state seq', 'log_state menu', 'log_state cursor'])
            if st.get('seq') == 75 and st.get('menu') == menu:
                e['back'] = {'presses': k, 'cursor_kept': st.get('cursor') == it['item']}
                return
            if st.get('seq') == 75 and st.get('menu') == 1 and menu != 1 and k > 0:
                e['back'] = {'presses': k, 'landed': 'main menu'}
                return
            self.g.batch(['press B 5 12', 'wait_frames 60'])
        e['back'] = {'presses': 10, 'landed': 'lost'}
        self.g.say('  could not come back from %d/%d: restarting' % (menu, it['item']))
        self.g.restart()

    def crawl(self, menus, exits, only_exits=False):
        order = [m for m in sorted(self.graph.menus) if m >= 1 and (not menus or m in menus)
                 and self.graph.path(m) is not None]
        later = []
        for m in order:
            for it in self.graph.menus[m]['items']:
                if 'code' in it and it['code'] not in (2000, 1001) and not self.inside_75(it['code']):
                    later.append((m, it))
                    continue
                if not only_exits:
                    self.record(self.walk_item(m, it))
        if exits or only_exits:
            for m, it in later:
                self.record(self.walk_item(m, it))
        else:
            for m, it in later:
                self.edges.append({'list': m, 'item': it['item'], 'expected': it['leads_to'], 'verdict': 'skipped',
                                   'why': 'exit action (run with --exits)'})

    @staticmethod
    def inside_75(code):
        # reader (a): codes that stay inside the main menu sequence (option pages, rules, credits, manual, ...)
        return (1300 <= code <= 1322) or code in (1000, 1001, 1326, 1327, 1328, 1329, 1336, 1337, 1338) or \
            1309 <= code <= 1312

    def record(self, e):
        self.edges.append(e)
        self.g.say('%3d/%-2d %-14s -> %-9s %s%s' % (e['list'], e['item'], e['expected'], e['verdict'],
                                                    ' after=%s' % e.get('after') if e['verdict'] != 'ok' else '',
                                                    ' back=%s' % e.get('back') if e.get('back') else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--flow', default=os.path.join(REPO, 'out', 'harness', 'atlas', 'flow_5889.json'))
    ap.add_argument('--name', default='crawl')
    ap.add_argument('--start', action='store_true')
    ap.add_argument('--quit', action='store_true')
    ap.add_argument('--menus', default='', help='only these list menus, e.g. 1,2,7')
    ap.add_argument('--exits', action='store_true', help='also walk the items that leave the main menu sequence')
    ap.add_argument('--only-exits', action='store_true', help='walk only the items that leave the main menu sequence')
    ap.add_argument('--shots', action='store_true', help='a screenshot after every A')
    ap.add_argument('--data', default='', help='another frozen save copy under out\\bench (run_list -Data)')
    ap.add_argument('--extra', default=STOCK)
    a = ap.parse_args()
    graph = Graph(a.flow)
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M')
    out_dir = os.path.join(REPO, 'out', 'harness', 'atlas', 'crawl', stamp)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'crawl.log'), 'w', encoding='utf-8') as logf:
        game = Game(a.name, logf, a.extra, a.data, a.shots)
        if a.start:
            game.start()
        else:
            fields, _ = live.parse(live.request(a.name, '!status'))
            game.run = fields.get('run')
        c = Crawler(game, graph)
        menus = {int(x) for x in a.menus.split(',') if x.strip()}
        try:
            c.crawl(menus, a.exits, a.only_exits)
        finally:
            counts = collections.Counter(e['verdict'] for e in c.edges)
            result = {'run': game.run, 'made': stamp, 'flow': os.path.basename(a.flow), 'counts': counts,
                      'labels': {str(k): v for k, v in c.labels.items()}, 'edges': c.edges,
                      'note': 'Session 91 crawler v2 (tools/atlas/crawl2.py); local only'}
            with open(os.path.join(out_dir, 'edges.json'), 'w', encoding='utf-8') as f:
                json.dump(result, f, indent=1)
            game.say('done: %s -> %s' % (dict(counts), out_dir))
            if a.quit:
                try:
                    live.request(a.name, 'quit', connect_wait=2.0)
                except live.NoGame:
                    pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
