"""S101 N1 (go-anywhere): address -> a plan of steps over the existing live verbs, and the executor.

    py -I tools/goto/resolve.py ADDRESS [--dry-run]       (goto.py is the front end; this is the library + a plan printer)

A plan is made with no game (offline): the atlas (docs/systems/game_atlas.json), the match builder's spec parser (tools/atlas/match.py),
and the SCEX moment codes of the 30 Years eras (tools/wm30/list.py --scex, kept under out\\goto\\scex.json: game data, never committed).
Outcome of resolving, three kinds, never silent:
    status "plan"           steps to run
    status "not_reachable"  a known gap, with the reason ("not reachable yet: <why>")
    status "error"          the address names something that does not exist (an atlas node that is not there, a match index with
                            no such moment, a spec the builder refuses): a loud failure, never a plan

Steps (dicts with "op"):
    nav          {target}                       tools/atlas/nav.py Nav.go (state-confirmed presses; the MCP game_nav)
    events_mark  {}                             remember the event bus position (tools/live/events.py) before the match is started
    match        {spec}                         the match builder's cvar batch, then wait for sequence 9 (tools/atlas/match.py run)
    wait_event   {kind, fields, timeout_s}      wait on the event bus (src/event_bus.h): entrance slot N, the bell (match clock_started),
                                                    lane 147: {kind: 'render', fields: {'what': 'roster_ready'}} = the renderer's wrestler roster is built (src/render_signal.h; also roster_failed)
    wm30_arm     {n, code, era}                 `!wm30 N [CODE]` (src/live_control.h -> src/wm30_jump.h LiveCommand) then the pick presses
    expect_log   {text, timeout_s}              the run log must show the game's own line (waits on a state, not on frames)
    wm30_fire    {bank, force}                  S101 lane 108 `!wm30 fire BANK [force]` in the running 30 Years match; retried while the game says busy (<= 600 s)
    expect_fire  {any, timeout_s}               the run log must show one of the [wm30] fire: lines (the flow states 25, then 14 / 19, then 10 / 8 and `back in the match (state 12)`)
    wm30_step    {step, force, steps, entry_free}  S101 lane 121 `!wm30 step K [force] [steps=..] [entry=none]`: the match script's step variable set to K; retried while busy (<= 600 s)
    passive      {slot, on}                     lane 161 `!director passive SLOT|all on|off` (src/situation_director.h): the COM word of a CPU wrestler set to 0 / back; retried while busy (<= 600 s)
    momentum     {slot, pct}                    lane 161 `!director momentum SLOT PCT`: the momentum meter (read-back in the answer and the log)
    stock        {slot, n}                      lane 161 `!director stock SLOT N`: the finisher stock
    passive_cpu  {on}                           lane 161 `!director passive_cpu on|off`: every CPU-driven wrestler goes passive at the bell, once per match (goto --passive-cpu)
    scene_menu   {id, pac}                      `!scene menu ID` (src/scene_director.h; main-menu exit action 5 with the scene id)
    scene_stage  {}                             S101 lane 38 `!scene stage` (menu 4025 mode 2: the Create a Story stage); retried while the answer says busy (<= 60 s)
    scene_play   {id, pac, performers, arena}   S101 lane 38 `!scene play ID [CHAR[/ATTIRE]...] [arena=N]` pushed into the ready stage; one request, not retried (the plan waits for 'stage: ready' first); arena -1 = the stage's own (76 = black)
    caw          {verb}                         S101 lane 118 the Create a Superstar editor's driver (tools/n2/caw_driver.py): one press a step, each proved by the drawn page; the editor must be open
    scene_leave  {}                             S101 lane 72 `!scene leave`: the editor's own cancel; the plan then waits for 'leave: back at the main menu'
    scene_mid    {ids}                          `!scene mid ID,ID` in the running match (mirror of sub_82738060); retried while the game says busy
    scene_victory {id, layout}                  `!scene victory ID[/L]` armed before the match; used once by the victory chooser
    expect_scene_log {text, timeout_s}          the run log must show the director's own [scene][director] line
Waits are always on an event, a state or a log line, never a frame count (the presses inside nav / match are the atlas's own
press_until_state loops).
"""
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))
from address import Address, AddressError, parse  # noqa: E402
import address  # noqa: E402
SHOW_ARENA_DEFAULT = 1  # S101 lane 77: RAW, for scenes made for 'the show's arena' and for an own arena without a pac


# The pick presses of the 30 Years start (src/wm30_jump.h kDrive's last line): A on the era card, then A on its first match until the
# game opens the era script file (picking a match opens EVTDSC<era>.PAC). The jump arms on the era load that follows.
WM30_PICK = 'press_until_file A EVTDSC 6 300 4'
# S101 Check J3 (run 7091): the proven route (s90_wm30.txt) presses A through the 30 Years intro until the match clock runs;
# a goto that only waits for the bell sits on the intro. The same line, sent as a batch after the jump is seen.
WM30_TO_BELL = 'press_until_state A match_clock !=0 60 600 5'
# S101 J3 investigation (wm30.md 'J3 result'): ~14 frames after the bell a 30 Years match raises an on-screen OK box that
# pauses the match (clock held at 15, Lua Action never runs). Press OK the way a player would: A only while the clock has not
# passed 30 (if_needed: no press when it is already running on).
WM30_CLEAR_BOX = ['wait_frames 60', 'press_until_state A match_clock >30 5 60 5 if_needed']

# N0-2: only X00 is known to run from the menu. X01 / X02 skip X00's setup (native 8 initialises the script variables), so a moment
# other than the match's own X00 is "not reachable yet" until one game check covers it (S101 review). The executor and the game
# side already carry a code; flip this when a check has run X01 / X02 from a test save.
MOMENTS_CHECKED = False

import stall_guard as SG  # noqa: E402  S101 lane 145: the stall watchdog
import when as W  # noqa: E402  S101 lane 102: the WHEN grammar over the event bus
# lane 150: after flow state 14 the match resumes through 10 (or 8 -> 9 -> 10) to 12; 21 is the match-END path, 15 the G+18576 == 9 branch [U]
POST14_FAIL = ['14 -> 21 (frame', '14 -> 24 (frame', '14 -> 4 (frame', '14 -> 15 (frame']
BACK_IN_MATCH = 'fire: back in the match (state 12)'
RESUME_NOTE = ('lane 150: after the resume (14 -> 10 -> 12) the match runs on; a run that does not hand the controller to a player (or pause) will see the CPU '
               'finish the match (run 7092: the CPU won by pinfall ~3500 frames later and the results menu came up)')

LIVE_BUDGET_FRAMES = 120000     # S101 lane 102: a live `match:SPEC@WHEN` gives up this many guest frames after the first bus line it sees (a singles match
RUMBLE_BUDGET_FRAMES = 400000   # ends in ~20-40k frames; GOANY_MOMENTS.md 2.5), and this many for a Rumble / Royal Rumble spec
LIVE_WAIT_S = 3600              # wall limit of a live wait_when (the poll reads the events file; a vanished game ends it earlier)

FINAL_WAIT_S = 120.0  # how long after the hand-over the run's hash row at F may take
POLL_S = 2.0 # S101 lane31: how often a moment_wait looks at the game, its log and its hash stream (the selftest makes it tiny)


def arm_arg(n, code):
    """The text after `!wm30 ` for a wm30_arm step. tools/goto/args_test.cpp feeds this to src/wm30_args.h (the game's parse rule)."""
    return '%d %d' % (n, code) if code else '%d' % n


def step_arg(step, force=False, steps=(), entry_free=False):
    """The text after `!wm30 ` for a wm30_step step. tools/goto/step_test.cpp feeds this to src/wm30_step_args.h (the game's parse rule)."""
    return 'step %d' % step + (' steps=%s' % ','.join(str(s) for s in steps) if steps else '') + (' entry=none' if entry_free else '') + (' force' if force else '')


DIRECTOR_OPS = ('passive', 'momentum', 'stock', 'passive_cpu')


def director_arg(s):
    """lane 161: the text after `!director ` for a passive / momentum / stock / passive_cpu step. tools/director/director_test.exe parse feeds it to
    src/situation_director_core.h (the game's parse rule); the input script takes the same text as a line."""
    op = s['op']
    if op == 'passive':
        return 'passive %s %s' % ('all' if s['slot'] == 'all' else int(s['slot']), 'on' if s['on'] else 'off')
    if op == 'momentum':
        return 'momentum %d %d' % (int(s['slot']), int(s['pct']))
    if op == 'stock':
        return 'stock %d %d' % (int(s['slot']), int(s['n']))
    if op == 'passive_cpu':
        return 'passive_cpu %s' % ('on' if s['on'] else 'off')
    raise ValueError('not a director op: %s' % op)


def director_step_from_text(text):
    """lane 161: `passive 1 on` / `momentum 2 100` / `stock 0 3` / `passive_cpu on` -> a plan step (a loud error for anything else)."""
    w = text.split()
    try:
        if w[0] == 'passive' and len(w) == 3 and w[2] in ('on', 'off'):
            return {'op': 'passive', 'slot': 'all' if w[1] == 'all' else int(w[1]), 'on': w[2] == 'on'}
        if w[0] == 'momentum' and len(w) == 3:
            return {'op': 'momentum', 'slot': int(w[1]), 'pct': int(w[2])}
        if w[0] == 'stock' and len(w) == 3:
            return {'op': 'stock', 'slot': int(w[1]), 'n': int(w[2])}
        if w[0] == 'passive_cpu' and len(w) == 2 and w[1] in ('on', 'off'):
            return {'op': 'passive_cpu', 'on': w[1] == 'on'}
    except (ValueError, IndexError):
        pass
    raise AddressError('--director %r is not passive SLOT|all on|off, momentum SLOT PCT, stock SLOT N or passive_cpu on|off' % text)


def add_director(plan, texts=(), passive_cpu=False):
    """lane 161: the plan's director steps. passive_cpu goes in before the match is started (a latch the game applies at the bell, once per match);
    each text is appended after the plan's last step (the place the plan reached). A plan that is not a plan is left alone."""
    if plan.status != 'plan':
        return plan
    if passive_cpu:
        at = next((i for i, s in enumerate(plan.steps) if s['op'] in ('match', 'wm30_arm')), len(plan.steps))
        plan.steps.insert(at, {'op': 'passive_cpu', 'on': True})
        plan.notes.append('lane 161: --passive-cpu: every CPU-driven wrestler is made passive at the bell (per-slot read-back in the run log: [director] lines)')
    for t in texts:
        plan.steps.append(director_step_from_text(t))
    return plan


