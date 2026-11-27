r"""S110 SCENEMOMENTS (docs\research\s110\SCENEMOMENTS.md): the match moments a scene request waits on through the EVENT BUS, and the director recipe
that makes a CPU match reach them. tools\run\scene.py calls parse() for an `at` word whose name is in HEAD, then wait_line(), expect() and recipe().

The route line is `wait_event KIND [WORD] [KEY=VALUE ...] TIMEOUT` (src\input_script.h, src\event_wait_core.h ParseSpec; the kinds and their
fields are the table at the top of src\event_bus.h). The game prints each bus event as ONE line `[wwe2k14][event] {"f": F, "t": T, "kind": K, FIELDS}`
(src\event_bus.h Emit :130-134): that line is the `# expect:` text. The runner's own `[input] line N: event ... after` line is not, because
tools\bench\run_read.py ignores the route's own lines for --expect (ECHO, run_read.py).

    at pin                      wait_event pin                      the referee's count started (judge [0x83767B6C]+72 -> 1)
    at count[=N]                wait_event count [count=N]          count 1..3
    at kickout[=N]              wait_event kickout [at_count=N]     the count stopped below 3 with no finish (emitted 20 frames late)
    at finish=NAME              wait_event finish finish_name=NAME  pinfall | count_out | dq | submission | last_one_left | time_limit | inferno | table
                                                                    (bare `at finish` stays the finish word, an in-route state: tools\run\scene.py)
    at move[:KEY=V ...]         wait_event move [KEY=V ...]         KEY slot | id | waza | move | class (signature | finisher)
    at eliminated[:KEY=V ...]   wait_event eliminated [KEY=V ...]   KEY slot | id | over_the_top; needs type= (a match that eliminates)
    at replay / replay_end      wait_event replay start | end       the PRTI replay track begins / ends playback
    at ring_break               wait_event break ring               the OMG ring break: served only after the request's own director lines
    at table_break              wait_event break table              a table's break latch (src\table_break.h, S110 TABLEEVENT); needs type= (Tables: 68)

S110 SCENEMOM2 (docs\research\s110\SCENEMOM2.md): every moment a finish makes impossible ends its wait at the finish, `... 72000 until finish`
(src\event_wait_core.h ParseSpec, S110 SCENESRC): the route FAILS at that line with the finish's fields, so a match that ends another way is
NOT ARRIVED at once instead of a 72000-frame blind wait. replay / replay_end come after the finish and wait without it.
"""
import re

WAIT = 72000   # frames: the bound of every bus moment, the finish word's own (tools\run\scene.py wait_lines 'finish')
# S110 SCENEMOM2: the end clause of a moment the finish makes impossible (S110 SCENESRC `until`, src\input_script.h). The moments that
# come AFTER the finish (the replay track: scene.py RANK puts them with the finish) wait without it: the mark does not move on an end, so
# `until finish` would end them at once on the finish already logged.
UNTIL = 'until finish'
AFTER_FINISH = ('replay', 'replay_end')
# the wait_event head per moment name (src\event_wait_core.h kMatchWhats: a match what may be the kind; `replay WORD` is state=WORD, `break WORD` what=WORD)
HEAD = {'pin': 'pin', 'count': 'count', 'kickout': 'kickout', 'finish': 'finish', 'move': 'move', 'eliminated': 'eliminated', 'replay': 'replay start',
        'replay_end': 'replay end', 'ring_break': 'break ring', 'table_break': 'break table'}
# the bus line's own text (src\event_bus.h :288 kickout, :299 pin, :308 count, :326 eliminated, :342 break, :397 finish, :442 replay;
# src\event_bus_rumble.h MoveLatch move, emitted from src\move_log_hook.h at the motion start); fields after the head, in the order the line prints them
LINE = {'pin': '"kind": "match", "what": "pin"', 'count': '"kind": "match", "what": "count"', 'kickout': '"kind": "match", "what": "kickout"',
        'eliminated': '"kind": "match", "what": "eliminated"', 'move': '"kind": "move"', 'replay': '"kind": "replay", "state": "start"',
        'replay_end': '"kind": "replay", "state": "end"', 'ring_break': '"kind": "break", "what": "ring"',
        'table_break': '"kind": "break", "what": "table"'}   # src\table_break_core.h Fields: "what" first
# `NAME=VALUE`: the field the value names; `NAME:KEY=V`: the fields a moment takes (an absent field never matches: src\event_wait_core.h CondHolds)
MAIN = {'count': 'count', 'kickout': 'at_count', 'finish': 'finish_name'}
FIELDS = {'count': ('count',), 'kickout': ('at_count',), 'finish': ('finish_name',), 'move': ('slot', 'id', 'waza', 'move', 'class'),
          'eliminated': ('slot', 'id', 'over_the_top'), 'pin': ('pinning_slot',)}
