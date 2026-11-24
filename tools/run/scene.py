r"""S109 SCENE: a check as a short scene request, lowered to a route (design: docs\research\s109\SCENE_plan.md; contract: docs\systems\run_contract.md "Scene requests").

    py -I tools\run\scene.py --write REQUEST|FILE [--exe EXE] [--data NAME]   write out\scenes\<item word>_<slug>_<hash8>.txt, print its path (exit 2 + REFUSED: why)
    py -I tools\run\scene.py --print REQUEST|FILE [--exe EXE]                  print the route, write nothing

A request is lines (a file, or one argument whose lines are joined by ` // `; a leading `scene:` is dropped):
    scene  WHO [; arena=A] [; type=T] [; pace=P] [; entrances=on|off] [; blood=on|off] [; dlc=on] [; save=NAME] [; player=SEAT[,SEAT]]
    at     MOMENT[+Nf] : ITEM [; ITEM ...]       lines run in order; an offset counts from the line's moment
    run    [exe=PATH] [intent=TERMS] [flags=--a=1 ...]
  WHO      names or #ids in seat order joined by `vs` or `,`; `/N` after one = its attire (0-9). Names go through db\ids (db\ids\dlc_characters.json with dlc=on).
           WHO may be empty for the launcher (the captured scenario's own wrestlers). Every seat is a CPU unless `player=SEAT` names it (pad 0, 1, ... in the order given).
  MOMENT   bell | roster | entrance:S[:STEP] (slot S 0-5, STEP default 1) | finish | victory | clock:N | seq:N | load:FILE
  ITEM     actions: passive S on|off|all on|off, momentum S PCT, stock S N, bleed S LEVEL, lightsout S, passive_cpu on|off
           takes:   screenshot L | shot face|wrestler|stage|prop N [L] | record L | every Nf xK L | log STATE
Start door (SCENE_plan.md section 7 choice 2a): `blood=` -> the match builder (`cvar wwe2k14_match`), which since S109 ENT also takes `entrances=off`
(spec `entrances=0`) and an attire (`p=ID:PAD:N`) (docs\research\s109\ENT_plan.md); everything else -> the launcher (`--wwe2k14_launch=lesnar_bryan`, or
`@quick` with type=), unchanged. A combination neither door serves is never silently reduced: every field is placed or the request is refused with the
reason. A route the lint refuses fast-forwarded is refused here with the lint's own text.
"""
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
for _p in ('tools/goto', 'tools/atlas', 'tools/bench', 'tools/run', 'tools/board'):
    sys.path.insert(0, os.path.join(ROOT, *_p.split('/')))
import when as _when       # noqa: E402  the WHEN grammar's clause names (tools\goto\when.py)
import match as _match     # noqa: E402  names -> ids, the type words (tools\atlas\match.py)

OUT_DIR = os.path.join(ROOT, 'out', 'scenes')
BASE_FLAGS = ['--wwe2k14_fixed_seed=1', '--vulkan_stored_pipelines_at_boot=false', '--wwe2k14_events=true', '--wwe2k14_rdr_owner=true', '--audio_mute=true']
SCENE_KEYS = ('arena', 'type', 'pace', 'entrances', 'blood', 'dlc', 'save', 'player')
BUILD_KEYS = ('type', 'p', 'arena', 'pace', 'o', 'entrances')   # the builder spec's keys a request fills, in spec order (src\build_spec.h)
# strings only an exe built with S109 ENT holds (the wwe2k14_match help text, src\scenario.h): an older builder refuses these specs in the game
# (`[build] spec ... refused`) and the route would wait for a match that never starts
ENT_MARK_ENTRANCES, ENT_MARK_ATTIRE = 'entrances=0 skips the entrances', 'p=ID:PAD:ATTIRE'
BUS_ONLY = ('pin', 'count', 'kickout', 'eliminated', 'remaining', 'entered', 'move', 'code', 'playing')   # event bus kinds with no in-route wait (SCENE_plan.md section 2)
GOLDEN_WAIT = {'launcher': ('INGAMELUA.PAC', 1800), 'bell_launcher': 30000, 'bell_builder': 40000}
RANK = {'entrance': 1, 'seq': 1, 'load': 1, 'bell': 2, 'roster': 3, 'clock': 4, 'finish': 5, 'victory': 6}
ITEM_WORD = {'victory': 'victory', 'entrance': 'entrance', 'match': 'match'}      # the route name's word -> the board item it must file under
ITEM_BOARD = {'victory': 'victory', 'entrance': 'entrances', 'match': 'matches'}   # tools\board\coverage.py ITEM_RULES
LABEL_RX = re.compile(r'^[A-Za-z0-9_.\-]+$')
STATE_RX = re.compile(r'^(0x[0-9A-Fa-f]{1,8}|[A-Za-z_][A-Za-z0-9_]*)$')


