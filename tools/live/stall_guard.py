"""S101 lane 145: the stall watchdog. Grown from Jarrod's ask after J3: "get better at realizing stalled game state like that,
when it's something as easy as just an A press".

A long wait on the game (goto steps, live.py run batches, routes) polls cheap progress signals once a second and notices when
frames advance but the word the wait depends on does not move while a message sits on screen.

  Detector (pure):   Detector(progress=('match_clock',), frames=180, max_presses=3).feed(Sample) -> Verdict
  Policy  (pure):    decide(text, seq) -> Decision   (safe list: tools/live/stall_safe.py; anything else STOPS)
  Plumbing:          Guard(name).tick(say) polls `!status` once a second at most; a stop raises Stalled.

A stall = the game frame advanced by >= `frames` (default 180 = 3 s at 60 fps) since the progress word last changed, the frame is
still advancing, and there is text on screen. Then: a text the safe list covers gets one A press (START for the start prompt),
logged `[stall] pressed A: screen text '...' (match_clock held at C for N frames)`, at most 3 per Guard; anything else raises
Stalled with `STALLED: match_clock held at C for N frames; screen text '...'` (and a bookmark is taken).

Why a batch cannot be watched from outside: the live channel serves one client at a time and `!status` waits until the running
batch answers. Waits inside a batch are covered by `live.py run --guard` (slices the batch) and by the goto's own waits; see
docs/systems/goto.md "Stall watchdog".

  py tools/live/stall_guard.py watch [--name main] [--progress match_clock] [--frames 180] [--no-press] [--for SECONDS]
"""
import argparse
import re
import os
import sys
import time
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stall_safe as SAFE  # noqa: E402


class Stalled(Exception):
    """A stall the watchdog must not answer by itself. str() is the report."""


@dataclass
class Sample:
    frame: int
    states: dict = field(default_factory=dict)   # name -> int (None = unreadable): the `states:` words of !status
    text: str = ''                               # screen text, strings joined by ' | '
    t: float = 0.0


@dataclass
class Decision:
    kind: str            # 'press' | 'stop'
    button: str = ''     # 'A' | 'START'
    why: str = ''


@dataclass
class Verdict:
    kind: str            # 'ok' | 'held' (word held, nothing on screen: a load) | 'press' | 'stop'
    held: int = 0        # guest frames the progress word has been held
    word: str = ''
    value: object = None
    text: str = ''
    button: str = ''
    why: str = ''

    def line(self):
        what = '%s held at %s for %d frames' % (self.word, self.value, self.held)
        if self.kind == 'press':
            return "[stall] pressed %s: screen text '%s' (%s)" % (self.button, self.text, what)
        if self.kind == 'stop':
            return "STALLED: %s; screen text '%s'%s" % (what, self.text, (' (%s)' % self.why) if self.why else '')
        return ''


def parse_status(fields):
    """`!status` fields (live.parse) -> Sample. states: 'seq=9 scene_word=1 match_clock=15'; unreadable words -> None."""
    states = {}
    for tok in fields.get('states', '').split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                states[k] = int(v, 0)
            except ValueError:
                states[k] = None
    try:
        frame = int(fields.get('frame', ''))
    except ValueError:
        frame = -1
    return Sample(frame, states, fields.get('text', '').strip(), time.time())


# ------------------------------------------------------------------ the policy
def _segments(text):
    return [s.strip() for s in text.split('|') if s.strip()]


def decide(text, seq=None):
    """The safe-list policy. A text is pressed only when it is a button segment plus allowed body segments; everything else
    (no text, an unknown segment, a vetoed word, a glyph-only body outside a match) is a stop. seq: states.toml seq (9 = match)."""
    segs = _segments(text)
    if not segs:
        return Decision('stop', why='no text on screen')
    veto = SAFE.VETO.search(text)
    if veto:
        return Decision('stop', why="the text names a choice or a data action: '%s'" % veto.group(0))
    in_match = seq == SAFE.SEQ_IN_GAME
    button, entry = '', ''
    for seg in segs:
        glyphs = '?' in seg
        core = re.sub(r'\?+', ' ', seg).strip()      # "??? OK": a glyph-coded body in the same segment as the button
        hit = next(((n, b) for n, rx, b, _ in SAFE.BUTTONS if core and re.fullmatch(rx, core, re.I)), None)
        if hit:
            if glyphs and not in_match:
                return Decision('stop', why="'%s': a glyph-coded box is only safe inside a match (seq %s)" % (seg, seq))
            if not button or hit[1] == 'START':
                button, entry = hit[1], hit[0]
            continue
        card = next(((n, m) for n, rx, _, m in SAFE.CARDS if re.fullmatch(rx, seg, re.I)), None)
        if card is None:
            return Decision('stop', why="unknown text '%s'" % seg)
        if card[1] and not in_match:
            return Decision('stop', why="'%s' (%s) is only safe inside a match (seq %s)" % (seg, card[0], seq))
    if not button:
        return Decision('stop', why='no button word on screen to press')
    return Decision('press', button, entry)


