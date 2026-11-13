"""S101 N1 (go-anywhere): the address scheme. One string names a place in the game; parse, print, validate.

    py -I tools/goto/address.py ADDRESS...        parse + canonical form (or the reason it is refused)

Forms (RENDERER_ARC.md 4A design 1; docs/systems/goto.md):
    menu:PATH                  an atlas node id ("list:7", "setup:55", "screen:1300", "mode:caw") or the words of its path name
                               ("play > one on one > ladder", "options")
    match:SPEC                 a match builder spec (tools/atlas/match.py: "Daniel Bryan vs Brock Lesnar; type=one on one > normal")
    match:SPEC@entrance:N      ... and wait for slot N's entrance (event bus `entrance`)
    match:SPEC@bell            ... and wait for the bell (event bus `match` / clock_started)
    wm30:N                     30 Years of WrestleMania match N (0-45; tools/wm30/list.py), its first moment (X00)
    wm30:N@moment:K            ... moment K: 0 / 1 / 2 = the match's own X00 / X01 / X02 code, or a raw SCEX code (>= 10000)
    wm30:N@fire:BANK[/force]   S101 lane 108: load 30 Years match N, then once the match runs fire scene bank BANK the way the match's own Lua call 1003 does
                               (flag 0: the match resumes after the scene); BANK must be a bank the match's script plays, unless /force
    wm30:N@step:K[/force]      S101 lane 121: load match N, then once the bell has gone set the match script's step to K so its own step body runs (positioning,
                               its own scene call, its own next step); K must be a real step of the chunk; a step behind an objective test needs /force
    wm30:N@moment:BANK         S101 lane 121: ... the step that plays scene bank BANK (1000-9999; the resolver picks it from the chunk): the faithful moment.
                               A moment skips the objective by design, so the plan carries force by itself
    caw:CATEGORY/ITEM          Create-a-Superstar category and item           (= caw:page:CATEGORY/row:ITEM)
    caw:tab:N                  S101 lane 118 (N2 / L-6): the editor's tab dot N (0-3) active; the editor must be open (sequence 76)
    caw:page:TITLE[/row:NAME][/open]   ... the page titled TITLE (reached by RB / LB from the learned page table), its row NAME (or #K), and A on it
    caw:row:NAME[/open]        ... a row of the page that is showing;  caw:back = B.  Each press is proved by the drawn page (tools/n2/caw_driver.py)
    universe:PAGE              a Universe page                                (parses; not reachable yet)
    cutscene:ID                a cutscene / story scene id                    (parses; not reachable yet)
    scene:ID                   S101 lane 38: play scene bank ID from the main menu through the stage push (`!scene stage`, then `!scene play ID`); ID is
                               checked against the bank-id table (tools/goto/scene_ids.py): an id no pac holds is an error
    scene:ID[:CHARS]@arena:N   S101 lane 72: ... into the set of arena N (a pac\bg\bgNN.pac: 0-9, 11-17, 21-23, 28-56, 66-68, 76-80); without it the stage's own arena 76 is the
                               editor's empty black stage, not a set
    scene:leave                S101 lane 72: leave the stage the game's own way (the editor's cancel: `!scene leave`), back to the main menu
    scene:ID:CHARS             ... with up to 6 performers: CHARS = CHAR[/ATTIRE],CHAR[/ATTIRE]... (character ids 100-511, attire 0-255; ids from db/ids/characters.json)
    match:SPEC@scene:ID[,ID...]  ... and, inside the match, post up to 6 scene banks now (mirror of sub_82738060: S101 check B: it preloads, it does not play)
    match:SPEC@after:ID[,ID...]  ... and queue up to 6 scene banks to play when the match ends (request table + G+468: the game's flow state 25)
    match:SPEC@victory:ID[/L]    ... and force the victory scene ID (L = the performer layout 0-17; optional)
    moment:NAME@frame:F        a recording in out/moments/ (tools/goto/moments.py), played to guest frame F, then handed over
    moment:NAME@event:KIND[:N] ... to the frame of the N-th (0-based, default 0) event line `E F TEXT` of the recording whose text starts with KIND
    PLACE@WHEN                 S101 lane 102: a WHEN over the event bus (tools/goto/when.py: @pin @count:N @kickout[:N][~late] @finish[:NAME] @eliminated#K
                               @remaining:N @load:WORD @code:X01 @playing:BANK @seq:N, modifiers #K {field=value} +-Nf, e.g. match:SPEC@pin-60f,
                               moment:NAME@kickout:2~late-30f): live = build the place and wait for the event; recorded = the library moment's index
                               names the frame offline and the replay hands over there (a negative offset needs the recording)
An address the resolver cannot reach yet parses fine and resolves to "not reachable yet: <why>" (tools/goto/resolve.py).
"""
import os
import re
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import when as _when  # S101 lane 102: the WHEN grammar (tools/goto/when.py)