class Refused(Exception):
    """The request cannot be served: the message is the reason (printed as `REFUSED: ...`)."""


# ------------------------------------------------------------------------------------------------ parsing
def normalise(text):
    """request text (file lines, or one argument joined by ` // `) -> [line] with comments / blanks dropped, `scene:` prefix dropped, `%23` -> `#`."""
    t = text.strip()
    if t.lower().startswith('scene:'):
        t = 'scene ' + t[6:].strip()
    lines = []
    for ln in re.split(r'\r?\n|\s//\s', t):
        ln = ln.strip()
        if ln and not ln.startswith('#'):
            lines.append(ln.replace('%23', '#'))
    return lines


def canonical(lines):
    return ' // '.join(lines)


def parse_request(text):
    lines = normalise(text)
    if not lines:
        raise Refused('empty request')
    scene, ats, run = None, [], {}
    for n, ln in enumerate(lines, 1):
        word, _, rest = ln.partition(' ')
        w = word.lower()
        if w == 'scene':
            if scene is not None:
                raise Refused('line %d: a second `scene` line (one scene per request)' % n)
            scene = parse_scene(rest)
        elif w == 'at':
            ats.append(parse_at(rest, n))
        elif w == 'run':
            for k, v in parse_run(rest).items():
                if k in run:
                    raise Refused('line %d: run %s given twice' % (n, k))
                run[k] = v
        else:
            raise Refused('line %d: "%s" is not scene | at | run' % (n, word))
    if scene is None:
        raise Refused('no `scene` line')
    return {'scene': scene, 'at': ats, 'run': run, 'lines': lines}


def parse_scene(rest):
    who, f = '', {}
    for part in [p.strip() for p in rest.split(';')]:
        if not part:
            continue
        if '=' not in part:
            if who:
                raise Refused('scene: two WHO parts ("%s" and "%s")' % (who, part))
            who = part
            continue
        k, v = [x.strip() for x in part.split('=', 1)]
        k = k.lower()
        if k not in SCENE_KEYS:
            raise Refused('scene: unknown field "%s" (fields: %s)' % (k, ', '.join(SCENE_KEYS)))
        if k in f:
            raise Refused('scene: %s given twice' % k)
        f[k] = v
    for k in ('entrances', 'blood', 'dlc'):
        if k in f and f[k].lower() not in ('on', 'off'):
            raise Refused('scene: %s=%s (on | off)' % (k, f[k]))
        if k in f:
            f[k] = f[k].lower()
    return {'who': who, 'f': f}


def parse_at(rest, n):
    mm = re.match(r'^\s*([A-Za-z]+(?::[^\s:+]+)*)(?:\+(\d+)f)?\s*:\s*(.*)$', rest)
    if not mm:
        raise Refused('line %d: at MOMENT[+Nf]: ITEM[; ITEM] (got "at %s")' % (n, rest))
    word, off, items = mm.group(1), int(mm.group(2) or 0), mm.group(3)
    return {'n': n, 'moment': parse_moment(word, n), 'off': off, 'items': [parse_item(i.strip(), n) for i in items.split(';') if i.strip()], 'text': rest}


