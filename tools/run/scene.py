r"""S109 SCENE: a check as a short scene request, lowered to a route (design: docs\research\s109\SCENE_plan.md; contract: docs\systems\run_contract.md "Scene requests").

    py -I tools\run\scene.py --write REQUEST|FILE [--exe EXE] [--data NAME]   write out\scenes\<item word>_<slug>_<hash8>.txt, print its path (exit 2 + REFUSED: why)
    py -I tools\run\scene.py --print REQUEST|FILE [--exe EXE]                  print the route, write nothing
    py -I tools\run\scene.py --describe ROUTE                                  the JSON line (describe()) of a route this tool wrote (run_list's scene door)
    --out PATH (with --write: a .txt file or a folder under out\); --json (either: ONE JSON line instead, describe(): the header flags, the arrival
    wait, the game lines to expect, the shots; a refusal is {"refused": WHY}, exit 2), so a caller never parses the route

A request is lines (a file, or one argument whose lines are joined by ` // `; a leading `scene:` is dropped):
    scene  [match] WHO [; arena=A] [; type=T] [; pace=P] [; entrances=on|off] [; blood=on|off] [; dlc=on] [; mods=on] [; save=NAME] [; player=SEAT[,SEAT]]
    scene  wm30 N | cas new | menu | mode NAME   [; dlc=on] [; mods=on] [; save=NAME]     (S110 SCENEMODES, docs\research\s110\SCENEMODES.md)
    at     MOMENT[+Nf] : ITEM [; ITEM ...]       lines run in order; an offset counts from the line's moment
    run    [exe=PATH] [intent=TERMS] [flags=--a=1 ...]
  WHO      names or #ids in seat order joined by `vs` or `,`; `/N` after one = its attire (0-9). Names go through db\ids (db\ids\dlc_characters.json with dlc=on).
           WHO may be empty for the launcher (the captured scenario's own wrestlers). Every seat is a CPU unless `player=SEAT` names it (pad 0, 1, ... in the order given).
           mods=on adds --wwe2k14_mods_superstars=true (src\mods.h:48): an imported superstar is then a seat id, `#397`.
  MOMENT   bell | roster | entrance:S[:STEP] (slot S 0-5, STEP default 1) | finish | victory | clock:N | seq:N | load:FILE
           bus moments (S110 SCENEMOMENTS, a `wait_event` each: tools\run\scene_moments.py): pin | count[=N] | kickout[=N] | finish=NAME
           | move[:KEY=V ...] | eliminated[:KEY=V ...] | replay | replay_end | ring_break | table_break; a request with no director verb gets
           the moment's default recipe before it (the `# recipe:` header line says which); every one but replay / replay_end ends at the
           finish (`until finish`, S110 SCENEMOM2): a match that ends another way fails that line with the finish's fields
           wm30 N (match 0-45): `card` (the READY card, before its A) or any match moment (after the A); default bell
           cas new: `page=R` or `R` (a record of the editor's flow table, holder slot 1) or `editor`; default the editor up (sequence 76). Several
           pages run one after the other (the go-to re-armed by `cvar`); a LETTERING / CUSTOM LOGO DATA item page only as the last
           menu: `page=R` or `R` (a front-end flow record; several run one after the other, `home` between); default the main menu (menu 1)
           mode NAME (a row of src\mode_table.h, read by tools\scene\mode_table.py: no other list): `arrived` (the mode's own sequence)
  ITEM     actions: passive S on|off|all on|off, momentum S PCT, stock S N, bleed S LEVEL, lightsout S, passive_cpu on|off, fire S (match and wm30 only),
           skip_replay (a match only: the game's own replay skip, then `wait_event replay skipped 600`; S110 FIRETABLE; the in-match instant
           replay only, not the replay after a finish: FIRETABLE.md "Fix round 2")
           takes:   screenshot L | shot face|wrestler|stage|prop N [L] (match and wm30 only) | record L | every Nf xK L | log STATE
Start door (SCENE_plan.md section 7 choice 2a): `blood=` -> the match builder (`cvar wwe2k14_match`), which since S109 ENT also takes `entrances=off`
(spec `entrances=0`) and an attire (`p=ID:PAD:N`) (docs\research\s109\ENT_plan.md); everything else -> the launcher (`--wwe2k14_launch=lesnar_bryan`, or
`@quick` with type=), unchanged. A combination neither door serves is never silently reduced: every field is placed or the request is refused with the
reason. A route the lint refuses fast-forwarded is refused here with the lint's own text.
Every route ends `log bench: done` after its last wait under `on_fail quit`: run_read exits 0 only with that line (tools\bench\run_read.py:222, :415-416), so
the verdict is the arrival wait itself; the `# expect:` header lines are game lines for `run_read --expect`.
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
import scene_moments as _moments   # noqa: E402  the bus moments (tools\run\scene_moments.py, S110 SCENEMOMENTS)

OUT_DIR = os.path.join(ROOT, 'out', 'scenes')
BASE_FLAGS = ['--wwe2k14_fixed_seed=1', '--vulkan_stored_pipelines_at_boot=false', '--wwe2k14_events=true', '--wwe2k14_rdr_owner=true', '--audio_mute=true']
SCENE_KEYS = ('arena', 'type', 'pace', 'entrances', 'blood', 'dlc', 'mods', 'save', 'player')
BUILD_KEYS = ('type', 'p', 'arena', 'pace', 'o', 'entrances')   # the builder spec's keys a request fills, in spec order (src\build_spec.h)
# strings only an exe built with S109 ENT holds (the wwe2k14_match help text, src\scenario.h): an older builder refuses these specs in the game
# (`[build] spec ... refused`) and the route would wait for a match that never starts
ENT_MARK_ENTRANCES, ENT_MARK_ATTIRE = 'entrances=0 skips the entrances', 'p=ID:PAD:ATTIRE'
BUS_ONLY = ('remaining', 'entered', 'code', 'playing')   # bus moments with no lowering yet (the Rumble's counters; code / playing are not in the game)
BUS_WORDS = ('pin', 'count', 'kickout', 'eliminated', 'move', 'finisher', 'elimination', 'eliminate')   # moment words, never items
GOLDEN_WAIT = {'launcher': ('INGAMELUA.PAC', 1800), 'bell_launcher': 30000, 'bell_builder': 40000}
# the order of a match's moments; the bus moments (S110 SCENEMOMENTS) sit between the clock and the finish, a replay with the finish
RANK = {'entrance': 1, 'seq': 1, 'load': 1, 'bell': 2, 'roster': 3, 'clock': 4, 'pin': 4, 'count': 4, 'kickout': 4, 'move': 4, 'eliminated': 4,
        'ring_break': 4, 'table_break': 4, 'finish': 5, 'replay': 5, 'replay_end': 5, 'victory': 6}
ITEM_WORD = {'victory': 'victory', 'entrance': 'entrance', 'match': 'match'}      # the route name's word -> the board item it must file under
ITEM_BOARD = {'victory': 'victory', 'entrance': 'entrances', 'match': 'matches', 'wm30': 'wm30', 'create': 'create', 'menu': 'menus'}   # tools\board\coverage.py ITEM_RULES
LABEL_RX = re.compile(r'^[A-Za-z0-9_.\-]+$')
STATE_RX = re.compile(r'^(0x[0-9A-Fa-f]{1,8}|[A-Za-z_][A-Za-z0-9_]*)$')
# ---- S110 SCENEMODES (docs\research\s110\SCENEMODES.md): the modes beside `match`
MODE_WORDS = ('match', 'wm30', 'cas', 'menu', 'mode', 'stage')
MODE_KEYS = ('dlc', 'mods', 'save')            # the shared fields every other mode takes (a match takes SCENE_KEYS)
WM30_LAST = 45                                 # src\wm30_jump.h:39 "match N (0-45, py tools\wm30\list.py)"
# the 30 Years READY card, as the S105 route that ran it waits for it (tools\input_scripts\scenes\s105_chk_caption.txt:9-15, docs\research\s105\T2b.md):
# READY is armed by the end of the match load; one A once the listener is live
WM30_CARD = ['wait_file EVTDSC 1200', 'wait_state 0x83924744>80>0 0x820353E0 6000 2', 'wait_state 0x83924744>4 3 9000 2',
             'wait_state 0x83924744>80>352 2 120 2', 'wait_state 0x836F3AA4 <=0 600 1']
WM30_READY = 'press_until_state A 0x83924744>80>16 !=2 4 30 6 unreadable'
WM30_EXPECT = '[wwe2k14][wm30][direct] exit action 3 requested'     # src\wm30_boot.h:175 (exit action 3: :1-4)
CAS_EXPECT_ARRIVED = '[wwe2k14][cas_goto] record %d: arrived at record %d'      # src\cas_goto.h:146 with :355 / :387 (both arrival forms)
# lane CASARRIVE's route states (src\cas_goto_core.h kStateNames: cas_goto 0 off / 1 running / 2 arrived / 3 stopped / 4 refused, cas_record = the
# record arrived at, 0 until then). Waited on only when the exe being run holds the name (route_lint.exe_knows, the way ENT_MARK_* work); otherwise
# the go-to's own bound is waited out and the `# expect:` lines are the test.
CAS_RECORD_STATE = 'cas_record'
CAS_EXPECT_STATE = '[wwe2k14][cas_goto] state cas_goto=2 cas_record=%d '        # src\cas_goto.h Publish (lane CASARRIVE): the arrival's state line
CAS_STATE_FRAMES = 3600
CAS_FALLBACK_FRAMES = 1800                     # tools\input_scripts\scenes\s110_cas_goto_probe.txt:15-17 (1200 + 600; run 231 arrived in ~1000)
# S110 SCENEMOMENTS: several pages in one request, the go-to re-armed by `cvar` (the route shape of src\cas_goto_core.h:245-253, lane CASARRIVE)
CAS_TARGET_STATE = 'cas_target'
# code 1003 / 1008 template choosers (FACES, SUPERSTAR, DIVA, the gated SUPERSTAR: out\harness\atlas\flow_cas_flow_slot1.json); the go-to refuses a
# path into one (src\cas_goto_core.h RefusePath, cas_reason 3)
CAS_TEMPLATE_RECORDS = (3, 23, 24, 25)
# item pages the go-to reaches but never LEAVES (their panel's B is not driven: src\cas_goto_core.h:159-161 PlanUp kNoPanel, cas_reason 16;
# docs\research\s110\CASARRIVE.md "Fix 1"): only the last page of a request
CAS_LAST_ONLY = dict([(r, 'LETTERING') for r in (43, 126, 127, 174, 175, 176, 177, 178, 179)] +
                     [(r, 'CUSTOM LOGO DATA') for r in (44, 130, 131, 180, 181, 182, 183, 184, 185)])
# No settle before a Create a Superstar picture: since S110 CASREADY the go-to's arrival waits for the parts panel, the model-ready word and the
# game's loading screen (src\cas_goto.h ArrivalReady; run 278: the at-arrival shot is complete; sweep 3: 189 arrived, 0 stopped).
MENU_PAGE_MARK = 'page needs a record id: page N [back]'   # src\menu_goto_core.h:186: an exe without it refuses the route's `page` line at load
MENU_EXPECT = '[wwe2k14][menu_goto] record %d page change done'   # src\menu_goto.h:99
# mode NAME: a row of src\mode_table.h kModes (the table the exe obeys), read by tools\scene\mode_table.py: the launch `--wwe2k14_launch=@mode:NAME`
# (src\mode_launch.h), the sequence it reaches, the log lines it prints. MODE_MARK must be in the exe.
MODE_MARK = '@mode:'
# S110 MODEPICK: `scene mode moveset #N` (src\mode_pick.h): the rows that take a wrestler, the ids, the route states, the line the pick prints.
# S110 ENTPICK: `scene mode entrance #N` too (src\mode_pick_core.h RowPick; EASY CREATION on N).
# The states are waited on only when the exe holds `mode_pick_id` (route_lint.exe_knows); otherwise a frame count, and the lines are the test.
PICK_ROWS = ('moveset', 'entrance')
PICK_IDS = 500
PICK_STATE, PICK_ID_STATE = 'mode_pick', 'mode_pick_id'
PICK_EXPECT = '[wwe2k14][launch] @mode:%s: picked wrestler %d ('
PICK_FRAMES, PICK_FALLBACK_FRAMES = 3600, 1800
_MODES = []


def mode_table():
    """tools\\scene\\mode_table.py (loaded by path: tools\\scene is not on this module's import path). Refused when the header does not parse."""
    if not _MODES:
        import importlib.util
        spec = importlib.util.spec_from_file_location('scene_mode_table', os.path.join(ROOT, 'tools', 'scene', 'mode_table.py'))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        try:
            m.rows(ROOT)
        except (OSError, ValueError) as x:
            raise Refused('the mode table (src\\mode_table.h): %s' % x)
        _MODES.append(m)
    return _MODES[0]


def mode_row(name):
    return mode_table().find(name, ROOT)
# the flow tables records are checked against (tools\menus\flow_graph.py output: local under out\, never committed). No table = not checked
# offline (the game refuses an unknown record itself: menu_goto Plan, cas_goto LoadPath: a FAILED / STOPPED line). A test sets
# RECORD_TABLES[kind] to {record: target menu or None} (or None = no table) before lowering.
FLOW_FILES = {'cas': ['flow_cas_flow_slot1.json'], 'menu': 'flow_[0-9]*.json'}
RECORD_TABLES = {}
SYNC_WORDS = ('wait_state', 'wait_file', 'wait_event', 'page', 'press_until_state')


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
    scene, at_lines, run = None, [], {}
    for n, ln in enumerate(lines, 1):
        word, _, rest = ln.partition(' ')
        w = word.lower()
        if w == 'scene':
            if scene is not None:
                raise Refused('line %d: a second `scene` line (one scene per request)' % n)
            scene = parse_scene(rest)
        elif w == 'at':
            at_lines.append((rest, n))
        elif w == 'run':
            for k, v in parse_run(rest).items():
                if k in run:
                    raise Refused('line %d: run %s given twice' % (n, k))
                run[k] = v
        else:
            raise Refused('line %d: "%s" is not scene | at | run' % (n, word))
    if scene is None:
        raise Refused('no `scene` line')
    mode = scene['mode']   # the positions an `at` line may name depend on the mode (S110 SCENEMODES)
    ats = [parse_at(rest, n) if mode == 'match' else parse_at_mode(rest, n, mode) for rest, n in at_lines]
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
    for k in ('entrances', 'blood', 'dlc', 'mods'):
        if k in f and f[k].lower() not in ('on', 'off'):
            raise Refused('scene: %s=%s (on | off)' % (k, f[k]))
        if k in f:
            f[k] = f[k].lower()
    return parse_mode(who, f)


def parse_mode(who, f):
    """The scene's MODE (S110 SCENEMODES): WHO's first word when it is a mode word, else a match. -> {'mode', 'who', 'f', + the mode's own}."""
    words = who.split()
    if not words or words[0].lower() not in MODE_WORDS:
        return {'mode': 'match', 'who': who, 'f': f}
    mode, arg = words[0].lower(), ' '.join(words[1:])
    if mode == 'match':
        return {'mode': 'match', 'who': arg, 'f': f}
    extra = [k for k in f if k not in MODE_KEYS]
    if extra:
        raise Refused('scene %s: %s is a match field (%s takes: %s)' % (mode, ', '.join(extra), mode, ', '.join(MODE_KEYS)))
    if mode == 'wm30':
        if not re.fullmatch(r'\d{1,3}', arg) or int(arg) > WM30_LAST:
            raise Refused('scene wm30 N: N is a 30 Years match index 0-%d (py -I tools\\wm30\\list.py prints them), got "%s"' % (WM30_LAST, arg))
        return {'mode': mode, 'n': int(arg), 'who': '', 'f': f}
    if mode == 'cas':
        if arg.lower() != 'new':
            raise Refused('scene cas new: only a NEW superstar is reachable (@cas = exit 8 = NEW, src\\cas_launch.h:1-11), got "cas %s" '
                          '(an existing or imported superstar is SCENETOOL\'s CASEDIT, not built)' % arg)
        return {'mode': mode, 'who': '', 'f': f}
    if mode == 'menu':
        if arg:
            raise Refused('scene menu takes no argument (the page is the `at` line: at page=R : ...), got "menu %s"' % arg)
        return {'mode': mode, 'who': '', 'f': f}
    if mode == 'mode':
        parts = arg.split()
        name = parts[0].lower() if parts else ''
        if name == 'cas':
            raise Refused('scene mode cas: the Create a Superstar row is `scene cas new` (the same @cas launch, and it takes a page: at page=R)')
        if mode_row(name) is None:
            raise Refused('scene mode NAME: "%s" is not a mode of the table (src\\mode_table.h; modes: %s)'
                          % (arg, ', '.join(r['name'] for r in mode_table().rows(ROOT) if r['name'] != 'cas')))
        pick = None
        if len(parts) > 1:
            if name not in PICK_ROWS:
                raise Refused('scene mode %s: a wrestler pick (#N) is taken by %s only (src\\mode_pick_core.h RowPick)' % (name, ', '.join(PICK_ROWS)))
            m = re.fullmatch(r'#?(\d{1,6})', ' '.join(parts[1:]))
            if not m or int(m.group(1)) >= PICK_IDS:
                raise Refused('scene mode %s #N: N is a roster id 0-%d (one the editor\'s own picker lists), got "%s"' % (name, PICK_IDS - 1, ' '.join(parts[1:])))
            pick = int(m.group(1))
        return {'mode': mode, 'name': name, 'pick': pick, 'who': '', 'f': f}
    raise Refused('scene stage: the stage push has no script line yet (SCENETOOL lane 6, MODELAUNCH): not offered')


def parse_at(rest, n):
    mm = re.match(r'^\s*([A-Za-z_]+(?:=[A-Za-z0-9_\-]+)?(?::[^\s:+]+)*)(?:\+(\d+)f)?\s*:\s*(.*)$', rest)
    if not mm:
        raise Refused('line %d: at MOMENT[+Nf]: ITEM[; ITEM] (got "at %s")' % (n, rest))
    word, off, items = mm.group(1), int(mm.group(2) or 0), mm.group(3)
    return {'n': n, 'moment': parse_moment(word, n), 'off': off, 'items': [parse_item(i.strip(), n) for i in items.split(';') if i.strip()], 'text': rest}


def parse_moment(word, n):
    parts = word.split(':')
    name = parts[0].split('=')[0].lower()
    if name in ('highlight', 'highlightreplay'):
        raise Refused('line %d: moment "%s": the post-match highlight replay is not offered; the replay track is `at replay` / `at replay_end` (a bus wait)' % (n, word))
    if name in BUS_ONLY:
        raise Refused('line %d: moment "%s": bus only: no lowering yet (goto\'s @%s waits host-side on the event bus). '
                      'Use a frame count after the nearest state, e.g. `bell+Nf`' % (n, word, name))
    if name == 'finish' and len(parts) > 1:
        raise Refused('line %d: moment "%s": the finish kind is `finish=NAME` (%s), a bus wait; `finish` alone waits on the finish word' % (n, word, _moments.FINISH_WORDS))
    if name in _moments.HEAD and parts[0].lower() != 'finish':   # S110 SCENEMOMENTS: a bus moment (bare `finish` stays the in-route finish word)
        try:
            return _moments.parse(word)
        except _moments.Refused as x:
            raise Refused('line %d: %s' % (n, x))
    if '=' in parts[0]:
        raise Refused('line %d: moment "%s": only a bus moment takes `=VALUE` (count=N, kickout=N, finish=NAME)' % (n, word))
    if name not in RANK:
        try:
            _when.parse('@' + word)
            known = False
        except _when.WhenError:
            known = None
        raise Refused('line %d: moment "%s" %s (moments: bell, roster, entrance:S[:STEP], finish, victory, clock:N, seq:N, load:FILE; on the bus: pin, '
                      'count[=N], kickout[=N], finish=NAME, move[:KEY=V], eliminated[:KEY=V], replay, replay_end, ring_break, table_break)'
                      % (n, word, 'has no in-route state' if known is False else 'is not a moment'))
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
    if v == 'fire':   # S110 FIRETABLE: the seat sets a standing table on fire when the game's own table step allows it (src\situation_fire_verb.h)
        need(len(w) == 2 and slot(w[1]), 'fire SLOT (SLOT 0-8)')
        return ('action', ' '.join(w))
    if v == 'skip_replay':   # S110 FIRETABLE: the verb, then the bus line that the skip went through (src\replay_fx_skip.h: state 4 after sub_826071F8)
        need(len(w) == 1, 'skip_replay (no argument)')
        return ('skip', 'skip_replay', 'wait_event replay skipped 600')
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
    if v in BUS_ONLY or v in BUS_WORDS:
        raise Refused('line %d: "%s": a moment, not an item: no verb makes it happen; wait for it with `at MOMENT : ITEMS` (pin, count=N, kickout, '
                      'finish=NAME, move, eliminated)' % (n, text))
    raise Refused('line %d: unknown item "%s" (actions: passive, momentum, stock, bleed, lightsout, passive_cpu, fire, skip_replay; takes: screenshot, shot, record, every, log)' % (n, w[0]))


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
        self.notes = []         # S110 SCENEMOMENTS: header lines (`# recipe: ...`, `# settle: ...`) of a mode route


RUMBLE_TYPES = range(20, 24)   # S111: Royal Rumble match types 20..23 (docs\research\s110\RUMBLECRASH.md; the game camera's side step skips the same four)


def choose_door(req, ids, nums=None):
    """blood= -> the match builder (it serves attire N and entrances=off too since S109 ENT: src\\build_spec.h); a Royal Rumble type (20..23) -> the
    match builder too (S111, RUMBLECRASH [C]: the @quick launcher never runs the menus' finalize that fills a Rumble's entrant list, sub_82927A10 +
    sub_829263B0, so a quick-launched Rumble crashes at the bell; the builder runs the game's own fill); otherwise the launcher, as before."""
    if 'blood' in req['scene']['f']:
        return 'builder'
    try:
        if nums and int(nums.get('type', -1)) in RUMBLE_TYPES:
            return 'builder'
    except ValueError:
        pass
    return 'launcher'


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
    if _moments.is_bus(moment):   # S110 SCENEMOMENTS: one wait_event (tools\run\scene_moments.py)
        return [_moments.wait_line(moment)]
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


def lower_moments(req, door, bell_timeout, release, info=None, wish=False, omg=False):
    """The match's `at` lines -> route steps. `info` (a dict, or None) gets 'expect' (the bus moments' game lines, the `# expect:` header) and 'notes'
    (`# recipe:` header lines). S110 SCENEMOMENTS: a bus moment is one `wait_event` (tools\\run\\scene_moments.py); when the request has no director
    verb of its own, the first bus moment's default recipe runs right after the bell wait; when no bell wait comes before that moment (an entrance
    first), the bell wait is added in front of the recipe: a director verb before the bell answers busy and is retried for only 1800 frames, then
    logged NOT DONE (src\\situation_director.h kScriptBusyFrames :130, :203), and the entrances can outlast that."""
    f = req['scene']['f']
    info = {} if info is None else info
    info.setdefault('expect', [])
    info.setdefault('notes', [])
    steps, cur, elapsed = [], None, 0
    ats = req['at']
    own = any(it[0] == 'action' for a in ats for it in a['items'])   # the request's own director lines: no default recipe
    pending, last = None, None   # the default recipe's lines until placed; the previous bus moment (a moment named twice is one wait)
    for i, a in enumerate(ats):
        m = a['moment']
        if not _moments.is_bus(m) or m == last:
            continue
        last = m
        lines, why = _moments.recipe(m, wish, omg)   # S110 FINPICK: `wish` = the exe has the `finisher` verb; S110 OMGVERB: `omg` = a dev exe with `omg`
        if m[0] == 'ring_break' and (lines is None or own) and not any(it[0] == 'action' for b in ats[:i] for it in b['items']):   # the line's own items run after its wait
            raise Refused('line %d: moment ring_break: %s; the request must make it happen with its own director lines at an earlier position' % (a['n'], why))
        if m[0] == 'ring_break' and lines and not own and door != 'wm30' and not req['scene'].get('who', '').strip():
            raise Refused('line %d: moment ring_break: its recipe needs WHO with seat 0 a super heavyweight (roster class 4, e.g. #125 Big Show or #179 Mark Henry: '
                          'the game\'s ring-break test reads only the attacker\'s class, docs\\research\\s110\\OMGVERB.md)' % a['n'])
        if m[0] == 'eliminated' and door != 'wm30' and 'type' not in f:
            raise Refused('line %d: moment eliminated needs type= naming a match that eliminates (a one-on-one ends at its first finish)' % a['n'])
        if m[0] == 'table_break' and door != 'wm30' and 'type' not in f:   # S110 SCENEMOM2: no director verb brings a table in
            raise Refused('line %d: moment table_break needs type= naming a Tables match (one on one: type=68, db\\ids\\match_types.json "TABLE"; '
                          'tools\\input_scripts\\short\\s110_table_acts.txt)' % a['n'])
        if own:
            info['notes'].append('recipe: the request\'s own director lines, for `%s`' % _moments.wait_line(m))
        elif lines and pending is None:
            pending = lines
            info['notes'].append('recipe: %s, for `%s`: %s (the request gives no director verb)' % (', '.join(lines), _moments.wait_line(m), why))
        elif not lines:
            info['notes'].append('recipe: none for `%s`: %s' % (_moments.wait_line(m), why))
        info['expect'].append(_moments.expect(m))

    belled = False   # a bell wait is in the steps already

    def place():
        nonlocal pending, belled
        if pending:
            if not belled:   # an entrance first: the recipe waits for the bell (docstring)
                steps.extend(wait_lines(('bell',), bell_timeout))
                belled = True
            steps.extend(pending)
            pending = None
    first = ats[0]['moment'] if ats else ('bell',)
    if RANK[first[0]] >= 2 and first[0] != 'bell':
        steps += wait_lines(('bell',), bell_timeout)   # the match is running before any later moment (every golden route waits the bell first)
        cur, elapsed, belled = ('bell',), 0, True
        place()
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
            if _moments.is_bus(m):
                place()
            steps += wait_lines(m, bell_timeout)
            if m == ('bell',):
                belled = True
                place()
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
            elif it[0] == 'skip':   # S110 FIRETABLE: the verb, then its bus wait
                steps += [it[1], it[2]]
            else:   # every: K shots at +N .. +K x N. The wait is one frame longer: the runner stops the periodic shot in the frame the wait ends,
                # BEFORE that frame's shot (src\input_script.h OnFrame :516-537 runs the steps, then the periodic check; wait_frames :1714), so N x K gave K - 1
                steps += ['screenshot_every %d %s dup' % (it[1], it[3]), 'wait_frames %d' % (it[1] * it[2] + 1), 'screenshot_every 0']   # S111 SHOTDIET: `dup` = K shots, not K distinct guest frames
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


FIRE_PROBE = '[wwe2k14][table_fire]'   # S110 FIRETABLE review: the log text of src\table_fire.h = the exe has the `fire` / `skip_replay` verbs


def check_fire_verbs(req, exe, route_lint):
    """S110 FIRETABLE review: `fire S` / `skip_replay` on an exe built before the two director verbs are refused here, when the route is
    lowered (that exe's script load would refuse the line: src\\input_script.h :1428 IsVerb). No exe named = nothing checked."""
    if not exe or route_lint.exe_knows(exe, FIRE_PROBE):
        return
    for a in req['at']:
        for it in a['items']:
            if it[0] == 'skip' or (it[0] == 'action' and it[1].split()[0] == 'fire'):
                raise Refused('line %d: "%s": the exe %s has no `fire` / `skip_replay` director verbs (built before S110 FIRETABLE)' % (a['n'], it[1], exe))


def lower(request, exe=None, data=None):
    """request text -> (route text, flags line, name stem). Raises Refused. `exe` / `data` are the run's -Exe / -Data: only checked, never changed."""
    import route_lint
    req = parse_request(request)
    if req['scene']['mode'] != 'match':
        return lower_mode(req, exe, data)   # S110 SCENEMODES: wm30 / cas / menu / mode
    f = req['scene']['f']
    dlc = f.get('dlc') == 'on'
    ids = resolve_who(req['scene']['who'], dlc)
    pads = resolve_players(f, len(ids))
    nums = numeric_spec(req, ids, pads, dlc)
    door = choose_door(req, ids, nums)
    run = req['run']
    exe = check_run(req, exe, data)
    check_fire_verbs(req, exe, route_lint)   # S110 FIRETABLE review
    h = hashlib.sha1(canonical(req['lines']).encode('utf-8')).hexdigest()
    go = int(h[:6], 16) % 999999 + 1
    plan = Plan()
    plan.door = door
    bell_timeout = lower_start(req, door, ids, pads, nums, plan, go)
    release = bool(exe) and route_lint.is_release(exe)
    info = {}
    wish = bool(exe) and route_lint.exe_knows(exe, _moments.FIT_PROBE)   # S110 FINPICK: the finisher_fit_S states = the `finisher` verb
    omg = bool(exe) and not release and route_lint.exe_knows(exe, _moments.OMG_PROBE)   # S110 OMGVERB: the recipe's corner set-up is the dev `state`
    body = lower_moments(req, door, bell_timeout, release, info, wish, omg)
    plan.intent = (['blood ' + f['blood']] if 'blood' in f else []) + plan.intent
    flags = tail_flags(plan, f, run)
    check_exe(exe, plan)
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
         '# door: ' + door + (' (the launcher: no blood= asked)' if door == 'launcher' else ' (%s needs the match builder; entrances %s)' % ('blood=' if 'blood' in req['scene']['f'] else 'a Royal Rumble (RUMBLECRASH)',
             'skipped (entrances=0)' if 'entrances' in nums else 'play')) + '; fast-forwarded: every wait is on game state or guest frames']
        + ['# ' + x for x in info['notes']] + ['# expect: ' + e for e in info['expect']]   # S110 SCENEMOMENTS: only a request with a bus moment has them
        + ['on_fail quit',
         'log bench: request ' + logged,
         'log bench: start scene ' + stem,
         'include boot_to_menu'] + plan.start + body + ['log bench: done', 'wait_frames 120', 'quit']) + '\n'
    lint_text(text, ' '.join(flags), exe, data)
    return text, ' '.join(flags), stem


