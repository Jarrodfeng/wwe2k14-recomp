"""S101 lane 102 (go-anywhere L-1, "bus moments"): the WHEN grammar over the event bus (src/event_bus.h, logs/events_NNN.jsonl).

    py -I tools/goto/when.py EVENTS.jsonl WHEN...          evaluate WHEN against a stream: the guest frame, the matched line (or why not)
    py -I tools/goto/when.py --parse WHEN...               parse + canonical text

A WHEN is one or more clauses, each `@NAME[:ARG][~late][#K][{field=value,...}][+-Nf|+-Ns]`, evaluated in order: every clause searches the
stream after the event the previous one matched (the stream position, not only the frame). Design: docs/research/s101/GOANY_MOMENTS.md 2.1.

    @bell                      match / clock_started
    @entrance:S[:STEP]         entrance {slot S [, step STEP]}
    @pin                       match / pin                 filter  by= (pinning_slot)
    @count:N                   match / count N
    @kickout[:N][~late]        match / kickout (at_count N); ~late = the count stopped >= 0.9 x the count interval after count N
                               (stopped_f - count_N frame >= round(0.9 x interval); interval = the gap between two counts of the same pin,
                               58 frames when the pin shows only one: measured S101 lane 95; src/event_bus.h `stopped_f`, lane 102)
    @finish[:NAME]             match / finish (pinfall, submission, ...)   filters winner= loser= (names resolve through db/ids/characters.json)
    @eliminated                match / eliminated          filters slot= id= over_the_top= n= (only real ones: id 255 and lines before the first
                               clock_started are the non-wrestler seats' start burst, never counted; S101 lane 120)
    @remaining:N               the eliminated line whose `remaining` == N (the Rumble bus carries it, S101 lane 120). A stream whose lines have no
                               `remaining` (older build) falls back to counting: entrants = {of=N} on the clause (or the caller's `entrants`)
    @entered                   match / entered (the Rumble entrants; the starters are n = 1..G+81, so @entered#3 is the first walk-in)  filters n= slot=
                               (id is -1 on `entered`; the follow-up `entered_id` line carries it)
    @move:WHO:CLASS            move / a character's move of class signature | finisher | normal (WHO: a name through db/ids/characters.json, every
                               whole-word match, variants of one name together, e.g. brock = both Brock Lesnars, rock != Brock, big = ambiguous;
                               S101 lane 120 / lane 113). LIMIT: the same move twice in a row on one character is ONE line (the game's logger
                               clears and rewrites inside one call; docs/systems/moves.md), so a repeat finisher after a reversal is not seen.
    @load:WORD                 a load whose file contains WORD (case-insensitive)
    @seq:N                     seq N
    @code:X01                  wm30 / code: the event's code % 100 == 1 (or @code:303 = that code)   -- bus kind comes with L-4, not in the game yet
    @playing:BANK              scene / bank BANK                                                       -- bus kind comes with L-4, not in the game yet

#K = the K-th (1-based) occurrence of the filtered event. A negative offset names a frame BEFORE the event: only a recording can arrive
there (offset_frames < 0 -> Hit.needs_recording). 1 second = 60 guest frames.
Nothing here starts a game. An unmatched WHEN raises WhenError saying what the stream does hold (never a silent frame).
"""
import json
import os
import re
import sys
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

KINDS = ('bell', 'entrance', 'pin', 'count', 'kickout', 'finish', 'eliminated', 'remaining', 'entered', 'move', 'load', 'seq', 'code', 'playing')
NEW_KINDS = ('pin', 'count', 'kickout', 'finish', 'eliminated', 'remaining', 'entered', 'move', 'load', 'seq', 'code', 'playing')  # the names address.py claims for `when`
CLAUSE = re.compile(r'@(?P<name>[a-z]+)(?::(?P<arg>[^@~#{}]*?))?(?P<late>~late)?(?:#(?P<occ>\d+))?(?:\{(?P<flt>[^{}@]*)\})?(?:#(?P<occ2>\d+))?(?P<off>[+-]\d+[fs])?\s*(?=@|$)')
# address.py: the right-most WHEN clause of an address (everything after its '@' has no other '@')
TAIL = re.compile(r'@(?:%s)\b[^@]*$' % '|'.join(NEW_KINDS))
FRAMES_PER_S = 60
DEFAULT_COUNT_INTERVAL = 58   # frames between two counts of one pin: measured (pin -> count 1 = 29, count -> count = 58) on events_7060/7063/7055
LATE_FRACTION = 0.9
MOVE_CLASSES = ('signature', 'finisher', 'normal')
FILTER_ALIAS = {'by': 'pinning_slot', 'winner': 'winner_id', 'loser': 'loser_id'}
NAME_FIELDS = ('winner_id', 'loser_id', 'id')


