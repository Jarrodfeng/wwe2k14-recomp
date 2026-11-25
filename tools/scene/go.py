r"""S110 SCENEGO: the ONE entry point of the scene tool (design: docs\research\s110\SCENETOOL.md 2.1-2.5; how an agent uses it: docs\agents\scene_tool.md).

    py -I tools\scene\go.py "REQUEST" [--exe tip|PATH] [--data NAME] [--print] [--live NAME] [--wait S] [--timeout S]

REQUEST is either
  * the structured scene itself (starts with `scene ` or `scene:`, lines joined by ` // `): passed straight to the lowering, or a FILE.scene;
  * looser words, resolved here part by part through the catalogue (`catalogue.py find`), never through a model:
        cas new at TOPS [: screenshot tops]
        cas new at TOPS : screenshot tops at MUSTACHE : screenshot mustache      (several ` at `: one after the other)
        Daniel Bryan vs Brock Lesnar at finish=pinfall      (a bus moment: pin, count=N, kickout, finish=NAME, move:class=finisher, replay ...)
        menu at OPTIONS
        mode threads
        wm30 Bret Hart vs Shawn Michaels  |  wm30 12
        Daniel Bryan vs Brock Lesnar in WrestleMania 29 [; type=tables] [at bell+300f : passive 1 on]
    A part that is unknown or ambiguous REFUSES the request (exit 2) with the nearest names of that part: fix the words and call again.
    A position with no items takes `screenshot arrived`. A DLC wrestler adds dlc=on by itself (the catalogue's WHERE says so).
--print     the resolved scene line, the arrival (its state line, the game lines to expect) and the lowered route; starts nothing (exit 0 / 2).
            `--exe tip` is previewed on the exe the tip pointer names (out\exe\tip.json); the run lowers again on the exe it really runs.
default     the question logged FIRST in out\scene\runs\runs.md, then ONE run through tools\bench\run_list.ps1 with the scene request itself
            (run_list's scene door lowers it on the exe it resolved: run lock, frozen-save mirror, a settings file under out\, fast-forward,
            window behind, muted; -Exe tip by default), read by tools\bench\run_read.py with EVERY expected game line, and the hand-back:
                SCENE    <the resolved scene line>
                ARRIVED  yes (frame F, line N: <the last expected game line>)   |   NO (<why: the failing line / the lines that never printed>)
                         |   NO (NOT ARRIVED: the match ended first, by <finish_name> ...): the finish ended a moment's wait (S110 SCENEMOM2)
                RUN      <log path>  <seconds> s  exe <exe>
                SHOTS    <paths> + sheet <path>
                LOG      PROBLEMS n [(first: ...)] | errors: known noise only (n groups) | NEW error groups n: "..." | SHORTFALLS n | UNSORTED n
                         (counted, not judged)  (full: py -I tools\bench\run_read.py <log>); the known boot groups: tools\scene\known_noise.py
                EXIT     0 arrived | 1 not arrived / run failed | 2 refused
            Arrived = the route reached `bench: done` (on_fail quit: only past the arrival wait) AND every `expect` line of the lowering printed.
--live NAME the same route lines as one batch into the live game NAME (tools\live\live.py run -f); refused when the route needs boot flags
            beyond the lowering's base set (a live game cannot take them after its start).
The two tools this one stands on are called ONLY from catalogue_find() and lower_scene() / describe_route() below: a change of their contract is
fixed there (tools\scene\go_selftest.py runs them for real; only run_list is a stand-in).
"""
# entry: main
import argparse
import datetime
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PY = sys.executable
CATALOGUE = os.path.join(ROOT, 'tools', 'scene', 'catalogue.py')
LOWER = os.path.join(ROOT, 'tools', 'run', 'scene.py')
RUN_LIST = os.path.join(ROOT, 'tools', 'bench', 'run_list.ps1')
RUN_READ = os.path.join(ROOT, 'tools', 'bench', 'run_read.py')
LIVE = os.path.join(ROOT, 'tools', 'live', 'live.py')
# go_selftest.py only (never set in normal use): a catalogue FILE (the real catalogue.py reads it), a python stand-in for run_list.ps1, the out folder
CATALOGUE_FILE = os.environ.get('SCENE_GO_CATALOGUE_FILE') or ''
RUNNER = os.environ.get('SCENE_GO_RUNNER') or ''
OUT = os.environ.get('SCENE_GO_OUT') or os.path.join(ROOT, 'out', 'scene')
# the lowering's own base flags (tools\run\scene.py BASE_FLAGS): the only header flags a live game can do without
LIVE_OK_FLAGS = {'--wwe2k14_fixed_seed', '--vulkan_stored_pipelines_at_boot', '--wwe2k14_events', '--wwe2k14_rdr_owner', '--audio_mute'}
# the catalogue kinds this tool resolves (tools\scene\catalogue.py KINDS)
KINDS = ('wrestler', 'arena', 'type', 'wm30', 'cas', 'menu', 'moment', 'mode')
MOMENT_RX = re.compile(r'^(arrived|bell|roster|finish(?:=[\w\-]+)?|victory|card|editor|pin|count(?:=\d)?|kickout(?:=\d)?|replay|replay_end|ring_break|table_break'
                       r'|(?:move|eliminated)(?::\w+=[\w\-]+)*|entrance:\d+(?::\d+)?|clock:\d+|seq:\d+|load:[\w.\-]+|page=\d+|\d+)$', re.I)