def fire_arg(bank, force=False, actor=0, mode=0):
    """The text after `!wm30 ` for a wm30_fire step. tools/goto/fire_test.cpp feeds this to src/wm30_fire_args.h (the game's parse rule)."""
    return 'fire %d' % bank + (' actor=%d' % actor if actor else '') + (' mode=%d' % mode if mode else '') + (' force' if force else '')


def era_accepts(d, era, code):
    """The game side's rule (src/wm30_jump.h EraHasCode): a code is X00, X00 + 1 or X00 + 2 of some match of that era."""
    return any(m['era'] == era and 0 <= code - m['code'] <= 2 for m in d['matches'].values())


def main_checkout():
    """The checkout that holds the gitignored out\\ and game_data\\ (this tree, or the one above .claude/worktrees/<name>)."""
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    if os.path.isdir(os.path.join(REPO, 'game_data')):
        return REPO
    up = os.path.abspath(os.path.join(REPO, '..', '..', '..'))
    return up if os.path.isdir(os.path.join(up, 'game_data')) else REPO


SCEX_JSON = os.path.join(main_checkout(), 'out', 'goto', 'scex.json')


@dataclass
class Plan:
    address: str
    status: str                   # plan | not_reachable | error
    reason: str = ''
    steps: list = field(default_factory=list)
    notes: list = field(default_factory=list)

    def lines(self):
        head = {'plan': 'PLAN', 'not_reachable': 'NOT REACHABLE YET', 'error': 'ERROR'}[self.status]
        out = ['%s  %s%s' % (head, self.address, ('  -- ' + self.reason) if self.reason else '')]
        for i, s in enumerate(self.steps, 1):
            out.append('  %d. %s' % (i, describe_step(s)))
        out += ['  note: ' + n for n in self.notes]
        return out


def describe_step(s):
    op = s['op']
    if op == 'nav':
        return 'nav %s%s' % (s['target'], ('  (%s)' % s['label']) if s.get('label') else '')
    if op == 'events_mark':
        return 'mark the event bus position'
    if op == 'match':
        return 'build + launch match [%s]  (%s)' % (s['spec'], s.get('summary', ''))
    if op == 'wait_event':
        return 'wait on event %s %s (<= %ds)' % (s['kind'], json.dumps(s['fields']), s['timeout_s'])
    if op == 'wm30_arm':
        return 'arm the jump: !wm30 %d%s (era %d), then %s' % (s['n'], (' %d' % s['code']) if s.get('code') else '', s['era'], WM30_PICK)
    if op == 'wm30_fire':
        return 'fire scene bank %d in the running match: !wm30 %s (the game calls its own Lua 1003 with flag 0; retries while the game says busy, <= 600 s)' % (s['bank'], fire_arg(s['bank'], s.get('force', False), s.get('actor', 0), s.get('mode', 0)))
    if op == 'wm30_step':
        return ('set the running match script\'s step to %d: !wm30 %s (the game\'s own commit sub_82A9CAD8(267, K) of a next step; the next Action tick runs the step body; '
                'retries while the game says busy, <= 600 s)' % (s['step'], step_arg(s['step'], s.get('force', False), s.get('steps', ()), s.get('entry_free', False))))
    if op in DIRECTOR_OPS:   # lane 161
        return 'ask the situation director: !director %s (the game\'s own leaf call; the answer carries the read-back; retries while the game says busy, <= 600 s)' % director_arg(s)
    if op == 'expect_fire':
        return 'expect one of the game log lines %s (<= %ds)' % (' / '.join('"%s"' % a for a in s['any']), s['timeout_s'])
    if op == 'moment_start':
        return 'start the game in replay mode on moment "%s", hand over at guest frame %d%s' % (s['name'], s['until'], ' (hash stream tracked)' if s.get('det') else '')
    if op == 'moment_wait':
        base = 'wait for the hand-over at frame %d (<= %ds), comparing the hash stream on the way' % (s['until'], s['timeout_s']) if s.get('det') else \
            'wait for the hand-over at frame %d (<= %ds)' % (s['until'], s['timeout_s'])
        return base + (' and the bus lines (event track)' if s.get('events') else '')   # S101 lane 102
    if op == 'wait_when':   # S101 lane 102
        return 'wait for the bus line of %s (stops at: %s; gives up %d guest frames after the first bus line, or %ds)' % (
            s['when'], ', '.join(s['terminal']), s['budget_frames'], s['timeout_s'])
    if op == 'wm30_clear_box':
        return 'press OK on the match-start box if it holds the clock (J3 result): %s' % ' ; '.join(WM30_CLEAR_BOX)
    if op == 'wm30_to_bell':
        return 'press A through the 30 Years intro until the match clock runs: %s (as the route s90_wm30.txt)' % WM30_TO_BELL
    if op == 'expect_log':
        return 'expect the game log line "%s" (<= %ds)' % (s['text'], s['timeout_s'])
    if op == 'scene_menu':
        return 'play scene bank %d (%s) from the main menu: !%s' % (s['id'], s.get('pac', '?'), 'scene ' + scene_arg('menu', [s['id']]))
    if op == 'scene_stage':
        return 'build the scene stage: !scene stage (the Create a Story stage, bank 4025 held; retries while the game says busy)'
    if op == 'scene_play':
        return 'push scene bank %d (%s) into the stage: !scene %s' % (s['id'], s.get('pac', '?'), scene_arg('play', [s['id']], performers=s.get('performers', ()), arena=s.get('arena', -1)))
    if op == 'scene_leave':
        return 'leave the stage the game\'s own way: !scene leave (the editor\'s cancel; the game takes the stage down and returns to the main menu)'
    if op == 'scene_after':
        return 'queue scene banks %s for the end of the match: !scene %s (retries while the game says busy, <= 60 s)' % (s['ids'], scene_arg('after', s['ids']))
    if op == 'scene_mid':
        return 'post scene banks %s in the running match: !scene %s (retries while the game says busy, <= 60 s)' % (s['ids'], scene_arg('mid', s['ids']))
    if op == 'scene_victory':
        return 'arm the victory scene %d%s: !scene %s' % (s['id'], ('/' + str(s['layout'])) if s.get('layout', -1) >= 0 else '', scene_arg('victory', [s['id']], s.get('layout', -1)))
    if op == 'caw':   # S101 lane 118
        return 'drive the Create a Superstar editor: caw:%s (one press a step, each proved by the drawn page; the editor must be open; FAIL names expected vs seen)' % s['verb']
    if op == 'expect_scene_log':
        return 'expect the game log line "%s" (<= %ds)' % (s['text'], s['timeout_s'])
    return json.dumps(s)


class Context:
    """Everything a plan needs, loaded once and only when asked for (each loader fails loudly when its input is missing)."""

    def __init__(self, atlas=None, scex=None, scene_ids=None, scene_sets=None, scene_rows=None, route='auto', step_table=None):
        self._atlas, self._scex, self._scene_ids = atlas, scex, scene_ids
        self._scene_sets = scene_sets
        self._scene_rows = scene_rows
        self._scene_args = None
        self._step_table = step_table
        self.route = route   # S101 lane 102: a `match:SPEC@WHEN` goes 'auto' (a library recording of the same place if one has the event, else live) | 'live' | 'recorded'

    @property
    def scene_rows(self):
        """S101 lane 108: {match index: [bank ids the match's Lua chunk plays with 1003]} (tools/wm30/scene_table.py, read from the player's pacs; lane R1)."""
        if self._scene_rows is None:
            sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
            import scene_table
            game = os.path.join(main_checkout(), 'game_data', 'extracted')
            if not os.path.isdir(game):
                raise RuntimeError('no game files at %s: the scene table (which banks a match plays) cannot be read' % game)
            rows, _ = scene_table.build(game)
            self._scene_rows = {r['index']: [int(s['bank']) for s in r['scenes']] for r in rows if r['index'] is not None}
            # the 1003 call's real -3 / -2 arguments per bank (U1 = actor 1, U2 = actor 0 [reviewer-confirmed]; mode literal) and its flag
            self._scene_args = {r['index']: {int(s['bank']): dict(actor=1 if s.get('a4') == 'U1' else 0, mode=int(s['a5']) if str(s.get('a5', '0')).lstrip('-').isdigit() else 0,
                                                               flag=int(s['flag']) if str(s['flag']).isdigit() else 0) for s in r['scenes']} for r in rows if r['index'] is not None}
        return self._scene_rows

    @property
    def step_table(self):
        """S101 lane 121: {match index: {'steps': {N: {'free', 'entries', 'banks'}}, 'plays': {bank: step}}} (tools/wm30/step_table.py, read from the player's pacs)."""
        if self._step_table is None:
            sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
            import step_table
            game = os.path.join(main_checkout(), 'game_data', 'extracted')
            if not os.path.isdir(game):
                raise RuntimeError('no game files at %s: the step table (which steps a match script has) cannot be read' % game)
            self._step_table = step_table.build(game)
        return self._step_table

    @property
    def scene_args(self):
        """S101 lane 108: {match index: {bank: {actor, mode, flag}}} from the same table as scene_rows."""
        if self._scene_args is None:
            self.scene_rows
        return self._scene_args or {}

    @property
    def scene_sets(self):
        """S101 lane 77: {bank: {arena: N or None = the show's arena, ...}} (tools/goto/scene_sets.py, read from the player's pacs)."""
        if self._scene_sets is None:
            import scene_sets
            self._scene_sets = scene_sets.build()
        return self._scene_sets

    @property
    def scene_ids(self):
        """{'ids': {id: pac}, 'pacs': {...}}: the scene bank-id table (tools/goto/scene_ids.py; out\\goto\\scene_ids.json, made from the game files if absent)."""
        if self._scene_ids is None:
            import scene_ids
            self._scene_ids = scene_ids.table()
        return self._scene_ids

    @property
    def atlas(self):
        if self._atlas is None:
            import nav
            self._atlas = nav.Atlas()
        return self._atlas

    @property
    def scex(self):
        """{'eras': {era: [codes]}, 'matches': {index: {era, code, record}}} from out\\goto\\scex.json, made from the game files if absent."""
        if self._scex is None:
            if not os.path.isfile(SCEX_JSON):
                game = os.path.join(main_checkout(), 'game_data', 'extracted')
                if not os.path.isdir(game):
                    raise RuntimeError('the SCEX code list %s is missing and there are no game files at %s' % (SCEX_JSON, game))
                os.makedirs(os.path.dirname(SCEX_JSON), exist_ok=True)
                import subprocess
                r = subprocess.run([sys.executable, '-I', os.path.join(REPO, 'tools', 'wm30', 'list.py'), '--game', game, '--scex', SCEX_JSON],
                                   capture_output=True, text=True, timeout=300)
                if r.returncode != 0 or not os.path.isfile(SCEX_JSON):
                    raise RuntimeError('could not make %s: %s' % (SCEX_JSON, (r.stdout + r.stderr).strip()[-300:]))
            with open(SCEX_JSON) as f:
                d = json.load(f)
            if 'matches' not in d or 'eras' not in d:
                raise RuntimeError('%s has no "matches" / "eras" (made by an older list.py --scex): delete it' % SCEX_JSON)
            self._scex = {'eras': {int(k): v for k, v in d['eras'].items()}, 'matches': {int(k): v for k, v in d['matches'].items()}}
        return self._scex