class WhenError(ValueError):
    """The WHEN is malformed, or the stream has no such event: the message says which and what the stream holds."""


class NotFound(WhenError):
    """Well-formed WHEN, but the stream (so far) holds no such event."""


@dataclass
class Clause:
    name: str
    arg: str = ''
    late: bool = False
    occ: int = 1
    filters: dict = field(default_factory=dict)
    offset: int = 0             # frames, signed
    text: str = ''

    def canon(self):
        out = '@' + self.name + (':' + self.arg if self.arg != '' else '') + ('~late' if self.late else '') + ('#%d' % self.occ if self.occ != 1 else '')
        if self.filters:
            out += '{' + ','.join('%s=%s' % kv for kv in self.filters.items()) + '}'
        if self.offset:
            out += '%+df' % self.offset
        return out


@dataclass
class Hit:
    frame: int                  # the frame the WHEN names: the last clause's event frame + its offset
    event_frame: int            # the last clause's event frame
    line: dict                  # the last clause's event
    index: int                  # its position in the stream
    chain: list = field(default_factory=list)   # [(Clause canon, event frame)] for every clause
    offset: int = 0

    @property
    def needs_recording(self):
        return self.offset < 0


# ---------------------------------------------------------------- parsing

def parse(text):
    """-> [Clause]. Raises WhenError. `text` is `@a@b:1...`; whitespace around clauses is ignored."""
    if not isinstance(text, str) or not text.strip():
        raise WhenError('empty WHEN')
    s = text.strip()
    if not s.startswith('@'):
        raise WhenError('a WHEN starts with @ (got "%s")' % s[:40])
    out, pos = [], 0
    while pos < len(s):
        m = CLAUSE.match(s, pos)
        if not m:
            raise WhenError('cannot read the WHEN clause at "%s" (forms: %s; modifiers ~late #K {field=value} +-Nf)' % (s[pos:pos + 30], ', '.join('@' + k for k in KINDS)))
        pos = m.end()
        out.append(_clause(m, s[m.start():m.end()].strip()))
    return out


def _clause(m, text):
    name = m.group('name')
    if name not in KINDS:
        raise WhenError('unknown WHEN @%s (known: %s)' % (name, ', '.join('@' + k for k in KINDS)))
    arg = (m.group('arg') or '').strip()
    c = Clause(name, arg, bool(m.group('late')), 1, {}, 0, text)
    occ = m.group('occ') if m.group('occ') is not None else m.group('occ2')
    if m.group('occ') is not None and m.group('occ2') is not None:
        raise WhenError('@%s: #K twice' % name)
    if occ is not None:
        c.occ = int(occ)
        if c.occ < 1:
            raise WhenError('@%s#%s: occurrences count from 1' % (name, occ))
    if m.group('flt'):
        for part in m.group('flt').split(','):
            k, eq, v = part.partition('=')
            k, v = k.strip(), v.strip()
            if not eq or not k or not v:
                raise WhenError('@%s{%s}: a filter is field=value' % (name, part))
            c.filters[k] = v
    if m.group('off'):
        o = m.group('off')
        c.offset = int(o[:-1]) * (FRAMES_PER_S if o[-1] == 's' else 1)
    # arguments per kind
    if name in ('bell', 'pin', 'eliminated', 'entered') and arg:
        raise WhenError('@%s takes no :argument (got ":%s")' % (name, arg))
    if name in ('count', 'remaining', 'seq', 'playing'):
        if not re.fullmatch(r'\d+', arg):
            raise WhenError('@%s:N needs a number (got "%s")' % (name, arg))
    if name == 'count' and not 1 <= int(arg) <= 3:
        raise WhenError('@count:N: the referee counts 1-3 (got %s)' % arg)
    if name == 'remaining' and int(arg) < 1:
        raise WhenError('@remaining:N: N is at least 1 (0 remaining is the finish: use @finish)')
    if name == 'kickout' and arg and not re.fullmatch(r'\d+', arg):
        raise WhenError('@kickout:N needs a number (got "%s")' % arg)
    if c.late and name != 'kickout':
        raise WhenError('~late belongs to @kickout only')
    if name == 'entrance':
        if not re.fullmatch(r'\d+(?::\d+)?', arg):
            raise WhenError('@entrance:S[:STEP] needs numbers (got "%s")' % arg)
    if name == 'move' and (arg.count(':') != 1 or arg.split(':')[1] not in MOVE_CLASSES or not arg.split(':')[0]):
        raise WhenError('@move:WHO:CLASS needs a character and a class (%s), got "%s"' % ('|'.join(MOVE_CLASSES), arg))
    if name == 'move':
        move_ids(arg.split(':')[0])   # an unknown or ambiguous name is refused when the WHEN is read
    if name == 'load' and not arg:
        raise WhenError('@load:WORD needs a word of the file name')
    if name == 'code' and not re.fullmatch(r'(?:X?\d{2}|\d{3,5})', arg, re.I):
        raise WhenError('@code:X01 / X02 or a raw code number (got "%s")' % arg)
    if name == 'finish' and arg and not re.fullmatch(r'[A-Za-z0-9_ \-]+', arg):
        raise WhenError('@finish:NAME is a finish name like pinfall (got "%s")' % arg)
    return c