FRAME_RX = re.compile(r'\bframe[= ](\d+)|^\[wwe2k14\]\[event\] \{"f": (\d+)')   # a route line's `frame N`, or a bus line's own "f" (src\event_bus.h Emit)
# the position a request without `at` arrives at (tools\run\scene.py: a match / wm30 waits the bell by default, cas new the editor, a mode its
# sequence); a menu without a page has no position to put a take on
DEFAULT_POS = {'match': 'bell', 'wm30': 'bell', 'cas': 'editor', 'mode': 'arrived', 'stage': None, 'menu': None}
sys.path.insert(0, HERE)   # -I leaves the script's folder off sys.path
import known_noise  # noqa: E402  the LOG line against the boot errors every clean run prints (S110 SCENEMOMENTS)


class Refused(Exception):
    """The request cannot be served as written: the lines are printed under REFUSED (exit 2)."""

    def __init__(self, lines):
        Exception.__init__(self, '; '.join(lines))
        self.lines = list(lines)


# ------------------------------------------------------------------ the two contracts (lanes CATALOGUE and SCENEMODES)
def catalogue_find(words, kind, n=5):
    """-> (code, rows). Contract (tools\\scene\\catalogue.py find): ranked lines KIND<TAB>ID<TAB>NAME<TAB>ALIASES<TAB>WHERE (ALIASES ' | '),
    exit 0 = matches; exit 2 = no good match: a `# NO MATCH` line, then the nearest; exit 1 = no catalogue (Refused with its own words)."""
    cmd = [PY, '-I', CATALOGUE, 'find'] + words.split() + ['--kind', kind, '--n', str(n)] + (['--catalogue', CATALOGUE_FILE] if CATALOGUE_FILE else [])
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
    rows = []
    for ln in p.stdout.splitlines():
        if not ln.strip() or ln.startswith('#'):
            continue
        c = ln.split('\t')
        if len(c) == 5:
            rows.append({'kind': c[0], 'id': c[1], 'name': c[2], 'aliases': [a for a in c[3].split(' | ') if a], 'where': c[4]})
    if p.returncode not in (0, 2):
        tail = (p.stdout + p.stderr).strip().splitlines()
        raise Refused(['catalogue.py find %s --kind %s: exit %d: %s' % (words, kind, p.returncode, tail[-1].lstrip('# ') if tail else 'no output')])
    return p.returncode, rows


def scene_json(args):
    """One call of the lowering that prints ONE JSON line (--json / --describe) -> the dict. Its {"refused": WHY} -> Refused."""
    p = subprocess.run([PY, '-I', LOWER] + args, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=300)
    lines = [ln for ln in p.stdout.splitlines() if ln.strip()]
    try:
        meta = json.loads(lines[-1]) if lines else None
    except ValueError:
        meta = None
    if not isinstance(meta, dict):
        why = (p.stdout + p.stderr).strip().splitlines()
        raise Refused(['the lowering (tools\\run\\scene.py %s) printed no JSON line, exit %d: %s' % (args[0], p.returncode, why[-1] if why else 'no output')])
    if 'refused' in meta:
        raise Refused(['the lowering refuses: %s' % meta['refused']])
    a = meta.get('arrival')
    if not (isinstance(a, dict) and a.get('line') and isinstance(a.get('expect'), list) and meta.get('verdict') == 'bench: done'):
        raise Refused(['the lowering\'s JSON has no arrival {kind, line, expect[]} or no `bench: done` verdict: %s' % json.dumps(meta)[:300]])
    return meta


def lower_scene(scene, exe=None, data=None, route=False):
    """-> the lowering's JSON (tools\\run\\scene.py describe(): stem, flags[], arrival {kind, line, expect[]}, verdict, shots[]), plus 'text'
    = the route itself when route=True (a second call without --json: --print and --live show / send it)."""
    base = ['--print', scene] + (['--exe', exe] if exe else []) + (['--data', data] if data else [])
    meta = scene_json(base + ['--json'])
    if route:
        p = subprocess.run([PY, '-I', LOWER] + base, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=300)
        meta['text'] = p.stdout if p.returncode == 0 else ''
    return meta


