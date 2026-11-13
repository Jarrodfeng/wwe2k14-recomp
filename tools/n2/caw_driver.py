"""S101 lane 118 (N2 / L-6): the Create a Superstar editor driver. One press per step, every step proved by what the frame draws.

    py -I tools/n2/caw_driver.py [--name main] learn
    py -I tools/n2/caw_driver.py [--name main] go ADDRESS           caw:tab:N | caw:page:TITLE | caw:page:TITLE/row:NAME | caw:row:NAME | ...[/open] | caw:back
    py -I tools/n2/caw_driver.py [--name main] state
    py -I tools/n2/caw_driver.py plan ADDRESS --capture DIR --frame N     (offline: the plan from a kept capture frame; no game)

Design: docs/research/s101/N2_EDITOR_DRIVER.md section 4 and docs/systems/caw_driver.md. The editor's state is the game's own drawn text
(src/caw_text.h, the live verb `!caw`, reference reader tools/n2/caw_read.py): sequence 76, the title, the row runs, the one red (hot) row, the red tab dot.
  * A step is ONE press, then an arrival proof held ARRIVE_FRAMES (4) frames in a row: seq 76, the expected title, a full row set on the grid, exactly one hot
    row, and the expected hot row / tab dot. "Text changed" is never the proof (a page change shows blank frames, then a title, then rows sliding in).
  * Lost press (nothing changed in LOST_FRAMES = 180 guest frames): pressed once more, once. Wrong page (a settled state that is neither the old nor the
    expected): re-planned ONCE from the state that is read. Never blind pressing: no press is sent unless the state before it read as a settled editor page.
  * The plan comes from the learned page table (title -> tab dot, rows, disabled rows, the rows the cursor visits), kept under out\\n2\\ only (game text).
  * A target row that is disabled (the flat grey row) or amber (ACCEPT: its highlight is not red, so arrival cannot be proved) is refused up front.
  * The final FAIL names expected vs seen, the last 5 distinct states with their guest frames, and a screenshot path; the caller logs it before another try.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'live'))

ARRIVE_FRAMES = 4       # consecutive settled frames that must all satisfy the proof
LOST_FRAMES = 180       # guest frames after a press with no change at all = a lost press
STABLE_WAIT = 30        # guest frames to wait for a settled page before a press (mid-transition)
NO_FRAMES_S = 30.0      # wall seconds without a new state: the game is not drawing / the verb is off
GREY = (0.3, 0.5)       # a flat grey row (disabled [U]): its red, not hot
AMBER_MIN = 0.9         # ACCEPT: amber (0.94, 0.63, 0.00): r high but g not small, so never "hot"
BUTTONS = ('RB', 'LB', 'DOWN', 'UP', 'A', 'B')
RETRY_OK = ('RB', 'LB', 'DOWN', 'UP')   # a lost press of these is pressed once more; A and B are never (a second A acts on whatever the first opened)


def norm(name):
    return re.sub(r'\s+', ' ', (name or '').strip()).casefold()


class CawError(Exception):
    """A refusal or a failed step; the message is the whole report."""


# ---- the page table (learned; game text, kept under out\ only) -----------------------------------------------------------------------------------

def main_checkout():
    here = os.path.abspath(os.path.join(HERE, '..', '..'))
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def table_path(root=None):
    if os.environ.get('WWE2K14_CAW_TABLE'):   # S101 lane 118 review: a test points the table elsewhere (the goto selftest uses an empty temp file)
        return os.environ['WWE2K14_CAW_TABLE']
    return os.path.join(root or main_checkout(), 'out', 'n2', 'caw_pages.json')


def rows_hash(pages):
    import hashlib
    return hashlib.sha1(json.dumps(sorted((t, p.get('dot'), p.get('rows')) for t, p in pages.items()), ensure_ascii=False).encode('utf-8')).hexdigest()[:16]


class Table:
    """pages: {TITLE: {dot, rows [names], disabled [names], unproven [names], reach [row indexes the cursor visits], via [parent title, row]}}"""

    def __init__(self, pages=None, stamp=None):
        self.pages = pages or {}
        self.stamp = stamp or {}   # learned_at, build, language, rows_hash (S101 lane 118 review)

    @staticmethod
    def load(path):
        try:
            with open(path, encoding='utf-8') as f:
                d = json.load(f)
                return Table(d.get('pages', {}), d.get('stamp'))
        except (OSError, ValueError):
            return Table()

    def hash_ok(self):
        """The stamped row-set hash still matches the pages (a hand-edited or half-written table is refused); an unstamped table (tests) is fine."""
        return not self.stamp or self.stamp.get('rows_hash') == rows_hash(self.pages)

    def save(self, path, build='unknown', language='unknown'):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.stamp = {'learned_at': time.strftime('%Y-%m-%d %H:%M'), 'build': build, 'language': language, 'rows_hash': rows_hash(self.pages)}
        with open(path, 'w', encoding='utf-8') as f:
            json.dump({'note': 'learned by tools/n2/caw_driver.py learn; holds game text: never commit', 'stamp': self.stamp, 'pages': self.pages}, f, indent=1, ensure_ascii=False)

    def find(self, title):
        for t, p in self.pages.items():
            if norm(t) == norm(title):
                return t, p
        return None, None

    def title_at(self, dot):
        for t, p in self.pages.items():
            if p.get('dot') == dot:
                return t
        return None

    def row_index(self, title, name):
        """-> index of a row by name or '#K' (K-th row, 0 based), or -1."""
        _, p = self.find(title)
        if p is None:
            return -1
        m = re.fullmatch(r'#(\d+)', name.strip())
        if m:
            k = int(m.group(1))
            return k if k < len(p['rows']) else -1
        for i, r in enumerate(p['rows']):
            if norm(r) == norm(name):
                return i
        return -1


# ---- verbs --------------------------------------------------------------------------------------------------------------------------------

def parse_verb(text):
    """'caw:page:CLOTHING/row:UPPER BODY/open' -> [('page','CLOTHING'), ('row','UPPER BODY'), ('open',)]. Raises CawError on a bad form."""
    t = text.strip()
    if t.lower().startswith('caw:'):
        t = t[4:]
    if not t:
        raise CawError('empty caw verb')
    out = []
    for seg in t.split('/'):
        seg = seg.strip()
        low = seg.lower()
        if low == 'open':
            out.append(('open',))
        elif low == 'back':
            out.append(('back',))
        elif low.startswith('tab:'):
            if not re.fullmatch(r'[0-3]', seg[4:]):
                raise CawError('tab: expects 0-3, got "%s"' % seg[4:])
            out.append(('tab', int(seg[4:])))
        elif low.startswith('page:') and seg[5:].strip():
            out.append(('page', seg[5:].strip()))
        elif low.startswith('row:') and seg[4:].strip():
            out.append(('row', seg[4:].strip()))
        else:
            raise CawError('unknown caw verb part "%s" (tab:N, page:TITLE, row:NAME, open, back)' % seg)
    for i, a in enumerate(out):
        if a[0] in ('tab', 'page') and i != 0:
            raise CawError('"%s" can only start a caw verb' % a[0])
        if a[0] == 'open' and (i == 0 or out[i - 1][0] != 'row'):
            raise CawError('"open" must follow a row:')
        if a[0] == 'back' and len(out) != 1:
            raise CawError('"back" stands alone')
    return out


# ---- states -------------------------------------------------------------------------------------------------------------------------------

def sig(s):
    return (s.get('title'), tuple(s.get('rows') or ()), s.get('hot'), s.get('tab'))


def settled(s):
    """A drawn editor page that can be acted on: text present, sequence 76, a title, rows on the grid and none missing, exactly one hot row."""
    return bool(s.get('has_text') and s.get('seq76') and s.get('title') and s.get('complete') and s.get('hot_n') == 1 and s.get('hot', -1) >= 0)


def settled_amber(s):
    """The same without a hot row: a page whose cursor row is amber (ACCEPT [U]) shows no red row."""
    return bool(s.get('has_text') and s.get('seq76') and s.get('title') and s.get('complete') and s.get('hot_n') == 0)


def brief(s):
    if not s.get('has_text'):
        return 'no text'
    return '%s seq%s %r rows=%d%s hot=%s(r %.2f) tab=%s dots=%s' % ('@%s' % s.get('guest'), s.get('seq'), s.get('title'), len(s.get('rows') or ()),
                                                            '' if s.get('complete') else '(incomplete)', s.get('hot'), s.get('hot_r', 0), s.get('tab'), s.get('dots'))


class Step:
    def __init__(self, button, label, action, title=None, title_not=None, rows=None, hot=None, tab=None, same_page=False, tolerant=False, hot_not=None):
        self.button, self.label, self.action = button, label, action
        self.title, self.title_not, self.rows, self.hot, self.tab, self.same_page = title, title_not, rows, hot, tab, same_page
        self.hot_not = hot_not   # the hot row must be anywhere but this one (a learn-walk step)
        self.tolerant = tolerant   # a learn-walk step: no change is an answer (the end of the list), not a failure

    def expect_text(self):
        bits = []
        if self.title is not None:
            bits.append('title %r' % self.title)
        if self.title_not is not None:
            bits.append('title not %r' % self.title_not)
        if self.hot is not None:
            bits.append('hot row %d' % self.hot)
        if self.hot_not is not None:
            bits.append('hot row not %d' % self.hot_not)
        if self.tab is not None:
            bits.append('tab dot %d' % self.tab)
        if self.rows is not None:
            bits.append('%d rows' % len(self.rows))
        return ', '.join(bits) or 'a settled page'

    def proof(self, s, s0):
        """-> True when state `s` is this step's arrival (before the K-frames hold)."""
        if not settled(s):
            return False
        if self.title is not None and norm(s['title']) != norm(self.title):
            return False
        if self.title_not is not None and norm(s['title']) == norm(self.title_not):
            return False
        if self.rows is not None and [norm(r) for r in s['rows']] != [norm(r) for r in self.rows]:
            return False
        if self.hot is not None and s['hot'] != self.hot:
            return False
        if self.hot_not is not None and s['hot'] == self.hot_not:
            return False
        if self.tab is not None and s['tab'] != self.tab:
            return False
        if self.same_page and (s['title'] != s0['title'] or list(s['rows']) != list(s0['rows'])):
            return False
        return True