def check_run(req, exe, data):
    """The `run` line and the scene's save= against the run's -Exe / -Data (every mode): -> the exe to check against. Refused on a mismatch."""
    run, f = req['run'], req['scene']['f']
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
    return exe


def check_exe(exe, plan):
    """The exe must define the cvars / hold the strings the start needs (an exe without them ignores the flag silently; the run would wait for nothing)."""
    if not exe:
        return
    import route_lint
    if not os.path.isfile(exe):
        raise Refused('exe %s: no such file' % exe)
    for cv, why in plan.cvars_needed:
        if not route_lint.exe_knows(exe, cv):
            raise Refused('exe %s does not define %s (needed for %s): it would ignore it silently; run an exe built with it' % (exe, cv, why))


def tail_flags(plan, f, run):
    """The header flag tokens (base + the start's + dlc / mods + the run line's); plan.intent completed (owner, dlc, mods first). Refused on a bad run field."""
    dlc, mods = f.get('dlc') == 'on', f.get('mods') == 'on'
    flags = list(BASE_FLAGS) + plan.flags
    if dlc:
        flags.append('--wwe2k14_dlc=true')
    if mods:   # src\mods.h:48: the Superstar Importer's pack joins the roster (its seats are ids: `#397`)
        flags.append('--wwe2k14_mods_superstars=true')
        plan.cvars_needed.append(('wwe2k14_mods_superstars', 'mods=on'))
    plan.intent = ['owner on'] + (['dlc on'] if dlc else []) + (['mods_superstars on'] if mods else []) + plan.intent
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
    return flags