def _menu(addr, ctx):
    atlas = ctx.atlas
    target = addr.body
    hits = [target] if target in atlas.nodes else atlas.resolve(target)
    if not hits:
        return Plan(str(addr), 'error', 'no atlas node or path name matches "%s" (tools/atlas/nav.py names WORDS lists them)' % target)
    if len(hits) > 1:
        return Plan(str(addr), 'error', 'ambiguous: %s' % ', '.join('%s (%s)' % (h, atlas.names.get(h)) for h in hits[:12]))
    nid = hits[0]
    name = atlas.names.get(nid, nid)
    if atlas.nodes.get(nid, {}).get('dead_end'):
        return Plan(str(addr), 'not_reachable', '%s (%s) is a dead end: the game has no known way back from it (game_atlas.md "Open")' % (nid, name))
    if atlas.plan('list:1', nid) is None and nid != 'list:1':
        return Plan(str(addr), 'not_reachable', '%s (%s) has no edge path from the main menu in the atlas' % (nid, name))
    return Plan(str(addr), 'plan', steps=[{'op': 'nav', 'target': nid, 'label': name}])


def scene_arg(verb, ids, layout=-1, mode=None, performers=(), arena=-1):
    """The text after `!scene ` for a scene step. tools/goto/scene_args_test.cpp feeds this to src/scene_director_args.h (the game's parse rule)."""
    if verb == 'stage':
        return 'stage'
    if verb == 'leave':  # S101 lane 72
        return 'leave'
    if verb == 'play':  # S101 lane 38: performers = ((character, attire), ...), attire -1 = not given
        return 'play %d' % ids[0] + ''.join(' %d/%d' % (c, a) if a >= 0 else ' %d' % c for c, a in performers) + (' arena=%d' % arena if arena >= 0 else '')
    if verb == 'victory':
        return 'victory %d' % ids[0] + ('/%d' % layout if layout >= 0 else '')
    if verb == 'menu':
        return 'menu %d' % ids[0] + (' mode=%d' % mode if mode is not None else '')
    return '%s %s' % (verb, ','.join(str(i) for i in ids))


def _bank_problem(ctx, ids):
    """'' if every bank id is held by a scene pac, else the reason (an id no pac holds is an ERROR, never a plan: the game would drop it)."""
    t = ctx.scene_ids
    missing = [i for i in ids if str(i) not in t['ids']]
    if missing:
        return 'scene bank id%s %s %s in no scene pac (docs/formats/evt_scene_ids.md: ranges <1000 Entrance, 1010-2110 Season, 4000-4670 WSD, 5000-5661 Universe, 6100-9500 Winning; the game would drop the id)' % (
            's' if len(missing) > 1 else '', ', '.join(str(i) for i in missing), 'are' if len(missing) > 1 else 'is')
    return ''


def _scene_leave(addr):
    """S101 lane 72: scene:leave = `!scene leave`, then wait for the director's own 'back at the main menu' line (the watcher's end, <= 1800 guest frames)."""
    return Plan(str(addr), 'plan', steps=[{'op': 'scene_leave'}, {'op': 'expect_scene_log', 'text': 'leave: back at the main menu', 'timeout_s': 180}],
                notes=['needs a stage built by `scene:ID` / `!scene stage` and a scene playing or held (the guards refuse otherwise, with the reason)',
                       'the editor\'s own cancel (story object exit code 3 + its slot 7): the game stops its loader objects and the sequence returns to the main menu; docs/systems/goto.md "Leaving the stage"'])


def _scene(addr, ctx):
    if addr.leave:
        return _scene_leave(addr)
    try:
        bad = _bank_problem(ctx, addr.scene_ids)
    except RuntimeError as x:
        return Plan(str(addr), 'error', str(x))
    if bad:
        return Plan(str(addr), 'error', bad)
    sid = addr.scene_ids[0]
    if not (1 <= sid <= 699 or 4000 <= sid <= 5999):  # S101 lane 59: scene_stage::HoldsInStage (sub_8231C538); docs/research/s101/STAGE_CRASH.md
        return Plan(str(addr), 'not_reachable', 'bank %d does not hold in the Create a Story stage (only 1-699 and 4000-5999 get the story command class): it would end the stage without '
                    'the game\'s orderly stop and the main menu then crashes on the story\'s stale loader objects (run 7066)' % sid)
    pac = ctx.scene_ids['ids'][str(sid)]
    notes = ['bank %d is in %s' % (sid, pac),
             'S101 lane 38, the stage push (docs/systems/goto.md "Stage push"): `!scene stage` builds the Create a Story stage from the main menu (menu 4025, mode 2) and '
             'the game holds it; `!scene play` then does the editor preview handler\'s tail with a one-record story of ours; no match is needed',
             'the goto arrives when the game logs "play: dispatched" (the scene is on screen); only banks 1-699 and 4000-5999 hold the stage (the story command class); others are not_reachable ']
    arena = addr.arena
    if arena < 0:  # S101 lane 77: the set the game's own editor uses for this scene (docs/formats/scene_location_catalogue.md)
        want = ctx.scene_sets.get(sid, {}).get('arena')
        if want is None:
            arena = SHOW_ARENA_DEFAULT
            notes.append("the show's arena: this scene is made for whichever arena the show is in; RAW (arena %d) by default, pass @arena:N for another" % arena)
        elif want not in address.LOADABLE_ARENAS:
            arena = SHOW_ARENA_DEFAULT
            notes.append("the scene's own arena %d has no BG%02d.PAC: RAW (arena %d) instead, pass @arena:N for another" % (want, want, arena))
        else:
            arena = want
            notes.append("arena %d: the scene's own set from the game's catalogue (scene header location + event flags, docs/formats/scene_location_catalogue.md); pass @arena:N to override" % arena)
    else:
        notes.append('arena %d: the game\'s own stage check reloads the set from BG%02d.PAC when the scene starts (log: G+64 / SL+36 in the stage lines)' % (addr.arena, addr.arena))
    if not addr.performers:
        notes.append('no performers given: the stage plays the scene with no wrestler assigned; add :CHAR[/ATTIRE],... (character ids from db/ids/characters.json) for wrestlers')
    if pac.startswith('Winning'):
        notes.append('a victory scene needs its winner as the first performer')
    if pac.startswith('Season'):
        notes.append('a 30 Years scene may need its era\'s script state [U] (S-3)')
    steps = [{'op': 'nav', 'target': 'list:1', 'label': 'MAIN MENU'}, {'op': 'scene_stage'},
             {'op': 'expect_scene_log', 'text': 'stage: ready', 'timeout_s': 180},
             {'op': 'scene_play', 'id': sid, 'pac': pac, 'performers': [list(p) for p in addr.performers], 'arena': arena},
             {'op': 'expect_scene_log', 'text': 'play: dispatched scene %d ' % sid, 'timeout_s': 90}]
    return Plan(str(addr), 'plan', steps=steps, notes=notes)


def _match_scene(addr, ctx, spec, summary):
    """match:SPEC@scene:IDS and match:SPEC@victory:ID."""
    steps = []
    notes = []
    try:
        if addr.scene_ids:
            bad = _bank_problem(ctx, addr.scene_ids)
        else:
            bad = _bank_problem(ctx, (addr.victory,))
            if not bad and ctx.scene_ids['ids'][str(addr.victory)][:7] != 'Winning':
                bad = 'scene %d is in %s, not a Winning pac: the victory chooser only ever picks Winning ids (6100-9500)' % (addr.victory, ctx.scene_ids['ids'][str(addr.victory)])
    except RuntimeError as x:
        return Plan(str(addr), 'error', str(x))
    if bad:
        return Plan(str(addr), 'error', bad)
    steps.append({'op': 'nav', 'target': 'list:1', 'label': 'MAIN MENU'})
    if addr.victory >= 0:
        # Armed before the match starts; the chooser runs when the match is decided. The match must be played to a finish (CPU vs CPU,
        # quick pace), and the ladder must reach its final test for this match [U].
        steps.append({'op': 'scene_victory', 'id': addr.victory, 'layout': addr.layout})
        steps.append({'op': 'events_mark'})
        steps.append({'op': 'match', 'spec': spec, 'summary': summary})
        steps.append({'op': 'expect_scene_log', 'text': 'victory: forced scene %d ' % addr.victory, 'timeout_s': 1800})
        notes.append('the match has to be played to its finish for the chooser to run (a CPU vs CPU quick pace match, or play it); it waits up to 30 min')
        notes.append('the performer layout is the ladder\'s own unless /L is given (per-wrestler 6000-6999 ids use 4, verified at 0x827C4C68); other families unconfirmed')
    elif addr.after:
        steps.append({'op': 'events_mark'})
        steps.append({'op': 'match', 'spec': spec, 'summary': summary})
        steps.append({'op': 'wait_event', 'kind': 'match', 'fields': {'what': 'clock_started'}, 'timeout_s': 600})
        steps.append({'op': 'scene_after', 'ids': list(addr.scene_ids)})
        steps.append({'op': 'expect_scene_log', 'text': 'after: the flow created the scene manager', 'timeout_s': 1800})
        notes.append('queued in the running match after the bell (G+472 == 0, R == 0): the scenes play when the match ends (flow state 25: sub_827C18E0 tests G+468); the match has to be played to its finish, it waits up to 30 min')
    else:
        steps.append({'op': 'events_mark'})
        steps.append({'op': 'match', 'spec': spec, 'summary': summary})
        steps.append({'op': 'wait_event', 'kind': 'match', 'fields': {'what': 'clock_started'}, 'timeout_s': 600})
        steps.append({'op': 'scene_mid', 'ids': list(addr.scene_ids)})
        notes.append('posted in the running match after the bell (R must be 0 and G+472 == 0): S101 check B: this PRELOADS (the ring is written, nothing reads it); use @after: to have the game play the scene')
    notes.append('events need the game started with --wwe2k14_events (tools/live/live.py start does)')
    return Plan(str(addr), 'plan', steps=steps, notes=notes)