def parse_moment(word, n):
    parts = word.split(':')
    name = parts[0].lower()
    if name in ('highlight', 'replay', 'highlightreplay'):
        raise Refused('line %d: moment "%s": the post-match highlight replay has no in-route sync today (SCENE_plan.md section 2: [L], not offered)' % (n, word))
    if name in BUS_ONLY:
        raise Refused('line %d: moment "%s": bus only: no in-route state (a route\'s wait_event knows only `render`; goto\'s @%s waits host-side on the event bus). '
                      'Use a frame count after the nearest state, e.g. `bell+Nf`' % (n, word, name))
    if name not in RANK:
        try:
            _when.parse('@' + word)
            known = False
        except _when.WhenError:
            known = None
        raise Refused('line %d: moment "%s" %s (moments: bell, roster, entrance:S[:STEP], finish, victory, clock:N, seq:N, load:FILE)'
                      % (n, word, 'has no in-route state' if known is False else 'is not a moment'))
    if name == 'finish' and len(parts) > 1:
        raise Refused('line %d: moment "%s": bus only: the finish kind (pinfall, submission ...) is on the event bus, not in a route; `finish` alone waits on the finish word' % (n, word))
    if name in ('bell', 'roster', 'finish', 'victory'):
        if len(parts) > 1:
            raise Refused('line %d: moment %s takes no argument (got "%s")' % (n, name, word))
        return (name,)
    if name == 'entrance':
        if len(parts) not in (2, 3) or not parts[1].isdigit() or (len(parts) == 3 and not parts[2].isdigit()):
            raise Refused('line %d: entrance:S[:STEP] with numbers (got "%s")' % (n, word))
        s, step = int(parts[1]), int(parts[2]) if len(parts) == 3 else 1
        if s > 5:
            raise Refused('line %d: entrance slot %d: states.toml has entrance_step_0 .. entrance_step_5 only' % (n, s))
        return ('entrance', s, step)
    if name in ('clock', 'seq'):
        if len(parts) != 2 or not parts[1].isdigit():
            raise Refused('line %d: %s:N with a number (got "%s")' % (n, name, word))
        return (name, int(parts[1]))
    if len(parts) != 2 or not re.fullmatch(r'[A-Za-z0-9_.\-]+', parts[1]):
        raise Refused('line %d: load:FILE (a file name part, got "%s")' % (n, word))
    return ('load', parts[1])


def parse_item(text, n):
    w = text.split()
    if not w:
        raise Refused('line %d: empty item' % n)
    v = w[0].lower()

    def need(ok, usage):
        if not ok:
            raise Refused('line %d: "%s": %s' % (n, text, usage))
    slot = lambda t: t.isdigit() and len(t) == 1 and t <= '8'
    if v == 'passive':
        need(len(w) == 3 and (slot(w[1]) or w[1] == 'all') and w[2] in ('on', 'off'), 'passive SLOT|all on|off (SLOT 0-8)')
        return ('action', ' '.join(w))
    if v == 'passive_cpu':
        need(len(w) == 2 and w[1] in ('on', 'off'), 'passive_cpu on|off')
        return ('action', ' '.join(w))
    if v == 'momentum':
        need(len(w) == 3 and slot(w[1]) and w[2].isdigit() and int(w[2]) <= 100, 'momentum SLOT PCT (SLOT 0-8, PCT 0-100)')
        return ('action', ' '.join(w))
    if v == 'stock':
        need(len(w) == 3 and slot(w[1]) and w[2].isdigit() and len(w[2]) == 1, 'stock SLOT N (SLOT 0-8, N 0-9)')
        return ('action', ' '.join(w))
    if v == 'bleed':
        need(len(w) == 3 and slot(w[1]) and w[2] in ('1', '2', '3'), 'bleed SLOT LEVEL (LEVEL 1-3)')
        return ('bleed', ' '.join(w))
    if v == 'lightsout':
        need(len(w) == 2 and slot(w[1]), 'lightsout SLOT')
        return ('dev', ' '.join(w))   # a dev-build verb like bleed, but not a blood one (src\situation_director_core.h:119-123)
    if v == 'screenshot':
        need(len(w) == 2 and LABEL_RX.match(w[1]), 'screenshot LABEL')
        return ('take', 'screenshot ' + w[1])
    if v == 'record':
        need(len(w) == 2 and LABEL_RX.match(w[1]), 'record LABEL')
        return ('take', 'record ' + w[1])
    if v == 'shot':
        need(len(w) in (3, 4) and w[1] in ('face', 'wrestler', 'stage', 'prop') and w[2].isdigit() and (len(w) == 3 or LABEL_RX.match(w[3])),
             'shot face|wrestler|stage|prop N [LABEL]')
        return ('take', ' '.join(w))
    if v == 'every':
        m = re.fullmatch(r'every\s+(\d+)f\s+x(\d+)\s+(\S+)', text, re.I)
        need(bool(m) and int(m.group(1)) > 0 and int(m.group(2)) > 0 and LABEL_RX.match(m.group(3)), 'every Nf xK LABEL (N, K > 0)')
        return ('every', int(m.group(1)), int(m.group(2)), m.group(3))
    if v == 'log':
        need(len(w) == 2 and STATE_RX.match(w[1]), 'log STATE (a states.toml name or 0xADDR)')
        return ('take', 'log_state ' + w[1])
    if v in BUS_ONLY or v in ('pin', 'count', 'kickout', 'finisher', 'elimination', 'eliminate'):
        raise Refused('line %d: "%s": bus only: no in-route verb or state for it' % (n, text))
    raise Refused('line %d: unknown item "%s" (actions: passive, momentum, stock, bleed, lightsout, passive_cpu; takes: screenshot, shot, record, every, log)' % (n, w[0]))