def canon(text):
    return ''.join(c.canon() for c in parse(text))


# ---------------------------------------------------------------- the stream

def load_events(path):
    """A events_NNN.jsonl (or an index's events.jsonl) -> [dict]. Raises WhenError on an unreadable file or a line that is not an event
    ({"f": int, "kind": str}); a last line without a newline (a file being written) is dropped."""
    try:
        with open(path, encoding='utf-8', newline='') as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as x:
        raise WhenError('cannot read the event stream %s: %s' % (path, x))
    lines = text.split('\n')
    if lines[-1] != '':
        lines = lines[:-1]
    out = []
    for n, raw in enumerate(lines, 1):
        raw = raw.rstrip('\r')
        if not raw.strip():
            continue
        out.append(check_event(raw, '%s line %d' % (path, n)))
    return out


def check_event(raw, where='line'):
    try:
        e = json.loads(raw)
    except ValueError as x:
        raise WhenError('%s is not JSON: %s' % (where, x))
    if not isinstance(e, dict) or not isinstance(e.get('f'), int) or not isinstance(e.get('kind'), str):
        raise WhenError('%s is not a bus event (needs "f" and "kind"): %s' % (where, raw[:80]))
    return e


_CHARS = None


def character_names():
    global _CHARS
    if _CHARS is None:
        with open(os.path.join(REPO, 'db', 'ids', 'characters.json'), encoding='utf-8') as f:
            _CHARS = {int(e['id']): e['name'] for e in json.load(f)['entries']}
    return _CHARS


def char_id(word):
    """A character id from digits or a name (case-insensitive; exact, else a unique substring). Raises WhenError."""
    if re.fullmatch(r'-?\d+', word):
        return int(word)
    names = character_names()
    low = word.lower()
    exact = [i for i, n in names.items() if n.lower() == low]
    if len(exact) == 1:
        return exact[0]
    part = [i for i, n in names.items() if low in n.lower()]
    if len(part) == 1:
        return part[0]
    raise WhenError('character "%s" %s in db/ids/characters.json' % (word, 'is ambiguous: ' + ', '.join(names[i] for i in part[:5]) if part else 'is not'))


def move_ids(word):
    """@move:WHO: the character ids of the name WHO, matched by WHOLE WORDS (`rock` is The Rock, not Brock), or the digits. Several ids only for
    variants of one name ("X" and "X (Retro)"); otherwise ambiguous, refused like char_id. Raises WhenError."""
    if re.fullmatch(r'-?\d+', word):
        return {int(word)}
    want = re.findall(r'[a-z0-9]+', word.lower())
    if not want:
        raise WhenError('@move:WHO: "%s" names no character' % word)
    found = {}
    for i, n in character_names().items():
        if all(w in re.findall(r'[a-z0-9]+', n.lower()) for w in want):
            found.setdefault(re.sub(r'\s*\(.*\)\s*$', '', n), set()).add(i)
    if not found:
        raise WhenError('character "%s" is not in db/ids/characters.json' % word)
    if len(found) > 1:
        exact = [b for b in found if b.lower() == word.lower()]
        if len(exact) != 1:
            raise WhenError('character "%s" is ambiguous: %s' % (word, ', '.join(sorted(found)[:5])))
        return found[exact[0]]
    return next(iter(found.values()))