SCHEMES = ('menu', 'match', 'wm30', 'caw', 'universe', 'cutscene', 'scene', 'moment')
WM30_MATCHES = 46
TOKEN = re.compile(r'^[A-Za-z0-9_.\-]+$')
SUFFIX = re.compile(r'@(bell|entrance:\d+|moment:\d+|scene:[\d,]*|after:[\d,]*|victory:[\d/]*)\s*$')
MAX_BANKS = 6      # the request record has six bank words (src/scene_director_args.h kMaxBanks)
MAX_LAYOUT = 17    # the victory chooser's performer-layout jump table has cases 0-17 (src/scene_director_args.h kMaxLayout)
MAX_ID = 65535     # src/scene_director_args.h kMaxId
MIN_CHAR, MAX_CHAR, MAX_ATTIRE, MAX_PERFORMERS = 100, 511, 255, 6  # src/scene_director_args.h kMinChar / kMaxChar / kMaxAttire / kMaxPerformers (S101 lane 38)
LOADABLE_ARENAS = tuple(list(range(0, 10)) + list(range(11, 18)) + list(range(21, 24)) + list(range(28, 57)) + list(range(66, 69)) + list(range(76, 81)))
# S101 lane 72: the arenas with a pac\bg\bgNN.pac (src/scene_stage_logic.h ArenaLoadable; the selftest compares this list with that header and with the game files)


_CHAR_IDS = None


def character_ids():
    """The stock character ids in db/ids/characters.json (S101 lane 38: a performer must be one of them; ids outside the table have no model key)."""
    global _CHAR_IDS
    if _CHAR_IDS is None:
        import json
        import os
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'db', 'ids', 'characters.json')
        with open(path, encoding='utf-8') as f:
            _CHAR_IDS = frozenset(int(e['id']) for e in json.load(f)['entries'])
    return _CHAR_IDS


class AddressError(ValueError):
    """A malformed address: the message says what is wrong."""


@dataclass(frozen=True)
class Address:
    scheme: str
    body: str                  # menu: the path; match: the spec; caw: CATEGORY/ITEM; universe / cutscene: the token; wm30: unused
    n: int = -1                # wm30: the match index
    moment: int = -1           # wm30: K (-1 = default X00)
    fire: int = -1             # S101 lane 108 wm30: @fire:BANK (-1 = none)
    fire_force: bool = False   # S101 lane 108 wm30: @fire:BANK/force
    step: int = -1             # S101 lane 121 wm30: @step:K (-1 = none)
    step_force: bool = False   # S101 lane 121 wm30: @step:K/force
    moment_bank: int = -1      # S101 lane 121 wm30: @moment:BANK, BANK 1000-9999 (the moment is the step that plays that bank)
    entrance: int = -1         # match: slot for @entrance:N (-1 = none)
    bell: bool = False         # match: @bell
    scene_ids: tuple = ()      # scene: (ID,); match: @scene:ID,ID (up to 6)
    victory: int = -1          # match: @victory:ID (-1 = none)
    layout: int = -1           # match: @victory:ID/L (-1 = not given)
    after: bool = False        # match: the scene_ids came from @after: (play at the match end) rather than @scene: (post now)
    frame: int = -1            # S101 lane31 moment: @frame:F (-1 = not given)
    event_kind: str = ''       # moment: @event:KIND
    event_n: int = 0           # moment: @event:KIND:N (0-based)
    performers: tuple = ()     # S101 lane 38 scene:ID:CHARS: ((character, attire), ...) attire -1 = not given
    arena: int = -1            # S101 lane 72 scene:ID@arena:N (-1 = not given: the stage's own, 76 = the empty stage)
    leave: bool = False        # S101 lane 72 scene:leave
    when: tuple = ()           # S101 lane 102: canonical WHEN clauses ('@pin', '@count:2-10f', ...) of a match: / moment: address, in order

    def __str__(self):
        if self.scheme == 'scene':
            if self.leave:
                return 'scene:leave'
            return ('scene:%d' % self.scene_ids[0] + ((':' + ','.join(('%d/%d' % (c, a)) if a >= 0 else '%d' % c for c, a in self.performers)) if self.performers else '') +
                    ('@arena:%d' % self.arena if self.arena >= 0 else ''))
        if self.scheme == 'wm30':
            return ('wm30:%d' % self.n + ('@moment:%d' % self.moment if self.moment >= 0 else '') +
                    (('@moment:%d' % self.moment_bank) if self.moment_bank >= 0 else '') +
                    (('@step:%d' % self.step + ('/force' if self.step_force else '')) if self.step >= 0 else '') +
                    (('@fire:%d' % self.fire + ('/force' if self.fire_force else '')) if self.fire >= 0 else ''))
        if self.scheme == 'moment':
            if self.when:
                return 'moment:%s%s' % (self.body, ''.join(self.when))
            if self.frame >= 0:
                return 'moment:%s@frame:%d' % (self.body, self.frame)
            return 'moment:%s@event:%s' % (self.body, self.event_kind) + (':%d' % self.event_n if self.event_n else '')
        if self.scheme == 'match':
            return ('match:%s' % self.body + ''.join(self.when) + ('@entrance:%d' % self.entrance if self.entrance >= 0 else '') + ('@bell' if self.bell else '') +
                    (('@after:%s' if self.after else '@scene:%s') % ','.join(str(i) for i in self.scene_ids) if self.scene_ids else '') +
                    (('@victory:%d' % self.victory + ('/%d' % self.layout if self.layout >= 0 else '')) if self.victory >= 0 else ''))
        return '%s:%s' % (self.scheme, self.body)