# ------------------------------------------------------------------------------------------------ S110 SCENEMODES: wm30 / cas / menu / mode
def parse_at_mode(rest, n, mode):
    """`at POSITION[+Nf] : ITEMS` of a non-match mode -> parse_at's dict; moment ('card',) / ('page', R) / ('editor',) / ('arrived',), or a match moment (wm30)."""
    mm = re.match(r'^\s*((?:page=)?\d+|[A-Za-z]+)(?:\+(\d+)f)?\s*:\s*(.*)$', rest)
    if mode == 'wm30' and not (mm and mm.group(1).lower() == 'card'):
        return parse_at(rest, n)   # every match moment, after the READY card's A
    if not mm:
        raise Refused('line %d: at POSITION[+Nf]: ITEM[; ITEM] (got "at %s")' % (n, rest))
    word, off, items = mm.group(1).lower(), int(mm.group(2) or 0), mm.group(3)
    pos = {'wm30': ('card',), 'mode': ('arrived',)}.get(mode)
    if mode in ('cas', 'menu') and re.fullmatch(r'(page=)?\d+', word):
        rec = int(word.split('=')[-1])
        if rec > 65535:
            raise Refused('line %d: record %d: a flow record id is 0-65535 (src\\cas_goto_core.h:113)' % (n, rec))
        pos = ('page', rec)
    elif mode == 'cas' and word == 'editor':
        pos = ('editor',)
    elif pos is None or word != pos[0]:
        raise Refused('line %d: "%s" is no position of %s (positions: %s)' % (n, word, mode, {
            'cas': 'page=R, R, editor', 'menu': 'page=R, R', 'mode': 'arrived', 'wm30': 'card or a match moment'}[mode]))
    its = [parse_item(i.strip(), n) for i in items.split(';') if i.strip()]
    for it in its:
        if it[0] == 'skip':   # S110 FIRETABLE: lowered with its bus wait in a match request only (lower_moments)
            raise Refused('line %d: "%s" is a match request item (scene WHO vs WHO ...): not offered in %s' % (n, it[1], mode))
        if mode != 'wm30' and (it[0] in ('action', 'bleed', 'dev') or (it[0] == 'take' and it[1].startswith('shot '))):
            raise Refused('line %d: "%s" is a match item: %s has no wrestler slots' % (n, it[1], mode))
    return {'n': n, 'moment': pos, 'off': off, 'items': its, 'text': rest}