def _filter_ok(e, filters):
    for k, v in filters.items():
        if k == 'of':
            continue   # a clause parameter (@remaining), not an event field
        key = FILTER_ALIAS.get(k, k)
        if key not in e:
            return False
        have = e[key]
        if key in NAME_FIELDS:
            if have != char_id(v):
                return False
        elif isinstance(have, bool):
            if str(have).lower() != v.lower():
                return False
        elif isinstance(have, int):
            if not re.fullmatch(r'-?\d+', v) or have != int(v):
                return False
        elif str(have).lower() != v.lower():
            return False
    return True


# ---------------------------------------------------------------- matching

def _is(e, kind, what=None):
    return e['kind'] == kind and (what is None or e.get('what') == what)


def _base(c, e):
    """Does event e match clause c's kind + argument (before filters / #K / ~late)?"""
    n, a = c.name, c.arg
    if n == 'bell':
        return _is(e, 'match', 'clock_started')
    if n == 'entrance':
        parts = a.split(':')
        return e['kind'] == 'entrance' and e.get('slot') == int(parts[0]) and (len(parts) < 2 or e.get('step') == int(parts[1]))
    if n == 'pin':
        return _is(e, 'match', 'pin')
    if n == 'count':
        return _is(e, 'match', 'count') and e.get('count') == int(a)
    if n == 'kickout':
        return _is(e, 'match', 'kickout') and (not a or e.get('at_count') == int(a))
    if n == 'finish':
        return _is(e, 'match', 'finish') and (not a or str(e.get('finish_name', '')).lower() == a.lower())
    if n == 'eliminated':
        return _is(e, 'match', 'eliminated') and e.get('id') != 255   # S101 lane 120: id 255 = a non-wrestler seat, never an elimination
    if n == 'entered':
        return _is(e, 'match', 'entered')
    if n == 'move':
        who, cls = a.split(':')
        return e['kind'] == 'move' and e.get('class') == cls and e.get('id') in move_ids(who)
    if n == 'load':
        return e['kind'] == 'load' and a.lower() in str(e.get('file', '')).lower()
    if n == 'seq':
        return e['kind'] == 'seq' and e.get('seq') == int(a)
    if n == 'code':
        if e['kind'] != 'wm30' or not isinstance(e.get('code'), int):
            return False
        if re.fullmatch(r'X?\d{2}', a, re.I):
            return e['code'] % 100 == int(a.lstrip('xX'))
        return e['code'] == int(a)
    if n == 'playing':
        return e['kind'] == 'scene' and e.get('bank') == int(a)
    return False


def late_info(stream, idx):
    """-> (stopped_f, count_n_frame, interval, threshold, is_late) for the kickout at stream[idx]; raises WhenError when it cannot be told.
    The count N frame and the interval are read from the same pin (the counts between the last pin event and this kickout)."""
    k = stream[idx]
    if 'stopped_f' not in k:
        raise WhenError('~late needs the kickout\'s stopped_f (src/event_bus.h, S101 lane 102): this stream\'s kickout at frame %d has none '
                        '(an older build emitted it 20 frames late with no frame of its own)' % k['f'])
    n = k.get('at_count', 0)
    pin_i = max((i for i in range(idx) if _is(stream[i], 'match', 'pin')), default=-1)
    counts = {}
    for i in range(pin_i + 1, idx):
        e = stream[i]
        if _is(e, 'match', 'count') and e.get('count') not in counts:
            counts[e['count']] = e['f']
    if n == 0:
        base = stream[pin_i]['f'] if pin_i >= 0 else None
    else:
        base = counts.get(n)
    if base is None:
        raise WhenError('~late: the kickout at frame %d (at_count %s) has no count %s / pin event before it in this stream' % (k['f'], n, n))
    gaps = [counts[c] - counts[c - 1] for c in sorted(counts) if c - 1 in counts]
    interval = gaps[-1] if gaps else DEFAULT_COUNT_INTERVAL
    threshold = round(LATE_FRACTION * interval)
    return k['stopped_f'], base, interval, threshold, (k['stopped_f'] - base) >= threshold


