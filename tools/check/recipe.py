"""S106 C2: the check recipe: load, lint, bars, participants (docs/research/s106/C2.md sections 1-2). Read-only, 0 game runs.

A recipe is tools/check/recipes/<ROW>.toml. `load(path)` -> dict, `lint(rec, tree)` -> (errors, warns), `parse_bar("> 0")`, `meets(value, bar)`,
`participants(rec, main, tree)` -> set of ids or None, `expand(rec)` -> the preconditions + guards the champion adds.
"""
# entry: load
import os
import re
import tomllib

BAR_OPS = ('>=', '<=', '!=', '==', '&', '=', '>', '<')
AGGS = ('count', 'max', 'min', 'sum', 'last', 'first', 'set')
ROLES = ('fault', 'mechanism')
ROW_RE = re.compile(r'^[A-Za-z0-9]+$')
FORBIDDEN_FLAGS = ('--user_data_root', '--cache_root', '--wwe2k14_input_script',
                   # S106 C2F F4: the switches the runner (check.py build_flags / run_list) owns: a copy in a recipe repeats one (last wins, no log line)
                   '--wwe2k14_settings_file', '--wwe2k14_use_launcher_settings', '--vsync', '--wwe2k14_mods_dir', '--wwe2k14_replay_dir',
                   '--game_data_root', '--wwe2k14_scenario_dir')
QUIT_LINE = r'\[wwe2k14\]\[input\] line \d+: quit \(frame'
PLANT_KEYS = ('threads', 'mask', 'caption', 'blood', 'palette', 'holder')


class BarError(Exception):
    pass


def load(path):
    with open(path, 'rb') as f:
        rec = tomllib.load(f)
    rec['_path'] = str(path)
    return rec


def parse_bar(s):
    """'> 0' -> ('>', 0); '== {2,3}' -> ('==', frozenset({2,3})); '& 0x40' -> ('&', 64)."""
    s = str(s).strip()
    for op in BAR_OPS:
        if s.startswith(op):
            rest = s[len(op):].strip()
            if rest.startswith('{') and rest.endswith('}'):
                try:
                    return op, frozenset(int(x, 0) for x in rest[1:-1].split(',') if x.strip())
                except ValueError:
                    raise BarError('bad set in bar %r' % s)
            try:
                return op, int(rest, 0)
            except ValueError:
                raise BarError('bad number in bar %r' % s)
    raise BarError('bar %r has no operator (one of %s)' % (s, ' '.join(BAR_OPS)))


def meets(value, bar):
    """value is an int / frozenset / None (UNREAD: never meets: the caller makes it VOID before it asks)."""
    if value is None:
        return False
    op, n = parse_bar(bar) if isinstance(bar, str) else bar
    if isinstance(n, frozenset):
        if op not in ('==', '=', '!='):
            raise BarError('a set only takes == / !=')
        return (set(value) == set(n)) == (op != '!=')
    if isinstance(value, (set, frozenset)):
        raise BarError('a set value needs a set bar')
    return {'=': value == n, '==': value == n, '!=': value != n, '>=': value >= n, '<=': value <= n,
            '>': value > n, '<': value < n, '&': (value & n) == n}[op]


def _meets0(bar):
    """Does a count of 0 meet this bar? (lint helper)"""
    try:
        return meets(0, bar)
    except BarError:
        return False


def _header_flags(tree, route):
    """The route's `# harness: flags ...` text (only those lines)."""
    out = []
    for l in _route_lines(tree, route):
        m = re.match(r'#\s*harness:\s*flags\s+(.*)', l)
        if m:
            out.append(m.group(1))
    return ' '.join(out)


def _bar_ok(s):
    try:
        parse_bar(s)
        return None
    except BarError as e:
        return str(e)


def expand(rec):
    """The champion adds two save expressions and a card guard. -> (save_exprs, guards)."""
    ch = rec.get('destination', {}).get('champion')
    if not ch:
        return [], []
    t, c = ch['title'], ch['char']
    exprs = ['holder:title=%d:char=%d' % (t, c)]
    if ch.get('plate'):          # S106 C2F F14: the plate precondition belongs to the belt-plate recipe only
        exprs.append('plate:title=%d' % t)
    guard = dict(name='champion %d in the card' % c,
                 line=r'\[wwe2k14\]\[match\] card \(.*participants .*\b\d+:%d \(slot' % c, agg='count', need='>= 1')
    return exprs, [guard]


def all_pre_exprs(rec):
    out = []
    for p in rec.get('pre', []):
        out += list(p.get('save', []))
    return expand(rec)[0] + out


def implicit_guards():
    """S106 C2F B1: every recipe carries these: the route reached its quit line (a cut / crashed log has none) and no route step failed."""
    return [dict(name='route reached its quit line', line=QUIT_LINE, agg='count', need='>= 1'),
            dict(name='no route step failed', line=r'script FAILED', agg='count', need='== 0')]