def parse_run(rest):
    out = {}
    # fields are key=value; flags= and intent= take the rest of the field up to the next ` key=` word
    parts = re.split(r'\s+(?=(?:exe|intent|flags|timeout)=)', rest.strip())
    for p in parts:
        if not p:
            continue
        k, eq, v = p.partition('=')
        k = k.strip().lower()
        if not eq or k not in ('exe', 'intent', 'flags', 'timeout'):
            raise Refused('run: exe=PATH | intent=TERMS | flags=--a=1 ... (got "%s")' % p)
        out[k] = v.strip()
    return out


# ------------------------------------------------------------------------------------------------ resolving
def _characters(dlc):
    ents = list(_match.load('characters.json'))
    if dlc:
        try:
            ents += [e for e in _match.load('dlc_characters.json')]
        except (OSError, KeyError):
            pass
    return [e for e in ents if e.get('kind', 'wrestler') in ('wrestler', 'diva', None)]


def resolve_who(who, dlc):
    """-> [(id, attire or None)]. Names through db\\ids (the DLC list only with dlc=on); a candidate list on an unknown name."""
    if not who.strip():
        return []
    chars = _characters(dlc)
    out = []
    for tok in [t for t in re.split(r'\s*,\s*|\s+vs\.?\s+', who.strip(), flags=re.I) if t.strip()]:
        att = None
        m = re.match(r'^(.*?)\s*/\s*(\S+)$', tok)
        if m:
            tok, a = m.group(1), m.group(2)
            if not re.fullmatch(r'\d', a):
                raise Refused('attire "%s" of "%s": attire 0-9 (the `N/S` colour sets and `s` are not in the request grammar; colour sets are launcher only)' % (a, tok))
            att = int(a)
        try:
            out.append((_match.pick(tok, chars, 'wrestler'), att))
        except _match.SpecError as x:
            hint = '' if dlc else ' (a DLC wrestler needs dlc=on: its names are read from db\\ids\\dlc_characters.json then)'
            raise Refused('WHO: %s%s' % (x, hint))
    return out


def resolve_players(f, n):
    if 'player' not in f:
        return {}
    pads = {}
    for t in [x.strip() for x in f['player'].split(',') if x.strip()]:
        if not t.isdigit() or int(t) >= n:
            raise Refused('player=%s: a seat number 0-%d (there are %d seats)' % (f['player'], max(n - 1, 0), n))
        if int(t) in pads:
            raise Refused('player=%s names seat %s twice' % (f['player'], t))
        pads[int(t)] = len(pads)
    if len(pads) > 4:
        raise Refused('player=%s: at most 4 controllers' % f['player'])
    return pads


# ------------------------------------------------------------------------------------------------ the lowering
class Plan:
    def __init__(self):
        self.door = ''          # launcher | builder
        self.flags = []         # the header flag line's tokens
        self.intent = []
        self.start = []         # route steps from the main-menu start to the match running
        self.cvars_needed = []  # (cvar, why): the exe must define them