def _candidates(stream, c, start, entrants=None):
    """The stream positions (>= start) that satisfy clause c, in order, as (index, event)."""
    first_bell = next((i for i, e in enumerate(stream) if _is(e, 'match', 'clock_started')), -1)
    if c.name == 'remaining':
        # S101 lane 120: the Rumble bus puts `remaining` on every eliminated line
        real = [(i, e) for i, e in enumerate(stream) if i >= start and i > first_bell and _is(e, 'match', 'eliminated') and e.get('id') != 255]
        carried = [(i, e) for i, e in real if 'remaining' in e]
        if carried:
            return [(i, e) for i, e in carried if e['remaining'] == int(c.arg)][:1]
        of = int(c.filters['of']) if 'of' in c.filters else entrants
        if of is None and not real:
            return []     # no elimination yet: the bus line that carries `remaining` has not come (a live wait keeps waiting)
        if of is None:
            raise WhenError('@remaining:%s needs the entrant count: write {of=N} (or the place names it: rumble=N)' % c.arg)
        need = of - int(c.arg)
        if need < 1:
            raise WhenError('@remaining:%s with %d entrants: that many remain from the start (the bell), not after an elimination' % (c.arg, of))
        elim = real
        return elim[need - 1:need]       # the (entrants - N)-th elimination leaves N
    out = []
    for i in range(start, len(stream)):
        e = stream[i]
        if c.name == 'eliminated' and i <= first_bell:
            continue   # S101 lane 120: before the bell is the seats' start burst
        if _base(c, e) and _filter_ok(e, c.filters):
            if c.late and not late_info(stream, i)[4]:
                continue
            out.append((i, e))
    return out


def summary(stream):
    """What the stream holds, in the order a person asks: 'bell at 12044; pins at 15081; kickouts: none; eliminated: none; finish pinfall at 15227'."""
    def at(pred):
        return [e for e in stream if pred(e)]
    bells = at(lambda e: _is(e, 'match', 'clock_started'))
    pins = at(lambda e: _is(e, 'match', 'pin'))
    kos = at(lambda e: _is(e, 'match', 'kickout'))
    els = at(lambda e: _is(e, 'match', 'eliminated') and e.get('id') != 255)
    fin = at(lambda e: _is(e, 'match', 'finish'))
    return '; '.join([
        'bell at %s' % ', '.join(str(e['f']) for e in bells) if bells else 'bell: none',
        'pins at %s' % ', '.join(str(e['f']) for e in pins) if pins else 'pins: none',
        'kickouts at %s' % ', '.join('%d (count %s)' % (e['f'], e.get('at_count')) for e in kos) if kos else 'kickouts: none',
        'eliminated: %d' % len(els) if els else 'eliminated: none',
        ', '.join('finish %s at %d' % (e.get('finish_name'), e['f']) for e in fin) if fin else 'finish: none'])


def evaluate(stream, when, entrants=None):
    """-> Hit for the WHEN (text or [Clause]) over the stream, or raises WhenError listing what the stream has."""
    clauses = parse(when) if isinstance(when, str) else when
    start, chain, last = 0, [], None
    for c in clauses:
        cand = _candidates(stream, c, start, entrants)
        if len(cand) < c.occ:
            raise NotFound('no %s in the stream%s: %d match%s%s. The stream holds: %s' % (
                c.canon(), (' after frame %d' % stream[start - 1]['f']) if start else '', len(cand), '' if len(cand) == 1 else 'es',
                ('; #%d asked' % c.occ) if c.occ != 1 else '', summary(stream)))
        i, e = cand[c.occ - 1]
        chain.append((c.canon(), e['f']))
        last = (i, e, c)
        start = i + 1
    i, e, c = last
    frame = e['f'] + c.offset
    if frame < 1:
        raise WhenError('%s names frame %d: before the first frame (the event is at %d, the offset %+d)' % (c.canon(), frame, e['f'], c.offset))
    return Hit(frame, e['f'], e, i, chain, c.offset)


# ---------------------------------------------------------------- a live wait: partial streams

