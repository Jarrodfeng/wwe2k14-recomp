"""Session 91 (the game atlas, step 3): the navigator - where am I, and take me to any screen.

  py tools/atlas/nav.py where [--name main]
  py tools/atlas/nav.py go TARGET [--name main]        TARGET: a node id (list:7, setup:0, rules:49, mode:caw, ...)
                                                        or words of its path name ("one on one > normal", "options",
                                                        "create a superstar"); ambiguous words list the choices
  py tools/atlas/nav.py names [WORDS]                   the named nodes (path names from the main menu)
MCP: game_where / game_nav (tools/live/mcp_server.py) call where() / go().

Works on a live game (tools/live/live.py: the live channel), with the atlas `docs/systems/game_atlas.json`
(tools/atlas/build_atlas.py). where() reads the states (sequence, SmartMenu menu / cursor / target, screen controller
mode) and matches them against the nodes' recognisers. go() plans the cheapest path over verified (then read) edges and
walks it one edge at a time: a list item = the cursor moved to it (each move confirmed by the cursor word) and A
pressed until the next node's recogniser is met; a back edge = B pressed until the target node is up. Before a list
press it waits (up to 2 s, optional) for the SmartMenu focus gate `0x836F3AA4` = 0 (reader g's review: the gate the
SmartMenu holds during its fade). After every edge it reads where it is again: an unexpected node re-plans from there
(up to 4 times); an unknown screen is recovered with B presses until a known node shows. Every press waits for its
state (`press_until_state`): fast-forward safe.
"""
import argparse
import heapq
import json
import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))
import live  # noqa: E402

ATLAS = os.path.join(REPO, 'docs', 'systems', 'game_atlas.json')
STATE = re.compile(r'line \d+(?: \([^)]*\))?: state (\w+) = (?:0x[0-9A-F]+ \((-?\d+)\)|(unreadable))')
TEXT = re.compile(r'line \d+(?: \([^)]*\))?: screen text: (.*) \(frame \d+\)$')
FAILED = re.compile(r'script FAILED at line \d+: (.*)$')
READ = ('seq', 'menu', 'cursor', 'target', 'screen_mode')
FOCUS_GATE = 'wait_state 0x836F3AA4 0 120 2 optional'


class Lost(Exception):
    pass


class Atlas:
    def __init__(self, path=ATLAS):
        with open(path, encoding='utf-8') as f:
            a = json.load(f)
        self.nodes, self.edges = a['nodes'], a['edges']
        self.out = {}
        for e in self.edges:
            if e['status'] in ('verified', 'read'):
                self.out.setdefault(e['from'], []).append(e)
        self.names = self._names()

    def _names(self):
        """Path names from the main menu ("PLAY > ONE ON ONE > NORMAL") by the list labels."""
        names, todo = {'list:1': 'MAIN MENU'}, ['list:1']
        while todo:
            nid = todo.pop(0)
            labels = self.nodes.get(nid, {}).get('labels') or []
            for e in self.out.get(nid, []):
                i = e['how'].get('item')
                if i is None or e['to'] in names:
                    continue
                lab = labels[i] if i < len(labels) else 'item %d' % i
                names[e['to']] = lab if nid == 'list:1' else '%s > %s' % (names[nid], lab)
                todo.append(e['to'])
        for nid, n in self.nodes.items():
            if n.get('label'):
                names.setdefault(nid, n['label'])
        return names

    def match(self, st):
        """The node whose recogniser fits the states best (most fields), or None."""
        best, score = None, -1
        for nid, n in self.nodes.items():
            r = n.get('recognise') or {}
            if not r or any(st.get(k) != v for k, v in r.items()):
                continue
            if len(r) > score:
                best, score = nid, len(r)
        return best

    def resolve(self, target):
        if target in self.nodes:
            return [target]
        words = [w.strip().lower() for w in target.split('>')]
        hits = []
        for nid, name in self.names.items():
            parts = (name + ' | ' + (self.nodes.get(nid, {}).get('label') or '')).lower()
            if all(w in parts for w in words):
                hits.append(nid)
        exact = [h for h in hits if self.names[h].lower().split(' > ')[-1] == words[-1]
                 or (self.nodes.get(h, {}).get('label') or '').lower() == words[-1]]
        if len(exact) == 1:
            return exact
        hits = exact or hits
        # the same words under PLAY and under OPTIONS > MATCH RULES: the shorter path (the PLAY one) wins
        depth = lambda h: self.names[h].count(' > ')
        if len(hits) > 1 and len([h for h in hits if depth(h) == min(map(depth, hits))]) == 1:
            return [min(hits, key=depth)]
        return hits

    def plan(self, src, dst):
        """Cheapest edge list src -> dst (verified edges cost 1 + presses, read edges +5)."""
        dist, prev, q = {src: 0}, {}, [(0, src)]
        while q:
            d, nid = heapq.heappop(q)
            if nid == dst:
                break
            if d > dist.get(nid, 1e9):
                continue
            for e in self.out.get(nid, []):
                c = d + 1 + (e['how'].get('times') or 0) + (5 if e['status'] == 'read' else 0)
                if c < dist.get(e['to'], 1e9):
                    dist[e['to']], prev[e['to']] = c, e
                    heapq.heappush(q, (c, e['to']))
        if dst not in prev and src != dst:
            return None
        path, nid = [], dst
        while nid != src:
            path.append(prev[nid])
            nid = prev[nid]['from']
        return list(reversed(path))