def describe_route(path):
    """The JSON line of the route run_list's scene door wrote (lowered on the exe the run used)."""
    return scene_json(['--describe', path])


# ------------------------------------------------------------------ resolution of looser words
def norm(s):
    return ' '.join(s.lower().replace('_', ' ').split())


def resolve_part(words, kind, misses):
    """One name-like part -> its catalogue row, or None with a refusal line added to misses."""
    code, rows = catalogue_find(words, kind)
    if code == 2 or not rows:
        misses.append('"%s" (%s): unknown; nearest: %s' % (words, kind, ' | '.join('%s (%s)' % (r['name'], r['id']) for r in rows[:5]) or 'none'))
        return None
    q = norm(words)
    exact = [r for r in rows if q == norm(r['name']) or q == norm(r['id']) or q in [norm(a) for a in r['aliases']]]
    if len(exact) == 1:
        return exact[0]
    if len(rows) == 1:
        return rows[0]
    some = exact or rows
    misses.append('"%s" (%s): ambiguous, %d match; say which: %s' % (words, kind, len(some), ' | '.join('%s (%s)' % (r['name'], r['id']) for r in some[:5])))
    return None


def resolve_who(who, misses, dlc):
    out = []
    for name in [x.strip() for x in re.split(r'\s+vs\.?\s+|\s*,\s*', who, flags=re.I) if x.strip()]:
        m = re.match(r'^(.*?)(/\d)?$', name)
        base, attire = m.group(1).strip(), m.group(2) or ''
        if re.fullmatch(r'#\d+', base):
            out.append(base + attire)
            continue
        r = resolve_part(base, 'wrestler', misses)
        if r and 'dlc=on' in r['where']:
            dlc.append(r['name'])
        out.append(('#%s' % r['id'] if r else base) + attire)
    return ' vs '.join(out)


def resolve_position(pos, kind, misses):
    m = re.match(r'^(.*?)(\+\d+f)?$', pos.strip())
    base, off = m.group(1).strip(), m.group(2) or ''
    if MOMENT_RX.match(base):
        return base + off
    r = resolve_part(base, kind, misses)
    if not r:
        return base + off
    if kind in ('cas', 'menu'):
        if not r['id'].isdigit():
            misses.append('"%s" (%s): %s has no record id in the catalogue (%s): `py -I tools\\scene\\catalogue.py check` names the run that makes it'
                          % (base, kind, r['name'], r['where']))
            return base + off
        return 'page=%s' % r['id'] + off
    return r['id'] + off