def all_guards(rec):
    return list(rec.get('guard', [])) + expand(rec)[1] + implicit_guards()


def lint(rec, tree, route_missing_is_error=True):
    """-> (errors, warns): lists of text. A missing route is a lint error, never a crash."""
    err, warn = [], []
    r = rec.get('recipe', {})
    d = rec.get('destination', {})
    row = r.get('row', '')
    if not ROW_RE.match(str(row)):
        err.append('recipe.row %r must match ^[A-Za-z0-9]+$ (it becomes a row= token)' % row)
    q = r.get('question', '')
    if not q or '\n' in q or '\r' in q:
        err.append('recipe.question is missing or has a newline')
    if r.get('preset') is None:
        err.append('recipe.preset is missing')
    route = d.get('route')
    if not route:
        err.append('destination.route is missing')
    elif not os.path.isfile(os.path.join(tree, route.replace('\\', os.sep))) and route_missing_is_error:
        err.append('destination.route %s does not exist in the tree (lint error, not a crash)' % route)
    fl_all = d.get('flags', '')
    route_flags = _header_flags(tree, route) if route else ''
    for src, text in (('destination.flags', fl_all), ('the route header', route_flags)):
        for tok in text.split():
            nm = tok.split('=', 1)[0]
            if nm in FORBIDDEN_FLAGS:
                err.append('%s may not name %s (the runner / run_list owns it)' % (src, nm))
            if nm == '--audio_mute' and tok.split('=', 1)[-1].lower() in ('false', '0', 'no'):
                err.append('%s may not name --audio_mute=false (every run is muted)' % src)
    names_seen = [t.split('=', 1)[0] for t in fl_all.split() if t.startswith('--')]
    for nm in sorted({n for n in names_seen if names_seen.count(n) > 1}):
        err.append('destination.flags gives %s twice (last wins, no log line)' % nm)
    route_abs = os.path.join(tree, route.replace('\\', os.sep)) if route else ''
    if route_abs and os.path.isfile(route_abs):
        import verbs
        for nm in verbs.conflicts(route_abs, names_seen):
            err.append('destination.flags %s is already set by the route\'s own `# harness: flags` line (a repeated switch: last wins)' % nm)
        if any(re.match(r'\s*on_fail\s+continue\b', l) for l in _route_lines(tree, route)):
            warn.append('the route says `on_fail continue`: a failed step lets the run go on (the quit-line and script-FAILED guards still read the log)')
    if re.search(r'--wwe2k14_mods=(true|1)\b', fl_all + ' ' + route_flags) and not d.get('mods'):
        err.append('flags turn on --wwe2k14_mods but destination.mods is not set (the game would write game_status.txt into the real game_data\\mods)')
    if re.search(r'--wwe2k14_settings_file=\S*appdata', d.get('flags', ''), re.I):
        err.append('destination.flags may not name a settings file under %APPDATA%')
    if d.get('settings', 'blank') not in ('blank', 'new_player'):
        err.append('destination.settings must be blank or new_player')
    if d.get('data', 'userdata_frozen') == 'plant' and 'plant' not in rec:
        err.append('destination.data = "plant" needs a [plant] table')
    for k in rec.get('plant', {}):
        if k not in PLANT_KEYS:
            err.append('plant.%s unknown (allowed %s)' % (k, ' '.join(PLANT_KEYS)))
    ch = d.get('champion')
    if ch is not None and not ('title' in ch and 'char' in ch):
        err.append('destination.champion wants { title = T, char = C }')
    names = set()
    for kind in ('guard', 'readout'):
        for it in rec.get(kind, []):
            n = it.get('name', '')
            if not n:
                err.append('[[%s]] without a name' % kind)
            if (kind, n) in names:
                err.append('[[%s]] name %r repeated' % (kind, n))
            names.add((kind, n))
            try:
                re.compile(it.get('line', ''))
            except re.error as e:
                err.append('[[%s]] %s: regex does not compile (%s)' % (kind, n, e))
            if not it.get('line'):
                err.append('[[%s]] %s: no line regex' % (kind, n))
            if it.get('agg') not in AGGS:
                err.append('[[%s]] %s: agg must be one of %s' % (kind, n, ' '.join(AGGS)))
            for k in ('after', 'before'):
                if k in it:
                    try:
                        re.compile(it[k])
                    except re.error as e:
                        err.append('[[%s]] %s: %s regex does not compile (%s)' % (kind, n, k, e))
            if it.get('agg') != 'count' and '(?P<v>' not in it.get('line', ''):
                err.append('[[%s]] %s: agg %s needs a (?P<v>\\d+) group in the line regex' % (kind, n, it.get('agg')))
            bars = ['need'] if kind == 'guard' else ['fault', 'fixed']
            for b in bars:
                if b in it:
                    m = _bar_ok(it[b])
                    if m:
                        err.append('[[%s]] %s.%s: %s' % (kind, n, b, m))
            if kind == 'guard' and 'need' not in it:
                err.append('[[guard]] %s: no need bar' % n)
            if kind == 'readout':
                role = it.get('role')
                if role not in ROLES:
                    err.append('[[readout]] %s: role must be fault or mechanism' % n)
                if 'fixed' not in it:
                    err.append('[[readout]] %s: no fixed bar' % n)
                if role == 'fault' and 'fault' not in it:
                    err.append('[[readout]] %s: a fault readout needs a fault bar' % n)
            for b in bars:                 # S106 C2F F10: a bar's type must match the aggregate (set vs number)
                if b in it and not _bar_ok(it[b]):
                    is_set = isinstance(parse_bar(it[b])[1], frozenset)
                    if it.get('agg') == 'set' and not is_set:
                        err.append('[[%s]] %s.%s: a set aggregate takes a set bar (== {..})' % (kind, n, b))
                    if it.get('agg') != 'set' and is_set:
                        err.append('[[%s]] %s.%s: a set bar needs agg = "set"' % (kind, n, b))
                    if is_set and parse_bar(it[b])[0] not in ('==', '=', '!='):
                        err.append('[[%s]] %s.%s: a set takes == / !=' % (kind, n, b))
            if kind == 'readout' and it.get('role') == 'fault':
                if not (it.get('specific') is True or r.get('eyes') == 'required'):
                    err.append('[[readout]] %s: a role = "fault" readout needs specific = true (its line is the fault\'s own) or recipe.eyes = "required"' % n)
                if it.get('agg') == 'count' and 'fault' in it and not _bar_ok(it['fault']) and _meets0(it['fault']):
                    sib = [g for g in rec.get('guard', []) if g.get('line') and it.get('line', '').startswith(g['line']) and g['line'] != it['line']
                           and not _bar_ok(g.get('need', '')) and not _meets0(g['need'])]
                    if not sib:
                        err.append('[[readout]] %s: a count fault bar that 0 lines meets needs a sibling [[guard]] (same line family, need >= 1): a log with no such line is not a fault' % n)
    for s in rec.get('shots', []):
        if not s.get('labels') or not s.get('ask') or s.get('fault') not in ('yes', 'no') or s.get('fixed') not in ('yes', 'no'):
            err.append('[[shots]] wants labels, ask, fault and fixed (yes / no)')
    if r.get('eyes') not in (None, 'required'):
        err.append('recipe.eyes must be "required" when given')
    if r.get('eyes') == 'required' and not rec.get('shots'):
        err.append('recipe.eyes = "required" needs a [[shots]] table')
    has_fault = any(x.get('role') == 'fault' for x in rec.get('readout', [])) or bool(rec.get('shots'))
    if not has_fault:
        err.append('no fault-side item (a role = "fault" readout or a [[shots]] table): BEFORE could not show the fault')
    for p in rec.get('pre', []):
        if not (p.get('save') or p.get('file')):
            err.append('[[pre]] needs save = [...] or file = ...')
    return err, warn


