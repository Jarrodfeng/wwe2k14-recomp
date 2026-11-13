#!/usr/bin/env python3
"""S101 lane 118 (N2 / L-6): the offline acceptance test of the editor driver (tools\\n2\\caw_driver.py), gate lane n2_caw_text. 0 game runs.

  py -I tools\\n2\\caw_driver_selftest.py

The driver's own code runs against (a) SimGame, a model of the editor that draws what the reader reports (synthetic page names, no game text): press
latency, lost presses, overshooting tab presses, the blank / title-only / sliding frames of a page change, a flicker, leaving the editor; and (b) the
real frames of the kept CAW walks, replayed as the state stream (the reader's own output). Section by section:

 1 planner         from the kept OTHER page, `caw:page:CLOTHING/row:HEADWEAR` plans [LB, DOWN] with the expected title / dot / hot row on each
 2 refusals        a disabled row, an amber (ACCEPT) row, an unknown page / row, a malformed verb, an unsettled page: refused with NO press sent (sim and kept frames)
 3 arrival proof   a 3-frame flicker is not an arrival; two hot rows, a title mismatch, a short row set, no text, sequence 75: each fails the proof
 4 learn + go      the learn walk on the sim fills the table (rows, grey rows skipped by the cursor, amber rows never pressed onto); go reaches pages and rows
 5 failure paths   a lost press is pressed once more; two lost presses end with the FAIL report (expected vs seen, last 5 states, screenshot); an overshoot
                   is re-planned once; a second wrong page ends; leaving the editor ends; a slow (100 frame) press is not a lost one
 6 kept frames     the real stream: the arrival at the hot move of capture frame 233 is accepted only after the 4th frame; a stream that never leaves OTHER fails
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import caw_common as C
import caw_driver as D
import caw_read

QUIET = lambda *_: None


# ---- SimGame: the editor as the reader reports it ----------------------------------------------------------------------------------------------

PAGES = [
    dict(title='ALPHA', rows=['A ONE', 'A GREY', 'A THREE', 'A FOUR', 'A ACCEPT'], disabled={1}, amber={4}),
    dict(title='BRAVO', rows=['B ONE', 'B TWO', 'B THREE'], disabled=set(), amber=set()),
    dict(title='CHARLIE', rows=['C ONE', 'C TWO', 'C THREE', 'C FOUR'], disabled=set(), amber=set()),
    dict(title='DELTA', rows=['D GREY', 'D ONE', 'D TWO', 'D THREE', 'D FOUR', 'D FIVE'], disabled={0}, amber=set()),
]


class SimGame:
    def __init__(self, latency=10, drop=0, overshoot=0, start_tab=3, start_hot=None, blip=False, never_settle=False, leave_after_press=False, per_poll=8, late=0, sub=False):
        self.latency, self.drop, self.overshoot, self.blip, self.never_settle, self.leave = latency, drop, overshoot, blip, never_settle, leave_after_press
        self.tab = start_tab
        self.hot = start_hot if start_hot is not None else self._first(start_tab)
        self.t = 1000
        self.events = []           # (time, button)
        self.trans = None          # (start, kind)
        self.pending = []
        self.per_poll = per_poll
        self.presses = []
        self.blip_until = None
        self.blip_hot = None
        self.left = False
        self.shots = 0
        self.late = late          # frames between the last poll and the press batch really starting (the live channel waits for a busy script first)
        self.sub = sub            # the page showing is a sub-page whose tab dot is still the parent's
        self.closed = 0

    def _first(self, tab):
        p = PAGES[tab]
        return min(i for i in range(len(p['rows'])) if i not in p['disabled'])

    def _apply(self, button):
        if button in ('RB', 'LB'):
            step = 1 if button == 'RB' else -1
            if self.overshoot > 0:
                self.overshoot -= 1
                step *= 2
            self.tab = max(0, min(3, self.tab + step))
            self.hot = self._first(self.tab)
            self.trans = self.t
        elif button in ('DOWN', 'UP'):
            d = 1 if button == 'DOWN' else -1
            k = self.hot + d
            while 0 <= k < len(PAGES[self.tab]['rows']) and k in PAGES[self.tab]['disabled']:
                k += d
            if 0 <= k < len(PAGES[self.tab]['rows']):
                if self.blip:
                    self.blip = False
                    self.blip_until, self.blip_hot = self.t + 3, self.hot   # shows the new row for 3 frames, then falls back
                self.hot = k
        if self.leave:
            self.left = True

    def _frame(self):
        t = self.t
        due = [e for e in self.events if e[0] <= t]
        for e in due:
            self.events.remove(e)
            self._apply(e[1])
        p = PAGES[self.tab]
        if self.sub:
            p = dict(title='SUB PAGE', rows=['S ONE', 'S TWO'], disabled=set(), amber=set())
        hot = self.hot if not self.sub else 0
        if self.blip_until is not None:
            if t >= self.blip_until:
                hot, self.hot, self.blip_until = self.blip_hot, self.blip_hot, None
        s = dict(guest=t, has_text=True, seq=75 if self.left else 76, seq76=not self.left, title=p['title'], rows=list(p['rows']), complete=True, hot=hot, hot_n=1, hot_r=0.9,
                 tab=self.tab, dots=4, flash=False)
        s['row_r'] = [0.392 if i in p['disabled'] else 0.941 if i in p['amber'] else 1.0 for i in range(len(p['rows']))]
        s['row_g'] = [0.392 if i in p['disabled'] else 0.627 if i in p['amber'] else 1.0 for i in range(len(p['rows']))]
        s['row_b'] = [0.392 if i in p['disabled'] else 0.0 if i in p['amber'] else 1.0 for i in range(len(p['rows']))]
        if hot in p['amber']:
            s['hot'], s['hot_n'], s['hot_r'] = -1, 0, 0.0
        else:
            s['row_r'][hot], s['row_g'][hot], s['row_b'][hot] = 0.9, 0.04, 0.02
        if self.trans is not None:
            a = t - self.trans
            if a < 3:
                s.update(title='', rows=[], row_r=[], row_g=[], row_b=[], complete=False, hot=-1, hot_n=0, hot_r=0.0, tab=-1, dots=0)
            elif a < 8:
                s.update(rows=[], row_r=[], row_g=[], row_b=[], complete=False, hot=-1, hot_n=0, hot_r=0.0)
            elif a < 13:
                s.update(complete=False, hot=-1, hot_n=0, hot_r=0.0)
            else:
                self.trans = None
        if self.never_settle:
            s.update(complete=False, hot=-1, hot_n=0)
        self.t += 1
        return s

    def advance(self, n):
        for _ in range(n):
            self.pending.append(self._frame())

    # the Link interface
    def poll(self, since):
        if not self.pending:
            self.advance(self.per_poll)
        out, self.pending = self.pending[:64], self.pending[64:]
        return [s for s in out if s['guest'] > since]

    def press(self, button):
        self.presses.append(button)
        if self.late:
            self.advance(self.late)   # frames pass before the batch starts; they are in the ring, unchanged
        started = self.t
        if self.drop > 0:
            self.drop -= 1
        else:
            self.events.append((self.t + self.latency, button))
        self.advance(18)     # `press` holds 6 frames and releases for 12
        return started       # the answer's `frames: A..B`

    def close(self):
        self.closed += 1

    def screenshot(self, label):
        self.shots += 1
        return 'out/sim/%s.png' % label


class ReplayLink:
    """The real states of a kept walk as the stream; presses change nothing (the walk is fixed). poll() hands out `per_poll` frames at a time."""

    def __init__(self, states, start, per_poll=8, loop=None):
        self.states, self.i, self.per_poll, self.presses, self.loop = states, start, per_poll, [], loop
        self.base = 0

    def poll(self, since):
        out = []
        for _ in range(self.per_poll):
            if self.i >= len(self.states):
                if self.loop is None:
                    break
                self.i, self.base = 0, self.base + self.loop
            s = dict(self.states[self.i])
            s['guest'] = s['guest'] + self.base
            out.append(s)
            self.i += 1
        return [s for s in out if s['guest'] > since]

    def press(self, button):
        self.presses.append(button)
        return None

    def close(self):
        self.closed = getattr(self, 'closed', 0) + 1

    def screenshot(self, label):
        return 'out/replay/%s.png' % label


def new_driver(link, table, wall_s=0.5):
    return D.Driver(link, table, log=QUIET, wall_s=wall_s)


def sim_table():
    t = D.Table()
    for dot, p in enumerate(PAGES):
        s = SimGame(start_tab=dot)
        s.advance(1)
        e = D.page_entry(s.pending[0], dot)
        e['reach'] = [i for i in range(len(p['rows'])) if i not in p['disabled'] and i not in p['amber']]
        t.pages[p['title']] = e
    return t


def raises(fn):
    try:
        fn()
    except D.CawError as x:
        return str(x)
    return None


def main():
    main_dir = C.main_checkout()
    caps = {w: C.capture_dir(main_dir, w) for w in C.WALKS}
    kept = caw_read.read_all(caps[C.WALKS[0]])
    for s in kept:
        s['flash'] = False

    # the table of the kept pages, from the reader's own state of frames 0 (OTHER) and 117 (CLOTHING)
    kt = D.Table()
    kt.pages[kept[0]['title']] = D.page_entry(kept[0], 3)
    kt.pages[kept[117]['title']] = D.page_entry(kept[117], 2)
    other, clothing = kept[0]['title'], kept[117]['title']
    for t, p in kt.pages.items():
        p['reach'] = [i for i, r in enumerate(p['rows']) if r not in p['disabled'] and r not in p['unproven']]

    # 1 planner on the kept state
    steps = D.plan(kt, kept[38], D.parse_verb('caw:page:%s/row:%s' % (clothing, kt.pages[clothing]['rows'][1])))
    ok = [(s.button, s.tab, s.hot) for s in steps] == [('LB', 2, None), ('DOWN', None, 1)] and steps[0].title == clothing and steps[1].title == clothing
    C.report('1 planner from OTHER', ok, 'caw:page:CLOTHING/row:<2nd row> from tab 3 hot 2 = %s' % ' '.join('[%s %s]' % (s.button, s.expect_text().split(',')[0] if False else s.label) for s in steps))
    steps2 = D.plan(kt, kept[117], D.parse_verb('caw:row:#3'))
    steps3 = D.plan(kt, kept[117], D.parse_verb('caw:page:%s/row:#1/open' % clothing))
    ok = [s.button for s in steps2] == ['DOWN', 'DOWN', 'DOWN'] and [s.button for s in steps3] == ['DOWN', 'A'] and steps3[0].same_page
    C.report('1 planner rows and open', ok, 'row:#3 from row 0 = %s; row:#1/open = %s' % ([s.button for s in steps2], [s.button for s in steps3]))
    ok = [(a[0]) for a in D.parse_verb('caw:tab:2')] == ['tab'] and D.parse_verb('page:UPPER BODY/row:X Y/open') == [('page', 'UPPER BODY'), ('row', 'X Y'), ('open',)]
    bad = [v for v in ('caw:tab:4', 'caw:row:X/page:Y', 'caw:open', 'caw:page:', 'caw:back/open', 'caw:nope') if raises(lambda v=v: D.parse_verb(v)) is None]
    C.report('1 verbs parse', ok and not bad, 'tab / page / row / open / back; refused: %s' % ('none missed' if not bad else bad))

    # 2 refusals: no press is ever sent
    cases = [('disabled row (kept)', kept[38], kt, 'caw:page:%s/row:%s' % (other, kt.pages[other]['disabled'][0]), 'disabled'),
             ('amber row (kept)', kept[38], kt, 'caw:row:%s' % kt.pages[other]['unproven'][0], 'amber'),
             ('unknown page', kept[38], kt, 'caw:page:NO SUCH PAGE', 'not in the learned table'),
             ('unknown row', kept[38], kt, 'caw:row:NO SUCH ROW', 'has no row'),
             ('unsettled page', dict(kept[38], hot_n=2, hot=-2), kt, 'caw:row:#1', 'not settled'),
             ('no tab dot', dict(kept[38], tab=-1), kt, 'caw:tab:1', 'tab')]
    for name, st, tb, verb, word in cases:
        msg = raises(lambda: D.plan(tb, st, D.parse_verb(verb)))
        C.report('2 refuses %s' % name, bool(msg) and word in msg and msg.startswith('refused'), (msg or 'NOT REFUSED')[:150])
    sim = SimGame(start_tab=0)
    drv = new_driver(sim, sim_table())
    msg = raises(lambda: drv.run('caw:page:ALPHA/row:A GREY'))
    msg2 = raises(lambda: drv.run('caw:row:A ACCEPT'))
    C.report('2 live driver refuses before any press', bool(msg) and bool(msg2) and not sim.presses and 'disabled' in msg and 'amber' in msg2, 'presses sent: %d (grey row, then amber row)' % len(sim.presses))
    sim = SimGame(start_tab=1, never_settle=True)
    drv = new_driver(sim, sim_table())
    msg = raises(lambda: drv.run('caw:tab:2'))
    C.report('2 unsettled page: no press', bool(msg) and 'did not settle' in msg and not sim.presses, '%s; presses %d' % ((msg or 'NO FAIL').split('\n')[0][:110], len(sim.presses)))

    # 3 arrival proof: each bad state fails it
    st = D.Step('DOWN', 'x', 0, title=clothing, rows=kt.pages[clothing]['rows'], hot=1, same_page=True)
    good = dict(kept[240])
    base = dict(kept[117])
    twins = {'two hot rows': dict(good, hot_n=2, hot=-2), 'title mismatch': dict(good, title='OTHER'), 'short row set': dict(good, rows=good['rows'][:3], row_r=good['row_r'][:3]),
             'rows off the grid (sliding)': dict(good, complete=False), 'no text': dict(good, has_text=False), 'sequence 75': dict(good, seq=75, seq76=False),
             'wrong hot row': dict(good, hot=0)}
    leaked = [k for k, v in twins.items() if st.proof(v, base)]
    C.report('3 proof passes the real arrival', st.proof(good, base) and not st.proof(base, base), 'frame 240 (hot row 1) arrives; the state before (hot row 0) does not')
    C.report('3 proof fails every planted twin', not leaked, '%d twins (%s)%s' % (len(twins), ', '.join(twins), ('; LEAKED: %s' % leaked) if leaked else ''))

    # 4 learn walk and go on the sim
    sim = SimGame(start_tab=2)
    drv = new_driver(sim, D.Table())
    tbl = drv.learn()
    a, d = tbl.pages['ALPHA'], tbl.pages['DELTA']
    ok = (sorted(tbl.pages) == ['ALPHA', 'BRAVO', 'CHARLIE', 'DELTA'] and a['rows'] == PAGES[0]['rows'] and a['disabled'] == ['A GREY'] and a['unproven'] == ['A ACCEPT'] and a['reach'] == [0, 2, 3]
          and d['disabled'] == ['D GREY'] and d['reach'] == [1, 2, 3, 4, 5] and [tbl.pages[p['title']]['dot'] for p in PAGES] == [0, 1, 2, 3])
    C.report('4 learn walk', ok, 'pages %s; ALPHA reach %s, grey %s, amber %s (never pressed onto); DELTA reach %s; presses %d' % (sorted(tbl.pages), a['reach'], a['disabled'], a['unproven'], d['reach'], len(sim.presses)))
    for verb, tab, hot in (('caw:page:CHARLIE/row:C THREE', 2, 2), ('caw:tab:0', 0, 0), ('caw:page:DELTA/row:D FIVE', 3, 5), ('caw:page:ALPHA/row:A FOUR', 0, 3), ('caw:row:#2', 0, 2)):
        d0 = len(sim.presses)
        fin = drv.run(verb)
        C.report('4 go %s' % verb, fin['tab'] == tab and fin['hot'] == hot and settled_ok(fin), 'tab %d hot %d, %d presses' % (fin['tab'], fin['hot'], len(sim.presses) - d0))

    # 5 failure paths
    tb = sim_table()
    sim = SimGame(start_tab=3, drop=1)
    drv = new_driver(sim, tb)
    fin = drv.run('caw:page:CHARLIE/row:C TWO')
    C.report('5 lost press: pressed once more', fin['tab'] == 2 and fin['hot'] == 1 and sim.presses == ['LB', 'LB', 'DOWN'], 'presses %s' % sim.presses)
    sim = SimGame(start_tab=3, drop=2)
    drv = new_driver(sim, tb)
    msg = raises(lambda: drv.run('caw:page:CHARLIE'))
    need = ['FAIL step 1', 'expected:', 'saw:', 'last ', 'distinct states', 'screenshot: out/sim/caw_fail_']
    C.report('5 two lost presses: FAIL report', bool(msg) and all(x in msg for x in need) and sim.presses == ['LB', 'LB'] and sim.shots == 1, 'presses %s; report has %s\n%s' % (sim.presses, need, indent(msg)))
    sim = SimGame(start_tab=3, overshoot=1)
    drv = new_driver(sim, tb)
    fin = drv.run('caw:page:CHARLIE')
    C.report('5 overshoot: re-planned once', fin['tab'] == 2 and fin['title'] == 'CHARLIE' and sim.presses == ['LB', 'RB'], 'presses %s (the first LB moved two dots, the re-plan pressed RB)' % sim.presses)
    sim = SimGame(start_tab=3, overshoot=9)
    drv = new_driver(sim, tb)
    msg = raises(lambda: drv.run('caw:page:CHARLIE'))
    C.report('5 second wrong page: FAIL', bool(msg) and 'already re-planned once' in msg and len(sim.presses) == 2, 'presses %s\n%s' % (sim.presses, indent(msg)))
    sim = SimGame(start_tab=3, leave_after_press=True)
    drv = new_driver(sim, tb)
    msg = raises(lambda: drv.run('caw:page:CHARLIE'))
    C.report('5 left the editor: FAIL', bool(msg) and 'left the editor' in msg and len(sim.presses) == 1, (msg or 'NO FAIL').split('\n')[0][:120])
    sim = SimGame(start_tab=3, latency=100)
    drv = new_driver(sim, tb)
    fin = drv.run('caw:page:CHARLIE')
    C.report('5 slow press (100 frames) is not lost', fin['tab'] == 2 and sim.presses == ['LB'], 'presses %s' % sim.presses)
    sim = SimGame(start_tab=2, start_hot=0, blip=True)
    drv = new_driver(sim, tb)
    fin = drv.run('caw:row:C TWO')
    C.report('5 a 3-frame flicker is not an arrival', fin['hot'] == 1 and sim.presses == ['DOWN', 'DOWN'], 'presses %s (the first DOWN showed the row for 3 frames, then fell back; the second held)' % sim.presses)
    sim = SimGame(start_tab=2, start_hot=0)
    drv = new_driver(sim, tb)
    fin = drv.run('caw:page:CHARLIE/row:C ONE')
    C.report('5 already there: no press', not sim.presses, 'presses %s' % sim.presses)

    # 6 the kept frames as the stream
    link = ReplayLink(kept, 38)
    drv = new_driver(link, kt)
    fin = drv.run('caw:page:%s/row:%s' % (clothing, kt.pages[clothing]['rows'][1]))
    arrived = [s['guest'] for s in kept if s['guest'] == fin['guest']]
    ok = fin['title'] == clothing and fin['hot'] == 1 and fin['tab'] == 2 and drv.last_arrival['guest'] == kept[236]['guest'] and link.presses == ['LB', 'DOWN']
    C.report('6 kept frames: arrival only after 4 frames', ok, 'presses %s; the proof held at guest %d = capture frame 236 (the hot move is frame 233, so frames 233-236)' % (link.presses, drv.last_arrival['guest']))
    link = ReplayLink([dict(kept[0])], 0, loop=1)
    link.states = [dict(kept[0], guest=kept[0]['guest'] + i) for i in range(1200)]
    drv = new_driver(link, kt)
    msg = raises(lambda: drv.run('caw:page:%s' % clothing))
    C.report('6 a stream that never leaves OTHER: FAIL', bool(msg) and 'FAIL step 1' in msg and 'changed nothing, twice' in msg and link.presses == ['LB', 'LB'], 'presses %s\n%s' % (link.presses, indent(msg)))
    review_plants(kept, kt, caps, main_dir)

    # 7 the goto address and resolver (tools/goto/address.py + resolve.py)
    sys.path.insert(0, os.path.join(C.REPO, 'tools', 'goto'))
    import address
    import resolve
    good = ['caw:tab:2', 'caw:page:CLOTHING', 'caw:page:UPPER BODY/row:X Y/open', 'caw:row:#3', 'caw:row:A B/open', 'caw:back', 'caw:head/hair']
    bad = ['caw:tab:7', 'caw:tab:', 'caw:page:', 'caw:row:A/page:B', 'caw:open', 'caw:head', 'caw:a/b/c', 'caw:page:A;rm']
    got_bad = [a for a in good if _parse_err(address, a)]
    miss = [a for a in bad if not _parse_err(address, a)]
    C.report('7 address parses the caw forms', not got_bad and not miss, '%d forms parse, %d malformed refused%s' % (len(good), len(bad), (' (WRONG: %s %s)' % (got_bad, miss)) if (got_bad or miss) else ''))
    import tempfile
    tdir = tempfile.mkdtemp(prefix='caw_l118_')
    tpath = os.path.join(tdir, 'caw_pages.json')
    saved = D.table_path
    try:
        D.table_path = lambda root=None: tpath
        p0 = resolve.resolve('caw:page:ALPHA/row:A FOUR')
        sim_table().save(tpath)
        pl = resolve.resolve('caw:page:ALPHA/row:A FOUR')
        pe = resolve.resolve('caw:alpha/zz')
        pg = resolve.resolve('caw:page:ALPHA/row:A GREY')
        pa = resolve.resolve('caw:page:ALPHA/row:A ACCEPT')
        pu = resolve.resolve('caw:page:NOWHERE')
        pr = resolve.resolve('caw:page:BRAVO/row:NO SUCH')
        pt = resolve.resolve('caw:tab:2')
        pb = resolve.resolve('caw:page:A/row:')
    finally:
        D.table_path = saved
    ok = (p0.status == 'not_reachable' and 'learn' in p0.reason and pl.status == 'plan' and pl.steps == [{'op': 'caw', 'verb': 'page:ALPHA/row:A FOUR'}] and pe.status == 'error' and 'has no row' in pe.reason
          and pg.status == 'error' and 'disabled' in pg.reason and pa.status == 'error' and 'amber' in pa.reason and pu.status == 'error' and pr.status == 'error'
          and pt.status == 'plan' and pb.status == 'error')
    C.report('7 resolver: plan / not reachable / error', ok, 'no table: %s; table: %s; legacy CATEGORY/ITEM form (mapped to page/row): %s; grey row: %s; amber row: %s; unknown page: %s; unknown row: %s; tab: %s; malformed: %s'
             % (p0.status, pl.status, pe.status, pg.status, pa.status, pu.status, pr.status, pt.status, pb.status))
    C.report('7 plan prints', any('drive the Create a Superstar editor' in ln for ln in pl.lines()), pl.lines()[1][:120])
    ok = all(C.results)
    print('\n%s: %d checks' % ('PASS' if ok else 'FAIL', len(C.results)))
    return 0 if ok else 1


class JsonOnly:
    """Wraps a link and hands the driver only the keys the game's Json() writes (src/caw_text.h), through a real json round trip."""

    def __init__(self, inner, keys):
        self.inner, self.keys = inner, keys

    def poll(self, since):
        import json
        return [json.loads(json.dumps({k: v for k, v in s.items() if k in self.keys})) for s in self.inner.poll(since)]

    def __getattr__(self, name):
        return getattr(self.inner, name)