# src\event_bus.h FinishName :207-217: the route word (a route word has no spaces; '_' matches a space: src\event_wait_core.h Norm) -> the bus spelling
FINISH_NAMES = {'pinfall': 'pinfall', 'count_out': 'count-out', 'count-out': 'count-out', 'dq': 'disqualification', 'disqualification': 'disqualification',
                'submission': 'submission', 'last_one_left': 'last one left', 'time_limit': 'time limit', 'inferno': 'inferno',
                'table': 'table'}   # type 2 under a table rule (src\event_bus.h, S110 TABLEEVENT)
FINISH_WORDS = 'pinfall | count_out | dq | submission | last_one_left | time_limit | inferno | table'
# S110 TOOLFIT (docs\research\s110\TOOLFIT.md 1): the bus spelling -> the finish_type its line prints (src\event_bus.h FinishName :220-231; type 2's
# two names: src\table_break_core.h Kind1Name :71). One type per name, so the finish line's text from "what" to winner_slot is one substring that
# a multi-fall rule's `fall` line (S110 ELIMFIN, src\event_bus.h :421, the same record after "what": "fall") never holds. scene_selftest pins this
# table against FinishName's text.
FINISH_TYPES = {'pinfall': 1, 'count-out': 2, 'table': 2, 'disqualification': 3, 'submission': 6, 'last one left': 7, 'time limit': 9, 'inferno': 10}
MOVE_CLASSES = ('signature', 'finisher')   # class normal is emitted only with --wwe2k14_events_moves=all (src\event_bus.h): not offered
# The finish recipe (docs\research\s110\BUSWAIT.md 8, tools\input_scripts\short\s110_buswait_check.txt; run 243 reached pin, count, finish and
# replay start / end with it): the seat S full momentum and one finisher stock, the other seat passive. {s} / {o}: the seat and the other one.
RECIPE_FINISH = ('momentum {s} 100', 'stock {s} 1', 'passive {o} on')
# S110 FINPICK (docs\research\s110\FINPICK.md): the seat's finisher wish (src\finisher_pick.h: the hook of the game's pair write sub_825FDE00 keeps
# the slot of 17 / 18 whose live move is / is not in the game's hold range 58000..58398). pin / count / finish=pinfall want `nohold`,
# finish=submission wants `hold`; that a hold ends by submission is [L] (FINPICK.md 4). finisher_fit_S is valid from the verb's `ok`
# (-1 no wish, 1 fits, 0 none fits, 2 a hold fits only in a posture family: S110 TOOLFIT): the route waits for it and asserts 1, so a seat with no
# fitting finisher, or only a family hold (the recipe sets no posture), fails right after the bell (tools\scene\go.py fit_failed says which).
# Only for an exe that holds the state names (FIT_PROBE, tools\run\scene.py lower): an older exe keeps the recipe without the wish.
# Run 282: the state-name prefix 'finisher_fit_' is NOT in the exe as text (a 13-character std::string is built in place by the compiler), so the
# probe is the verb's own log format, a long literal of src\finisher_pick.h that is.
FIT_PROBE = '[wwe2k14][finisher]'
WISH_LINES = ('finisher {s} {kind}', 'wait_state finisher_fit_{s} >=0 600 1', 'assert_state finisher_fit_{s} 1')
# S110 OMGVERB (docs\research\s110\OMGVERB.md): the ring break is the game's own superplex act 2101, chosen by sub_825FCEB8 at a finisher
# candidate when the attacker (seat 0 here) is a super heavyweight (roster class 4), holds 3 stocks (its cost), is in the ring within 180 of a
# target in a corner-groggy act (150 / 167 / 630) and his meter is NOT full (so no `momentum 100`). The `omg` verb presses for the seat when
# that test passes every gate (src\omg_wish.h); omg_fit_S is valid from the verb's ok: -1 none, 0 a gate fails, 1 armed, 2 the request taken
# (kind 4), 3 the follow-up's test started the break, 4 reversed, 5 the follow-up's test refused, 6 the follow-up gone without its test
# (S110 OMGVERB fix 2, run 315: a request is not a break). The route waits for >= 3 and asserts 3: a 4 / 5 / 6 fails at once, and the hook's
# `[wwe2k14][omg]` line says why. The corner set-up is the dev verb `state {o} CornerGroggy` (the game's own state door; act 150, Lua
# StateCornerGroggy) once the CPU is within 160 (0x836F38D0>2812 < 160.0f = 1126170624): so the recipe needs a DEV exe with the verb.
# OMG_PROBE: a text only an exe with the fix-2 hook holds (its follow-up line, src\omg_wish_core.h FollowUpText), so an older exe gets no recipe.
OMG_PROBE = "the follow-up's ring-break test (FRA kind 8, its own press)"
RING_LINES = ('wait_state 0x836F38D0>2812 <1126170624 3600 2', 'passive {o} on', 'stock {s} 3', 'omg {s} ring', 'wait_state omg_fit_{s} >=0 600 1',
              'assert_state omg_fit_{s} 1', 'state {o} CornerGroggy', 'wait_state omg_fit_{s} >=3 3600 1', 'assert_state omg_fit_{s} 3')