def record_table(kind):
    """{record: target menu or None} from the newest flow table of `kind` under out\\harness\\atlas (tools\\menus\\flow_graph.py --json), or None."""
    if kind not in RECORD_TABLES:
        import glob
        import json
        d = os.path.join(ROOT, 'out', 'harness', 'atlas')
        names = FLOW_FILES[kind]
        paths = [os.path.join(d, x) for x in names] if isinstance(names, list) else glob.glob(os.path.join(d, names))
        paths = sorted([p for p in paths if os.path.isfile(p)], key=os.path.getmtime)
        table = None
        if paths:
            with open(paths[-1], encoding='utf-8') as fh:
                doc = json.load(fh)
            table = {}
            for m in doc.get('menus', []):
                for it in m.get('items', []):
                    if 'record' in it:
                        table[int(it['record'])] = it.get('submenu', it.get('code'))
        RECORD_TABLES[kind] = table
    return RECORD_TABLES[kind]


def take_lines(ats):
    """The position lines of a non-match mode, in order -> route lines: an offset counts from the previous line on the same position
    (lower_moments' rule; refused when it goes back); `every` as lower_moments writes it."""
    out, cur, elapsed, pad = [], None, 0, 0
    for a in ats:
        if a['moment'] != cur:
            cur, elapsed, pad = a['moment'], 0, 0
        if a['off'] < elapsed - pad:
            raise Refused('line %d: +%df is before the previous line\'s %df on the same position: lines run in order' % (a['n'], a['off'], elapsed - pad))
        if a['off'] > elapsed:
            out.append('wait_frames %d' % (a['off'] - elapsed))
            elapsed, pad = a['off'], 0
        else:
            pad = elapsed - a['off']
        for it in a['items']:
            if it[0] == 'every':
                out += ['screenshot_every %d %s dup' % (it[1], it[3]), 'wait_frames %d' % (it[1] * it[2] + 1), 'screenshot_every 0']
                elapsed += it[1] * it[2] + 1
            else:
                out.append(it[1])
    return out