def review_plants(kept, kt, caps, main_dir):
    import json
    import subprocess
    # R1 the live JSON feeds the driver: the C++ Json() lines of the kept walk (exactly what `!caw since` answers) through page_entry, learn-style data and a run
    lines = [json.loads(x) for x in C.cpp_json_lines(caps[C.WALKS[0]])]
    keys = set(lines[0])
    need = {'row_r', 'row_g', 'row_b'}
    try:
        e0 = D.page_entry(lines[0], 3)
        e1 = D.page_entry(lines[117], 2)
        ok = bool(need <= keys) and len(e0['disabled']) == 1 and len(e0['unproven']) == 1 and e1['disabled'] == [] and e1['unproven'] == []
        msg = 'keys %s; OTHER page: %d grey, %d amber; CLOTHING: %d grey, %d amber' % (sorted(need & keys), len(e0['disabled']), len(e0['unproven']), len(e1['disabled']), len(e1['unproven']))
    except (KeyError, IndexError) as x:
        ok, msg = False, 'page_entry on the game\'s JSON raised %s %s' % (type(x).__name__, x)
    C.report('R1 page_entry on the game\'s own JSON (the first live learn raised KeyError)', ok, msg)
    sim = SimGame(start_tab=2)
    drv = new_driver(JsonOnly(sim, keys), D.Table())
    try:
        tbl = drv.learn()
        ok = sorted(tbl.pages) == ['ALPHA', 'BRAVO', 'CHARLIE', 'DELTA'] and tbl.pages['ALPHA']['disabled'] == ['A GREY'] and tbl.pages['ALPHA']['unproven'] == ['A ACCEPT']
        msg = 'learn through JSON-only states: %s' % sorted(tbl.pages)
    except Exception as x:
        ok, msg = False, 'learn on JSON-only states raised %s %s' % (type(x).__name__, x)
    C.report('R1 learn on JSON-only states', ok, msg)
    sts = [dict(x, flash=False) for x in lines]
    link = ReplayLink(sts, 38)
    drv = new_driver(link, kt)
    fin = drv.run('caw:page:%s/row:%s' % (sts[117]['title'], kt.pages[sts[117]['title']]['rows'][1]))
    C.report('R1 a run on the game\'s JSON stream arrives', fin['hot'] == 1 and drv.last_arrival['guest'] == sts[236]['guest'], 'arrival at guest %d' % drv.last_arrival['guest'])
    # R2 a press that starts 200 frames after the last poll is one press, not two
    tb = sim_table()
    sim = SimGame(start_tab=3, late=200)
    drv = new_driver(sim, tb)
    fin = drv.run('caw:page:CHARLIE')
    C.report('R2 a press landing 200 frames late: exactly one press', sim.presses == ['LB'] and fin['tab'] == 2, 'presses %s' % sim.presses)
    # R3 A and B are never re-pressed
    sim = SimGame(start_tab=0)
    drv = new_driver(sim, tb)
    msg = D.CawError and None
    try:
        drv.run('caw:page:ALPHA/row:A ONE/open')
        msg = None
    except D.CawError as x:
        msg = str(x)
    C.report('R3 an A with no change: one press, then FAIL', bool(msg) and sim.presses == ['A'] and 'never pressed twice' in msg, 'presses %s' % sim.presses)
    # R4 `!caw off` is always sent
    sim = SimGame(start_tab=3)
    D.run_verb(sim, tb, 'caw:page:CHARLIE', log=QUIET)
    sim2 = SimGame(start_tab=3, drop=2)
    msg = raises(lambda: D.run_verb(sim2, tb, 'caw:page:CHARLIE', log=QUIET))
    sim3 = SimGame(start_tab=3)
    msg3 = raises(lambda: D.run_verb(sim3, tb, 'caw:page:NOPE', log=QUIET))
    C.report('R4 off sent after success, FAIL and a refusal', sim.closed == 1 and sim2.closed == 1 and sim3.closed == 1 and bool(msg) and bool(msg3), 'closes: %d / %d / %d' % (sim.closed, sim2.closed, sim3.closed))
    # R5 the right tab dot on a sub-page is not 'done'; and the end of a run is asserted
    sim = SimGame(start_tab=0, sub=True)
    drv = new_driver(sim, tb)
    msg = raises(lambda: drv.run('caw:page:ALPHA'))
    C.report('R5 sub-page with the right tab dot: refused, no press', bool(msg) and 'sub-page' in msg and not sim.presses, (msg or 'REPORTED DONE')[:140])
    wrong = dict(kept[240], title='NOT IT', tab=1, hot=3)
    bad = D.final_check(kt, D.parse_verb('caw:page:%s/row:%s' % (kept[117]['title'], kt.pages[kept[117]['title']]['rows'][1])), wrong)
    good = D.final_check(kt, D.parse_verb('caw:page:%s/row:%s' % (kept[117]['title'], kt.pages[kept[117]['title']]['rows'][1])), kept[240])
    C.report('R5 final check', 'title' in bad and 'tab dot' in bad and 'hot row' in bad and good == '', 'wrong state: %s | right state: %r' % (bad, good))
    # R6 a wrong page while going to a ROW re-plans from the page action
    sim = SimGame(start_tab=3, overshoot=1)
    drv = new_driver(sim, tb)
    fin = drv.run('caw:page:CHARLIE/row:C TWO')
    C.report('R6 re-plan from the page action', sim.presses == ['LB', 'RB', 'DOWN'] and fin['title'] == 'CHARLIE' and fin['hot'] == 1, 'presses %s' % sim.presses)
    # R11 a dead channel is a CawError, never a traceback
    class Dead:
        class NoGame(Exception):
            pass

        def request(self, *a, **k):
            raise Dead.NoGame('no game answers')

        def parse(self, a):
            return {}, []

        def run_batch(self, *a, **k):
            raise Dead.NoGame('no game answers')

    ll = D.LiveLink('x', live_module=Dead())
    m1, m2 = raises(lambda: ll.poll(0)), raises(lambda: ll.press('A'))
    ll.close()
    C.report('R11 lost game: CawError, no traceback', bool(m1) and bool(m2) and 'lost the game' in m1, (m1 or '')[:100])
    # R15 the table: stamped, replaced by learn, refused when the rows differ
    import tempfile
    tdir = tempfile.mkdtemp(prefix='caw_l118_t_')
    tp = os.path.join(tdir, 't.json')
    old = D.Table({'ZZZ': dict(dot=0, rows=['X'], disabled=[], unproven=[], reach=[0])})
    sim = SimGame(start_tab=2)
    drv = new_driver(sim, old)
    t2 = drv.learn()
    t2.save(tp, build='b1', language='en')
    t3 = D.Table.load(tp)
    ok1 = 'ZZZ' not in t3.pages and sorted(t3.pages) == ['ALPHA', 'BRAVO', 'CHARLIE', 'DELTA'] and t3.stamp.get('build') == 'b1' and t3.stamp.get('language') == 'en' and t3.hash_ok()
    t3.pages['BRAVO']['rows'][0] = 'edited'
    ok2 = not t3.hash_ok()
    sim = SimGame(start_tab=1)
    tb2 = sim_table()
    tb2.pages['BRAVO']['rows'] = ['B ONE', 'B TWO', 'B CHANGED']
    drv = new_driver(sim, tb2)
    msg = raises(lambda: drv.run('caw:page:BRAVO/row:B TWO'))
    C.report('R15 table stamp, learn replaces, a changed row set is refused', ok1 and ok2 and bool(msg) and 're-run' in msg and not sim.presses, 'stamp %s; hash edit caught %s; refusal: %s' % (t3.stamp.get('rows_hash'), ok2, (msg or '')[:90]))
    # R12 the goto selftest ignores the real table: with a table holding HEAD / HAIR in the environment it still passes
    hp = os.path.join(tdir, 'head.json')
    D.Table({'HEAD': dict(dot=0, rows=['HAIR'], disabled=[], unproven=[], reach=[0])}).save(hp)
    env = dict(os.environ, WWE2K14_CAW_TABLE=hp)
    sys.path.insert(0, os.path.join(C.REPO, 'tools', 'goto'))
    import resolve as _res
    saved = os.environ.get('WWE2K14_CAW_TABLE')
    os.environ['WWE2K14_CAW_TABLE'] = hp
    ctl = _res.resolve('caw:head/hair').status
    if saved is None:
        del os.environ['WWE2K14_CAW_TABLE']
    else:
        os.environ['WWE2K14_CAW_TABLE'] = saved
    r = subprocess.run([sys.executable, '-I', os.path.join(C.REPO, 'tools', 'goto', 'selftest.py')], capture_output=True, text=True, env=env, timeout=600)
    C.report('R12 goto selftest unaffected by a table with HEAD / HAIR', ctl == 'plan' and r.returncode == 0 and 'all passed' in r.stdout, 'control: that table alone makes caw:head/hair a %s; goto selftest exit %d' % (ctl, r.returncode))


def _parse_err(address, text):
    try:
        address.parse(text)
    except address.AddressError:
        return True
    return False


def settled_ok(s):
    return D.settled(s)


def indent(msg):
    return '\n'.join('      | ' + ln for ln in (msg or 'NO FAIL').split('\n'))


if __name__ == '__main__':
    sys.exit(main())