def is_grey(c):
    """The flat grey row (disabled [U]): r, g and b equal and mid-dark (0.39 measured)."""
    return GREY[0] <= c[0] <= GREY[1] and max(c) - min(c) < 0.05


def is_amber(c):
    """ACCEPT's amber (0.94, 0.63, 0.00): r high, g middling, b near 0 - not red (g > 0.25 r), not white."""
    return c[0] >= AMBER_MIN - 0.05 and 0.4 <= c[1] <= 0.8 and c[2] <= 0.15


def page_entry(s, dot):
    """The table entry of a settled page state: its rows, the flat grey (disabled) rows and the amber ones (ACCEPT [U]); `reach` starts at the hot row."""
    rows = list(s['rows'])
    col = list(zip(s['row_r'], s['row_g'], s['row_b']))
    disabled = [rows[k] for k in range(len(rows)) if is_grey(col[k]) and k != s['hot']]
    unproven = [rows[k] for k in range(len(rows)) if is_amber(col[k]) and k != s['hot']]
    return {'dot': dot, 'rows': rows, 'disabled': disabled, 'unproven': unproven, 'reach': [s['hot']]}


# ---- the planner (pure: a state, the table, the verb -> steps; unit-tested offline on the kept frames) --------------------------------------------

def _neighbours(reach, idx, going_down):
    pos = [r for r in reach]
    if idx not in pos:
        return None
    i = pos.index(idx) + (1 if going_down else -1)
    return pos[i] if 0 <= i < len(pos) else None