def sync_line(body):
    """The last line of `body` that waits on the game (state, file, event, a served page, a state-checked press): the arrival test."""
    return next((ln for ln in reversed(body) if ln.split()[0] in SYNC_WORDS), '')


def lower_wm30(req, plan, exe):
    import route_lint
    n = req['scene']['n']
    plan.flags += ['--wwe2k14_wm30=%d' % n, '--wwe2k14_wm30_direct']
    plan.cvars_needed += [('wwe2k14_wm30', 'wm30 N'), ('wwe2k14_wm30_direct', 'the menu-free 30 Years start (src\\wm30_boot.h)')]
    check_fire_verbs(req, exe, route_lint)   # S110 FIRETABLE review: `fire S` is a wm30 match item too
    cards = [a for a in req['at'] if a['moment'] == ('card',)]
    rest = [a for a in req['at'] if a['moment'] != ('card',)]
    if req['at'][:len(cards)] != cards:
        raise Refused('line %d: `card` comes before every match moment (the READY card is the start of the match): lines run in order' % cards[-1]['n'])
    body = list(WM30_CARD) + take_lines(cards)
    if cards and not rest:
        return body, 'wm30', '%d_card' % n, [WM30_EXPECT], sync_line(body)
    body.append(WM30_READY)
    info = {}
    body += lower_moments(dict(req, at=rest), 'wm30', GOLDEN_WAIT['bell_launcher'], bool(exe) and route_lint.is_release(exe), info,
                          bool(exe) and route_lint.exe_knows(exe, _moments.FIT_PROBE),   # S110 FINPICK
                          bool(exe) and not route_lint.is_release(exe) and route_lint.exe_knows(exe, _moments.OMG_PROBE))   # S110 OMGVERB
    plan.notes += info['notes']
    return body, 'wm30', '%d' % n, [WM30_EXPECT] + info['expect'], sync_line(body)