def to_scene(request):
    """-> the structured scene line. Structured input passes straight; looser words are resolved part by part (Refused lists every bad part)."""
    t = ' '.join(request.split())
    if re.match(r'^scene[\s:]', t, re.I):
        return t
    misses = []
    place, items, ats = t, '', []   # ats: [(position, items)], one per ` at ` (S110 SCENEMOMENTS: several run one after the other)
    chunks = re.split(r'\s+at\s+', t, flags=re.I)
    if len(chunks) > 1:
        place = chunks[0]
        for rest in chunks[1:]:
            k = re.search(r'\s*:\s+|\s*:$', rest)
            ats.append((rest[:k.start()], rest[k.end():]) if k else (rest, ''))
    else:
        k = re.search(r'\s+:\s+', t)
        if k:
            place, items = t[:k.start()], t[k.end():]
    place = place.strip()
    low = place.lower()
    if re.match(r'^(cas|create a superstar)(\s+new)?$', low):
        head, pkind, mkind = 'cas new', 'cas', 'cas'
    elif low == 'menu':
        head, pkind, mkind = 'menu', 'menu', 'menu'
    elif low.startswith('wm30'):
        what = place[4:].strip()
        if re.fullmatch(r'\d+', what or 'x'):
            head = 'wm30 ' + what
        else:
            r = resolve_part(what, 'wm30', misses) if what else None
            if not what:
                misses.append('wm30: which match (a number 0-45 or its names: py -I tools\\scene\\catalogue.py find WORDS --kind wm30)')
            head = 'wm30 ' + (r['id'] if r else what)
        pkind, mkind = 'moment', 'wm30'
    elif re.match(r'^mode(\s|$)', low):
        what = place[4:].strip()
        r = resolve_part(what, 'mode', misses) if what else None
        if not what:
            misses.append('mode: which mode (py -I tools\\scene\\catalogue.py find WORDS --kind mode lists them)')
        if r and r['id'] == 'cas':
            head, pkind, mkind = 'cas new', 'cas', 'cas'
        else:
            head, pkind, mkind = 'mode ' + (r['id'] if r else what), 'moment', 'mode'
    elif re.match(r'^stage\s', low):
        head, pkind, mkind = place, 'moment', 'stage'
    else:
        parts = [x.strip() for x in place.split(';') if x.strip()]
        who = parts[0] if parts else ''
        fields = []
        mi = re.search(r'\s+in\s+(.+)$', who, re.I)
        if mi and '=' not in who:
            r = resolve_part(mi.group(1).strip(), 'arena', misses)
            fields.append('arena=%s' % (r['id'] if r else mi.group(1).strip()))
            who = who[:mi.start()]
        if '=' in who:
            fields.insert(0, who)
            who = ''
        for f in parts[1:]:
            key, _, val = f.partition('=')
            key, val = key.strip().lower(), val.strip()
            if key == 'arena' and val and not val.isdigit():
                r = resolve_part(val, 'arena', misses)
                f = 'arena=%s' % (r['id'] if r else val)
            elif key == 'type' and val and not val.isdigit():
                r = resolve_part(val, 'type', misses)
                f = 'type=%s' % (r['name'] if r else val)
            fields.append(f)
        dlc = []
        who_line = resolve_who(who, misses, dlc) if who.strip() else ''
        if dlc and not any(re.match(r'^dlc\s*=', f, re.I) for f in fields):
            fields.append('dlc=on')   # a DLC wrestler's seat needs it (the catalogue's WHERE; scene.py reads the DLC names only then)
        head = '; '.join([who_line] * bool(who_line) + fields)
        pkind, mkind = 'moment', 'match'
    if misses:
        raise Refused(misses)
    line = 'scene ' + head
    if ats:
        got = []
        for pos, _its in ats:
            if not pos.strip():
                misses.append('an `at` with no position: PLACE at POSITION [: ITEMS] [at POSITION ...]')
            got.append(resolve_position(pos, pkind, misses) if pos.strip() else '')
        if misses:
            raise Refused(misses)
        for k, (p, (_pos, its)) in enumerate(zip(got, ats)):   # no items = a screenshot named arrived (arrived_2, arrived_3 ... after the first)
            line += ' // at %s : %s' % (p, its.strip() or 'screenshot arrived' + ('_%d' % (k + 1) if k else ''))
    elif DEFAULT_POS[mkind]:
        line += ' // at %s : %s' % (DEFAULT_POS[mkind], items.strip() or 'screenshot arrived')
    elif items.strip():
        raise Refused(['"%s": items need a position: %s' % (place, 'menu at PAGE : ITEMS (a page label: catalogue.py find WORDS --kind menu)' if mkind == 'menu'
                                                            else 'PLACE at POSITION : ITEMS')])
    return line


# ------------------------------------------------------------------ the run (transport A: run_list's scene door) and the read
def ps_quote(s):
    return "'" + str(s).replace("'", "''") + "'"


def door_request(scene):
    """The scene line as run_list's scene door takes it (tools\\bench\\run_list_scene.ps1: an entry starting `scene:`)."""
    return 'scene:' + scene[len('scene'):] if re.match(r'^scene\s', scene, re.I) else scene


def run_command(scene, exe, data, wait):
    if RUNNER:
        return [PY, '-I', RUNNER, '--request', door_request(scene), '--exe', exe, '--data', data or '', '--wait', str(wait)]
    cmd = '& %s -Scripts @(%s) -Exe %s -WaitSeconds %d' % (ps_quote(RUN_LIST), ps_quote(door_request(scene)), ps_quote(exe), wait)
    if data:
        cmd += ' -Data %s' % ps_quote(data)
    return ['pwsh', '-NoProfile', '-NonInteractive', '-Command', cmd]


def kill_tree(p):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], capture_output=True)
    else:
        p.kill()