def plan(table, state, actions):
    """-> list of Step. Raises CawError (a refusal) with the reason. `state` must be a settled page."""
    if not settled(state):
        raise CawError('refused: the editor page is not settled (%s)' % brief(state))
    steps = []
    title, tab, hot = state['title'], state['tab'], state['hot']
    _, here = table.find(title)
    if here is not None and [norm(r) for r in here['rows']] != [norm(r) for r in state['rows']]:
        raise CawError('refused: the rows of "%s" differ from the learned table (another build or language, or a different page state): re-run: caw_driver.py learn' % title)
    if tab < 0 and any(a[0] in ('tab', 'page') for a in actions):
        raise CawError('refused: no single red tab dot is drawn (tab=%s); the tab cannot be known' % tab)
    for ai, a in enumerate(actions):
        kind = a[0]
        if kind in ('tab', 'page'):
            if kind == 'tab':
                want = a[1]
                want_title = table.title_at(want)
            else:
                t, p = table.find(a[1])
                if p is None:
                    raise CawError('refused: page "%s" is not in the learned table (%s); run: caw_driver.py learn' % (a[1], ', '.join(sorted(table.pages)) or 'empty table'))
                want, want_title = p['dot'], t
            if tab == want and norm(title) != norm(want_title or '') and (kind == 'page' or want_title is not None):
                raise CawError('refused: tab dot %d is the right one but the page showing is "%s", not "%s" (a sub-page? run caw:back first)' % (tab, title, want_title))
            if norm(title) == norm(want_title or '') and tab == want and kind == 'page':
                pass
            else:
                while tab != want:
                    up = want > tab
                    ntab = tab + (1 if up else -1)
                    ntitle = table.title_at(ntab)
                    steps.append(Step('RB' if up else 'LB', 'tab dot %d -> %d' % (tab, ntab), ai, title=ntitle, title_not=title, tab=ntab))
                    tab, title = ntab, (ntitle or '?')
                    hot = 0   # a new page starts on its first row [C: both pages seen]; the learned entry says which one (a grey first row is skipped)
                    if ntitle is not None:
                        _, np_ = table.find(ntitle)
                        steps[-1].rows = np_['rows'] if np_ else None
                        if np_ and np_.get('reach'):
                            hot = np_['reach'][0]
        elif kind == 'row':
            t, p = table.find(title)
            if p is None:
                raise CawError('refused: page "%s" is not in the learned table; run: caw_driver.py learn' % title)
            idx = table.row_index(title, a[1])
            if idx < 0:
                raise CawError('refused: page "%s" has no row "%s" (rows: %s)' % (t, a[1], ', '.join(p['rows'])))
            if norm(p['rows'][idx]) in [norm(x) for x in p.get('disabled', [])]:
                raise CawError('refused: row "%s" of page "%s" is disabled (the flat grey row): the cursor cannot stop there' % (p['rows'][idx], t))
            if norm(p['rows'][idx]) in [norm(x) for x in p.get('unproven', [])]:
                raise CawError('refused: row "%s" of page "%s" is amber: its highlight is not red, so arrival cannot be proved [U]' % (p['rows'][idx], t))
            reach = p.get('reach') or [i for i, r in enumerate(p['rows']) if norm(r) not in [norm(x) for x in p.get('disabled', []) + p.get('unproven', [])]]
            if idx not in reach:
                raise CawError('refused: row "%s" of page "%s" was never reached by the learn walk' % (p['rows'][idx], t))
            if hot not in reach and hot != idx:
                raise CawError('refused: the hot row %d of "%s" is not on the learned cursor path' % (hot, t))
            while hot != idx:
                down = idx > hot
                nh = _neighbours(reach, hot, down)
                if nh is None:
                    raise CawError('refused: no cursor path from row %d to row %d of "%s"' % (hot, idx, t))
                steps.append(Step('DOWN' if down else 'UP', 'row %d -> %d' % (hot, nh), ai, title=t, rows=p['rows'], hot=nh, same_page=True))
                hot = nh
        elif kind == 'open':
            steps.append(Step('A', 'open row %d of "%s"' % (hot, title), ai, title_not=title))
            via = None
            for t2, p2 in table.pages.items():
                if p2.get('via') == [title, hot]:
                    via = t2
            if via:
                steps[-1].title = via
            title = via or '?'
            hot, tab = 0, tab
        elif kind == 'back':
            steps.append(Step('B', 'back out of "%s"' % title, ai, title_not=title))
    return steps