def lower_cas(req, plan, exe):
    import route_lint
    groups = []   # [(record, [at lines])]: the pages in request order, a run of lines on one page as one visit (lower_menu's grouping)
    for a in req['at']:
        if a['moment'][0] == 'page':
            if groups and groups[-1][0] == a['moment'][1]:
                groups[-1][1].append(a)
            else:
                groups.append((a['moment'][1], [a]))
    recs = [r for r, _g in groups]
    if recs and any(a['moment'] == ('editor',) for a in req['at']):
        raise Refused('cas: `editor` and a page in one request: the go-to starts as soon as the editor is up; give the page only')
    table = record_table('cas') if recs else None
    for k, r in enumerate(recs):
        if r in CAS_TEMPLATE_RECORDS:
            raise Refused('cas: record %d is a template chooser (code 1003 / 1008): entering it is the game\'s template change, which the go-to never '
                          'drives (src\\cas_goto_core.h RefusePath; cas_reason 3)' % r)
        if r in CAS_LAST_ONLY and k + 1 < len(recs):
            raise Refused('cas: record %d (a %s item page) can only be the LAST page of a request: the go-to reaches it but never leaves it (its panel\'s '
                          'B is not proved to keep the superstar: src\\cas_goto_core.h PlanUp kNoPanel, cas_reason 16; CASARRIVE.md Fix 1)' % (r, CAS_LAST_ONLY[r]))
        if table is not None and r not in table:
            raise Refused('cas: record %d is not in the editor\'s flow table (%d records; py -I tools\\menus\\flow_graph.py --records DUMP --names lists them)' % (r, len(table)))
    row = mode_row('cas')
    plan.flags.append('--wwe2k14_launch=@cas')
    plan.cvars_needed += [('wwe2k14_launch', 'the @cas launch'), ('@cas', 'the Create a Superstar launch (src\\cas_launch.h)')]
    body, expect, slug = ['wait_state seq %d 3600 2' % row['seq']], [mode_table().expect_lines(row)[2]], 'cas_editor'
    arrive = body[0]
    if len(recs) > 1:
        # S110 SCENEMOMENTS: each page by `cvar` (the boot flag is not set), the route shape of src\cas_goto_core.h:245-253; `assert_state cas_goto 2`
        # first because cas_record is 0 both before an arrival and on record 0 (the HEAD tab)
        plan.cvars_needed += [('wwe2k14_cas_goto', 'a Create a Superstar page (src\\cas_goto.h)'),
                              (CAS_TARGET_STATE, 'several Create a Superstar pages in one request: the go-to\'s route states (lane CASARRIVE)')]
        plan.notes.append('pages: %s, each by `cvar wwe2k14_cas_goto R` (the go-to re-arms on a new value: src\\cas_goto.h Step)' % ', '.join(str(r) for r in recs))
        for r, group in groups:
            body += ['cvar wwe2k14_cas_goto %d' % r, 'wait_state %s %d 60 1' % (CAS_TARGET_STATE, r), 'wait_state cas_goto >=2 3000', 'log_state cas_reason',
                     'assert_state cas_goto 2', 'assert_state %s %d' % (CAS_RECORD_STATE, r)]
            body += take_lines(group)
            expect += [CAS_EXPECT_ARRIVED % (r, r), CAS_EXPECT_STATE % r]
        return body, 'create', 'cas_' + '_'.join(str(r) for r in recs), expect, 'assert_state %s %d' % (CAS_RECORD_STATE, recs[-1])
    if recs:
        r = recs[0]
        plan.flags.append('--wwe2k14_cas_goto=%d' % r)
        plan.cvars_needed.append(('wwe2k14_cas_goto', 'a Create a Superstar page (src\\cas_goto.h)'))
        expect.append(CAS_EXPECT_ARRIVED % (r, r))
        if exe and route_lint.exe_knows(exe, CAS_RECORD_STATE):
            # cas_record is 0 until the go-to arrives, then the record (a stop leaves it 0: the wait times out and on_fail quit ends the route)
            body += ['wait_state %s %d %d' % (CAS_RECORD_STATE, r, CAS_STATE_FRAMES), 'log_state cas_goto']
            arrive = body[-2]
            expect.append(CAS_EXPECT_STATE % r)
        else:   # no route state for the go-to on this exe: its own bound is waited out (a step not settled in 900 frames stops it, src\cas_goto.h:86)
            body.append('wait_frames %d' % CAS_FALLBACK_FRAMES)
            arrive = body[-1]
        slug = 'cas_%d' % r
    return body + take_lines(req['at']), 'create', slug, expect, arrive