# the catalogue's words for each moment (tools\scene\catalogue.py reads this dict with ast: keep it a literal)
USAGE = {'pin': ('pin', 'wait_event pin until finish; default recipe: the finish recipe'),
         'count': ('count[=N]', 'wait_event count [count=N] until finish; default recipe: the finish recipe'),
         'kickout': ('kickout[=N]', 'wait_event kickout [at_count=N] until finish; no recipe (the CPU match\'s own pins)'),
         'finish': ('finish=NAME', 'wait_event finish finish_name=NAME until finish; default recipe for pinfall (and submission, on an exe with the `finisher` verb): the finish recipe with the seat\'s finisher wish; bare `finish` is the in-route finish word'),
         'move': ('move[:KEY=V ...]', 'wait_event move [slot= id= waza= move= class=signature|finisher] until finish; default recipe: the finish recipe'),
         'eliminated': ('eliminated[:KEY=V ...]', 'wait_event eliminated [slot= id= over_the_top=] until finish; needs type= (a match that eliminates); no recipe'),
         'replay': ('replay', 'wait_event replay start; default recipe: the finish recipe'),
         'replay_end': ('replay_end', 'wait_event replay end; default recipe: the finish recipe'),
         'ring_break': ('ring_break', 'wait_event break ring until finish; default recipe on a dev exe with the `omg` verb: seat 0 (a super heavyweight, roster class 4, named in WHO) 3 stocks, `omg 0 ring`, the target passive and put in the corner-groggy state; else the request\'s own director lines'),
         'table_break': ('table_break', 'wait_event break table until finish; needs type= (a Tables match, one on one: type=68); no recipe (no director verb breaks a table)')}
VALUE_RX = re.compile(r'^[A-Za-z0-9_\-]+$')


class Refused(Exception):
    """The moment cannot be served: scene.py turns it into its own Refused with the line number."""


def parse(word):
    """`count=2` / `move:class=finisher:slot=0` / `finish=pinfall` / `pin` -> (name, ((key, value), ...)). Refused on a bad word."""
    parts = word.split(':')
    name, eq, main = parts[0].partition('=')
    name = name.lower()
    if name not in HEAD:
        raise Refused('"%s" is not a bus moment' % word)
    conds = []
    if eq:
        if name not in MAIN:
            raise Refused('moment "%s": %s takes no `=VALUE` (%s)' % (word, name, USAGE.get(name, (name,))[0]))
        conds.append((MAIN[name], main))
    elif name == 'finish':
        raise Refused('moment "%s": finish=NAME (%s)' % (word, FINISH_WORDS))
    for p in parts[1:]:
        k, eq2, v = p.partition('=')
        if not eq2 or k.lower() not in FIELDS.get(name, ()):
            raise Refused('moment "%s": `:%s` is not a field of %s (fields: %s)' % (word, p, name, ', '.join(FIELDS.get(name, ())) or 'none'))
        conds.append((k.lower(), v))
    seen = set()
    for k, v in conds:
        if k in seen:
            raise Refused('moment "%s": %s given twice' % (word, k))
        seen.add(k)
        if not VALUE_RX.match(v):
            raise Refused('moment "%s": %s=%s (letters, digits, _ and -)' % (word, k, v))
        if k in ('count', 'at_count') and v not in ('1', '2', '3'):
            raise Refused('moment "%s": %s=%s (the referee counts 1..3)' % (word, k, v))
        if k == 'slot' and not (v.isdigit() and int(v) <= 8):
            raise Refused('moment "%s": slot=%s (0-8)' % (word, v))
        if k == 'finish_name' and v.lower() not in FINISH_NAMES:
            raise Refused('moment "%s": finish=%s (%s)' % (word, v, FINISH_WORDS))
        if k == 'class' and v.lower() not in MOVE_CLASSES:
            raise Refused('moment "%s": class=%s (signature | finisher; normal moves are emitted only with --wwe2k14_events_moves=all)' % (word, v))
    conds = [(k, FINISH_NAMES[v.lower()].replace(' ', '_') if k == 'finish_name' else v.lower() if k == 'class' else v) for k, v in conds]
    return name, tuple(conds)