RAW_MAX_PARTS = 6  # participants in a p= list (the builder has six slots)
RAW_KEYS = ('type', 'p', 'arena', 'pace', 'o', 'title', 'rumble', 'pods')  # the game's numeric builder spec (src/scenario.h ParseBuildSpec)


def raw_spec(body):
    """The numeric builder spec a case zip's manifest carries (`type=0;p=343:0,324:-1;arena=4;pace=1`, the wwe2k14_match cvar) -> (spec, summary),
    or None when `body` is not that form (S101 lane 32: where.address of a case zip resolves back through goto)."""
    parts = [p for p in body.split(';') if p.strip()]
    if not parts or not body.strip().startswith('type='):
        return None
    seen = set()
    for p in parts:
        k, eq, v = p.partition('=')
        k, v = k.strip(), v.strip()
        if not eq or k not in RAW_KEYS or not v or not re.fullmatch(r'[0-9:,/\-]+', v):
            return None
        # the builder's own limits (src/scenario.h ParseBuildSpec / the slots): raised as a refusal, so a bad spec is an error, not a plan
        if k in seen:
            raise ValueError('key %s twice' % k)
        seen.add(k)
        if k == 'type' and not re.fullmatch(r'\d{1,3}', v):
            raise ValueError('type=%s is not a match type number' % v)
        if k == 'arena' and not re.fullmatch(r'\d{1,3}', v):
            raise ValueError('arena=%s is not an arena number' % v)
        if k == 'pace' and not re.fullmatch(r'[012]', v):
            raise ValueError('pace=%s: 0 normal, 1 quick, 2 epic' % v)
        if k == 'p':
            items = [x for x in v.split(',') if x]
            if len(items) > RAW_MAX_PARTS:
                raise ValueError('p= has %d participants: a spec takes at most %d' % (len(items), RAW_MAX_PARTS))
            for x in items:
                m2 = re.fullmatch(r'(\d{1,3}):(-?\d)', x)
                if not m2 or int(m2.group(1)) > 499 or not -1 <= int(m2.group(2)) <= 3:
                    raise ValueError('p item "%s": ID:PAD with ID 0-499 and PAD -1..3' % x)
        if k == 'rumble':
            ids = [x for x in v.split(',') if x]
            if len(ids) > 40 or not all(re.fullmatch(r'\d{1,3}', x) and int(x) <= 499 for x in ids):
                raise ValueError('rumble= takes up to 40 character ids 0-499')
    return ';'.join(p.strip() for p in parts), 'raw builder spec %s' % body.strip()


def _match(addr, ctx):
    import match
    try:
        raw = raw_spec(addr.body)
        spec, summary = raw if raw else match.build_spec(addr.body, ctx.atlas)
    except (match.SpecError, ValueError) as x:
        return Plan(str(addr), 'error', 'match spec refused: %s' % x)
    if addr.scene_ids or addr.victory >= 0:
        return _match_scene(addr, ctx, spec, summary)
    if addr.when:
        return _match_when(addr, ctx, spec, summary)
    steps = []
    if addr.entrance >= 0 or addr.bell:
        steps.append({'op': 'events_mark'})
    steps.append({'op': 'nav', 'target': 'list:1', 'label': 'MAIN MENU'})
    steps.append({'op': 'match', 'spec': spec, 'summary': summary})
    if addr.entrance >= 0:
        steps.append({'op': 'wait_event', 'kind': 'entrance', 'fields': {'slot': addr.entrance}, 'timeout_s': 180})
    if addr.bell:
        steps.append({'op': 'wait_event', 'kind': 'match', 'fields': {'what': 'clock_started'}, 'timeout_s': 600})
    return Plan(str(addr), 'plan', steps=steps, notes=['events need the game started with --wwe2k14_events (tools/live/live.py start does)'] if len(steps) > 2 else [])


def fire_problem(ctx, addr):
    """S101 lane 108: '' if the match's own Lua chunk plays bank addr.fire with 1003 (lane R1's table), else why not. `/force` skips it."""
    if addr.fire_force:
        return ''
    try:
        banks = ctx.scene_rows.get(addr.n)
    except RuntimeError as x:
        return str(x)
    if banks is None:
        return 'match %d is not in the scene table' % addr.n
    if addr.fire not in banks:
        return ('match %d\'s Lua chunk never plays bank %d with 1003 (it plays %s); add /force to fire it anyway' % (addr.n, addr.fire, ', '.join(str(b) for b in banks) or 'no scene at all'))
    return ''


def step_plan(addr, ctx):
    """S101 lane 121: (K, force, steps, entry_free, bank, notes) for a wm30 @step:K / @moment:BANK address, or an error string.
    @step:K is the low-level form: K must be a step of the chunk, and a step behind an objective test needs /force (the objective is skipped).
    @moment:BANK names the step that plays BANK; a moment skips the objective by design, so force is carried by the plan itself."""
    try:
        t = ctx.step_table.get(addr.n)
    except RuntimeError as x:
        return str(x)
    if t is None:
        return 'match %d is not in the step table' % addr.n
    notes = []
    bank = -1
    if addr.moment_bank >= 0:
        bank = addr.moment_bank
        if bank not in t['plays']:
            return ('match %d\'s Lua chunk never plays bank %d with 1003 (it plays %s): there is no step to drive; wm30:%d@fire:%d/force fires it bare' %
                    (addr.n, bank, ', '.join(str(b) for b in sorted(t['plays'])) or 'no scene at all', addr.n, bank))
        k = t['plays'][bank]
    else:
        k = addr.step
        if k not in t['steps']:
            return 'step %d is not a step of match %d\'s script (its steps: %s)' % (k, addr.n, ', '.join(str(s) for s in sorted(t['steps'])))
    st = t['steps'][k]
    force = False
    if k == 0 and not addr.step_force:
        return 'step 0 is the script\'s init (preloads); add /force to re-run it'
    if not st['free']:
        path = st.get('path')
        skipped = '; '.join(path)[:400] if path else 'no setter path from step 0 is known'
        if addr.moment_bank >= 0:
            force = True
            notes.append('moment %d = step %d: the objective tests on the way from step 0 are skipped (%s); that is what a moment is for' % (bank, k, skipped))
        elif addr.step_force:
            force = True
            notes.append('step %d is behind objective tests; /force skips them (%s)' % (k, skipped))
        else:
            return 'step %d of match %d is behind objective tests (%s); add /force to skip them' % (k, addr.n, skipped)
    elif addr.step_force:
        force = True
        notes.append('step %d is reachable from step 0 with no test; /force only re-runs a step the script has already moved past' % k)
    if len(st['banks']) > 0 and addr.moment_bank < 0:
        notes.append('step %d plays scene bank %s' % (k, ', '.join(str(b) for b in st['banks'])))
    return k, force, sorted(t['steps']), st['free'], bank, notes


def _match_when(addr, ctx, spec, summary):
    """S101 lane 102: match:SPEC@WHEN. Recorded when the library holds a recording of the same place whose bus index has the event (arrives
    at the frame, before the event if asked); otherwise live (build the place, wait for the bus line: at or just after the event)."""
    import moments
    when_text = ''.join(addr.when)
    route = getattr(ctx, 'route', 'auto')
    try:
        clauses = W.parse(when_text)
    except W.WhenError as x:
        return Plan(str(addr), 'error', str(x))
    why_not = ''
    if route != 'live':
        try:
            hits, skipped = moments.find_moments(when_text, place=addr.body)
        except moments.MomentError as x:
            hits, skipped = [], [('library', str(x))]
        if hits:
            f, name, _ = hits[0]
            p = _moment(Address('moment', name, when=addr.when), ctx)
            if p.status == 'plan':
                p.address = str(addr)
                p.notes.insert(0, 'recorded: moment "%s" holds this place and %s at frame %d (--live builds the place and waits instead)' % (name, when_text, f))
                return p
            why_not = 'moment "%s" has the event but its plan fails: %s' % (name, p.reason)
        elif skipped:
            why_not = 'no indexed moment of this place has it (%d moment(s) skipped: %s)' % (len(skipped), skipped[0][1][:100])
        else:
            why_not = 'no indexed moment of this place has it'
        if route == 'recorded':
            return Plan(str(addr), 'error', '--recorded: %s' % why_not)
    if clauses[-1].offset < 0:
        return Plan(str(addr), 'error', 'arriving before an event needs a recording: none in the library matches (%s); run the harvest (a CPU-vs-CPU run of this place '
                    'in replay mode with --wwe2k14_events, then py -I tools/goto/moments.py add NAME REC --events EVENTS --place "%s")' % (why_not or 'live was forced', addr.body))
    names = {c.name for c in clauses}
    terminal = [] if 'finish' in names else ['finish']
    if 'seq' not in names:
        terminal.append('seq leaving 9')
    big = bool(re.search(r'rumble', spec, re.I))
    entrants = moments.bus.entrants_of({}, spec)
    steps = [{'op': 'events_mark'}, {'op': 'nav', 'target': 'list:1', 'label': 'MAIN MENU'}, {'op': 'match', 'spec': spec, 'summary': summary},
             {'op': 'wait_when', 'when': when_text, 'terminal': terminal, 'budget_frames': RUMBLE_BUDGET_FRAMES if big else LIVE_BUDGET_FRAMES,
              'timeout_s': LIVE_WAIT_S, 'entrants': entrants}]
    notes = ['live: arrives at or just after the event (a recording of this place, harvested once, arrives before it: %s)' % (why_not or 'forced live'),
             'events need the game started with --wwe2k14_events (tools/live/live.py start does)']
    # S101 lane 120: a Rumble needs no {of=N} (the bus carries `remaining` on every eliminated line); any other spec still does
    if any(c.name == 'remaining' for c in clauses) and not (big or re.search(r'rumble', addr.body, re.I)) and entrants is None and not any('of' in c.filters for c in clauses):
        return Plan(str(addr), 'error', '@remaining needs the entrant count: write {of=N} on the clause (or rumble=N in the spec)')
    if any(c.name in ('code', 'playing') for c in clauses):
        notes.append('@code / @playing read bus kinds (wm30, scene) that the game does not emit yet (lane L-4): the wait would end at the budget')
    return Plan(str(addr), 'plan', steps=steps, notes=notes)