def run_list(scene, exe, data, wait, timeout):
    """-> (output text, seconds). A hard timeout kills the whole tree (run_list, its runner, the game)."""
    t0 = datetime.datetime.now()
    p = subprocess.Popen(run_command(scene, exe, data, wait), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    try:
        out, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        kill_tree(p)
        out, _ = p.communicate()
        out = (out or '') + '\nABORT: go.py hard timeout after %d s: run_list and its game were killed' % timeout
    return out or '', (datetime.datetime.now() - t0).total_seconds()


def parse_run_list(out):
    """run_list's lines: `[run_list] -Exe tip -> PATH`, `[run_list] scene request -> ROUTE`, the result line `<route leaf> -> wwe2k14_NNN  EXITED after
    40.4s, exit code 0 ...`, an ABORT."""
    res = {'log': None, 'line': None, 'exe': None, 'abort': None, 'secs': None, 'route': None}
    lines = [ln.strip() for ln in out.splitlines()]
    for s in lines:
        m = re.match(r'^\[run_list\] -Exe \S+ -> (.+)$', s)
        if m:
            res['exe'] = m.group(1).strip()
        m = re.match(r'^\[run_list\] scene request -> (.+\.txt)$', s)
        if m:
            res['route'] = m.group(1).strip()
        if s.startswith('ABORT') and not res['abort']:
            res['abort'] = s
    leaf = os.path.basename(res['route'] or '').lower()
    for s in lines:
        m = re.match(r'^(\S+\.txt) -> (wwe2k14_\d+)?\s*(.*)$', s)
        if m and leaf and m.group(1).lower() == leaf:
            res['log'], res['line'] = m.group(2), s
            t = re.search(r'(?:EXITED|KILLED|STILL RUNNING) after ([\d.,]+)s', s)
            if t:
                res['secs'] = float(t.group(1).replace(',', '.'))
    return res


def log_path(res, exe):
    if not res['log']:
        return None
    exe_path = res['exe'] or (exe if os.path.isfile(exe) else None)
    dirs = [os.path.join(os.path.dirname(exe_path), 'logs')] if exe_path else []
    rs = os.path.join(ROOT, 'out', 'harness', 'run_save', res['log'] + '.txt')
    if os.path.isfile(rs):
        lines = open(rs, encoding='utf-8', errors='replace').read().splitlines()
        if len(lines) > 1 and lines[1].strip():
            dirs.append(os.path.join(lines[1].strip(), 'logs'))
            res['exe'] = res['exe'] or os.path.join(lines[1].strip(), 'wwe2k14.exe')
    for d in dirs:
        p = os.path.join(d, res['log'] + '.log')
        if os.path.isfile(p):
            return p
    return None


def run_read(log, expect):
    args = [PY, '-I', RUN_READ, log, '--json']
    for e in expect:
        args += ['--expect', e]
    p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=900)
    try:
        return json.loads(p.stdout)
    except ValueError:
        return {'error': (p.stdout + p.stderr).strip().splitlines()[-1:] or ['run_read exit %d' % p.returncode]}


def line_at(parts, n):
    """The run's line N (run_read numbers the parts' lines on, oldest part first)."""
    k = 0
    for p in parts:
        with open(p, encoding='utf-8', errors='replace') as fh:
            for ln in fh:
                k += 1
                if k == n:
                    return ln.rstrip('\r\n')
    return ''


def game_text(line):
    m = re.search(r'\[t\d+\] ?(.*)$', line)
    return (m.group(1) if m else line).strip()


# S110 SCENEMOM2: a moment wait its `until finish` clause ended (src\input_script.h:2467: `script FAILED at line N: wait_event SPEC: ended by UNTIL
# after F frames (seq S, bus frame B, frame G): FIELDS`; SPEC = the wait's text + ` until ` + the until's, :1198): the match ended first
ENDED_RX = re.compile(r'script FAILED at line (\d+): wait_event (.*?): ended by (.*?) after \d+ frames \(seq \d+, bus frame \d+, frame (\d+)\): (.*)$')


def ended_by(line):
    """-> {'finish': finish_name, 'why': the ARRIVED text} when `line` is a wait the until clause ended, else None."""
    m = ENDED_RX.search(line or '')
    if not m:
        return None
    n, spec, until, frame, fields = m.groups()
    f = dict(re.findall(r'"(\w+)": "?([^",]*)"?', fields))
    if 'finish_name' not in f:
        return {'finish': until, 'why': 'NOT ARRIVED: line %s (`wait_event %s`) was ended by %s at frame %s: %s' % (n, spec, until, frame, fields[:160])}
    why = 'NOT ARRIVED: the match ended first, by %s (winner_slot %s, frame %s), while line %s waited for `wait_event %s`' % (
        f['finish_name'], f.get('winner_slot', '?'), frame, n, spec)
    if f['finish_name'] == 'submission' and re.search(r'what=(pin|count)\b|finish_name=pinfall', spec):
        why += ('; likely cause (not settled): the favoured seat played a submission finisher (the default recipe, when the route used it, gives '
                'seat 0 the stock; which of its two finishers a CPU plays is not known offline: docs\\research\\s110\\SCENEMOM2.md 4): putting the '
                'other wrestler first may help, or give your own director lines')
    return {'finish': f['finish_name'], 'why': why}