def kinds_in(clauses):
    return {c.name for c in clauses}


def terminal_hit(stream, clauses, entrants=None):
    """For a live wait over the stream so far (no Hit yet): the first terminal event that ends the wait for good, or None. Terminal =
    the finish (unless a clause still to match is @finish), seq leaving 9 for another sequence (unless a clause is @seq). Only events after
    the last matched clause count: a `finish` that a previous clause matched is not a terminal."""
    start, remaining = 0, list(clauses)
    while remaining:
        c = remaining[0]
        cand = _candidates(stream, c, start, entrants)
        if len(cand) < c.occ:
            break
        start = cand[c.occ - 1][0] + 1
        remaining.pop(0)
    if not remaining:
        return None
    left = kinds_in(remaining)
    for j in range(start, len(stream)):
        e = stream[j]
        if 'finish' not in left and _is(e, 'match', 'finish'):
            return e
        if 'seq' not in left and e['kind'] == 'seq' and e.get('seq') != 9 and any(x['kind'] == 'seq' and x.get('seq') == 9 for x in stream[:j]):
            return e
    return None


def live_state(stream, when, entrants=None):
    """('hit', Hit) | ('terminal', event) | ('wait', None) for the stream so far."""
    clauses = parse(when) if isinstance(when, str) else when
    try:
        return 'hit', evaluate(stream, clauses, entrants)
    except NotFound:
        pass
    t = terminal_hit(stream, clauses, entrants)
    return ('terminal', t) if t else ('wait', None)


# ---------------------------------------------------------------- the DO-order check (past bug: S101 check B, run 7055)

LOG_EVENT = re.compile(r'\[wwe2k14\]\[event\] (\{.*\})\s*$')


def order_check(log_lines, arm_regex, trigger_when):
    """The DO verbs (@after, @victory, @fire) must be acknowledged BEFORE the event that triggers what they arm. `log_lines` is the game's
    run log (lines with the bus events `[wwe2k14][event] {json}` and the verb's acknowledgement line, e.g. `victory: armed scene 6343`);
    `arm_regex` finds the acknowledgement; `trigger_when` is the WHEN of the trigger event (e.g. `@load:WINNING`). Order is the log's own
    line order (the arm line carries no guest frame). -> (ok, message). Never passes on a missing line."""
    arm_i, stream, pos = None, [], []
    rx = re.compile(arm_regex)
    for i, ln in enumerate(log_lines):
        if arm_i is None and rx.search(ln):
            arm_i = i
        m = LOG_EVENT.search(ln)
        if m:
            stream.append(check_event(m.group(1), 'log line %d' % (i + 1)))
            pos.append(i)
    if arm_i is None:
        return False, 'the log has no line matching /%s/: the verb was never acknowledged' % arm_regex
    try:
        hit = evaluate(stream, trigger_when)
    except WhenError as x:
        return False, 'the trigger %s is not in the log\'s event lines: %s' % (trigger_when, x)
    trig_i = pos[hit.index]
    if arm_i > trig_i:
        return False, ('armed after the trigger: the verb was acknowledged at log line %d, but %s (frame %d) came at line %d, %d lines earlier: '
                       'the trigger has already run, the arrival is refused' % (arm_i + 1, trigger_when, hit.event_frame, trig_i + 1, arm_i - trig_i))
    return True, 'armed before the trigger: verb at log line %d, %s (frame %d) at line %d' % (arm_i + 1, trigger_when, hit.event_frame, trig_i + 1)


def main(argv):
    if argv and argv[0] == '--parse':
        bad = 0
        for t in argv[1:]:
            try:
                print('%s -> %s' % (t, canon(t)))
            except WhenError as x:
                bad += 1
                print('%s -> REFUSED: %s' % (t, x))
        return 1 if bad else 0
    if len(argv) < 2:
        print(__doc__)
        return 1
    try:
        stream = load_events(argv[0])
        rc = 0
        for t in argv[1:]:
            try:
                h = evaluate(stream, t)
                print('%s -> frame %d (event at %d: %s)%s' % (t, h.frame, h.event_frame, json.dumps(h.line), '  [needs a recording]' if h.needs_recording else ''))
            except WhenError as x:
                rc = 1
                print('%s -> %s' % (t, x))
        return rc
    except WhenError as x:
        print('REFUSED: %s' % x)
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