def lower_menu(req, plan, exe):
    plan.cvars_needed.append((MENU_PAGE_MARK, 'the route line `page N` (src\\menu_goto.h, S110 GOTO)'))
    body, expect, done, group = ['wait_state menu 1 1200 30'], [], [], []   # the main menu is up (the match builder's own sync, lower_start)
    table = record_table('menu')

    def flush():
        nonlocal group
        body.extend(take_lines(group))
        group = []
    for a in req['at']:
        if group and a['moment'] == group[0]['moment']:
            group.append(a)
            continue
        flush()
        r = a['moment'][1]
        if table is not None and r not in table:
            raise Refused('menu: record %d is not in the front-end flow table (%d records; --wwe2k14_menu_goto_list=true lists them in the run log)' % (r, len(table)))
        if done:
            body.append('home')   # back to the main menu by the game's own B (src\menu_home.h) before the next page
        body.append('page %d' % r)
        if table is not None and table.get(r) is not None:
            body.append('wait_state menu %d 600 2' % table[r])   # the record's submenu / screen code is the menu word once it is up (game_atlas.md:84-90)
        body += ['log_state menu', 'log_state screen_mode']
        expect.append(MENU_EXPECT % r)
        done.append(r)
        group = [a]
    flush()
    return body, 'menu', '_'.join(str(r) for r in done) or 'main', expect, sync_line(body)


def lower_cold(req, plan, exe):
    name = req['scene']['name']
    row = mode_row(name)
    plan.flags.append('--wwe2k14_launch=@mode:%s' % name)
    plan.cvars_needed += [('wwe2k14_launch', 'the cold mode launch'), (MODE_MARK, 'the cold mode launch (src\\mode_launch.h)')]
    body = ['wait_state seq %d 3600 2' % row['seq']]
    # the coverage board's item, from the row's menu path: a creation suite, the 30 Years menu, else a front-end mode
    word = 'create' if row['path'].startswith('CREATIONS') else 'wm30' if row['path'].startswith('30 YEARS') else 'menu'
    pick = req['scene'].get('pick')
    if pick is None:
        return body + take_lines(req['at']), word, 'mode_' + name, mode_table().expect_lines(row), body[0]
    # S110 MODEPICK: the editor on wrestler `pick` (src\mode_pick.h): the pick's flag, its line, and its route state when the exe has it
    import route_lint
    plan.flags.append('--wwe2k14_mode_pick=%d' % pick)
    plan.cvars_needed.append(('wwe2k14_mode_pick', 'the wrestler pick (src\\mode_pick.h, S110 MODEPICK)'))
    expect = mode_table().expect_lines(row) + [PICK_EXPECT % (name, pick)]
    if exe and route_lint.exe_knows(exe, PICK_ID_STATE):
        body += ['wait_state %s 2 %d' % (PICK_STATE, PICK_FRAMES), 'assert_state %s %d' % (PICK_ID_STATE, pick)]
        arrive = body[-2]
    else:
        body.append('wait_frames %d' % PICK_FALLBACK_FRAMES)
        arrive = body[-1]
    return body + take_lines(req['at']), word, 'mode_%s_%d' % (name, pick), expect, arrive