# S110 TOOLFIT (docs\research\s110\TOOLFIT.md 2): the finisher wish's assert failed (tools\run\scene_moments.py WISH_LINES; src\input_script.h:2615:
# `script FAILED at line N: assert_state NAME VALUE (is 0x.. (V) [focus], frame F)`): finisher_fit_S 2 = seat S's fitting holds are all in a posture
# family of the game's chooser sub_82602DE0, 0 = no hold in slots 17 / 18 (src\finisher_pick_core.h FitOf). The verb's answer is the director's
# line `director: finisher S hold (TEXT; char ..)` (src\situation_director_core.h:515). The family is picked at sub_82602DE0 +1AD0..+1CC8 by
# sub_82534288 (an angle test between the two wrestlers, 0..3); a family hold it does not pick returns 2 (+800: the next test of sub_82605418).
FIT_RX = re.compile(r'script FAILED at line (\d+): assert_state finisher_fit_(\d) 1 \(is \S+ \((-?\d+)\)[^,]*, frame (\d+)\)')


def last_with(parts, text):
    """The run's last line holding `text` ('' when none)."""
    hit = ''
    for p in parts:
        with open(p, encoding='utf-8', errors='replace') as fh:
            for ln in fh:
                if text in ln:
                    hit = ln.rstrip('\r\n')
    return hit


def fit_failed(line, parts):
    """-> {'why': the NOT ARRIVED text, 'fit': V} when `line` is the finisher wish's failed assert, else None."""
    m = FIT_RX.search(line or '')
    if not m:
        return None
    n, s, v, frame = m.groups()
    said = re.search(r'director: finisher \d hold \((.*); char ', last_with(parts, '[wwe2k14][director] director: finisher %s hold (' % s))
    what = said.group(1) if said else 'the verb\'s answer is not in the log'
    if v == '2':
        why = ('NOT ARRIVED: seat %s\'s finisher wish `hold` is met only by a hold in a posture family (line %s, frame %s: finisher_fit_%s is 2; %s). '
               'The game\'s chooser sub_82602DE0 sorts holds into six families by move id and takes a family hold only when an angle test between '
               'the two wrestlers (sub_82534288) picks that family (likely where the attacker stands by a downed opponent: not settled; run 289: the '
               'STF 58350 never played in 72000 frames). No director verb sets that (the missing verb). The submission recipe needs a family-free '
               'hold (ids 58000..58099, the Yes Lock / Kimura class) in seat %s\'s slot 17 or 18: name a wrestler who has one first (the tool '
               'never swaps the seats)') % (s, n, frame, s, what, s)
    elif v == '0':
        why = ('NOT ARRIVED: seat %s has no hold finisher in slots 17 / 18 (line %s, frame %s: finisher_fit_%s is 0; %s): the submission recipe '
               'needs one') % (s, n, frame, s, what)
    else:
        why = 'NOT ARRIVED: line %s: finisher_fit_%s is %s at frame %s (%s)' % (n, s, v, frame, what)
    return {'why': why, 'fit': v}


def handback(scene, rr, res, log, secs, expect):
    """-> (lines, exit code) from run_read's JSON and run_list's lines. Arrived = `bench: done` reached AND every expected line found."""
    L = ['SCENE    ' + scene]
    if rr is None or 'error' in rr:
        why = res['abort'] or (res['line'] and 'no log found for %s' % res['line']) or (rr or {}).get('error') or 'run_list printed no result line'
        L += ['ARRIVED  NO (%s)' % (why if isinstance(why, str) else ' '.join(why)),
              'RUN      %s  %.1f s  exe %s' % (log or 'no log', secs, res['exe'] or '?'),
              'SHOTS    none', 'LOG      not read', 'EXIT     1 run failed']
        return L, 1
    found = rr.get('expect_found') or {}
    missing = [e for e in expect if e not in found]
    probs = rr.get('problems') or []
    done = rr.get('bench_done_line')
    arrived = done is not None and not missing
    parts = rr.get('parts') or [rr['log']]
    ended = None   # S110 SCENEMOM2: the finish ended a moment's wait (ended_by)
    if arrived:
        n = max([found[e] for e in expect] or [done])
        ln = line_at(parts, n)
        fm = FRAME_RX.search(game_text(ln))
        # S110 SCENEMOM2 review: the line whole (a table break's body_slot / body_id come after 200 characters)
        L.append('ARRIVED  yes (frame %s, line %d: %s)' % (fm.group(1) or fm.group(2) if fm else '?', n, game_text(ln)))
    else:
        fail = [p for p in probs if p['kind'] == 'script FAILED'] or probs
        first = fail[0] if fail else {}
        ended = ended_by(line_at(parts, first['line_no']) if first.get('line_no') else '') or ended_by(first.get('text', ''))
        # S110 TOOLFIT: the finisher wish's assert (fit_failed)
        fit = None if ended else (fit_failed(line_at(parts, first['line_no']) if first.get('line_no') else '', parts) or
                                  fit_failed(first.get('text', ''), parts))
        if ended:
            why = ended['why']
        elif fit:
            why = fit['why']
        elif fail:
            why = '%s%s' % ('frame %s: ' % fail[0]['first_frame'] if fail[0].get('first_frame') else '', game_text(fail[0]['text'])[:220])
        elif done is None:
            why = 'the route never reached `bench: done` (%s)' % (res['line'] or 'no run_list line')
        else:
            why = 'the route finished, but %d expected game line(s) never printed: %s' % (len(missing), ' | '.join('"%s"' % e for e in missing))
        if fail and missing and not ended and not fit:
            why += '; never printed: %s' % ' | '.join('"%s"' % e for e in missing)
        L.append('ARRIVED  NO (%s)' % why)
    L.append('RUN      %s  %.1f s  exe %s' % (rr['log'], secs, res['exe'] or '?'))
    files = (rr.get('shots') or {}).get('files') or []
    d = (rr.get('shots') or {}).get('dir') or ''
    sheets = [os.path.join(d, f['name']) for f in files if 'sheet' in f['name'].lower()]
    pics = [os.path.join(d, f['name']) for f in files if 'sheet' not in f['name'].lower()]
    shown = ', '.join(pics[:4]) + (' ... (%d)' % len(pics) if len(pics) > 4 else ' (%d)' % len(pics)) if pics else 'none'
    L.append('SHOTS    %s + sheet %s' % (shown, ', '.join(sheets) or 'none'))
    L.append(known_noise.summary(rr, rr['log'], game_text))   # S110 SCENEMOMENTS: names the NEW error groups; the known boot noise only counted
    if not arrived:
        L.append('EXIT     1 not arrived' + (' (the match ended first: %s)' % ended['finish'] if ended else ''))
        return L, 1
    if probs:
        L.append('EXIT     1 arrived, but the run has PROBLEMS %d (see LOG)' % len(probs))
        return L, 1
    L.append('EXIT     0 arrived')
    return L, 0