def final_check(table, actions, s):
    """'' when the settled state `s` is where the verb pointed (title, tab dot, hot row); else what differs. open / back end somewhere not known in advance: not checked."""
    tab = title = hot = None
    for a in actions:
        if a[0] == 'tab':
            tab, title = a[1], table.title_at(a[1])
        elif a[0] == 'page':
            t, p = table.find(a[1])
            tab, title = (p['dot'], t) if p else (None, a[1])
        elif a[0] == 'row':
            hot = table.row_index(title or s['title'], a[1])
        else:
            return ''
    bad = []
    if title is not None and norm(s['title']) != norm(title):
        bad.append('title %r, wanted %r' % (s['title'], title))
    if tab is not None and s['tab'] != tab:
        bad.append('tab dot %s, wanted %s' % (s['tab'], tab))
    if hot is not None and hot >= 0 and s['hot'] != hot:
        bad.append('hot row %s, wanted %s' % (s['hot'], hot))
    return '; '.join(bad)


def run_verb(link, table, verb, log=print, wall_s=NO_FRAMES_S):
    """Runs one verb and ALWAYS switches `!caw` off afterwards, success or FAIL (the labels must not stay on)."""
    try:
        return Driver(link, table, log=log, wall_s=wall_s).run(verb)
    finally:
        if hasattr(link, 'close'):
            link.close()