# ------------------------------------------------------------------ the detector
class Detector:
    """Feeds on Samples (about one a second). progress: the state words the wait depends on; the first readable one is tracked
    (a wait on the match clock passes ('match_clock',); a wait on a menu passes ('menu',))."""

    def __init__(self, progress=('match_clock',), frames=180, max_presses=3, min_value=1):
        self.progress, self.frames, self.max_presses = tuple(progress), frames, max_presses
        self.min_value = min_value   # a word below this (the clock at 0: no bell yet) is not a held wait: a menu or a load
        self.word = None
        self.value = None
        self.since = None          # guest frame the value was last seen to change (or the first sample)
        self.last_frame = None
        self.presses = 0

    def _read(self, s):
        for w in self.progress:
            v = s.states.get(w)
            if v is not None and v >= self.min_value:
                return w, v
        return None, None

    def feed(self, s):
        if s.frame < 0:
            return Verdict('ok')
        advancing = self.last_frame is not None and s.frame > self.last_frame
        self.last_frame = s.frame
        word, value = self._read(s)
        if word is None:
            return Verdict('ok')
        if word != self.word or value != self.value or self.since is None:
            self.word, self.value, self.since = word, value, s.frame
            return Verdict('ok')
        held = s.frame - self.since
        if held < self.frames or not advancing:
            return Verdict('ok', held, word, value)
        if not s.text:
            return Verdict('held', held, word, value)
        dec = decide(s.text, s.states.get('seq'))
        if dec.kind == 'press' and self.presses < self.max_presses:
            self.presses += 1
            self.since = s.frame            # the word must stay held another `frames` frames before the next verdict
            return Verdict('press', held, word, value, s.text, dec.button, dec.why)
        why = dec.why if dec.kind == 'stop' else 'still held after %d auto-presses' % self.presses
        return Verdict('stop', held, word, value, s.text, why=why)


# ------------------------------------------------------------------ the live plumbing
class Guard:
    """One per wait (or one per goto run: the press budget is per Guard). tick(say) polls at most once a second, never raises on a
    game that is busy or gone, and raises Stalled for a stop."""

    def __init__(self, name='main', progress=('match_clock',), frames=180, max_presses=3, poll_s=1.0, press=True, min_value=1):
        self.name, self.press, self.poll_s = name, press, poll_s
        self.det = Detector(progress, frames, max_presses, min_value)
        self.next_poll = 0.0

    def sample(self):
        import live
        try:
            fields, _ = live.parse(live.request(self.name, '!status', connect_wait=0.4))
        except (live.NoGame, OSError):
            return None
        return parse_status(fields) if fields.get('status') == 'ok' else None

    def tick(self, say=print):
        now = time.time()
        if now < self.next_poll:
            return None
        self.next_poll = now + self.poll_s
        s = self.sample()
        return self.act(self.det.feed(s), say) if s is not None else None

    def act(self, v, say=print):
        import live
        if v.kind == 'press':
            if not self.press:
                raise Stalled(v.line().replace('[stall] pressed', 'STALLED (watch only, would press'))
            say(v.line())
            live.run_batch(self.name, ['press %s' % v.button], 30)
        elif v.kind == 'stop':
            line = v.line()
            try:
                bm, _ = live.bookmark(self.name)
                line += '\n' + bm.splitlines()[0]
            except Exception:
                pass
            raise Stalled(line)
        return v


def run_guarded(name, lines, timeout=120, shot=False, width=1280, say=print, slice_s=4, frames=180, press=True):
    """live.py `run --guard`: a batch that waits, watched. The batch goes out with a short answer slice; while it runs on (the
    answer says `timeout ... running: line N`) the channel is free for `!status`, which feeds the Detector. A stall on a safe text:
    `!stop` ends the batch, one press answers the message, and the result is status 'stalled' naming the line to resume from
    (the rest of the batch is NOT run: a press cannot be injected into a running batch). Anything else: Stalled (with a bookmark).
    -> (fields, log, pairs) like live.run_batch."""
    import live
    fields, log, pairs = live.run_batch(name, lines, slice_s, shot, width)
    if fields.get('status') != 'timeout':
        return fields, log, pairs                 # ended inside the slice
    g = Guard(name, frames=frames, press=press)
    end = time.time() + timeout
    while time.time() < end:
        try:
            cur, _ = live.parse(live.request(name, '!status', connect_wait=2.0))
        except live.NoGame:
            break
        if cur.get('busy') == 'no':
            fields['status'] = 'ok'
            fields['note'] = 'the batch ended while watched; its own answer (log lines) was not collected'
            return fields, log, pairs
        v = g.det.feed(parse_status(cur))
        if v.kind == 'press' and press:
            say(v.line())
            live.request(name, '!stop')
            live.run_batch(name, ['press %s' % v.button], 30)
            fields['status'] = 'stalled'
            fields['error'] = '%s; the batch was stopped at %s: re-send the rest from there' % (v.line(), fields.get('running', 'its running line'))
            return fields, log, pairs
        if v.kind in ('press', 'stop'):
            raise Stalled(v.line() if v.kind == 'stop' else 'STALLED (watch only): ' + v.line())
        time.sleep(1.0)
    fields['status'] = 'timeout'
    return fields, log, pairs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['watch'])
    ap.add_argument('--name', default='main')
    ap.add_argument('--progress', default='match_clock')
    ap.add_argument('--frames', type=int, default=180)
    ap.add_argument('--no-press', action='store_true')
    ap.add_argument('--for', dest='secs', type=float, default=600.0)
    a = ap.parse_args()
    g = Guard(a.name, tuple(a.progress.split(',')), a.frames, press=not a.no_press)
    end = time.time() + a.secs
    try:
        while time.time() < end:
            g.tick(print)
            time.sleep(0.5)
    except Stalled as x:
        print(x)
        return 1
    print('no stall in %.0f s' % a.secs)
    return 0


if __name__ == '__main__':
    sys.exit(main())