class Nav:
    # Opus review (out/harness/atlas/readers/nav_review.md) fixes: a batch that is not ok / failed is an error (never
    # read as "unknown screen"); A is pressed only until the SOURCE list is gone, then the target is waited for (no
    # extra presses into whatever opened); B goes one hop at a time; rows are checked by their target word (SM+368)
    # before A, so our GRAPHICS row (OPTIONS 3) or a hidden row cannot shift a press; screens whose way back is not
    # known (dead_end) are refused as destinations; recover() presses B only inside the menus / modes.
    MENU_SEQS = {75}

    def __init__(self, name='main', atlas=None, say=None):
        self.name, self.atlas, self.say = name, atlas or Atlas(), say or (lambda *a: None)
        self.MENU_SEQS = {75} | {n['recognise']['seq'] for n in self.atlas.nodes.values()
                                 if n.get('kind') == 'mode' and (n.get('recognise') or {}).get('seq')}

    def batch(self, lines, timeout=300):
        fields, log, _ = live.run_batch(self.name, lines, timeout)
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
                failed = m.group(1)
        status = fields.get('status')
        if status not in ('ok', 'failed'):
            raise Lost('live channel answered %s (%s)' % (status, fields.get('error', '')))
        return st, text, failed

    def read(self):
        st, text, _ = self.batch(['log_state %s' % s for s in READ] + ['log_text'])
        return st, text

    FOCUS = re.compile(r'\[wwe2k14\]\[focus\] frame (\d+): (\d+) widgets took the input(.*)$')
    ENTRY = re.compile(r'm=([0-9A-F]+) vt=([0-9A-F]+) row=(-?\d+)/(-?\d+) col=(-?\d+)/(-?\d+) moved=(\d+) '
                       r'pads=([0-9A-F]+) f=\d+(?: list=[0-9A-F]+ item=(-?\d+)/(-?\d+))?')

    def focus(self):
        """The input widgets that took the input in the last two frames (src/focus_widget.h), the focused one first:
        it listens to pad 1 (+8 bit 0) and keeps its own cursor (row or column count > 0)."""
        fields, log, _ = live.run_batch(self.name, ['cvar wwe2k14_focus_dump %d' % (int(time.time() * 1000) % 10**9),
                                                    'wait_frames 3'], 60)
        widgets = []
        for ln in log:
            m = self.FOCUS.search(ln.rstrip())
            if m:
                widgets = [dict(m=g[0], vt=g[1], row=int(g[2]), rows=int(g[3]), col=int(g[4]), cols=int(g[5]),
                                moved=int(g[6]), pads=int(g[7], 16)) for g in self.ENTRY.findall(m.group(3))]
                for w, g in zip(widgets, self.ENTRY.findall(m.group(3))):
                    if g[8]:  # a UI list's mixin: the list keeps the cursor ([list+88] / [list+612])
                        w['rows'], w['row'], w['cols'], w['col'] = int(g[9]), int(g[8]), 1, 0
        own = [w for w in widgets if (w['rows'] > 0 or w['cols'] > 0)]
        mine = [w for w in own if w['pads'] & 1] or own
        return mine + [w for w in widgets if w not in mine]

    def where(self):
        st, text = self.read()
        nid = self.atlas.match(st)
        out = {'node': nid, 'name': self.atlas.names.get(nid), 'states': st, 'text': text}
        n = self.atlas.nodes.get(nid) or {}
        if n.get('kind') == 'list' and st.get('cursor') is not None and st['cursor'] >= 0:
            cur, tgt = st['cursor'], st.get('target')
            # the row is named by its target word where it is unique in this list, else by the stock index
            rows = [e for e in self.atlas.out.get(nid, []) if 'item' in e['how']]
            by_t = [e for e in rows if e.get('target') == tgt]
            e = by_t[0] if len(by_t) == 1 else next((r for r in rows if r['how']['item'] == cur), None)
            labels = n.get('labels') or []
            out['item'] = cur
            out['of'] = n.get('items')
            if e:
                i = e['how']['item']
                out['highlighted'] = labels[i] if i < len(labels) else None
                out['leads_to'] = e['to']
            elif tgt == 1300 and nid == 'list:4':
                out['highlighted'], out['leads_to'] = 'GRAPHICS (ours)', None
        else:
            # any other screen: the focused input widget's own cursor (Session 91 step 6)
            try:
                ws = self.focus()
            except live.NoGame:
                ws = []
            if ws:
                w = ws[0]
                out['widget'] = w
                if w['rows'] > 0 or w['cols'] > 0:
                    out['grid'] = 'row %d of %d, column %d of %d' % (w['row'] + 1, w['rows'], w['col'] + 1, w['cols'])
        return out

    @staticmethod
    def describe_widget(w):
        if 'grid' in w:
            return ', %s (widget vtable %s)' % (w['grid'], w['widget']['vt'])
        if 'widget' in w:
            return ', input widget vtable %s (keeps no cursor of its own)' % w['widget']['vt']
        return ''

    @staticmethod
    def describe(w):
        s = 'at %s (%s)' % (w['node'], w['name']) if w['node'] else 'at an unknown screen (states %s)' % w['states']
        if 'item' in w:
            s += ', item %d of %s: %s -> %s' % (w['item'], w['of'], w.get('highlighted'), w.get('leads_to'))
        return s + Nav.describe_widget(w)

    def wait_node(self, nid, frames):
        """wait_state on every field of nid's recogniser (screen_mode is set some frames after the menu id)."""
        r = self.atlas.nodes[nid].get('recognise') or {}
        lines = ['wait_state %s %d %d 2' % (k, v, frames) for k, v in r.items()]
        if nid.startswith('setup:'):  # the select screen draws a while after its menu id (run 5907): stage 3 = up
            lines.append('wait_state setup_stage 3 600 2 optional')  # static (reader b), so optional
        _, _, failed = self.batch(lines, timeout=max(60, frames // 30 + 60))
        if failed:
            raise Lost('%s did not come up (%s)' % (nid, failed))

    def row_to(self, e):
        """Cursor onto e's row, checked by the row's target word: the stock index first, then its neighbours (our
        GRAPHICS row moves OPTIONS 3.. down one; a hidden row moves the rest up)."""
        frm, item, want = e['from'], e['how']['item'], e.get('target')
        menu = self.atlas.nodes[frm]['recognise']['menu']
        for idx in (item, item + 1, item - 1, item + 2):
            if idx < 0:
                continue
            st, _ = self.read()
            if st.get('menu') != menu or st.get('cursor') is None:
                raise Lost('not on %s any more (menu %s)' % (frm, st.get('menu')))
            cur = st['cursor']
            if cur != idx:
                btn = 'DOWN' if idx > cur else 'UP'
                st, _, failed = self.batch([FOCUS_GATE, 'press_until_state %s cursor %d %d 30 5' %
                                            (btn, idx, abs(idx - cur) + 4), 'log_state target'])
                if failed:
                    continue
            else:
                st, _, _ = self.batch(['log_state target'])
            if want is None or st.get('target') == want:
                return idx
        raise Lost('no row of %s has target %s (item %d)' % (frm, want, item))

    def answer_box(self, leaving_options=False):
        """A message box waiting for an answer ([0x839294BC]+16 = 2; docs/systems/menus.md "The game's message box")
        ignores B: answer it with A (the default button; e.g. "keep these settings?" after a MATCH RULES value
        changed). True when one was answered."""
        st, text, _ = self.batch(['log_state msgbox', 'log_text'])
        if st.get('msgbox') != 2:
            return False
        # Run 5906 (screenshot): leaving a changed option page asks "Are you sure you don't want to keep these
        # settings?" with NO preselected; NO stays on the page, YES leaves and drops the change: LEFT to YES, then A.
        # Only that question gets YES; any other box (QUIT TO DESKTOP, save prompts) gets its default button. The
        # question's text is not in the screen text (run 5907), so it is known by where it comes up: B on an option
        # page (screen mode 23) or a MATCH RULES page (mode 6), the screens with the keep-settings leave check.
        lines = ['press_until_state A msgbox !=2 3 60 5 unreadable', 'wait_frames 30']
        if leaving_options or (text and "want to keep these settings" in text):
            lines.insert(0, 'press_until_state LEFT msgbox_button 0 3 30 5 if_needed')
        self.batch(lines)
        return True

    def step(self, e):
        how = e['how']
        if 'press' in how:
            self.answer_box()
        frm, to = e['from'], e['to']
        slow = to.startswith('mode:')
        if 'item' in how:
            self.row_to(e)
            menu = self.atlas.nodes[frm]['recognise']['menu']
            # A until this list is gone (or the SmartMenu: a mode switch), never more: then wait for the target
            _, _, failed = self.batch([FOCUS_GATE, 'press_until_state A menu !=%d 3 90 5 unreadable' % menu])
            if failed:
                raise Lost('A on %s item %d was refused (%s)' % (frm, how['item'], failed))
            self.wait_node(to, 1800 if slow else 300)
        elif self.atlas.nodes.get(frm, {}).get('kind') == 'mode':
            # a mode: B until the main menu sequence is back (each B goes up one of the mode's own screens)
            _, _, failed = self.batch(['press_until_state B seq 75 %d 150 5' % ((how.get('times') or 1) + 3)],
                                      timeout=600)
            if failed:
                raise Lost('B did not leave %s (%s)' % (frm, failed))
            self.wait_node(to, 600)
        else:
            # one B, one hop: until the screen we were on is gone, then the parent must show. A B that opened a
            # message box (MATCH RULES after a value changed: "keep these settings?") is answered with A (its
            # default, NO) and B goes on (run 5904: the box ignores the next Bs).
            r = self.atlas.nodes[frm].get('recognise') or {}
            failed = None
            for _ in range(3):
                _, _, failed = self.batch(['press_until_state B menu !=%d 1 90 5 unreadable' % r.get('menu', -1)])
                if not failed:
                    break
                mode_now, _ = self.read()
                self.answer_box(leaving_options=mode_now.get('screen_mode') in (6, 23))
                st, _ = self.read()
                if st.get('menu') != r.get('menu'):
                    failed = None
                    break
            if failed:
                raise Lost('B did not leave %s (%s)' % (frm, failed))
            self.wait_node(to, 300)

    def recover(self):
        """B until a known node is up - only inside the menus and creation modes (never in a match or the title)."""
        for _ in range(12):
            w = self.where()
            if w['node']:
                return w
            if w['states'].get('seq') not in self.MENU_SEQS:
                raise Lost('unknown screen outside the menus (sequence %s): not pressing blind' % w['states'].get('seq'))
            if not self.answer_box():
                self.batch(['press B 5 12', 'wait_frames 90'])
        raise Lost('no known screen after 12 B presses')

    def go(self, target):
        hits = self.atlas.resolve(target)
        if len(hits) != 1:
            raise Lost('target "%s": %s' % (target, ', '.join('%s (%s)' % (h, self.atlas.names.get(h))
                                                             for h in hits[:15]) or 'no match'))
        dst = hits[0]
        if self.atlas.nodes.get(dst, {}).get('dead_end') or self.atlas.plan(dst, 'list:1') is None:
            raise Lost('%s (%s) has no known way back: refused as a destination' % (dst, self.atlas.names.get(dst)))
        log = []
        for attempt in range(5):
            w = self.where()
            if not w['node']:
                w = self.recover()
                log.append('recovered at %s' % w['node'])
            if w['node'] == dst:
                log.append('arrived: %s' % self.describe(w))
                return log
            path = self.atlas.plan(w['node'], dst)
            if path is None:
                w = self.recover() if w['states'].get('seq') in self.MENU_SEQS else w
                path = self.atlas.plan(w['node'], dst) if w['node'] else None
                if path is None:
                    raise Lost('no path from %s to %s' % (w['node'], dst))
            log.append('plan from %s: %s' % (w['node'], ' -> '.join(e['to'] for e in path)))
            try:
                for e in path:
                    self.step(e)
                    now = self.where()
                    if now['node'] != e['to']:
                        log.append('expected %s, at %s: re-planning' % (e['to'], now['node']))
                        break
                else:
                    log.append('arrived: %s' % self.describe(self.where()))
                    return log
            except Lost as x:
                log.append('step failed: %s' % x)
        raise Lost('gave up after 5 plans: ' + ' | '.join(log))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['where', 'go', 'names'])
    ap.add_argument('target', nargs='?', default='')
    ap.add_argument('--name', default='main')
    a = ap.parse_args()
    atlas = Atlas()
    if a.cmd == 'names':
        for nid, name in sorted(atlas.names.items(), key=lambda kv: kv[1]):
            if not a.target or a.target.lower() in name.lower():
                print('%-12s %s' % (nid, name))
        return 0
    nav = Nav(a.name, atlas)
    if a.cmd == 'where':
        print(Nav.describe(nav.where()))
        return 0
    try:
        for line in nav.go(a.target):
            print(line)
    except Lost as x:
        print('LOST: %s' % x)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