def check_names(table, actions):
    """Offline check of a verb's names against the learned table (no state): '' when fine, else the refusal. A row: with no page: before it is checked live."""
    title = None
    for a in actions:
        if a[0] == 'tab':
            title = table.title_at(a[1])
        elif a[0] == 'page':
            t, p = table.find(a[1])
            if p is None:
                return 'refused: page "%s" is not in the learned table (%s); run: caw_driver.py learn' % (a[1], ', '.join(sorted(table.pages)) or 'empty table')
            title = t
        elif a[0] == 'row' and title is not None:
            idx = table.row_index(title, a[1])
            _, p = table.find(title)
            if idx < 0:
                return 'refused: page "%s" has no row "%s" (rows: %s)' % (title, a[1], ', '.join(p['rows']))
            if norm(p['rows'][idx]) in [norm(x) for x in p.get('disabled', [])]:
                return 'refused: row "%s" of page "%s" is disabled (the flat grey row): the cursor cannot stop there' % (p['rows'][idx], title)
            if norm(p['rows'][idx]) in [norm(x) for x in p.get('unproven', [])]:
                return 'refused: row "%s" of page "%s" is amber: its highlight is not red, so arrival cannot be proved [U]' % (p['rows'][idx], title)
        elif a[0] == 'open':
            title = None   # the sub-page's rows are learned only when it is opened
    return ''


# ---- links: the live game, and a replay of kept frames for the offline tests ---------------------------------------------------------------------

def _guarded(fn):
    """A lost game / odd answer is a CawError (a report), never a traceback (S101 lane 118 review)."""
    def wrap(self, *a, **k):
        try:
            return fn(self, *a, **k)
        except CawError:
            raise
        except Exception as x:
            raise CawError('lost the game on channel "%s": %s: %s' % (self.name, type(x).__name__, x))
    wrap.__name__ = fn.__name__
    return wrap


class LiveLink:
    def __init__(self, name='main', live_module=None):
        if live_module is None:
            import live as live_module
        self.live = live_module
        self.name = name
        self.on = False

    @_guarded
    def _ensure_on(self):
        if not self.on:
            fields, _ = self.live.parse(self.live.request(self.name, '!caw on'))
            if fields.get('status') != 'ok':
                raise CawError('the game refused !caw on: %s' % (fields.get('caw') or fields.get('error') or fields))
            self.on = True

    @_guarded
    def poll(self, since):
        self._ensure_on()
        fields, _ = self.live.parse(self.live.request(self.name, '!caw since %d 64' % since))
        if fields.get('status') != 'ok':
            raise CawError('!caw since failed: %s' % (fields.get('caw') or fields))
        return json.loads(fields.get('caw', '[]'))

    def newest(self):
        got = self.poll(0)
        return got[-1]['guest'] if got else 0

    @_guarded
    def press(self, button):
        """-> the guest frame the press batch started at (`frames: A..B` of the answer; the batch waits for a busy script first, so A can be far after the last poll), or None."""
        fields, log, _ = self.live.run_batch(self.name, ['press ' + button], 30)
        if fields.get('status') != 'ok':
            raise CawError('the press of %s was not accepted: %s %s' % (button, fields.get('status'), fields.get('error', '')))
        m = re.match(r'(\d+)\.\.', fields.get('frames', ''))
        return int(m.group(1)) if m else None

    def close(self):
        """`!caw off` (labels back to what they were); never raises."""
        try:
            self.live.request(self.name, '!caw off', connect_wait=3.0)
        except Exception:
            pass
        self.on = False

    def screenshot(self, label):
        try:
            _, _, pairs = self.live.run_batch(self.name, ['screenshot ' + label], 30)
            return pairs[0][0] if pairs else '(no screenshot)'
        except Exception as x:   # a failed screenshot must not hide the FAIL report
            return '(screenshot failed: %s)' % x