def _wm30(addr, ctx):
    try:
        d = ctx.scex
    except RuntimeError as x:
        return Plan(str(addr), 'error', str(x))
    m = d['matches'].get(addr.n)
    if m is None:
        return Plan(str(addr), 'error', 'match %d is not in the 30 Years index (%d matches read)' % (addr.n, len(d['matches'])))
    era, own = m['era'], m['code']
    codes = d['eras'][era]
    code, notes = 0, []
    if addr.moment >= 0:
        code = own + addr.moment if addr.moment < 3 else addr.moment
        if code not in codes:
            return Plan(str(addr), 'not_reachable',
                        'era %d has no SCEX moment code %d for match %d (own codes %s)' % (era, code, addr.n, [c for c in (own, own + 1, own + 2) if c in codes]))
        if code > 65535 or not era_accepts(d, era, code):
            return Plan(str(addr), 'not_reachable',
                        'moment code %d is in era %d\'s script but is no match\'s X00 / X01 / X02: the game side refuses it (the game keeps 16 bits; an unknown code may soft-lock the loading screen)' % (code, era))
        if code != own and not MOMENTS_CHECKED:
            return Plan(str(addr), 'not_reachable',
                        'moment code %d is not the match\'s X00 %d: needs X00\'s setup (native 8 initialises the script variables), unchecked' % (code, own))
        if code != own:
            notes.append('starting code %d (not the match\'s X00 %d): N0-2 says only X00 is known to run from the menu; X01 / X02 alone are untested' % (code, own))
        if code % 100 == 2:
            notes.append('X02 writes progress flags and unlocks into the save (N0-2): run it on a test save only')
    steps = [{'op': 'nav', 'target': 'mode:wm30', 'label': '30 YEARS OF WRESTLEMANIA'},
             {'op': 'wm30_arm', 'n': addr.n, 'code': code if code != own else 0, 'era': era},
             {'op': 'expect_log', 'text': 'jump to match %d ' % addr.n, 'code': code or own, 'timeout_s': 90}]
    if addr.step >= 0 or addr.moment_bank >= 0:
        sp = step_plan(addr, ctx)
        if isinstance(sp, str):
            return Plan(str(addr), 'error', sp)
        k, force, real_steps, free, bank, snotes = sp
        notes.extend(snotes)
        plays = bool(ctx.step_table[addr.n]['steps'][k]['banks'])
        flag1 = plays and ctx.step_table[addr.n].get('flags', {}).get(ctx.step_table[addr.n]['steps'][k]['banks'][0], 0) == 1
        steps.insert(3, {'op': 'events_mark'})
        steps.append({'op': 'wm30_to_bell'})   # S101 Check J3
        steps.append({'op': 'wait_event', 'kind': 'match', 'fields': {'what': 'clock_started'}, 'timeout_s': 600})
        steps.append({'op': 'wm30_clear_box'})   # S101 J3 result
        steps.append({'op': 'wm30_step', 'step': k, 'force': force, 'steps': real_steps, 'entry_free': free})
        if flag1:  # S101 lane 121 review F1: a flag-1 (end-of-match) scene has no kind-6 tail: the flow goes 25 -> 19 and the match ends
            steps.append({'op': 'expect_fire', 'any': ['-> 25 (frame'], 'fail': [], 'timeout_s': 600})
            steps.append({'op': 'expect_fire', 'any': ['25 -> 19 (frame'], 'fail': ['25 -> 14 (frame'], 'timeout_s': 600})
            notes.append('this step plays a flag-1 (end-of-match) scene: the flow goes 25 -> 19, the post-match path; the match ENDS after the scene (it does not resume)')
        elif plays:
            steps.append({'op': 'expect_fire', 'any': ['-> 25 (frame'], 'fail': [], 'timeout_s': 600})
            steps.append({'op': 'expect_fire', 'any': ['25 -> 14 (frame'], 'fail': ['25 -> 19 (frame'], 'timeout_s': 600})
            steps.append({'op': 'expect_fire', 'any': ['14 -> 10 (frame', '14 -> 8 (frame'], 'fail': POST14_FAIL, 'timeout_s': 1800})  # lane 150: 14 -> 10 (or 8) is the resume
            steps.append({'op': 'expect_fire', 'any': [BACK_IN_MATCH], 'fail': POST14_FAIL, 'timeout_s': 600})  # lane 150: then 10 -> 12, the running match
        if plays and not flag1:
            notes.append(RESUME_NOTE)  # lane 150
        notes.append('S101 lane 121: the match script\'s step variable is set to %d once the bell has gone; the game then runs the step body itself (positioning, its own scene call, its own next step)' % k)
        if force:
            notes.append('forced: the script\'s own tests for this step were not met; the step list and entry rule are checked by the game (src/wm30_step_args.h)')
        notes.append('unconfirmed: a skipped-into step may rely on state earlier steps set (the 105 / 106 handshake, preloads); the match itself continues, the chunk may stall later')
        notes.append('needs the game started with --wwe2k14_live NAME')
        return Plan(str(addr), 'plan', steps=steps, notes=notes)
    if addr.fire >= 0:
        bad = fire_problem(ctx, addr)
        if bad:
            return Plan(str(addr), 'error', bad)
        real = ctx.scene_args.get(addr.n, {}).get(addr.fire) if not addr.fire_force else None
        actor, mode = (real['actor'], real['mode']) if real else (0, 0)
        if real and real['flag'] == 1:
            notes.append('bank %d is a flag-1 (end-of-match) scene in the Lua; it is fired with flag 0 so the kind-6 tail stays and the match resumes' % addr.fire)
        # the bus line proves a match is running before the first ask (the game answers busy until then anyway)
        steps.insert(3, {'op': 'events_mark'})
        steps.append({'op': 'wm30_to_bell'})   # S101 Check J3
        steps.append({'op': 'wait_event', 'kind': 'match', 'fields': {'what': 'clock_started'}, 'timeout_s': 600})
        steps.append({'op': 'wm30_clear_box'})   # S101 J3 result
        steps.append({'op': 'wm30_fire', 'bank': addr.fire, 'force': addr.fire_force, 'actor': actor, 'mode': mode})
        steps.append({'op': 'expect_fire', 'any': ['-> 25 (frame'], 'fail': [], 'timeout_s': 600})
        steps.append({'op': 'expect_fire', 'any': ['25 -> 14 (frame'], 'fail': ['25 -> 19 (frame'], 'timeout_s': 600})
        steps.append({'op': 'expect_fire', 'any': ['14 -> 10 (frame', '14 -> 8 (frame'], 'fail': POST14_FAIL, 'timeout_s': 1800})  # lane 150: 14 -> 10 (or 8) is the resume
        steps.append({'op': 'expect_fire', 'any': [BACK_IN_MATCH], 'fail': POST14_FAIL, 'timeout_s': 600})  # lane 150: then 10 -> 12, the running match
        notes.append(RESUME_NOTE)  # lane 150
        notes.append('S101 lane 108: the game\'s own Lua call 1003 (flag 0) is made once the match runs; the scene plays when the match\'s step ends (flow state 25), then the flow goes to 14 (resume) -- the one live check: the scene plays and the match goes on')
        notes.append('forced: the scene\'s objective was not met, and the Lua step around 1003 (wrestler positioning calls) does not run; the QTE question is in docs/systems/wm30.md "Objective scenes"')
        notes.append('needs the game started with --wwe2k14_live NAME')
    return Plan(str(addr), 'plan', steps=steps, notes=notes)


def _moment(addr, ctx):
    """S101 lane31: moment:NAME@frame:F / @event:KIND[:N]. Offline: the library's recording must parse, the frame must lie inside it (an
    address past the end, or naming an event the recording lacks, is an ERROR, not a plan)."""
    import moments
    try:
        meta, rec = moments.load_moment(addr.body)
        if rec.problems:
            return Plan(str(addr), 'error', 'the recording of moment "%s" does not validate: %s' % (addr.body, '; '.join(rec.problems[:3])))
        if addr.when:   # S101 lane 102: the bus index names the frame, offline
            frame, hit, _, _ = moments.when_frame(addr.body, ''.join(addr.when))
        else:
            frame = moments.resolve_frame(rec, addr.frame, addr.event_kind, addr.event_n)
    except moments.MomentError as x:
        return Plan(str(addr), 'error', str(x))
    d = moments.moment_dir(addr.body)
    bus_path = os.path.join(d, meta['bus_events']) if meta.get('bus_events') else None   # S101 lane 102: the event track reference
    det = os.path.join(d, meta['det']) if meta.get('det') else None
    if det:  # the reference must reach the frame, or the track check could not judge it: fail at plan time, not an hour in
        try:
            _, rrows, _ = moments.parse_det(det)
        except moments.MomentError as x:
            return Plan(str(addr), 'error', str(x))
        if not rrows or max(rrows) < frame:
            return Plan(str(addr), 'error', 'the moment\'s hash stream ends at frame %d, before frame %d: the track check cannot cover it '
                        '(pick an earlier frame, or add the moment again without --det)' % (max(rrows, default=0), frame))
    steps = [{'op': 'moment_start', 'name': addr.body, 'dir': d, 'until': frame, 'det': det, 'data': meta.get('data') or '', 'events': bus_path},
             {'op': 'moment_wait', 'name': addr.body, 'until': frame, 'det': det, 'timeout_s': 3600, 'events': bus_path}]
    notes = ['frame %d of %d (%s)' % (frame, rec.end_frame, ('%s: bus event at frame %d, offset %+d' % (''.join(addr.when), hit.event_frame, hit.offset)) if addr.when else
                                      'event %s:%d' % (addr.event_kind, addr.event_n) if addr.event_kind else 'given')]
    if bus_path:
        notes.append('event track: every bus line before frame %d must re-occur at its indexed frame (a missing or extra line stops the goto)' % frame)
    notes.append('the hash stream is compared on the way: the first slip stops the goto' if det else
                 'no hash stream for this moment: no track check (replay SLIP lines in the log are still read)')
    notes.append('starts its own game (replay mode, fast-forward to the frame, then the pad is yours); stop the running game first')
    return Plan(str(addr), 'plan', steps=steps, notes=notes)