def parse(text):
    """-> Address, or raises AddressError with the reason."""
    if not isinstance(text, str) or not text.strip():
        raise AddressError('empty address')
    text = text.strip()
    if ':' not in text:
        raise AddressError('no scheme: expected one of %s followed by ":" (got "%s")' % (', '.join(SCHEMES), text))
    scheme, rest = text.split(':', 1)
    scheme = scheme.lower()
    if scheme not in SCHEMES:
        raise AddressError('unknown scheme "%s": expected one of %s' % (scheme, ', '.join(SCHEMES)))
    rest = rest.strip()
    if not rest:
        raise AddressError('%s: needs something after the colon' % scheme)

    if scheme == 'menu':
        if '@' in rest:
            raise AddressError('menu: takes no @ suffix (got "%s")' % rest)
        return Address('menu', rest)

    if scheme == 'match':
        entrance, bell = -1, False
        scene_ids, victory, layout, after = (), -1, -1, False
        order = []     # S101 lane 102: every wait clause (the new WHENs, @bell, @entrance) right to left
        while True:
            wm = _when.TAIL.search(rest)
            if wm:
                try:
                    wc = _when.parse(wm.group(0))
                except _when.WhenError as x:
                    raise AddressError('match: %s' % x)
                order.append(wc[0].canon())
                rest = rest[:wm.start()].rstrip()
                continue
            m = SUFFIX.search(rest)
            if not m:
                break
            word = m.group(1)
            rest = rest[:m.start()].rstrip()
            if word == 'bell' or word.startswith('entrance:'):
                order.append('@' + word)
            if word == 'bell':
                if bell:
                    raise AddressError('match: @bell twice')
                bell = True
            elif word.startswith('entrance:'):
                if entrance >= 0:
                    raise AddressError('match: @entrance twice')
                entrance = int(word.split(':')[1])
            elif word.startswith(('scene:', 'after:')):
                if scene_ids:
                    raise AddressError('match: @scene / @after twice')
                after = word.startswith('after:')
                parts = word.split(':', 1)[1].split(',')
                if not all(re.fullmatch(r'\d{1,5}', p) and 1 <= int(p) <= MAX_ID for p in parts):
                    raise AddressError('match: @scene: expects comma separated bank ids 1-%d (got "%s")' % (MAX_ID, word))
                if len(parts) > MAX_BANKS:
                    raise AddressError('match: @scene: at most %d banks (the request record has %d bank words)' % (MAX_BANKS, MAX_BANKS))
                scene_ids = tuple(int(p) for p in parts)
            elif word.startswith('victory:'):
                if victory >= 0:
                    raise AddressError('match: @victory twice')
                m2 = re.fullmatch(r'(\d{1,5})(?:/(\d{1,2}))?', word.split(':', 1)[1])
                if not m2 or not 1 <= int(m2.group(1)) <= MAX_ID:
                    raise AddressError('match: @victory: expects ID or ID/LAYOUT (got "%s")' % word)
                victory = int(m2.group(1))
                if m2.group(2) is not None:
                    layout = int(m2.group(2))
                    if layout > MAX_LAYOUT:
                        raise AddressError('match: @victory: the layout is 0-%d (got %d)' % (MAX_LAYOUT, layout))
            else:
                raise AddressError('match: @%s is a wm30 suffix' % word)
        if not rest:
            raise AddressError('match: empty spec')
        if '@' in rest:
            raise AddressError('match: unknown @ suffix in "%s" (known: @entrance:N, @bell, @scene:ID[,ID], @after:ID[,ID], @victory:ID[/L])' % rest)
        if entrance > 5:
            raise AddressError('match: @entrance:%d: slots are 0-5' % entrance)
        whens = ()
        if any(not (o == '@bell' or o.startswith('@entrance:')) for o in order):
            if scene_ids or victory >= 0:
                raise AddressError('match: a WHEN cannot be combined with @scene / @after / @victory (separate verbs)')
            whens = tuple(reversed(order))   # @bell / @entrance written beside a WHEN are clauses of it, in the order written
            entrance, bell = -1, False
        if (scene_ids or victory >= 0) and (entrance >= 0 or bell):
            raise AddressError('match: @scene / @victory cannot be combined with @entrance / @bell (the scene is posted in the running match)')
        if scene_ids and victory >= 0:
            raise AddressError('match: @scene and @victory are separate verbs: pick one')
        return Address('match', rest, entrance=entrance, bell=bell, scene_ids=scene_ids, victory=victory, layout=layout, after=after, when=whens)

    if scheme == 'scene':
        if rest.lower() == 'leave':  # S101 lane 72
            return Address('scene', 'leave', leave=True)
        arena = -1
        am = re.search(r'@arena:(\d+)\s*$', rest)
        if am:
            arena = int(am.group(1)) if len(am.group(1)) <= 3 else 9999
            if arena not in LOADABLE_ARENAS:
                raise AddressError('scene: arena %s has no BGnn.PAC in the game files (loadable: 0-9, 11-17, 21-23, 28-56, 66-68, 76-80; created arenas 200+ are refused)' % am.group(1))
            rest = rest[:am.start()].rstrip()
        ident, _, chars = rest.partition(':')
        if not re.fullmatch(r'\d{1,5}', ident) or not 1 <= int(ident) <= MAX_ID:
            raise AddressError('scene: expected one bank id, digits only, 1-%d (got "%s")' % (MAX_ID, rest))
        performers = []
        if ':' in rest:
            for part in chars.split(','):
                m = re.fullmatch(r'(\d{1,3})(?:/(\d{1,3}))?', part)
                if not m or not MIN_CHAR <= int(m.group(1)) <= MAX_CHAR:
                    raise AddressError('scene: performers are CHAR[/ATTIRE],... with a character id %d-%d (got "%s")' % (MIN_CHAR, MAX_CHAR, part))
                if int(m.group(1)) not in character_ids():
                    raise AddressError('scene: character %s is not in db/ids/characters.json' % m.group(1))
                attire = int(m.group(2)) if m.group(2) is not None else -1
                if attire > MAX_ATTIRE:
                    raise AddressError('scene: an attire is 0-%d (got "%s")' % (MAX_ATTIRE, part))
                performers.append((int(m.group(1)), attire))
            if len(performers) > MAX_PERFORMERS:
                raise AddressError('scene: at most %d performers (a scene context holds six)' % MAX_PERFORMERS)
        return Address('scene', rest, scene_ids=(int(ident),), performers=tuple(performers), arena=arena)

    if scheme == 'wm30':
        m = re.fullmatch(r'(\d+)(?:@moment:(\d+))?(?:@step:(\d{1,3})(/force)?)?(?:@fire:(\d{1,5})(/force)?)?', rest)
        if not m:
            raise AddressError('wm30: expected N, N@moment:K, N@moment:BANK, N@step:K[/force] or N[@moment:K]@fire:BANK[/force] (got "%s")' % rest)
        n = int(m.group(1))
        if n >= WM30_MATCHES:
            raise AddressError('wm30:%d: the match index is 0-%d' % (n, WM30_MATCHES - 1))
        k = int(m.group(2)) if m.group(2) is not None else -1
        bank = -1
        if 1000 <= k <= 9999:  # S101 lane 121: a scene bank (the Season banks are 1010-2110); SCEX codes are 0-2 or >= 10000
            bank, k = k, -1
        elif 3 <= k < 10000:
            raise AddressError('wm30: moment %d: use 0 / 1 / 2 (X00 / X01 / X02), a raw SCEX code >= 10000, or a scene bank 1000-9999 (the step that plays it)' % k)
        step = int(m.group(3)) if m.group(3) is not None else -1
        fire = int(m.group(5)) if m.group(5) is not None else -1
        if fire == 0 or fire > MAX_ID:
            raise AddressError('wm30: @fire: expects a scene bank id 1-%d (got "%s")' % (MAX_ID, m.group(5)))
        if fire >= 0 and k >= 0:
            raise AddressError('wm30: @fire: starts the match at its own X00; combining it with @moment is not supported')
        if (bank >= 0 or step >= 0) and k >= 0:
            raise AddressError('wm30: @step / @moment:BANK start the match at its own X00; combining them with an SCEX @moment is not supported')
        if sum(1 for x in (bank >= 0, step >= 0, fire >= 0) if x) > 1:
            raise AddressError('wm30: use one of @moment:BANK, @step:K, @fire:BANK')
        return Address('wm30', '', n=n, moment=k, fire=fire, fire_force=bool(m.group(6)), step=step, step_force=bool(m.group(4)), moment_bank=bank)

    if scheme == 'moment':
        # S101 lane 102: moment:NAME@WHEN... (a bus predicate over the moment's event index)
        wm = re.fullmatch(r'([A-Za-z0-9_.\-]+)(@(?:%s).*)' % '|'.join(_when.KINDS), rest)
        if wm:
            try:
                clauses = _when.parse(wm.group(2))
            except _when.WhenError as x:
                raise AddressError('moment: %s' % x)
            return Address('moment', wm.group(1), when=tuple(c.canon() for c in clauses))
        m = re.fullmatch(r'([A-Za-z0-9_.\-]+)@(?:frame:(\d+)|event:([A-Za-z0-9_.\-]+)(?::(\d+))?)', rest)
        if not m:
            raise AddressError('moment: expected NAME@frame:F or NAME@event:KIND[:N] (got "%s")' % rest)
        if m.group(2) is not None:
            if len(m.group(2)) > 9:
                raise AddressError('moment: frame %s is not a guest frame' % m.group(2))
            return Address('moment', m.group(1), frame=int(m.group(2)))
        return Address('moment', m.group(1), event_kind=m.group(3), event_n=int(m.group(4) or 0))

    if scheme == 'caw' and re.match(r'(?i)(tab:|page:|row:|back$)', rest):   # S101 lane 118: the editor driver's verbs (tools/n2/caw_driver.py parse_verb)
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'n2'))
        import caw_driver as _caw
        try:
            _caw.parse_verb(rest)
        except _caw.CawError as x:
            raise AddressError('caw: %s' % x)
        if not re.fullmatch(r'[A-Za-z0-9_.\-# :/]+', rest):
            raise AddressError('caw: names may hold letters, digits, space and _ . - # only (got "%s")' % rest)
        return Address('caw', rest)
    if scheme == 'caw':
        parts = rest.split('/')
        if len(parts) != 2 or not all(TOKEN.match(p) for p in parts):
            raise AddressError('caw: expected CATEGORY/ITEM (letters, digits, _ . -), got "%s"' % rest)
        return Address('caw', rest)

    # universe / cutscene: one token
    if not TOKEN.match(rest):
        raise AddressError('%s: expected one token (letters, digits, _ . -), got "%s"' % (scheme, rest))
    return Address(scheme, rest)


def main(argv):
    bad = 0
    for a in argv or ['wm30:5@moment:1', 'menu:play > one on one']:
        try:
            print('%s  ->  %s' % (a, parse(a)))
        except AddressError as x:
            bad += 1
            print('%s  ->  REFUSED: %s' % (a, x))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