class Driver:
    def __init__(self, link, table, log=print, wall_s=NO_FRAMES_S):
        self.link, self.table, self.log, self.wall_s = link, table, log, wall_s
        self.history = []          # distinct states [(sig, state)] in order
        self.last_guest = 0
        self.latest = None         # the newest state pulled
        self.last_arrival = None   # the state at which the last step's proof had held ARRIVE_FRAMES frames
        self.presses = 0
        self.step_no = 0

    # -- reading
    def _pull(self):
        """New states since the last one; remembers the distinct ones. -> list."""
        got = self.link.poll(self.last_guest)
        for s in got:
            self.last_guest = max(self.last_guest, s['guest'])
            self.latest = s
            key = (s.get('has_text'), s.get('seq76')) + sig(s)
            if not self.history or self.history[-1][0] != key:
                self.history.append((key, s))
        return got

    def _wait_frames(self, pred, frames, what):
        """Pulls states until pred(state) holds for one; gives up after `frames` guest frames of states (or the wall limit). -> state or None."""
        start, quiet_since = None, time.time()
        while True:
            got = self._pull()
            if got:
                quiet_since = time.time()
            for s in got:
                start = s['guest'] if start is None else start
                if pred(s):
                    return s
                if s['guest'] - start > frames:
                    return None
            if time.time() - quiet_since > self.wall_s:
                self.fail(None, 'no new frame from the game for %.0f s while waiting for %s (is !caw on, the game drawing, owner mode live?)' % (self.wall_s, what))
            if not got:
                time.sleep(0.02)

    def drain(self):
        """Reads every state waiting (so a decision is made on the newest frame, not on a backlog)."""
        while len(self._pull()) >= 64:
            pass

    def stable_state(self):
        """A settled page (exactly one hot row). A cursor resting on an amber row (ACCEPT) draws no red row, so it is NOT settled: a verb that starts there FAILs after the wait."""
        self.drain()
        if self.latest is not None and settled(self.latest):
            return self.latest
        s = self._wait_frames(settled, STABLE_WAIT, 'a settled editor page')
        if s is None:
            self.fail(None, 'before the press: the editor page did not settle within %d guest frames (last: %s)' % (STABLE_WAIT, brief(self.history[-1][1]) if self.history else 'nothing read'))
        return s

    # -- one step
    def do_step(self, step):
        """-> ('arrived', state) | ('wrong', state) | ('end', state) for a tolerant step with no change. Raises CawError on the final FAIL."""
        self.step_no += 1
        s0 = self.stable_state()
        attempts = 0
        while True:
            attempts += 1
            g0 = self.last_guest
            started = self.link.press(step.button)
            self.presses += 1
            if started:   # count the 180 frames from where the press batch really began, not from the last poll
                g0 = started - 1
            self.log('  step %d: press %s (%s); expect %s; attempt %d' % (self.step_no, step.button, step.label, step.expect_text(), attempts))
            verdict, s = self._watch(step, s0, g0)
            if verdict == 'arrived':
                self.log('    arrived: %s' % brief(s))
                self.last_arrival = s
                return 'arrived', s
            if verdict == 'nochange':
                if step.tolerant:
                    return 'end', s0
                if attempts == 1 and step.button in RETRY_OK:
                    self.log('    no change in %d guest frames: the press was lost, pressing once more' % LOST_FRAMES)
                    continue
                if step.button not in RETRY_OK:
                    self.fail(step, 'the press of %s changed nothing in %d guest frames; %s is never pressed twice (a second one could act on whatever opened)' % (step.button, LOST_FRAMES, step.button), s0)
                self.fail(step, 'the press of %s changed nothing, twice (%d guest frames each)' % (step.button, LOST_FRAMES), s0)
            if verdict == 'left':
                self.fail(step, 'the game left the editor (sequence is not 76): %s' % brief(s), s0)
            return 'wrong', s   # a settled page that is neither the old nor the expected

    def _watch(self, step, s0, g0):
        held, changed, last_changed = 0, False, None
        start = None
        quiet_since = time.time()
        s0sig = sig(s0)
        while True:
            got = [s for s in self._pull_after(g0)]
            if got:
                quiet_since = time.time()
            for s in got:
                start = s['guest'] if start is None else start
                if s.get('has_text') and not s.get('seq76') and s.get('seq', -1) not in (-1, 76) and held == 0:
                    return 'left', s
                if sig(s) != s0sig and (s.get('has_text') and s.get('title') is not None):
                    changed = True
                if step.proof(s, s0):
                    held += 1
                    last_changed = s
                    if held >= ARRIVE_FRAMES:
                        return 'arrived', s
                else:
                    held = 0
                    if settled(s) and sig(s) != s0sig:
                        # a settled page that is not the target: it must hold before it counts as "wrong" (blank / sliding frames are not settled)
                        last_changed = s
                if s['guest'] - start > LOST_FRAMES:
                    if not changed:
                        return 'nochange', s0
                    return 'wrong', (last_changed or s)
            if time.time() - quiet_since > self.wall_s:
                self.fail(step, 'no new frame from the game for %.0f s after the press' % self.wall_s, s0)
            if not got:
                time.sleep(0.02)

    def _pull_after(self, g0):
        return [s for s in self._pull() if s['guest'] > g0]

    # -- a verb
    def run(self, text):
        actions = parse_verb(text)
        s = self.stable_state()
        steps = plan(self.table, s, actions)
        self.log('plan for %s: %s' % (text, ' '.join('[%s %s]' % (st.button, st.label) for st in steps) or '(already there)'))
        replans, i = 0, 0
        while i < len(steps):
            st = steps[i]
            verdict, got = self.do_step(st)
            if verdict == 'arrived':
                i += 1
                continue
            # wrong page: re-plan once from the state that is read, only for absolute targets (tab / page / row); never for open / back
            if st.action < len(actions) and actions[st.action][0] in ('tab', 'page', 'row') and replans < 1:
                replans += 1
                cur = self.stable_state()
                k = st.action
                while k > 0 and actions[k][0] not in ('tab', 'page'):
                    k -= 1   # a row is relative to its page: re-plan from the tab / page action it belongs to
                self.log('  wrong page: %s; re-planning once from %s' % (brief(got), brief(cur)))
                steps = plan(self.table, cur, actions[k:])
                i = 0
                continue
            self.fail(st, 'the page after the press is not the expected one' + (' (already re-planned once)' if replans else ''), got)
        final = self.stable_state()
        bad = final_check(self.table, actions, final)
        if bad:
            self.fail(None, 'the run ended on a page that is not the target: %s' % bad, final)
        self.log('done: %s' % brief(final))
        return final

    # -- the failure
    def fail(self, step, why, saw=None):
        last5 = [s for _, s in self.history[-5:]]
        lines = ['FAIL step %d: %s' % (self.step_no, why)]
        if step is not None:
            lines.append('  expected: %s (after %s)' % (step.expect_text(), step.label))
        lines.append('  saw: %s' % (brief(saw) if saw else (brief(last5[-1]) if last5 else 'nothing')))
        if step is not None and step.rows is not None and saw and settled(saw) and step.title is not None and norm(saw['title']) == norm(step.title) \
                and [norm(r) for r in saw['rows']] != [norm(r) for r in step.rows]:
            lines.append('  the rows differ from learned: re-run: caw_driver.py learn')
        lines.append('  last %d distinct states:' % len(last5))
        for s in last5:
            lines.append('    ' + brief(s))
        lines.append('  screenshot: %s' % self.link.screenshot('caw_fail_%s' % time.strftime('%H%M%S')))
        lines.append('  presses sent: %d. Log this before another try (CLAUDE.md rule 7); the driver does not retry or relaunch.' % self.presses)
        raise CawError('\n'.join(lines))

    # -- learning
    def learn(self):
        """Walks the four tabs and every row (RB across, DOWN down each page), fills and saves the table. Never presses onto an amber row."""
        s = self.stable_state()
        while s['tab'] > 0:   # to dot 0
            tab = s['tab']
            _, s = self.do_step(Step('LB', 'to tab %d' % (tab - 1), 0, tab=tab - 1, title_not=s['title']))
        pages = {}
        for dot in range(4):
            s = self.stable_state()
            if s['tab'] != dot:
                self.fail(None, 'learn: expected tab dot %d, the frame reads %s' % (dot, s['tab']))
            entry = page_entry(s, dot)
            rows, disabled, unproven, reach = entry['rows'], entry['disabled'], entry['unproven'], entry['reach']
            for _ in range(len(rows) - 1):
                k = reach[-1] + 1
                while k < len(rows) and norm(rows[k]) in [norm(x) for x in disabled]:
                    k += 1   # the cursor is expected to skip a disabled row [U: this walk is what measures it]
                if k >= len(rows) or norm(rows[k]) in [norm(x) for x in unproven]:
                    break   # no row below, or the next one is amber: never press onto it
                verdict, s2 = self.do_step(Step('DOWN', 'learn: down from row %d' % reach[-1], 0, title=s['title'], rows=rows, same_page=True, tolerant=True,
                                                 hot_not=reach[-1]))
                if verdict == 'end' or s2['hot'] <= reach[-1]:
                    break
                reach.append(s2['hot'])
            pages[s['title']] = entry
            self.log('learned %r dot %d: %d rows, cursor visits %s, disabled %d, amber %d' % (s['title'], dot, len(rows), reach, len(disabled), len(unproven)))
            if dot < 3:
                self.do_step(Step('RB', 'learn: to tab %d' % (dot + 1), 0, tab=dot + 1, title_not=s['title']))
        self.table.pages = pages   # replaces: a page the game no longer has must not stay in the table
        return self.table