def is_bus(moment):
    """A parsed moment tuple of this module: (name, ((k, v), ...)); bare `finish` is ('finish',) and is not."""
    return len(moment) == 2 and moment[0] in HEAD and isinstance(moment[1], tuple)


def wait_line(moment):
    """`wait_event HEAD [K=V ...] 72000 until finish` (S110 SCENEMOM2), without the end clause for a moment after the finish."""
    name, conds = moment
    return 'wait_event %s%s %d%s' % (HEAD[name], ''.join(' %s=%s' % kv for kv in conds), WAIT, '' if name in AFTER_FINISH else ' ' + UNTIL)


def expect(moment):
    """The game line's text (one substring of one bus line) the arrival must show."""
    name, conds = moment
    c = dict(conds)
    if name == 'finish':   # S110 TOOLFIT: from "what" to winner_slot (FINISH_TYPES): a `fall` line has the same finish_name but "what": "fall"
        n = c['finish_name'].replace('_', ' ')
        return '"what": "finish", "finish_type": %d, "finish_name": "%s", "winner_slot": ' % (FINISH_TYPES[n], n)
    text = LINE[name]
    if name == 'count' and 'count' in c:
        text += ', "count": %s' % c['count']
    if name == 'kickout' and 'at_count' in c:
        text += ', "at_count": %s' % c['at_count']
    if name == 'move' and 'slot' in c:   # not eliminated: a Rumble's line prints "n" before "slot" (src\event_bus_rumble.h); the wait filters the slot
        text += ', "slot": %s' % c['slot']
    return text


def recipe(moment, wish=False, omg=False):
    """-> (the director lines that make a CPU match reach the moment, why), or (None, why) when no recipe reaches it. `wish` (S110 FINPICK): the
    exe has the `finisher` verb: pin / count / finish=pinfall add `finisher S nohold`, finish=submission gets the finish recipe with `finisher S hold`.
    `omg` (S110 OMGVERB): a dev exe with the `omg` verb: ring_break gets RING_LINES."""
    name, conds = moment
    c = dict(conds)
    fin = c.get('finish_name') if name == 'finish' else None
    if name in ('kickout', 'eliminated'):
        return None, 'no recipe: the match\'s own %s' % ('pins (a kickout is the pinned seat\'s)' if name == 'kickout' else 'eliminations')
    if name == 'finish' and fin != 'pinfall' and not (wish and fin == 'submission'):
        return None, 'no recipe: the finish recipe ends the match by pinfall; a %s finish is the match\'s own' % c.get('finish_name')
    if name == 'ring_break' and omg:
        return [r.format(s=0, o=1) for r in RING_LINES], ('the ring-break recipe (OMGVERB.md): seat 0 must be a super heavyweight (roster class 4); '
                                                         '3 stocks, the omg wish, the target passive in the corner-groggy state once within 160')
    if name == 'ring_break':
        return None, ('no recipe reaches the OMG ring break on this exe (the recipe needs a dev exe with the `omg` verb and the dev `state` verb: '
                      'docs\\research\\s110\\OMGVERB.md)')
    if name == 'table_break':
        return None, ('no recipe: the Tables match\'s own course (CPU vs CPU, tools\\input_scripts\\short\\s110_table_acts.txt); no director verb '
                      'puts a wrestler through a table (the missing verb: docs\\research\\s110\\SCENEMOM2.md 3)')
    s = int(c['slot']) if name == 'move' and 'slot' in c else 0
    o = 1 if s == 0 else 0
    lines = [r.format(s=s, o=o) for r in RECIPE_FINISH]
    why = 'the finish recipe (BUSWAIT.md 8, run 243): seat %d momentum 100 + one stock, seat %d passive' % (s, o)
    kind = 'hold' if fin == 'submission' else ('nohold' if name in ('pin', 'count') or fin == 'pinfall' else None)
    if wish and kind:
        lines += [w.format(s=s, kind=kind) for w in WISH_LINES]
        why += '; seat %d\'s finisher wish %s (FINPICK.md: the game keeps the slot whose move fits; a hold ends by submission [L])' % (s, kind)
    return lines, why