def log_question(request, scene, exe, folder):
    """The project's rule: a run's question is logged before the run starts."""
    os.makedirs(os.path.join(OUT, 'runs'), exist_ok=True)
    with open(os.path.join(OUT, 'runs', 'runs.md'), 'a', encoding='utf-8') as fh:
        fh.write('- %s  request `%s`  ->  `%s`  (exe %s; %s)\n' % (datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'), request, scene, exe, folder))


def live_run(name, low):
    bad = [t for t in low['flags'] if t.split('=')[0] not in LIVE_OK_FLAGS]
    if bad:
        raise Refused(['--live %s: the route needs boot flags a running game cannot take: %s (run it without --live)' % (name, ' '.join(bad))])
    body, started = [], False
    for ln in low['text'].splitlines():
        if ln.strip() == 'include boot_to_menu':
            started = True
            continue
        if started and ln.strip() not in ('quit', 'on_fail quit') and not ln.startswith('#'):
            body.append(ln)
    return body


def preview_exe(exe):
    """The exe --print lowers on: a file as given; `tip` = the exe the tip pointer names (out\\exe\\tip.json of the main checkout), when there."""
    if os.path.isfile(exe):
        return exe, exe
    if exe in ('tip', 'tip-stream'):
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'goto'))
        import scene_ids
        ptr = os.path.join(scene_ids.main_checkout(), 'out', 'exe', 'tip.json' if exe == 'tip' else 'tip-stream.json')
        try:
            with open(ptr, encoding='utf-8') as fh:
                p = json.load(fh).get('exe') or ''
        except (OSError, ValueError):
            p = ''
        if os.path.isfile(p):
            return p, '%s (the tip pointer %s; the run lowers again on the exe run_list resolves)' % (p, ptr)
        return None, 'none (no tip pointer exe at %s: the preview assumes an exe without newer marks)' % ptr
    return None, 'none (%s is not a file)' % exe