# ---- the command line ---------------------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--name', default='main')
    ap.add_argument('--table', default='')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('learn')
    sub.add_parser('state')
    g = sub.add_parser('go')
    g.add_argument('address')
    p = sub.add_parser('plan')
    p.add_argument('address')
    p.add_argument('--capture', required=True)
    p.add_argument('--frame', type=int, default=0)
    ap.add_argument('--build', default='unknown', help='learn: stamped into the table')
    ap.add_argument('--language', default='unknown', help='learn: stamped into the table')
    a = ap.parse_args(argv)
    path = a.table or table_path()
    table = Table.load(path)
    link = None
    try:
        if not table.hash_ok():
            raise CawError('refused: %s does not match its own row-set hash (edited by hand or half written): re-run: caw_driver.py learn' % path)
        if a.cmd == 'plan':
            import caw_read
            import glob
            geom = open(os.path.join(a.capture, 'geometry.bin'), 'rb').read()
            files = sorted(glob.glob(os.path.join(a.capture, 'frames', 'frame_*.bin')))
            s = caw_read.read_cstate(files[a.frame], geom)
            for st in plan(table, s, parse_verb(a.address)):
                print('%s  %s  expect %s' % (st.button, st.label, st.expect_text()))
            return 0
        link = LiveLink(a.name)
        d = Driver(link, table)
        if a.cmd == 'state':
            print(brief(d.stable_state()))
        elif a.cmd == 'learn':
            d.learn().save(path, a.build, a.language)
            print('saved %s' % path)
        else:
            d.run(a.address)
        return 0
    except CawError as x:
        print(x)
        return 1
    finally:
        if link is not None:
            link.close()   # `!caw off` whatever happened


if __name__ == '__main__':
    sys.exit(main())