def choose_door(req, ids):
    """blood= -> the match builder (it serves attire N and entrances=off too since S109 ENT: src\\build_spec.h); otherwise the launcher, as before."""
    return 'builder' if 'blood' in req['scene']['f'] else 'launcher'


def numeric_spec(req, ids, pads, dlc):
    """The numeric builder spec through the atlas's own grammar (type words, limits): -> dict type/p/arena/pace/o as strings; Refused on a limit."""
    f = req['scene']['f']
    parts = [' vs '.join('#%d' % i for i, _ in ids)] if ids else []
    parts.append('players=0')
    if not ids and 'type' not in f:
        parts.append('type=0')   # the atlas grammar wants a type for a spec without names; with no WHO nothing is built from it (the launcher's own scenario)
    for k in ('type', 'arena', 'pace'):
        if k in f:
            parts.append('%s=%s' % (k, f[k]))
    if f.get('blood'):
        parts.append('o=81:%d' % (1 if f['blood'] == 'on' else 0))
    try:
        spec, _ = _match.build_spec(';'.join(parts))
    except _match.SpecError as x:
        raise Refused('scene: %s' % x)
    d = dict(kv.split('=', 1) for kv in spec.split(';') if kv)
    if not ids and 'type' not in f:
        d.pop('type', None)
    if ids:   # the builder's seats; an attire as the third part (S109 ENT, src\build_spec.h); the launcher uses participants_text instead
        d['p'] = ','.join('%d:%d' % (i, pads.get(s, -1)) + ('' if a is None else ':%d' % a) for s, (i, a) in enumerate(ids))
    if f.get('entrances') == 'off':
        d['entrances'] = '0'   # the builder's skip (the launcher uses --wwe2k14_launch_skip_entrances)
    try:
        import resolve as _resolve
        raw = _resolve.raw_spec(';'.join('%s=%s' % (k, d[k]) for k in BUILD_KEYS if k in d))
    except ValueError as x:
        raise Refused('scene: %s (the match builder\'s own limits, tools\\goto\\resolve.py raw_spec)' % x)
    return d


def participants_text(ids, pads):
    seats = []
    for s, (i, a) in enumerate(ids):
        pad = pads.get(s)
        if a is None:
            seats.append('%d' % i if pad is None else '%d:%d' % (i, pad))
        else:
            seats.append('%d:%s:%d' % (i, '' if pad is None else pad, a))
    return ','.join(seats)


def lower_start(req, door, ids, pads, nums, plan, go):
    f = req['scene']['f']
    if door == 'launcher':
        if 'type' in f or 'pace' in f:
            if 'type' not in f:
                raise Refused('pace= with the launcher needs type= (pace goes to the @quick launcher: --wwe2k14_quick_pace)')
            if not ids:
                raise Refused('type= needs WHO: the @quick launcher builds the match from the participants given')
            plan.flags += ['--wwe2k14_launch=@quick', '--wwe2k14_quick_type=%s' % nums['type']]
            plan.cvars_needed.append(('wwe2k14_quick_type', 'the @quick launcher'))
            if 'pace' in f:
                plan.flags.append('--wwe2k14_quick_pace=%s' % nums['pace'])
                plan.cvars_needed.append(('wwe2k14_quick_pace', 'pace'))
        else:
            if len(ids) > 2:
                raise Refused('WHO has %d wrestlers and no type=: the captured scenario (lesnar_bryan) is a one-on-one; give type= (the @quick launcher builds the match type)' % len(ids))
            plan.flags.append('--wwe2k14_launch=lesnar_bryan')
        plan.cvars_needed.append(('wwe2k14_launch', 'the launcher'))
        if f.get('entrances') == 'off':
            plan.flags.append('--wwe2k14_launch_skip_entrances=true')
            plan.cvars_needed.append(('wwe2k14_launch_skip_entrances', 'entrances=off'))
            plan.intent.append('flag --wwe2k14_launch_skip_entrances=true')
        if ids:
            plan.flags.append('--wwe2k14_launch_participants=' + participants_text(ids, pads))
            plan.cvars_needed.append(('wwe2k14_launch_participants', 'WHO'))
        if 'arena' in f:
            plan.flags.append('--wwe2k14_launch_arena=%s' % nums['arena'])
            plan.cvars_needed.append(('wwe2k14_launch_arena', 'arena='))
        f_, t_ = GOLDEN_WAIT['launcher']
        plan.start += ['wait_file %s %d' % (f_, t_), 'log bench: match launched']
        return GOLDEN_WAIT['bell_launcher']
    # builder
    if not ids:
        raise Refused('WHO is needed for the match builder (blood=): it builds the participants from the names')
    spec = ';'.join('%s=%s' % (k, nums[k]) for k in BUILD_KEYS if k in nums)
    plan.cvars_needed += [('wwe2k14_match', 'the match builder'), ('wwe2k14_match_go', 'the match builder')]
    if 'entrances' in nums:   # S109 ENT: the builder's skip (REX_HOOK_RAW(sub_822CBD28) inside our own finalize)
        plan.cvars_needed.append((ENT_MARK_ENTRANCES, 'entrances=off on the match builder (an exe built with S109 ENT)'))
    if any(a is not None for _, a in ids):
        plan.cvars_needed.append((ENT_MARK_ATTIRE, 'an attire on the match builder (an exe built with S109 ENT)'))
    plan.start += ['wait_state menu 1 1200 30', 'wait_frames 120', 'log bench: main menu idle, firing the builder',
                   'cvar wwe2k14_match_dry false', 'cvar wwe2k14_match_compare ""', 'cvar wwe2k14_match %s' % spec,
                   'cvar wwe2k14_match_go %d' % go, 'wait_state seq 9 3600 2']
    return GOLDEN_WAIT['bell_builder']