NOT_YET = {
    'caw': 'the atlas has the mode (mode:caw) but no node for any Create-a-Superstar category or item (game_atlas.md "Open": deep screens are not nodes yet)',
    'universe': 'the atlas has mode:universe (screen 1497) but not its pages (game_atlas.md "Open")',
    'cutscene': 'no verb plays a cutscene / story scene by id: the Season scene banks (N0-2) need the viewer path or the Lua CallFunction 1002 / 1003 form, whose arguments are unconfirmed',
}


def _caw(addr, ctx):   # S101 lane 118
    """caw:... -> a one-step plan (the driver does the pressing); the names are checked against the learned page table (out\\n2\\caw_pages.json) when the verb needs it."""
    sys.path.insert(0, os.path.join(REPO, 'tools', 'n2'))
    import caw_driver as cd
    body = addr.body
    if not re.match(r'(?i)(tab:|page:|row:|back$)', body):   # the old CATEGORY/ITEM form
        cat, item = body.split('/')
        body = 'page:%s/row:%s' % (cat, item)
    try:
        actions = cd.parse_verb(body)
    except cd.CawError as x:
        return Plan(str(addr), 'error', str(x))
    notes = ['the editor must be open (sequence 76); the driver reads the page from the frame\'s text: needs the new renderer (owner mode) and `!caw on` (the driver sends it)',
             'one press per step; each step is proved by seq 76, the title, a full row set, exactly one hot row and the expected hot row / tab dot, held 4 frames',
             'a lost press is pressed once more; a wrong page is re-planned once; then FAIL (expected vs seen, the last 5 states, a screenshot)']
    if any(a[0] in ('page', 'row') for a in actions):
        table = cd.Table.load(cd.table_path())
        if not table.pages:
            return Plan(str(addr), 'not_reachable', 'no learned page table (%s): open the editor and run `py -I tools\\n2\\caw_driver.py learn` once (it walks the four tabs and every row; the table holds game text and stays under out\\)' % cd.table_path())
        problem = cd.check_names(table, actions)
        if problem:
            return Plan(str(addr), 'error', problem)
    return Plan(str(addr), 'plan', steps=[{'op': 'caw', 'verb': body}], notes=notes)


def resolve(addr, ctx=None):
    """Address (or its text) -> Plan. Never raises for a bad address: the Plan says what is wrong."""
    ctx = ctx or Context()
    if isinstance(addr, str):
        try:
            addr = parse(addr)
        except AddressError as x:
            return Plan(str(addr), 'error', 'malformed address: %s' % x)
    if addr.scheme == 'menu':
        return _menu(addr, ctx)
    if addr.scheme == 'match':
        return _match(addr, ctx)
    if addr.scheme == 'wm30':
        return _wm30(addr, ctx)
    if addr.scheme == 'scene':
        return _scene(addr, ctx)
    if addr.scheme == 'moment':
        return _moment(addr, ctx)
    if addr.scheme == 'caw':   # S101 lane 118
        return _caw(addr, ctx)
    return Plan(str(addr), 'not_reachable', NOT_YET[addr.scheme])


# ---- the executor (needs a running game with --wwe2k14_live NAME; run by the main session's check, not by the selftest) ----

class Stopped(Exception):
    """The goto could not arrive: the message says where the game is."""


def _where(name):
    try:
        import nav
        return nav.Nav.describe(nav.Nav(name).where())
    except Exception as x:  # the game may be gone: say so
        return 'position unknown (%s)' % x


def _era_to_top(name, say):
    """The pick presses A on the focused era card and then on its first match, and a locked match cannot be picked: put the era
    column's cursor on era 0 first, confirmed by the focused widget's row (the focus dump, tools/atlas/nav.py Nav.focus), not by a
    press count. [U]: the era column as a focus widget has never been read live. Stops when the cursor cannot be read or moved."""
    import nav
    n = nav.Nav(name)
    for _ in range(4):
        col = [w for w in n.focus() if w['rows'] > 1]
        if not col:
            # S101 Check J3 (runs 7089 / 7090): the era column never shows as a focus widget live. The proven route (s90_wm30.txt)
            # never moves the era cursor either: the armed jump (!wm30 N) decides the match at the pick, so with the all-open test
            # save any card works. Go on; the next step's "jump to match N" log line still proves the arrival (it stops otherwise).
            say('  era cursor not readable (no focused column widget): picking the focused card; the armed jump decides the match')
            return
        row = col[0]['row']
        if row == 0:
            say('  era cursor on the first card')
            return
        say('  era cursor on card %d: moving up' % (row + 1))
        _, _, failed = n.batch(['press UP', 'wait_frames 20'] * row)
        if failed:
            raise Stopped('moving the era cursor up failed: %s' % failed)
    raise Stopped('the era cursor did not reach the first card')


def _disarm(name):
    """Best effort after any stop that follows an arm: a failed pick must not leave the jump armed for the player's next pick."""
    try:
        import live
        live.request(name, "!wm30 off", connect_wait=3.0)
        live.request(name, "!scene off", connect_wait=3.0)
    except Exception:
        pass


def moment_flags(s):
    """The game switches of a moment_start step (S101 lane31; src/replay.h): replay mode on the library's recording, handing over at
    `until`, fast-forward, and the hash stream under the reference's own spec when there is a reference to compare with."""
    flags = ['--wwe2k14_replay_play=%s' % s['name'], '--wwe2k14_replay_dir=%s' % s['dir'], '--wwe2k14_replay_until=%d' % s['until'],
             '--vsync=false']
    if s.get('events'):
        flags.append('--wwe2k14_events=true')   # S101 lane 102: the replay's own bus lines, for the event track
    if s.get('det'):
        import moments
        spec = ''
        for ln in moments.parse_det(s['det'])[2]:
            m = re.search(r'\bspec=(\S+)', ln)
            if m:
                spec = m.group(1)
                break
        flags += ['--wwe2k14_det_hash=%s' % (spec or 'default,objects,sim'), '--wwe2k14_det_hash_v2=true']
    return flags


def _exe_knows(exe, flag):
    """True when the exe file holds the flag's name (the cvar is defined in this build). An exe without it ignores the flag silently
    (LESSONS), and the goto would wait for a hand-over that never comes. Chunked read: the exe is big."""
    needle = flag.encode()
    try:
        with open(exe, 'rb') as f:
            tail = b''
            while True:
                chunk = f.read(8 << 20)
                if not chunk:
                    return False
                if needle in tail + chunk:
                    return True
                tail = chunk[-len(needle):]
    except OSError as x:
        raise Stopped('cannot read the game exe %s: %s' % (exe, x))


def _moment_quit(name):
    """Best effort: the game this goto started holds a run-lock slot (up to 2 h) until it closes."""
    try:
        import live
        live.request(name, 'quit', connect_wait=2.0)
    except Exception:
        pass


def _moment_start(s, name, say):
    """Goes through the door's checks that matter here (tools/run/contract.py rule 3: every flag defined in the sources; the exe itself must
    hold the cvar) and mutes the game. tools/run/run.py cannot be used as is: it starts a route, and a moment needs the live channel."""
    import live
    flags = moment_flags(s) + ['--audio_mute=true']
    sys.path.insert(0, os.path.join(REPO, 'tools', 'run'))
    import contract as C
    cvars = C.load_cvars()
    parsed, bad = C.parse_flags(flags, 'moment_start')
    if bad:
        raise Stopped('moment_start: not --flag=value tokens: %s' % bad)
    unknown = [f.text() for f in parsed if f.bare not in cvars]
    if unknown:
        raise Stopped('moment_start: flags the sources do not define (the game would ignore them): %s' % ', '.join(unknown))
    exe = os.path.join(live.EXE_DIR, 'wwe2k14.exe')
    for need in ('wwe2k14_replay_until', 'wwe2k14_replay_play'):
        if not _exe_knows(exe, need):
            raise Stopped('the game exe %s does not hold the cvar %s: build it (this goto would wait for a hand-over that never comes)' % (exe, need))
    out = live.start(name, script='idle', fast=True, extra=' '.join(flags), data=s.get('data') or '')
    say('  ' + out)
    if not out.startswith('started'):
        _moment_quit(name)
        raise Stopped('the game did not start on moment "%s": %s' % (s['name'], out))


def _moment_wait(s, name, say):
    """Waits on the game's own lines, never a frame count. Every poll: the game must still answer (a vanished game stops it), the log
    is read line by line (a half-written line is carried to the next poll), the hash stream is compared with the reference up to the
    rows written. Stops: a SLIP line, a playback that cannot start / ended before F, no Init line naming F, the first track slip.
    On any stop the bookmark is taken and the game this goto started is closed. At the hand-over: vsync back on (the run was
    fast-forwarded), then the final track check waits for the run's row at F."""
    import live
    try:
        _moment_wait_inner(s, name, say, live)
    except Stopped as x:
        text = str(x)
        try:
            bm, _ = live.bookmark(name)
            text += '\n' + bm
        except Exception as e:
            text += '\n(bookmark not taken: %s)' % e
        _moment_quit(name)
        raise Stopped(text + '\n(the game this goto started was closed)')