# ------------------------------------------------------------------ main
def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('request', help='the scene request (structured or looser words), or a FILE.scene')
    ap.add_argument('--exe', default='tip')
    ap.add_argument('--data', default='', help="run_list's -Data: a mirror NAME under out\\bench (default userdata_frozen)")
    ap.add_argument('--print', dest='show', action='store_true')
    ap.add_argument('--live', metavar='NAME')
    ap.add_argument('--wait', type=int, default=600, help="run_list's -WaitSeconds (the game is killed after it)")
    ap.add_argument('--timeout', type=int, default=0, help='hard timeout of the whole call in s (default: wait + 900, room for a tip build and the lock)')
    a = ap.parse_args(argv)
    request = a.request
    if request.lower().endswith('.scene') and os.path.isfile(request):
        request = open(request, encoding='utf-8').read()
    scene = None
    try:
        if a.data and not re.fullmatch(r'[\w.\-]+', a.data):
            raise Refused(['--data %s: a mirror NAME under out\\bench (run_list -Data), not a path' % a.data])
        scene = to_scene(request)
        exe_low, exe_said = preview_exe(a.exe) if (a.show or a.live) else (None, '')
        try:
            low = lower_scene(scene, exe_low, a.data or None, route=bool(a.show or a.live))
        except Refused:
            # Run 315: a run's first lowering has no exe, so a recipe that needs a newer verb (ring_break's `omg`) was refused here before
            # run_list could lower on the real exe. Then it is tried once on the tip pointer's exe. A stale pointer can still refuse what
            # the rebuilt exe would take: one short run on `-Exe tip` moves the pointer (the structural form: run_list resolves the exe
            # first and hands it back).
            if exe_low is not None or a.show or a.live:
                raise
            exe_low, exe_said = preview_exe(a.exe)
            if exe_low is None:
                raise
            low = lower_scene(scene, exe_low, a.data or None, route=False)
        body = live_run(a.live, low) if a.live else None
    except Refused as x:
        print('SCENE    %s' % (scene or '(not resolved) ' + ' '.join(request.split())))
        print('REFUSED  %d part(s): fix them and call again (words: py -I tools\\scene\\catalogue.py find WORDS --kind %s)' % (len(x.lines), '|'.join(KINDS)))
        for ln in x.lines:
            print('  ' + ln)
        print('EXIT     2 refused')
        return 2
    if a.show:
        print('SCENE    ' + scene)
        print('ARRIVAL  %s: %s' % (low['arrival']['kind'], low['arrival']['line']))
        for e in low['arrival']['expect']:
            print('EXPECT   ' + e)
        print('VERDICT  `%s` reached (on_fail quit)%s' % (low['verdict'], ' and every EXPECT line printed' if low['arrival']['expect'] else ''))
        print('EXE      ' + exe_said)
        print('ROUTE')
        print(low['text'], end='')
        print('EXIT     0 printed (nothing started)')
        return 0
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S') + '_%d' % os.getpid()
    folder = os.path.join(OUT, 'runs', stamp)
    os.makedirs(folder, exist_ok=True)
    log_question(' '.join(request.split()), scene, 'live ' + a.live if a.live else a.exe, folder)
    if a.live:
        bf = os.path.join(folder, 'live_batch.txt')
        with open(bf, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('\n'.join(body) + '\n')
        p = subprocess.run([PY, LIVE, 'run', '-f', bf, '--name', a.live, '--timeout', str(a.wait)], capture_output=True, text=True, errors='replace',
                           timeout=a.wait + 120)
        out = p.stdout + p.stderr
        want = low['arrival']['expect']
        hit = p.returncode == 0 and all(any(e in ln and not re.search(r'\]\[input\] line \d+: ', ln) for ln in out.splitlines()) for e in want)
        L = ['SCENE    ' + scene, 'ARRIVED  %s' % ('yes (live.py exit 0%s)' % (', every expected line printed' if want else '') if hit else 'NO (live.py exit %d: %s)' % (
            p.returncode, (out.strip().splitlines() or ['no output'])[-1][:200])),
             'RUN      live %s  batch %s' % (a.live, bf), 'SHOTS    see the batch answer above', 'LOG      live.py exit %d' % p.returncode,
             'EXIT     %d %s' % ((0, 'arrived') if hit else (1, 'not arrived'))]
        print(out.rstrip())
        print('\n'.join(L))
        return 0 if hit else 1
    out, secs = run_list(scene, a.exe, a.data, a.wait, a.timeout or a.wait + 900)
    with open(os.path.join(folder, 'run_list.txt'), 'w', encoding='utf-8') as fh:
        fh.write(out)
    res = parse_run_list(out)
    if res['abort'] and res['abort'].startswith('ABORT: scene request refused') and not res['route']:
        print('SCENE    ' + scene)
        print('REFUSED  1 part(s): the lowering on the exe the run resolved (%s) refuses' % (res['exe'] or a.exe))
        print('  ' + res['abort'][len('ABORT: scene request refused: '):])
        print('EXIT     2 refused')
        return 2
    try:
        expect = describe_route(res['route'])['arrival']['expect'] if res['route'] else []
    except Refused as x:
        res['abort'] = res['abort'] or 'the route run_list ran: ' + '; '.join(x.lines)
        expect = []
    log = log_path(res, a.exe)
    rr = run_read(log, expect) if log and res['route'] else None
    L, code = handback(scene, rr, res, log, res['secs'] if res['secs'] is not None else secs, expect)
    with open(os.path.join(folder, 'handback.txt'), 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(L + ['ROUTE    %s' % (res['route'] or 'none')]) + '\n')
    print('\n'.join(L))
    return code


if __name__ == '__main__':
    sys.exit(main())