def wait_lines(moment, bell_timeout):
    k = moment[0]
    if k == 'bell':
        return ['wait_state match_clock >0 %d' % bell_timeout]
    if k == 'roster':
        return ['wait_event render roster_ready 2400']
    if k == 'entrance':
        return ['wait_state entrance_step_%d >=%d 3600 2' % (moment[1], moment[2])]
    if k == 'finish':
        return ['wait_state 0x8376ACCC !=0 72000 2', 'log_state 0x8376ACCC']
    if k == 'victory':
        return ['wait_file WINNING 6000']
    if k == 'clock':
        return ['wait_state match_clock >=%d 40000' % moment[1]]
    if k == 'seq':
        return ['wait_state seq %d 3600 2' % moment[1]]
    return ['wait_file %s 1800' % moment[1]]


def lower_moments(req, door, bell_timeout, release):
    f = req['scene']['f']
    steps, cur, elapsed = [], None, 0
    ats = req['at']
    first = ats[0]['moment'] if ats else ('bell',)
    if RANK[first[0]] >= 2 and first[0] != 'bell':
        steps += wait_lines(('bell',), bell_timeout)   # the match is running before any later moment (every golden route waits the bell first)
        cur, elapsed = ('bell',), 0
    for a in ats:
        m, n = a['moment'], a['n']
        if m[0] == 'entrance' and (f.get('entrances') == 'off'):
            raise Refused('line %d: entrance moment with entrances=off: no entrance plays' % n)
        if cur is None or m != cur:
            if cur is not None and RANK[m[0]] < RANK[cur[0]]:
                raise Refused('line %d: moment %s comes before %s in a match: lines run in order, put it earlier' % (n, m[0], cur[0]))
            if cur is not None and m[0] == cur[0] == 'clock' and m[1] < cur[1]:
                raise Refused('line %d: clock:%d is before clock:%d: lines run in order' % (n, m[1], cur[1]))
            if m[0] == 'victory' and (cur is None or cur[0] != 'finish'):
                # WINNING opens 1,100-1,700 frames after the finish word (S108 logs); a match runs 3,500-6,500+ frames after the bell, past wait_file's 6000:
                # the finish is the victory's own state, waited first (as `bell` before any later moment)
                steps += wait_lines(('finish',), bell_timeout)
            steps += wait_lines(m, bell_timeout)
            cur, elapsed = m, 0
        if a['off'] < elapsed:
            raise Refused('line %d: +%df is before the previous line\'s %df on the same moment: lines run in order' % (n, a['off'], elapsed))
        if a['off'] > elapsed:
            steps.append('wait_frames %d' % (a['off'] - elapsed))
            elapsed = a['off']
        for it in a['items']:
            if it[0] in ('bleed', 'dev'):
                if release:
                    raise Refused('line %d: "%s" is a dev-build verb and the exe is a release build' % (n, it[1]))
                if it[0] == 'bleed' and f.get('blood') != 'on':
                    raise Refused('line %d: "%s" needs blood=on (a launched match is blood off, route_lint (d)); blood=on starts the match builder' % (n, it[1]))
                steps.append(it[1])
            elif it[0] in ('action', 'take'):
                steps.append(it[1])
            else:   # every: K shots at +N .. +K x N. The wait is one frame longer: the runner stops the periodic shot in the frame the wait ends,
                # BEFORE that frame's shot (src\input_script.h OnFrame :516-537 runs the steps, then the periodic check; wait_frames :1714), so N x K gave K - 1
                steps += ['screenshot_every %d %s' % (it[1], it[3]), 'wait_frames %d' % (it[1] * it[2] + 1), 'screenshot_every 0']
                elapsed += it[1] * it[2] + 1
    if not ats:
        steps += wait_lines(('bell',), bell_timeout)
    return steps