def _moment_wait_inner(s, name, say, live):
    import moments
    run = int(live.parse(live.request(name, '!status', connect_wait=30))[0]['run'])
    log = live.run_log(run)
    csv_path = os.path.join(live.REPO, 'out', 'harness', 'determinism', 'det_%03d.csv' % run)
    t0 = time.time()
    end = t0 + s['timeout_s']
    checked, seen, carry = 0, 0, ''
    init_ok = False
    next_beat = t0 + 60
    handed = False

    ev_ref, ev_path = None, None
    if s.get('events'):   # S101 lane 102: the event track
        try:
            ev_ref = W.load_events(s['events'])
        except W.WhenError as x:
            raise Stopped('the moment\'s event index cannot be read: %s' % x)
        ev_path = os.path.join(os.path.dirname(log), 'events_%03d.jsonl' % run)

    def ev_track(final=False):
        if ev_ref is None:
            return
        if not os.path.isfile(ev_path):
            if final:
                raise Stopped('the run wrote no bus file %s (started without --wwe2k14_events=true?): the event track cannot finish' % ev_path)
            return
        try:
            run_ev = W.load_events(ev_path)
        except W.WhenError as x:
            if final:
                raise Stopped('the run\'s bus file cannot be read: %s' % x)
            return
        t = moments.bus.event_track(ev_ref, run_ev, s['until'], complete=final)
        if not t.ok:
            raise Stopped(t.line())

    def track(upto, final=False):
        nonlocal checked
        if not s.get('det'):
            return
        try:
            _, rows, _ = moments.parse_det(csv_path)
        except moments.MomentError as x:
            if final or os.path.isfile(csv_path):
                if final:
                    raise Stopped('the run\'s hash stream cannot be read: %s' % x)
            return
        have = max(rows) if rows else 0
        want = min(have, upto)
        if want > checked:
            t = moments.track_check(s['det'], csv_path, want, from_frame=checked + 1)
            if not t.ok:
                raise Stopped(t.line())
            checked = want

    while time.time() < end:
        try:  # liveness: the game must answer (it quits by itself when it crashes or the run ends)
            live.request(name, '!status', connect_wait=1.0)
        except live.NoGame:
            raise Stopped('the game stopped answering on the live channel before frame %d (crashed or closed); log %s' % (s['until'], log))
        try:
            with open(log, encoding='utf-8', errors='replace') as f:
                f.seek(seen)
                chunk = f.read()
                seen = f.tell()
        except OSError as x:
            raise Stopped('cannot read the run log %s: %s' % (log, x))
        text = carry + chunk
        lines = text.split('\n')
        carry = lines.pop()  # a half-written last line waits for the next poll
        for ln in lines:
            if '[replay] SLIP' in ln:
                raise Stopped('the recording slipped: %s' % ln.strip()[-200:])
            if 'FAILED to read' in ln or '[replay] playback stopped' in ln or 'no recorded answer' in ln:
                raise Stopped('the recording does not play: %s' % ln.strip()[-200:])
            if 'replay_until: hand over at frame %d ' % s['until'] in ln:
                init_ok = True
            if '[replay] playing ' in ln and not init_ok:
                raise Stopped('the game plays the recording but never logged "replay_until: hand over at frame %d": this exe ignores '
                              'the switch or the frame is past the recording' % s['until'])
            if 'recording ended at frame' in ln:
                raise Stopped('the recording ended before the hand-over (%s)' % ln.strip()[-160:])
            if 'recording stopped (replay_until) at frame' in ln:
                handed = True
        track(s['until'])
        ev_track()
        if handed:
            try:  # the run was fast-forwarded (vsync=false): the player's game runs at normal speed again
                live.run_batch(name, ['cvar vsync true'], 30)
            except Exception as x:
                say('  WARNING: could not set vsync back on (%s): the game keeps fast-forwarding' % x)
            fin = time.time() + FINAL_WAIT_S  # the run's row at F is written at the swap; wait for it, never judge a partial stream
            while s.get('det') and checked < s['until'] and time.time() < fin:
                try:
                    live.request(name, '!status', connect_wait=1.0)
                except live.NoGame:
                    raise Stopped('the game stopped answering after the hand-over, before its hash row at frame %d was written' % s['until'])
                track(s['until'], final=False)
                if checked < s['until']:
                    time.sleep(POLL_S)
            if s.get('det') and checked < s['until']:
                raise Stopped('the run\'s hash stream never reached frame %d (stops at %d): cannot finish the track check' % (s['until'], checked))
            if ev_ref is not None:   # the bus file may trail the swap by a flush: retry a missing line for a while, never pass it
                fin = time.time() + FINAL_WAIT_S
                while True:
                    try:
                        ev_track(final=True)
                        break
                    except Stopped:
                        if time.time() >= fin:
                            raise
                        time.sleep(POLL_S)
            say('  handed over at frame %d%s%s' % (s['until'], ' (hash stream matched to it)' if s.get('det') else '',
                                                     ' (bus lines matched to it)' if ev_ref is not None else ''))
            return
        if not init_ok and time.time() - t0 > 300:
            raise Stopped('no "replay_until: hand over at frame %d" line in 300 s: the game did not start the playback' % s['until'])
        if time.time() >= next_beat:
            say('  ... %d s, hash stream checked to frame %d of %d' % (time.time() - t0, checked, s['until']))
            next_beat += 60
        time.sleep(POLL_S)
    raise Stopped('the game did not reach frame %d within %d s' % (s['until'], s['timeout_s']))