def lower_mode(req, exe=None, data=None):
    """A wm30 / cas / menu / mode request -> (route text, flags line, name stem): the match route's shape and trailer, plus `# arrival:` / `# expect:`."""
    sc = req['scene']
    exe = check_run(req, exe, data)
    h = hashlib.sha1(canonical(req['lines']).encode('utf-8')).hexdigest()
    plan = Plan()
    plan.door = sc['mode']
    body, word, slug, expect, arrive = {'wm30': lower_wm30, 'cas': lower_cas, 'menu': lower_menu, 'mode': lower_cold}[sc['mode']](req, plan, exe)
    flags = tail_flags(plan, sc['f'], req['run'])
    check_exe(exe, plan)
    stem = '%s_%s_%s' % (word, slug, h[:8])
    got = board_item(stem)
    if got != ITEM_BOARD[word]:
        raise Refused('internal: the route name %s would file on the coverage board as "%s", not "%s" (tools\\board\\coverage.py ITEM_RULES)' % (stem, got, ITEM_BOARD[word]))
    logged = canonical(req['lines']).replace('#', '%23')
    text = '\n'.join(
        ['# harness: flags ' + ' '.join(flags),
         '# intent: ' + ', '.join(plan.intent),
         '# Generated by tools\\run\\scene.py from the scene request below (docs\\systems\\run_contract.md "Scene requests"): do not edit, re-generate.',
         '# request: ' + logged.replace('\n', ' '),
         '# door: %s; fast-forwarded: every wait is on game state or guest frames' % ' '.join(plan.flags),
         '# arrival: ' + arrive,
         '# verdict: on_fail quit, so `bench: done` is reached only past the arrival line (run_read exits 0 only with it)' + (
             '; the arrival line is a frame count (no route state for it on this exe): the `# expect:` lines are the test' if arrive.startswith('wait_frames') else '')]
        + ['# ' + x for x in plan.notes]
        + ['# expect: ' + e for e in expect]
        + ['on_fail quit',
           'log bench: request ' + logged,
           'log bench: start scene ' + stem,
           'include boot_to_menu'] + body + ['log bench: done', 'wait_frames 120', 'quit']) + '\n'
    lint_text(text, ' '.join(flags), exe, data)
    return text, ' '.join(flags), stem


def describe(text, path=None):
    """The one JSON line a caller reads instead of the route: header flags, intent, the arrival line, the game lines to expect, the shots it takes."""
    import route_lint
    lines = text.split('\n')
    head = {ln.split(':', 1)[0][2:]: ln.split(': ', 1)[1] for ln in lines[:40] if re.match(r'^# (arrival|request): ', ln)}
    body = [ln for ln in lines if ln and not ln.startswith('#')]
    start = next((i for i, ln in enumerate(body) if ln.startswith('include ')), -1)
    end = next((i for i, ln in enumerate(body) if ln == 'log bench: done'), len(body))
    steps = body[start + 1:end]
    arrive = head.get('arrival') or sync_line(steps)
    shots = []
    for i, s in enumerate(steps):
        w = s.split()
        if w[0] == 'screenshot':
            shots.append(w[1])
        elif w[0] == 'shot':
            shots.append(w[3] if len(w) > 3 else '%s_%s' % (w[1], w[2]))
        elif w[0] == 'screenshot_every' and w[1] != '0' and i + 1 < len(steps) and steps[i + 1].startswith('wait_frames '):
            shots.append('%s x%d' % (w[2], int(steps[i + 1].split()[1]) // int(w[1])))
    return {'route': path, 'stem': next((ln.split('bench: start scene ', 1)[1] for ln in body if 'bench: start scene ' in ln), ''),
            'request': head.get('request', '').replace('%23', '#'), 'flags': route_lint.header_flags(text), 'intent': route_lint.intent(text) or [],
            'arrival': {'kind': 'frames' if arrive.startswith('wait_frames') else 'page' if arrive.startswith('page') else 'state', 'line': arrive,
                        'expect': [ln[len('# expect: '):] for ln in lines[:40] if ln.startswith('# expect: ')]},
            'verdict': 'bench: done', 'shots': shots, 'shot_count': sum(int(s.rsplit(' x', 1)[1]) if ' x' in s else 1 for s in shots)}


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
    import json
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--write', metavar='REQUEST|FILE')
    g.add_argument('--print', dest='show', metavar='REQUEST|FILE')
    g.add_argument('--describe', metavar='ROUTE', help='the JSON line of a route this tool wrote (what a caller of run_list\'s scene door reads)')
    ap.add_argument('--exe')
    ap.add_argument('--data')
    ap.add_argument('--out', metavar='PATH', help='with --write: a .txt file or a folder under out\\ (default out\\scenes\\<stem>.txt)')
    ap.add_argument('--json', action='store_true', help='one JSON line (describe()) instead of the route / its path')
    a = ap.parse_args(argv)
    try:
        out_root = os.path.join(ROOT, 'out')
        if a.out is not None and (a.write is None or os.path.commonpath([os.path.abspath(a.out), out_root]) != out_root):
            raise Refused('--out %s: a path under %s, with --write' % (a.out, out_root))
        if a.describe is not None:
            if not os.path.isfile(a.describe):
                raise Refused('--describe %s: no such route' % a.describe)
            with open(a.describe, encoding='utf-8') as fh:
                text = fh.read()
            if '# Generated by tools\\run\\scene.py' not in text:
                raise Refused('--describe %s: not a route this tool wrote (no `# Generated by` line)' % a.describe)
            print(json.dumps(describe(text, os.path.abspath(a.describe))))
        elif a.write is not None:
            if a.out is not None and a.out.lower().endswith('.txt'):
                text, _flags, _stem = lower(read_arg(a.write), a.exe, a.data)
                path = os.path.abspath(a.out)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, 'w', encoding='utf-8', newline='\n') as fh:
                    fh.write(text)
            else:
                path = write(read_arg(a.write), a.exe, a.data, os.path.abspath(a.out) if a.out is not None else None)
            if a.json:
                with open(path, encoding='utf-8') as fh:
                    print(json.dumps(describe(fh.read(), path)))
            else:
                print(path)
        else:
            text, flags, _ = lower(read_arg(a.show), a.exe, a.data)
            if a.json:
                print(json.dumps(describe(text)))
            else:
                print(text, end='')
    except Refused as x:
        print(json.dumps({'refused': str(x)}) if a.json else 'REFUSED: %s' % x)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