def item_word(req):
    ms = [a['moment'][0] for a in req['at']]
    return 'victory' if 'victory' in ms else 'entrance' if 'entrance' in ms else 'match'


def make_slug(req, ids, door):
    f = req['scene']['f']
    bits = ['v'.join(str(i) for i, _ in ids) or 'cap']
    if 'arena' in f:
        bits.append('a' + re.sub(r'\W+', '', f['arena'].lower())[:12])
    if 'blood' in f:
        bits.append('blood' + f['blood'])
    if f.get('entrances') == 'off':
        bits.append('noent')
    if 'type' in f:
        bits.append('t' + re.sub(r'\W+', '', f['type'].lower())[:12])
    return '_'.join(bits)


def board_item(name):
    import coverage
    for item, rx in coverage.ITEM_RULES:
        if re.search(rx, name, re.I):
            return item
    return None


def lower(request, exe=None, data=None):
    """request text -> (route text, flags line, name stem). Raises Refused. `exe` / `data` are the run's -Exe / -Data: only checked, never changed."""
    import route_lint
    req = parse_request(request)
    f = req['scene']['f']
    dlc = f.get('dlc') == 'on'
    ids = resolve_who(req['scene']['who'], dlc)
    pads = resolve_players(f, len(ids))
    door = choose_door(req, ids)
    nums = numeric_spec(req, ids, pads, dlc)
    run = req['run']
    if 'timeout' in run:
        raise Refused('run timeout=%s: the run\'s wall limit is run_list\'s -WaitSeconds, not the route\'s; pass -WaitSeconds to run_list' % run['timeout'])
    if exe is None and run.get('exe'):
        exe = run['exe']
    if run.get('exe') and exe != run['exe']:
        raise Refused('run exe=%s but run_list was given -Exe %s: pass the same exe' % (run['exe'], exe))
    if 'save' in f and data is None:   # the route cannot choose its save (run_list's -Data does): without it the field would be dropped
        raise Refused('scene save=%s: the save is run_list\'s -Data (scene.py --data); give -Data %s, the request cannot set it' % (f['save'], f['save']))
    if 'save' in f and data != f['save']:
        raise Refused('scene save=%s but run_list was given -Data %s: pass -Data %s (the request cannot change run_list\'s save)' % (f['save'], data, f['save']))
    h = hashlib.sha1(canonical(req['lines']).encode('utf-8')).hexdigest()
    go = int(h[:6], 16) % 999999 + 1
    plan = Plan()
    plan.door = door
    bell_timeout = lower_start(req, door, ids, pads, nums, plan, go)
    release = bool(exe) and route_lint.is_release(exe)
    body = lower_moments(req, door, bell_timeout, release)
    # the exe must define the cvars the start needs (an exe without them ignores the flag silently; the run would wait for nothing)
    if exe:
        if not os.path.isfile(exe):
            raise Refused('exe %s: no such file' % exe)
        for cv, why in plan.cvars_needed:
            if not route_lint.exe_knows(exe, cv):
                raise Refused('exe %s does not define %s (needed for %s): it would ignore it silently; run an exe built with it' % (exe, cv, why))
    # flags / intent
    flags = list(BASE_FLAGS) + plan.flags
    if dlc:
        flags.append('--wwe2k14_dlc=true')
    plan.intent = ['owner on'] + (['dlc on'] if dlc else []) + (['blood ' + f['blood']] if 'blood' in f else []) + plan.intent
    for t in [x.strip() for x in run.get('intent', '').split(',') if x.strip()]:
        if t == 'watch':
            raise Refused('run intent=watch: a run a person plays / watches stays hand-written (SCENE_plan.md section 5)')
        plan.intent.append(t)
    own = {t.split('=')[0] for t in flags}
    for t in run.get('flags', '').split():
        if not t.startswith('--'):
            raise Refused('run flags: "%s" is not a --flag' % t)
        if t.split('=')[0] in own:
            raise Refused('run flags: %s is already set by the scene (a flag given twice makes CLI11 drop later startup flags)' % t.split('=')[0])
        flags.append(t)
    word = item_word(req)
    stem = '%s_%s_%s' % (word, make_slug(req, ids, door), h[:8])
    got = board_item(stem)
    if got != ITEM_BOARD[word]:
        raise Refused('internal: the route name %s would file on the coverage board as "%s", not "%s" (tools\\board\\coverage.py ITEM_RULES)' % (stem, got, ITEM_BOARD[word]))
    logged = canonical(req['lines']).replace('#', '%23')
    text = '\n'.join(
        ['# harness: flags ' + ' '.join(flags),
         '# intent: ' + ', '.join(plan.intent),
         '# Generated by tools\\run\\scene.py from the scene request below (docs\\systems\\run_contract.md "Scene requests"): do not edit, re-generate.',
         '# request: ' + logged.replace('\n', ' '),
         '# door: ' + door + (' (the launcher: no blood= asked)' if door == 'launcher' else ' (blood= needs the match builder; entrances %s)' % (
             'skipped (entrances=0)' if 'entrances' in nums else 'play')) + '; fast-forwarded: every wait is on game state or guest frames',
         'on_fail quit',
         'log bench: request ' + logged,
         'log bench: start scene ' + stem,
         'include boot_to_menu'] + plan.start + body + ['log bench: done', 'wait_frames 120', 'quit']) + '\n'
    lint_text(text, ' '.join(flags), exe, data)
    return text, ' '.join(flags), stem