def _route_lines(tree, route):
    p = os.path.join(tree, route.replace('\\', os.sep))
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read().splitlines()
    except OSError:
        return []


def participants(rec, main, tree):
    """-> (set of ids, source text) or (None, why). Order: launch_participants flag, match cvar / flag, scenario toml."""
    d = rec['destination']
    flags = d.get('flags', '')
    lines = _route_lines(tree, d['route'])
    header = _header_flags(tree, d['route'])      # S106 C2F F11: only the `# harness: flags` lines, never any comment
    allf = flags + ' ' + header
    m = re.search(r'--wwe2k14_launch_participants=(\S+)', allf)
    if m:
        return {int(x.split(':')[0]) for x in m.group(1).split(',') if x}, 'flag --wwe2k14_launch_participants'
    for text in [allf] + [l for l in lines if not l.lstrip().startswith('#')]:      # a comment never names the participants (S106 C2F F11)
        m = re.search(r'(?:--wwe2k14_match=|cvar wwe2k14_match )\S*?\bp=([\d:,\-]+)', text)
        if m:
            return {int(x.split(':')[0]) for x in m.group(1).split(',') if x}, 'match line'
    m = re.search(r'--wwe2k14_launch=(\S+)', allf)
    if m:
        toml = os.path.join(main, 'out', 'scenarios', m.group(1) + '.toml')
        if not os.path.isfile(toml):
            return None, 'scenario %s not found' % toml
        with open(toml, 'rb') as f:
            doc = tomllib.load(f)
        ps = doc.get('summary', {}).get('participant', [])
        if not ps:
            return None, 'scenario %s has no [[summary.participant]]' % m.group(1)
        return {int(p['id']) for p in ps}, 'scenario ' + m.group(1)
    return None, 'no participants in the flags, the route or a scenario'