def _wait_when(s, name, events, say):
    """S101 lane 102: tails the run's bus file (the Events object from events_mark), re-evaluating the WHEN over every line since the mark.
    Arrived on the predicate's line; a terminal event (the finish, leaving sequence 9) first, or the frame budget, or the wall limit is a loud
    stop that lists what the stream held. A positive offset waits that many more guest frames (wait_frames) after the line."""
    import live
    stream = []
    end = time.time() + s['timeout_s']
    first = None
    while time.time() < end:
        for e in events.read():
            try:
                stream.append(W.check_event(json.dumps(e), 'bus line'))
            except W.WhenError:
                continue
        if stream and first is None:
            first = stream[0]['f']
        try:
            state, what = W.live_state(stream, s['when'], s.get('entrants'))
        except W.WhenError as x:
            raise Stopped('the WHEN cannot be evaluated: %s' % x)
        if state == 'hit':
            say('  event: %s' % json.dumps(what.line))
            off = what.offset
            if off > 0:
                live.run_batch(name, ['wait_frames %d' % off], 60 + off // 30)
            return what
        if state == 'terminal':
            raise Stopped('%s did not happen before the match moved on: %s at frame %d came first. The stream held: %s' % (
                s['when'], json.dumps(what), what['f'], W.summary(stream)))
        if stream and stream[-1]['f'] - first > s['budget_frames']:
            raise Stopped('%s did not happen within %d guest frames of the first bus line (last bus line at frame %d). The stream held: %s' % (
                s['when'], s['budget_frames'], stream[-1]['f'], W.summary(stream)))
        try:
            live.request(name, '!status', connect_wait=1.0)
        except live.NoGame:
            raise Stopped('the game stopped answering while waiting for %s. The stream held: %s' % (s['when'], W.summary(stream)))
        time.sleep(0.2)
    raise Stopped('%s did not happen within %d s. The stream held: %s' % (s['when'], s['timeout_s'], W.summary(stream)))


def _stall_guard(name):
    """S101 lane 145: one Guard per goto run (3 auto-presses at most). WWE2K14_NO_STALL_GUARD=1 turns it into a no-op."""
    g = SG.Guard(name)
    if os.environ.get('WWE2K14_NO_STALL_GUARD'):
        g.tick = lambda say=print: None
    return g


def execute(plan, name='main', say=print):
    """Runs a plan. Returns normally on arrival; raises Stopped (with the game's position) otherwise."""
    if plan.status != 'plan':
        raise Stopped('%s: %s' % (plan.address, plan.reason))
    import live
    import nav
    events = None
    log_mark = 0  # run-log size when the jump was armed: expect_log reads only what came after
    armed = False
    scene_armed = False  # S101 lane 32: a victory scene armed and not yet used
    guard = _stall_guard(name)   # S101 lane 145: the stall watchdog for every log / event wait below (docs/systems/goto.md)

    def tick():
        try:
            guard.tick(lambda m: say('  ' + m))
        except SG.Stalled as x:
            raise Stopped(str(x))
    for i, s in enumerate(plan.steps, 1):
        say('step %d/%d: %s' % (i, len(plan.steps), describe_step(s)))
        op = s['op']
        try:
            if op == 'nav':
                for line in nav.Nav(name).go(s['target']):
                    say('  ' + line)
            elif op == 'events_mark':
                import events as ev_mod
                fields, _ = live.parse(live.request(name, '!status'))
                events = ev_mod.Events(int(fields['run']))
                events.skip_to_end()
            elif op == 'match':
                go = int(time.time()) % 1000000 + 1
                lines = ['cvar wwe2k14_match %s' % s['spec'], 'cvar wwe2k14_match_compare ""', 'cvar wwe2k14_match_go %d' % go,
                         'wait_state seq 9 3600 2', 'log_state seq']
                st, _, failed = nav.Nav(name).batch(lines, timeout=900)
                if failed:
                    raise Stopped('match build failed: %s' % failed)
            elif op == 'wm30_clear_box':   # S101 J3 result
                fields, _, _ = live.run_batch(name, WM30_CLEAR_BOX, 300)
                if fields.get('status') != 'ok':
                    raise Stopped('the match clock stayed held after pressing OK: %s %s (a box still up?)' % (fields.get('status'), fields.get('error', '')))
            elif op == 'wm30_to_bell':   # S101 Check J3
                fields, _, _ = live.run_batch(name, [WM30_TO_BELL], 900)
                if fields.get('status') != 'ok':
                    raise Stopped('pressing through the intro ended %s %s' % (fields.get('status'), fields.get('error', '')))
            elif op == 'wait_event':
                if events is None:
                    raise Stopped('internal: wait_event without events_mark')
                hit = events.wait(s['fields'] | {'kind': s['kind']}, s['timeout_s'], tick=tick)
                if hit is None:
                    raise Stopped('event %s %s did not come within %ds' % (s['kind'], s['fields'], s['timeout_s']))
                say('  event: %s' % json.dumps(hit))
            elif op == 'wm30_arm':
                # The log position first: without it expect_log could match a line from an older jump (stop, never guess 0).
                try:
                    log_mark = os.path.getsize(live.run_log(int(live.parse(live.request(name, '!status'))[0]['run'])))
                except (OSError, KeyError, ValueError) as x:
                    raise Stopped('cannot read the run log position (%s): expect_log would match an older line' % x)
                # Arm alone, and require the game to say it armed, before any press (a refused arm must not be followed by a pick).
                armed = True
                fields, _ = live.parse(live.request(name, '!wm30 ' + arm_arg(s['n'], s.get('code', 0))))
                say('  wm30: %s' % fields.get('wm30', '(no wm30 line in the answer)'))
                if fields.get('status') != 'ok' or not fields.get('wm30', '').startswith('armed'):
                    raise Stopped('the game did not arm the jump: status %s, %s' % (fields.get('status'), fields.get('wm30', fields.get('error', '?'))))
                _era_to_top(name, say)
                fields, _, _ = live.run_batch(name, [WM30_PICK], 180)
                if fields.get('status') != 'ok':
                    raise Stopped('pick batch ended %s %s' % (fields.get('status'), fields.get('error', '')))
            elif op == 'wm30_fire':
                try:
                    log_mark = os.path.getsize(live.run_log(int(live.parse(live.request(name, '!status'))[0]['run'])))
                except (OSError, KeyError, ValueError) as x:
                    raise Stopped('cannot read the run log position (%s): the expected log line could match an older one' % x)
                end = time.time() + s.get('timeout_s', 600)
                while True:
                    fields, _ = live.parse(live.request(name, '!wm30 ' + fire_arg(s['bank'], s.get('force', False), s.get('actor', 0), s.get('mode', 0)), connect_wait=15.0))
                    note = fields.get('wm30', '(no wm30 line in the answer)')
                    say('  wm30: %s' % note)
                    if 'busy:' in note and time.time() < end:  # loading, or the script has not preloaded yet: ask again
                        tick()
                        time.sleep(2.0)
                        continue
                    break
                if fields.get('status') != 'ok' or not note.startswith('fired'):
                    raise Stopped('the game did not accept the fire: status %s, %s' % (fields.get('status'), note))
            elif op in DIRECTOR_OPS:   # lane 161: one live request; busy (the bell has not gone, a scene) is asked again; anything but ok stops the plan
                end = time.time() + s.get('timeout_s', 600)
                while True:
                    fields, _ = live.parse(live.request(name, '!director ' + director_arg(s), connect_wait=15.0))
                    note = fields.get('director', '(no director line in the answer)')
                    say('  director: %s' % note)
                    if 'busy:' in note and time.time() < end:
                        tick()
                        time.sleep(2.0)
                        continue
                    break
                if fields.get('status') != 'ok':
                    raise Stopped('the situation director did not accept %s: %s' % (director_arg(s), note))
            elif op == 'wm30_step':
                try:
                    log_mark = os.path.getsize(live.run_log(int(live.parse(live.request(name, '!status'))[0]['run'])))
                except (OSError, KeyError, ValueError) as x:
                    raise Stopped('cannot read the run log position (%s): the expected log line could match an older one' % x)
                end = time.time() + s.get('timeout_s', 600)
                while True:
                    fields, _ = live.parse(live.request(name, '!wm30 ' + step_arg(s['step'], s.get('force', False), s.get('steps', ()), s.get('entry_free', False)), connect_wait=15.0))
                    note = fields.get('wm30', '(no wm30 line in the answer)')
                    say('  wm30: %s' % note)
                    if 'busy:' in note and time.time() < end:  # loading, the bell has not gone, or the script has not started: ask again
                        tick()
                        time.sleep(2.0)
                        continue
                    break
                if fields.get('status') != 'ok' or not note.startswith('stepped'):
                    raise Stopped('the game did not accept the step: status %s, %s' % (fields.get('status'), note))
            elif op == 'expect_fire':
                run = int(live.parse(live.request(name, '!status'))[0]['run'])
                path = live.run_log(run)
                end = time.time() + s['timeout_s']
                found = False
                while time.time() < end and not found:
                    try:
                        with open(path, encoding='utf-8', errors='replace') as f:
                            f.seek(log_mark)
                            for ln in f:
                                if '[wm30]' not in ln or 'fire:' not in ln:
                                    continue
                                if any(a in ln for a in s['any']):
                                    found = True
                                    break
                                if any(a in ln for a in s.get('fail', ())):
                                    raise Stopped('the flow left the match path: %s' % ln.strip()[-200:])
                                if 'failed' in ln or 'refused' in ln:
                                    raise Stopped('the fire stopped: %s' % ln.strip()[-200:])
                    except OSError as x:
                        raise Stopped('cannot read the run log %s: %s' % (path, x))
                    if not found:
                        tick()
                        time.sleep(1.0)
                if not found:
                    raise Stopped('the game never logged %s within %ds' % (' / '.join(s['any']), s['timeout_s']))
            elif op == 'expect_log':
                run = int(live.parse(live.request(name, '!status'))[0]['run'])
                path = live.run_log(run)
                want_code = ('moment code %d,' % s['code']) if s.get('code') else ''
                end = time.time() + s['timeout_s']
                found = False
                while time.time() < end and not found:
                    try:
                        with open(path, encoding='utf-8', errors='replace') as f:
                            f.seek(log_mark)
                            for ln in f:
                                if '[wm30]' not in ln:
                                    continue
                                if s['text'] in ln and want_code in ln:
                                    found = True
                                    break
                                if 'refused' in ln or 'not found in the game' in ln:
                                    raise Stopped('the game refused the jump: %s' % ln.strip()[-200:])
                    except OSError as x:
                        raise Stopped('cannot read the run log %s: %s' % (path, x))
                    if not found:
                        tick()
                        time.sleep(0.5)
                if not found:
                    raise Stopped('the game never logged "%s" with %s' % (s['text'], want_code or 'any code'))
                armed = False  # the hook used it
            elif op in ('scene_menu', 'scene_mid', 'scene_after', 'scene_victory', 'scene_stage', 'scene_play', 'scene_leave'):
                verb = {'scene_menu': 'menu', 'scene_mid': 'mid', 'scene_after': 'after', 'scene_victory': 'victory', 'scene_stage': 'stage', 'scene_play': 'play', 'scene_leave': 'leave'}[op]
                try:
                    log_mark = os.path.getsize(live.run_log(int(live.parse(live.request(name, '!status'))[0]['run'])))
                except (OSError, KeyError, ValueError) as x:
                    raise Stopped('cannot read the run log position (%s): the expected log line could match an older one' % x)
                ids = [s['id']] if op in ('scene_menu', 'scene_victory', 'scene_play') else [] if op in ('scene_stage', 'scene_leave') else s['ids']
                text = '!scene ' + scene_arg(verb, ids, s.get('layout', -1), s.get('mode'), [tuple(p) for p in s.get('performers', ())], s.get('arena', -1))
                end = time.time() + (60 if op in ('scene_mid', 'scene_after', 'scene_stage') else 0)
                while True:
                    fields, _ = live.parse(live.request(name, text, connect_wait=15.0))
                    note = fields.get('scene', '(no scene line in the answer)')
                    say('  scene: %s' % note)
                    # "busy:" = the game is in a state where this verb cannot run yet (entrances, a scene flow); only that is retried.
                    if op in ('scene_mid', 'scene_after', 'scene_stage') and 'busy:' in note and time.time() < end:
                        time.sleep(2.0)
                        continue
                    break
                ok = fields.get('status') == 'ok' and note.startswith(('fired', 'armed'))
                if not ok:
                    raise Stopped('the game did not accept the scene request: status %s, %s' % (fields.get('status'), note))
                scene_armed = op in ('scene_victory', 'scene_after')
            elif op == 'expect_scene_log':
                run = int(live.parse(live.request(name, '!status'))[0]['run'])
                path = live.run_log(run)
                end = time.time() + s['timeout_s']
                found = False
                while time.time() < end and not found:
                    try:
                        with open(path, encoding='utf-8', errors='replace') as f:
                            f.seek(log_mark)
                            for ln in f:
                                if '[scene][director]' in ln and s['text'] in ln:
                                    found = True
                                    break
                    except OSError as x:
                        raise Stopped('cannot read the run log %s: %s' % (path, x))
                    if not found:
                        tick()
                        time.sleep(1.0)
                if not found:
                    raise Stopped('the game never logged "%s" within %ds' % (s['text'], s['timeout_s']))
                scene_armed = False  # the hook used it
            elif op == 'wait_when':   # S101 lane 102
                if events is None:
                    raise Stopped('internal: wait_when without events_mark')
                _wait_when(s, name, events, say)
            elif op == 'caw':   # S101 lane 118: the editor driver (tools/n2/caw_driver.py); its own log lines, its own FAIL report
                sys.path.insert(0, os.path.join(REPO, 'tools', 'n2'))
                import caw_driver as cd
                try:
                    tbl = cd.Table.load(cd.table_path())
                    if not tbl.hash_ok():
                        raise cd.CawError('refused: the learned table does not match its own row-set hash: re-run caw_driver.py learn')
                    cd.run_verb(cd.LiveLink(name), tbl, s['verb'], log=lambda m: say('  ' + m))   # always ends with `!caw off`
                except cd.CawError as x:
                    raise Stopped(str(x))
            elif op == 'moment_start':
                _moment_start(s, name, say)
            elif op == 'moment_wait':
                _moment_wait(s, name, say)
            else:
                raise Stopped('unknown step op %s' % op)
        except Stopped:
            if armed or scene_armed:
                _disarm(name)
            raise
        except Exception as x:  # nav.Lost, live.NoGame, ...
            if armed or scene_armed:
                _disarm(name)
            raise Stopped('%s at step %d (%s): %s' % (type(x).__name__, i, op, x))
    say('arrived: %s' % plan.address)


def failure_report(name, text):
    """What a failed goto leaves behind: where the game is and the F11 bookmark bundle (screenshot, states, last 5 s, warnings).
    The case zip (the bug button, src\\case) is not on the live channel yet: open (docs/systems/goto.md)."""
    out = ['STOPPED: %s' % text, 'where: %s' % _where(name)]
    try:
        import live
        t, _ = live.bookmark(name)
        out.append(t)
    except Exception as x:
        out.append('bookmark not taken: %s' % x)
    return '\n'.join(out)


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    bad = 0
    for a in args or ['wm30:5@moment:1']:
        p = resolve(a)
        print('\n'.join(p.lines()))
        bad += p.status == 'error'
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