def lint_text(text, flags, exe, data=None):
    """The route lint's verdict on the lowered route, fast-forwarded (no performance / watch term): an ERROR refuses the request with the lint's own text
    (a frame-counted shot after an entrance sync, a frame-counted press after a page change, a flag no cvar defines ...)."""
    import tempfile
    import route_lint
    with tempfile.TemporaryDirectory(prefix='scene_lint_') as d:
        p = os.path.join(d, 'route.txt')
        with open(p, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
        errs, _w, _f, _h = route_lint.lint_route(p, ROOT, final=flags, exe=exe, save=data)   # the run's -Data: save terms are checked against it, as run_list does
    if errs:
        raise Refused('the route lint (tools\\bench\\route_lint.py) refuses the lowered route fast-forwarded: ' + ' | '.join(errs))


def write(request, exe=None, data=None, out_dir=None):
    """lower + write + lint: the path of the route. A route the lint refuses fast-forwarded is removed and refused with the lint's text."""
    text, flags, stem = lower(request, exe, data)   # lower() has run the lint
    out_dir = out_dir or OUT_DIR
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, stem + '.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(text)
    return path


def read_arg(arg):
    if os.path.isfile(arg):
        with open(arg, encoding='utf-8') as fh:
            return fh.read()
    return arg


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--write', metavar='REQUEST|FILE')
    g.add_argument('--print', dest='show', metavar='REQUEST|FILE')
    ap.add_argument('--exe')
    ap.add_argument('--data')
    a = ap.parse_args(argv)
    try:
        if a.write is not None:
            print(write(read_arg(a.write), a.exe, a.data))
        else:
            text, flags, _ = lower(read_arg(a.show), a.exe, a.data)
            print(text, end='')
    except Refused as x:
        print('REFUSED: %s' % x)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
